#include "nvidia_graphics_workaround.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

namespace mornye {
namespace {

constexpr const char* kFlutterCompositorVertexShaderMarker =
    "gl_Position = vec4(offset + position * scale, 0, 1);";

bool g_installed = false;
EpoxyGlHooks g_slots = {};
GlShaderSourceFn g_orig_shader_source_stub = nullptr;
GlShaderSourceFn g_real_shader_source = nullptr;
GlGetUniformLocationFn g_orig_get_uniform_location_stub = nullptr;
GlGetUniformLocationFn g_real_get_uniform_location = nullptr;
GlUniform2fFn g_orig_uniform2f_stub = nullptr;
GlUniform2fFn g_real_uniform2f = nullptr;

bool g_tracking_compositor_shader = false;
int g_compositor_offset_location = -1;
int g_compositor_scale_location = -1;

void HookedGlShaderSource(unsigned int shader,
                          int count,
                          const char* const* string,
                          const int* length) {
  if (count >= 1 && string != nullptr) {
    for (int i = 0; i < count; ++i) {
      if (string[i] != nullptr &&
          strstr(string[i], kFlutterCompositorVertexShaderMarker) != nullptr) {
        g_tracking_compositor_shader = true;
        break;
      }
    }
  }

  if (g_real_shader_source != nullptr) {
    g_real_shader_source(shader, count, string, length);
    return;
  }
  *g_slots.shader_source_slot = g_orig_shader_source_stub;
  g_orig_shader_source_stub(shader, count, string, length);
  if (*g_slots.shader_source_slot != g_orig_shader_source_stub &&
      *g_slots.shader_source_slot != &HookedGlShaderSource) {
    g_real_shader_source = *g_slots.shader_source_slot;
  }
  *g_slots.shader_source_slot = &HookedGlShaderSource;
}

int HookedGlGetUniformLocation(unsigned int program, const char* name) {
  int location = -1;
  if (g_real_get_uniform_location != nullptr) {
    location = g_real_get_uniform_location(program, name);
  } else {
    *g_slots.get_uniform_location_slot = g_orig_get_uniform_location_stub;
    location = g_orig_get_uniform_location_stub(program, name);
    if (*g_slots.get_uniform_location_slot !=
            g_orig_get_uniform_location_stub &&
        *g_slots.get_uniform_location_slot != &HookedGlGetUniformLocation) {
      g_real_get_uniform_location = *g_slots.get_uniform_location_slot;
    }
    *g_slots.get_uniform_location_slot = &HookedGlGetUniformLocation;
  }

  if (g_tracking_compositor_shader && name != nullptr) {
    if (strcmp(name, "offset") == 0) {
      g_compositor_offset_location = location;
    } else if (strcmp(name, "scale") == 0) {
      g_compositor_scale_location = location;
      g_tracking_compositor_shader = false;
    }
  }
  return location;
}

void HookedGlUniform2f(int location, float v0, float v1) {
  if (g_compositor_offset_location >= 0 &&
      location == g_compositor_offset_location) {
    // Flutter 3.50.0 computes offset = (2 * x / width) - 1.0 (e.g. -1.0 for
    // x = 0). Fixed versions pass 0.0 for x = 0. Only shift negative offsets.
    if (v0 <= -0.5f && v1 <= -0.5f) {
      v0 += 1.0f;
      v1 += 1.0f;
    }
  } else if (g_compositor_scale_location >= 0 &&
             location == g_compositor_scale_location) {
    // Flutter 3.50.0 computes scale with integer division (backing_w / view_w),
    // which truncates to 0 during a frame where the window grew faster than the
    // backing store.
    if (v0 == 0.0f) {
      v0 = 1.0f;
    }
    if (v1 == 0.0f) {
      v1 = 1.0f;
    }
  }

  if (g_real_uniform2f != nullptr) {
    g_real_uniform2f(location, v0, v1);
    return;
  }
  *g_slots.uniform2f_slot = g_orig_uniform2f_stub;
  g_orig_uniform2f_stub(location, v0, v1);
  if (*g_slots.uniform2f_slot != g_orig_uniform2f_stub &&
      *g_slots.uniform2f_slot != &HookedGlUniform2f) {
    g_real_uniform2f = *g_slots.uniform2f_slot;
  }
  *g_slots.uniform2f_slot = &HookedGlUniform2f;
}

}  // namespace

bool ApplyNvidiaGraphicsWorkaround(const EpoxyGlHooks* custom_hooks) {
  const char* disable = getenv("MORNYE_DISABLE_NVIDIA_WORKAROUND");
  if (disable != nullptr && strcmp(disable, "1") == 0) {
    return false;
  }
  const char* legacy_disable = getenv("MORNYE_DISABLE_NVIDIA_ZINK_WORKAROUND");
  if (legacy_disable != nullptr && strcmp(legacy_disable, "1") == 0) {
    return false;
  }
  if (g_installed) {
    return true;
  }

  EpoxyGlHooks hooks = {};
  if (custom_hooks != nullptr) {
    hooks = *custom_hooks;
  } else {
    hooks.shader_source_slot = reinterpret_cast<GlShaderSourceFn*>(
        dlsym(RTLD_DEFAULT, "epoxy_glShaderSource"));
    hooks.get_uniform_location_slot = reinterpret_cast<GlGetUniformLocationFn*>(
        dlsym(RTLD_DEFAULT, "epoxy_glGetUniformLocation"));
    hooks.uniform2f_slot = reinterpret_cast<GlUniform2fFn*>(
        dlsym(RTLD_DEFAULT, "epoxy_glUniform2f"));
  }

  if (hooks.shader_source_slot == nullptr ||
      hooks.get_uniform_location_slot == nullptr ||
      hooks.uniform2f_slot == nullptr ||
      *hooks.shader_source_slot == nullptr ||
      *hooks.get_uniform_location_slot == nullptr ||
      *hooks.uniform2f_slot == nullptr) {
    return false;
  }

  g_slots = hooks;
  g_orig_shader_source_stub = *hooks.shader_source_slot;
  g_orig_get_uniform_location_stub = *hooks.get_uniform_location_slot;
  g_orig_uniform2f_stub = *hooks.uniform2f_slot;

  *hooks.shader_source_slot = &HookedGlShaderSource;
  *hooks.get_uniform_location_slot = &HookedGlGetUniformLocation;
  *hooks.uniform2f_slot = &HookedGlUniform2f;
  g_installed = true;
  return true;
}

bool g_niri_mini_player_active = false;
bool g_niri_was_tiled = true;
int g_niri_saved_tile_width = 0;
unsigned long long g_niri_window_id = 0;
bool g_niri_has_saved_floating_pos = false;
int g_niri_saved_floating_x = 0;
int g_niri_saved_floating_y = 0;
int g_niri_workspace_offset_x = 0;
int g_niri_workspace_offset_y = 0;

std::string SanitizeJsonObjectStrings(const char* begin, const char* end) {
  // Keeps JSON key literals intact while blanking out string *values* (after
  // ':') so arbitrary window titles cannot impersonate JSON keys such as
  // "pid": or "is_floating":.
  std::string out;
  out.reserve(static_cast<size_t>(end - begin));
  bool in_string = false;
  bool escape = false;
  bool is_value_string = false;
  char last_sig = '{';

  for (const char* p = begin; p < end; ++p) {
    char c = *p;
    if (in_string) {
      if (escape) {
        escape = false;
        if (!is_value_string) out.push_back(c);
      } else if (c == '\\') {
        escape = true;
        if (!is_value_string) out.push_back(c);
      } else if (c == '"') {
        in_string = false;
        is_value_string = false;
        out.push_back('"');
      } else if (!is_value_string) {
        out.push_back(c);
      }
    } else {
      if (c == '"') {
        in_string = true;
        is_value_string = (last_sig == ':');
        out.push_back('"');
      } else {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
          last_sig = c;
        }
        out.push_back(c);
      }
    }
  }
  return out;
}

bool ParseSingleNiriWindowObject(const std::string& obj,
                                 NiriWindowState* state,
                                 int* out_pid,
                                 bool* out_focused) {
  const char* id_pos = strstr(obj.c_str(), "\"id\":");
  if (id_pos == nullptr) {
    return false;
  }
  *state = NiriWindowState{};
  state->id = strtoull(id_pos + 5, nullptr, 10);
  state->found = true;

  *out_pid = -1;
  const char* pid_pos = strstr(obj.c_str(), "\"pid\":");
  if (pid_pos != nullptr && strncmp(pid_pos + 6, "null", 4) != 0) {
    *out_pid = static_cast<int>(strtol(pid_pos + 6, nullptr, 10));
  }

  *out_focused = false;
  const char* foc_pos = strstr(obj.c_str(), "\"is_focused\":");
  if (foc_pos != nullptr) {
    *out_focused = strncmp(foc_pos + 13, "true", 4) == 0;
  }

  const char* flt_pos = strstr(obj.c_str(), "\"is_floating\":");
  if (flt_pos != nullptr) {
    state->is_floating = strncmp(flt_pos + 14, "true", 4) == 0;
  }

  const char* tile_pos = strstr(obj.c_str(), "\"tile_size\":[");
  if (tile_pos != nullptr) {
    char* end_w = nullptr;
    double tw = strtod(tile_pos + 13, &end_w);
    if (end_w != nullptr && *end_w == ',') {
      double th = strtod(end_w + 1, nullptr);
      state->tile_width = static_cast<int>(lround(tw));
      state->tile_height = static_cast<int>(lround(th));
    }
  }

  const char* fpos = strstr(obj.c_str(), "\"tile_pos_in_workspace_view\":[");
  if (fpos != nullptr) {
    char* end_x = nullptr;
    double fx = strtod(fpos + 30, &end_x);
    if (end_x != nullptr && *end_x == ',') {
      double fy = strtod(end_x + 1, nullptr);
      state->has_floating_pos = true;
      state->floating_x = fx;
      state->floating_y = fy;
    }
  }
  return true;
}

// MORNYE_NIRI_DEBUG=1 prints every Niri IPC request and reply to stderr.
bool NiriDebugEnabled() {
  const char* value = getenv("MORNYE_NIRI_DEBUG");
  return value != nullptr && strcmp(value, "1") == 0;
}

bool SendNiriSocketCommand(const char* request_line,
                           std::string* out_response) {
  const char* sock_path = getenv("NIRI_SOCKET");
  if (sock_path == nullptr || sock_path[0] == '\0') {
    return false;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  if (strlen(sock_path) >= sizeof(addr.sun_path)) {
    close(fd);
    return false;
  }
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    close(fd);
    return false;
  }

  size_t len = strlen(request_line);
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, request_line + sent, len - sent);
    if (n <= 0) {
      close(fd);
      return false;
    }
    sent += static_cast<size_t>(n);
  }

  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    response.append(buf, static_cast<size_t>(n));
    if (response.find('\n') != std::string::npos) {
      break;
    }
  }
  close(fd);
  if (NiriDebugEnabled()) {
    // The window list is long; its size is enough to tell it arrived.
    const bool is_query = strstr(request_line, "\"Windows\"") != nullptr;
    fprintf(stderr, "[mornye niri] -> %.*s\n[mornye niri] <- %s\n",
            static_cast<int>(strcspn(request_line, "\n")), request_line,
            is_query ? (std::to_string(response.size()) + " bytes").c_str()
                     : response.substr(0, response.find('\n')).c_str());
  }
  if (out_response != nullptr) {
    *out_response = response;
  }
  return !response.empty();
}

bool QueryNiriWindowState(int pid, NiriWindowState* out_state) {
  std::string response;
  if (!SendNiriSocketCommand("\"Windows\"\n", &response)) {
    return false;
  }
  return ParseNiriWindowStateForPid(response.c_str(), pid, out_state);
}

unsigned long long NiriMiniPlayerWindowId() {
  return g_niri_mini_player_active ? g_niri_window_id : 0;
}

std::string FormatNiriIdField(bool has_id, unsigned long long id) {
  if (!has_id) {
    return "null";
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%llu", id);
  return std::string(buf);
}

void ResetNvidiaGraphicsWorkaroundForTesting() {
  if (g_installed) {
    if (g_slots.shader_source_slot != nullptr &&
        g_orig_shader_source_stub != nullptr) {
      *g_slots.shader_source_slot = g_orig_shader_source_stub;
    }
    if (g_slots.get_uniform_location_slot != nullptr &&
        g_orig_get_uniform_location_stub != nullptr) {
      *g_slots.get_uniform_location_slot = g_orig_get_uniform_location_stub;
    }
    if (g_slots.uniform2f_slot != nullptr && g_orig_uniform2f_stub != nullptr) {
      *g_slots.uniform2f_slot = g_orig_uniform2f_stub;
    }
  }
  g_installed = false;
  g_slots = {};
  g_orig_shader_source_stub = nullptr;
  g_real_shader_source = nullptr;
  g_orig_get_uniform_location_stub = nullptr;
  g_real_get_uniform_location = nullptr;
  g_orig_uniform2f_stub = nullptr;
  g_real_uniform2f = nullptr;
  g_tracking_compositor_shader = false;
  g_compositor_offset_location = -1;
  g_compositor_scale_location = -1;
  g_niri_mini_player_active = false;
  g_niri_was_tiled = true;
  g_niri_saved_tile_width = 0;
  g_niri_window_id = 0;
  g_niri_has_saved_floating_pos = false;
  g_niri_saved_floating_x = 0;
  g_niri_saved_floating_y = 0;
  g_niri_workspace_offset_x = 0;
  g_niri_workspace_offset_y = 0;
}

bool NormalizeWindowGeometryHints(MornyeGdkGeometry* geometry,
                                  unsigned int* geom_mask) {
  constexpr unsigned int kGdkHintMinSize = 1u << 1;
  constexpr unsigned int kGdkHintMaxSize = 1u << 2;
  constexpr int kDesktopMinimumWidth = 800;
  constexpr int kDesktopMinimumHeight = 600;

  if (geometry == nullptr || geom_mask == nullptr) {
    return false;
  }
  if ((*geom_mask & kGdkHintMinSize) == 0) {
    return false;
  }
  if (geometry->min_width <= 0 || geometry->min_height <= 0 ||
      geometry->min_width >= kDesktopMinimumWidth ||
      geometry->min_height >= kDesktopMinimumHeight) {
    return false;
  }

  geometry->max_width = geometry->min_width;
  geometry->max_height = geometry->min_height;
  *geom_mask |= kGdkHintMaxSize;
  return true;
}

bool ParseNiriWindowStateForPid(const char* json,
                                int pid,
                                NiriWindowState* out_state) {
  if (json == nullptr || out_state == nullptr) {
    return false;
  }
  *out_state = NiriWindowState{};
  const char* array_start = strstr(json, "\"Windows\"");
  if (array_start != nullptr) {
    array_start = strchr(array_start, '[');
  } else {
    array_start = strchr(json, '[');
  }
  if (array_start == nullptr) {
    return false;
  }

  bool in_string = false;
  bool escape = false;
  int brace_depth = 0;
  const char* obj_start = nullptr;

  NiriWindowState pid_match = {};
  NiriWindowState focused_match = {};

  for (const char* p = array_start + 1; *p != '\0'; ++p) {
    char c = *p;
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      if (brace_depth == 0) {
        obj_start = p;
      }
      ++brace_depth;
    } else if (c == '}') {
      --brace_depth;
      if (brace_depth == 0 && obj_start != nullptr) {
        std::string sanitized = SanitizeJsonObjectStrings(obj_start, p + 1);
        NiriWindowState candidate = {};
        int candidate_pid = -1;
        bool candidate_focused = false;
        if (ParseSingleNiriWindowObject(sanitized, &candidate, &candidate_pid,
                                        &candidate_focused)) {
          if (pid > 0 && candidate_pid == pid) {
            if (!pid_match.found || candidate_focused) {
              pid_match = candidate;
            }
          }
          if (candidate_focused && !focused_match.found) {
            focused_match = candidate;
          }
        }
        obj_start = nullptr;
      }
    } else if (c == ']' && brace_depth == 0) {
      break;
    }
  }

  if (pid_match.found) {
    *out_state = pid_match;
    return true;
  }
  if (focused_match.found) {
    *out_state = focused_match;
    return true;
  }
  return false;
}

bool SyncNiriMiniPlayerWindow(bool open,
                              int width,
                              int height,
                              int target_x,
                              int target_y,
                              int pid) {
  const char* sock_path = getenv("NIRI_SOCKET");
  if (sock_path == nullptr || sock_path[0] == '\0') {
    return false;
  }
  const int effective_pid = pid > 0 ? pid : static_cast<int>(getpid());
  NiriWindowState state = {};
  QueryNiriWindowState(effective_pid, &state);
  if (state.found) {
    g_niri_window_id = state.id;
  }
  if (NiriDebugEnabled()) {
    fprintf(stderr,
            "[mornye niri] %s pid=%d found=%d id=%llu floating=%d "
            "tile=%dx%d active=%d\n",
            open ? "open" : "close", effective_pid, state.found,
            state.found ? state.id : 0ull, state.is_floating,
            state.tile_width, state.tile_height, g_niri_mini_player_active);
  }
  const std::string id_str =
      FormatNiriIdField(g_niri_window_id > 0, g_niri_window_id);

  char cmd[256];
  if (open) {
    const bool opening = !g_niri_mini_player_active;
    if (opening) {
      g_niri_was_tiled = state.found ? !state.is_floating : true;
      if (state.found && state.tile_width >= 800) {
        g_niri_saved_tile_width = state.tile_width;
      }
      g_niri_mini_player_active = true;
      snprintf(cmd, sizeof(cmd),
               "{\"Action\":{\"MoveWindowToFloating\":{\"id\":%s}}}\n",
               id_str.c_str());
      SendNiriSocketCommand(cmd, nullptr);
    } else if (state.found && state.is_floating && state.has_floating_pos) {
      g_niri_saved_floating_x =
          static_cast<int>(lround(state.floating_x)) - g_niri_workspace_offset_x;
      g_niri_saved_floating_y =
          static_cast<int>(lround(state.floating_y)) - g_niri_workspace_offset_y;
      g_niri_has_saved_floating_pos = true;
    }

    snprintf(cmd, sizeof(cmd),
             "{\"Action\":{\"SetWindowWidth\":{\"id\":%s,\"change\":{"
             "\"SetFixed\":%d}}}}\n",
             id_str.c_str(), width);
    SendNiriSocketCommand(cmd, nullptr);

    snprintf(cmd, sizeof(cmd),
             "{\"Action\":{\"SetWindowHeight\":{\"id\":%s,\"change\":{"
             "\"SetFixed\":%d}}}}\n",
             id_str.c_str(), height);
    SendNiriSocketCommand(cmd, nullptr);

    const bool had_saved_pos = g_niri_has_saved_floating_pos;
    int move_x = had_saved_pos ? g_niri_saved_floating_x : target_x;
    int move_y = had_saved_pos ? g_niri_saved_floating_y : target_y;
    snprintf(cmd, sizeof(cmd),
             "{\"Action\":{\"MoveFloatingWindow\":{\"id\":%s,\"x\":{"
             "\"SetFixed\":%d.0},\"y\":{\"SetFixed\":%d.0}}}}\n",
             id_str.c_str(), move_x, move_y);
    SendNiriSocketCommand(cmd, nullptr);

    if (!had_saved_pos && opening) {
      g_niri_saved_floating_x = move_x;
      g_niri_saved_floating_y = move_y;
      g_niri_has_saved_floating_pos = true;
      NiriWindowState post_state = {};
      if (QueryNiriWindowState(effective_pid, &post_state) &&
          post_state.has_floating_pos) {
        g_niri_workspace_offset_x =
            static_cast<int>(lround(post_state.floating_x)) - move_x;
        g_niri_workspace_offset_y =
            static_cast<int>(lround(post_state.floating_y)) - move_y;
      }
    }
    return true;
  }

  if (!g_niri_mini_player_active) {
    return false;
  }
  if (state.found && state.is_floating && state.has_floating_pos) {
    g_niri_saved_floating_x =
        static_cast<int>(lround(state.floating_x)) - g_niri_workspace_offset_x;
    g_niri_saved_floating_y =
        static_cast<int>(lround(state.floating_y)) - g_niri_workspace_offset_y;
    g_niri_has_saved_floating_pos = true;
  }
  g_niri_mini_player_active = false;
  if (g_niri_was_tiled) {
    snprintf(cmd, sizeof(cmd),
             "{\"Action\":{\"MoveWindowToTiling\":{\"id\":%s}}}\n",
             id_str.c_str());
    SendNiriSocketCommand(cmd, nullptr);
    if (g_niri_saved_tile_width >= 800) {
      snprintf(cmd, sizeof(cmd),
               "{\"Action\":{\"SetWindowWidth\":{\"id\":%s,\"change\":{"
               "\"SetFixed\":%d}}}}\n",
               id_str.c_str(), g_niri_saved_tile_width);
      SendNiriSocketCommand(cmd, nullptr);
    }
    snprintf(cmd, sizeof(cmd),
             "{\"Action\":{\"ResetWindowHeight\":{\"id\":%s}}}\n",
             id_str.c_str());
    SendNiriSocketCommand(cmd, nullptr);
  }
  return true;
}

}  // namespace mornye
