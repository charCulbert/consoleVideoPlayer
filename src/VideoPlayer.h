#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct VideoFrame {
    std::vector<uint8_t> data;  // RGB24 pixel data
    int width;
    int height;
    int linesize;
};

class VideoPlayer {
public:
    VideoPlayer() = default;
    ~VideoPlayer();

    // Load and decode entire video into RAM
    bool loadVideo(const std::string& filePath);

    // Playback control
    void play();
    void pause();
    void stop();
    void seek(double seconds);

    // Sync to external audio clock (preferred method - drift-free)
    void syncToTimestamp(double audioTimestamp);

    // Get current frame for rendering
    const VideoFrame* getCurrentFrame();

    // Update playback position (call regularly) - fallback timer-based method
    void update();

    // Getters
    bool isLoaded() const { return loaded; }
    bool isPlaying() const { return playing; }
    std::string getErrorMessage() const { return errorMessage; }
    int getFrameCount() const { return frames.size(); }
    double getFPS() const { return fps; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }
    double getDuration() const { return duration; }

private:
    bool loaded = false;
    std::atomic<bool> playing{false};
    std::string errorMessage;

    std::vector<VideoFrame> frames;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration = 0.0;

    // Playback state
    std::atomic<int> currentFrameIndex{0};
    std::chrono::steady_clock::time_point lastFrameTime;
    std::chrono::microseconds frameDuration{0};

    // Sync mode tracking - when receiving external clock sync, disable internal timer
    std::atomic<bool> externalSyncActive{false};
    std::chrono::steady_clock::time_point lastSyncTime;
};