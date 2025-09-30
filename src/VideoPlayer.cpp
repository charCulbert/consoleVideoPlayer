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

    // Frame not in cache yet - return closest available frame to avoid blank screen
    // Try nearby frames (decoder might be slightly behind)
    for (int offset = -5; offset <= 5; offset++) {
        int nearbyFrame = frameIndex + offset;
        if (nearbyFrame >= 0 && nearbyFrame < totalFrames) {
            auto nearIt = frameCache.find(nearbyFrame);
            if (nearIt != frameCache.end()) {
                return &nearIt->second;
            }
        }
    }

    return nullptr;  // No frames available at all
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

    // Lock FFmpeg contexts (NOT thread-safe!)
    std::lock_guard<std::mutex> decoderLock(decoderMutex);

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

    // Safety limit: don't read more than 2 seconds worth of frames
    int maxFramesToRead = (int)(fps * 2);
    int framesRead = 0;

    // Read packets until we find our frame
    while (av_read_frame(formatContext, packet) >= 0 && framesRead < maxFramesToRead) {
        if (packet->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(codecContext, packet) >= 0) {
                while (avcodec_receive_frame(codecContext, frame) >= 0) {
                    int64_t framePts = frame->best_effort_timestamp;
                    framesRead++;

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

// Ensure a frame is loaded (non-blocking - background thread will handle it)
void VideoPlayer::ensureFrameLoaded(int frameIndex) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (frameCache.find(frameIndex) != frameCache.end()) {
        return;  // Already loaded
    }

    // Not in cache - background decoder thread will fetch it
    // Don't block the render thread!
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

// Background decoder thread - sequential decode ahead of playback
void VideoPlayer::backgroundDecoderTask() {
    DEBUG_PRINT("Background decoder thread started");

    std::lock_guard<std::mutex> decoderLock(decoderMutex);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int numBytes = width * height * 3;

    int sequentialFrameIndex = 0;  // Start from beginning
    bool needSeek = true;
    int lastPlaybackFrame = 0;

    while (!shouldStopDecoder) {
        if (!playing || !loaded) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int currentFrame = currentFrameIndex.load(std::memory_order_relaxed);

        // Keep decoder just 50 frames ahead of playback
        const int DECODE_AHEAD = 50;

        // If playback jumped (seek/loop), or decoder fell too far behind
        if (currentFrame < lastPlaybackFrame - 10 || currentFrame > lastPlaybackFrame + 200) {
            // Jump detected - seek decoder to just behind current position
            sequentialFrameIndex = currentFrame - 10;
            if (sequentialFrameIndex < 0) sequentialFrameIndex = 0;
            needSeek = true;
        }
        lastPlaybackFrame = currentFrame;

        // If we're already too far ahead, wait
        if (sequentialFrameIndex > currentFrame + DECODE_AHEAD) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // Check if already cached
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            if (frameCache.find(sequentialFrameIndex) != frameCache.end()) {
                sequentialFrameIndex++;
                if (sequentialFrameIndex >= totalFrames) {
                    sequentialFrameIndex = 0;
                    needSeek = true;
                }
                continue;
            }
        }

        // Seek if needed
        if (needSeek) {
            int64_t timestamp = (int64_t)(sequentialFrameIndex / fps * AV_TIME_BASE);
            av_seek_frame(formatContext, -1, timestamp, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(codecContext);
            needSeek = false;
        }

        // Decode next frame sequentially
        bool frameDecoded = false;
        if (av_read_frame(formatContext, packet) >= 0) {
            if (packet->stream_index == videoStreamIndex) {
                if (avcodec_send_packet(codecContext, packet) >= 0) {
                    if (avcodec_receive_frame(codecContext, frame) >= 0) {
                        // Convert to RGB24
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
                            frameCache[sequentialFrameIndex] = std::move(vf);
                            cacheOrder.push_back(sequentialFrameIndex);
                            evictOldFrames();
                        }

                        frameDecoded = true;

                        // Log progress every 50 frames
                        if (sequentialFrameIndex % 50 == 0) {
                            DEBUG_PRINT("Decoded frame " << sequentialFrameIndex << "/" << totalFrames <<
                                       " (cache: " << frameCache.size() << " frames)");
                        }

                        sequentialFrameIndex++;

                        if (sequentialFrameIndex >= totalFrames) {
                            sequentialFrameIndex = 0;
                            needSeek = true;
                        }
                    }
                }
            }
            av_packet_unref(packet);
        } else {
            // EOF - wrap to beginning
            sequentialFrameIndex = 0;
            needSeek = true;
        }

        if (!frameDecoded) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    av_frame_free(&frame);
    av_packet_free(&packet);

    DEBUG_PRINT("Background decoder thread stopped");
}