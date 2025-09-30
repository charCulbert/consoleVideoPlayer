cmake_minimum_required(VERSION 3.15)
project(consoleVideoPlayer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(LIBAV REQUIRED
    libavformat
    libavcodec
    libavutil
    libswscale
)
pkg_check_modules(JACK REQUIRED jack)

# OpenGL
find_package(OpenGL REQUIRED)

# Source files
set(SOURCES
    src/main.cpp
    src/VideoPlayer.cpp
    src/UdpReceiver.cpp
    src/JackTransportClient.cpp
)

# Create executable
add_executable(consoleVideoPlayer ${SOURCES})

# Include directories
target_include_directories(consoleVideoPlayer PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${LIBAV_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIRS}
    ${JACK_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(consoleVideoPlayer
    ${SDL2_LIBRARIES}
    ${LIBAV_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${JACK_LIBRARIES}
    pthread
)

# Compiler flags
target_compile_options(consoleVideoPlayer PRIVATE
    -Wall
    -Wextra
    -O3
)

# Install target
install(TARGETS consoleVideoPlayer DESTINATION bin)