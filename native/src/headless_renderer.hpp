#pragma once

#ifdef RIVE_VULKAN

#include <cstdint>
#include <memory>
#include <vector>

#include "rive/factory.hpp"
#include "rive/artboard.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/renderer/vulkan/render_context_vulkan_impl.hpp"
#include "rive/renderer/vulkan/render_target_vulkan.hpp"
#include "rive_vk_bootstrap/vulkan_instance.hpp"
#include "rive_vk_bootstrap/vulkan_device.hpp"
#include "rive_vk_bootstrap/vulkan_headless_frame_synchronizer.hpp"

class HeadlessRenderer
{
public:
    HeadlessRenderer(int width, int height);
    ~HeadlessRenderer();

    // Render a single frame and return raw RGBA pixel data (w*h*4 bytes)
    std::vector<uint8_t> renderFrame(rive::ArtboardInstance* artboard,
                                     rive::StateMachineInstance* stateMachine);

    // Get the factory for File::import
    rive::Factory* factory() { return m_renderContext.get(); }

    // Get the RenderContext (also usable as Factory for CommandServer)
    rive::gpu::RenderContext* renderContext() { return m_renderContext.get(); }

    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    int m_width;
    int m_height;

    std::unique_ptr<rive_vkb::VulkanInstance> m_instance;
    std::unique_ptr<rive_vkb::VulkanDevice> m_device;
    std::unique_ptr<rive::gpu::RenderContext> m_renderContext;
    std::unique_ptr<rive_vkb::VulkanHeadlessFrameSynchronizer>
        m_frameSynchronizer;
    rive::rcp<rive::gpu::RenderTargetVulkanImpl> m_renderTarget;
};

#endif // RIVE_VULKAN
