#pragma once

#include <string>
#include <SDL2/SDL_ttf.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

class VideoPlayer;

class Overlay {
public:
    Overlay();
    ~Overlay();

    bool init(const char* fontPath, int fontSize);
    void render(VideoPlayer& player, int droppedFrames);
    void toggle();
    bool isEnabled() const { return enabled; }

private:
    TTF_Font* font = nullptr;
    bool enabled = true;

    // Cached textures (regenerate only when values change)
    GLuint bufferLabelTex = 0;
    int bufferLabelW = 0, bufferLabelH = 0;

    GLuint frameTextTex = 0;
    int frameTextW = 0, frameTextH = 0;
    int lastRenderedFrame = -1;

    GLuint droppedFramesTextTex = 0;
    int droppedFramesTextW = 0, droppedFramesTextH = 0;
    int lastRenderedDroppedFrames = -1;

    void cleanup();
    GLuint renderTextToTexture(const std::string& text, SDL_Color color, int& outWidth, int& outHeight);
};
