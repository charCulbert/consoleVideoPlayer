/*
 * VideoPlayer.cpp - JACK-synced video player with seamless looping
 *
 * Key features:
 * - Background decoder continuously decodes ahead of playback position
 * - Wraps at loop boundaries automatically (handles positive/negative sync offsets)
 * - LRU frame cache (300 frames ~600MB for 720p)
 * - Pre-loads first 150 frames for instant startup and smooth loop point
 */

#include "VideoPlayer.h"
#include <iostream>
#include <iomanip>
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

    // Find decoder - try hardware first, fallback to software
    const AVCodec* codec = nullptr;
    const char* hwDecoderName = nullptr;

    // Determine hardware decoder based on codec and platform
    if (codecParams->codec_id == AV_CODEC_ID_H264) {
        #ifdef __APPLE__
            hwDecoderName = "h264_videotoolbox";
        #else
            hwDecoderName = "h264_vaapi";  // VA-API (Intel/AMD on Linux)
        #endif
    } else if (codecParams->codec_id == AV_CODEC_ID_HEVC) {
        #ifdef __APPLE__
            hwDecoderName = "hevc_videotoolbox";
        #else
            hwDecoderName = "hevc_vaapi";
        #endif
    }

    // Try hardware decoder
    if (hwDecoderName) {
        codec = avcodec_find_decoder_by_name(hwDecoderName);
    }

    // Fallback to software decoder
    if (!codec) {
        codec = avcodec_find_decoder(codecParams->codec_id);
    }

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

    // Try to open codec (hardware might fail, so fallback to software)
    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        if (hwDecoderName) {
            avcodec_free_context(&codecContext);

            // Retry with software decoder
            codec = avcodec_find_decoder(codecParams->codec_id);
            if (!codec) {
                errorMessage = "Codec not found";
                closeFFmpegContexts();
                return false;
            }

            codecContext = avcodec_alloc_context3(codec);
            if (!codecContext || avcodec_parameters_to_context(codecContext, codecParams) < 0) {
                errorMessage = "Failed to allocate codec context";
                closeFFmpegContexts();
                return false;
            }

            codecContext->thread_count = 0;
            if (avcodec_open2(codecContext, codec, nullptr) < 0) {
                errorMessage = "Failed to open codec";
                closeFFmpegContexts();
                return false;
            }
        } else {
            errorMessage = "Failed to open codec";
            closeFFmpegContexts();
            return false;
        }
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

    // Pre-load first frames for smooth startup and seamless looping
    int maxPreload = std::min(PRELOAD_FRAMES, totalFrames);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int frameCount = 0;

    av_seek_frame(formatContext, -1, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codecContext);

    // Sequential decode (much faster than per-frame seeking)
    while (av_read_frame(formatContext, packet) >= 0 && frameCount < maxPreload) {
        if (packet->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(codecContext, packet) >= 0) {
                while (avcodec_receive_frame(codecContext, frame) >= 0) {
                    VideoFrame vf;
                    vf.width = width;
                    vf.height = height;
                    vf.linesize = width * 3;
                    vf.data.resize(width * height * 3);

                    uint8_t* dest[1] = { vf.data.data() };
                    int destLinesize[1] = { vf.linesize };
                    sws_scale(swsContext, frame->data, frame->linesize, 0, height, dest, destLinesize);

                    std::lock_guard<std::mutex> lock(cacheMutex);
                    frameCache[frameCount] = std::move(vf);
                    cacheOrder.push_back(frameCount);

                    if (++frameCount >= maxPreload) break;
                }
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);

    // Start background decoder thread
    shouldStopDecoder = false;
    decoderThread = std::thread(&VideoPlayer::backgroundDecoderTask, this);

    loaded = true;
    return true;
}

void VideoPlayer::play() {
    if (!loaded) return;
    playing = true;
    lastFrameTime = std::chrono::steady_clock::now();
}

void VideoPlayer::pause() {
    playing = false;
}

void VideoPlayer::stop() {
    playing = false;
    currentFrameIndex = 0;
}

void VideoPlayer::seek(double seconds) {
    if (!loaded || totalFrames == 0) return;

    int targetFrame = (int)(seconds * fps);
    currentFrameIndex = wrapFrameIndex(targetFrame);
    lastFrameTime = std::chrono::steady_clock::now();
}

void VideoPlayer::syncToTimestamp(double audioTimestamp) {
    if (!loaded || totalFrames == 0) return;

    // Set frame index to exact JACK timecode (even when paused!)
    int targetFrame = (int)(audioTimestamp * fps);

    currentFrameIndex.store(targetFrame, std::memory_order_relaxed);
    externalSyncActive.store(true, std::memory_order_relaxed);
    lastSyncTime = std::chrono::steady_clock::now();
}

const VideoFrame* VideoPlayer::getCurrentFrame() {
    if (!loaded) return nullptr;

    int requestedFrame = currentFrameIndex.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(cacheMutex);

    // Is requested frame in cache?
    auto it = frameCache.find(requestedFrame);
    if (it != frameCache.end()) {
        // Frame found! Remember it as last valid
        lastValidFrameIndex = requestedFrame;
        return &it->second;
    }

    // Frame not in cache - hold last valid frame
    if (lastValidFrameIndex >= 0) {
        auto lastIt = frameCache.find(lastValidFrameIndex);
        if (lastIt != frameCache.end()) {
            return &lastIt->second;
        }
    }

    // No frame available
    return nullptr;
}

int VideoPlayer::getBufferedFrameCount(int startFrame, int maxCheck) {
    std::lock_guard<std::mutex> lock(cacheMutex);

    int count = 0;
    for (int i = 0; i < maxCheck; i++) {
        // Handle wrap-around properly
        int frameIdx = wrapFrameIndex(startFrame + i);

        if (frameCache.find(frameIdx) != frameCache.end()) {
            count++;
        } else {
            break;  // Stop at first missing frame
        }
    }
    return count;
}

void VideoPlayer::update() {
    if (!playing || !loaded || totalFrames == 0) return;

    // External sync active? Check if still receiving sync messages
    if (externalSyncActive.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        auto timeSinceSync = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSyncTime);

        if (timeSinceSync.count() < 100) return;  // Still synced, nothing to do

        // Sync lost - fall back to internal timer
        externalSyncActive.store(false, std::memory_order_relaxed);
        lastFrameTime = now;
    }

    // Internal timer-based advancement (fallback when no external sync)
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastFrameTime);

    while (elapsed >= frameDuration) {
        currentFrameIndex = wrapFrameIndex(currentFrameIndex + 1);
        elapsed -= frameDuration;
        lastFrameTime += frameDuration;
    }
}
// Wrap frame index to valid range [0, totalFrames)
int VideoPlayer::wrapFrameIndex(int frameIndex) const {
    if (totalFrames == 0) return 0;
    frameIndex %= totalFrames;
    if (frameIndex < 0) frameIndex += totalFrames;
    return frameIndex;
}

// Calculate signed circular distance from->to (handles wraparound)
// Returns: positive if 'to' is ahead of 'from', negative if behind
int VideoPlayer::circularDistance(int from, int to) const {
    if (totalFrames == 0) return 0;
    int distance = to - from;

    // Handle wraparound: choose shortest path
    if (distance > totalFrames / 2) {
        distance -= totalFrames;
    } else if (distance < -totalFrames / 2) {
        distance += totalFrames;
    }

    return distance;
}

// Evict frames that are behind playback (streaming buffer - pop as we pass them)
void VideoPlayer::evictOldFrames() {
    // Must be called with cacheMutex locked
    int playbackPos = currentFrameIndex.load(std::memory_order_relaxed);

    // Remove ANY frames behind playback (we'll never show them again)
    auto it = cacheOrder.begin();
    while (it != cacheOrder.end()) {
        int frameIdx = *it;
        int distance = frameIdx - playbackPos;

        // Handle wraparound
        if (distance < -totalFrames / 2) distance += totalFrames;
        else if (distance > totalFrames / 2) distance -= totalFrames;

        // If frame is behind playback (even by 1), evict it
        if (distance < 0) {
            frameCache.erase(frameIdx);
            it = cacheOrder.erase(it);
        } else {
            ++it;
        }
    }

    // Safety: if somehow still over limit, evict oldest
    while (frameCache.size() > MAX_CACHED_FRAMES && !cacheOrder.empty()) {
        int oldestFrame = cacheOrder.front();
        cacheOrder.pop_front();
        frameCache.erase(oldestFrame);
    }
}

// Background decoder: continuously decode ahead of playback for smooth rendering
void VideoPlayer::backgroundDecoderTask() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return;
    }

    int decoderPos = 0;
    bool needSeek = true;

    while (!shouldStopDecoder) {
        if (!loaded) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // What frame does playback need?
        int playbackPos = currentFrameIndex.load(std::memory_order_relaxed);
        int decodeAhead = playing ? DECODE_AHEAD_FRAMES : 20;

        // Is decoder in the useful range [playback, playback + decodeAhead]?
        // Check this FIRST so seeking while paused works
        int distance = circularDistance(decoderPos, playbackPos);
        // Only seek if decoder is way behind (>50) or way too far ahead (> decodeAhead + 50)
        if (distance > 50 || distance < -(decodeAhead + 50)) {
            // Out of range - seek to playback position
            decoderPos = playbackPos;
            needSeek = true;
        }

        // Are we buffered enough from playback position? If so, wait
        int bufferedCount = 0;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            for (int i = 0; i < decodeAhead; i++) {
                int checkFrame = wrapFrameIndex(playbackPos + i);
                if (frameCache.find(checkFrame) != frameCache.end()) {
                    bufferedCount++;
                } else {
                    break;
                }
            }
        }

        if (bufferedCount >= decodeAhead) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Is current decoder frame already cached? Skip it
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            if (frameCache.find(decoderPos) != frameCache.end()) {
                decoderPos = wrapFrameIndex(decoderPos + 1);
                if (decoderPos == 0) needSeek = true;  // Wrapped around
                continue;
            }
        }

        // Decode the frame at decoderPos
        bool decoded = false;
        {
            std::lock_guard<std::mutex> decoderLock(decoderMutex);
            if (shouldStopDecoder) break;

            if (needSeek) {
                int64_t timestamp = (int64_t)(decoderPos / fps * AV_TIME_BASE);
                av_seek_frame(formatContext, -1, timestamp, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(codecContext);
                needSeek = false;
            }

            if (av_read_frame(formatContext, packet) >= 0) {
                if (packet->stream_index == videoStreamIndex) {
                    if (avcodec_send_packet(codecContext, packet) >= 0) {
                        if (avcodec_receive_frame(codecContext, frame) >= 0) {
                            VideoFrame vf;
                            vf.width = width;
                            vf.height = height;
                            vf.linesize = width * 3;
                            vf.data.resize(width * height * 3);

                            uint8_t* dest[1] = { vf.data.data() };
                            int destLinesize[1] = { vf.linesize };
                            sws_scale(swsContext, frame->data, frame->linesize, 0, height, dest, destLinesize);

                            decoded = true;
                            int currentDecoderPos = decoderPos;

                            {
                                std::lock_guard<std::mutex> lock(cacheMutex);
                                frameCache[currentDecoderPos] = std::move(vf);
                                cacheOrder.push_back(currentDecoderPos);
                                evictOldFrames();
                            }

                            decoderPos = wrapFrameIndex(decoderPos + 1);
                            if (decoderPos == 0) {
                                needSeek = true;
                            }
                        }
                    }
                }
                av_packet_unref(packet);
            } else {
                // EOF - wrap
                decoderPos = 0;
                needSeek = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        if (!decoded) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
}
