#include "ui/media_tools.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace cryptvault {

namespace {
std::atomic<bool> g_cancel_media{false};
}
void CancelMediaJobs() { g_cancel_media.store(true); }

bool IsVideoName(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* kExts[] = {"mp4", "mkv", "webm", "avi", "mov", "m4v", "wmv", "flv", "mpg", "mpeg", "ogv", "3gp", "ts", "m2ts"};
    for (const char* e : kExts) {
        if (ext == e) return true;
    }
    return false;
}

#ifdef _WIN32

bool FfmpegAvailable() { return false; }
double VideoDurationSeconds(const std::string&) { return 0.0; }
bool GrabVideoFrameRgba(const std::string&, int, std::vector<unsigned char>&) { return false; }
struct VideoPreview::Impl {};
VideoPreview::VideoPreview() = default;
VideoPreview::~VideoPreview() = default;
bool VideoPreview::Start(const std::string&, int, int) { return false; }
void VideoPreview::Stop() {}
bool VideoPreview::Active() const { return false; }
bool VideoPreview::Failed() const { return false; }
bool VideoPreview::TakeFrame(std::vector<unsigned char>&) { return false; }

#else

namespace {

std::string FindProgram(const char* env_var, const char* name, const char* sibling_of = nullptr) {
    auto executable = [](const std::string& p) { return !p.empty() && ::access(p.c_str(), X_OK) == 0; };
    if (env_var) {
        if (const char* env = std::getenv(env_var)) {
            if (executable(env)) return env;
        }
    }
    if (sibling_of) {  // e.g. ffprobe next to a custom ffmpeg
        std::string dir = sibling_of;
        size_t slash = dir.rfind('/');
        if (slash != std::string::npos) {
            std::string candidate = dir.substr(0, slash + 1) + name;
            if (executable(candidate)) return candidate;
        }
    }
    const char* path = std::getenv("PATH");
    std::string paths = path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= paths.size()) {
        size_t end = paths.find(':', start);
        if (end == std::string::npos) end = paths.size();
        std::string dir = paths.substr(start, end - start);
        if (!dir.empty()) {
            std::string candidate = dir + "/" + name;
            if (executable(candidate)) return candidate;
        }
        start = end + 1;
    }
    return "";
}

const std::string& FfmpegPath() {
    static const std::string path = FindProgram("CRYPTVAULT_FFMPEG", "ffmpeg");
    return path;
}

const std::string& FfprobePath() {
    static const std::string path = FindProgram(nullptr, "ffprobe", FfmpegPath().empty() ? nullptr : FfmpegPath().c_str());
    return path;
}

// A child process whose stdout we read through a pipe. stdin and stderr are
// /dev/null. No shell is involved: arguments are passed as an argv array, so
// file names can't be interpreted as commands.
class Child {
public:
    ~Child() { Kill(); }

    bool Start(const std::vector<std::string>& args) {
        int fds[2];
        if (::pipe(fds) != 0) return false;
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addclose(&actions, fds[1]);
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        pid_t pid = 0;
        int rc = ::posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(fds[1]);
        if (rc != 0) {
            ::close(fds[0]);
            return false;
        }
        pid_ = pid;
        fd_ = fds[0];
        return true;
    }

    // Reads exactly n bytes. False on EOF/error/timeout/stop request.
    bool ReadExact(unsigned char* buffer, size_t n, int timeout_ms, const std::atomic<bool>* stop = nullptr) {
        size_t got = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (got < n) {
            if ((stop && stop->load()) || g_cancel_media.load()) return false;
            if (std::chrono::steady_clock::now() > deadline) return false;
            struct pollfd p{fd_, POLLIN, 0};
            int pr = ::poll(&p, 1, 100);
            if (pr < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (pr == 0) continue;
            ssize_t r = ::read(fd_, buffer + got, n - got);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }

    std::string ReadAll(size_t max_bytes, int timeout_ms) {
        std::string out;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        char buf[256];
        while (out.size() < max_bytes && std::chrono::steady_clock::now() < deadline && !g_cancel_media.load()) {
            struct pollfd p{fd_, POLLIN, 0};
            int pr = ::poll(&p, 1, 100);
            if (pr < 0 && errno != EINTR) break;
            if (pr <= 0) continue;
            ssize_t r = ::read(fd_, buf, sizeof buf);
            if (r <= 0) break;
            out.append(buf, static_cast<size_t>(r));
        }
        return out;
    }

    void Kill() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    pid_t pid() const { return pid_; }

private:
    pid_t pid_ = -1;
    int fd_ = -1;
};

std::string StartTimeArg(const std::string& path) {
    double dur = VideoDurationSeconds(path);
    double start = dur > 30.0 ? std::min(dur * 0.10, 120.0) : (dur > 4.0 ? dur * 0.10 : 0.0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", start);
    return buf;
}

std::string FitFilter(int w, int h) {
    // Scale to fit, keep aspect, pad with transparent pixels to exactly w x h.
    return "scale=" + std::to_string(w) + ":" + std::to_string(h) + ":force_original_aspect_ratio=decrease,format=rgba,pad="
           + std::to_string(w) + ":" + std::to_string(h) + ":(ow-iw)/2:(oh-ih)/2:color=black@0";
}

}  // namespace

bool FfmpegAvailable() { return !FfmpegPath().empty(); }

double VideoDurationSeconds(const std::string& path) {
    if (FfprobePath().empty()) return 0.0;
    Child child;
    if (!child.Start({FfprobePath(), "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", "-i", path})) return 0.0;
    std::string out = child.ReadAll(64, 3000);
    char* end = nullptr;
    double v = std::strtod(out.c_str(), &end);
    return (end != out.c_str() && v > 0.0 && v < 1e7) ? v : 0.0;
}

bool GrabVideoFrameRgba(const std::string& path, int box, std::vector<unsigned char>& rgba) {
    if (!FfmpegAvailable()) return false;
    Child child;
    if (!child.Start({FfmpegPath(), "-v", "error", "-nostdin", "-ss", StartTimeArg(path), "-i", path, "-an", "-sn", "-dn",
                      "-frames:v", "1", "-vf", FitFilter(box, box), "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"})) {
        return false;
    }
    rgba.assign(static_cast<size_t>(box) * box * 4, 0);
    return child.ReadExact(rgba.data(), rgba.size(), 6000);
}

struct VideoPreview::Impl {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::vector<unsigned char> latest;
    bool has_new = false;
    std::string path;
    int width = 0, height = 0;

    void Run() {
        const size_t frame_bytes = static_cast<size_t>(width) * height * 4;
        const std::string start = StartTimeArg(path);
        std::vector<unsigned char> frame(frame_bytes);
        bool produced_any = false;
        while (!stop.load()) {  // loop the clip until stopped
            Child child;
            if (!child.Start({FfmpegPath(), "-v", "error", "-nostdin", "-re", "-ss", start, "-i", path, "-an", "-sn", "-dn",
                              "-t", "20", "-vf", "fps=12," + FitFilter(width, height), "-f", "rawvideo", "-pix_fmt", "rgba",
                              "pipe:1"})) {
                break;
            }
            bool produced_this_run = false;
            while (!stop.load() && child.ReadExact(frame.data(), frame_bytes, 8000, &stop)) {
                produced_this_run = produced_any = true;
                std::lock_guard<std::mutex> lock(mutex);
                latest = frame;
                has_new = true;
            }
            if (!produced_this_run) break;  // nothing decoded: don't spin restarting a broken file
        }
        if (!produced_any && !stop.load()) failed.store(true);
        running.store(false);
    }
};

VideoPreview::VideoPreview() : impl_(std::make_unique<Impl>()) {}
VideoPreview::~VideoPreview() { Stop(); }

bool VideoPreview::Start(const std::string& path, int width, int height) {
    Stop();
    if (!FfmpegAvailable() || width <= 0 || height <= 0) return false;
    impl_->stop.store(false);
    impl_->failed.store(false);
    impl_->running.store(true);
    impl_->has_new = false;
    impl_->latest.clear();
    impl_->path = path;
    impl_->width = width;
    impl_->height = height;
    impl_->thread = std::thread([this] { impl_->Run(); });
    return true;
}

void VideoPreview::Stop() {
    if (impl_->thread.joinable()) {
        impl_->stop.store(true);
        impl_->thread.join();   // the reader polls the stop flag every 100 ms and kills ffmpeg on exit
    }
    impl_->running.store(false);
}

bool VideoPreview::Active() const { return impl_->thread.joinable() && impl_->running.load(); }
bool VideoPreview::Failed() const { return impl_->failed.load(); }

bool VideoPreview::TakeFrame(std::vector<unsigned char>& rgba) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->has_new) return false;
    rgba = impl_->latest;
    impl_->has_new = false;
    return true;
}

#endif  // _WIN32

}  // namespace cryptvault
