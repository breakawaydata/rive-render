#include "queue_renderer.hpp"
#include "headless_renderer.hpp"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "rive/animation/linear_animation_instance.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/artboard.hpp"
#include "rive/assets/font_asset.hpp"
#include "rive/assets/image_asset.hpp"
#include "rive/command_queue.hpp"
#include "rive/command_server.hpp"
#include "rive/file.hpp"
#include "rive/simple_array.hpp"

using namespace rive;

static std::vector<uint8_t> readAssetFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        throw std::runtime_error("Failed to open asset: " + path);
    auto size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(size);
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

// File listener to know when loading completes
class QueueFileListener : public CommandQueue::FileListener
{
  public:
    std::atomic<bool> loaded{false};
    std::atomic<bool> errored{false};
    std::string errorMsg;

    void onFileLoaded(const FileHandle, uint64_t) override { loaded.store(true); }

    void onFileError(const FileHandle, uint64_t, std::string error) override
    {
        errorMsg = std::move(error);
        errored.store(true);
    }
};

// Wait for a condition, pumping messages on the queue
template <typename Pred>
static void waitFor(rcp<CommandQueue>& queue, Pred pred, const char* what, int timeoutMs = 10000)
{
    auto start = std::chrono::steady_clock::now();
    while (!pred())
    {
        queue->processMessages();
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeoutMs)
        {
            throw std::runtime_error(std::string("Timeout waiting for: ") + what);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    queue->processMessages();
}

QueueRenderResult renderWithQueue(const Config& config, const std::vector<uint8_t>& rivBytes)
{
    // 1. Create headless renderer
    HeadlessRenderer headless(config.width, config.height, config.swiftshader);

    // 2. Create queue + server
    auto queue = make_rcp<CommandQueue>();
    CommandServer server(queue, headless.renderContext());

    // 3. Start server on background thread
    std::thread serverThread([&server]() { server.serveUntilDisconnect(); });

    // CommandFileAssetLoader matches by FileAsset::uniqueName(), which is the
    // base name plus asset id with the extension stripped (see
    // file_asset.cpp::uniqueName). Users may supply either:
    //   (a) the full uniqueFilename ("flower-45020.png") → strip extension
    //   (b) just the base asset name ("staticImgBG") → needs "-<id>" appended
    // We register under the stripped key first (handles case a). After the
    // file loads, a runOnce fallback matches any unresolved assets by base
    // name (handles case b).
    auto toUniqueName = [](const std::string& key)
    {
        auto dot = key.rfind('.');
        return dot == std::string::npos ? key : key.substr(0, dot);
    };

    try
    {
        // 4. Decode and register referenced assets BEFORE loading the file.
        //    Commands are processed in order on the server thread, so the
        //    CommandFileAssetLoader sees the global registrations when the
        //    subsequent loadFile runs — no explicit wait required.
        //    Also keep decoded handles so we can set them as view model
        //    image properties later (images may be VM-bound, not just
        //    file-referenced assets).
        // Decode and register referenced assets BEFORE loading the file.
        // Also keep decoded handles for VM image property assignment.
        std::map<std::string, RenderImageHandle> decodedImages;
        for (auto& [name, path] : config.assets.images)
        {
            auto handle = queue->decodeImage(readAssetFile(path));
            queue->addGlobalImageAsset(toUniqueName(name), handle);
            decodedImages[name] = handle;
        }
        for (auto& [name, path] : config.assets.fonts)
        {
            auto handle = queue->decodeFont(readAssetFile(path));
            queue->addGlobalFontAsset(toUniqueName(name), handle);
        }

        // 5. Load file
        QueueFileListener fileListener;
        auto fileHandle =
            queue->loadFile(std::vector<uint8_t>(rivBytes.begin(), rivBytes.end()), &fileListener);
        waitFor(
            queue, [&]() { return fileListener.loaded.load() || fileListener.errored.load(); },
            "file load");
        if (fileListener.errored.load())
        {
            throw std::runtime_error("Failed to load .riv: " + fileListener.errorMsg);
        }

        // 5b. Fallback asset matching by base name.
        // The addGlobalImageAsset / addGlobalFontAsset calls above use the
        // key as-is (possibly stripped of file extension). If the user
        // supplied just the asset's base name (e.g. "staticImgBG") rather
        // than the full uniqueFilename ("staticImgBG-4117592.png"), the
        // CommandFileAssetLoader won't find a match because it looks up by
        // uniqueName() = name + "-" + assetId. Fix this by iterating the
        // loaded file's assets on the server thread and directly assigning
        // decoded images/fonts to any asset whose base name matches a
        // config key that was not already resolved.
        if (!decodedImages.empty())
        {
            // Build lookup: baseName -> decoded handle (images)
            std::map<std::string, RenderImageHandle> imageByBaseName;
            for (auto& [name, handle] : decodedImages)
            {
                imageByBaseName[toUniqueName(name)] = handle;
            }
            // Build lookup: baseName -> decoded handle (fonts)
            std::map<std::string, FontHandle> fontByBaseName;
            for (auto& [name, path] : config.assets.fonts)
            {
                // We already decoded fonts above; reconstruct the handle.
                // Unfortunately we didn't save FontHandles — re-decode is
                // wasteful. Instead, for fonts we registered them as global
                // assets which should match if the user supplied the full
                // uniqueFilename. For the name-only case we'd need the
                // handle, so let's skip fonts for now (the image case is
                // the critical fix).
                (void)name;
                (void)path;
            }

            queue->runOnce(
                [fileHandle, imageByBaseName](CommandServer* srv)
                {
                    auto* file = srv->getFile(fileHandle);
                    if (!file)
                        return;
                    auto assets = file->assets();
                    for (auto& assetRef : assets)
                    {
                        auto* asset = assetRef.get();
                        if (!asset || !asset->is<ImageAsset>())
                            continue;
                        auto* imageAsset = asset->as<ImageAsset>();
                        if (imageAsset->renderImage() != nullptr)
                            continue;
                        auto itr = imageByBaseName.find(asset->name());
                        if (itr != imageByBaseName.end())
                        {
                            auto* renderImage = srv->getImage(itr->second);
                            if (renderImage)
                                imageAsset->renderImage(ref_rcp(renderImage));
                        }
                    }
                });
        }

        // 6. Instantiate artboard.
        // Intentionally do NOT call setArtboardSize — that resizes the
        // artboard's own bounds to the canvas, which distorts Yoga-layout
        // positioning (basketball.riv shows this clearly: the ball drifts
        // off-center and the floor shadow disappears). HeadlessRenderer
        // already uses Fit::contain + Alignment::center against the
        // artboard's natural bounds, so the scaling happens at draw time.
        auto abHandle = config.artboard.empty()
                            ? queue->instantiateDefaultArtboard(fileHandle)
                            : queue->instantiateArtboardNamed(fileHandle, config.artboard);

        // 7. Instantiate state machine
        auto smHandle = config.stateMachine.empty()
                            ? queue->instantiateDefaultStateMachine(abHandle)
                            : queue->instantiateStateMachineNamed(abHandle, config.stateMachine);

        // 8. Bind view model data (if provided)
        if (!config.viewModelData.properties.empty() || !decodedImages.empty())
        {
            // Instantiate a view model instance.
            //
            // When viewModel is specified, look up the VM by name.
            // Otherwise infer from the artboard. Use the named-instance
            // overload when an instance name is provided, otherwise
            // create a default instance.
            auto vmHandle = !config.viewModelData.viewModel.empty()
                                ? (!config.viewModelData.instance.empty()
                                       ? queue->instantiateViewModelInstanceNamed(
                                             fileHandle, config.viewModelData.viewModel,
                                             config.viewModelData.instance)
                                       : queue->instantiateDefaultViewModelInstance(
                                             fileHandle, config.viewModelData.viewModel))
                            : config.viewModelData.instance.empty()
                                ? queue->instantiateDefaultViewModelInstance(fileHandle, abHandle)
                                : queue->instantiateViewModelInstanceNamed(
                                      fileHandle, abHandle, config.viewModelData.instance);

            // Set properties via queue commands
            for (auto& [path, prop] : config.viewModelData.properties)
            {
                if (prop.type == "string")
                    queue->setViewModelInstanceString(vmHandle, path, prop.stringValue);
                else if (prop.type == "number")
                    queue->setViewModelInstanceNumber(vmHandle, path, prop.numberValue);
                else if (prop.type == "boolean")
                    queue->setViewModelInstanceBool(vmHandle, path, prop.boolValue);
                else if (prop.type == "color")
                    queue->setViewModelInstanceColor(vmHandle, path, prop.colorValue);
                else if (prop.type == "enum")
                    queue->setViewModelInstanceEnum(vmHandle, path, prop.stringValue);
            }

            // Set view model image properties from decoded assets.
            for (auto& [name, imgHandle] : decodedImages)
            {
                queue->setViewModelInstanceImage(vmHandle, name, imgHandle);
            }

            // Bind the VM instance to the state machine (also sets
            // the artboard data context internally).
            queue->bindViewModelInstance(smHandle, vmHandle);

            // Direct property application on the server thread.
            // The queue-based set* commands above work when the artboard's
            // own VM has matching properties. But some artboards (e.g.
            // cropped wrappers around a "Sport Card" nested artboard) have
            // a different VM schema — the artboard VM might lack properties
            // like firstName/lastName that exist on the file-level VM.
            // To handle both cases, always create an instance from the
            // file's first VM, set all properties on it directly, and
            // bind it. This ensures all properties reach the artboard
            // regardless of which VM schema it was originally linked to.
            queue->runOnce(
                [fileHandle, abHandle, smHandle, &config](CommandServer* srv)
                {
                    auto* file = srv->getFile(fileHandle);
                    if (!file || file->viewModelCount() == 0)
                        return;

                    auto* rawVM = file->viewModel(0);
                    auto* viewModelRuntime = rawVM ? file->viewModelByName(rawVM->name()) : nullptr;
                    if (!viewModelRuntime)
                        return;

                    auto inst = viewModelRuntime->createInstance();
                    if (!inst)
                        return;

                    for (auto& [path, prop] : config.viewModelData.properties)
                    {
                        if (prop.type == "string")
                        {
                            if (auto* p = inst->propertyString(path))
                                p->value(prop.stringValue);
                        }
                        else if (prop.type == "number")
                        {
                            if (auto* p = inst->propertyNumber(path))
                                p->value(prop.numberValue);
                        }
                        else if (prop.type == "boolean")
                        {
                            if (auto* p = inst->propertyBoolean(path))
                                p->value(prop.boolValue);
                        }
                        else if (prop.type == "color")
                        {
                            if (auto* p = inst->propertyColor(path))
                                p->value(static_cast<int>(prop.colorValue));
                        }
                        else if (prop.type == "enum")
                        {
                            if (auto* p = inst->propertyEnum(path))
                                p->value(prop.stringValue);
                        }
                    }

                    auto* sm = srv->getStateMachineInstance(smHandle);
                    if (sm)
                        sm->bindViewModelInstance(inst->instance());
                    auto* artboard = srv->getArtboardInstance(abHandle);
                    if (artboard)
                        artboard->bindViewModelInstance(inst->instance());

                    // Force multiple advance cycles so data binds
                    // propagate through nested artboards. The first
                    // advance instantiates nested artboard components
                    // and relays the data context. The second advance
                    // processes the dirty data binds inside the nested
                    // artboard (e.g. text runs reading firstName/
                    // lastName from the VM). Without both, nested text
                    // renders empty.
                    for (int i = 0; i < 2; i++)
                    {
                        if (sm)
                            sm->advanceAndApply(0.0f);
                        else if (artboard)
                            artboard->advance(0.0f);
                    }
                });
        }

        // 8b. Apply stateMachineInputs before any advances.
        // Use a draw callback to access the StateMachineInstance on the server thread,
        // then wait for it to complete before proceeding to warmup/rendering.
        if (!config.stateMachineNumberInputs.empty() || !config.stateMachineBoolInputs.empty())
        {
            std::mutex inputMtx;
            std::condition_variable inputCv;
            bool inputsDone = false;

            auto inputDrawKey = queue->createDrawKey();
            queue->draw(inputDrawKey,
                        CommandServerDrawCallback(
                            [&](DrawKey, CommandServer* srv)
                            {
                                auto* sm = srv->getStateMachineInstance(smHandle);
                                if (sm)
                                {
                                    for (auto& [name, value] : config.stateMachineNumberInputs)
                                    {
                                        auto* input = sm->getNumber(name);
                                        if (input)
                                            input->value(value);
                                    }
                                    for (auto& [name, value] : config.stateMachineBoolInputs)
                                    {
                                        auto* input = sm->getBool(name);
                                        if (input)
                                            input->value(value);
                                    }
                                }
                                std::lock_guard<std::mutex> lock(inputMtx);
                                inputsDone = true;
                                inputCv.notify_one();
                            }));

            // Wait for inputs to be applied before warmup
            std::unique_lock<std::mutex> lock(inputMtx);
            inputCv.wait(lock, [&] { return inputsDone; });
        }

        // 9. Determine frame parameters.
        // For screenshots, step at a fixed 60 Hz and advance up to
        // `screenshot.timestamp`, keeping only the final rendered frame.
        // For GIFs/videos, use the configured fps and duration.
        // Screenshot takes priority when both are present — that matches
        // main.cpp's post-render branch ordering, so the frame count and
        // the output selector can't disagree.
        const float fps = config.hasScreenshot() ? 60.0f : config.output.fps;
        const float dt = 1.0f / fps;
        const int totalFrames =
            config.hasScreenshot()
                ? std::max(1, static_cast<int>(config.screenshot.timestamp * fps))
                : std::max(1, static_cast<int>(fps * config.output.duration));

        // 10. Render frames via draw callbacks.
        // All time advancement happens on the server thread inside the draw
        // callback so that both state machines *and* linear animations (which
        // have no first-class CommandQueue command) can drive the scene.
        // A per-server-thread scene cache holds the lazily-created
        // LinearAnimationInstance so it is advanced by the same instance each
        // frame — recreating it would reset elapsed time.
        struct SceneCache
        {
            bool initialized = false;
            std::unique_ptr<LinearAnimationInstance> linearAnim;
        };
        auto scene = std::make_shared<SceneCache>();

        std::vector<std::vector<uint8_t>> frames;
        frames.reserve(config.hasOutput() ? totalFrames : 1);

        std::mutex frameMutex;
        std::condition_variable frameCv;
        bool frameReady = false;
        std::vector<uint8_t> currentFrame;

        auto drawKey = queue->createDrawKey();

        // Advances the scene on the server thread. Lazy-inits the linear
        // animation fallback the first time it runs. Shared by both advance-
        // only callbacks (screenshot warmup) and the full draw callback, so
        // they share the same SceneCache and produce identical scene state.
        auto advanceScene = [abHandle, smHandle, scene](CommandServer* srv, float frameDt)
        {
            auto* artboard = srv->getArtboardInstance(abHandle);
            if (!artboard)
                return static_cast<ArtboardInstance*>(nullptr);

            auto* sm = srv->getStateMachineInstance(smHandle);
            if (!scene->initialized)
            {
                scene->initialized = true;
                if (!sm && artboard->animationCount() > 0)
                    scene->linearAnim = artboard->animationAt(0);
            }

            if (sm)
                sm->advanceAndApply(frameDt);
            else if (scene->linearAnim)
                scene->linearAnim->advanceAndApply(frameDt);

            // Always advance the artboard in addition to any state machine /
            // linear animation advance. Matches the old direct path, and is
            // load-bearing for scenes that rely on artboard-level updates
            // (e.g. teststatemachine.riv's transitions won't settle without
            // it — frames end up frozen at a mid-transition pose).
            artboard->advance(frameDt);

            return artboard;
        };

        // Screenshots may request t=0 — we still render one frame but with
        // zero advance. Every other case advances by `dt` each step.
        const float frameDt =
            (config.hasScreenshot() && config.screenshot.timestamp == 0) ? 0.0f : dt;

        // Screenshot warmup: advance the scene via runOnce callbacks (CPU
        // only, no GPU render) for every frame except the final one. This
        // matches the old direct path's behaviour — a timestamp=10 screenshot
        // should do one Vulkan render pass, not 600.
        if (config.hasScreenshot())
        {
            for (int i = 0; i < totalFrames - 1; i++)
            {
                queue->runOnce([advanceScene, frameDt](CommandServer* srv)
                               { advanceScene(srv, frameDt); });
            }
        }

        // Post-warmup pass (only when VM data or assets are involved).
        if (!config.viewModelData.properties.empty() || !decodedImages.empty())
        {
            auto* factory = headless.factory();
            queue->runOnce(
                [fileHandle, abHandle, smHandle, &config, factory](CommandServer* srv)
                {
                    auto* file = srv->getFile(fileHandle);
                    if (!file)
                        return;

                    // Download CDN-hosted fonts
                    if (factory)
                    {
                        auto assets = file->assets();
                        for (auto& assetRef : assets)
                        {
                            auto* asset = assetRef.get();
                            if (!asset || !asset->is<FontAsset>())
                                continue;
                            auto* fontAsset = asset->as<FontAsset>();
                            if (fontAsset->font() != nullptr)
                                continue;
                            auto cdnBase = fontAsset->cdnBaseUrl();
                            auto cdnUuid = fontAsset->cdnUuidStr();
                            if (cdnBase.empty() || cdnUuid.empty())
                                continue;
                            std::string url = cdnBase;
                            if (!url.empty() && url.back() != '/')
                                url += '/';
                            url += cdnUuid;
                            std::string cmd = "curl -sL '" + url + "'";
                            FILE* pipe = popen(cmd.c_str(), "r");
                            if (!pipe)
                                continue;
                            std::vector<uint8_t> fontData;
                            uint8_t buf[8192];
                            size_t n;
                            while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0)
                                fontData.insert(fontData.end(), buf, buf + n);
                            pclose(pipe);
                            if (fontData.size() > 100)
                            {
                                SimpleArray<uint8_t> arr(fontData.data(), fontData.size());
                                fontAsset->decode(arr, factory);
                            }
                        }
                    }

                    if (file->viewModelCount() == 0)
                        return;

                    auto* rawVM = file->viewModel(0);
                    auto* viewModelRuntime = rawVM ? file->viewModelByName(rawVM->name()) : nullptr;
                    if (!viewModelRuntime)
                        return;

                    auto inst = viewModelRuntime->createInstance();
                    if (!inst)
                        return;

                    for (auto& [path, prop] : config.viewModelData.properties)
                    {
                        if (prop.type == "string")
                        {
                            auto* p = inst->propertyString(path);
                            if (p)
                                p->value(prop.stringValue);
                        }
                        else if (prop.type == "number")
                        {
                            if (auto* p = inst->propertyNumber(path))
                                p->value(prop.numberValue);
                        }
                        else if (prop.type == "boolean")
                        {
                            if (auto* p = inst->propertyBoolean(path))
                                p->value(prop.boolValue);
                        }
                        else if (prop.type == "color")
                        {
                            if (auto* p = inst->propertyColor(path))
                                p->value(static_cast<int>(prop.colorValue));
                        }
                        else if (prop.type == "enum")
                        {
                            if (auto* p = inst->propertyEnum(path))
                                p->value(prop.stringValue);
                        }
                    }

                    auto* sm = srv->getStateMachineInstance(smHandle);
                    if (sm)
                        sm->bindViewModelInstance(inst->instance());
                    auto* artboard = srv->getArtboardInstance(abHandle);
                    if (artboard)
                        artboard->bindViewModelInstance(inst->instance());

                    // Two advances: first propagates data context to nested
                    // artboards, second processes data binds inside them.
                    for (int i = 0; i < 2; i++)
                    {
                        if (sm)
                            sm->advanceAndApply(0.0f);
                        else if (artboard)
                            artboard->advance(0.0f);
                    }
                });
        }

        // Per-frame render loop. For screenshots this executes exactly once
        // (producing the final frame); for animation output it runs once per
        // frame and keeps every frame.
        const int renderFrames = config.hasScreenshot() ? 1 : totalFrames;
        for (int i = 0; i < renderFrames; i++)
        {
            queue->draw(drawKey, CommandServerDrawCallback(
                                     [&, frameDt, advanceScene](DrawKey, CommandServer* srv)
                                     {
                                         auto* artboard = advanceScene(srv, frameDt);
                                         if (artboard)
                                             currentFrame = headless.renderFrame(artboard, nullptr);

                                         std::lock_guard<std::mutex> lock(frameMutex);
                                         frameReady = true;
                                         frameCv.notify_one();
                                     }));

            // Wait for frame on main thread
            {
                std::unique_lock<std::mutex> lock(frameMutex);
                frameCv.wait(lock, [&] { return frameReady; });
                frameReady = false;
            }

            frames.push_back(std::move(currentFrame));
        }

        // 11. Cleanup
        queue->disconnect();
        serverThread.join();

        return QueueRenderResult{
            .frames = std::move(frames),
            .width = config.width,
            .height = config.height,
        };
    }
    catch (...)
    {
        queue->disconnect();
        serverThread.join();
        throw;
    }
}
