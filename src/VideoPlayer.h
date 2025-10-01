#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <list>

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
    int getFrameCount() const { return totalFrames; }
    double getFPS() const { return fps; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }
    double getDuration() const { return duration; }
    int getCurrentFrameIndex() const { return currentFrameIndex.load(std::memory_order_relaxed); }

private:
    bool loaded = false;
    std::atomic<bool> playing{false};
    std::string errorMessage;

    // Video metadata
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration = 0.0;
    int totalFrames = 0;

    // Playback state
    std::atomic<int> currentFrameIndex{0};
    std::chrono::steady_clock::time_point lastFrameTime;
    std::chrono::microseconds frameDuration{0};

    // Sync mode tracking - when receiving external clock sync, disable internal timer
    std::atomic<bool> externalSyncActive{false};
    std::chrono::steady_clock::time_point lastSyncTime;

    // Frame cache (ring buffer) - on-demand decoding
    static constexpr size_t MAX_CACHED_FRAMES = 300;  // ~600MB for 720p
    std::unordered_map<int, VideoFrame> frameCache;
    std::list<int> cacheOrder;  // LRU tracking
    mutable std::mutex cacheMutex;

    // FFmpeg decoder mutex (FFmpeg contexts are NOT thread-safe)
    std::mutex decoderMutex;

    // FFmpeg contexts (kept open for on-demand decoding)
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    SwsContext* swsContext = nullptr;
    int videoStreamIndex = -1;

    // Background decoder thread
    std::thread decoderThread;
    std::atomic<bool> shouldStopDecoder{false};
    std::atomic<int> lastDecodedFrame{-1};

    // Private methods
    bool decodeFrame(int frameIndex);
    void ensureFrameLoaded(int frameIndex);
    void backgroundDecoderTask();
    void evictOldFrames();
    void closeFFmpegContexts();
};