#pragma once

#include <SDL2/SDL.h>
#include <string>

class Overlay;
class VideoPlayer;

struct AppSettings {
    std::string videoFilePath;
    double syncOffsetMs = 0.0;
    bool fullscreen = false;
    std::string scaleMode = "letterbox";
};

class KeyboardController {
public:
    KeyboardController(AppSettings& settings, Overlay& overlay, SDL_Window* window);

    // Returns false if quit requested
    bool handleEvent(const SDL_Event& event);

    double getSyncOffsetMs() const { return settings.syncOffsetMs; }
    bool needsFullscreenToggle() const { return toggleFullscreen; }
    void clearFullscreenToggle() { toggleFullscreen = false; }

private:
    AppSettings& settings;
    Overlay& overlay;
    SDL_Window* window;

    bool toggleFullscreen = false;

    const double OFFSET_STEP_MS = 1.0;  // Adjust by 1ms per keypress
    const double OFFSET_STEP_LARGE_MS = 10.0;  // Shift+arrow = 10ms

    void cycleScaleMode();
};
