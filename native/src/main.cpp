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

#include "rive/file.hpp"
#include "rive/artboard.hpp"
#include "rive/animation/linear_animation_instance.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/renderer/render_context.hpp"

#ifdef RIVE_VULKAN
#include "headless_renderer.hpp"
#endif

#include "config.hpp"
#include "output_gif.hpp"
#include "output_png.hpp"
#include "output_video.hpp"
#ifdef RIVE_VULKAN
#include "queue_renderer.hpp"
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

static void outputJson(bool success,
                       const std::string& outputPath = "",
                       int frameCount = 0,
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

        // Read .riv file
        auto rivBytes = readFileBytes(config.rivFile);

#ifdef RIVE_VULKAN
        // CommandQueue/CommandServer mode
        if (config.useCommandQueue)
        {
            auto result = renderWithQueue(config, rivBytes);

            if (config.hasScreenshot())
            {
                if (!result.frames.empty())
                {
                    writePng(config.screenshot.path,
                             config.width,
                             config.height,
                             result.frames[0]);
                }
                outputJson(true, config.screenshot.path, 1);
                return 0;
            }

            if (config.hasOutput())
            {
                if (config.output.format == "png" && !result.frames.empty())
                {
                    writePng(config.output.path,
                             config.width,
                             config.height,
                             result.frames[0]);
                    outputJson(true, config.output.path, 1);
                }
                else if (config.output.format == "gif")
                {
                    writeGif(config.output.path,
                             config.width,
                             config.height,
                             config.output.fps,
                             result.frames,
                             config.ffmpegPath);
                    outputJson(true,
                               config.output.path,
                               static_cast<int>(result.frames.size()));
                }
                else if (config.output.format == "mp4" ||
                         config.output.format == "webm")
                {
                    writeVideo(config.output.path,
                               config.width,
                               config.height,
                               config.output.fps,
                               result.frames,
                               config.output.format,
                               config.ffmpegPath);
                    outputJson(true,
                               config.output.path,
                               static_cast<int>(result.frames.size()));
                }
                return 0;
            }
        }

        // Direct rendering mode (simpler, no threading)
        HeadlessRenderer headless(config.width, config.height);
        auto* factory = headless.factory();
#else
        std::cerr << "ERROR: Built without Vulkan support. Cannot render."
                  << std::endl;
        outputJson(false, "", 0, "No Vulkan support");
        return 1;
#endif

        // Import .riv file
        auto file = rive::File::import(
            rive::Span<const uint8_t>(rivBytes.data(), rivBytes.size()),
            factory);
        if (!file)
        {
            outputJson(false, "", 0, "Failed to import .riv file");
            return 1;
        }

        // Get artboard
        std::unique_ptr<rive::ArtboardInstance> artboard;
        if (!config.artboard.empty())
        {
            artboard = file->artboardNamed(config.artboard);
        }
        else
        {
            artboard = file->artboardDefault();
        }
        if (!artboard)
        {
            outputJson(false, "", 0, "Failed to get artboard");
            return 1;
        }
        // Get state machine or linear animation
        std::unique_ptr<rive::StateMachineInstance> stateMachine;
        std::unique_ptr<rive::LinearAnimationInstance> linearAnimation;

        if (!config.stateMachine.empty())
        {
            stateMachine = artboard->stateMachineNamed(config.stateMachine);
        }
        else if (artboard->stateMachineCount() > 0)
        {
            stateMachine = artboard->defaultStateMachine();
        }

        // If no state machine found, fall back to linear animation
        if (!stateMachine && artboard->animationCount() > 0)
        {
            linearAnimation = artboard->animationAt(0);
        }

        // Helper: advance one time step using whichever animation is active
        auto advanceScene = [&](float step) {
            if (stateMachine)
            {
                stateMachine->advanceAndApply(step);
            }
            else if (linearAnimation)
            {
                linearAnimation->advanceAndApply(step);
            }
            artboard->advance(step);
        };

        // Screenshot mode
        if (config.hasScreenshot())
        {
            // Advance to the target timestamp
            float elapsed = 0.0f;
            float dt = 1.0f / 60.0f;
            while (elapsed < config.screenshot.timestamp)
            {
                float step = std::min(dt, config.screenshot.timestamp - elapsed);
                advanceScene(step);
                elapsed += step;
            }

#ifdef RIVE_VULKAN
            auto pixels =
                headless.renderFrame(artboard.get(), stateMachine.get());
            writePng(config.screenshot.path,
                     config.width,
                     config.height,
                     pixels);
#endif
            outputJson(true, config.screenshot.path, 1);
            return 0;
        }

        // Animation mode (GIF/video)
        if (config.hasOutput())
        {
            int totalFrames =
                static_cast<int>(config.output.fps * config.output.duration);
            float dt = 1.0f / config.output.fps;
            std::vector<std::vector<uint8_t>> frames;
            frames.reserve(totalFrames);

            for (int i = 0; i < totalFrames; i++)
            {
                advanceScene(dt);

#ifdef RIVE_VULKAN
                auto pixels =
                    headless.renderFrame(artboard.get(), stateMachine.get());
                frames.push_back(std::move(pixels));
#endif
            }

            if (config.output.format == "png")
            {
                if (!frames.empty())
                {
                    writePng(config.output.path,
                             config.width,
                             config.height,
                             frames[0]);
                }
                outputJson(true, config.output.path, 1);
            }
            else if (config.output.format == "gif")
            {
                writeGif(config.output.path,
                         config.width,
                         config.height,
                         config.output.fps,
                         frames,
                         config.ffmpegPath);
                outputJson(true,
                           config.output.path,
                           static_cast<int>(frames.size()));
            }
            else if (config.output.format == "mp4" ||
                     config.output.format == "webm")
            {
                writeVideo(config.output.path,
                           config.width,
                           config.height,
                           config.output.fps,
                           frames,
                           config.output.format,
                           config.ffmpegPath);
                outputJson(true,
                           config.output.path,
                           static_cast<int>(frames.size()));
            }
            else
            {
                outputJson(false,
                           "",
                           0,
                           "Unknown output format: " + config.output.format);
                return 1;
            }

            return 0;
        }

        outputJson(false,
                    "",
                    0,
                    "No screenshot or output config provided");
        return 1;
    }
    catch (const std::exception& e)
    {
        outputJson(false, "", 0, e.what());
        return 1;
    }
}
