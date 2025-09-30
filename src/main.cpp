#include <iostream>
#include <filesystem>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>  // For PBO extensions

#include "VideoPlayer.h"
#include "UdpReceiver.h"
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
};

std::string getConfigFilePath() {
    const std::string configName = "consoleVideoPlayer.config.json";

    // Priority order: /var/lib/consoleSyncedPlayer/ -> ../ -> ./
    std::vector<std::string> searchPaths = {
#ifdef __linux__
        "/var/lib/consoleSyncedPlayer/" + configName,
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
    DEBUG_PRINT("Loading settings...");
    Settings settings;
    const std::string settingsFile = getConfigFilePath();

    try {
        if (std::filesystem::exists(settingsFile)) {
            DEBUG_PRINT("Settings file exists: " << settingsFile);

            std::ifstream file(settingsFile);
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            auto json = parseSimpleJson(content);

            if (json.count("videoFilePath")) settings.videoFilePath = json["videoFilePath"];
            if (json.count("udpPort")) settings.udpPort = std::stoi(json["udpPort"]);
            if (json.count("fullscreen")) {
                std::string fullscreenValue = json["fullscreen"];
                DEBUG_PRINT("Parsed fullscreen value: '" << fullscreenValue << "'");
                settings.fullscreen = (fullscreenValue == "true");
            }
            if (json.count("windowTitle")) settings.windowTitle = json["windowTitle"];

            DEBUG_PRINT("Settings loaded successfully");
        } else {
            DEBUG_PRINT("Settings file does not exist: " << settingsFile);
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

    DEBUG_PRINT("Starting Console Video Player");

    std::cout << "Console Video Player" << std::endl;
    std::cout << "====================" << std::endl;

    auto settings = loadSettings();

    std::cout << "\nLoaded settings:" << std::endl;
    std::cout << "  Video file: " << settings.videoFilePath << std::endl;
    std::cout << "  UDP port: " << settings.udpPort << std::endl;
    std::cout << "  Fullscreen: " << (settings.fullscreen ? "yes" : "no") << std::endl;
    std::cout << "  Window title: " << settings.windowTitle << std::endl;

    // Check if video file exists
    if (!std::filesystem::exists(settings.videoFilePath)) {
        std::cerr << "Error: Video file not found at " << settings.videoFilePath << std::endl;
        return 1;
    }

    // Initialize SDL
    DEBUG_PRINT("Initializing SDL...");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    // Set OpenGL attributes
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Create window
    DEBUG_PRINT("Creating SDL window...");
    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    int windowWidth = 1280;
    int windowHeight = 720;

    if (settings.fullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        windowWidth = 1920;
        windowHeight = 1080;
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
    DEBUG_PRINT("Creating OpenGL context...");
    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        std::cerr << "OpenGL context creation failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Load PBO extension functions manually
    DEBUG_PRINT("Loading OpenGL PBO extensions...");
    glGenBuffers = (PFNGLGENBUFFERSPROC)SDL_GL_GetProcAddress("glGenBuffers");
    glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteBuffers");
    glBindBuffer = (PFNGLBINDBUFFERPROC)SDL_GL_GetProcAddress("glBindBuffer");
    glBufferData = (PFNGLBUFFERDATAPROC)SDL_GL_GetProcAddress("glBufferData");

    if (!glGenBuffers || !glDeleteBuffers || !glBindBuffer || !glBufferData) {
        std::cerr << "Failed to load PBO extension functions. PBOs not supported on this system." << std::endl;
        std::cerr << "Falling back to synchronous texture uploads..." << std::endl;
        // Continue without PBOs - will use fallback path
    } else {
        DEBUG_PRINT("PBO extensions loaded successfully");
    }

    // Enable vsync
    SDL_GL_SetSwapInterval(1);

    // Get actual window size
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    std::cout << "  Window size: " << windowWidth << "x" << windowHeight << std::endl;

    // Load video
    DEBUG_PRINT("Loading video file...");
    VideoPlayer videoPlayer;

    if (!videoPlayer.loadVideo(settings.videoFilePath)) {
        std::cerr << "Failed to load video: " << videoPlayer.getErrorMessage() << std::endl;
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::cout << "\nVideo loaded:" << std::endl;
    std::cout << "  Resolution: " << videoPlayer.getWidth() << "x" << videoPlayer.getHeight() << std::endl;
    std::cout << "  FPS: " << videoPlayer.getFPS() << std::endl;
    std::cout << "  Frames: " << videoPlayer.getFrameCount() << std::endl;
    std::cout << "  Duration: " << videoPlayer.getDuration() << "s" << std::endl;

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
        std::cout << "PBO double-buffering enabled (" << (pboSize / 1024.0 / 1024.0) << " MB per buffer)" << std::endl;
    } else {
        std::cout << "PBOs not available - using synchronous texture uploads" << std::endl;
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
    DEBUG_PRINT("Initializing JACK Transport client...");
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

    std::cout << "\nJACK Transport initialized" << std::endl;
    std::cout << "  JACK sample rate: " << jackSampleRate << " Hz" << std::endl;
    std::cout << "  Video FPS: " << fps << std::endl;
    std::cout << "Sample-accurate video sync enabled!" << std::endl;
    std::cout << "Press ESC or Q to quit\n" << std::endl;

    // Start playing
    videoPlayer.play();

    // Main render loop
    bool running = true;
    SDL_Event event;

    DEBUG_PRINT("Entering main render loop");

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

        // Seek to JACK transport position
        videoPlayer.seek(currentSeconds);

        // Get current frame
        const VideoFrame* frame = videoPlayer.getCurrentFrame();
        static const VideoFrame* lastFramePtr = nullptr;

        if (frame) {
            // Only upload when frame changes (optimization for both paths)
            if (frame != lastFramePtr) {
                lastFramePtr = frame;

                if (pbosEnabled) {
                    // PBO double-buffering path: async upload
                    // Step 1: Bind PBO[pboIndex] and upload new frame data (async DMA starts)
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[pboIndex]);
                    glBufferData(GL_PIXEL_UNPACK_BUFFER, pboSize, frame->data.data(), GL_STREAM_DRAW);

                    // Step 2: Bind texture and the OTHER PBO (from previous frame)
                    glBindTexture(GL_TEXTURE_2D, texture);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos[(pboIndex + 1) % 2]);

                    // Step 3: Update texture from PBO (uses data uploaded in previous iteration)
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                                frame->width, frame->height, 0,
                                GL_RGB, GL_UNSIGNED_BYTE, nullptr); // nullptr = use bound PBO

                    // Step 4: Unbind PBO and swap indices for next frame
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                    pboIndex = (pboIndex + 1) % 2;
                } else {
                    // Fallback: synchronous upload (old method)
                    glBindTexture(GL_TEXTURE_2D, texture);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                                frame->width, frame->height, 0,
                                GL_RGB, GL_UNSIGNED_BYTE, frame->data.data());
                }
            }

            // Clear and render
            glClear(GL_COLOR_BUFFER_BIT);

            // Calculate aspect ratio preserving letterbox
            float videoAspect = (float)frame->width / (float)frame->height;
            float windowAspect = (float)windowWidth / (float)windowHeight;

            float renderWidth, renderHeight;
            float offsetX = 0, offsetY = 0;

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

    std::cout << "\nShutting down..." << std::endl;

    // Cleanup
    DEBUG_PRINT("Cleaning up...");
    // JACK transport client will be automatically cleaned up via RAII
    if (pbosEnabled && glDeleteBuffers) {
        glDeleteBuffers(2, pbos);
    }
    glDeleteTextures(1, &texture);
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();

    DEBUG_PRINT("Program ended normally");
    return 0;
}