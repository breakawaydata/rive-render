#include "device_select.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <dlfcn.h>
#endif

namespace
{

constexpr const char* kDefaultNvidiaCtlPath = "/dev/nvidiactl";
constexpr const char* kSwiftShaderIcdName = "vk_swiftshader_icd.json";
constexpr const char* kNvidiaIcdName = "nvidia_icd.json";

std::string envOrEmpty(const char* name)
{
    const char* value = std::getenv(name);
    return value ? value : "";
}

bool fileExists(const std::string& path)
{
    return !path.empty() && access(path.c_str(), F_OK) == 0;
}

bool namesSwiftShader(const std::string& value)
{
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("swiftshader") != std::string::npos;
}

std::string joinPath(const std::string& dir, const char* name)
{
    return dir.empty() ? std::string(name) : dir + "/" + name;
}

std::string binaryDirectory()
{
#ifdef __linux__
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0)
    {
        return "";
    }
    exePath[len] = '\0';
    std::string path(exePath);
    auto slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
#else
    return "";
#endif
}

// A directory only this user can use: created 0700 if missing, then checked with lstat so a
// symlink, a directory owned by someone else, or a group/world-writable one is refused.
bool ensurePrivateDirectory(const std::string& dir)
{
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
    {
        return false;
    }
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0)
    {
        return false;
    }
    return S_ISDIR(st.st_mode) && st.st_uid == getuid() && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

// Whether the NVIDIA Vulkan driver library can be loaded.
bool nvidiaVulkanLibraryAvailable()
{
    std::string override = envOrEmpty(RIVE_RENDER_NVIDIA_VULKAN_LIB_ENV);
    if (override == "0" || override == "1")
    {
        return override == "1";
    }
#ifdef __linux__
    if (void* handle = dlopen("libEGL_nvidia.so.0", RTLD_LAZY | RTLD_LOCAL))
    {
        dlclose(handle);
        return true;
    }
#endif
    return false;
}

// Write `content` to `path` through a temp file in the same directory and rename() it into place,
// so a concurrent reader sees the old file or the whole new one, never a partial write.
bool writeFileAtomically(const std::string& path, const std::string& content)
{
    std::string tmpl = path + ".XXXXXX";
    int fd = mkstemp(&tmpl[0]);
    if (fd < 0)
    {
        return false;
    }
    bool ok = fchmod(fd, 0644) == 0;
    size_t written = 0;
    while (ok && written < content.size())
    {
        ssize_t n = write(fd, content.data() + written, content.size() - written);
        if (n < 0)
        {
            ok = false;
        }
        else
        {
            written += static_cast<size_t>(n);
        }
    }
    ok = (close(fd) == 0) && ok;
    if (ok && rename(tmpl.c_str(), path.c_str()) != 0)
    {
        ok = false;
    }
    if (!ok)
    {
        unlink(tmpl.c_str());
    }
    return ok;
}

} // namespace

const char* nvidiaIcdJson()
{
    return R"({"file_format_version":"1.0.1","ICD":{"library_path":"libEGL_nvidia.so.0","api_version":"1.4.312"}})";
}

IcdSelection fallbackIcd(const IcdInputs& in)
{
    if (in.bundledSwiftshaderIcdExists)
    {
        return {"swiftshader", joinPath(in.binaryDir, kSwiftShaderIcdName), false};
    }
    return {"default", "", false};
}

IcdSelection selectIcd(const IcdInputs& in)
{
    if (in.metal)
    {
        return {"metal", "", false};
    }
    if (in.swiftshader)
    {
        // Unchanged from the original --swiftshader behaviour: point at the bundled json whether
        // or not it exists, so a missing bundle surfaces as a Vulkan device error.
        return {"swiftshader",
                in.binaryDir.empty() ? "" : joinPath(in.binaryDir, kSwiftShaderIcdName), false};
    }
    if (!in.callerIcdFilenames.empty() || !in.callerDriverFiles.empty())
    {
        return {"caller", "", false};
    }
    if (in.nvidiaDeviceNode && in.nvidiaVulkanLibrary)
    {
        if (in.bundledNvidiaIcdExists)
        {
            return {"nvidia", joinPath(in.binaryDir, kNvidiaIcdName), false};
        }
        std::string tmp = in.tmpDir.empty() ? "/tmp" : in.tmpDir;
        while (tmp.size() > 1 && tmp.back() == '/')
        {
            tmp.pop_back();
        }
        return {"nvidia", tmp + "/rive-render-" + std::to_string(in.uid) + "/" + kNvidiaIcdName,
                true};
    }
    return fallbackIcd(in);
}

bool autoSelectsNvenc(const IcdInputs& in, const IcdSelection& selection)
{
    if (in.metal || !in.nvidiaDeviceNode || selection.kind == "swiftshader")
    {
        return false;
    }
    if (selection.kind == "caller" &&
        (namesSwiftShader(in.callerIcdFilenames) || namesSwiftShader(in.callerDriverFiles)))
    {
        return false;
    }
    return true;
}

IcdInputs currentIcdInputs(bool swiftshader)
{
    IcdInputs in;
    in.swiftshader = swiftshader;
    in.callerIcdFilenames = envOrEmpty("VK_ICD_FILENAMES");
    in.callerDriverFiles = envOrEmpty("VK_DRIVER_FILES");
#ifdef __APPLE__
    in.metal = true;
#endif
    std::string nvidiaCtl = envOrEmpty(RIVE_RENDER_NVIDIACTL_PATH_ENV);
    in.nvidiaDeviceNode = fileExists(nvidiaCtl.empty() ? kDefaultNvidiaCtlPath : nvidiaCtl);
    in.nvidiaVulkanLibrary = in.nvidiaDeviceNode && nvidiaVulkanLibraryAvailable();
    in.binaryDir = binaryDirectory();
    in.bundledNvidiaIcdExists = fileExists(joinPath(in.binaryDir, kNvidiaIcdName));
    in.bundledSwiftshaderIcdExists = fileExists(joinPath(in.binaryDir, kSwiftShaderIcdName));
    in.tmpDir = envOrEmpty("TMPDIR");
    in.uid = static_cast<unsigned>(getuid());
    return in;
}

IcdSelection applyIcdSelection(const IcdSelection& selection)
{
    if (selection.kind == "default")
    {
        // Only reached when the caller set no ICD of their own (otherwise the kind is "caller"),
        // so anything in VK_ICD_FILENAMES is our earlier override.
        unsetenv("VK_ICD_FILENAMES");
        return selection;
    }
    if (selection.kind != "nvidia" && selection.kind != "swiftshader")
    {
        return selection;
    }
    if (selection.icdPath.empty())
    {
        return selection;
    }
    if (selection.generate)
    {
        std::string dir = selection.icdPath.substr(0, selection.icdPath.rfind('/'));
        if (!ensurePrivateDirectory(dir) ||
            !writeFileAtomically(selection.icdPath, nvidiaIcdJson()))
        {
            std::cerr << "rive-render: could not safely write " << selection.icdPath
                      << "; leaving the Vulkan loader default" << std::endl;
            unsetenv("VK_ICD_FILENAMES");
            return {"default", "", false};
        }
    }
    setenv("VK_ICD_FILENAMES", selection.icdPath.c_str(), 1);
    return selection;
}
