#pragma once

#include <string>
#include <SDL2/SDL_ttf.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

class VideoPlayer;

struct VideoInfo {
    int width;
    int height;
    double fps;
    double duration;
    std::string codecName;
};

struct DisplaySettings {
    double syncOffsetMs;
    bool fullscreen;
    std::string scaleMode;
    std::string videoFilePath;
};

class Overlay {
public:
    Overlay();
    ~Overlay();

    bool init(const char* fontPath, int fontSize);
    void render(VideoPlayer& player, const VideoInfo& videoInfo, const DisplaySettings& displaySettings);
    void toggle();
    void shutdown();  // Call before TTF_Quit()
    bool isEnabled() const { return enabled; }

private:
    TTF_Font* font = nullptr;
    bool enabled = false;

    // Cached textures (regenerate only when values change)
    GLuint bufferLabelTex = 0;
    int bufferLabelW = 0, bufferLabelH = 0;

    GLuint frameTextTex = 0;
    int frameTextW = 0, frameTextH = 0;
    int lastRenderedFrame = -1;

    GLuint videoInfoTex = 0;
    int videoInfoW = 0, videoInfoH = 0;

    GLuint settingsTex = 0;
    int settingsW = 0, settingsH = 0;
    double lastRenderedOffset = 99999.0;
    std::string lastScaleMode;
    bool lastFullscreen = false;

    GLuint commandTex = 0;
    int commandW = 0, commandH = 0;
    double lastCommandOffset = 99999.0;
    std::string lastCommandScaleMode;
    bool lastCommandFullscreen = false;

    GLuint helpTex = 0;
    int helpW = 0, helpH = 0;

    void cleanup();
    GLuint renderTextToTexture(const std::string& text, SDL_Color color, int& outWidth, int& outHeight);
    std::string generateCommandString(const DisplaySettings& settings);
};
