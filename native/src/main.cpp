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

#include "rive/animation/linear_animation_instance.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/artboard.hpp"
#include "rive/file.hpp"
#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_boolean_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_color_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_enum_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_number_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_string_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_runtime.hpp"

#ifdef RIVE_VULKAN
#include "headless_renderer.hpp"
#endif

#include "asset_loader.hpp"
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
                    writePng(config.screenshot.path, config.width, config.height, result.frames[0]);
                }
                outputJson(true, config.screenshot.path, 1);
                return 0;
            }

            if (config.hasOutput())
            {
                if (config.output.format == "png" && !result.frames.empty())
                {
                    writePng(config.output.path, config.width, config.height, result.frames[0]);
                    outputJson(true, config.output.path, 1);
                }
                else if (config.output.format == "gif")
                {
                    writeGif(config.output.path, config.width, config.height, config.output.fps,
                             result.frames, config.ffmpegPath);
                    outputJson(true, config.output.path, static_cast<int>(result.frames.size()));
                }
                else if (config.output.format == "mp4" || config.output.format == "webm")
                {
                    writeVideo(config.output.path, config.width, config.height, config.output.fps,
                               result.frames, config.output.format, config.ffmpegPath);
                    outputJson(true, config.output.path, static_cast<int>(result.frames.size()));
                }
                return 0;
            }
        }

        // Direct rendering mode (simpler, no threading)
        HeadlessRenderer headless(config.width, config.height);
        auto* factory = headless.factory();
#else
        std::cerr << "ERROR: Built without Vulkan support. Cannot render." << std::endl;
        outputJson(false, "", 0, "No Vulkan support");
        return 1;
#endif

        // Set up asset loader for referenced images/fonts
        auto assetLoader = rive::make_rcp<MappedAssetLoader>();
        assetLoader->imagePaths = config.assets.images;
        assetLoader->fontPaths = config.assets.fonts;

        // Import .riv file
        rive::ImportResult importResult;
        auto file = rive::File::import(rive::Span<const uint8_t>(rivBytes.data(), rivBytes.size()),
                                       factory, &importResult, std::move(assetLoader));
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

        // Apply viewModelData (if provided)
        if (!config.viewModelData.properties.empty())
        {
            // Get or create a ViewModel instance from the file
            rive::rcp<rive::ViewModelInstance> vmInstance;

            if (!config.viewModelData.instance.empty())
            {
                // Create by viewModel name + instance name
                if (!config.viewModelData.viewModel.empty())
                {
                    vmInstance = file->createViewModelInstance(config.viewModelData.viewModel,
                                                               config.viewModelData.instance);
                }
                else
                {
                    vmInstance = file->createViewModelInstance(config.viewModelData.instance);
                }
            }
            else
            {
                // Create default instance for the artboard
                vmInstance = file->createDefaultViewModelInstance(artboard.get());
            }

            if (vmInstance)
            {
                // Use ViewModelRuntime API to set properties
                auto vmRuntime = rive::rcp<rive::ViewModelInstanceRuntime>(
                    new rive::ViewModelInstanceRuntime(vmInstance));

                for (auto& [path, prop] : config.viewModelData.properties)
                {
                    if (prop.type == "string")
                    {
                        auto* p = vmRuntime->propertyString(path);
                        if (p)
                            p->value(prop.stringValue);
                    }
                    else if (prop.type == "number")
                    {
                        auto* p = vmRuntime->propertyNumber(path);
                        if (p)
                            p->value(prop.numberValue);
                    }
                    else if (prop.type == "boolean")
                    {
                        auto* p = vmRuntime->propertyBoolean(path);
                        if (p)
                            p->value(prop.boolValue);
                    }
                    else if (prop.type == "color")
                    {
                        auto* p = vmRuntime->propertyColor(path);
                        if (p)
                            p->value(static_cast<int>(prop.colorValue));
                    }
                    else if (prop.type == "enum")
                    {
                        auto* p = vmRuntime->propertyEnum(path);
                        if (p)
                            p->value(prop.stringValue);
                    }
                }

                // Bind the ViewModel instance to the artboard
                if (stateMachine)
                {
                    stateMachine->bindViewModelInstance(vmInstance);
                }
                artboard->bindViewModelInstance(vmInstance);
            }
        }

        // Apply stateMachineInputs (if provided and state machine exists)
        if (stateMachine)
        {
            for (auto& [name, value] : config.stateMachineNumberInputs)
            {
                auto* input = stateMachine->getNumber(name);
                if (input)
                {
                    input->value(value);
                }
            }
            for (auto& [name, value] : config.stateMachineBoolInputs)
            {
                auto* input = stateMachine->getBool(name);
                if (input)
                {
                    input->value(value);
                }
            }
        }

        // Helper: advance one time step using whichever animation is active
        auto advanceScene = [&](float step)
        {
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
            auto pixels = headless.renderFrame(artboard.get(), stateMachine.get());
            writePng(config.screenshot.path, config.width, config.height, pixels);
#endif
            outputJson(true, config.screenshot.path, 1);
            return 0;
        }

        // Animation mode (GIF/video)
        if (config.hasOutput())
        {
            int totalFrames = static_cast<int>(config.output.fps * config.output.duration);
            float dt = 1.0f / config.output.fps;
            std::vector<std::vector<uint8_t>> frames;
            frames.reserve(totalFrames);

            for (int i = 0; i < totalFrames; i++)
            {
                advanceScene(dt);

#ifdef RIVE_VULKAN
                auto pixels = headless.renderFrame(artboard.get(), stateMachine.get());
                frames.push_back(std::move(pixels));
#endif
            }

            if (config.output.format == "png")
            {
                if (!frames.empty())
                {
                    writePng(config.output.path, config.width, config.height, frames[0]);
                }
                outputJson(true, config.output.path, 1);
            }
            else if (config.output.format == "gif")
            {
                writeGif(config.output.path, config.width, config.height, config.output.fps, frames,
                         config.ffmpegPath);
                outputJson(true, config.output.path, static_cast<int>(frames.size()));
            }
            else if (config.output.format == "mp4" || config.output.format == "webm")
            {
                writeVideo(config.output.path, config.width, config.height, config.output.fps,
                           frames, config.output.format, config.ffmpegPath);
                outputJson(true, config.output.path, static_cast<int>(frames.size()));
            }
            else
            {
                outputJson(false, "", 0, "Unknown output format: " + config.output.format);
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
