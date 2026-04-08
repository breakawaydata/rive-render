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
        'src/**.cpp',
        'src/**.hpp',
        'src/**.h',
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

    -- Vulkan bootstrap (adds includes, files for vk bootstrap)
    if _OPTIONS['with_vulkan'] then
        local vk_bootstrap_dir = RIVE_RUNTIME_DIR .. '/renderer/rive_vk_bootstrap'
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

    filter('system:macosx')
    do
        files({ 'src/**.mm' })
        buildoptions({ '-fobjc-arc' })
        links({
            'Cocoa.framework',
            'Metal.framework',
            'QuartzCore.framework',
            'IOKit.framework',
            'CoreFoundation.framework',
        })
    end

    filter('system:linux')
    do
        links({ 'pthread', 'dl' })
    end

    filter({ 'toolset:not msc' })
    do
        buildoptions({ '-Wno-shorten-64-to-32' })
    end

    filter({})
end
