#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Write RGBA pixel data to a PNG file
void writePng(const std::string& path,
              int width,
              int height,
              const std::vector<uint8_t>& pixels);
