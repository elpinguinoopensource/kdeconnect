/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "streamwriter.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

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
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "No writable /dev/video device found (is v4l2loopback loaded?)";
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
    //  - threads 1: the default frame-parallel decoder pipelines 4-6 frames
    //    (~200 ms at 30 fps) before emitting the first one. NOTE: the ffmpeg
    //    option is "-threads"; "-thread_count" is the x264/openh264 private
    //    name and makes ffmpeg abort at argument parsing (code 8).
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
                    QStringLiteral("1"),
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
        connectSource();
        if (m_source->bytesAvailable() > 0) {
            drainSource();
        }
    }
}

void StreamWriter::stop()
{
    disconnectSource();

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

    m_buffer.append(m_source->readAll());

    if (m_dumpFile.isOpen()) {
        m_dumpFile.write(m_buffer);
    }

    QProcess *process = m_process.data();
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
