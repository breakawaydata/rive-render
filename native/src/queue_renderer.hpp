#pragma once

#include <string>
#include <vector>

#include "config.hpp"

// Render using the CommandQueue/CommandServer pattern.
// This mode supports:
// - Asset overrides + CDN assets resolved at import (internal FileAssetLoader)
// - View model data binding (incl. lists, images, fonts, triggers)
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

// Fill in config.width / config.height from the artboard's own size when
// either is missing (<= 0). With one dimension given, the other follows the
// artboard's aspect ratio. No-op when both are set.
void resolveCanvasSize(Config& config, const std::vector<uint8_t>& rivBytes);

QueueRenderResult renderWithQueue(const Config& config, const std::vector<uint8_t>& rivBytes);
