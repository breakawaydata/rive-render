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

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef __linux__
#include <limits.h>
#include <unistd.h>
#endif

#include "config.hpp"
#include "output_gif.hpp"
#include "output_png.hpp"
#include "output_video.hpp"
#include "queue_renderer.hpp"

#ifdef __linux__
// Point the Vulkan loader at the SwiftShader ICD shipped next to the
// binary. Must run before any rive-runtime code initializes Vulkan.
static void enableBundledSwiftShader()
{
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0)
    {
        return;
    }
    exePath[len] = '\0';
    std::string dir(exePath);
    auto slash = dir.rfind('/');
    if (slash == std::string::npos)
    {
        return;
    }
    std::string icdPath = dir.substr(0, slash) + "/vk_swiftshader_icd.json";
    setenv("VK_ICD_FILENAMES", icdPath.c_str(), 1);
}
#endif

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

static void outputJson(bool success, const std::string& outputPath = "", int frameCount = 0,
                       const std::string& error = "")
{
    std::cout << "{\"success\":" << (success ? "true" : "false");
    if (!outputPath.empty())
        std::cout << ",\"outputPath\":\"" << outputPath << "\"";
    if (frameCount > 0)
        std::cout << ",\"frameCount\":" << frameCount;
    if (!error.empty())
        std::cout << ",\"error\":\"" << error << "\"";
    std::cout << "}" << std::endl;
}

int main(int argc, char* argv[])
{
    try
    {
        // Read JSON config from stdin (or --config file)
        std::string jsonStr;
        if (argc > 2 && std::string(argv[1]) == "--config")
        {
            std::ifstream f(argv[2]);
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
        auto rivBytes = readFileBytes(config.rivFile);

#ifdef __linux__
        if (config.swiftshader)
        {
            enableBundledSwiftShader();
        }
#endif

        auto result = renderWithQueue(config, rivBytes);

        if (config.hasScreenshot())
        {
            if (result.frames.empty())
            {
                outputJson(false, "", 0, "No frame produced for screenshot");
                return 1;
            }
            writePng(config.screenshot.path, config.width, config.height, result.frames[0]);
            outputJson(true, config.screenshot.path, 1);
            return 0;
        }

        if (config.hasOutput())
        {
            const auto& format = config.output.format;
            if (format == "png")
            {
                if (result.frames.empty())
                {
                    outputJson(false, "", 0, "No frame produced for png output");
                    return 1;
                }
                writePng(config.output.path, config.width, config.height, result.frames[0]);
                outputJson(true, config.output.path, 1);
            }
            else if (format == "gif")
            {
                writeGif(config.output.path, config.width, config.height, config.output.fps,
                         result.frames, config.ffmpegPath);
                outputJson(true, config.output.path, static_cast<int>(result.frames.size()));
            }
            else if (format == "mp4" || format == "webm")
            {
                writeVideo(config.output.path, config.width, config.height, config.output.fps,
                           result.frames, format, config.ffmpegPath);
                outputJson(true, config.output.path, static_cast<int>(result.frames.size()));
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
