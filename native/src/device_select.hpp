#pragma once

#include <string>

// Which Vulkan ICD (driver) the Linux build renders through, and whether the "auto" video encoder
// may use NVENC. The decisions are pure functions of IcdInputs so they can be exercised on any
// platform (see `rive_render --select-device`); only the Linux renderer applies them.

// Test-only override of the NVIDIA device node path (default /dev/nvidiactl). Setting it to an
// existing file makes the host look like it has an NVIDIA GPU; to a missing path, like it has none.
#define RIVE_RENDER_NVIDIACTL_PATH_ENV "RIVE_RENDER_NVIDIACTL_PATH"

struct IcdInputs
{
    // Config `swiftshader` flag.
    bool swiftshader = false;
    // Values of VK_ICD_FILENAMES and VK_DRIVER_FILES in the caller's environment ("" = unset).
    std::string callerIcdFilenames;
    std::string callerDriverFiles;
    // The NVIDIA device node exists.
    bool nvidiaDeviceNode = false;
    // The ICD json files shipped next to the binary exist.
    bool bundledNvidiaIcdExists = false;
    bool bundledSwiftshaderIcdExists = false;
    // Directory holding the binary and its bundled files ("" when unknown).
    std::string binaryDir;
    // $TMPDIR (falls back to /tmp when empty) and the uid, for the generated NVIDIA ICD json.
    std::string tmpDir;
    unsigned uid = 0;
    // macOS: Vulkan is not used at all.
    bool metal = false;
};

struct IcdSelection
{
    // "metal", "swiftshader", "caller", "nvidia" or "default".
    std::string kind;
    // The ICD json the loader will be pointed at; empty for "metal" and "default".
    std::string icdPath;
    // "nvidia" with no bundled json: icdPath must be generated before use (see applyIcdSelection).
    bool generate = false;
};

// Precedence: swiftshader flag, then the caller's VK_ICD_FILENAMES / VK_DRIVER_FILES, then the
// NVIDIA device node, then the bundled SwiftShader ICD, then the loader default.
IcdSelection selectIcd(const IcdInputs& in);

// True when encoder "auto" should use NVENC: an NVIDIA device node exists and the render is on
// the GPU rather than on SwiftShader. Never on macOS.
bool autoSelectsNvenc(const IcdInputs& in, const IcdSelection& selection);

// Inputs read from this process: environment, filesystem and platform.
IcdInputs currentIcdInputs(bool swiftshader);

// Content of the generated NVIDIA ICD json.
const char* nvidiaIcdJson();

// Make the Vulkan loader use `selection`: writes the generated NVIDIA json (to a temp file renamed
// into place, so concurrent renders never read a half-written file) and sets VK_ICD_FILENAMES.
// Returns the selection that is now in effect, which is "default" if the json could not be written.
IcdSelection applyIcdSelection(const IcdSelection& selection);
