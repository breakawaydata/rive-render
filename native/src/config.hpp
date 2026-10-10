#pragma once

#include <cstdint>
#include <map>
#include <memory>
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

    // mp4 only (ignored for gif, webm and png). Validated by Config::parse.
    std::string encoder = "auto";    // "auto", "nvenc" or "x264"
    std::string preset = "veryfast"; // x264 preset
    std::string nvencPreset = "p4";  // NVENC preset, "p1".."p7"
    // Single-threaded x264 (and libvpx for webm) for byte-reproducible output.
    bool deterministic = false;
};

// One entry inside a `{ "type": "list" }` PropertyValue.
struct ListItemConfig;

// The payload of a `{ "type": "artboard" }` PropertyValue: an artboard taken
// from an extra .riv file, plus the view model data for that artboard.
struct ArtboardBindingConfig;

struct ViewModelPropertyValue
{
    // "string", "number", "boolean", "color", "enum", "image", "font",
    // "trigger", "list", "artboard"
    std::string type;
    std::string stringValue;
    float numberValue = 0.0f;
    bool boolValue = false;
    uint32_t colorValue = 0;

    // list: child rows, each one becoming a ViewModelInstance bound into
    // the parent VM's list property in vector order.
    std::vector<ListItemConfig> listValue;

    // artboard: which artboard of which extra file to bind, and the nested
    // view model data. Held by pointer because the type is incomplete here
    // (it contains a map of ViewModelPropertyValue); null for every other
    // property type.
    std::shared_ptr<ArtboardBindingConfig> artboardValue;
};

struct ListItemConfig
{
    std::string viewModel;
    std::string instance;
    std::map<std::string, ViewModelPropertyValue> properties;
};

struct ArtboardBindingConfig
{
    std::string file;     // key into Config::extraFiles
    std::string artboard; // artboard name inside that file
    std::string viewModel;
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

// A second .riv file whose artboards can be bound into artboard-typed view
// model properties of the main file.
struct ExtraFileConfig
{
    std::string rivFile;
    AssetConfig assets;
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
    // alias -> extra file; aliases are what artboard properties refer to.
    std::map<std::string, ExtraFileConfig> extraFiles;

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
