#pragma once

#include <memory>
#include <string>

#include "ffmpeg_process.hpp"

// Start an ffmpeg process that encodes streamed RGBA frames into an animated GIF (spawned
// directly, no shell; see ffmpeg_process.hpp)
std::unique_ptr<FfmpegEncoder> openGifEncoder(const std::string& outputPath, int width, int height,
                                              float fps, const std::string& ffmpegPath = "ffmpeg");
