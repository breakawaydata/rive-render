#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Run ffmpeg as a child process and stream raw RGBA frames into its stdin.
//
// ffmpeg is started with posix_spawnp and an explicit argv, never through a shell, so it works
// in images that have no /bin/sh (distroless / hardened bases, where popen() fails). A bare
// program name such as "ffmpeg" is looked up on PATH; a path containing '/' is used as is.
//
// `args` is the full argv after the program name. ffmpeg's stdout goes to /dev/null so it
// cannot corrupt the JSON result on our stdout; its stderr is captured and the tail of it is
// included in the exception thrown when ffmpeg cannot be started or exits non-zero.
//
// Every frame must hold at least width * height * 4 bytes; exactly that many are written.
// `what` names the operation in error messages (e.g. "video encoding").
void runFfmpegWithFrames(const std::string& ffmpegPath, const std::vector<std::string>& args,
                         int width, int height, const std::vector<std::vector<uint8_t>>& frames,
                         const std::string& what);
