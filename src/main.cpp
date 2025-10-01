#include <iostream>
#include <filesystem>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>  // For PBO extensions

#include "VideoPlayer.h"
#include "JackTransportClient.h"

// PBO function pointers (manually loaded GL extensions)
PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;
PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
PFNGLBUFFERDATAPROC glBufferData = nullptr;

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
    std::string videoFilePath = "../test_video.mp4";
    int udpPort = 8080;
    bool fullscreen = true;
    std::string windowTitle = "Video Player";
    std::string scaleMode = "letterbox";  // Options: "letterbox", "stretch", "crop"
};

std::string getConfigFilePath() {
    const std::string configName = "consoleVideoPlayer.config.json";

    // Priority order: /var/lib/consolePlayers/ -> ../ -> ./
    std::vector<std::string> searchPaths = {
#ifdef __linux__
        "/var/lib/consolePlayers/" + configName,
#endif
        "../" + configName,
        configName
    };

    for (const auto& path : searchPaths) {
        if (std::filesystem::exists(path)) {
            return path;
        }
    }

    // If none exist, return the first path (will use defaults)
    return searchPaths[0];
}

// Simple JSON parser for our limited needs
std::map<std::string, std::string> parseSimpleJson(const std::string& content) {
    std::map<std::string, std::string> result;
    size_t pos = 0;

    while (pos < content.size()) {
        size_t keyStart = content.find('"', pos);
        if (keyStart == std::string::npos) break;
        keyStart++;

        size_t keyEnd = content.find('"', keyStart);
        if (keyEnd == std::string::npos) break;

        std::string key = content.substr(keyStart, keyEnd - keyStart);

        size_t valueStart = content.find(':', keyEnd);
        if (valueStart == std::string::npos) break;
        valueStart++;

        // Skip whitespace
        while (valueStart < content.size() && (content[valueStart] == ' ' || content[valueStart] == '\t' || content[valueStart] == '\n')) {
            valueStart++;
        }

        std::string value;
        if (content[valueStart] == '"') {
            // String value
            valueStart++;
            size_t valueEnd = content.find('"', valueStart);
            if (valueEnd == std::string::npos) break;
            value = content.substr(valueStart, valueEnd - valueStart);
            pos = valueEnd + 1;
        } else if (content[valueStart] == 't' || content[valueStart] == 'f') {
            // Boolean
            size_t valueEnd = content.find_first_of(",}", valueStart);
            if (valueEnd == std::string::npos) valueEnd = content.size();
            value = content.substr(valueStart, valueEnd - valueStart);
            // Trim
            size_t end = value.find_last_not_of(" \t\n\r");
            if (end != std::string::npos) value = value.substr(0, end + 1);
            pos = valueEnd;
        } else {
            // Number
            size_t valueEnd = content.find_first_of(",}", valueStart);
            if (valueEnd == std::string::npos) valueEnd = content.size();
            value = content.substr(valueStart, valueEnd - valueStart);
            // Trim
            size_t end = value.find_last_not_of(" \t\n\r");
            if (end != std::string::npos) value = value.substr(0, end + 1);
            pos = valueEnd;
        }

        result[key] = value;
    }

    return result;
}

Settings loadSettings() {
    Settings settings;
    const std::string settingsFile = getConfigFilePath();

    try {
        if (std::filesystem::exists(settingsFile)) {
            std::ifstream file(settingsFile);
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            auto json = parseSimpleJson(content);

            if (json.count("videoFilePath")) settings.videoFilePath = json["videoFilePath"];
            if (json.count("udpPort")) settings.udpPort = std::stoi(json["udpPort"]);
            if (json.count("fullscreen")) {
                std::string fullscreenValue = json["fullscreen"];
                settings.fullscreen = (fullscreenValue == "true");
            }
            if (json.count("windowTitle")) settings.windowTitle = json["windowTitle"];
            if (json.count("scaleMode")) settings.scaleMode = json["scaleMode"];

        }
    } catch (const std::exception& e) {
        std::cout << "Warning: Could not load settings, using defaults: " << e.what() << std::endl;
    }
    return settings;
}

// Global video player for UDP callback
// UDP-based command handling (replaced by JACK Transport)
// VideoPlayer* g_videoPlayer = nullptr;
// void handleCommand(const std::string& command) { ... }

int main() {
    // Install signal handlers
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);

    std::cout << "Console Video Player (JACK Sync)" << std::endl;
    std::cout << "=================================" << std::endl;

    auto settings = loadSettings();

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
    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
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
        settings.windowTitle.c_str(),
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

    // Load PBO extension functions manually
    glGenBuffers = (PFNGLGENBUFFERSPROC)SDL_GL_GetProcAddress("glGenBuffers");
    glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteBuffers");
    glBindBuffer = (PFNGLBINDBUFFERPROC)SDL_GL_GetProcAddress("glBindBuffer");
    glBufferData = (PFNGLBUFFERDATAPROC)SDL_GL_GetProcAddress("glBufferData");

    if (!glGenBuffers || !glDeleteBuffers || !glBindBuffer || !glBufferData) {
        std::cout << "⚠ PBOs not supported - using synchronous texture uploads" << std::endl;
        // Continue without PBOs - will use fallback path
    }

    // Enable vsync
    SDL_GL_SetSwapInterval(1);

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

    // Setup PBOs (Pixel Buffer Objects) for async texture uploads (if available)
    GLuint pbos[2] = {0, 0};
    size_t pboSize = videoPlayer.getWidth() * videoPlayer.getHeight() * 3; // RGB24
    int pboIndex = 0;      // Current PBO for uploading
    bool pbosEnabled = false;

    if (glGenBuffers && glBindBuffer && glBufferData) {
        glGenBuffers(2, pbos);

        // Initialize both PBOs
        for (int i = 0; i < 2; i++) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[i]);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, pboSize, nullptr, GL_STREAM_DRAW);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); // Unbind

        pbosEnabled = true;
        std::cout << "✓ PBO double-buffering enabled" << std::endl;
    }

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
    std::cout << "\nReady. Press ESC or Q to quit.\n" << std::endl;

    // Start playing
    videoPlayer.play();

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
                } else if (event.key.keysym.sym == SDLK_SPACE) {
                    if (videoPlayer.isPlaying()) {
                        videoPlayer.pause();
                    } else {
                        videoPlayer.play();
                    }
                }
            }
        }

        // Update video player
        videoPlayer.update();

        // Sync video play/pause state to JACK Transport
        bool jackIsPlaying = jackTransport.isTransportRolling();
        static int pboWarmupFramesRemaining = 0;  // Force sync upload for 2 frames to flush both PBOs

        if (jackIsPlaying && !videoPlayer.isPlaying()) {
            videoPlayer.play();
            pboWarmupFramesRemaining = 2;  // Flush both PBO buffers with sync uploads
            // Reset PBO index to ensure clean state after warmup
            if (pbosEnabled) {
                pboIndex = 0;
            }
        } else if (!jackIsPlaying && videoPlayer.isPlaying()) {
            videoPlayer.pause();
        }

        // Query JACK transport position and sync video to it
        jack_nframes_t currentJackFrame = jackTransport.getCurrentFrame();
        double currentSeconds = (double)currentJackFrame / jackSampleRate;
        int targetVideoFrame = (int)(currentSeconds * fps);

        // Clamp to valid frame range
        int totalFrames = videoPlayer.getFrameCount();
        if (targetVideoFrame >= totalFrames) {
            targetVideoFrame = totalFrames - 1;
        }
        if (targetVideoFrame < 0) {
            targetVideoFrame = 0;
        }

        // Always seek to JACK transport position (works even when paused)
        videoPlayer.seek(currentSeconds);

        // Get current frame
        const VideoFrame* frame = videoPlayer.getCurrentFrame();
        static int lastUploadedFrameIndex = -1;
        static int lastTargetVideoFrame = -1;

        if (frame) {
            // Detect seeks: if target frame jumped by more than 5 frames, force PBO warmup
            // This flushes stale PBO buffers and ensures correct frame displays immediately
            if (lastTargetVideoFrame != -1 && std::abs(targetVideoFrame - lastTargetVideoFrame) > 5) {
                pboWarmupFramesRemaining = 2;  // Flush both PBO buffers
                // Reset PBO index to ensure clean state after warmup
                if (pbosEnabled) {
                    pboIndex = 0;
                }
            }
            lastTargetVideoFrame = targetVideoFrame;

            // Upload when target frame index changes (not just pointer)
            // This ensures texture updates even when getCurrentFrame() returns same cached frame during seeks
            if (targetVideoFrame != lastUploadedFrameIndex) {
                lastUploadedFrameIndex = targetVideoFrame;

                // Use PBOs only when playing (1-frame delay is acceptable during motion)
                // When paused or warming up PBOs, use synchronous upload for immediate visual feedback
                if (pbosEnabled && videoPlayer.isPlaying() && pboWarmupFramesRemaining == 0) {
                    // PBO double-buffering path: async upload (1-frame delay)
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[pboIndex]);
                    glBufferData(GL_PIXEL_UNPACK_BUFFER, pboSize, frame->data.data(), GL_STREAM_DRAW);

                    glBindTexture(GL_TEXTURE_2D, texture);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[(pboIndex + 1) % 2]);

                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                                frame->width, frame->height, 0,
                                GL_RGB, GL_UNSIGNED_BYTE, nullptr); // nullptr = use bound PBO

                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                    pboIndex = (pboIndex + 1) % 2;
                } else {
                    // Paused, warming up, or PBOs unavailable: synchronous upload (immediate, no delay)

                    // During warmup: overwrite BOTH PBOs with current frame data to flush stale data
                    if (pbosEnabled && pboWarmupFramesRemaining > 0) {
                        for (int i = 0; i < 2; i++) {
                            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[i]);
                            glBufferData(GL_PIXEL_UNPACK_BUFFER, pboSize, frame->data.data(), GL_STREAM_DRAW);
                        }
                    }

                    // Then upload texture synchronously (unbind PBO for immediate upload)
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                    glBindTexture(GL_TEXTURE_2D, texture);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                                frame->width, frame->height, 0,
                                GL_RGB, GL_UNSIGNED_BYTE, frame->data.data());

                    if (pboWarmupFramesRemaining > 0) {
                        pboWarmupFramesRemaining--;  // Count down warmup frames
                    }
                }
            }

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
        }

        // Swap buffers
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    // JACK transport client will be automatically cleaned up via RAII
    if (pbosEnabled && glDeleteBuffers) {
        glDeleteBuffers(2, pbos);
    }
    glDeleteTextures(1, &texture);
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}