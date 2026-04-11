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

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif

#include "config.hpp"
#include "output_gif.hpp"
#include "output_png.hpp"
#include "output_video.hpp"
#ifdef RIVE_VULKAN
#include "queue_renderer.hpp"
#endif

#ifdef __APPLE__
// Preload MoltenVK from the binary's own directory so it's available
// without requiring the user to set DYLD_LIBRARY_PATH or install it via
// Homebrew. This handles the case where libMoltenVK.dylib is bundled
// alongside the binary (as done by the rive-render npm postinstall).
// The rive-runtime's vulkan_library.cpp has been patched at build time
// to also search /opt/homebrew/lib and /usr/local/lib as fallbacks.
static void preloadMoltenVK()
{
    char exePath[1024];
    uint32_t size = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &size) == 0)
    {
        std::string dir(exePath);
        dir = dir.substr(0, dir.rfind('/'));
        std::string mvkPath = dir + "/libMoltenVK.dylib";
        if (dlopen(mvkPath.c_str(), RTLD_NOW | RTLD_GLOBAL))
            return;
    }
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
#ifdef __APPLE__
    preloadMoltenVK();
#endif

#ifndef RIVE_VULKAN
    std::cerr << "ERROR: Built without Vulkan support. Cannot render." << std::endl;
    outputJson(false, "", 0, "No Vulkan support");
    return 1;
#else

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
#endif
}
