/*
 * Copyright 2025 BreakAway Data
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * rive-render: Headless Rive animation renderer.
 * Reads JSON config from stdin, renders .riv files to PNG/GIF/MP4,
 * outputs JSON result to stdout.
 */

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "config.hpp"
#include "device_select.hpp"
#include "ffmpeg_process.hpp"
#include "output_gif.hpp"
#include "output_png.hpp"
#include "output_video.hpp"
#include "queue_renderer.hpp"

static std::vector<uint8_t> readFileBytes(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
    {
        throw std::runtime_error("Failed to open file: " + path);
    }
    auto size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(size);
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

static std::string readStdin()
{
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

static std::string jsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
                out += static_cast<char>(c);
        }
    }
    return out;
}

static void outputJson(bool success, const std::string& outputPath = "", int frameCount = 0,
                       const std::string& error = "", int width = 0, int height = 0,
                       const std::string& encoder = "", const std::string& encoderFallback = "")
{
    std::cout << "{\"success\":" << (success ? "true" : "false");
    if (!outputPath.empty())
        std::cout << ",\"outputPath\":\"" << jsonEscape(outputPath) << "\"";
    if (frameCount > 0)
        std::cout << ",\"frameCount\":" << frameCount;
    if (width > 0 && height > 0)
        std::cout << ",\"width\":" << width << ",\"height\":" << height;
    if (!encoder.empty())
        std::cout << ",\"encoder\":\"" << jsonEscape(encoder) << "\"";
    if (!encoderFallback.empty())
        std::cout << ",\"encoderFallback\":\"" << jsonEscape(encoderFallback) << "\"";
    if (!error.empty())
        std::cout << ",\"error\":\"" << jsonEscape(error) << "\"";
    std::cout << "}" << std::endl;
}

// An error message on one line (its last 600 characters), for the stderr log and the result's
// encoderFallback field.
static std::string oneLine(const std::string& message)
{
    constexpr size_t kMaxLength = 600;
    std::string out;
    for (char c : message)
    {
        if (c == '\n' || c == '\r')
        {
            if (!out.empty() && out.back() != ' ')
                out += " | ";
        }
        else
            out += c;
    }
    if (out.size() > kMaxLength)
        out = "..." + out.substr(out.size() - kMaxLength); // the end is where ffmpeg's error is
    return out;
}

// What `--select-device` prints: the Vulkan driver and the video encoder this host would use,
// decided without rendering or touching Vulkan.
static void outputSelectDevice(const Config& config)
{
    const IcdInputs inputs = currentIcdInputs(config.swiftshader);
    const IcdSelection icd = selectIcd(inputs);
    std::string encoder;
    if (config.output.format == "gif")
        encoder = "gif";
    else
        encoder = videoCodecName(resolveVideoCodec(config.output, autoSelectsNvenc(inputs, icd)));

    std::cout << "{\"success\":true,\"icd\":\"" << jsonEscape(icd.kind) << "\"";
    if (!icd.icdPath.empty())
        std::cout << ",\"icdFile\":\"" << jsonEscape(icd.icdPath) << "\"";
    std::cout << ",\"encoder\":\"" << jsonEscape(encoder) << "\"}" << std::endl;
}

// Render `config` into the ffmpeg process `open` starts, streaming each frame as it is drawn.
// Returns the frame count. A partial output file is removed when anything fails.
static int renderIntoEncoder(const Config& config, const std::vector<uint8_t>& rivBytes,
                             const std::function<std::unique_ptr<FfmpegEncoder>()>& open)
{
    auto encoder = open();
    int frames = 0;
    try
    {
        renderWithQueue(config, rivBytes,
                        [&](std::vector<uint8_t>&& frame)
                        {
                            encoder->push(std::move(frame));
                            frames++;
                        });
        if (frames == 0)
            throw std::runtime_error("No frames to encode");
        encoder->finish();
    }
    catch (...)
    {
        encoder.reset(); // kills and reaps ffmpeg before the file is removed
        std::remove(config.output.path.c_str());
        throw;
    }
    return frames;
}

struct VideoOutcome
{
    int frames = 0;
    std::string codec;
    std::string fallbackReason;
};

static VideoOutcome renderVideoFile(const Config& config, const std::vector<uint8_t>& rivBytes,
                                    bool autoNvenc)
{
    const auto& out = config.output;
    VideoEncodeOptions options;
    options.codec = resolveVideoCodec(out, autoNvenc);
    options.preset = out.preset;
    options.nvencPreset = out.nvencPreset;
    options.deterministic = out.deterministic;

    auto attempt = [&](const VideoEncodeOptions& opts)
    {
        return renderIntoEncoder(config, rivBytes,
                                 [&]()
                                 {
                                     return openVideoEncoder(out.path, config.width, config.height,
                                                             out.fps, out.format, opts,
                                                             config.ffmpegPath);
                                 });
    };

    VideoOutcome outcome;
    if (options.codec != VideoCodec::Nvenc)
    {
        outcome.frames = attempt(options);
        outcome.codec = videoCodecName(options.codec);
        return outcome;
    }

    // NVENC can fail for reasons only the host knows (no usable GPU, driver, session limit, an
    // ffmpeg built without it). Rendering is deterministic and fast on the GPU, so redo the whole
    // render with x264 once instead of failing the job.
    std::string reason;
    try
    {
        outcome.frames = attempt(options);
        outcome.codec = videoCodecName(VideoCodec::Nvenc);
        return outcome;
    }
    catch (const FfmpegError& e)
    {
        reason = oneLine(e.what());
    }
    std::cerr << "rive-render: h264_nvenc encode failed (" << reason << "); retrying with libx264"
              << std::endl;

    options.codec = VideoCodec::X264;
    try
    {
        outcome.frames = attempt(options);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error("h264_nvenc failed (" + reason +
                                 "), and the libx264 fallback "
                                 "failed too: " +
                                 e.what());
    }
    outcome.codec = videoCodecName(VideoCodec::X264);
    outcome.fallbackReason = reason;
    return outcome;
}

int main(int argc, char* argv[])
{
    // Initialize libcurl once, before any worker threads are spawned (the
    // CommandServer thread starts inside renderWithQueue). libcurl's
    // curl_global_init is not thread-safe and must run while no other thread
    // is active; the RAII guard also guarantees curl_global_cleanup on every
    // exit path.
    struct CurlGlobal
    {
        CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
        ~CurlGlobal() { curl_global_cleanup(); }
    } curlGlobal;

    try
    {
        // Read JSON config from stdin (or --config file)
        std::string configFile;
        bool selectDevice = false;
        for (int a = 1; a < argc; a++)
        {
            const std::string arg = argv[a];
            if (arg == "--config" && a + 1 < argc)
                configFile = argv[++a];
            else if (arg == "--select-device")
                selectDevice = true;
        }

        std::string jsonStr;
        if (!configFile.empty())
        {
            std::ifstream f(configFile);
            std::ostringstream ss;
            ss << f.rdbuf();
            jsonStr = ss.str();
        }
        else
        {
            jsonStr = readStdin();
        }

        if (jsonStr.empty())
        {
            outputJson(false, "", 0, "No input provided");
            return 1;
        }

        auto config = Config::parse(jsonStr);
        if (selectDevice)
        {
            outputSelectDevice(config);
            return 0;
        }

        auto rivBytes = readFileBytes(config.rivFile);
        resolveCanvasSize(config, rivBytes);
        const int w = config.width;
        const int h = config.height;

        // A screenshot and a png output are one frame: keep it for writePng.
        std::vector<uint8_t> singleFrame;
        auto keepFirstFrame = [&singleFrame](std::vector<uint8_t>&& frame)
        {
            if (singleFrame.empty())
                singleFrame = std::move(frame);
        };

        if (config.hasScreenshot())
        {
            renderWithQueue(config, rivBytes, keepFirstFrame);
            if (singleFrame.empty())
            {
                outputJson(false, "", 0, "No frame produced for screenshot");
                return 1;
            }
            writePng(config.screenshot.path, config.width, config.height, singleFrame);
            outputJson(true, config.screenshot.path, 1, "", w, h);
            return 0;
        }

        if (config.hasOutput())
        {
            const auto& format = config.output.format;
            if (format == "png")
            {
                renderWithQueue(config, rivBytes, keepFirstFrame);
                if (singleFrame.empty())
                {
                    outputJson(false, "", 0, "No frame produced for png output");
                    return 1;
                }
                writePng(config.output.path, config.width, config.height, singleFrame);
                outputJson(true, config.output.path, 1, "", w, h);
            }
            else if (format == "gif")
            {
                int frames = renderIntoEncoder(
                    config, rivBytes,
                    [&]()
                    {
                        return openGifEncoder(config.output.path, config.width, config.height,
                                              config.output.fps, config.ffmpegPath);
                    });
                outputJson(true, config.output.path, frames, "", w, h, "gif");
            }
            else if (format == "mp4" || format == "webm")
            {
                const IcdInputs inputs = currentIcdInputs(config.swiftshader);
                auto video =
                    renderVideoFile(config, rivBytes, autoSelectsNvenc(inputs, selectIcd(inputs)));
                outputJson(true, config.output.path, video.frames, "", w, h, video.codec,
                           video.fallbackReason);
            }
            else
            {
                outputJson(false, "", 0, "Unknown output format: " + format);
                return 1;
            }
            return 0;
        }

        outputJson(false, "", 0, "No screenshot or output config provided");
        return 1;
    }
    catch (const std::exception& e)
    {
        outputJson(false, "", 0, e.what());
        return 1;
    }
}
