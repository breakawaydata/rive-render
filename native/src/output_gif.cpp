#include "output_gif.hpp"

#include <cstdio>
#include <sstream>
#include <stdexcept>

void writeGif(const std::string& outputPath,
              int width,
              int height,
              float fps,
              const std::vector<std::vector<uint8_t>>& frames,
              const std::string& ffmpegPath)
{
    if (frames.empty())
    {
        throw std::runtime_error("No frames to encode");
    }

    // Use ffmpeg with palettegen filter for high-quality GIF output
    // Two-pass approach via complex filtergraph for best palette
    std::ostringstream cmd;
    cmd << ffmpegPath
        << " -y"
        << " -f rawvideo"
        << " -pix_fmt rgba"
        << " -s " << width << "x" << height
        << " -r " << fps
        << " -i pipe:0"
        << " -filter_complex "
           "\"[0:v]split[a][b];[a]palettegen=max_colors=256:stats_mode=diff[p];"
           "[b][p]paletteuse=dither=floyd_steinberg\""
        << " -loop 0"  // loop forever
        << " " << outputPath
        << " 2>/dev/null";

    FILE* pipe = popen(cmd.str().c_str(), "w");
    if (!pipe)
    {
        throw std::runtime_error("Failed to launch ffmpeg: " + ffmpegPath);
    }

    size_t expectedSize = static_cast<size_t>(width) * height * 4;
    for (const auto& frame : frames)
    {
        if (frame.size() < expectedSize)
        {
            pclose(pipe);
            throw std::runtime_error("Frame pixel buffer too small");
        }
        size_t written = fwrite(frame.data(), 1, expectedSize, pipe);
        if (written != expectedSize)
        {
            pclose(pipe);
            throw std::runtime_error("Failed to write frame data to ffmpeg");
        }
    }

    int status = pclose(pipe);
    if (status != 0)
    {
        throw std::runtime_error(
            "ffmpeg exited with error during GIF encoding. Is ffmpeg installed?");
    }
}
