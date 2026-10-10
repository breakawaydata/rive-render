#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config.hpp"
#include "ffmpeg_process.hpp"

enum class VideoCodec
{
    X264,  // libx264, mp4
    Nvenc, // h264_nvenc, mp4
    Vp9,   // libvpx-vp9, webm
};

struct VideoEncodeOptions
{
    VideoCodec codec = VideoCodec::X264;
    std::string preset = "veryfast";
    std::string nvencPreset = "p4";
    // Single-threaded, bit-reproducible output (see buildVideoArgs).
    bool deterministic = false;
};

// ffmpeg's name for the codec: "libx264", "h264_nvenc" or "libvpx-vp9".
const char* videoCodecName(VideoCodec codec);

// The codec a video output uses. webm is always VP9. For mp4, `deterministic` forces x264,
// "x264" and "nvenc" are taken as written, and "auto" is NVENC only when `autoSelectsNvenc`.
VideoCodec resolveVideoCodec(const OutputConfig& output, bool autoSelectsNvenc);

// The ffmpeg argv (after the program name) that encodes raw RGBA frames from stdin into
// `outputPath`. Throws std::runtime_error for a format that is not mp4 or webm.
//
// deterministic is for byte-reproducible output (the committed reference mp4s). Multi-threaded
// x264/libvpx rate control reads neighboring macroblocks in thread-scheduling order, which
// produces different output on different CPU topologies; CI runs regressed on asset-heavy scenes
// before the encoders were pinned to one thread. It costs speed, so it is opt-in.
std::vector<std::string> buildVideoArgs(const std::string& outputPath, int width, int height,
                                        float fps, const std::string& format,
                                        const VideoEncodeOptions& options);

// Start an ffmpeg process that encodes streamed RGBA frames into a video file (spawned directly,
// no shell; see ffmpeg_process.hpp).
std::unique_ptr<FfmpegEncoder> openVideoEncoder(const std::string& outputPath, int width,
                                                int height, float fps, const std::string& format,
                                                const VideoEncodeOptions& options,
                                                const std::string& ffmpegPath = "ffmpeg");
