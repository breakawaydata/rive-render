#pragma once

#include <cstdint>
#include <functional>
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
// Frames are handed to a sink as they are rendered (RGBA, width * height * 4 bytes), so the
// caller can stream them to an encoder and memory does not grow with the frame count. The sink
// runs on the calling thread; an exception from it aborts the render and propagates.
using FrameSink = std::function<void(std::vector<uint8_t>&&)>;

struct QueueRenderResult
{
    int frameCount;
    int width;
    int height;
};

// Fill in config.width / config.height from the artboard's own size when
// either is missing (<= 0). With one dimension given, the other follows the
// artboard's aspect ratio. No-op when both are set.
void resolveCanvasSize(Config& config, const std::vector<uint8_t>& rivBytes);

// A screenshot or png output renders and delivers exactly one frame.
QueueRenderResult renderWithQueue(const Config& config, const std::vector<uint8_t>& rivBytes,
                                  const FrameSink& sink);
