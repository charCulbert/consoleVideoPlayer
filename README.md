# Console Video Player

High-performance video player with UDP command control for synchronized playback with audio looper.

## Features

- **<5ms latency**: Pre-decodes entire video to RAM for instant seeking
- **UDP command control**: PLAY, PAUSE, STOP, SEEK commands
- **SDL2 + OpenGL**: Hardware-accelerated rendering with vsync
- **KMS/DRM support**: Runs headless without X11/Wayland
- **Systemd service**: Kiosk-mode ready for production deployment

## Building

### Dependencies

```bash
sudo apt install build-essential cmake pkg-config \
    libsdl2-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libgl1-mesa-dev
```

### Compile

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Configuration

Edit `consoleVideoPlayer.config.json`:

```json
{
  "videoFilePath": "/path/to/video.mp4",
  "udpPort": 8080,
  "fullscreen": true,
  "windowTitle": "Video Player"
}
```

Or in production, config is read from `/var/lib/consoleVideoPlayer/consoleVideoPlayer.config.json`

## UDP Commands

Send UDP messages to port 8080 (configurable):

- `PLAY` - Start playback
- `PAUSE` - Pause playback
- `STOP` - Stop and reset to beginning
- `SEEK 0` - Seek to position (in seconds)
- `LOOP` / `HELLO` - Seek to start (triggered by audio looper)

### Test with netcat:

```bash
echo "PLAY" | nc -u -w1 localhost 8080
echo "SEEK 0" | nc -u -w1 localhost 8080
```

### Or from your audio looper:
The audio player already sends UDP broadcasts, this video player will receive them automatically.

## Running

### Manual:
```bash
./build/consoleVideoPlayer
```

### As systemd service (kiosk mode):

1. Edit the service file to match your paths:
```bash
sudo nano consoleVideoPlayer.service
```

2. Copy service file:
```bash
sudo cp consoleVideoPlayer.service /etc/systemd/system/
```

3. Create config directory:
```bash
sudo mkdir -p /var/lib/consoleVideoPlayer
sudo cp consoleVideoPlayer.config.json /var/lib/consoleVideoPlayer/
sudo chown -R char:char /var/lib/consoleVideoPlayer
```

4. Enable and start:
```bash
sudo systemctl daemon-reload
sudo systemctl enable consoleVideoPlayer
sudo systemctl start consoleVideoPlayer
```

5. Check status:
```bash
sudo systemctl status consoleVideoPlayer
journalctl -u consoleVideoPlayer -f
```

## Performance Notes

- **Memory usage**: ~3MB per frame for 1080p (e.g., 10s @ 30fps = ~900MB)
- **Seek latency**: ~1-3ms total (sub-millisecond index update + GPU texture swap + vsync)
- **Rendering**: Vsync-locked to display refresh rate (60Hz typically)

## Permissions for KMS/DRM

If running as non-root with KMS/DRM backend, add user to video group:

```bash
sudo usermod -a -G video $USER
```

Then logout/login or reboot.

## Keyboard Controls (when not in systemd)

- `SPACE` - Toggle play/pause
- `ESC` or `Q` - Quit

## Architecture

- **VideoPlayer**: FFmpeg-based decoder, loads entire video to RAM as RGB24 frames
- **UdpReceiver**: Non-blocking UDP listener thread for commands
- **main.cpp**: SDL2+OpenGL renderer with texture streaming

## Synchronization with Audio Looper

The audio looper (consoleAppStaticPlayer) sends UDP broadcasts at loop points.
This video player receives those messages and seeks back to the start, keeping
audio and video synchronized with <5ms latency.

## Troubleshooting

**Black screen**: Check `journalctl -u consoleVideoPlayer` for errors

**Permission denied**: Ensure user is in `video` group for KMS/DRM access

**High memory usage**: Expected - entire video is decoded to RAM for low latency

**Choppy playback**: Ensure vsync is enabled and system isn't overloaded