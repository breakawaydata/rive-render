#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

// Thrown when ffmpeg cannot be started, dies, or exits non-zero. Callers that can retry with a
// different encoder catch this type; a plain std::runtime_error is a problem with the input.
class FfmpegError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// Run ffmpeg as a child process and stream raw RGBA frames into its stdin.
//
// ffmpeg is started with posix_spawnp and an explicit argv, never through a shell, so it works
// in images that have no /bin/sh (distroless / hardened bases, where popen() fails). A bare
// program name such as "ffmpeg" is looked up on PATH; a path containing '/' is used as is.
//
// `args` is the full argv after the program name. ffmpeg's stdout goes to /dev/null so it
// cannot corrupt the JSON result on our stdout; its stderr is captured and the tail of it is
// included in the exception thrown when ffmpeg cannot be started or exits non-zero.
//
// Frames go through a bounded queue drained by a writer thread, so the producer renders the
// next frame while ffmpeg encodes the previous ones, and memory never grows with the frame
// count: at most `kQueueCapacity` queued frames plus the one being written.
//
// Every frame must hold at least width * height * 4 bytes; exactly that many are written.
// `what` names the operation in error messages (e.g. "video encoding").
class FfmpegEncoder
{
  public:
    static constexpr size_t kQueueCapacity = 4;

    // Starts ffmpeg. Throws FfmpegError if it cannot be launched.
    FfmpegEncoder(const std::string& ffmpegPath, const std::vector<std::string>& args, int width,
                  int height, const std::string& what);
    // Safe on an exception path: kills ffmpeg if finish() was not reached, joins the threads and
    // reaps the child, leaving no thread or zombie behind.
    ~FfmpegEncoder();

    FfmpegEncoder(const FfmpegEncoder&) = delete;
    FfmpegEncoder& operator=(const FfmpegEncoder&) = delete;

    // Queue a frame for ffmpeg, blocking while the queue is full. Throws FfmpegError as soon as
    // the writer has failed (ffmpeg exited early). Throws std::runtime_error for a frame that is
    // too small.
    void push(std::vector<uint8_t>&& frame);

    // Close ffmpeg's stdin, let it drain, and wait for it. Throws FfmpegError if ffmpeg failed.
    void finish();

  private:
    void writerLoop();
    // Stop everything and reap the child. `killChild` sends SIGKILL first.
    void shutdown(bool killChild);
    // Empty when the encode succeeded; otherwise the full error message.
    std::string failureMessage();

    std::string m_what;
    size_t m_frameBytes;
    pid_t m_pid = -1;
    int m_stdinFd = -1;
    int m_stderrFd = -1;

    std::thread m_stderrReader;
    std::thread m_writer;

    std::mutex m_stderrMutex;
    std::string m_stderrCaptured;

    std::mutex m_mutex; // guards the members below
    std::condition_variable m_notFull;
    std::condition_variable m_notEmpty;
    std::deque<std::vector<uint8_t>> m_queue;
    bool m_closed = false; // no more frames will be pushed
    bool m_writeFailed = false;
    int m_writeErrno = 0;

    bool m_reaped = false;
    bool m_waitFailed = false;
    int m_waitErrno = 0;
    int m_status = 0;
};
