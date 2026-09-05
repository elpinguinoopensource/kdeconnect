# Camera-as-Webcam — implementation status & development notes (desktop)

Living document for the **Camera as webcam** feature (Phase 0), desktop side.
Tracks implementation state, protocol, the Docker build environment and the
ffmpeg/hardware gotchas that cost the most debugging time. Keep updated when
touching `plugins/camera/`.

Android (sender) counterpart: `elpinguinoopensource/kdeconnect-android` →
`docs/camera-webcam.md`.

## Status (2026-09-04)

| Milestone | State |
|---|---|
| DESK-1 plugin skeleton (DBus API, capabilities, CMake) | ✅ done |
| DESK-2 StreamWriter (payload → ffmpeg → /dev/videoN, backpressure, lifecycle) | ✅ done |
| DESK-3 DBus interfaces + CameraPage.qml + DevicePage entry + config | ✅ done |
| DESK-4 QUERYCAP device detection (skip real webcams) | ✅ done |
| Low-latency ffmpeg args + nobuffer regression resolved | ✅ done |
| Phase 0 validated end-to-end on real hardware | ✅ **GREEN** |
| PR | [kdeconnect#1](https://github.com/elpinguinoopensource/kdeconnect/pull/1) |

Validated against the Android plugin on **Redmi Note 9S** over LAN:
**30–31 fps sustained** measured on `/dev/video10`, live frames, ~4 Mbps at
640×480, `stopCamera` leaves zero orphaned ffmpeg processes.

## Pipeline (receiver side)

```
kdeconnect.camera.stream (payloadSize=-1, streamed over TLS)
  ──► CameraPlugin ──► StreamWriter (reassemble + watermarks)
  ──► ffmpeg -f h264 -i pipe:0 ──► /dev/videoN (v4l2loopback) ──► any V4L2 app
```

## Wire protocol

Single source of truth: `CameraProtocol.kt` in the Android repo. Summary:

| Type | Direction | Notes |
|---|---|---|
| `kdeconnect.camera.list` | both | request / reply (`cameras` array) |
| `kdeconnect.camera.start` | desktop→android | `cameraId, width, height, fps, bitrate` — **bitrate in bps** |
| `kdeconnect.camera.stream` | android→desktop | H.264 Annex-B as streamed payload; body has `width/height/fps` used to configure ffmpeg |
| `kdeconnect.camera.stop` | both | remote stop (watchdog/user) also arrives from the phone |
| `kdeconnect.camera.error` | android→desktop | `in_use` \| `denied` \| `unsupported` \| `disconnected` \| `stopped` |

Core plumbing relied upon (verified in `core/`):
- Incoming packet with payload → `LanDeviceLink::dataReceived` opens a TLS
  client socket to the phone's `transferInfo` port and hands the plugin an
  already-connected `QSslSocket` via `np.payload()`; read on `readyRead`
  (same pattern as `core/filetransferjob.cpp`).
- An **unrouted** packet with payload is closed by `core/device.cpp` — the
  plugin must close the payload itself when the stream ends.
- Plugin loading intersects capabilities: our plugin loads even on devices
  without camera support (harmless, no packets arrive).

## Architecture map (this repo)

| File | Responsibility |
|---|---|
| `plugins/camera/cameraplugin.{h,cpp}` | DBus API (`org.kde.kdeconnect.device.camera`), packet routing, ffmpeg lifecycle |
| `plugins/camera/streamwriter.{h,cpp}` | payload → ffmpeg stdin; 512 KB/256 KB backpressure watermarks; device selection via `VIDIOC_QUERYCAP` (`V4L2_CAP_VIDEO_OUTPUT`); debug raw dump |
| `plugins/camera/kdeconnect_camera.json` | incoming/outgoing capabilities |
| `plugins/camera/kdeconnect_camera_config.qml` | plugin config (default width/height/fps/bitrate) |
| `plugins/camera/README.md` | **v4l2loopback setup, requirements, troubleshooting** |
| `dbusinterfaces/dbusinterfaces.{h,cpp}` | `CameraDeviceDbusInterface` + `CameraDbusInterfaceFactory` |
| `declarativeplugin/kdeconnectdeclarativeplugin.h` | QML type registration |
| `app/qml/CameraPage.qml`, `app/qml/DevicePage.qml` | user UI (picker, resolution/fps/bitrate, start/stop) |
| `tests/camerastreamtest.cpp` | 7 unit tests for stream plumbing (needs `-DBUILD_TESTING=ON`) |

## Build & test environment (Docker)

The host (openSUSE Tumbleweed) has **no KF6 devel packages and no passwordless
sudo**, so builds run in a container: `kde-build` = `opensuse/tumbleweed`,
repo mounted read-only at `/src`, build dirs `/build` (tests off) and
`/build-test` (tests on).

Dependency gotchas for the recipe:

1. `zypper install libsystemd0`, then
   `rpm -e --nodeps libsystemd0 && rpm -Uvh --nodeps libsystemd0-mini` plus
   `qt6-gui-private-devel` + `qt6-base-private-devel` (from
   `/var/cache/zypp/packages`) — Qt ≥6.11 needs Qt6GuiPrivate and the
   mini/full libsystemd packages file-conflict.
2. `zypper install --oldpackage libpcre2-8-0=10.47-1.6` (pin required by
   qt6-gui-devel).
3. Correct package names: `kf6-extra-cmake-modules`, `qt6-base-devel`,
   `libopenssl-devel`, `pulseaudio-qt6-devel`, `kirigami-addons6-devel`
   (there is **no** `extra-cmake-modules` / `openssl-devel` / `qt6-devel`).

Build & deploy loop for the plugin:

```sh
docker exec kde-build bash -c "cd /build && cmake --build . --target kdeconnect_camera -j4"
docker cp kde-build:/build/bin/kdeconnect/kdeconnect_camera.so /tmp/kde-e2e/lib64/qt6/plugins/kdeconnect/
# tests:
docker exec kde-build bash -c "cd /build-test && ctest -R camera --output-on-failure"
```

Style checks used by the verification loop:
`qmllint` at `/usr/lib64/qt6/bin/qmllint` (import warnings are expected),
and `grep -P '[^\x00-\x7F]'` over changed files must be empty (repo is ASCII;
also: all UI strings/messages in English).

## Running the E2E test daemon

```sh
# system daemon must be stopped — ours owns org.kde.kdeconnect
pkill -f /usr/bin/kdeconnectd
cd /tmp/kde-e2e
LD_LIBRARY_PATH=/tmp/kde-e2e/lib64 \
QT_PLUGIN_PATH=/tmp/kde-e2e/lib64/qt6/plugins \
XDG_RUNTIME_DIR=/run/user/1000 \
QT_ASSUME_STDERR_HAS_CONSOLE=1 \
QT_LOGGING_RULES='kdeconnect.core=true;kdeconnect.plugin.camera=true' \
./bin/kdeconnectd > /tmp/kde-e2e.log 2>&1 &
```

Notes:
- `QT_LOGGING_TO_CONSOLE` is deprecated; without `QT_ASSUME_STDERR_LOGGING`/
  `..._HAS_CONSOLE` the logging rules never reach the file.
- The daemon **device id is stable** across restarts (derived from
  `~/.config/kdeconnect/certificate.pem`) — pairing survives daemon swaps.
- After killing/restarting the daemon repeatedly, the phone may keep a TCP
  socket to the old process and never re-handshake. Fix: force-stop the app
  and relaunch `org.kde.kdeconnect_tp.debug/org.kde.kdeconnect.ui.MainActivity`.
- Devices that don't advertise camera capabilities (e.g. a stock Galaxy S24
  Ultra) won't load the plugin — expected, not a bug.

## DBus test recipes

Interface is `org.kde.kdeconnect.device.camera` (note the `.device.`), object
path `/modules/kdeconnect/devices/<deviceId>/camera`. `startCamera` takes
**five** args `(s,i,i,i,i)` and bitrate is **bps**:

```sh
CAM=/modules/kdeconnect/devices/ae460ab747e741c1823240a4f90c4170/camera
dbus-send --session --dest=org.kde.kdeconnect --print-reply $CAM \
  org.kde.kdeconnect.device.camera.startCamera \
  string:"1" int32:640 int32:480 int32:30 int32:4000000
# streaming property / stop:
dbus-send --session --dest=org.kde.kdeconnect --print-reply --type=method_call \
  $CAM org.freedesktop.DBus.Properties.Get string:org.kde.kdeconnect.device.camera string:streaming
dbus-send --session --dest=org.kde.kdeconnect --print-reply $CAM \
  org.kde.kdeconnect.device.camera.stopCamera
```

Measure live fps on the loopback while streaming:

```sh
ffmpeg -hide_banner -f v4l2 -video_size 640x480 -i /dev/video10 -t 2 -f null - 2>&1 | tail -1
# snapshot (device is exclusive — stop the stream first or ffmpeg fails with EBUSY):
timeout 10 ffmpeg -f v4l2 -video_size 640x480 -i /dev/video10 -frames:v 1 -y /tmp/webcam_view.jpg
```

## ffmpeg gotchas — read before touching `buildArguments()`

The final, hardware-verified argument set:

```
ffmpeg -hide_banner -loglevel warning -probesize 131072 -analyzeduration 0
       -flags low_delay -threads 0 -thread_type slice -f h264 -framerate <fps> -i pipe:0
       -f v4l2 -pix_fmt yuv420p /dev/videoN
```

- **DO NOT add `-fflags nobuffer`.** The Qualcomm SM6125 encoder prefixes every
  access unit with an empty AUD NAL (`00 00 00 01 00`); with nobuffer the h264
  demuxer emits those as zero-length packets, the filter graph dies with
  EINVAL, frame count freezes at ~2 and the pipeline stalls. Synthetic x264
  streams (no AUD) do **not** reproduce it — only real phone streams do. This
  regression cost a full debug cycle; the rationale is also a code comment at
  `streamwriter.cpp` (~line 228).
- `-threads 0 -thread_type slice`, not `-thread_count 1` (the latter is the
  x264/openh264 private name; ffmpeg exits at argument parsing with code 8
  "Unrecognized option"). `-threads 0` lets ffmpeg pick the thread count, and
  restricting parallelism to slice threading decodes several slices of the
  *same* frame concurrently without ever holding a frame back. The default
  frame threading pipelines 4–6 frames (~200 ms at 30 fps).
- `probesize 131072` + `analyzeduration 0`: defaults buffer up to 5 MB/5 s of
  stream before the first frame. SPS+PPS+IDR are in-band, 128 KB is plenty.
- The raw stream has no timing metadata; consumers must force `-framerate`
  (ffprobe reports a bogus `r_frame_rate` on the Annex-B dump — harmless).

## Debug instrumentation

- **Raw bitstream dump**: set `KDECONNECT_CAMERA_DUMP=/tmp/camera.h264` in the
  daemon's environment before starting a stream. StreamWriter mirrors the
  exact Annex-B bytes it feeds to ffmpeg's stdin (no-op when unset). Analyze
  offline with `ffprobe -show_frames` / `ffmpeg -f h264 -i dump ...`. This is
  how the nobuffer regression was root-caused without guessing.
- **Orphan check** after stop: `pgrep -x ffmpeg` must be empty.
- **Log tail**: `journalctl --user -u kdeconnectd` for the installed daemon,
  or the `/tmp/kde-e2e.log` redirect for the test one.
- **Do not `truncate -s 0`** a log file a running daemon holds open — writes
  go into the hole and the file stays 0 bytes. Delete + restart instead.

## Known gaps / next steps

- [ ] Expose `StreamWriter::devicePath` over DBus (CameraPage status label
      shows a literal "/dev/videoX").
- [ ] Mid-stream bitrate/resolution renegotiation.
- [ ] Camera selection by facing in the UI.
- [ ] v4l2loopback auto-load helper (needs root/polkit; README documents
      `modprobe` instead — deliberately out of Phase 0 scope).
- [ ] End-to-end latency number (optical-clock test; see Android doc).
- [ ] Upstream coordination with KDE Connect MR !251 (`remotevideo`): distinct
      protocol, no collision, but align before upstreaming.

## Resources

- Plugin user/setup docs: [`plugins/camera/README.md`](../plugins/camera/README.md).
- Task specs (kept local, gitignored): `memory/tasks/DESK-*.md`.
- Upstream repo (where patches ultimately go): <https://invent.kde.org/network/kdeconnect-kde>
  / <https://github.com/KDE/kdeconnect-kde>.
- KDE Connect dev wiki: <https://community.kde.org/KDEConnect>; bugs: <https://bugs.kde.org>.
- v4l2loopback: <https://github.com/umlaeute/v4l2loopback>.
- V4L2 output-device API (`VIDIOC_QUERYCAP`): <https://kernel.org/doc/html/latest/userspace-api/media/v4l/dev_output.html>.
- ffmpeg h264 demuxer options: <https://ffmpeg.org/ffmpeg-formats.html#h264-1>.
