#ifdef RIVE_VULKAN

#include "queue_renderer.hpp"
#include "headless_renderer.hpp"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "rive/animation/linear_animation_instance.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/artboard.hpp"
#include "rive/command_queue.hpp"
#include "rive/command_server.hpp"

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
    HeadlessRenderer headless(config.width, config.height);

    // 2. Create queue + server
    auto queue = make_rcp<CommandQueue>();
    CommandServer server(queue, headless.renderContext());

    // 3. Start server on background thread
    std::thread serverThread([&server]() { server.serveUntilDisconnect(); });

    // CommandFileAssetLoader matches by FileAsset::uniqueName(), which is the
    // base name plus asset id with the extension stripped (see
    // file_asset.cpp::uniqueName). Users typically supply the uniqueFilename
    // ("flower-45020.png"), so strip the trailing extension to line up.
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
        for (auto& [name, path] : config.assets.images)
        {
            auto handle = queue->decodeImage(readAssetFile(path));
            queue->addGlobalImageAsset(toUniqueName(name), handle);
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

        // 6. Instantiate artboard
        auto abHandle = config.artboard.empty()
                            ? queue->instantiateDefaultArtboard(fileHandle)
                            : queue->instantiateArtboardNamed(fileHandle, config.artboard);

        // Set artboard size
        queue->setArtboardSize(abHandle, static_cast<float>(config.width),
                               static_cast<float>(config.height));

        // 7. Instantiate state machine
        auto smHandle = config.stateMachine.empty()
                            ? queue->instantiateDefaultStateMachine(abHandle)
                            : queue->instantiateStateMachineNamed(abHandle, config.stateMachine);

        // 8. Bind view model data (if provided)
        if (!config.viewModelData.properties.empty())
        {
            // Use named ViewModel if specified, otherwise default
            auto vmHandle =
                !config.viewModelData.viewModel.empty()
                    ? (!config.viewModelData.instance.empty()
                           ? queue->instantiateViewModelInstanceNamed(fileHandle, abHandle,
                                                                      config.viewModelData.instance)
                           : queue->instantiateBlankViewModelInstance(fileHandle, abHandle))
                : config.viewModelData.instance.empty()
                    ? queue->instantiateBlankViewModelInstance(fileHandle, abHandle)
                    : queue->instantiateViewModelInstanceNamed(fileHandle, abHandle,
                                                               config.viewModelData.instance);

            // Set properties
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
        // For GIFs/videos, use the configured fps and duration.
        // For screenshots, step at a fixed 60 Hz and advance up to
        // `screenshot.timestamp`, keeping only the final rendered frame.
        const float fps = config.hasOutput() ? config.output.fps : 60.0f;
        const float dt = 1.0f / fps;
        const int totalFrames =
            config.hasOutput()
                ? std::max(1, static_cast<int>(fps * config.output.duration))
                : std::max(1, static_cast<int>(config.screenshot.timestamp * fps));

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

        for (int i = 0; i < totalFrames; i++)
        {
            // Screenshots may request t=0 — we still render one frame but with
            // zero advance. Every other case advances by `dt` before drawing.
            const float frameDt =
                (config.hasScreenshot() && config.screenshot.timestamp == 0) ? 0.0f : dt;

            queue->draw(drawKey,
                        CommandServerDrawCallback(
                            [&, frameDt, scene](DrawKey, CommandServer* srv)
                            {
                                auto* artboard = srv->getArtboardInstance(abHandle);
                                if (!artboard)
                                {
                                    std::lock_guard<std::mutex> lock(frameMutex);
                                    frameReady = true;
                                    frameCv.notify_one();
                                    return;
                                }

                                auto* sm = srv->getStateMachineInstance(smHandle);

                                // Lazy-init the linear animation fallback on
                                // the first draw, once we know whether this
                                // artboard has a state machine.
                                if (!scene->initialized)
                                {
                                    scene->initialized = true;
                                    if (!sm && artboard->animationCount() > 0)
                                    {
                                        scene->linearAnim = artboard->animationAt(0);
                                    }
                                }

                                if (sm)
                                {
                                    sm->advanceAndApply(frameDt);
                                }
                                else if (scene->linearAnim)
                                {
                                    scene->linearAnim->advanceAndApply(frameDt);
                                }
                                else
                                {
                                    artboard->advance(frameDt);
                                }

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

            if (config.hasOutput())
            {
                frames.push_back(std::move(currentFrame));
            }
            else if (i == totalFrames - 1)
            {
                frames.push_back(std::move(currentFrame));
            }
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

#endif // RIVE_VULKAN
