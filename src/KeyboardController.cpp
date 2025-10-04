#include "KeyboardController.h"
#include "Overlay.h"
#include <iostream>
#include <iomanip>

KeyboardController::KeyboardController(AppSettings& settings, Overlay& overlay, SDL_Window* window)
    : settings(settings), overlay(overlay), window(window) {}

void KeyboardController::cycleScaleMode() {
    if (settings.scaleMode == "letterbox") {
        settings.scaleMode = "stretch";
    } else if (settings.scaleMode == "stretch") {
        settings.scaleMode = "crop";
    } else {
        settings.scaleMode = "letterbox";
    }
    std::cout << "Scale mode: " << settings.scaleMode << std::endl;
}

bool KeyboardController::handleEvent(const SDL_Event& event) {
    if (event.type == SDL_QUIT) {
        return false;
    }

    if (event.type == SDL_KEYDOWN) {
        SDL_Keycode key = event.key.keysym.sym;
        bool shift = event.key.keysym.mod & KMOD_SHIFT;

        switch (key) {
            case SDLK_ESCAPE:
            case SDLK_q:
                return false;

            case SDLK_i:
                overlay.toggle();
                std::cout << "Overlay " << (overlay.isEnabled() ? "ON" : "OFF") << std::endl;
                break;

            case SDLK_UP:
            case SDLK_RIGHT: {
                double step = shift ? OFFSET_STEP_LARGE_MS : OFFSET_STEP_MS;
                settings.syncOffsetMs += step;
                std::cout << "Sync offset: " << std::fixed << std::setprecision(1)
                          << settings.syncOffsetMs << " ms" << std::endl;
                break;
            }

            case SDLK_DOWN:
            case SDLK_LEFT: {
                double step = shift ? OFFSET_STEP_LARGE_MS : OFFSET_STEP_MS;
                settings.syncOffsetMs -= step;
                std::cout << "Sync offset: " << std::fixed << std::setprecision(1)
                          << settings.syncOffsetMs << " ms" << std::endl;
                break;
            }

            case SDLK_0:
                settings.syncOffsetMs = 0.0;
                std::cout << "Sync offset reset to 0.0 ms" << std::endl;
                break;

            case SDLK_f:
                settings.fullscreen = !settings.fullscreen;
                toggleFullscreen = true;
                std::cout << "Fullscreen: " << (settings.fullscreen ? "ON" : "OFF") << std::endl;
                break;

            case SDLK_s:
                cycleScaleMode();
                break;

            case SDLK_c: {
                // Print command to reproduce current state
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

                std::cout << "\nCommand to reproduce current state:\n" << cmd << "\n" << std::endl;
                break;
            }
        }
    }

    return true;
}
