"""Check the Linux runner workarounds: the NVIDIA compositor offset and Niri IPC."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
PACKAGE_SCRIPT = REPO_ROOT / "scripts" / "package_linux.sh"
CPP_HEADER = REPO_ROOT / "linux" / "runner" / "nvidia_graphics_workaround.h"
CPP_SOURCE = REPO_ROOT / "linux" / "runner" / "nvidia_graphics_workaround.cc"

CPP_TEST_SOURCE = r"""
#include "nvidia_graphics_workaround.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr const char* kFlutterCompositorVertexShader =
    "attribute vec2 position;\n"
    "attribute vec2 in_texcoord;\n"
    "uniform vec2 offset;\n"
    "uniform vec2 scale;\n"
    "varying vec2 texcoord;\n"
    "\n"
    "void main() {\n"
    "  gl_Position = vec4(offset + position * scale, 0, 1);\n"
    "  texcoord = in_texcoord;\n"
    "}\n";

constexpr const char* kOtherVertexShader =
    "attribute vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0, 1); }\n";

mornye::GlShaderSourceFn g_epoxy_shader_source = nullptr;
mornye::GlGetUniformLocationFn g_epoxy_get_uniform_location = nullptr;
mornye::GlUniform2fFn g_epoxy_uniform2f = nullptr;

int g_shader_source_calls = 0;
int g_last_uniform_location = -1;
float g_last_u0 = 0.0f;
float g_last_u1 = 0.0f;

void RealGlShaderSource(unsigned int, int, const char* const*, const int*) {
  ++g_shader_source_calls;
}

void StubGlShaderSource(unsigned int shader,
                        int count,
                        const char* const* string,
                        const int* length) {
  // Mimics libepoxy's global_rewrite_ptr: only resolves if slot == stub.
  if (g_epoxy_shader_source == &StubGlShaderSource) {
    g_epoxy_shader_source = &RealGlShaderSource;
  }
  g_epoxy_shader_source(shader, count, string, length);
}

int RealGlGetUniformLocation(unsigned int program, const char* name) {
  if (program == 42) {
    if (strcmp(name, "offset") == 0) return 10;
    if (strcmp(name, "scale") == 0) return 11;
  }
  return 99;
}

int StubGlGetUniformLocation(unsigned int program, const char* name) {
  if (g_epoxy_get_uniform_location == &StubGlGetUniformLocation) {
    g_epoxy_get_uniform_location = &RealGlGetUniformLocation;
  }
  return g_epoxy_get_uniform_location(program, name);
}

void RealGlUniform2f(int location, float v0, float v1) {
  g_last_uniform_location = location;
  g_last_u0 = v0;
  g_last_u1 = v1;
}

void StubGlUniform2f(int location, float v0, float v1) {
  if (g_epoxy_uniform2f == &StubGlUniform2f) {
    g_epoxy_uniform2f = &RealGlUniform2f;
  }
  g_epoxy_uniform2f(location, v0, v1);
}

bool Near(float a, float b) {
  return fabsf(a - b) < 1e-5f;
}

void ResetMocks() {
  mornye::ResetNvidiaGraphicsWorkaroundForTesting();
  g_epoxy_shader_source = &StubGlShaderSource;
  g_epoxy_get_uniform_location = &StubGlGetUniformLocation;
  g_epoxy_uniform2f = &StubGlUniform2f;
  g_shader_source_calls = 0;
  g_last_uniform_location = -1;
  g_last_u0 = 0.0f;
  g_last_u1 = 0.0f;
  unsetenv("MORNYE_DISABLE_NVIDIA_WORKAROUND");
  unsetenv("MORNYE_DISABLE_NVIDIA_ZINK_WORKAROUND");
}

}  // namespace

int main() {
  mornye::EpoxyGlHooks hooks = {
      &g_epoxy_shader_source,
      &g_epoxy_get_uniform_location,
      &g_epoxy_uniform2f,
  };

  // 1. Shifts Flutter 3.50.0's buggy (-1.0, -1.0) offset to (0.0, 0.0) and
  //    clamps 0.0 integer-division scale truncation to 1.0.
  ResetMocks();
  assert(mornye::ApplyNvidiaGraphicsWorkaround(&hooks));

  // Unrelated shader before Flutter's compositor shader is ignored.
  g_epoxy_shader_source(1, 1, &kOtherVertexShader, nullptr);
  assert(g_shader_source_calls == 1);
  assert(g_epoxy_get_uniform_location(7, "offset") == 99);
  g_epoxy_uniform2f(99, -1.0f, -1.0f);
  assert(Near(g_last_u0, -1.0f) && Near(g_last_u1, -1.0f));

  // Flutter's compositor shader registers offset (10) and scale (11).
  g_epoxy_shader_source(2, 1, &kFlutterCompositorVertexShader, nullptr);
  assert(g_shader_source_calls == 2);
  assert(g_epoxy_get_uniform_location(42, "offset") == 10);
  assert(g_epoxy_get_uniform_location(42, "scale") == 11);

  // Root layer in Flutter 3.50.0: (-1.0, -1.0) -> (0.0, 0.0).
  g_epoxy_uniform2f(10, -1.0f, -1.0f);
  assert(g_last_uniform_location == 10);
  assert(Near(g_last_u0, 0.0f) && Near(g_last_u1, 0.0f));

  // Fixed Flutter (#196083) passing (0.0, 0.0) is left untouched.
  g_epoxy_uniform2f(10, 0.0f, 0.0f);
  assert(Near(g_last_u0, 0.0f) && Near(g_last_u1, 0.0f));

  // Integer-division truncation during resize: (0.0, 1.0) -> (1.0, 1.0).
  g_epoxy_uniform2f(11, 0.0f, 1.0f);
  assert(g_last_uniform_location == 11);
  assert(Near(g_last_u0, 1.0f) && Near(g_last_u1, 1.0f));

  // Normal scale passes through untouched.
  g_epoxy_uniform2f(11, 1.0f, 1.0f);
  assert(Near(g_last_u0, 1.0f) && Near(g_last_u1, 1.0f));

  // 2. Opt-out environment variable disables the hook.
  ResetMocks();
  setenv("MORNYE_DISABLE_NVIDIA_WORKAROUND", "1", 1);
  assert(!mornye::ApplyNvidiaGraphicsWorkaround(&hooks));
  assert(g_epoxy_shader_source == &StubGlShaderSource);

  // 4. Mini-player geometry hints (< 800x600) gain GDK_HINT_MAX_SIZE so
  //    Wayland tiling compositors (such as Niri) constrain the window to the
  //    mini-player size, while desktop shell hints (800x600) remain min-only.
  mornye::MornyeGdkGeometry mini_geom = {};
  mini_geom.min_width = 360;
  mini_geom.min_height = 140;
  unsigned int mini_mask = 1u << 1;  // GDK_HINT_MIN_SIZE
  assert(mornye::NormalizeWindowGeometryHints(&mini_geom, &mini_mask));
  assert(mini_geom.max_width == 360);
  assert(mini_geom.max_height == 140);
  assert(mini_mask == ((1u << 1) | (1u << 2)));

  mornye::MornyeGdkGeometry full_geom = {};
  full_geom.min_width = 800;
  full_geom.min_height = 600;
  unsigned int full_mask = 1u << 1;  // GDK_HINT_MIN_SIZE
  assert(!mornye::NormalizeWindowGeometryHints(&full_geom, &full_mask));
  assert(full_geom.max_width == 0);
  assert(full_geom.max_height == 0);
  assert(full_mask == (1u << 1));

  // 5. ParseNiriWindowStateForPid ignores adversarial window titles containing
  //    JSON-like substrings and extracts the matching PID's window state.
  const char* sample_niri_json =
      "{\"Ok\":{\"Windows\":["
      "{\"id\":101,\"title\":\"fake {\\\"pid\\\":777,\\\"is_floating\\\":true}\","
      "\"app_id\":\"kitty\",\"pid\":555,\"is_focused\":true,\"is_floating\":false,"
      "\"layout\":{\"tile_size\":[1912.0,1028.0],\"window_size\":[1912,1028],"
      "\"tile_pos_in_workspace_view\":null}},"
      "{\"id\":225,\"title\":\"Mornye\",\"app_id\":\"com.zarz.mornye\","
      "\"pid\":777,\"is_focused\":false,\"is_floating\":false,"
      "\"layout\":{\"tile_size\":[1912.0,1028.0],\"window_size\":[1912,1028],"
      "\"tile_pos_in_workspace_view\":[1536.0,16.0]}}"
      "]}}\n";
  mornye::NiriWindowState niri_state = {};
  assert(mornye::ParseNiriWindowStateForPid(sample_niri_json, 777, &niri_state));
  assert(niri_state.found);
  assert(niri_state.id == 225);
  assert(!niri_state.is_floating);
  assert(niri_state.tile_width == 1912);
  assert(niri_state.tile_height == 1028);
  assert(niri_state.has_floating_pos);
  assert(Near(static_cast<float>(niri_state.floating_x), 1536.0f));
  assert(Near(static_cast<float>(niri_state.floating_y), 16.0f));

  printf("ALL_CPP_TESTS_PASSED\n");
  return 0;
}
"""


class LinuxNvidiaWorkaroundTest(unittest.TestCase):
    def test_shell_scripts_pass_syntax_check(self):
        res = subprocess.run(
            ["/bin/bash", "-n", str(PACKAGE_SCRIPT)],
            text=True,
            capture_output=True,
        )
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_cpp_epoxy_shader_offset_hook(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            test_src = tmp_path / "test_workaround.cc"
            test_bin = tmp_path / "test_workaround"
            test_src.write_text(CPP_TEST_SOURCE, encoding="utf-8")
            compile_res = subprocess.run(
                [
                    "g++",
                    "-std=c++14",
                    "-Wall",
                    "-Werror",
                    "-Wextra",
                    "-I",
                    str(CPP_HEADER.parent),
                    str(test_src),
                    str(CPP_SOURCE),
                    "-ldl",
                    "-o",
                    str(test_bin),
                ],
                text=True,
                capture_output=True,
            )
            self.assertEqual(compile_res.returncode, 0, compile_res.stderr)
            run_res = subprocess.run(
                [str(test_bin)],
                text=True,
                capture_output=True,
            )
            self.assertEqual(run_res.returncode, 0, run_res.stderr)
            self.assertIn("ALL_CPP_TESTS_PASSED", run_res.stdout)

    def test_niri_mini_player_floating_ipc_lifecycle(self):
        import socket
        import threading

        with tempfile.TemporaryDirectory() as tmp:
            sock_path = Path(tmp) / "niri.sock"
            received = []
            state = {"is_floating": False, "pos": "null", "windows_queries": 0}

            srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            srv.bind(str(sock_path))
            srv.listen(16)
            stop_flag = False

            def server_loop():
                while not stop_flag:
                    try:
                        conn, _ = srv.accept()
                    except OSError:
                        break
                    data = conn.recv(4096).decode("utf-8").strip()
                    if data:
                        received.append(data)
                    if data == '"Windows"':
                        state["windows_queries"] += 1
                        fl = "true" if state["is_floating"] else "false"
                        reply = (
                            '{"Ok":{"Windows":[{"id":225,"title":"Mornye",'
                            '"app_id":"com.zarz.mornye","pid":777,'
                            f'"is_focused":true,"is_floating":{fl},'
                            '"layout":{"tile_size":[1912.0,1028.0],'
                            '"window_size":[1912,1028],'
                            f'"tile_pos_in_workspace_view":{state["pos"]}}}'
                            "]}}\n"
                        )
                        conn.sendall(reply.encode("utf-8"))
                        if state["windows_queries"] == 2:
                            # Simulate the user dragging the floating mini player
                            # to (1200, 48) below Niri's 44px top bar (92.0).
                            state["pos"] = "[1200.0,92.0]"
                    else:
                        if "MoveWindowToFloating" in data:
                            state["is_floating"] = True
                            state["pos"] = "[1544.0,60.0]"
                        elif "MoveWindowToTiling" in data:
                            state["is_floating"] = False
                            state["pos"] = "null"
                        conn.sendall(b'{"Ok":"Handled"}\n')
                    conn.close()

            thread = threading.Thread(target=server_loop, daemon=True)
            thread.start()
            try:
                driver_src = Path(tmp) / "niri_driver.cc"
                driver_bin = Path(tmp) / "niri_driver"
                driver_src.write_text(
                    '#include <assert.h>\n'
                    '#include "nvidia_graphics_workaround.h"\n'
                    'int main() {\n'
                    '  mornye::ResetNvidiaGraphicsWorkaroundForTesting();\n'
                    '  assert(mornye::SyncNiriMiniPlayerWindow(true, 360, 140, 1544, 16, 777));\n'
                    '  assert(mornye::SyncNiriMiniPlayerWindow(true, 360, 248, 0, 0, 777));\n'
                    '  assert(mornye::SyncNiriMiniPlayerWindow(false, 0, 0, 0, 0, 777));\n'
                    '  return 0;\n'
                    '}\n',
                    encoding="utf-8",
                )
                comp = subprocess.run(
                    [
                        "g++",
                        "-std=c++14",
                        "-Wall",
                        "-Werror",
                        "-I",
                        str(CPP_HEADER.parent),
                        str(driver_src),
                        str(CPP_SOURCE),
                        "-ldl",
                        "-o",
                        str(driver_bin),
                    ],
                    text=True,
                    capture_output=True,
                )
                self.assertEqual(comp.returncode, 0, comp.stderr)
                env = os.environ.copy()
                env["NIRI_SOCKET"] = str(sock_path)
                run_res = subprocess.run(
                    [str(driver_bin)],
                    env=env,
                    text=True,
                    capture_output=True,
                )
                self.assertEqual(run_res.returncode, 0, run_res.stderr)
            finally:
                stop_flag = True
                srv.close()

            self.assertIn(
                '{"Action":{"MoveWindowToFloating":{"id":225}}}', received
            )
            self.assertIn(
                '{"Action":{"SetWindowWidth":{"id":225,"change":{"SetFixed":360}}}}',
                received,
            )
            self.assertIn(
                '{"Action":{"SetWindowHeight":{"id":225,"change":{"SetFixed":140}}}}',
                received,
            )
            self.assertIn(
                '{"Action":{"MoveFloatingWindow":{"id":225,"x":{"SetFixed":1544.0},"y":{"SetFixed":16.0}}}}',
                received,
            )
            # Lyrics toggle keeps the dragged floating position (1200, 48).
            self.assertIn(
                '{"Action":{"SetWindowHeight":{"id":225,"change":{"SetFixed":248}}}}',
                received,
            )
            self.assertIn(
                '{"Action":{"MoveFloatingWindow":{"id":225,"x":{"SetFixed":1200.0},"y":{"SetFixed":48.0}}}}',
                received,
            )
            # Closing mini player returns window to tiling and restores column width.
            self.assertIn(
                '{"Action":{"MoveWindowToTiling":{"id":225}}}', received
            )
            self.assertIn(
                '{"Action":{"SetWindowWidth":{"id":225,"change":{"SetFixed":1912}}}}',
                received,
            )
            self.assertIn(
                '{"Action":{"ResetWindowHeight":{"id":225}}}', received
            )


if __name__ == "__main__":
    unittest.main()
