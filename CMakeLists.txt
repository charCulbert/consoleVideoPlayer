cmake_minimum_required(VERSION 3.20)
project(consoleVideoPlayer)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(consoleVideoPlayer
    src/main.cpp
    src/VideoPlayer.cpp
    src/JackTransportClient.cpp
    src/Overlay.cpp
    src/KeyboardController.cpp
)

# Common packages for both platforms
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
pkg_check_modules(LIBAV REQUIRED libavformat libavcodec libavutil libswscale)
pkg_check_modules(JACK REQUIRED jack)
find_package(OpenGL REQUIRED)

target_include_directories(consoleVideoPlayer PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${LIBAV_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIRS}
    ${JACK_INCLUDE_DIRS}
)

# Platform-specific configuration
if(APPLE)
    target_link_directories(consoleVideoPlayer PRIVATE
        ${SDL2_LIBRARY_DIRS}
        ${SDL2_TTF_LIBRARY_DIRS}
        ${LIBAV_LIBRARY_DIRS}
        ${JACK_LIBRARY_DIRS}
    )
    target_link_libraries(consoleVideoPlayer PRIVATE
        ${SDL2_LIBRARIES}
        ${SDL2_TTF_LIBRARIES}
        ${LIBAV_LIBRARIES}
        ${OPENGL_LIBRARIES}
        ${JACK_LIBRARIES}
    )
elseif(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)

    target_link_libraries(consoleVideoPlayer PRIVATE
        ${SDL2_LIBRARIES}
        ${SDL2_TTF_LIBRARIES}
        ${LIBAV_LIBRARIES}
        ${OPENGL_LIBRARIES}
        ${JACK_LIBRARIES}
        Threads::Threads
    )
endif()

target_compile_options(consoleVideoPlayer PRIVATE -Wall -Wextra -O3)

install(TARGETS consoleVideoPlayer DESTINATION bin)
