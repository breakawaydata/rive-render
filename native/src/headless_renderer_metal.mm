#include "headless_renderer.hpp"

#include <stdexcept>

#import <Metal/Metal.h>

#include "rive/renderer/metal/render_context_metal_impl.h"
#include "rive/renderer/rive_renderer.hpp"

using namespace rive;
using namespace rive::gpu;

struct HeadlessRenderer::Impl
{
    id<MTLDevice> gpu = nil;
    id<MTLCommandQueue> queue = nil;
    std::unique_ptr<rive::gpu::RenderContext> renderContext;
    rive::rcp<rive::gpu::RenderTargetMetal> renderTarget;
    id<MTLTexture> targetTexture = nil;
    id<MTLBuffer> readbackBuffer = nil;
};

HeadlessRenderer::HeadlessRenderer(int width, int height, bool useSwiftShader)
    : m_impl(std::make_unique<Impl>()), m_width(width), m_height(height)
{
    // useSwiftShader is a Linux/Vulkan-only knob — on macOS we always
    // render through the native Metal backend and there is no
    // software-rasterizer option.
    (void)useSwiftShader;

    @autoreleasepool
    {
        m_impl->gpu = MTLCreateSystemDefaultDevice();
        if (m_impl->gpu == nil)
        {
            throw std::runtime_error("Failed to create Metal device. No GPU available?");
        }
        m_impl->queue = [m_impl->gpu newCommandQueue];
        if (m_impl->queue == nil)
        {
            throw std::runtime_error("Failed to create Metal command queue");
        }

        m_impl->renderContext = RenderContextMetalImpl::MakeContext(m_impl->gpu, {});
        if (!m_impl->renderContext)
        {
            throw std::runtime_error("Failed to create Metal render context");
        }

        auto* metalImpl = m_impl->renderContext->static_impl_cast<RenderContextMetalImpl>();
        m_impl->renderTarget = metalImpl->makeRenderTarget(
            MTLPixelFormatBGRA8Unorm, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        if (!m_impl->renderTarget)
        {
            throw std::runtime_error("Failed to create Metal render target");
        }

        MTLTextureDescriptor* desc = [[MTLTextureDescriptor alloc] init];
        desc.pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.width = static_cast<NSUInteger>(width);
        desc.height = static_cast<NSUInteger>(height);
        desc.textureType = MTLTextureType2D;
        desc.mipmapLevelCount = 1;
        desc.usage = MTLTextureUsageRenderTarget;
        desc.storageMode = MTLStorageModePrivate;
        m_impl->targetTexture = [m_impl->gpu newTextureWithDescriptor:desc];
        if (m_impl->targetTexture == nil)
        {
            throw std::runtime_error("Failed to create Metal offscreen texture");
        }
        m_impl->renderTarget->setTargetTexture(m_impl->targetTexture);

        NSUInteger byteCount = static_cast<NSUInteger>(width) * static_cast<NSUInteger>(height) * 4;
        m_impl->readbackBuffer = [m_impl->gpu newBufferWithLength:byteCount
                                                          options:MTLResourceStorageModeShared];
        if (m_impl->readbackBuffer == nil)
        {
            throw std::runtime_error("Failed to create Metal readback buffer");
        }
    }
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
    std::vector<uint8_t> pixels;
    @autoreleasepool
    {
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

        id<MTLCommandBuffer> commandBuffer = [m_impl->queue commandBuffer];
        m_impl->renderContext->flush({
            .renderTarget = m_impl->renderTarget.get(),
            .externalCommandBuffer = (__bridge void*)commandBuffer,
        });

        id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
        [blitEncoder copyFromTexture:m_impl->targetTexture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(static_cast<NSUInteger>(m_width),
                                                 static_cast<NSUInteger>(m_height), 1)
                            toBuffer:m_impl->readbackBuffer
                   destinationOffset:0
              destinationBytesPerRow:static_cast<NSUInteger>(m_width) * 4
            destinationBytesPerImage:static_cast<NSUInteger>(m_width) *
                                     static_cast<NSUInteger>(m_height) * 4];
        [blitEncoder endEncoding];

        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];

        const size_t w = static_cast<size_t>(m_width);
        const size_t h = static_cast<size_t>(m_height);
        pixels.resize(w * h * 4);
        const uint8_t* contents = reinterpret_cast<const uint8_t*>(m_impl->readbackBuffer.contents);
        const size_t rowBytes = w * 4;
        // Flip Y and swap BGRA → RGBA.
        for (size_t y = 0; y < h; ++y)
        {
            const uint8_t* srcRow = &contents[(h - y - 1) * rowBytes];
            uint8_t* dstRow = &pixels[y * rowBytes];
            for (size_t x = 0; x < rowBytes; x += 4)
            {
                dstRow[x + 0] = srcRow[x + 2];
                dstRow[x + 1] = srcRow[x + 1];
                dstRow[x + 2] = srcRow[x + 0];
                dstRow[x + 3] = srcRow[x + 3];
            }
        }
    }
    return pixels;
}
