#include "VideoPlayer.h"
#include <iostream>
#include <cstring>

#define DEBUG_PRINT(msg) do { \
    std::cout << "[VideoPlayer] " << msg << std::endl; \
    std::cout.flush(); \
} while(0)

VideoPlayer::~VideoPlayer() {
    frames.clear();
}

bool VideoPlayer::loadVideo(const std::string& filePath) {
    DEBUG_PRINT("Loading video: " << filePath);

    AVFormatContext* formatContext = nullptr;
    if (avformat_open_input(&formatContext, filePath.c_str(), nullptr, nullptr) < 0) {
        errorMessage = "Failed to open video file";
        return false;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        errorMessage = "Failed to find stream info";
        avformat_close_input(&formatContext);
        return false;
    }

    // Find video stream
    int videoStreamIndex = -1;
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
        avformat_close_input(&formatContext);
        return false;
    }

    // Get FPS
    AVRational frameRate = formatContext->streams[videoStreamIndex]->avg_frame_rate;
    fps = (double)frameRate.num / (double)frameRate.den;
    if (fps <= 0) fps = 25.0; // Default fallback

    frameDuration = std::chrono::microseconds((int64_t)(1000000.0 / fps));

    // Get duration
    duration = (double)formatContext->duration / AV_TIME_BASE;

    DEBUG_PRINT("Video info: " << codecParams->width << "x" << codecParams->height
                << " @ " << fps << " fps, duration: " << duration << "s");

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        errorMessage = "Codec not found";
        avformat_close_input(&formatContext);
        return false;
    }

    AVCodecContext* codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        errorMessage = "Failed to allocate codec context";
        avformat_close_input(&formatContext);
        return false;
    }

    if (avcodec_parameters_to_context(codecContext, codecParams) < 0) {
        errorMessage = "Failed to copy codec parameters";
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        errorMessage = "Failed to open codec";
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    width = codecContext->width;
    height = codecContext->height;

    // Setup scaler to convert to RGB24
    SwsContext* swsContext = sws_getContext(
        width, height, codecContext->pix_fmt,
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!swsContext) {
        errorMessage = "Failed to create scaler context";
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    // Decode all frames
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* frameRGB = av_frame_alloc();

    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);

    DEBUG_PRINT("Starting frame decode...");
    int frameCount = 0;

    while (av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(codecContext, packet) >= 0) {
                while (avcodec_receive_frame(codecContext, frame) >= 0) {
                    // Allocate RGB frame buffer
                    VideoFrame vf;
                    vf.width = width;
                    vf.height = height;
                    vf.linesize = width * 3;
                    vf.data.resize(numBytes);

                    uint8_t* dest[1] = { vf.data.data() };
                    int destLinesize[1] = { vf.linesize };

                    // Convert to RGB24
                    sws_scale(swsContext,
                             frame->data, frame->linesize, 0, height,
                             dest, destLinesize);

                    frames.push_back(std::move(vf));
                    frameCount++;

                    if (frameCount % 100 == 0) {
                        DEBUG_PRINT("Decoded " << frameCount << " frames...");
                    }
                }
            }
        }
        av_packet_unref(packet);
    }

    // Flush decoder
    avcodec_send_packet(codecContext, nullptr);
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

        frames.push_back(std::move(vf));
        frameCount++;
    }

    DEBUG_PRINT("Decoded total " << frameCount << " frames");

    // Calculate memory usage
    size_t totalMemory = frameCount * numBytes;
    double memoryMB = (double)totalMemory / (1024.0 * 1024.0);
    DEBUG_PRINT("Video memory usage: " << memoryMB << " MB");

    // Cleanup
    av_frame_free(&frameRGB);
    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(swsContext);
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);

    loaded = true;
    DEBUG_PRINT("Video loaded successfully");
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
    if (!loaded || frames.empty()) return;

    int targetFrame = (int)(seconds * fps);
    currentFrameIndex = std::max(0, std::min(targetFrame, (int)frames.size() - 1));
    lastFrameTime = std::chrono::steady_clock::now();

    DEBUG_PRINT("Seeked to " << seconds << "s (frame " << currentFrameIndex << ")");
}

const VideoFrame* VideoPlayer::getCurrentFrame() {
    if (!loaded || frames.empty()) return nullptr;
    return &frames[currentFrameIndex];
}

void VideoPlayer::update() {
    if (!playing || !loaded || frames.empty()) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastFrameTime);

    // Advance frames based on elapsed time
    while (elapsed >= frameDuration) {
        currentFrameIndex++;

        // Loop back to start
        if (currentFrameIndex >= (int)frames.size()) {
            currentFrameIndex = 0;
        }

        elapsed -= frameDuration;
        lastFrameTime += frameDuration;
    }
}