#pragma once

#include <cstdint>
#include <map>
#include <string>

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

struct ViewModelPropertyValue
{
    std::string type; // "string", "number", "boolean", "color", "enum"
    std::string stringValue;
    float numberValue = 0.0f;
    bool boolValue = false;
    uint32_t colorValue = 0;
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

    // ffmpeg path for video encoding
    std::string ffmpegPath = "ffmpeg";

    // Use CommandQueue/CommandServer rendering mode
    bool useCommandQueue = false;

    bool hasScreenshot() const { return !screenshot.path.empty(); }
    bool hasOutput() const { return !output.path.empty(); }

    static Config parse(const std::string& json);
};
