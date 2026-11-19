#ifndef RUNNER_NIRI_WORKSPACE_FOLLOWER_H_
#define RUNNER_NIRI_WORKSPACE_FOLLOWER_H_

namespace mornye {

// Niri has no sticky windows, so a floating mini player stays behind on its
// workspace when the user switches away. While started, this listens to
// Niri's event stream and moves the mini player to every workspace that
// becomes focused, without taking the focus. Does nothing outside Niri.
void StartNiriWorkspaceFollower();
void StopNiriWorkspaceFollower();

}  // namespace mornye

#endif  // RUNNER_NIRI_WORKSPACE_FOLLOWER_H_
