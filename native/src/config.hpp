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
    // "string", "number", "boolean", "color", "enum", "image", "font",
    // "trigger", "list"
    std::string type;
    std::string stringValue;
    float numberValue = 0.0f;
    bool boolValue = false;
    uint32_t colorValue = 0;

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
    // Canvas size in pixels. <= 0 means "use the artboard's size" (see
    // resolveCanvasSize); with one side given the other keeps the aspect.
    int width = 0;
    int height = 0;

    ScreenshotConfig screenshot;
    OutputConfig output;
    ViewModelDataConfig viewModelData;
    AssetConfig assets;

    // State machine input overrides. A `true` bool targeting a trigger input
    // fires it.
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
