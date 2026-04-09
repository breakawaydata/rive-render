#pragma once

#include <string>
#include <vector>

#include "config.hpp"

// Render using the CommandQueue/CommandServer pattern.
// This mode supports:
// - Async asset loading via queue->decodeImage/decodeFont
// - View model data binding via queue->setViewModelInstance*
// - Frame-by-frame state machine advancement
// - Thread-safe rendering via draw callback on server thread
//
// Returns rendered RGBA frames.
struct QueueRenderResult
{
    std::vector<std::vector<uint8_t>> frames;
    int width;
    int height;
};

QueueRenderResult renderWithQueue(const Config& config, const std::vector<uint8_t>& rivBytes);
