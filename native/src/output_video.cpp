#include "output_video.hpp"

#include <sstream>
#include <stdexcept>

const char* videoCodecName(VideoCodec codec)
{
    switch (codec)
    {
    case VideoCodec::X264:
        return "libx264";
    case VideoCodec::Nvenc:
        return "h264_nvenc";
    case VideoCodec::Vp9:
        return "libvpx-vp9";
    }
    return "";
}

VideoCodec resolveVideoCodec(const OutputConfig& output, bool autoSelectsNvenc)
{
    if (output.format == "webm")
    {
        return VideoCodec::Vp9;
    }
    if (output.deterministic || output.encoder == "x264")
    {
        return VideoCodec::X264;
    }
    if (output.encoder == "nvenc")
    {
        return VideoCodec::Nvenc;
    }
    return autoSelectsNvenc ? VideoCodec::Nvenc : VideoCodec::X264;
}

std::vector<std::string> buildVideoArgs(const std::string& outputPath, int width, int height,
                                        float fps, const std::string& format,
                                        const VideoEncodeOptions& options)
{
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

    if (format == "mp4" && options.codec == VideoCodec::Nvenc)
    {
        args.insert(args.end(),
                    {"-c:v", "h264_nvenc", "-pix_fmt", "yuv420p", "-preset", options.nvencPreset,
                     "-rc", "vbr", "-cq", "23", "-b:v", "0", "-profile:v", "high"});
    }
    else if (format == "mp4")
    {
        args.insert(args.end(), {"-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset",
                                 options.preset, "-crf", "23"});
        if (options.deterministic)
        {
            args.insert(args.end(), {"-x264-params", "threads=1:sliced-threads=0"});
        }
    }
    else if (format == "webm")
    {
        args.insert(args.end(),
                    {"-c:v", "libvpx-vp9", "-pix_fmt", "yuv420p", "-crf", "30", "-b:v", "0"});
        if (options.deterministic)
        {
            args.insert(args.end(), {"-threads", "1", "-row-mt", "0"});
        }
        else
        {
            args.insert(args.end(), {"-row-mt", "1"});
        }
    }
    else
    {
        throw std::runtime_error("Unsupported video format: " + format);
    }

    args.push_back(outputPath);
    return args;
}

std::unique_ptr<FfmpegEncoder> openVideoEncoder(const std::string& outputPath, int width,
                                                int height, float fps, const std::string& format,
                                                const VideoEncodeOptions& options,
                                                const std::string& ffmpegPath)
{
    // No shell: ffmpeg is spawned directly, its stdout goes to /dev/null (our stdout carries the
    // JSON result) and its stderr is captured for the error message.
    return std::make_unique<FfmpegEncoder>(
        ffmpegPath, buildVideoArgs(outputPath, width, height, fps, format, options), width, height,
        "video encoding");
}
