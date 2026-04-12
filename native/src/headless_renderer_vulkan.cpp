#include "headless_renderer.hpp"

#include <stdexcept>
#include <vulkan/vulkan.h>

#include "rive/renderer/rive_renderer.hpp"
#include "rive/renderer/vulkan/render_context_vulkan_impl.hpp"
#include "rive/renderer/vulkan/render_target_vulkan.hpp"
#include "rive_vk_bootstrap/vulkan_device.hpp"
#include "rive_vk_bootstrap/vulkan_headless_frame_synchronizer.hpp"
#include "rive_vk_bootstrap/vulkan_instance.hpp"

using namespace rive;
using namespace rive::gpu;

struct HeadlessRenderer::Impl
{
    std::unique_ptr<rive_vkb::VulkanInstance> instance;
    std::unique_ptr<rive_vkb::VulkanDevice> device;
    std::unique_ptr<rive::gpu::RenderContext> renderContext;
    std::unique_ptr<rive_vkb::VulkanHeadlessFrameSynchronizer> frameSynchronizer;
    rive::rcp<rive::gpu::RenderTargetVulkanImpl> renderTarget;
};

HeadlessRenderer::HeadlessRenderer(int width, int height, bool /*useSwiftShader*/)
    : m_impl(std::make_unique<Impl>()), m_width(width), m_height(height)
{
    using namespace rive_vkb;

    m_impl->instance = VulkanInstance::Create(VulkanInstance::Options{
        .appName = "rive-render",
        .idealAPIVersion = VK_API_VERSION_1_3,
    });
    if (!m_impl->instance)
    {
        throw std::runtime_error("Failed to create Vulkan instance");
    }

    m_impl->device = VulkanDevice::Create(*m_impl->instance, VulkanDevice::Options{
                                                                 .headless = true,
                                                             });
    if (!m_impl->device)
    {
        throw std::runtime_error("Failed to create Vulkan device. Is a Vulkan driver "
                                 "(or SwiftShader) available?");
    }

    auto vkImpl = RenderContextVulkanImpl::MakeContext(
        m_impl->instance->vkInstance(), m_impl->device->vkPhysicalDevice(),
        m_impl->device->vkDevice(), m_impl->device->vulkanFeatures(),
        m_impl->instance->getVkGetInstanceProcAddrPtr(), {});
    if (!vkImpl)
    {
        throw std::runtime_error("Failed to create Vulkan render context");
    }

    auto* vkCtx = vkImpl->static_impl_cast<RenderContextVulkanImpl>()->vulkanContext();

    m_impl->frameSynchronizer = VulkanHeadlessFrameSynchronizer::Create(
        *m_impl->instance, *m_impl->device, ref_rcp(vkCtx),
        VulkanHeadlessFrameSynchronizer::Options{
            .width = static_cast<uint32_t>(width),
            .height = static_cast<uint32_t>(height),
            .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
            .imageUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
        });
    if (!m_impl->frameSynchronizer)
    {
        throw std::runtime_error("Failed to create headless frame synchronizer");
    }

    m_impl->renderTarget = vkImpl->static_impl_cast<RenderContextVulkanImpl>()->makeRenderTarget(
        width, height, m_impl->frameSynchronizer->imageFormat(),
        m_impl->frameSynchronizer->imageUsageFlags());
    if (!m_impl->renderTarget)
    {
        throw std::runtime_error("Failed to create render target");
    }

    m_impl->renderContext = std::move(vkImpl);
}

HeadlessRenderer::~HeadlessRenderer() = default;

rive::Factory* HeadlessRenderer::factory()
{
    return m_impl->renderContext.get();
}

rive::gpu::RenderContext* HeadlessRenderer::renderContext()
{
    return m_impl->renderContext.get();
}

std::vector<uint8_t> HeadlessRenderer::renderFrame(ArtboardInstance* artboard,
                                                   StateMachineInstance* /*stateMachine*/)
{
    VkResult result = m_impl->frameSynchronizer->beginFrame();
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to begin frame");
    }

    m_impl->renderTarget->setTargetImageView(m_impl->frameSynchronizer->vkImageView(),
                                             m_impl->frameSynchronizer->vkImage(),
                                             m_impl->frameSynchronizer->lastAccess());

    m_impl->renderContext->beginFrame({
        .renderTargetWidth = static_cast<uint32_t>(m_width),
        .renderTargetHeight = static_cast<uint32_t>(m_height),
        .loadAction = gpu::LoadAction::clear,
        .clearColor = 0x00000000,
    });

    RiveRenderer renderer(m_impl->renderContext.get());
    renderer.save();
    renderer.align(Fit::contain, Alignment::center, AABB(0, 0, m_width, m_height),
                   artboard->bounds());
    artboard->draw(&renderer);
    renderer.restore();

    m_impl->renderContext->flush({
        .renderTarget = m_impl->renderTarget.get(),
        .externalCommandBuffer = m_impl->frameSynchronizer->currentCommandBuffer(),
        .currentFrameNumber = m_impl->frameSynchronizer->currentFrameNumber(),
        .safeFrameNumber = m_impl->frameSynchronizer->safeFrameNumber(),
    });

    auto lastAccess = m_impl->renderTarget->targetLastAccess();
    m_impl->frameSynchronizer->queueImageCopy(&lastAccess, IAABB::MakeWH(m_width, m_height));

    result = m_impl->frameSynchronizer->endFrame(lastAccess);
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to end frame");
    }

    std::vector<uint8_t> pixels;
    result = m_impl->frameSynchronizer->getPixelsFromLastImageCopy(&pixels);
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to read pixels from GPU");
    }

    // Vulkan/SwiftShader readback is Y-flipped — swap rows in place.
    size_t rowBytes = static_cast<size_t>(m_width) * 4;
    for (int y = 0; y < m_height / 2; y++)
    {
        int opposite = m_height - 1 - y;
        for (size_t x = 0; x < rowBytes; x++)
        {
            std::swap(pixels[y * rowBytes + x], pixels[opposite * rowBytes + x]);
        }
    }

    return pixels;
}
