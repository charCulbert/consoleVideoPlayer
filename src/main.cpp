#include <iostream>
#include <iomanip>
#include <filesystem>
#include <cmath>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "VideoPlayer.h"
#include "JackTransportClient.h"
#include "Overlay.h"

// PBOs removed - they add 1-frame latency. Direct texture upload is fast enough.

// Simple JSON parser for config (minimal implementation)
#include <fstream>
#include <sstream>
#include <map>

// Signal handler for debugging
void signal_handler(int sig) {
    void *array[20];
    size_t size;
    std::cerr << "Error: signal " << sig << " caught" << std::endl;
    size = backtrace(array, 20);
    std::cerr << "Stack trace:" << std::endl;
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}

#define DEBUG_PRINT(msg) do { \
    std::cout << "[DEBUG " << __FILE__ << ":" << __LINE__ << "] " << msg << std::endl; \
    std::cout.flush(); \
} while(0)

struct Settings {
    std::string videoFilePath;
    double syncOffsetMs = 0.0;
    bool fullscreen = false;
    std::string scaleMode = "letterbox";  // Options: "letterbox", "stretch", "crop"
};

void showHelp(const char* programName) {
    std::cout << "Usage: " << programName << " <video_file> [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -o, --offset <ms>       Sync offset in milliseconds (default: 0.0)\n";
    std::cout << "                          Positive = delay video (video plays later)\n";
    std::cout << "                          Negative = advance video (video plays earlier)\n";
    std::cout << "                          Example: -o 15.5 or --offset -10.0\n\n";
    std::cout << "  -f, --fullscreen        Enable fullscreen mode (default: windowed)\n\n";
    std::cout << "  -s, --scale <mode>      Video scaling mode (default: letterbox)\n";
    std::cout << "                          letterbox - fit inside, preserve aspect, add bars\n";
    std::cout << "                          stretch   - fill window, ignore aspect ratio\n";
    std::cout << "                          crop      - fill window, preserve aspect, crop edges\n\n";
    std::cout << "  -h, --help              Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << programName << " video.mp4\n";
    std::cout << "  " << programName << " video.mp4 --offset 15.0 --fullscreen\n";
    std::cout << "  " << programName << " video.mp4 -o -10.5 -f -s stretch\n";
}

Settings parseCommandLine(int argc, char* argv[]) {
    Settings settings;

    // Check for help first
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "-h" || std::string(argv[i]) == "--help") {
            showHelp(argv[0]);
            exit(0);
        }
    }

    // Require at least video file
    if (argc < 2) {
        std::cerr << "Error: No video file specified\n\n";
        showHelp(argv[0]);
        exit(1);
    }

    settings.videoFilePath = argv[1];

    // Parse optional flags
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-o" || arg == "--offset") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a value\n";
                exit(1);
            }
            try {
                settings.syncOffsetMs = std::stod(argv[++i]);
            } catch (...) {
                std::cerr << "Error: Invalid offset value: " << argv[i] << "\n";
                exit(1);
            }
        } else if (arg == "-f" || arg == "--fullscreen") {
            settings.fullscreen = true;
        } else if (arg == "-s" || arg == "--scale") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a value\n";
                exit(1);
            }
            settings.scaleMode = argv[++i];
            if (settings.scaleMode != "letterbox" &&
                settings.scaleMode != "stretch" &&
                settings.scaleMode != "crop") {
                std::cerr << "Error: Invalid scale mode: " << settings.scaleMode << "\n";
                std::cerr << "Valid modes: letterbox, stretch, crop\n";
                exit(1);
            }
        } else {
            std::cerr << "Error: Unknown option: " << arg << "\n\n";
            showHelp(argv[0]);
            exit(1);
        }
    }

    return settings;
}

// Global video player for UDP callback
// UDP-based command handling (replaced by JACK Transport)
// VideoPlayer* g_videoPlayer = nullptr;
// void handleCommand(const std::string& command) { ... }

int main(int argc, char* argv[]) {
    // Install signal handlers
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);

    std::cout << "Console Video Player (JACK Sync)" << std::endl;
    std::cout << "=================================" << std::endl;

    auto settings = parseCommandLine(argc, argv);

    // Check if video file exists
    if (!std::filesystem::exists(settings.videoFilePath)) {
        std::cerr << "Error: Video file not found at " << settings.videoFilePath << std::endl;
        return 1;
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    // Set OpenGL attributes
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Create window
    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS;
    int windowWidth = 1280;
    int windowHeight = 720;

    if (settings.fullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        // Query actual display size
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
            windowWidth = dm.w;
            windowHeight = dm.h;
            std::cout << "Fullscreen: " << windowWidth << "x" << windowHeight << std::endl;
        } else {
            windowWidth = 1920;
            windowHeight = 1080;
        }
    }

    SDL_Window* window = SDL_CreateWindow(
        "Console Video Player",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        windowWidth, windowHeight,
        windowFlags
    );

    if (!window) {
        std::cerr << "Window creation failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    // Create OpenGL context
    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        std::cerr << "OpenGL context creation failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize SDL_ttf
    if (TTF_Init() < 0) {
        std::cerr << "TTF initialization failed: " << TTF_GetError() << std::endl;
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize overlay
    Overlay overlay;
    if (!overlay.init("/System/Library/Fonts/Courier.ttc", 24)) {
        std::cerr << "Warning: Could not load font, overlay disabled" << std::endl;
    }

    // PBOs removed for lower latency (direct upload is fast enough for HD video)

    // Enable vsync
    SDL_GL_SetSwapInterval(1);

    // Dropped frames counter (only when decoder fails to provide requested frame)
    int droppedFrames = 0;

    // Get actual window size
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    // Load video
    VideoPlayer videoPlayer;

    if (!videoPlayer.loadVideo(settings.videoFilePath)) {
        std::cerr << "Failed to load video: " << videoPlayer.getErrorMessage() << std::endl;
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::cout << "Video: " << videoPlayer.getWidth() << "x" << videoPlayer.getHeight()
              << " @ " << videoPlayer.getFPS() << " fps (" << videoPlayer.getDuration() << "s)" << std::endl;

    // Setup OpenGL texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Direct texture uploads (PBOs removed to eliminate 1-frame latency)

    // Setup OpenGL viewport
    glViewport(0, 0, windowWidth, windowHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, windowWidth, windowHeight, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    // Initialize JACK Transport client
    JackTransportClient jackTransport("consoleVideoPlayer");
    if (!jackTransport.isInitialized()) {
        std::cerr << "Failed to initialize JACK Transport: " << jackTransport.getErrorMessage() << std::endl;
        std::cerr << "Make sure JACK server is running (try: jackd -d alsa -r 48000)" << std::endl;
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    jack_nframes_t jackSampleRate = jackTransport.getSampleRate();
    double fps = videoPlayer.getFPS();

    std::cout << "✓ JACK Transport synced (" << jackSampleRate << " Hz)" << std::endl;

    if (settings.syncOffsetMs != 0.0) {
        std::cout << "✓ Sync offset: " << std::fixed << std::setprecision(1)
                  << settings.syncOffsetMs << " ms "
                  << (settings.syncOffsetMs > 0 ? "(video delayed)" : "(video advanced)") << std::endl;
    }

    std::cout << "\nReady. Waiting for JACK Transport... (ESC or Q to quit)\n" << std::endl;

    // Main render loop
    bool running = true;
    SDL_Event event;

    while (running) {
        // Handle events
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                    running = false;
                } else if (event.key.keysym.sym == SDLK_i) {
                    overlay.toggle();
                    std::cout << "Overlay " << (overlay.isEnabled() ? "ON" : "OFF") << std::endl;
                }
            }
        }

        // Simple deterministic logic:
        // 1. Get time from JACK
        // 2. Apply offset (wrapping handled by syncToTimestamp)
        // 3. Sync video to that time (sets target frame, decoder follows)
        // 4. Get frame (exact if ready, nearby if not)
        // 5. Upload and render

        // Sync play/pause state
        if (jackTransport.isTransportRolling() && !videoPlayer.isPlaying()) {
            videoPlayer.play();
        } else if (!jackTransport.isTransportRolling() && videoPlayer.isPlaying()) {
            videoPlayer.pause();
        }

        // Step 1: Get JACK time and CLAMP to file duration (hold last frame if past end)
        jack_nframes_t jackFrame = jackTransport.getCurrentFrame();
        double jackTime = (double)jackFrame / jackSampleRate;
        double duration = videoPlayer.getDuration();

        // Clamp JACK time to [0, duration]
        double clampedJackTime = jackTime;
        if (clampedJackTime < 0) clampedJackTime = 0;
        if (clampedJackTime > duration) clampedJackTime = duration;

        // Step 2: Apply offset
        double offsetSec = settings.syncOffsetMs / 1000.0;
        double videoTime = clampedJackTime - offsetSec;  // positive offset = delay video, negative = advance

        // Step 3: Wrap ONLY if offset made it negative (wraps to end of video)
        if (videoTime < 0) {
            videoTime = fmod(videoTime, duration);
            if (videoTime < 0) videoTime += duration;
        }
        // If videoTime > duration, clamp to last frame
        if (videoTime > duration) {
            videoTime = duration - 0.001;  // Just before end
        }

        // Step 2: Convert to frame number
        int targetFrame = (int)(videoTime * videoPlayer.getFPS());
        if (targetFrame >= videoPlayer.getFrameCount()) targetFrame = videoPlayer.getFrameCount() - 1;
        if (targetFrame < 0) targetFrame = 0;

        // Step 3: Set playback position
        videoPlayer.syncToTimestamp(videoTime);

        // Step 4: Get frame from cache (getCurrentFrame handles holding last valid frame)
        const VideoFrame* frame = videoPlayer.getCurrentFrame();

        // Count dropped frames only when decoder fails to provide the requested frame
        if (!frame) {
            droppedFrames++;
        }

        // Upload directly to GPU (no PBO delay)
        if (frame) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame->width, frame->height,
                        0, GL_RGB, GL_UNSIGNED_BYTE, frame->data.data());

            // Clear and render
            glClear(GL_COLOR_BUFFER_BIT);

            // Calculate rendering dimensions based on scale mode
            float videoAspect = (float)frame->width / (float)frame->height;
            float windowAspect = (float)windowWidth / (float)windowHeight;

            float renderWidth, renderHeight;
            float offsetX = 0, offsetY = 0;

            if (settings.scaleMode == "stretch") {
                // Stretch to fill - ignore aspect ratio
                renderWidth = windowWidth;
                renderHeight = windowHeight;
            }
            else if (settings.scaleMode == "crop") {
                // Fill window, preserve aspect, crop edges (fit smallest dimension)
                if (windowAspect > videoAspect) {
                    // Window wider - fit width, crop top/bottom
                    renderWidth = windowWidth;
                    renderHeight = windowWidth / videoAspect;
                    offsetY = (windowHeight - renderHeight) / 2.0f;
                } else {
                    // Window taller - fit height, crop sides
                    renderHeight = windowHeight;
                    renderWidth = windowHeight * videoAspect;
                    offsetX = (windowWidth - renderWidth) / 2.0f;
                }
            }
            else {
                // Default: "letterbox" - fit inside, preserve aspect (fit largest dimension)
                if (windowAspect > videoAspect) {
                    // Window wider than video - letterbox sides
                    renderHeight = windowHeight;
                    renderWidth = windowHeight * videoAspect;
                    offsetX = (windowWidth - renderWidth) / 2.0f;
                } else {
                    // Window taller than video - letterbox top/bottom
                    renderWidth = windowWidth;
                    renderHeight = windowWidth / videoAspect;
                    offsetY = (windowHeight - renderHeight) / 2.0f;
                }
            }

            // Draw textured quad
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(offsetX, offsetY);
            glTexCoord2f(1, 0); glVertex2f(offsetX + renderWidth, offsetY);
            glTexCoord2f(1, 1); glVertex2f(offsetX + renderWidth, offsetY + renderHeight);
            glTexCoord2f(0, 1); glVertex2f(offsetX, offsetY + renderHeight);
            glEnd();

            // Draw overlay
            overlay.render(videoPlayer, droppedFrames);
        }

        // Swap buffers
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    TTF_Quit();
    glDeleteTextures(1, &texture);
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
