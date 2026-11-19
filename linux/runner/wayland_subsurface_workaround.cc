#include "wayland_subsurface_workaround.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

namespace mornye {
namespace {

constexpr const char kFlutterLibrary[] = "libflutter_linux_gtk.so";
constexpr const char kMarshalSymbol[] = "wl_proxy_marshal";
// The most arguments a Wayland request can carry (WL_CLOSURE_MAX_ARGS).
constexpr int kMaxRequestArgs = 20;

using GetInterfaceFn = const struct wl_interface* (*)(struct wl_proxy*);
GetInterfaceFn g_get_interface = nullptr;

// Does what libwayland's wl_proxy_marshal does, except that a set_sync request
// on a wl_subsurface goes out as set_desync. The arguments are read from the
// request's signature, as libwayland itself reads them.
void MarshalWithDesyncedSubsurfaces(struct wl_proxy* proxy,
                                    uint32_t opcode,
                                    ...) {
  const struct wl_interface* interface = g_get_interface(proxy);
  if (interface == &wl_subsurface_interface &&
      opcode == WL_SUBSURFACE_SET_SYNC) {
    opcode = WL_SUBSURFACE_SET_DESYNC;
  }

  union wl_argument args[kMaxRequestArgs] = {};
  int count = 0;
  va_list ap;
  va_start(ap, opcode);
  for (const char* type = interface->methods[opcode].signature;
       *type != '\0' && count < kMaxRequestArgs; ++type) {
    switch (*type) {
      case 'i':
        args[count++].i = va_arg(ap, int32_t);
        break;
      case 'u':
        args[count++].u = va_arg(ap, uint32_t);
        break;
      case 'f':
        args[count++].f = va_arg(ap, wl_fixed_t);
        break;
      case 's':
        args[count++].s = va_arg(ap, const char*);
        break;
      case 'o':
      case 'n':
        args[count++].o = va_arg(ap, struct wl_object*);
        break;
      case 'a':
        args[count++].a = va_arg(ap, struct wl_array*);
        break;
      case 'h':
        args[count++].h = va_arg(ap, int32_t);
        break;
      default:
        // A version number or '?' (nullable) qualifies the next argument.
        break;
    }
  }
  va_end(ap);
  wl_proxy_marshal_array(proxy, opcode, args);
}

size_t RelocationSymbol(ElfW(Xword) info) {
#if __ELF_NATIVE_CLASS == 64
  return ELF64_R_SYM(info);
#else
  return ELF32_R_SYM(info);
#endif
}

struct PatchResult {
  bool found_library = false;
  int patched = 0;
};

// glibc stores most dynamic-section addresses already relocated; some ports
// keep them as offsets from the load address.
ElfW(Addr) DynamicAddress(ElfW(Addr) value, ElfW(Addr) base) {
  return value < base ? value + base : value;
}

// Rewrites every slot in |info|'s RELA table at |relocations| that resolves
// |symbol| so it holds |replacement| instead.
int PatchRelocations(const struct dl_phdr_info* info,
                     const ElfW(Rela)* relocations,
                     size_t size,
                     const ElfW(Sym)* symbols,
                     const char* strings,
                     void* replacement) {
  const long page = sysconf(_SC_PAGESIZE);
  int patched = 0;
  for (size_t i = 0; i < size / sizeof(ElfW(Rela)); ++i) {
    const size_t symbol_index = RelocationSymbol(relocations[i].r_info);
    if (symbol_index == 0 ||
        strcmp(strings + symbols[symbol_index].st_name, kMarshalSymbol) != 0) {
      continue;
    }
    void** slot =
        reinterpret_cast<void**>(info->dlpi_addr + relocations[i].r_offset);
    // libflutter is linked with BIND_NOW and RELRO, so its GOT is read-only
    // once the loader has filled it in.
    const uintptr_t page_start =
        reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(page - 1);
    void* page_ptr = reinterpret_cast<void*>(page_start);
    if (mprotect(page_ptr, page, PROT_READ | PROT_WRITE) != 0) {
      continue;
    }
    __atomic_store_n(slot, replacement, __ATOMIC_SEQ_CST);
    mprotect(page_ptr, page, PROT_READ);
    ++patched;
  }
  return patched;
}

int PatchLibrary(struct dl_phdr_info* info, size_t, void* data) {
  auto* result = static_cast<PatchResult*>(data);
  if (info->dlpi_name == nullptr ||
      strstr(info->dlpi_name, kFlutterLibrary) == nullptr) {
    return 0;
  }
  result->found_library = true;

  const ElfW(Dyn)* dynamic = nullptr;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
      dynamic = reinterpret_cast<const ElfW(Dyn)*>(
          info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    }
  }
  if (dynamic == nullptr) return 1;

  const ElfW(Addr) base = info->dlpi_addr;
  const ElfW(Sym)* symbols = nullptr;
  const char* strings = nullptr;
  const ElfW(Rela)* plt_relocations = nullptr;
  size_t plt_size = 0;
  bool plt_is_rela = false;
  const ElfW(Rela)* relocations = nullptr;
  size_t relocations_size = 0;
  for (const ElfW(Dyn)* entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
    switch (entry->d_tag) {
      case DT_SYMTAB:
        symbols = reinterpret_cast<const ElfW(Sym)*>(
            DynamicAddress(entry->d_un.d_ptr, base));
        break;
      case DT_STRTAB:
        strings =
            reinterpret_cast<const char*>(DynamicAddress(entry->d_un.d_ptr, base));
        break;
      case DT_JMPREL:
        plt_relocations = reinterpret_cast<const ElfW(Rela)*>(
            DynamicAddress(entry->d_un.d_ptr, base));
        break;
      case DT_PLTRELSZ:
        plt_size = entry->d_un.d_val;
        break;
      case DT_PLTREL:
        plt_is_rela = entry->d_un.d_val == DT_RELA;
        break;
      case DT_RELA:
        relocations = reinterpret_cast<const ElfW(Rela)*>(
            DynamicAddress(entry->d_un.d_ptr, base));
        break;
      case DT_RELASZ:
        relocations_size = entry->d_un.d_val;
        break;
    }
  }
  if (symbols == nullptr || strings == nullptr) return 1;

  void* replacement = reinterpret_cast<void*>(&MarshalWithDesyncedSubsurfaces);
  if (plt_relocations != nullptr && plt_is_rela) {
    result->patched += PatchRelocations(info, plt_relocations, plt_size,
                                        symbols, strings, replacement);
  }
  // Code built with -fno-plt calls through a GLOB_DAT slot instead.
  if (relocations != nullptr) {
    result->patched += PatchRelocations(info, relocations, relocations_size,
                                        symbols, strings, replacement);
  }
  return 1;
}

}  // namespace

bool ApplyWaylandSubsurfaceWorkaround() {
  const char* disabled = getenv("MORNYE_DISABLE_SUBSURFACE_DESYNC");
  if (disabled != nullptr && strcmp(disabled, "1") == 0) return false;

  // Added in libwayland 1.22. Without it the wrapper cannot tell a request's
  // arguments apart, so older systems keep Flutter's behaviour.
  g_get_interface = reinterpret_cast<GetInterfaceFn>(
      dlsym(RTLD_DEFAULT, "wl_proxy_get_interface"));
  if (g_get_interface == nullptr) return false;

  PatchResult result;
  dl_iterate_phdr(&PatchLibrary, &result);
  return result.patched > 0;
}

}  // namespace mornye
