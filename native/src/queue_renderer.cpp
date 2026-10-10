#include "queue_renderer.hpp"
#include "headless_renderer.hpp"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <curl/curl.h>

#include "utils/no_op_factory.hpp"

#include "rive/animation/linear_animation_instance.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/artboard.hpp"
#include "rive/assets/font_asset.hpp"
#include "rive/assets/image_asset.hpp"
#include "rive/command_queue.hpp"
#include "rive/command_server.hpp"
#include "rive/file.hpp"
#include "rive/file_asset_loader.hpp"
#include "rive/logging_scripting_context.hpp"
#include "rive/renderer.hpp"
#include "rive/simple_array.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_asset_font_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_asset_image_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_list_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_trigger_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_runtime.hpp"

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

static size_t curlWriteToVector(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    const size_t total = size * nmemb;
    const auto* bytes = reinterpret_cast<const uint8_t*>(ptr);
    out->insert(out->end(), bytes, bytes + total);
    return total;
}

// Fetch bytes from a URL in-process via libcurl. Returns empty on failure.
//
// This deliberately does NOT shell out to the `curl` binary: the production
// backend runs on a hardened, minimal container image with no `/bin/sh` and
// no `curl` executable, so a `popen("curl ...")` here silently failed and
// CDN-hosted Rive assets (notably nameplate fonts) never loaded — rendering
// blank text. libcurl is linked into the binary, so this works regardless of
// what executables exist in the runtime image.
static std::vector<uint8_t> fetchUrl(const std::string& url)
{
    // curl_global_init / curl_global_cleanup are handled once in main() before
    // any threads start (libcurl's global init is not thread-safe). Here we
    // only create a per-call easy handle, which is safe on the server thread.
    CURL* curl = curl_easy_init();
    if (!curl)
        return {};

    std::vector<uint8_t> data;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToVector);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); // HTTP >= 400 -> failure
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // thread-safe timeouts
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "rive-render");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // accept gzip/deflate

    const CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return {};
    return data;
}

// Build a RiveCDN URL from a FileAsset's cdnBaseUrl + cdnUuidStr.
// Returns empty string if the asset has no CDN reference.
static std::string cdnUrlFor(const rive::FileAsset& asset)
{
    auto cdnBase = asset.cdnBaseUrl();
    auto cdnUuid = asset.cdnUuidStr();
    if (cdnBase.empty() || cdnUuid.empty())
        return {};
    std::string url = cdnBase;
    if (url.back() != '/')
        url += '/';
    url += cdnUuid;
    return url;
}

// Strip a trailing file extension: "flower-45020.png" -> "flower-45020".
static std::string stripExtension(const std::string& key)
{
    auto dot = key.rfind('.');
    return dot == std::string::npos ? key : key.substr(0, dot);
}

// Resolves every image/font asset in the .riv at import time. Passed to the
// CommandServer as its internal loader, which rive-runtime consults *before*
// its own command-queue global-asset handling, with the asset's in-band bytes
// (if any) and CDN reference in hand. Handles, in order:
//
//   1. Caller overrides keyed by uniqueName ("flower-45020", with or without
//      the file extension). These replace the asset even when the .riv embeds
//      it. rive-runtime >= v0.1.4xx stopped letting global assets override
//      embedded ones, so this loader is what keeps `assets.images` /
//      `assets.fonts` able to swap embedded art the way they always could.
//   2. Caller overrides keyed by the bare asset name ("flower"), for assets
//      with no in-band bytes (referenced / CDN-hosted slots) only.
//   3. CDN-hosted assets with no override: fetched in-process via libcurl.
//
// Everything else (embedded, no override) returns false so the importer
// decodes the in-band bytes as normal.
//
// Extra files (artboards bound into the main file's artboard properties) have
// their own override tables: two files can name an asset the same, and each
// must get the override that was configured for *its* file. The caller picks
// the table with setActive() before loading each file, and loads files one at
// a time, so the table is never switched while a load is in flight.
class RenderAssetLoader : public FileAssetLoader
{
  public:
    // Key of the main file's table.
    static constexpr const char* kMainFile = "";

    explicit RenderAssetLoader(const AssetConfig& mainAssets)
    {
        addFile(kMainFile, mainAssets);
        m_active = &m_files.at(kMainFile);
    }

    // Reads every override file of `assets` up front, so a bad asset path
    // fails before the server thread exists.
    void addFile(const std::string& alias, const AssetConfig& assets)
    {
        Overrides& overrides = m_files[alias];
        for (auto& [name, path] : assets.images)
            overrides.images[stripExtension(name)] = readAssetFile(path);
        for (auto& [name, path] : assets.fonts)
            overrides.fonts[stripExtension(name)] = readAssetFile(path);
    }

    // Selects the override table used for the next file load.
    void setActive(const std::string& alias) { m_active = &m_files.at(alias); }

    bool loadContents(FileAsset& asset, Span<const uint8_t> inBandBytes, Factory* factory) override
    {
        const bool isImage = asset.is<ImageAsset>();
        const bool isFont = asset.is<FontAsset>();
        if (!isImage && !isFont)
            return false;
        auto& overrides = isImage ? m_active->images : m_active->fonts;

        auto it = overrides.find(asset.uniqueName());
        if (it == overrides.end() && inBandBytes.empty())
            it = overrides.find(asset.name());
        if (it != overrides.end())
            return decode(asset, it->second, factory);

        if (!inBandBytes.empty())
            return false;
        auto url = cdnUrlFor(asset);
        if (url.empty())
            return false;
        auto bytes = fetchUrl(url);
        // Tiny bodies are error pages / empty responses, never a real asset.
        if (bytes.size() <= 100)
            return false;
        return decode(asset, bytes, factory);
    }

  private:
    static bool decode(FileAsset& asset, const std::vector<uint8_t>& bytes, Factory* factory)
    {
        SimpleArray<uint8_t> arr(bytes.data(), bytes.size());
        return asset.decode(arr, factory);
    }

    struct Overrides
    {
        std::map<std::string, std::vector<uint8_t>> images;
        std::map<std::string, std::vector<uint8_t>> fonts;
    };

    std::map<std::string, Overrides> m_files;
    const Overrides* m_active = nullptr;
};

// Walk a property tree and collect every filesystem path referenced by a
// property of `type` ("image" / "font"). Recurses through nested list rows and
// bound artboards so a payload anywhere in the tree gets pre-decoded.
static void collectAssetPaths(const std::map<std::string, ViewModelPropertyValue>& properties,
                              const char* type, std::vector<std::string>& outPaths)
{
    for (auto& [_, prop] : properties)
    {
        if (prop.type == type && !prop.stringValue.empty())
            outPaths.push_back(prop.stringValue);
        else if (prop.type == "list")
        {
            for (auto& item : prop.listValue)
                collectAssetPaths(item.properties, type, outPaths);
        }
        else if (prop.type == "artboard" && prop.artboardValue)
            collectAssetPaths(prop.artboardValue->properties, type, outPaths);
    }
}

// Resolve a VM runtime using the 3-priority chain shared by all render paths:
//   1. Caller-specified `vmName` (if non-empty).
//   2. The artboard's own default VM (so authored list/component-list slots
//      resolve against the right schema).
//   3. The file's first VM (legacy fallback).
// Returns nullptr if none can be found.
static rive::ViewModelRuntime* resolveViewModelRuntime(rive::File* file,
                                                       rive::ArtboardInstance* artboard,
                                                       const std::string& vmName)
{
    rive::ViewModelRuntime* vm = nullptr;
    if (!vmName.empty())
        vm = file->viewModelByName(vmName);
    if (!vm && artboard)
        vm = file->defaultArtboardViewModel(artboard);
    if (!vm && file->viewModelCount() > 0)
        vm = file->viewModelByIndex(0);
    return vm;
}

// Decoded images/fonts for `{ type: "image" | "font" }` VM properties, keyed by
// the filesystem path supplied in the payload. Resolved to raw pointers on the
// server thread (via CommandServer::getImage / getFont) before properties are
// applied, so applyPropertiesDirect is purely a dispatch.
struct DecodedVmAssets
{
    std::map<std::string, rive::RenderImage*> images;
    std::map<std::string, rive::Font*> fonts;
};

// A view model instance created for a bound artboard, with the properties it
// was given. Kept alive until the parent is bound so its triggers can fire.
struct NestedBinding
{
    rcp<rive::ViewModelInstanceRuntime> inst;
    const std::map<std::string, ViewModelPropertyValue>* properties;
};

// Everything applyPropertiesDirect needs besides the instance it fills.
struct ApplyContext
{
    const DecodedVmAssets& decoded;
    CommandServer* server;
    const std::map<std::string, FileHandle>& extraFiles;
    // Appended to for every artboard binding, so the caller can fire the
    // nested triggers after the parent is bound.
    std::vector<NestedBinding>& nested;

    // The loaded File behind an extraFiles alias, or nullptr for an alias the
    // config never declared.
    rive::File* resolveFile(const std::string& alias) const
    {
        auto it = extraFiles.find(alias);
        return it == extraFiles.end() ? nullptr : server->getFile(it->second);
    }
};

// Bind an artboard of an extra file into the artboard property at `path`, with
// a view model instance of that same file carrying the nested properties.
// Throws on a bad alias, artboard, view model or property path: a binding that
// silently did nothing would render an empty card with no hint why.
static void bindExternalArtboard(rive::ViewModelInstanceRuntime* inst, const std::string& path,
                                 const ArtboardBindingConfig& binding, const ApplyContext& ctx);

// Apply a property map to a ViewModelInstanceRuntime on the server thread.
// Recursively descends into list children — each list row gets a freshly
// created VM instance, has its own properties applied, and is appended to the
// parent list. Triggers are skipped here; they fire after binding (see
// fireTriggers) so the bound state machine observes them.
static void applyPropertiesDirect(rive::File* file, rive::ViewModelInstanceRuntime* inst,
                                  const std::map<std::string, ViewModelPropertyValue>& properties,
                                  const ApplyContext& ctx)
{
    if (!inst)
        return;
    const DecodedVmAssets& decoded = ctx.decoded;
    for (auto& [path, prop] : properties)
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
        else if (prop.type == "image")
        {
            auto* p = inst->propertyImage(path);
            auto it = decoded.images.find(prop.stringValue);
            if (p && it != decoded.images.end() && it->second)
                p->value(it->second);
        }
        else if (prop.type == "font")
        {
            auto* p = inst->propertyFont(path);
            auto it = decoded.fonts.find(prop.stringValue);
            if (p && it != decoded.fonts.end() && it->second)
                p->value(it->second);
        }
        else if (prop.type == "list")
        {
            auto* listProp = inst->propertyList(path);
            if (!listProp)
                continue;
            // Reset to a known state so the rendered list matches the
            // payload exactly. Without this, repeated renders against the
            // same VM instance would accumulate rows.
            listProp->removeAllInstances();
            for (auto& item : prop.listValue)
            {
                if (file->viewModelCount() == 0)
                    break;
                // Resolve the row VM: prefer the caller-supplied name, fall
                // back to the file's first VM (same last-resort as the parent
                // path). No artboard context is available here, so the
                // artboard-default-VM step is skipped for rows.
                rive::ViewModelRuntime* rowVm =
                    resolveViewModelRuntime(file, nullptr, item.viewModel);
                if (!rowVm)
                    continue;
                // Prefer createDefaultInstance for new list rows so the row
                // VM ships with file-authored defaults — that's also what
                // populates each row's underlying property objects.
                // `createInstance()` returns an *empty* runtime instance
                // whose `propertyString(...)` / `propertyNumber(...)` etc.
                // return nullptr, so any user-supplied row properties get
                // silently dropped.
                auto rowInst = !item.instance.empty() ? rowVm->createInstanceFromName(item.instance)
                                                      : rowVm->createDefaultInstance();
                if (!rowInst)
                    continue;
                applyPropertiesDirect(file, rowInst.get(), item.properties, ctx);
                listProp->addInstance(rowInst.get());
            }
        }
        else if (prop.type == "artboard")
        {
            if (!prop.artboardValue)
                throw std::runtime_error("Artboard property '" + path + "' has no binding");
            bindExternalArtboard(inst, path, *prop.artboardValue, ctx);
        }
    }
}

static void bindExternalArtboard(rive::ViewModelInstanceRuntime* inst, const std::string& path,
                                 const ArtboardBindingConfig& binding, const ApplyContext& ctx)
{
    const std::string where = "Artboard property '" + path + "'";
    if (binding.file.empty() || binding.artboard.empty())
        throw std::runtime_error(where + " requires both \"file\" and \"artboard\"");

    auto* abProp = inst->propertyArtboard(path);
    if (!abProp)
        throw std::runtime_error("No artboard property at path '" + path + "'");

    rive::File* extra = ctx.resolveFile(binding.file);
    if (!extra)
        throw std::runtime_error(where + ": unknown file '" + binding.file +
                                 "' (declare it under extraFiles)");

    auto bindable = extra->bindableArtboardNamed(binding.artboard);
    if (!bindable)
        throw std::runtime_error(where + ": artboard '" + binding.artboard +
                                 "' not found in file '" + binding.file + "'");

    // The nested artboard reads its data from a view model instance of ITS
    // file. Without one it falls back to the parent file's view models and
    // renders empty, so always create one whenever the file has a view model.
    if (!binding.viewModel.empty() && !extra->viewModelByName(binding.viewModel))
        throw std::runtime_error(where + ": view model '" + binding.viewModel +
                                 "' not found in file '" + binding.file + "'");
    auto nestedArtboard = extra->artboardNamed(binding.artboard);
    rive::ViewModelRuntime* nestedVm =
        resolveViewModelRuntime(extra, nestedArtboard.get(), binding.viewModel);
    rcp<rive::ViewModelInstanceRuntime> nestedInst;
    if (nestedVm)
    {
        nestedInst = nestedVm->createDefaultInstance();
        if (!nestedInst)
            throw std::runtime_error(where + ": could not create a view model instance of '" +
                                     nestedVm->name() + "'");
        applyPropertiesDirect(extra, nestedInst.get(), binding.properties, ctx);
    }
    else if (!binding.properties.empty())
    {
        throw std::runtime_error(where + ": file '" + binding.file +
                                 "' has no view model for the properties to bind to");
    }

    // Order matters: value() clears any view model instance already bound to
    // the property, so the instance goes in second.
    abProp->value(bindable);
    if (nestedInst)
    {
        abProp->viewModelInstance(nestedInst->instance());
        ctx.nested.push_back({nestedInst, &binding.properties});
    }
}

// Fire every top-level `{ type: "trigger" }` property. Runs after the
// instance is bound so the state machine's data-bound transitions and
// listeners see the trigger on their next advance.
static void fireTriggers(rive::ViewModelInstanceRuntime* inst,
                         const std::map<std::string, ViewModelPropertyValue>& properties)
{
    for (auto& [path, prop] : properties)
    {
        if (prop.type != "trigger")
            continue;
        if (auto* p = inst->propertyTrigger(path))
            p->trigger();
    }
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
static void waitFor(rcp<CommandQueue>& queue, Pred pred, const char* what, int timeoutMs)
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

// Run `fn` on the server thread and block until it has finished. Any
// exception it throws is rethrown on the calling thread.
static void runSync(rcp<CommandQueue>& queue, std::function<void(CommandServer*)> fn)
{
    std::promise<void> done;
    auto future = done.get_future();
    queue->runOnce(
        [&fn, &done](CommandServer* srv)
        {
            try
            {
                fn(srv);
                done.set_value();
            }
            catch (...)
            {
                done.set_exception(std::current_exception());
            }
        });
    future.get();
}

// Route Luau console output and script errors to stderr so stdout carries
// only the JSON result line.
static ScriptingContextFactory stderrScriptingContextFactory()
{
    return makeLoggingScriptingContextFactory(
        [](ScriptingLogLevel level, const char* data, size_t length)
        {
            const char* prefix = level == ScriptingLogLevel::error  ? "[rive script error] "
                                 : level == ScriptingLogLevel::warn ? "[rive script warn] "
                                                                    : "[rive script] ";
            std::fprintf(stderr, "%s%.*s\n", prefix, static_cast<int>(length), data);
        });
}

void resolveCanvasSize(Config& config, const std::vector<uint8_t>& rivBytes)
{
    if (config.width > 0 && config.height > 0)
        return;

    // Import on a throwaway no-op factory just to read the artboard bounds.
    // Assets are left unresolved (no loader) since only geometry is needed.
    NoOpFactory factory;
    auto file = File::import(rivBytes, &factory);
    if (!file)
        throw std::runtime_error("Failed to load .riv while measuring artboard size");
    auto artboard =
        config.artboard.empty() ? file->artboardDefault() : file->artboardNamed(config.artboard);
    if (!artboard)
        throw std::runtime_error("Artboard not found: " + config.artboard);

    const float abWidth = artboard->width();
    const float abHeight = artboard->height();
    if (abWidth <= 0 || abHeight <= 0)
        throw std::runtime_error("Artboard has no size; pass width and height explicitly");

    // Only one dimension given: keep the artboard's aspect ratio.
    if (config.width > 0)
        config.height = static_cast<int>(std::lround(config.width * abHeight / abWidth));
    else if (config.height > 0)
        config.width = static_cast<int>(std::lround(config.height * abWidth / abHeight));
    else
    {
        config.width = static_cast<int>(std::lround(abWidth));
        config.height = static_cast<int>(std::lround(abHeight));
    }
}

QueueRenderResult renderWithQueue(const Config& config, const std::vector<uint8_t>& rivBytes,
                                  const FrameSink& sink)
{
    // 1. Create headless renderer
    HeadlessRenderer headless(config.width, config.height, config.swiftshader);

    // 2. Create queue + server. The server's internal asset loader resolves
    //    caller asset overrides and CDN-hosted assets during File::import
    //    (see RenderAssetLoader). It reads every override file up front, so
    //    a bad asset path fails here, before the server thread exists.
    //    Extra files get their own override tables, so same-named assets in
    //    different files cannot collide.
    auto queue = make_rcp<CommandQueue>();
    auto assetLoader = make_rcp<RenderAssetLoader>(config.assets);
    for (auto& [alias, extra] : config.extraFiles)
        assetLoader->addFile(alias, extra.assets);
    // Read the extra .riv files here too, so a bad path fails before the
    // server thread exists.
    std::map<std::string, std::vector<uint8_t>> extraRivBytes;
    for (auto& [alias, extra] : config.extraFiles)
    {
        std::ifstream f(extra.rivFile, std::ios::binary);
        if (!f.is_open())
            throw std::runtime_error("Failed to open extra .riv file '" + alias +
                                     "': " + extra.rivFile);
        extraRivBytes[alias] = std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                                    std::istreambuf_iterator<char>());
    }
    auto server = std::make_unique<CommandServer>(queue, headless.renderContext(), assetLoader);

    // 3. Start server on background thread
    std::thread serverThread([&server]() { server->serveUntilDisconnect(); });

    try
    {
        // 4. Decode every image/font referenced by a `{ type: "image" | "font" }`
        //    VM property (including those nested inside list rows). These bind
        //    to VM property slots, not to file asset slots — the same split as
        //    `@rive-app/react-native`'s `RiveImages` (our `assets.images`) vs.
        //    `ViewModelImageProperty` (our `{ type: "image" }`).
        std::map<std::string, RenderImageHandle> vmImageHandles;
        std::map<std::string, FontHandle> vmFontHandles;
        {
            std::vector<std::string> paths;
            collectAssetPaths(config.viewModelData.properties, "image", paths);
            for (auto& path : paths)
                if (!vmImageHandles.count(path)) // dedupe: same file referenced twice
                    vmImageHandles[path] = queue->decodeImage(readAssetFile(path));
            paths.clear();
            collectAssetPaths(config.viewModelData.properties, "font", paths);
            for (auto& path : paths)
                if (!vmFontHandles.count(path))
                    vmFontHandles[path] = queue->decodeFont(readAssetFile(path));
        }

        // 5. Load file. Generous timeout: CDN-hosted assets are downloaded
        //    during import by RenderAssetLoader.
        QueueFileListener fileListener;
        auto fileHandle = queue->loadFile(std::vector<uint8_t>(rivBytes.begin(), rivBytes.end()),
                                          &fileListener, 0, stderrScriptingContextFactory());
        waitFor(
            queue, [&]() { return fileListener.loaded.load() || fileListener.errored.load(); },
            "file load", 120000);
        if (fileListener.errored.load())
        {
            throw std::runtime_error("Failed to load .riv: " + fileListener.errorMsg);
        }

        // 5b. Load the extra files one at a time, each with its own asset
        //     override table active, waiting for each load to finish before
        //     switching tables.
        std::map<std::string, FileHandle> extraFileHandles;
        std::vector<std::unique_ptr<QueueFileListener>> extraListeners;
        for (auto& [alias, bytes] : extraRivBytes)
        {
            assetLoader->setActive(alias);
            auto listener = std::make_unique<QueueFileListener>();
            auto handle = queue->loadFile(std::vector<uint8_t>(bytes.begin(), bytes.end()),
                                          listener.get(), 0, stderrScriptingContextFactory());
            auto* l = listener.get();
            extraListeners.push_back(std::move(listener));
            waitFor(
                queue, [&]() { return l->loaded.load() || l->errored.load(); }, "extra file load",
                120000);
            if (l->errored.load())
                throw std::runtime_error("Failed to load extra .riv '" + alias +
                                         "': " + l->errorMsg);
            extraFileHandles[alias] = handle;
        }
        assetLoader->setActive(RenderAssetLoader::kMainFile);

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

        // 8. Bind view model data and apply state machine inputs, all on the
        //    server thread before any time advances. Lists, images and fonts
        //    need the runtime API (per-row property setting isn't exposed as
        //    queue commands), so everything goes through the same direct path.
        //
        //    Only bind when the caller supplied data: binding a default VM
        //    instance can change how the artboard renders vs. its authored
        //    defaults, which regression tests rely on as the baseline.
        //    `assets.images` also triggers a bind, preserving the historical
        //    behaviour for callers that swap images without VM properties.
        const bool bindViewModel =
            !config.viewModelData.properties.empty() || !config.assets.images.empty();
        runSync(queue,
                [&](CommandServer* srv)
                {
                    auto* file = srv->getFile(fileHandle);
                    auto* artboard = srv->getArtboardInstance(abHandle);
                    auto* sm = srv->getStateMachineInstance(smHandle);
                    if (!file || !artboard)
                        throw std::runtime_error(config.artboard.empty()
                                                     ? "File has no default artboard"
                                                     : "Artboard not found: " + config.artboard);

                    if (bindViewModel && file->viewModelCount() > 0)
                    {
                        const auto& vmName = config.viewModelData.viewModel;
                        const auto& instanceName = config.viewModelData.instance;
                        auto* viewModelRuntime = resolveViewModelRuntime(file, artboard, vmName);
                        if (!vmName.empty() && !file->viewModelByName(vmName))
                            throw std::runtime_error("View model not found: " + vmName);
                        if (!viewModelRuntime)
                            throw std::runtime_error("No view model found to bind");

                        // createDefaultInstance (not createInstance) so file-
                        // authored default VM data — including default list rows
                        // referenced by an `ArtboardComponentList` — survives the
                        // bind. applyPropertiesDirect calls removeAllInstances()
                        // on each list it touches, so caller list payloads still
                        // fully replace the defaults.
                        auto inst = instanceName.empty()
                                        ? viewModelRuntime->createDefaultInstance()
                                        : viewModelRuntime->createInstanceFromName(instanceName);
                        if (!inst)
                            throw std::runtime_error("View model instance not found: " +
                                                     instanceName);

                        DecodedVmAssets decoded;
                        for (auto& [path, handle] : vmImageHandles)
                            decoded.images[path] = srv->getImage(handle);
                        for (auto& [path, handle] : vmFontHandles)
                            decoded.fonts[path] = srv->getFont(handle);
                        std::vector<NestedBinding> nestedBindings;
                        ApplyContext applyCtx{decoded, srv, extraFileHandles, nestedBindings};
                        applyPropertiesDirect(file, inst.get(), config.viewModelData.properties,
                                              applyCtx);

                        // The state machine and its artboard share one data
                        // context, so binding the state machine binds both.
                        if (sm)
                            sm->bindViewModelInstance(inst->instance());
                        else
                            artboard->bindViewModelInstance(inst->instance());
                        fireTriggers(inst.get(), config.viewModelData.properties);
                        // Triggers on bound artboards' view models fire after
                        // the parent bind too, so their state machines see them.
                        for (auto& nested : nestedBindings)
                            fireTriggers(nested.inst.get(), *nested.properties);

                        // Two zero-dt advances so data binds propagate through
                        // nested artboards: the first instantiates nested artboard
                        // components and relays the data context, the second
                        // processes the dirty data binds inside them (e.g. text
                        // runs reading firstName/lastName from the VM). Without
                        // both, nested text renders empty.
                        for (int i = 0; i < 2; i++)
                        {
                            if (sm)
                                sm->advanceAndApply(0.0f);
                            else
                                artboard->advance(0.0f);
                        }
                    }

                    if (sm)
                    {
                        for (auto& [name, value] : config.stateMachineNumberInputs)
                            if (auto* input = sm->getNumber(name))
                                input->value(value);
                        for (auto& [name, value] : config.stateMachineBoolInputs)
                        {
                            if (auto* input = sm->getBool(name))
                                input->value(value);
                            // `true` on a trigger input fires it.
                            else if (value)
                                if (auto* trigger = sm->getTrigger(name))
                                    trigger->fire();
                        }
                    }
                });

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

        // Per-frame render loop. Screenshots and png output execute it exactly
        // once (screenshots produce the final frame); animation output runs it
        // once per frame and hands every frame to the sink.
        const bool singleFrame = config.hasScreenshot() || config.output.format == "png";
        const int renderFrames = singleFrame ? 1 : totalFrames;
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

            // The sink usually only queues the frame for an encoder thread, so the
            // next draw is issued while the previous frame is still being encoded.
            // It may block (backpressure) or throw; either way no draw is in flight
            // here, and a throw takes the catch below.
            sink(std::move(currentFrame));
            currentFrame = {};
        }

        // 11. Cleanup
        queue->disconnect();
        serverThread.join();

        return QueueRenderResult{
            .frameCount = renderFrames,
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
