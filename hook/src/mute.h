#pragma once

namespace mute {

// Mute this process's audio session (Core Audio, per-process: the rest of the PC is untouched).
// Safe to call repeatedly; returns true when the session reports muted.
bool mute_process();

}  // namespace mute
