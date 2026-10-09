#include "output_video.hpp"

#include "ffmpeg_process.hpp"

#include <sstream>
#include <stdexcept>

void writeVideo(const std::string& outputPath, int width, int height, float fps,
                const std::vector<std::vector<uint8_t>>& frames, const std::string& format,
                const std::string& ffmpegPath)
{
    if (frames.empty())
    {
        throw std::runtime_error("No frames to encode");
    }

    // Formatted exactly as the old shell command line streamed them, so ffmpeg sees the same
    // values (e.g. "29.97", "30").
    std::ostringstream size;
    size << width << "x" << height;
    std::ostringstream rate;
    rate << fps;

    std::vector<std::string> args = {
        "-y",                   // overwrite output
        "-f",       "rawvideo", // input format
        "-pix_fmt", "rgba",     // pixel format
        "-s",       size.str(), // frame size
        "-r",       rate.str(), // frame rate
        "-i",       "pipe:0",   // read from stdin
    };

    // Force single-threaded encoding for bit-reproducible output.
    // Multi-threaded x264/libvpx rate control reads neighboring macroblocks
    // in thread-scheduling order, which produces different output on
    // different CPU topologies — CI runs regressed on asset-heavy scenes.
    if (format == "mp4")
    {
        args.insert(args.end(), {"-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "medium",
                                 "-crf", "23", "-x264-params", "threads=1:sliced-threads=0"});
    }
    else if (format == "webm")
    {
        args.insert(args.end(), {"-c:v", "libvpx-vp9", "-pix_fmt", "yuv420p", "-crf", "30", "-b:v",
                                 "0", "-threads", "1", "-row-mt", "0"});
    }
    else
    {
        throw std::runtime_error("Unsupported video format: " + format);
    }

    args.push_back(outputPath);

    // No shell: ffmpeg is spawned directly, its stdout goes to /dev/null (our stdout carries the
    // JSON result) and its stderr is captured for the error message.
    runFfmpegWithFrames(ffmpegPath, args, width, height, frames, "video encoding");
}
