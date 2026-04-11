#pragma once
/**
 * @file FileWatcher.hpp
 * @brief Platform-abstracted real-time file watcher.
 *
 * Uses inotify on Linux and kqueue on macOS to detect file modifications
 * without polling. Each watcher monitors a single file in its own thread,
 * reading new lines and pushing them into the shared RingBuffer.
 *
 * RAII: all OS handles (inotify fd, kqueue fd, file fd) are cleaned up
 * in the destructor or via RAII wrapper classes.
 */

#include "PatternEngine.hpp"   // LogEntry
#include "RingBuffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <mutex>

// Platform-specific headers
#ifdef __linux__
    #include <sys/inotify.h>
    #include <unistd.h>
    #include <cerrno>
    #include <cstring>
#elif defined(__APPLE__)
    #include <sys/event.h>
    #include <sys/time.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <cerrno>
    #include <cstring>
#endif

namespace logmonitor {

/// Default ring buffer type used throughout the system
using LogRingBuffer = RingBuffer<LogEntry, 65536>;

// ─── RAII handle wrappers ───

#ifdef __linux__
/// RAII wrapper for inotify file descriptor
class InotifyHandle {
public:
    InotifyHandle() : fd_(inotify_init1(IN_NONBLOCK)) {
        if (fd_ < 0) {
            throw std::runtime_error(
                std::string("inotify_init1 failed: ") + std::strerror(errno));
        }
    }
    ~InotifyHandle() { if (fd_ >= 0) ::close(fd_); }
    InotifyHandle(const InotifyHandle&) = delete;
    InotifyHandle& operator=(const InotifyHandle&) = delete;
    [[nodiscard]] int fd() const noexcept { return fd_; }
private:
    int fd_;
};
#endif

#ifdef __APPLE__
/// RAII wrapper for kqueue file descriptor
class KqueueHandle {
public:
    KqueueHandle() : fd_(kqueue()) {
        if (fd_ < 0) {
            throw std::runtime_error(
                std::string("kqueue() failed: ") + std::strerror(errno));
        }
    }
    ~KqueueHandle() { if (fd_ >= 0) ::close(fd_); }
    KqueueHandle(const KqueueHandle&) = delete;
    KqueueHandle& operator=(const KqueueHandle&) = delete;
    [[nodiscard]] int fd() const noexcept { return fd_; }
private:
    int fd_;
};

/// RAII wrapper for a plain file descriptor (used with kqueue)
class FileDescriptor {
public:
    FileDescriptor() : fd_(-1) {}
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    FileDescriptor(FileDescriptor&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    FileDescriptor& operator=(FileDescriptor&& o) noexcept {
        if (this != &o) { if (fd_ >= 0) ::close(fd_); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
private:
    int fd_;
};
#endif

// ─── Per-file state (shared with dashboard for timestamps) ───

struct FileWatchState {
    std::string path;
    std::atomic<uint64_t> lines_read{0};
    std::atomic<int64_t> last_seen_epoch_ms{0};  // milliseconds since epoch

    void update_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        last_seen_epoch_ms.store(ms, std::memory_order_relaxed);
    }
};

/**
 * @brief Watches a single log file and pushes new lines into the ring buffer.
 *
 * Runs in its own thread. Uses OS-level file change notifications to avoid
 * polling. Handles log rotation (file truncation) by resetting the read position.
 */
class FileWatcher {
public:
    FileWatcher(std::string file_path,
                LogRingBuffer& ring_buffer,
                std::shared_ptr<FileWatchState> state,
                std::atomic<bool>& stop_flag)
        : file_path_(std::move(file_path))
        , ring_buffer_(ring_buffer)
        , state_(std::move(state))
        , stop_flag_(stop_flag)
    {}

    /// Start watching in the current thread (blocking)
    void run() {
        // Open the file and seek to end (only watch new lines)
        std::ifstream file(file_path_);
        if (!file.is_open()) {
            // File might not exist yet — wait for creation
            if (!wait_for_file()) return;
            file.open(file_path_);
            if (!file.is_open()) return;
        }
        file.seekg(0, std::ios::end);
        auto last_pos = file.tellg();
        auto last_size = std::filesystem::file_size(file_path_);

#ifdef __linux__
        run_inotify(file, last_pos, last_size);
#elif defined(__APPLE__)
        run_kqueue(file, last_pos, last_size);
#else
        // Fallback: polling (for unsupported platforms)
        run_polling(file, last_pos, last_size);
#endif
    }

private:
    /// Wait for file to be created (checks every 500ms)
    bool wait_for_file() {
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            if (std::filesystem::exists(file_path_)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return false;
    }

    /// Read new lines from current position and push to ring buffer
    void read_new_lines(std::ifstream& file, std::streampos& last_pos,
                        std::uintmax_t& last_size) {
        // Check for log rotation (file was truncated)
        auto current_size = std::filesystem::file_size(file_path_);
        if (current_size < last_size) {
            // File was truncated — reset to beginning
            file.clear();
            file.seekg(0, std::ios::beg);
            last_pos = 0;
        }
        last_size = current_size;

        file.clear();  // Clear any EOF flags
        file.seekg(last_pos);

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;

            LogEntry entry;
            entry.line = std::move(line);
            entry.source = file_path_;
            entry.timestamp = std::chrono::steady_clock::now();
            entry.line_number = state_->lines_read.fetch_add(1, std::memory_order_relaxed) + 1;

            // Try to push; if buffer is full, spin briefly then retry
            int retries = 0;
            while (!ring_buffer_.try_push(std::move(entry))) {
                if (stop_flag_.load(std::memory_order_relaxed)) return;
                if (++retries > 100) {
                    std::this_thread::yield();
                    retries = 0;
                }
            }

            state_->update_timestamp();
        }

        last_pos = file.tellg();
        if (last_pos == std::streampos(-1)) {
            // tellg returns -1 at EOF with failbit set
            file.clear();
            file.seekg(0, std::ios::end);
            last_pos = file.tellg();
        }
    }

#ifdef __linux__
    void run_inotify(std::ifstream& file, std::streampos last_pos,
                     std::uintmax_t last_size) {
        InotifyHandle inotify;
        int wd = inotify_add_watch(inotify.fd(), file_path_.c_str(), IN_MODIFY);
        if (wd < 0) {
            std::cerr << "[ERROR] inotify_add_watch failed for " << file_path_
                      << ": " << std::strerror(errno) << "\n";
            return;
        }

        constexpr size_t BUF_SIZE = 4096;
        char buf[BUF_SIZE];

        while (!stop_flag_.load(std::memory_order_relaxed)) {
            // Use select() with timeout for interruptible blocking
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(inotify.fd(), &fds);
            struct timeval tv = {0, 200000};  // 200ms timeout

            int ret = select(inotify.fd() + 1, &fds, nullptr, nullptr, &tv);
            if (ret > 0) {
                // Drain inotify events
                ssize_t n = read(inotify.fd(), buf, BUF_SIZE);
                (void)n;  // We just need to drain; we'll read the file regardless
                read_new_lines(file, last_pos, last_size);
            }
        }
    }
#endif

#ifdef __APPLE__
    void run_kqueue(std::ifstream& file, std::streampos last_pos,
                    std::uintmax_t last_size) {
        KqueueHandle kq;

        // Open the file for kqueue monitoring (need a raw fd)
        FileDescriptor watch_fd(::open(file_path_.c_str(), O_RDONLY | O_EVTONLY));
        if (!watch_fd.valid()) {
            std::cerr << "[ERROR] Cannot open " << file_path_
                      << " for kqueue: " << std::strerror(errno) << "\n";
            return;
        }

        // Register for VNODE write events
        struct kevent change;
        EV_SET(&change, watch_fd.fd(), EVFILT_VNODE,
               EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_ATTRIB, 0, nullptr);

        if (kevent(kq.fd(), &change, 1, nullptr, 0, nullptr) < 0) {
            std::cerr << "[ERROR] kevent register failed for " << file_path_
                      << ": " << std::strerror(errno) << "\n";
            return;
        }

        struct kevent event;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            // 200ms timeout for interruptible blocking
            struct timespec timeout = {0, 200000000};  // 200ms
            int n = kevent(kq.fd(), nullptr, 0, &event, 1, &timeout);

            if (n > 0) {
                read_new_lines(file, last_pos, last_size);
            }
            // n == 0 → timeout (check stop flag and loop)
            // n < 0 → error (check stop flag and loop)
        }
    }
#endif

    /// Fallback polling implementation
    void run_polling(std::ifstream& file, std::streampos last_pos,
                     std::uintmax_t last_size) {
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            read_new_lines(file, last_pos, last_size);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::string file_path_;
    LogRingBuffer& ring_buffer_;
    std::shared_ptr<FileWatchState> state_;
    std::atomic<bool>& stop_flag_;
};

} // namespace logmonitor
