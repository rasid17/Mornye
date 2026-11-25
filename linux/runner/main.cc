#include "my_application.h"
#include "nvidia_graphics_workaround.h"
#include "system_unwinder.h"
#include "wayland_subsurface_workaround.h"

int main(int argc, char** argv) {
  use_system_unwinder(argv);
  mornye::ApplyNvidiaGraphicsWorkaround();
  mornye::ApplyWaylandSubsurfaceWorkaround();
  g_autoptr(MyApplication) app = my_application_new();
  return g_application_run(G_APPLICATION(app), argc, argv);
}
