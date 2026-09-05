/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "streamwriter.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "plugin_camera_debug.h"

namespace {

/// True if the given /dev/video node advertises V4L2_CAP_VIDEO_OUTPUT (i.e. it
/// is a v4l2loopback device, not a physical webcam).
bool isVideoOutputDevice(const QString &path)
{
    const QByteArray enc = QFile::encodeName(path);
    int fd = ::open(enc.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EBUSY) {
            fd = ::open(enc.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
        if (fd < 0) {
            return false;
        }
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    const bool ok = ::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0
        && (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT);
    ::close(fd);
    return ok;
}

/// First /dev/video* node advertising video output capability, empty string if
/// there is none.
QString findVideoOutputDevice()
{
    const QDir devDir(QStringLiteral("/dev"));
    const QStringList nodes = devDir.entryList({QStringLiteral("video*")}, QDir::System, QDir::Name);
    for (const QString &node : nodes) {
        const QString path = devDir.absoluteFilePath(node);
        if (isVideoOutputDevice(path)) {
            return path;
        }
    }
    return QString();
}

} // namespace

StreamWriter::StreamWriter(QSharedPointer<QIODevice> source, int width, int height, int fps, QObject *parent)
    : QObject(parent)
    , m_source(std::move(source))
    , m_width(width)
    , m_height(height)
    , m_fps(fps)
    , m_devicePath(findVideoOutputDevice())
{
    // Activity watchdog (see StartGraceMs/ActivityTickMs in the header): a
    // singleShot restarted on every batch of source bytes. If it ever fires,
    // the phone stopped sending while keeping the socket open.
    m_activityTimer.setSingleShot(true);
    connect(&m_activityTimer, &QTimer::timeout, this, &StreamWriter::handleActivityTimeout);

    // Congestion reporter: every StatsIntervalMs, tell the plugin how much
    // data is still queued for ffmpeg and whether the drain is paused, so the
    // phone can adapt its encoder bitrate before frames start getting dropped.
    m_statsTimer.setInterval(StatsIntervalMs);
    connect(&m_statsTimer, &QTimer::timeout, this, [this] {
        const qint64 backlog = m_process ? m_process->bytesToWrite() : 0;
        Q_EMIT statsTicked(backlog, !m_draining);
    });

    if (!m_source) {
        return;
    }

    // The stream is over when the source closes, either remotely (LanDeviceLink
    // emits readChannelFinished on socket disconnect) or locally via close().
    // Guarded by m_running so our own closeSource() does not re-trigger it.
    auto endOfStream = [this] {
        if (!m_running) {
            return;
        }
        stop();
        Q_EMIT finished();
    };
    connect(m_source.data(), &QIODevice::aboutToClose, this, endOfStream);
    connect(m_source.data(), &QIODevice::readChannelFinished, this, endOfStream);
}

StreamWriter::~StreamWriter()
{
    stop();
}

bool StreamWriter::isRunning() const
{
    return m_running;
}

QString StreamWriter::devicePath() const
{
    return m_devicePath;
}

void StreamWriter::setDevicePath(const QString &path)
{
    m_devicePath = path;
}

void StreamWriter::setActivityWatchdog(int graceMs, int tickMs)
{
    m_activityGraceMs = graceMs;
    m_activityTickMs = tickMs;
}

int StreamWriter::width() const
{
    return m_width;
}

int StreamWriter::height() const
{
    return m_height;
}

int StreamWriter::fps() const
{
    return m_fps;
}

void StreamWriter::start()
{
    if (m_running) {
        return;
    }
    m_buffer.clear();
    m_stderrTail.clear();

    if (!m_source || !m_source->isOpen() || !m_source->isReadable()) {
        Q_EMIT failed(QStringLiteral("no_open_payload"));
        return;
    }

    if (m_devicePath.isEmpty()) {
        qCWarning(KDECONNECT_PLUGIN_CAMERA)
            << "No /dev/video node with output capability found (is v4l2loopback loaded? "
               "exclusive_caps=1 makes the node capture-only and unusable as a sink)";
        closeSource();
        Q_EMIT failed(QStringLiteral("no_v4l2_device"));
        return;
    }

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "ffmpeg not found in PATH, cannot start camera stream";
        closeSource();
        Q_EMIT failed(QStringLiteral("ffmpeg_not_found"));
        return;
    }

    const int framerate = m_fps > 0 ? m_fps : 30;

    QProcess *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_process = process;
    m_running = true;
    m_draining = true;

    // Activity watchdog: arm it for this run. The pipeline is allowed to stay
    // silent during StartGraceMs (ffmpeg probing + encoder ramp-up), after
    // which every source byte postpones the stall verdict by another tick.
    m_uptime.start();
    m_activityTimer.start(m_activityGraceMs);

    // Debug-only: when KDECONNECT_CAMERA_DUMP is set, mirror the raw payload
    // bytes to a file so the live bitstream can be analysed offline.
    const QString dumpPath = qEnvironmentVariable("KDECONNECT_CAMERA_DUMP");
    if (!dumpPath.isEmpty()) {
        m_dumpFile.setFileName(dumpPath);
        if (!m_dumpFile.open(QIODevice::WriteOnly)) {
            qCWarning(KDECONNECT_PLUGIN_CAMERA) << "Cannot open camera dump file" << dumpPath;
        }
    }

    connect(process,
            &QProcess::readyReadStandardOutput,
            this,
            [this, process] {
                // Channels are merged, so ffmpeg stderr arrives here. Keep a small
                // tail of it to attach to failure reports.
                m_stderrTail.append(process->readAllStandardOutput());
                static constexpr int maxTail = 4096;
                if (m_stderrTail.size() > maxTail) {
                    m_stderrTail.remove(0, m_stderrTail.size() - maxTail);
                }
            });

    connect(process,
            &QProcess::bytesWritten,
            this,
            [this](qint64) {
                // Resume draining the source once ffmpeg's queue is below the low mark.
                if (!m_draining && m_process && m_process->bytesToWrite() < LowWaterMark) {
                    m_draining = true;
                    connectSource();
                    if (m_source && m_source->bytesAvailable() > 0) {
                        drainSource();
                    }
                }
            });

    connect(process, &QProcess::finished, this, &StreamWriter::handleProcessFinished);

    connect(process,
            &QProcess::errorOccurred,
            this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || !m_running) {
                    return;
                }
                m_running = false;
                m_process = nullptr;
                disconnectSource();
                m_activityTimer.stop();
                m_statsTimer.stop();
                m_buffer.clear();
                closeSource();
                process->deleteLater();
                Q_EMIT failed(QStringLiteral("ffmpeg_failed_to_start"));
            });

    qCDebug(KDECONNECT_PLUGIN_CAMERA) << "Starting ffmpeg" << m_width << 'x' << m_height << '@' << framerate
                                      << "->" << m_devicePath;

    // Low-latency flags. This is a LIVE Annex-B H.264 stream arriving on a
    // pipe, so every byte must reach the decoder immediately; the defaults
    // are tuned for files and add seconds of delay:
    //  - probesize/analyzeduration: avformat_find_stream_info() would buffer
    //    up to 5 MB / 5 s of the stream while probing before decoding starts.
    //    128 KB is plenty to see SPS+PPS+first IDR (they are in-band).
    //  - low_delay: tell the decoder not to reorder/delay frames.
    //  - threads 0 + thread_type slice: let ffmpeg pick the thread count, but
    //    restrict parallelism to slice threading, i.e. decoding several slices
    //    of the SAME frame concurrently. The default frame threading keeps a
    //    decoder pool of 4-6 frames in flight (~200 ms at 30 fps) before the
    //    first frame is emitted: pure pipeline latency on a live stream. Slice
    //    threading never holds a frame back (no reordering, no lookahead), so
    //    it costs no extra latency, and when the encoder ships a single slice
    //    per access unit (as many phone encoders do) it simply degrades to
    //    serial decoding. NOTE: the ffmpeg option is "-threads";
    //    "-thread_count" is the x264/openh264 private name and makes ffmpeg
    //    abort at argument parsing (code 8).
    //  - NO "-fflags nobuffer": the Qualcomm encoder prefixes every access
    //    unit with an empty AUD NAL (00 00 00 01 00). With nobuffer the
    //    demuxer emits those as zero-length packets, the filter graph dies
    //    with EINVAL (frame count freezes at ~2) and the pipeline stalls
    //    after the first frame. Measured on Redmi Note 9S (SM6125) streams;
    //    synthetic x264 streams (no AUD) do not reproduce it.
    process->start(ffmpeg,
                   {QStringLiteral("-hide_banner"),
                    QStringLiteral("-loglevel"),
                    QStringLiteral("warning"),
                    QStringLiteral("-probesize"),
                    QStringLiteral("131072"),
                    QStringLiteral("-analyzeduration"),
                    QStringLiteral("0"),
                    QStringLiteral("-flags"),
                    QStringLiteral("low_delay"),
                    QStringLiteral("-threads"),
                    QStringLiteral("0"),
                    QStringLiteral("-thread_type"),
                    QStringLiteral("slice"),
                    QStringLiteral("-f"),
                    QStringLiteral("h264"),
                    QStringLiteral("-framerate"),
                    QString::number(framerate),
                    QStringLiteral("-i"),
                    QStringLiteral("pipe:0"),
                    QStringLiteral("-f"),
                    QStringLiteral("v4l2"),
                    QStringLiteral("-pix_fmt"),
                    QStringLiteral("yuv420p"),
                    m_devicePath});

    if (m_running) {
        // Congestion reporter for the phone's adaptive bitrate controller
        // (started once ffmpeg is up; see statsTicked()).
        m_statsTimer.start();
        connectSource();
        if (m_source->bytesAvailable() > 0) {
            drainSource();
        }
    }
}

void StreamWriter::stop()
{
    disconnectSource();
    m_activityTimer.stop();
    m_statsTimer.stop();

    QProcess *process = m_process.data();
    m_process = nullptr;
    const bool wasRunning = m_running;
    m_running = false;
    m_draining = true;

    if (process) {
        process->write(m_buffer);
        m_buffer.clear();
        process->closeWriteChannel(); // Closing stdin lets ffmpeg finish the file cleanly
        if (!process->waitForFinished(500)) {
            process->kill();
            process->waitForFinished(1000);
        }
        process->deleteLater();
    } else {
        m_buffer.clear();
    }

    m_dumpFile.close();

    closeSource();

    if (wasRunning) {
        qCDebug(KDECONNECT_PLUGIN_CAMERA) << "Stream pipeline stopped";
    }
}

void StreamWriter::handleProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess *process = qobject_cast<QProcess *>(sender());
    disconnectSource();
    m_activityTimer.stop();
    m_statsTimer.stop();
    m_process = nullptr;
    const bool wasRunning = m_running;
    m_running = false;
    m_buffer.clear();
    m_dumpFile.close();
    closeSource();
    if (process) {
        process->deleteLater();
    }

    if (!wasRunning) {
        return; // Already torn down by stop()
    }

    if (status == QProcess::CrashExit || exitCode != 0) {
        const QString tail = QString::fromLocal8Bit(m_stderrTail).trimmed();
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "ffmpeg exited with code" << exitCode << status << tail;
        Q_EMIT failed(tail.isEmpty() ? QStringLiteral("ffmpeg_exit_%1").arg(exitCode) : tail);
    } else {
        Q_EMIT finished();
    }
}

void StreamWriter::handleActivityTimeout()
{
    if (!m_running) {
        return; // Torn down between the timeout signal and this slot.
    }
    // Safety net: never declare a stall during the initial grace window. A
    // stray early burst of bytes may have re-armed the timer with the short
    // tick interval while the phone encoder is still ramping up.
    if (m_uptime.isValid() && m_uptime.elapsed() < m_activityGraceMs) {
        m_activityTimer.start(m_activityGraceMs - int(m_uptime.elapsed()));
        return;
    }
    qCWarning(KDECONNECT_PLUGIN_CAMERA) << "Camera stream produced no bytes for a full watchdog window, assuming the phone went silent";
    // stop() disconnects the source, reaps ffmpeg and closes the payload,
    // matching the teardown done by the other failure paths before failed().
    stop();
    Q_EMIT failed(QStringLiteral("stream_stalled"));
}

void StreamWriter::connectSource()
{
    if (m_sourceConnected || !m_source) {
        return;
    }
    m_sourceConnected = true;
    m_sourceConnection = connect(m_source.data(), &QIODevice::readyRead, this, &StreamWriter::drainSource);
}

void StreamWriter::disconnectSource()
{
    if (!m_sourceConnected) {
        return;
    }
    m_sourceConnected = false;
    disconnect(m_sourceConnection);
}

void StreamWriter::drainSource()
{
    if (!m_running || !m_source) {
        return;
    }

    QProcess *process = m_process.data();

    // Fast path: no backlog pending and ffmpeg's queue has room. Write the
    // freshly read bytes straight to the child; copying them through
    // m_buffer first would be a second full copy per byte on the hot path.
    if (m_buffer.isEmpty() && process && process->bytesToWrite() < HighWaterMark) {
        const QByteArray chunk = m_source->readAll();
        if (chunk.isEmpty()) {
            return;
        }
        // Activity watchdog: bytes arrived from the source, postpone a stall.
        m_activityTimer.start(m_activityTickMs);
        if (m_dumpFile.isOpen()) {
            m_dumpFile.write(chunk);
        }
        process->write(chunk);
        if (process->bytesToWrite() > HighWaterMark) {
            m_draining = false;
            disconnectSource();
            qCWarning(KDECONNECT_PLUGIN_CAMERA) << "ffmpeg write queue full, pausing source drain";
        }
        return;
    }

    // Slow path (ffmpeg still starting up, or draining a backlog after a
    // high-water pause): buffer first, then flush the whole thing at once.
    const QByteArray chunk = m_source->readAll();
    if (!chunk.isEmpty()) {
        m_activityTimer.start(m_activityTickMs); // Activity watchdog: source is alive.
    }
    m_buffer.append(chunk);

    if (m_buffer.isEmpty()) {
        return; // Nothing readable this round.
    }

    if (m_dumpFile.isOpen()) {
        m_dumpFile.write(m_buffer);
    }

    if (!process) {
        return; // Keep buffering until ffmpeg is up; bytesWritten/startup path flushes later
    }

    process->write(m_buffer);
    m_buffer.clear();

    if (process->bytesToWrite() > HighWaterMark) {
        m_draining = false;
        disconnectSource();
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "ffmpeg write queue full, pausing source drain";
    }
}

void StreamWriter::closeSource()
{
    if (m_source && m_source->isOpen()) {
        m_source->close();
    }
}

#include "moc_streamwriter.cpp"
