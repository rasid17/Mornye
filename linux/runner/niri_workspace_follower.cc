#include "niri_workspace_follower.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvidia_graphics_workaround.h"

namespace mornye {
namespace {

struct Follower {
  GSocketConnection* connection;
  GDataInputStream* input;
  GCancellable* cancellable;
};

// The follower that is listening, or nullptr. A stopped follower frees itself
// once its pending read reports the cancellation.
Follower* g_follower = nullptr;

void FreeFollower(Follower* follower) {
  g_clear_object(&follower->input);
  if (follower->connection != nullptr) {
    g_io_stream_close(G_IO_STREAM(follower->connection), nullptr, nullptr);
  }
  g_clear_object(&follower->connection);
  g_clear_object(&follower->cancellable);
  g_free(follower);
}

// Returns the id in a {"WorkspaceActivated":{"id":N,"focused":true}} event,
// or 0 for any other event.
guint64 FocusedWorkspaceId(const char* line) {
  if (strncmp(line, "{\"WorkspaceActivated\"", 21) != 0 ||
      strstr(line, "\"focused\":true") == nullptr) {
    return 0;
  }
  const char* id = strstr(line, "\"id\":");
  return id == nullptr ? 0 : g_ascii_strtoull(id + 5, nullptr, 10);
}

void ReadNextEvent(Follower* follower);

void OnEventLine(GObject* source, GAsyncResult* result, gpointer user_data) {
  auto* follower = static_cast<Follower*>(user_data);
  g_autofree char* line = g_data_input_stream_read_line_finish_utf8(
      G_DATA_INPUT_STREAM(source), result, nullptr, nullptr);
  if (line == nullptr || g_cancellable_is_cancelled(follower->cancellable)) {
    // Stopped, or Niri closed the stream.
    if (g_follower == follower) g_follower = nullptr;
    FreeFollower(follower);
    return;
  }

  const guint64 workspace = FocusedWorkspaceId(line);
  const unsigned long long window = NiriMiniPlayerWindowId();
  if (workspace != 0 && window != 0) {
    char command[192];
    snprintf(command, sizeof(command),
             "{\"Action\":{\"MoveWindowToWorkspace\":{\"window_id\":%llu,"
             "\"reference\":{\"Id\":%llu},\"focus\":false}}}\n",
             window, static_cast<unsigned long long>(workspace));
    SendNiriSocketCommand(command, nullptr);
  }
  ReadNextEvent(follower);
}

void ReadNextEvent(Follower* follower) {
  g_data_input_stream_read_line_async(follower->input, G_PRIORITY_DEFAULT,
                                      follower->cancellable, OnEventLine,
                                      follower);
}

}  // namespace

void StartNiriWorkspaceFollower() {
  const char* socket_path = getenv("NIRI_SOCKET");
  if (g_follower != nullptr || socket_path == nullptr ||
      socket_path[0] == '\0') {
    return;
  }

  g_autoptr(GSocketClient) client = g_socket_client_new();
  g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket_path);
  g_autoptr(GError) error = nullptr;
  GSocketConnection* connection = g_socket_client_connect(
      client, G_SOCKET_CONNECTABLE(address), nullptr, &error);
  if (connection == nullptr) {
    if (NiriDebugEnabled()) {
      fprintf(stderr, "[mornye niri] event stream: %s\n", error->message);
    }
    return;
  }
  static const char kRequest[] = "\"EventStream\"\n";
  if (!g_output_stream_write_all(
          g_io_stream_get_output_stream(G_IO_STREAM(connection)), kRequest,
          sizeof(kRequest) - 1, nullptr, nullptr, &error)) {
    if (NiriDebugEnabled()) {
      fprintf(stderr, "[mornye niri] event stream: %s\n", error->message);
    }
    g_object_unref(connection);
    return;
  }

  auto* follower = g_new0(Follower, 1);
  follower->connection = connection;
  follower->input = g_data_input_stream_new(
      g_io_stream_get_input_stream(G_IO_STREAM(connection)));
  follower->cancellable = g_cancellable_new();
  g_follower = follower;
  // The first line is Niri's reply to the request; it is not a
  // WorkspaceActivated event, so the handler skips it like any other.
  ReadNextEvent(follower);
}

void StopNiriWorkspaceFollower() {
  if (g_follower == nullptr) return;
  g_cancellable_cancel(g_follower->cancellable);
  g_follower = nullptr;
}

}  // namespace mornye
