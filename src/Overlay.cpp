#include "Overlay.h"
#include "VideoPlayer.h"
#include <cstdio>

Overlay::Overlay() {}

Overlay::~Overlay() {
    cleanup();
    if (font) {
        TTF_CloseFont(font);
    }
}

bool Overlay::init(const char* fontPath, int fontSize) {
    font = TTF_OpenFont(fontPath, fontSize);
    if (!font) {
        // Try alternative font paths
        font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", fontSize);
    }
    return font != nullptr;
}

void Overlay::cleanup() {
    if (bufferLabelTex) {
        glDeleteTextures(1, &bufferLabelTex);
        bufferLabelTex = 0;
    }
    if (frameTextTex) {
        glDeleteTextures(1, &frameTextTex);
        frameTextTex = 0;
    }
    if (droppedFramesTextTex) {
        glDeleteTextures(1, &droppedFramesTextTex);
        droppedFramesTextTex = 0;
    }
    lastRenderedFrame = -1;
    lastRenderedDroppedFrames = -1;
}

void Overlay::toggle() {
    enabled = !enabled;
    if (!enabled) {
        cleanup();
    }
}

GLuint Overlay::renderTextToTexture(const std::string& text, SDL_Color color, int& outWidth, int& outHeight) {
    if (!font) return 0;

    SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), color);
    if (!surface) return 0;

    // Convert to RGBA format for OpenGL
    SDL_Surface* rgbaSurface = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surface);

    if (!rgbaSurface) return 0;

    outWidth = rgbaSurface->w;
    outHeight = rgbaSurface->h;

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgbaSurface->w, rgbaSurface->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgbaSurface->pixels);

    SDL_FreeSurface(rgbaSurface);
    return texture;
}

void Overlay::render(VideoPlayer& player, int droppedFrames) {
    if (!enabled || !font) return;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int currentFrame = player.getCurrentFrameIndex();
    int bufferedCount = player.getBufferedFrameCount(currentFrame, 150);
    float bufferPercent = bufferedCount / 150.0f;

    SDL_Color white = {255, 255, 255, 255};

    // "Buffer:" label (cached - only generate once)
    if (!bufferLabelTex) {
        bufferLabelTex = renderTextToTexture("Buffer:", white, bufferLabelW, bufferLabelH);
    }
    if (bufferLabelTex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, bufferLabelTex);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(10, 10);
        glTexCoord2f(1, 0); glVertex2f(10 + bufferLabelW, 10);
        glTexCoord2f(1, 1); glVertex2f(10 + bufferLabelW, 10 + bufferLabelH);
        glTexCoord2f(0, 1); glVertex2f(10, 10 + bufferLabelH);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }

    // Background bar (dark gray)
    glColor3f(0.2f, 0.2f, 0.2f);
    glBegin(GL_QUADS);
    glVertex2f(10, 35);
    glVertex2f(310, 35);
    glVertex2f(310, 55);
    glVertex2f(10, 55);
    glEnd();

    // Buffer bar (color based on health)
    if (bufferPercent > 0.7f) {
        glColor3f(0.0f, 1.0f, 0.0f); // Green
    } else if (bufferPercent > 0.3f) {
        glColor3f(1.0f, 1.0f, 0.0f); // Yellow
    } else {
        glColor3f(1.0f, 0.0f, 0.0f); // Red
    }
    glBegin(GL_QUADS);
    glVertex2f(10, 35);
    glVertex2f(10 + (300 * bufferPercent), 35);
    glVertex2f(10 + (300 * bufferPercent), 55);
    glVertex2f(10, 55);
    glEnd();

    // Frame number and timecode (only regenerate when frame changes)
    if (currentFrame != lastRenderedFrame) {
        if (frameTextTex) {
            glDeleteTextures(1, &frameTextTex);
        }

        double timecode = currentFrame / player.getFPS();
        int minutes = (int)(timecode / 60);
        int seconds = (int)timecode % 60;
        int ms = (int)((timecode - (int)timecode) * 1000);

        char frameText[128];
        snprintf(frameText, sizeof(frameText), "Frame: %d | %d:%02d.%03d",
                 currentFrame, minutes, seconds, ms);
        frameTextTex = renderTextToTexture(frameText, white, frameTextW, frameTextH);
        lastRenderedFrame = currentFrame;
    }

    if (frameTextTex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, frameTextTex);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(10, 65);
        glTexCoord2f(1, 0); glVertex2f(10 + frameTextW, 65);
        glTexCoord2f(1, 1); glVertex2f(10 + frameTextW, 65 + frameTextH);
        glTexCoord2f(0, 1); glVertex2f(10, 65 + frameTextH);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }

    // Dropped frames counter (only regenerate when count changes)
    if (droppedFrames != lastRenderedDroppedFrames) {
        if (droppedFramesTextTex) {
            glDeleteTextures(1, &droppedFramesTextTex);
        }

        char droppedText[128];
        snprintf(droppedText, sizeof(droppedText), "Dropped: %d", droppedFrames);

        // Color based on severity
        SDL_Color color;
        if (droppedFrames == 0) {
            color = {0, 255, 0, 255};  // Green
        } else if (droppedFrames < 10) {
            color = {255, 255, 0, 255};  // Yellow
        } else {
            color = {255, 0, 0, 255};  // Red
        }

        droppedFramesTextTex = renderTextToTexture(droppedText, color, droppedFramesTextW, droppedFramesTextH);
        lastRenderedDroppedFrames = droppedFrames;
    }

    if (droppedFramesTextTex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, droppedFramesTextTex);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(10, 95);
        glTexCoord2f(1, 0); glVertex2f(10 + droppedFramesTextW, 95);
        glTexCoord2f(1, 1); glVertex2f(10 + droppedFramesTextW, 95 + droppedFramesTextH);
        glTexCoord2f(0, 1); glVertex2f(10, 95 + droppedFramesTextH);
        glEnd();
    }

    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1.0f, 1.0f, 1.0f);
}
