#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct ScreenshotConfig
{
    std::string path;
    float timestamp = 0.0f;
};

struct OutputConfig
{
    std::string format; // "png", "gif", "mp4", "webm"
    std::string path;
    float fps = 30.0f;
    float duration = 0.0f;
    int quality = 90;
};

// One entry inside a `{ "type": "list" }` PropertyValue.
struct ListItemConfig;

struct ViewModelPropertyValue
{
    // "string", "number", "boolean", "color", "enum", "image", "list"
    std::string type;
    std::string stringValue;
    float numberValue = 0.0f;
    bool boolValue = false;
    uint32_t colorValue = 0;

    // image: filesystem path (PNG/JPEG/WebP).
    // Stored in `stringValue` so the existing JSON parser handles it without
    // a special-case path. Kept as a separate alias here for clarity at call
    // sites in queue_renderer.cpp.
    const std::string& imagePath() const { return stringValue; }

    // list: child rows, each one becoming a ViewModelInstance bound into
    // the parent VM's list property in vector order.
    std::vector<ListItemConfig> listValue;
};

struct ListItemConfig
{
    std::string viewModel;
    std::string instance;
    std::map<std::string, ViewModelPropertyValue> properties;
};

struct ViewModelDataConfig
{
    std::string viewModel;
    std::string instance;
    std::map<std::string, ViewModelPropertyValue> properties;
};

struct AssetConfig
{
    std::map<std::string, std::string> images; // assetName -> filePath
    std::map<std::string, std::string> fonts;
};

struct Config
{
    std::string rivFile;
    std::string artboard;
    std::string stateMachine;
    int width = 800;
    int height = 600;

    ScreenshotConfig screenshot;
    OutputConfig output;
    ViewModelDataConfig viewModelData;
    AssetConfig assets;

    // State machine input overrides
    std::map<std::string, float> stateMachineNumberInputs;
    std::map<std::string, bool> stateMachineBoolInputs;

    // ffmpeg path for video encoding
    std::string ffmpegPath = "ffmpeg";

    // Linux only: route rendering through bundled SwiftShader ICD
    // (software Vulkan). Ignored on macOS, which always uses Metal.
    bool swiftshader = false;

    bool hasScreenshot() const { return !screenshot.path.empty(); }
    bool hasOutput() const { return !output.path.empty(); }

    static Config parse(const std::string& json);
};
