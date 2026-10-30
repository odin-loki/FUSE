// fuse::ui::os — the one seam between FUSE GUI and the OS layer (docs/gui/PLAN.md §1).
// GUI code never calls the OS for files, environment, clocks, locks or dynamic loading itself; it calls these.
// The default implementation (Os/src/os_forge.cpp) forwards to FORGE (forge::os). A package with a different OS
// layer builds with -DFUSE_UI_OS_BACKEND=custom and supplies its own translation unit defining these functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::os {

/// Environment variable, or `fallback` when unset / empty.
std::string env(std::string_view name, std::string_view fallback = {});
/// Per-user configuration directory for `app` (%APPDATA%\app, $XDG_CONFIG_HOME/app, ~/.config/app, ...).
std::string configDir(std::string_view app);
/// Per-user runtime directory for sockets and locks ($XDG_RUNTIME_DIR, else the temp dir).
std::string runtimeDir();

std::expected<std::string, std::string> readFile(std::string_view path);
/// Writes through a temporary file and an atomic rename, creating missing parent directories.
std::expected<void, std::string> writeFileAtomic(std::string_view path, std::string_view data);
bool isDirectory(std::string_view path);
bool exists(std::string_view path);
/// Absolute, normalised path with forward slashes.
std::string absolutePath(std::string_view path);

struct DirEntry {
    std::string name;
    bool isDir = false;
};
/// Entries of `path` (no "." / ".."), unsorted.
std::expected<std::vector<DirEntry>, std::string> listDirectory(std::string_view path);

/// Watches files or directories for changes (hot reload of styles and .fuseui documents). Watching a file sees an
/// atomic replace-by-rename as a modification.
class FileWatcher {
public:
    virtual ~FileWatcher() = default;
    virtual bool add(std::string_view path) = 0;
    /// Paths that changed since the last poll; waits at most `timeoutMs` (0 = non-blocking).
    virtual std::vector<std::string> poll(int timeoutMs = 0) = 0;
};
std::unique_ptr<FileWatcher> createFileWatcher();

/// Seconds on a monotonic clock (arbitrary origin, fixed for the process).
double monotonicSeconds();
void sleepSeconds(double seconds);

/// Exclusive per-user lock named `name`; empty when another process holds it. Released on destruction.
class InstanceLock {
public:
    virtual ~InstanceLock() = default;
};
std::unique_ptr<InstanceLock> tryLockInstance(std::string_view name);

/// Runtime-loaded shared library (window-system, Vulkan and accessibility libraries are loaded this way).
class Library {
public:
    virtual ~Library() = default;
    virtual void* symbol(const char* name) const = 0;
    template <class F> F get(const char* name) const { return reinterpret_cast<F>(symbol(name)); }
};
/// Tries each file name in order (e.g. "libX11.so.6", "libX11.so"); null when none loads.
std::unique_ptr<Library> loadLibrary(std::initializer_list<const char*> names);

/// Audio output (PCM float playback). Built on loadLibrary, so it serves every OS backend: PulseAudio (libpulse-simple)
/// then ALSA (libasound) on Linux / BSD, waveOut (winmm) on Windows; other platforms report no backend.
/// `device`: backend-specific name (ALSA PCM such as "default" / "null"; empty = system default). The environment
/// variable FUSE_UI_AUDIO_DEVICE overrides an empty `device` ("alsa:<pcm>" forces ALSA, "pulse" forces PulseAudio).
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    /// Queues interleaved float samples; blocks while the device buffer is full (that is the playback clock).
    virtual bool write(const float* interleaved, std::size_t frames) = 0;
    /// Waits until everything written has been played.
    virtual void drain() = 0;
    /// Frames accepted by write() so far.
    virtual std::uint64_t framesWritten() const = 0;
    /// Seconds of written audio not yet heard (device latency + queue), best effort.
    virtual double latencySeconds() const = 0;
    virtual std::string backend() const = 0; ///< "pulse", "alsa:<pcm>", "winmm"
    virtual std::uint32_t sampleRate() const = 0;
    virtual std::uint32_t channels() const = 0;
};
std::unique_ptr<AudioOutput> openAudioOutput(std::uint32_t sampleRate, std::uint32_t channels, std::string* error = nullptr,
                                             std::string_view device = {});

/// Text clipboard without a bound window (window backends override this with their own selection handling).
std::expected<std::string, std::string> clipboardText();
bool setClipboardText(std::string_view text);

/// Rich clipboard (G2): typed payloads by MIME type ("text/plain", "text/html", "image/png", "text/uri-list", ...).
/// Default backend: wl-clipboard on Wayland, xclip on X11 (both spawned through FORGE). Unsupported -> error.
std::expected<std::vector<std::string>, std::string> clipboardTypes();
std::expected<std::string, std::string> clipboardData(std::string_view mime);
/// Offers `data` under `mime`. (One type per call; the last call owns the clipboard.)
bool setClipboardData(std::string_view mime, std::string_view data);

/// File-descriptor plumbing for window-system protocols that pass fds (Wayland shm buffers, keymaps, data
/// transfer). POSIX only; on Windows these fail. Backed by forge::os pipes / mappings plus the FORGE gaps noted in
/// os_forge.cpp (anonymous shared-memory fd).
struct SharedBuffer {
    int fd = -1;
    void* data = nullptr;
    std::size_t size = 0;
    SharedBuffer() = default;
    SharedBuffer(SharedBuffer&& o) noexcept { *this = std::move(o); }
    SharedBuffer& operator=(SharedBuffer&& o) noexcept;
    ~SharedBuffer();
};
/// Anonymous shared memory of `size` bytes with an fd to hand to another process.
std::expected<SharedBuffer, std::string> createSharedBuffer(std::size_t size);
/// Reads `size` bytes of a mappable fd (e.g. a Wayland keymap); the fd stays open.
std::expected<std::string, std::string> readMappedFd(int fd, std::size_t size);
/// Pipe: {read fd, write fd}; both close-on-exec.
std::expected<std::pair<int, int>, std::string> makePipe();
/// Reads `fd` to EOF (waiting at most `timeoutMs` per chunk) and closes it.
std::string readAllAndClose(int fd, int timeoutMs = 2000);
/// Writes all of `data` to `fd` and closes it.
bool writeAllAndClose(int fd, std::string_view data);
void closeFd(int fd);

/// Native (system) file dialog (G2). Default backend: zenity / kdialog (Linux, BSD), osascript (macOS), spawned
/// through FORGE. Returns std::nullopt-like `unavailable` when no native dialog exists, so the caller falls back to
/// the in-UI widgets::FileDialog; an empty list means the user cancelled.
struct FileFilter {
    std::string name;                  ///< "Images"
    std::vector<std::string> patterns; ///< {"*.png", "*.jpg"}
};
struct NativeDialogSpec {
    enum class Kind : std::uint8_t { Open, Save, Folder } kind = Kind::Open;
    std::string title, directory, fileName;
    std::vector<FileFilter> filters;
    bool multiple = false;
};
struct NativeDialogResult {
    bool unavailable = false;
    std::vector<std::string> paths; ///< empty + !unavailable: cancelled
};
NativeDialogResult nativeFileDialog(const NativeDialogSpec& spec);
/// Argument vector the default backend would run (testable without a display); empty when unavailable.
std::vector<std::string> nativeFileDialogCommand(const NativeDialogSpec& spec);
/// {display name, "*.png;*.jpg"} pairs as IFileDialog::SetFileTypes wants them (also used by the Win32 backend).
std::vector<std::pair<std::string, std::string>> fileDialogFilterSpec(const NativeDialogSpec& spec);

/// Runs a helper program to completion (desktop integration: notifications, URL opening, printing). Backed by
/// forge::os::spawn_wait. `timeoutMs` 0 = no limit.
struct ProcessResult {
    int exitCode = 0;
    std::string out, err;
};
std::expected<ProcessResult, std::string> runProcess(std::vector<std::string> argv, std::optional<std::string> stdinData = {},
                                                     int timeoutMs = 10000);
/// Full path of `name` on PATH, empty when absent.
std::string findExecutable(std::string_view name);
/// "windows", "linux", "freebsd", "macos", "ios", "android", or "unknown".
std::string_view platformName();

/// HDR state of the display showing a native window (HWND on Windows): FORGE's display_hdr (DXGI IDXGIOutput6).
/// `known` false: this OS layer cannot tell (non-Windows, or a FORGE without display_hdr); callers then trust the
/// swapchain colour spaces the driver offers.
struct DisplayHdr {
    bool known = false;
    bool capable = false; ///< can be driven in HDR10
    bool enabled = false; ///< the OS drives it in HDR right now (Windows "Use HDR")
    unsigned bitsPerColor = 8;
    float minNits = 0.f, maxNits = 0.f, maxFullFrameNits = 0.f;
};
DisplayHdr displayHdr(void* nativeWindow);

} // namespace fuse::ui::os
