#include "Overlay.h"
#include "VideoPlayer.h"
#include <cstdio>
#include <iostream>

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
    if (videoInfoTex) {
        glDeleteTextures(1, &videoInfoTex);
        videoInfoTex = 0;
    }
    if (settingsTex) {
        glDeleteTextures(1, &settingsTex);
        settingsTex = 0;
    }
    if (commandTex) {
        glDeleteTextures(1, &commandTex);
        commandTex = 0;
    }
    if (helpTex) {
        glDeleteTextures(1, &helpTex);
        helpTex = 0;
    }
    lastRenderedFrame = -1;
    lastRenderedOffset = 99999.0;
    lastScaleMode.clear();
    lastFullscreen = false;
    lastCommandOffset = 99999.0;
    lastCommandScaleMode.clear();
    lastCommandFullscreen = false;
}

std::string Overlay::generateCommandString(const DisplaySettings& settings) {
    std::string cmd = "./build/consoleVideoPlayer " + settings.videoFilePath;

    if (settings.syncOffsetMs != 0.0) {
        char offsetBuf[32];
        snprintf(offsetBuf, sizeof(offsetBuf), " --offset %.1f", settings.syncOffsetMs);
        cmd += offsetBuf;
    }

    if (settings.fullscreen) {
        cmd += " --fullscreen";
    }

    if (settings.scaleMode != "letterbox") {
        cmd += " --scale " + settings.scaleMode;
    }

    return cmd;
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

void Overlay::render(VideoPlayer& player, const VideoInfo& videoInfo, const DisplaySettings& displaySettings) {
    if (!enabled || !font) return;

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int currentFrame = player.getCurrentFrameIndex();
    int bufferedCount = player.getBufferedFrameCount(currentFrame, 150);
    float bufferPercent = bufferedCount / 150.0f;

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color cyan = {0, 255, 255, 255};

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

    // Video info (static - only generate once)
    if (!videoInfoTex) {
        char infoText[256];
        snprintf(infoText, sizeof(infoText), "%dx%d @ %.2f fps | %.1fs | %s",
                 videoInfo.width, videoInfo.height, videoInfo.fps,
                 videoInfo.duration, videoInfo.codecName.c_str());
        videoInfoTex = renderTextToTexture(infoText, cyan, videoInfoW, videoInfoH);
    }
    if (videoInfoTex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, videoInfoTex);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(10, 95);
        glTexCoord2f(1, 0); glVertex2f(10 + videoInfoW, 95);
        glTexCoord2f(1, 1); glVertex2f(10 + videoInfoW, 95 + videoInfoH);
        glTexCoord2f(0, 1); glVertex2f(10, 95 + videoInfoH);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }

    // Settings (regenerate when any setting changes)
    if (!settingsTex || displaySettings.syncOffsetMs != lastRenderedOffset ||
        displaySettings.scaleMode != lastScaleMode || displaySettings.fullscreen != lastFullscreen) {
        if (settingsTex) {
            glDeleteTextures(1, &settingsTex);
        }

        char settingsText[256];
        snprintf(settingsText, sizeof(settingsText), "Offset: %.1fms | %s | %s",
                 displaySettings.syncOffsetMs,
                 displaySettings.scaleMode.c_str(),
                 displaySettings.fullscreen ? "fullscreen" : "windowed");
        settingsTex = renderTextToTexture(settingsText, white, settingsW, settingsH);
        lastRenderedOffset = displaySettings.syncOffsetMs;
        lastScaleMode = displaySettings.scaleMode;
        lastFullscreen = displaySettings.fullscreen;
    }
    if (settingsTex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, settingsTex);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(10, 125);
        glTexCoord2f(1, 0); glVertex2f(10 + settingsW, 125);
        glTexCoord2f(1, 1); glVertex2f(10 + settingsW, 125 + settingsH);
        glTexCoord2f(0, 1); glVertex2f(10, 125 + settingsH);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }

    // Command to reproduce current state (regenerate when settings change)
    if (!commandTex || displaySettings.syncOffsetMs != lastCommandOffset ||
        displaySettings.scaleMode != lastCommandScaleMode || displaySettings.fullscreen != lastCommandFullscreen) {
        if (commandTex) {
            glDeleteTextures(1, &commandTex);
        }

        std::string cmdStr = generateCommandString(displaySettings);
        SDL_Color green = {0, 255, 0, 255};
        commandTex = renderTextToTexture(cmdStr, green, commandW, commandH);
        lastCommandOffset = displaySettings.syncOffsetMs;
        lastCommandScaleMode = displaySettings.scaleMode;
        lastCommandFullscreen = displaySettings.fullscreen;
    }
    if (commandTex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, commandTex);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(10, 155);
        glTexCoord2f(1, 0); glVertex2f(10 + commandW, 155);
        glTexCoord2f(1, 1); glVertex2f(10 + commandW, 155 + commandH);
        glTexCoord2f(0, 1); glVertex2f(10, 155 + commandH);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }

    // Keyboard help (always on)
    if (!helpTex) {
        std::string helpText =
            "I:Overlay  F:Fullscreen  S:Scale  Arrows:Offset  0:Reset  Q:Quit";
        SDL_Color yellow = {255, 255, 0, 255};
        helpTex = renderTextToTexture(helpText, yellow, helpW, helpH);
    }
    if (helpTex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, helpTex);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(10, 185);
        glTexCoord2f(1, 0); glVertex2f(10 + helpW, 185);
        glTexCoord2f(1, 1); glVertex2f(10 + helpW, 185 + helpH);
        glTexCoord2f(0, 1); glVertex2f(10, 185 + helpH);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }

    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1.0f, 1.0f, 1.0f);
}
