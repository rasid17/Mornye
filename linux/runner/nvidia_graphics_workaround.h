#ifndef FLUTTER_NVIDIA_GRAPHICS_WORKAROUND_H_
#define FLUTTER_NVIDIA_GRAPHICS_WORKAROUND_H_

namespace mornye {

using GlShaderSourceFn = void (*)(unsigned int shader,
                                  int count,
                                  const char* const* string,
                                  const int* length);
using GlGetUniformLocationFn = int (*)(unsigned int program, const char* name);
using GlUniform2fFn = void (*)(int location, float v0, float v1);

struct EpoxyGlHooks {
  GlShaderSourceFn* shader_source_slot = nullptr;
  GlGetUniformLocationFn* get_uniform_location_slot = nullptr;
  GlUniform2fFn* uniform2f_slot = nullptr;
};

// Matches GdkGeometry from <gdk/gdkwindow.h> without requiring GTK headers when
// compiling the standalone preload library.
struct MornyeGdkGeometry {
  int min_width;
  int min_height;
  int max_width;
  int max_height;
  int base_width;
  int base_height;
  int width_inc;
  int height_inc;
  double min_aspect;
  double max_aspect;
  int win_gravity;
};

// Ensures mini-player geometry hints (smaller than the 800x600 desktop shell
// minimum) include GDK_HINT_MAX_SIZE so Wayland tiling compositors (such as
// Niri) and GTK3's tiled configure handler constrain the surface to the
// mini-player dimensions instead of keeping the tiled column width.
bool NormalizeWindowGeometryHints(MornyeGdkGeometry* geometry,
                                  unsigned int* geom_mask);

struct NiriWindowState {
  unsigned long long id = 0;
  bool found = false;
  bool is_floating = false;
  int tile_width = 0;
  int tile_height = 0;
  bool has_floating_pos = false;
  double floating_x = 0.0;
  double floating_y = 0.0;
};

// Parses the JSON response from Niri's "Windows" IPC query and extracts the
// state of the window belonging to |pid| (falling back to the focused window).
bool ParseNiriWindowStateForPid(const char* json,
                                int pid,
                                NiriWindowState* out_state);

// Synchronizes Mornye's mini-player floating/tiling state with the Niri
// Wayland compositor via NIRI_SOCKET when running under Niri.
bool SyncNiriMiniPlayerWindow(bool open,
                              int width,
                              int height,
                              int target_x,
                              int target_y,
                              int pid = 0);

// Flutter 3.50.0 disables glBlitFramebuffer on NVIDIA proprietary drivers and
// falls back to FlCompositorOpenGLShader in fl_compositor_opengl.cc, where
// composite_layer() passes offset = (2 * x / width) - 1.0 to a vertex shader
// whose quad vertices already span [-1, 1] in NDC (flutter/flutter#195885,
// fixed upstream in #196083). That shifts the root layer at (0, 0) by
// (-1, -1) in NDC so only the top-right quarter of the UI appears in the
// bottom-left quarter of the window.
//
// Installs a libepoxy hook on epoxy_glShaderSource,
// epoxy_glGetUniformLocation and epoxy_glUniform2f that detects
// FlCompositorOpenGLShader and adds +1.0 to its negative (-1.0) NDC layer
// offsets while preventing integer-division 0.0 scale truncation during
// window resizes.
bool ApplyNvidiaGraphicsWorkaround(const EpoxyGlHooks* custom_hooks = nullptr);

void ResetNvidiaGraphicsWorkaroundForTesting();

}  // namespace mornye

#endif  // FLUTTER_NVIDIA_GRAPHICS_WORKAROUND_H_
