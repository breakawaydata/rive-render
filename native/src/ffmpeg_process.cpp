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
#include <vector>

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

FfmpegEncoder::FfmpegEncoder(const std::string& ffmpegPath, const std::vector<std::string>& args,
                             int width, int height, const std::string& what)
    : m_what(what), m_frameBytes(static_cast<size_t>(width) * height * 4)
{
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
        throw FfmpegError("Failed to launch ffmpeg (" + ffmpegPath +
                          "): " + std::strerror(spawnErr) + ". Is ffmpeg installed and in PATH?");
    }
    m_pid = pid;
    m_stdinFd = inPipe[1];
    m_stderrFd = errPipe[0];

    try
    {
        // Drain stderr on a thread so a chatty ffmpeg can never block on a full stderr pipe
        // while we block writing its stdin.
        m_stderrReader = std::thread(
            [this]()
            {
                char buf[4096];
                for (;;)
                {
                    ssize_t n = read(m_stderrFd, buf, sizeof(buf));
                    if (n < 0 && errno == EINTR)
                    {
                        continue;
                    }
                    if (n <= 0)
                    {
                        break;
                    }
                    std::lock_guard<std::mutex> lock(m_stderrMutex);
                    m_stderrCaptured.append(buf, static_cast<size_t>(n));
                    if (m_stderrCaptured.size() > kStderrCap)
                    {
                        m_stderrCaptured.erase(0, m_stderrCaptured.size() - kStderrCap);
                    }
                }
            });
        m_writer = std::thread([this]() { writerLoop(); });
    }
    catch (const std::exception& e)
    {
        // ffmpeg is already running: stop it and collect it, then report.
        shutdown(true);
        throw FfmpegError(std::string("Failed to start ffmpeg helper threads: ") + e.what());
    }
}

FfmpegEncoder::~FfmpegEncoder()
{
    // A no-op after finish() or a reported failure; otherwise we are unwinding from an error and
    // ffmpeg is still waiting for frames, so it is killed instead of finishing a truncated file.
    shutdown(true);
}

void FfmpegEncoder::writerLoop()
{
    for (;;)
    {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_notEmpty.wait(lock, [this] { return !m_queue.empty() || m_closed; });
            if (m_queue.empty())
            {
                return; // closed and drained
            }
            frame = std::move(m_queue.front());
            m_queue.pop_front();
            m_notFull.notify_one();
        }
        if (!writeAll(m_stdinFd, frame.data(), m_frameBytes))
        {
            int err = errno;
            std::lock_guard<std::mutex> lock(m_mutex);
            m_writeFailed = true;
            m_writeErrno = err;
            m_queue.clear();
            m_notFull.notify_all();
            return;
        }
    }
}

void FfmpegEncoder::push(std::vector<uint8_t>&& frame)
{
    if (frame.size() < m_frameBytes)
    {
        throw std::runtime_error("Frame pixel buffer too small");
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_notFull.wait(lock, [this] { return m_queue.size() < kQueueCapacity || m_writeFailed; });
    if (m_writeFailed)
    {
        // EPIPE means ffmpeg already closed its stdin and is exiting on its own; any other write
        // error leaves it in an unknown state, so it is killed.
        bool killChild = m_writeErrno != EPIPE;
        lock.unlock();
        shutdown(killChild);
        throw FfmpegError(failureMessage());
    }
    m_queue.push_back(std::move(frame));
    m_notEmpty.notify_one();
}

void FfmpegEncoder::finish()
{
    shutdown(false);
    std::string message = failureMessage();
    if (!message.empty())
    {
        throw FfmpegError(message);
    }
}

void FfmpegEncoder::shutdown(bool killChild)
{
    if (m_reaped)
    {
        return;
    }
    if (killChild && m_pid > 0)
    {
        kill(m_pid, SIGKILL);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        if (killChild)
        {
            m_queue.clear();
        }
    }
    m_notEmpty.notify_all();
    m_notFull.notify_all();
    if (m_writer.joinable())
    {
        m_writer.join();
    }
    // EOF on stdin tells ffmpeg the stream is complete.
    closeFd(m_stdinFd);

    if (m_pid > 0)
    {
        if (waitForChild(m_pid, &m_status) < 0)
        {
            m_waitFailed = true;
            m_waitErrno = errno;
        }
    }
    m_reaped = true;

    if (m_stderrReader.joinable())
    {
        m_stderrReader.join();
    }
    closeFd(m_stderrFd);
}

std::string FfmpegEncoder::failureMessage()
{
    std::string reason;
    if (m_waitFailed)
    {
        reason = std::string("waitpid failed: ") + std::strerror(m_waitErrno);
    }
    else if (WIFSIGNALED(m_status))
    {
        reason = "ffmpeg was killed by signal " + std::to_string(WTERMSIG(m_status));
    }
    else if (WIFEXITED(m_status) && WEXITSTATUS(m_status) != 0)
    {
        reason = "ffmpeg exited with status " + std::to_string(WEXITSTATUS(m_status));
    }
    else if (m_writeFailed)
    {
        reason =
            std::string("Failed to write frame data to ffmpeg: ") + std::strerror(m_writeErrno);
    }

    if (reason.empty())
    {
        return "";
    }
    std::string message = reason + " during " + m_what;
    std::string tail;
    {
        std::lock_guard<std::mutex> lock(m_stderrMutex);
        tail = stderrTail(m_stderrCaptured);
    }
    if (!tail.empty())
    {
        message += ". ffmpeg stderr:\n" + tail;
    }
    return message;
}
