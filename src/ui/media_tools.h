// media_tools: video thumbnails and the hover video preview.
//
// CryptVault does not link a video codec library. When the `ffmpeg` command
// is installed (it is on most desktops; Fedora ships it via RPM Fusion /
// ffmpeg-free), it is run as a separate, sandboxed-by-process subprocess that
// writes raw pixels to a pipe; when it isn't, video files simply keep the
// plain file icon and the preview says how to enable it. Nothing here ever
// runs on vault contents - only the explorer side asks for previews.
//
// Override the program with CRYPTVAULT_FFMPEG=/path/to/ffmpeg (ffprobe is
// looked up next to it, then on PATH, and is optional: it only picks a
// better start point for long videos).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cryptvault {

bool IsVideoName(const std::string& file_name);

// Aborts any ffmpeg/ffprobe work in progress or queued (they return false
// promptly) - called when the app is shutting down so quitting never waits
// on a slow video.
void CancelMediaJobs();

// ffmpeg found on this machine? (checked once)
bool FfmpegAvailable();

// Length in seconds via ffprobe, or 0 when unknown.
double VideoDurationSeconds(const std::string& path);

// One frame from ~10% into the video, scaled to fit box x box (aspect kept,
// transparent padding), as box*box*4 RGBA bytes. Blocking (up to ~6 s) - call
// from a worker thread.
bool GrabVideoFrameRgba(const std::string& path, int box, std::vector<unsigned char>& rgba);

// Muted, looping preview of a video (about 20 s starting near 10% in) decoded
// by a background thread at ~12 frames per second into `width` x `height`
// RGBA frames. The UI thread polls TakeFrame() each frame and uploads the
// newest one to a texture. Stop() (or destruction) kills the ffmpeg process.
class VideoPreview {
public:
    VideoPreview();
    ~VideoPreview();
    VideoPreview(const VideoPreview&) = delete;
    VideoPreview& operator=(const VideoPreview&) = delete;

    // False when ffmpeg isn't available. Restarts if already playing something.
    bool Start(const std::string& path, int width, int height);
    void Stop();

    bool Active() const;        // started and not stopped
    bool Failed() const;        // ffmpeg ran but produced no frames (unreadable/unsupported file)
    // Copies the newest frame not yet returned; false if there is no new one.
    bool TakeFrame(std::vector<unsigned char>& rgba);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cryptvault
