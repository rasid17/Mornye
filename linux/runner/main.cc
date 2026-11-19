#include "my_application.h"
#include "nvidia_graphics_workaround.h"

int main(int argc, char** argv) {
  mornye::ApplyNvidiaGraphicsWorkaround();
  g_autoptr(MyApplication) app = my_application_new();
  return g_application_run(G_APPLICATION(app), argc, argv);
}
