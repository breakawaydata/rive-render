#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Write a sequence of RGBA frames to an animated GIF via an ffmpeg subprocess (spawned
// directly, no shell; see ffmpeg_process.hpp)
void writeGif(const std::string& outputPath, int width, int height, float fps,
              const std::vector<std::vector<uint8_t>>& frames,
              const std::string& ffmpegPath = "ffmpeg");
