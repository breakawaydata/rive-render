#ifdef RIVE_VULKAN

#include "queue_renderer.hpp"
#include "headless_renderer.hpp"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

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

// Image listener to track async decoding
class QueueImageListener : public CommandQueue::RenderImageListener
{
  public:
    std::atomic<int> decoded{0};
    std::atomic<int> total{0};

    void onRenderImageDecoded(const RenderImageHandle, uint64_t) override { decoded.fetch_add(1); }
};

// Font listener to track async decoding
class QueueFontListener : public CommandQueue::FontListener
{
  public:
    std::atomic<int> decoded{0};
    std::atomic<int> total{0};

    void onFontDecoded(const FontHandle, uint64_t) override { decoded.fetch_add(1); }
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

    try
    {
        // 4. Load file
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

        // 5. Decode referenced assets
        QueueImageListener imageListener;
        for (auto& [name, path] : config.assets.images)
        {
            auto bytes = readAssetFile(path);
            imageListener.total.fetch_add(1);
            queue->decodeImage(std::move(bytes), &imageListener);
        }

        QueueFontListener fontListener;
        for (auto& [name, path] : config.assets.fonts)
        {
            auto bytes = readAssetFile(path);
            fontListener.total.fetch_add(1);
            queue->decodeFont(std::move(bytes), &fontListener);
        }

        // Wait for all assets to decode
        if (imageListener.total.load() > 0 || fontListener.total.load() > 0)
        {
            waitFor(
                queue,
                [&]()
                {
                    return imageListener.decoded.load() >= imageListener.total.load() &&
                           fontListener.decoded.load() >= fontListener.total.load();
                },
                "asset decoding", 30000);
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
            auto vmHandle = config.viewModelData.instance.empty()
                                ? queue->instantiateBlankViewModelInstance(fileHandle, abHandle)
                                : queue->instantiateViewModelInstanceNamed(
                                      fileHandle, abHandle, config.viewModelData.instance);

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
            }
        }

        // 9. Determine frame parameters
        float fps = config.hasOutput() ? config.output.fps : 60.0f;
        float dt = 1.0f / fps;

        // For screenshots: advance to timestamp first
        if (config.hasScreenshot() && config.screenshot.timestamp > 0)
        {
            float warmupDt = 1.0f / 60.0f;
            int warmupFrames = static_cast<int>(config.screenshot.timestamp * 60.0f);
            for (int i = 0; i < warmupFrames; i++)
            {
                queue->advanceStateMachine(smHandle, warmupDt);
            }
        }

        int totalFrames = config.hasOutput() ? static_cast<int>(fps * config.output.duration) : 1;

        // 10. Render frames via draw callback
        // Following the Apple runtime pattern: the draw callback receives
        // the CommandServer, gets the artboard + render context from it,
        // then does all rendering (beginFrame, draw, flush) inside the
        // callback on the server thread.
        std::vector<std::vector<uint8_t>> frames;
        frames.reserve(totalFrames);

        std::mutex frameMutex;
        std::condition_variable frameCv;
        bool frameReady = false;
        std::vector<uint8_t> currentFrame;

        auto drawKey = queue->createDrawKey();

        for (int i = 0; i < totalFrames; i++)
        {
            if (config.hasOutput())
            {
                queue->advanceStateMachine(smHandle, dt);
            }

            queue->draw(drawKey, CommandServerDrawCallback(
                                     [&](DrawKey, CommandServer* srv)
                                     {
                                         // Get artboard from server (owns the instance)
                                         auto* artboard = srv->getArtboardInstance(abHandle);
                                         if (!artboard)
                                         {
                                             std::lock_guard<std::mutex> lock(frameMutex);
                                             frameReady = true;
                                             frameCv.notify_one();
                                             return;
                                         }

                                         // Get render context from server's factory
                                         // (matches Apple runtime: riveContext =
                                         //    static_cast<RenderContext*>(server->factory()))
                                         auto* riveContext =
                                             static_cast<gpu::RenderContext*>(srv->factory());

                                         // Render the frame using the headless renderer's
                                         // Vulkan infrastructure but the server's artboard
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

#endif // RIVE_VULKAN
