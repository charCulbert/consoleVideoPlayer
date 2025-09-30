#include "VideoPlayer.h"
#include <iostream>
#include <cstring>
#include <cmath>

#define DEBUG_PRINT(msg) do { \
    std::cout << "[VideoPlayer] " << msg << std::endl; \
    std::cout.flush(); \
} while(0)

VideoPlayer::~VideoPlayer() {
    // Stop decoder thread
    shouldStopDecoder = true;
    if (decoderThread.joinable()) {
        decoderThread.join();
    }

    // Clean up FFmpeg contexts
    closeFFmpegContexts();

    // Clear frame cache
    std::lock_guard<std::mutex> lock(cacheMutex);
    frameCache.clear();
    cacheOrder.clear();
}

void VideoPlayer::closeFFmpegContexts() {
    if (swsContext) {
        sws_freeContext(swsContext);
        swsContext = nullptr;
    }
    if (codecContext) {
        avcodec_free_context(&codecContext);
    }
    if (formatContext) {
        avformat_close_input(&formatContext);
    }
}

bool VideoPlayer::loadVideo(const std::string& filePath) {
    DEBUG_PRINT("Loading video: " << filePath);

    // Open video file
    if (avformat_open_input(&formatContext, filePath.c_str(), nullptr, nullptr) < 0) {
        errorMessage = "Failed to open video file";
        return false;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        errorMessage = "Failed to find stream info";
        closeFFmpegContexts();
        return false;
    }

    // Find video stream
    AVCodecParameters* codecParams = nullptr;
    for (unsigned int i = 0; i < formatContext->nb_streams; i++) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = i;
            codecParams = formatContext->streams[i]->codecpar;
            break;
        }
    }

    if (videoStreamIndex == -1) {
        errorMessage = "No video stream found";
        closeFFmpegContexts();
        return false;
    }

    // Get FPS and duration
    AVRational frameRate = formatContext->streams[videoStreamIndex]->avg_frame_rate;
    fps = (double)frameRate.num / (double)frameRate.den;
    if (fps <= 0) fps = 25.0; // Default fallback
    frameDuration = std::chrono::microseconds((int64_t)(1000000.0 / fps));

    duration = (double)formatContext->duration / AV_TIME_BASE;
    totalFrames = (int)(duration * fps);

    DEBUG_PRINT("Video info: " << codecParams->width << "x" << codecParams->height
                << " @ " << fps << " fps, duration: " << duration << "s, frames: " << totalFrames);

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        errorMessage = "Codec not found";
        closeFFmpegContexts();
        return false;
    }

    codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        errorMessage = "Failed to allocate codec context";
        closeFFmpegContexts();
        return false;
    }

    if (avcodec_parameters_to_context(codecContext, codecParams) < 0) {
        errorMessage = "Failed to copy codec parameters";
        closeFFmpegContexts();
        return false;
    }

    codecContext->thread_count = 0;

    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        errorMessage = "Failed to open codec";
        closeFFmpegContexts();
        return false;
    }

    width = codecContext->width;
    height = codecContext->height;

    // Setup scaler to convert to RGB24
    swsContext = sws_getContext(
        width, height, codecContext->pix_fmt,
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!swsContext) {
        errorMessage = "Failed to create scaler context";
        closeFFmpegContexts();
        return false;
    }

    // Pre-load first 150 frames sequentially (fast startup + seamless looping)
    DEBUG_PRINT("Pre-loading first 150 frames...");

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int numBytes = width * height * 3;
    int frameCount = 0;
    int maxPreload = std::min(150, totalFrames);

    // Seek to beginning
    av_seek_frame(formatContext, -1, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codecContext);

    // Decode sequentially (no seeking per frame - much faster!)
    while (av_read_frame(formatContext, packet) >= 0 && frameCount < maxPreload) {
        if (packet->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(codecContext, packet) >= 0) {
                while (avcodec_receive_frame(codecContext, frame) >= 0) {
                    VideoFrame vf;
                    vf.width = width;
                    vf.height = height;
                    vf.linesize = width * 3;
                    vf.data.resize(numBytes);

                    uint8_t* dest[1] = { vf.data.data() };
                    int destLinesize[1] = { vf.linesize };

                    sws_scale(swsContext,
                             frame->data, frame->linesize, 0, height,
                             dest, destLinesize);

                    // Add to cache
                    {
                        std::lock_guard<std::mutex> lock(cacheMutex);
                        frameCache[frameCount] = std::move(vf);
                        cacheOrder.push_back(frameCount);
                    }

                    frameCount++;
                    if (frameCount >= maxPreload) break;
                }
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);

    DEBUG_PRINT("Pre-loaded " << frameCount << " frames");

    // Calculate expected memory usage
    size_t expectedMemory = MAX_CACHED_FRAMES * width * height * 3;
    double memoryMB = (double)expectedMemory / (1024.0 * 1024.0);
    DEBUG_PRINT("Ring buffer size: " << MAX_CACHED_FRAMES << " frames (~" << memoryMB << " MB)");

    // Start background decoder thread
    shouldStopDecoder = false;
    decoderThread = std::thread(&VideoPlayer::backgroundDecoderTask, this);

    loaded = true;
    DEBUG_PRINT("Video loaded successfully (on-demand decoding enabled)");
    return true;
}

void VideoPlayer::play() {
    if (!loaded) return;
    playing = true;
    lastFrameTime = std::chrono::steady_clock::now();
    DEBUG_PRINT("Playing");
}

void VideoPlayer::pause() {
    playing = false;
    DEBUG_PRINT("Paused");
}

void VideoPlayer::stop() {
    playing = false;
    currentFrameIndex = 0;
    DEBUG_PRINT("Stopped");
}

void VideoPlayer::seek(double seconds) {
    if (!loaded || totalFrames == 0) return;

    int targetFrame = (int)(seconds * fps);
    currentFrameIndex = std::max(0, std::min(targetFrame, totalFrames - 1));
    lastFrameTime = std::chrono::steady_clock::now();

    DEBUG_PRINT("Seeked to " << seconds << "s (frame " << currentFrameIndex << ")");
}

void VideoPlayer::syncToTimestamp(double audioTimestamp) {
    if (!loaded || totalFrames == 0 || !playing) return;

    // Calculate target frame from audio timestamp
    // Handle looping: take modulo of video duration
    double loopedTime = std::fmod(audioTimestamp, duration);
    if (loopedTime < 0) loopedTime += duration;

    int targetFrame = (int)(loopedTime * fps);

    // Clamp to valid range
    targetFrame = std::max(0, std::min(targetFrame, totalFrames - 1));

    // Update frame index directly - no accumulation, no drift!
    currentFrameIndex.store(targetFrame, std::memory_order_relaxed);

    // Mark external sync as active and update timestamp
    externalSyncActive.store(true, std::memory_order_relaxed);
    lastSyncTime = std::chrono::steady_clock::now();
}

const VideoFrame* VideoPlayer::getCurrentFrame() {
    if (!loaded) return nullptr;

    int frameIndex = currentFrameIndex.load(std::memory_order_relaxed);
    ensureFrameLoaded(frameIndex);

    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = frameCache.find(frameIndex);
    if (it != frameCache.end()) {
        return &it->second;
    }

    return nullptr;  // Frame not available yet (shouldn't happen if ensureFrameLoaded works)
}

void VideoPlayer::update() {
    if (!playing || !loaded || totalFrames == 0) return;

    // Check if external sync is active (receiving SYNC messages at 1kHz)
    if (externalSyncActive.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        auto timeSinceLastSync = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSyncTime);

        // If we've received SYNC within last 100ms, external clock is driving - do nothing
        if (timeSinceLastSync.count() < 100) {
            return; // External sync active - skip internal timer
        }

        // External sync timed out - fall back to internal timer
        DEBUG_PRINT("External sync timeout - falling back to internal timer");
        externalSyncActive.store(false, std::memory_order_relaxed);
        lastFrameTime = now; // Reset timer
    }

    // Fallback: timer-based frame advancement (when no external sync)
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastFrameTime);

    // Advance frames based on elapsed time
    while (elapsed >= frameDuration) {
        currentFrameIndex++;

        // Loop back to start
        if (currentFrameIndex >= totalFrames) {
            currentFrameIndex = 0;
        }

        elapsed -= frameDuration;
        lastFrameTime += frameDuration;
    }
}
// Decode a single frame at the specified index
bool VideoPlayer::decodeFrame(int frameIndex) {
    if (frameIndex < 0 || frameIndex >= totalFrames) return false;

    // Check if already cached
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (frameCache.find(frameIndex) != frameCache.end()) {
            return true;  // Already decoded
        }
    }

    // Seek to the frame's timestamp
    int64_t timestamp = (int64_t)(frameIndex / fps * AV_TIME_BASE);
    if (av_seek_frame(formatContext, -1, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }

    avcodec_flush_buffers(codecContext);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    bool frameDecoded = false;
    int64_t targetPts = av_rescale_q(frameIndex,
                                     AVRational{1, (int)fps},
                                     formatContext->streams[videoStreamIndex]->time_base);

    // Read packets until we find our frame
    while (av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(codecContext, packet) >= 0) {
                while (avcodec_receive_frame(codecContext, frame) >= 0) {
                    int64_t framePts = frame->best_effort_timestamp;

                    // Check if this is close to our target frame
                    if (std::abs(framePts - targetPts) < fps / 2) {
                        // This is our frame! Convert to RGB24
                        VideoFrame vf;
                        vf.width = width;
                        vf.height = height;
                        vf.linesize = width * 3;
                        vf.data.resize(width * height * 3);

                        uint8_t* dest[1] = { vf.data.data() };
                        int destLinesize[1] = { vf.linesize };

                        sws_scale(swsContext,
                                 frame->data, frame->linesize, 0, height,
                                 dest, destLinesize);

                        // Add to cache
                        {
                            std::lock_guard<std::mutex> lock(cacheMutex);
                            frameCache[frameIndex] = std::move(vf);
                            cacheOrder.push_back(frameIndex);
                            evictOldFrames();
                        }

                        frameDecoded = true;
                        break;
                    }
                }
            }
        }
        av_packet_unref(packet);

        if (frameDecoded) break;
    }

    av_frame_free(&frame);
    av_packet_free(&packet);

    return frameDecoded;
}

// Ensure a frame is loaded (blocking if necessary)
void VideoPlayer::ensureFrameLoaded(int frameIndex) {
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (frameCache.find(frameIndex) != frameCache.end()) {
            return;  // Already loaded
        }
    }

    // Frame not in cache - decode it now (blocking)
    decodeFrame(frameIndex);
}

// Evict old frames if cache is too large (LRU)
void VideoPlayer::evictOldFrames() {
    // Must be called with cacheMutex locked
    while (frameCache.size() > MAX_CACHED_FRAMES) {
        if (cacheOrder.empty()) break;

        int oldestFrame = cacheOrder.front();
        cacheOrder.pop_front();
        frameCache.erase(oldestFrame);
    }
}

// Background decoder thread - keeps buffer filled ahead of playback
void VideoPlayer::backgroundDecoderTask() {
    DEBUG_PRINT("Background decoder thread started");

    while (!shouldStopDecoder) {
        if (!playing || !loaded) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int currentFrame = currentFrameIndex.load(std::memory_order_relaxed);

        // Decode ahead by 200 frames (~8 seconds @ 24fps)
        const int LOOKAHEAD = 200;

        bool allLoaded = true;
        for (int offset = 0; offset < LOOKAHEAD; offset++) {
            int targetFrame = (currentFrame + offset) % totalFrames;

            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                if (frameCache.find(targetFrame) != frameCache.end()) {
                    continue;  // Already cached
                }
            }

            // Decode this frame
            if (decodeFrame(targetFrame)) {
                allLoaded = false;
                lastDecodedFrame = targetFrame;
            }

            // Don't hog CPU - yield after each decode
            std::this_thread::sleep_for(std::chrono::microseconds(100));

            if (shouldStopDecoder) break;
        }

        // Loop-aware pre-fetching: if we're near the end, also load beginning frames
        if (currentFrame > (totalFrames - 150)) {
            for (int i = 0; i < 150 && i < totalFrames; i++) {
                {
                    std::lock_guard<std::mutex> lock(cacheMutex);
                    if (frameCache.find(i) != frameCache.end()) {
                        continue;  // Already cached
                    }
                }

                decodeFrame(i);

                if (shouldStopDecoder) break;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        // If everything in lookahead is loaded, sleep a bit longer
        if (allLoaded) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    DEBUG_PRINT("Background decoder thread stopped");
}