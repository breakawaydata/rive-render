#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "rive/animation/state_machine_instance.hpp"
#include "rive/artboard.hpp"
#include "rive/factory.hpp"
#include "rive/renderer/render_context.hpp"

class HeadlessRenderer
{
  public:
    HeadlessRenderer(int width, int height, bool useSwiftShader = false);
    ~HeadlessRenderer();

    HeadlessRenderer(const HeadlessRenderer&) = delete;
    HeadlessRenderer& operator=(const HeadlessRenderer&) = delete;

    // Render a single frame and return raw RGBA pixel data (w*h*4 bytes).
    std::vector<uint8_t> renderFrame(rive::ArtboardInstance* artboard,
                                     rive::StateMachineInstance* stateMachine);

    // Get the factory for File::import.
    rive::Factory* factory();

    // Get the RenderContext (also usable as Factory for CommandServer).
    rive::gpu::RenderContext* renderContext();

    int width() const { return m_width; }
    int height() const { return m_height; }

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    int m_width;
    int m_height;
};
