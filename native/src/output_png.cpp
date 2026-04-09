#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "output_png.hpp"
#include "stb_image_write.h"

#include <stdexcept>

void writePng(const std::string& path, int width, int height, const std::vector<uint8_t>& pixels)
{
    if (pixels.size() < static_cast<size_t>(width * height * 4))
    {
        throw std::runtime_error("Pixel buffer too small for PNG output");
    }

    int result = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4);
    if (!result)
    {
        throw std::runtime_error("Failed to write PNG: " + path);
    }
}
