#ifndef RUNNER_WAYLAND_SUBSURFACE_WORKAROUND_H_
#define RUNNER_WAYLAND_SUBSURFACE_WORKAROUND_H_

namespace mornye {

// Flutter 3.50 draws a Wayland window through a subsurface that it puts in
// synchronized mode (fl_subsurface.cc), and its raster thread holds a mutex
// while eglSwapBuffers waits for the compositor to hand a buffer back. A
// synchronized subsurface gets its buffers back only after the parent window
// commits, which needs the GTK thread, and that thread blocks on the same
// mutex as soon as the window is resized: the app freezes for good on
// maximize or a mini player switch (flutter/flutter#196663).
//
// Points libflutter's import of wl_proxy_marshal at a wrapper that sends
// wl_subsurface.set_desync where libflutter asks for set_sync, so the
// compositor applies and releases the subsurface's buffers on its own.
// Returns whether the import was replaced. Set
// MORNYE_DISABLE_SUBSURFACE_DESYNC=1 to leave libflutter as it is.
bool ApplyWaylandSubsurfaceWorkaround();

}  // namespace mornye

#endif  // RUNNER_WAYLAND_SUBSURFACE_WORKAROUND_H_
