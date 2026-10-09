#include "ffmpeg_process.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace
{

// Keep at most this much of ffmpeg's stderr; the end of it is where the error is.
constexpr size_t kStderrCap = 16 * 1024;
// Number of trailing stderr lines quoted in an error message.
constexpr size_t kStderrTailLines = 12;

void closeFd(int& fd)
{
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

void makePipe(int fds[2])
{
    if (pipe(fds) != 0)
    {
        throw std::runtime_error(std::string("Failed to create pipe for ffmpeg: ") +
                                 std::strerror(errno));
    }
    // The parent's ends must not leak into the child (or into any later child).
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
}

// The last few non-empty lines of ffmpeg's stderr. ffmpeg's progress output uses '\r'.
std::string stderrTail(const std::string& captured)
{
    std::vector<std::string> lines;
    std::string current;
    for (char c : captured)
    {
        if (c == '\n' || c == '\r')
        {
            if (!current.empty())
            {
                lines.push_back(current);
            }
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }
    if (!current.empty())
    {
        lines.push_back(current);
    }
    size_t start = lines.size() > kStderrTailLines ? lines.size() - kStderrTailLines : 0;
    std::string out;
    for (size_t i = start; i < lines.size(); i++)
    {
        out += lines[i];
        if (i + 1 < lines.size())
        {
            out += "\n";
        }
    }
    return out;
}

// Write all of `size` bytes, retrying on EINTR and short writes. Returns false on any other
// error (EPIPE when ffmpeg has exited early), with errno set.
bool writeAll(int fd, const uint8_t* data, size_t size)
{
    while (size > 0)
    {
        ssize_t n = write(fd, data, size);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

pid_t waitForChild(pid_t pid, int* status)
{
    pid_t waited;
    do
    {
        waited = waitpid(pid, status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited;
}

} // namespace

void runFfmpegWithFrames(const std::string& ffmpegPath, const std::vector<std::string>& args,
                         int width, int height, const std::vector<std::vector<uint8_t>>& frames,
                         const std::string& what)
{
    size_t expectedSize = static_cast<size_t>(width) * height * 4;
    for (const auto& frame : frames)
    {
        if (frame.size() < expectedSize)
        {
            throw std::runtime_error("Frame pixel buffer too small");
        }
    }

    // A write to a pipe whose reader (ffmpeg) has exited raises SIGPIPE, which would kill this
    // process before it can report anything. Ignore it so write() fails with EPIPE instead;
    // the child gets SIGPIPE reset to default below.
    std::signal(SIGPIPE, SIG_IGN);

    int inPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    makePipe(inPipe);
    try
    {
        makePipe(errPipe);
    }
    catch (...)
    {
        closeFd(inPipe[0]);
        closeFd(inPipe[1]);
        throw;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    sigset_t emptyMask;
    sigemptyset(&emptyMask);
    posix_spawnattr_setsigmask(&attr, &emptyMask);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(ffmpegPath.c_str()));
    for (const auto& arg : args)
    {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    int spawnErr = posix_spawnp(&pid, ffmpegPath.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    // The child holds its own copies of these ends now.
    closeFd(inPipe[0]);
    closeFd(errPipe[1]);

    if (spawnErr != 0)
    {
        closeFd(inPipe[1]);
        closeFd(errPipe[0]);
        throw std::runtime_error("Failed to launch ffmpeg (" + ffmpegPath + "): " +
                                 std::strerror(spawnErr) + ". Is ffmpeg installed and in PATH?");
    }

    // Drain stderr on a thread so a chatty ffmpeg can never block on a full stderr pipe while
    // we block writing its stdin.
    std::string captured;
    std::mutex capturedMutex;
    int errFd = errPipe[0];
    auto drainStderr = [errFd, &captured, &capturedMutex]()
    {
        char buf[4096];
        for (;;)
        {
            ssize_t n = read(errFd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR)
            {
                continue;
            }
            if (n <= 0)
            {
                break;
            }
            std::lock_guard<std::mutex> lock(capturedMutex);
            captured.append(buf, static_cast<size_t>(n));
            if (captured.size() > kStderrCap)
            {
                captured.erase(0, captured.size() - kStderrCap);
            }
        }
    };
    std::thread stderrReader;
    try
    {
        stderrReader = std::thread(drainStderr);
    }
    catch (const std::exception& e)
    {
        // ffmpeg is already running: give it EOF, collect it, then report.
        closeFd(inPipe[1]);
        closeFd(errPipe[0]);
        int ignored = 0;
        waitForChild(pid, &ignored);
        throw std::runtime_error(std::string("Failed to start ffmpeg stderr reader: ") + e.what());
    }

    bool writeFailed = false;
    int writeErrno = 0;
    for (const auto& frame : frames)
    {
        if (!writeAll(inPipe[1], frame.data(), expectedSize))
        {
            writeFailed = true;
            writeErrno = errno;
            break;
        }
    }
    // EOF on stdin tells ffmpeg the stream is complete.
    closeFd(inPipe[1]);

    int status = 0;
    pid_t waited = waitForChild(pid, &status);

    stderrReader.join();
    closeFd(errPipe[0]);

    std::string reason;
    if (waited < 0)
    {
        reason = std::string("waitpid failed: ") + std::strerror(errno);
    }
    else if (WIFSIGNALED(status))
    {
        reason = "ffmpeg was killed by signal " + std::to_string(WTERMSIG(status));
    }
    else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
    {
        reason = "ffmpeg exited with status " + std::to_string(WEXITSTATUS(status));
    }
    else if (writeFailed)
    {
        reason = std::string("Failed to write frame data to ffmpeg: ") + std::strerror(writeErrno);
    }

    if (!reason.empty())
    {
        std::string message = reason + " during " + what;
        std::string tail = stderrTail(captured);
        if (!tail.empty())
        {
            message += ". ffmpeg stderr:\n" + tail;
        }
        throw std::runtime_error(message);
    }
}
