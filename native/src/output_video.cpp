#include "output_video.hpp"

#include <cstdio>
#include <sstream>
#include <stdexcept>

void writeVideo(const std::string& outputPath,
                int width,
                int height,
                float fps,
                const std::vector<std::vector<uint8_t>>& frames,
                const std::string& format,
                const std::string& ffmpegPath)
{
    if (frames.empty())
    {
        throw std::runtime_error("No frames to encode");
    }

    std::ostringstream cmd;
    cmd << ffmpegPath
        << " -y"                              // overwrite output
        << " -f rawvideo"                     // input format
        << " -pix_fmt rgba"                   // pixel format
        << " -s " << width << "x" << height   // frame size
        << " -r " << fps                      // frame rate
        << " -i pipe:0";                      // read from stdin

    if (format == "mp4")
    {
        cmd << " -c:v libx264"
            << " -pix_fmt yuv420p"
            << " -preset medium"
            << " -crf 23";
    }
    else if (format == "webm")
    {
        cmd << " -c:v libvpx-vp9"
            << " -pix_fmt yuv420p"
            << " -crf 30"
            << " -b:v 0";
    }
    else
    {
        throw std::runtime_error("Unsupported video format: " + format);
    }

    cmd << " " << outputPath;
    // Redirect stderr to /dev/null to keep our stdout clean for JSON
    cmd << " 2>/dev/null";

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
            "ffmpeg exited with error. Is ffmpeg installed and in PATH?");
    }
}
