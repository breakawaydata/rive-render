-- rive-cli native build
-- Uses rive-runtime's premake5 build system

RIVE_RUNTIME_DIR = path.getabsolute('../deps/rive-runtime')

-- Include rive-runtime build configs
dofile(RIVE_RUNTIME_DIR .. '/renderer/premake5_pls_renderer.lua')
dofile(RIVE_RUNTIME_DIR .. '/premake5_v2.lua')
dofile(RIVE_RUNTIME_DIR .. '/decoders/premake5_v2.lua')

-- Our CLI binary
project('rive_render')
do
    kind('ConsoleApp')
    exceptionhandling('On')
    rtti('On')

    includedirs({
        RIVE_RUNTIME_DIR .. '/include',
        RIVE_RUNTIME_DIR .. '/renderer/include',
        RIVE_RUNTIME_DIR .. '/renderer/src',
        'src',
    })

    files({
        'src/**.hpp',
        'src/**.h',
        'src/config.cpp',
        'src/main.cpp',
        'src/output_gif.cpp',
        'src/output_png.cpp',
        'src/output_video.cpp',
        'src/queue_renderer.cpp',
    })

    links({
        'rive',
        'rive_pls_renderer',
        'rive_decoders',
        'libwebp',
        'rive_harfbuzz',
        'rive_sheenbidi',
        'rive_yoga',
    })

    filter({ 'options:not no_rive_png' })
    do
        links({ 'zlib', 'libpng' })
    end
    filter({ 'options:not no_rive_jpeg' })
    do
        links({ 'libjpeg' })
    end
    filter({})

    defines({ 'YOGA_EXPORT=' })

    filter('system:macosx')
    do
        -- macOS renders through the native Metal backend in rive_pls_renderer.
        files({ 'src/headless_renderer_metal.mm' })
        buildoptions({ '-fobjc-arc' })
        links({
            'Cocoa.framework',
            'Metal.framework',
            'MetalKit.framework',
            'QuartzCore.framework',
            'Foundation.framework',
            'IOKit.framework',
            'CoreFoundation.framework',
        })
    end

    filter('system:linux')
    do
        -- Linux uses Vulkan (real driver by default, SwiftShader when
        -- the --swiftshader runtime flag is set on the Config).
        files({ 'src/headless_renderer_vulkan.cpp' })

        -- GNU ld is strict about link order; linkgroups wraps libs in
        -- --start-group / --end-group to resolve circular deps.
        linkgroups('On')
        links({ 'pthread', 'dl', 'z' })
    end

    filter({})

    -- Vulkan bootstrap sources + headers. vulkan_headers /
    -- vulkan_memory_allocator globals are populated by rive-runtime's
    -- premake when --with_vulkan is passed; only reference them inside
    -- this Lua guard so macOS builds (which omit --with_vulkan) don't
    -- crash evaluating the script.
    if _OPTIONS['with_vulkan'] then
        local vk_bootstrap_dir = RIVE_RUNTIME_DIR .. '/renderer/rive_vk_bootstrap'
        filter('system:linux')
        do
            includedirs({ vk_bootstrap_dir .. '/include' })
            externalincludedirs({
                vulkan_headers .. '/include',
                vulkan_memory_allocator .. '/include',
            })
            files({
                vk_bootstrap_dir .. '/include/**.hpp',
                vk_bootstrap_dir .. '/src/*.cpp',
                vk_bootstrap_dir .. '/src/*.hpp',
            })
        end
        filter({})
    end

    filter({ 'toolset:not msc' })
    do
        buildoptions({ '-Wno-shorten-64-to-32' })
    end

    filter({})
end
