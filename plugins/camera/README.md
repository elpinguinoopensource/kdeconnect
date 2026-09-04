# KDE Connect Camera Plugin

Receives a live H.264 Annex-B stream (`kdeconnect.camera.stream`) from a paired
device and feeds it through an `ffmpeg` child process into a v4l2loopback device,
so the phone camera shows up as a regular webcam (`/dev/videoN`).

## Requirements

- `ffmpeg` in `PATH` (used as `-f h264 -i pipe:0 -f v4l2 -pix_fmt yuv420p /dev/videoN`).
- The `v4l2loopback` kernel module loaded with at least one video device.

## v4l2loopback setup

Load the module (needs root):

```sh
sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="KDE Connect Camera"
```

To make it persistent, create `/etc/modules-load.d/v4l2loopback.conf` with the
line `v4l2loopback` and an options file:

```sh
echo "options v4l2loopback devices=1 video_nr=10 card_label=\"KDE Connect Camera\"" | sudo tee /etc/modprobe.d/v4l2loopback.conf
```

The plugin picks the first `/dev/video*` node advertising `V4L2_CAP_VIDEO_OUTPUT`
(via `VIDIOC_QUERYCAP`), so only v4l2loopback devices are selected. Physical
webcams (`uvcvideo`) are automatically skipped even when the user has write
access to them. The `v4l2loopback` kernel module must be loaded for the plugin
to find a device.

## Testing

Start a stream from the phone (or via DBus: `startCamera` on
`org.kde.kdeconnect.device.camera`), then view the virtual webcam:

```sh
ffplay /dev/video10
```

## Notes

- Only one stream at a time; a second `camera.stream` packet while streaming is
  ignored (its payload socket is closed).
- The payload socket is closed when the stream ends (stop, error, disconnect)
  to avoid leaking sockets.
- If `ffmpeg` cannot keep up, the plugin pauses reading from the socket above an
  8 MB write-queue backlog and resumes below 4 MB.
