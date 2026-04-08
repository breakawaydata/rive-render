#ifdef RIVE_VULKAN

#include "headless_renderer.hpp"

#include <stdexcept>
#include <vulkan/vulkan.h>

using namespace rive;
using namespace rive::gpu;

HeadlessRenderer::HeadlessRenderer(int width, int height)
    : m_width(width), m_height(height)
{
    using namespace rive_vkb;

    // 1. Create Vulkan instance (headless — no surface extensions needed)
    m_instance = VulkanInstance::Create(VulkanInstance::Options{
        .appName = "rive-render",
        .idealAPIVersion = VK_API_VERSION_1_3,
    });
    if (!m_instance)
    {
        throw std::runtime_error("Failed to create Vulkan instance");
    }

    // 2. Create device in headless mode
    m_device = VulkanDevice::Create(
        *m_instance,
        VulkanDevice::Options{
            .headless = true,
        });
    if (!m_device)
    {
        throw std::runtime_error(
            "Failed to create Vulkan device. Is a Vulkan driver "
            "(or SwiftShader/MoltenVK) available?");
    }

    // 3. Create PLS render context
    auto vkImpl = RenderContextVulkanImpl::MakeContext(
        m_instance->vkInstance(),
        m_device->vkPhysicalDevice(),
        m_device->vkDevice(),
        m_device->vulkanFeatures(),
        m_instance->getVkGetInstanceProcAddrPtr(),
        {});
    if (!vkImpl)
    {
        throw std::runtime_error("Failed to create Vulkan render context");
    }

    // Get the VulkanContext before moving
    auto* vkCtx =
        vkImpl->static_impl_cast<RenderContextVulkanImpl>()->vulkanContext();

    // 4. Create headless frame synchronizer (manages offscreen image + readback)
    m_frameSynchronizer = VulkanHeadlessFrameSynchronizer::Create(
        *m_instance,
        *m_device,
        ref_rcp(vkCtx),
        VulkanHeadlessFrameSynchronizer::Options{
            .width = static_cast<uint32_t>(width),
            .height = static_cast<uint32_t>(height),
            .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
            .imageUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
        });
    if (!m_frameSynchronizer)
    {
        throw std::runtime_error(
            "Failed to create headless frame synchronizer");
    }

    // 5. Create render target
    m_renderTarget =
        vkImpl->static_impl_cast<RenderContextVulkanImpl>()->makeRenderTarget(
            width,
            height,
            m_frameSynchronizer->imageFormat(),
            m_frameSynchronizer->imageUsageFlags());
    if (!m_renderTarget)
    {
        throw std::runtime_error("Failed to create render target");
    }

    m_renderContext = std::move(vkImpl);
}

HeadlessRenderer::~HeadlessRenderer() = default;

std::vector<uint8_t> HeadlessRenderer::renderFrame(
    ArtboardInstance* artboard,
    StateMachineInstance* stateMachine)
{
    // Begin frame on the synchronizer (manages command buffers)
    VkResult result = m_frameSynchronizer->beginFrame();
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to begin frame");
    }

    // Set up the render target to point to the synchronizer's image
    m_renderTarget->setTargetImageView(m_frameSynchronizer->vkImageView(),
                                       m_frameSynchronizer->vkImage(),
                                       m_frameSynchronizer->lastAccess());

    // Begin the render context frame
    m_renderContext->beginFrame({
        .renderTargetWidth = static_cast<uint32_t>(m_width),
        .renderTargetHeight = static_cast<uint32_t>(m_height),
        .loadAction = gpu::LoadAction::clear,
        .clearColor = 0x00000000, // transparent black
    });

    // Create renderer and draw artboard
    RiveRenderer renderer(m_renderContext.get());
    renderer.save();
    renderer.align(Fit::contain,
                   Alignment::center,
                   AABB(0, 0, m_width, m_height),
                   artboard->bounds());
    artboard->draw(&renderer);
    renderer.restore();

    // Flush rendering to GPU
    m_renderContext->flush({
        .renderTarget = m_renderTarget.get(),
        .externalCommandBuffer = m_frameSynchronizer->currentCommandBuffer(),
        .currentFrameNumber = m_frameSynchronizer->currentFrameNumber(),
        .safeFrameNumber = m_frameSynchronizer->safeFrameNumber(),
    });

    // Queue pixel readback (GPU → CPU)
    auto lastAccess = m_renderTarget->targetLastAccess();
    m_frameSynchronizer->queueImageCopy(
        &lastAccess,
        IAABB::MakeWH(m_width, m_height));

    // End frame (submits command buffer)
    result = m_frameSynchronizer->endFrame(lastAccess);
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to end frame");
    }

    // Read pixels back
    std::vector<uint8_t> pixels;
    result = m_frameSynchronizer->getPixelsFromLastImageCopy(&pixels);
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to read pixels from GPU");
    }

    // Flip vertically — Vulkan/MoltenVK readback produces a Y-flipped image.
    size_t rowBytes = static_cast<size_t>(m_width) * 4;
    for (int y = 0; y < m_height / 2; y++)
    {
        int opposite = m_height - 1 - y;
        // Swap rows in-place
        for (size_t x = 0; x < rowBytes; x++)
        {
            std::swap(pixels[y * rowBytes + x],
                      pixels[opposite * rowBytes + x]);
        }
    }

    return pixels;
}

#endif // RIVE_VULKAN
