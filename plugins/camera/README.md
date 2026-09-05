# KDE Connect Camera Plugin

Receives a live H.264 Annex-B stream (`kdeconnect.camera.stream`) from a paired
device and feeds it through an `ffmpeg` child process into a v4l2loopback device,
so the phone camera shows up as a regular webcam (`/dev/videoN`).

## Requirements

- `ffmpeg` in `PATH` (used as `-f h264 -i pipe:0 -f v4l2 -pix_fmt yuv420p /dev/videoN`).
- The `v4l2loopback` kernel module loaded with at least one **output** video device.

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

> **Note on `exclusive_caps`.** With `exclusive_caps=1` the node advertises
> *output* capability while it is idle and flips to *capture* capability as soon
> as a producer (our ffmpeg, or the idle cover) is attached — this is what keeps
> plain webcam apps from seeing an empty device. The plugin handles the flip
> naturally: it only probes for an output node when starting a pipeline, and it
> stops the idle cover before the live stream opens the node (the loopback slot
> is single-writer; a second opener gets `EBUSY`).

## Latency

The stream is live, so the ffmpeg invocation is tuned to minimise delay:

- `-probesize 131072 -analyzeduration 0`: the defaults buffer up to 5 MB / 5 s
  of the stream while probing before decoding starts. 128 KB is plenty to see
  the in-band SPS/PPS/first IDR.
- `-flags low_delay`: no frame reordering or decoder-side delay.
- `-threads 0 -thread_type slice`: parallelism is restricted to slice threading,
  which decodes several slices of the *same* frame concurrently and never holds
  a frame back. The default frame threading keeps 4-6 frames in flight
  (~200 ms at 30 fps).
- `-fflags nobuffer` is deliberately **not** used. Some phone encoders prefix
  every access unit with an empty AUD NAL; with `nobuffer` the demuxer turns
  those into zero-length packets and the pipeline stalls after the first frame.

The write queue is bounded by a 512 KB high-water mark (≈1 s at 4 Mbps) with a
256 KB low-water mark. Bytes sitting in that queue are stale frames, i.e. pure
added latency, so it is kept small on purpose: when the high mark is hit the
plugin pauses reading from the socket and lets the backlog move to the phone,
whose drop-oldest buffer trims it and recovers with a fresh IDR.

## Stall detection

A phone that stops sending frames without closing the socket would otherwise
hang the session (TCP keepalive alone takes ~60 s to notice, and the TLS socket
may never notice at all). The writer therefore runs an activity watchdog: after
a 15 s start-up grace period (ffmpeg probing plus encoder ramp-up), five
consecutive seconds without a single payload byte tear the pipeline down and
report `stream_stalled`. Every batch of received bytes postpones the verdict.

## Testing

Start a stream from the phone (or via DBus: `startCamera` on
`org.kde.kdeconnect.device.camera`), then view the virtual webcam:

```sh
ffplay /dev/video10
```

`tests/camerastreamtest.cpp` covers the plumbing (failure paths, teardown,
metadata) everywhere, and the watchdog end-to-end on machines that have both
`ffmpeg` and a usable v4l2loopback output node (skipped otherwise).

## Notes

- Only one stream at a time; a second `camera.stream` packet while streaming is
  ignored (its payload socket is closed).
- The payload socket is closed when the stream ends (stop, error, disconnect)
  to avoid leaking sockets.

## Debugging

Set `KDECONNECT_CAMERA_DUMP=/path/to/file` to mirror the raw Annex-B payload to
a file while streaming. The dump can be inspected offline with, for example,
`ffprobe -show_packets file.h264`.
