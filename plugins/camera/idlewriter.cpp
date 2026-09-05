/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "idlewriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "plugin_camera_debug.h"

namespace {

/// True if the given /dev/video node advertises V4L2_CAP_VIDEO_OUTPUT (i.e. it
/// is a v4l2loopback device, not a physical webcam). Duplicated from
/// streamwriter.cpp on purpose: the two writers stay self-contained and a
/// later cleanup may factor the helpers out into a shared translation unit.
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

/// Current negotiated frame size of the device via VIDIOC_G_FMT, falling back
/// to 1280x720 when the query is unavailable (device busy, no format set yet,
/// not a real node). Opening read-write is best-effort: some loopback nodes
/// only answer on O_RDONLY.
void queryDeviceSize(const QString &path, int &width, int &height)
{
    width = 1280;
    height = 720;

    const QByteArray enc = QFile::encodeName(path);
    int fd = ::open(enc.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fd = ::open(enc.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0) {
        return;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (::ioctl(fd, VIDIOC_G_FMT, &fmt) == 0 && fmt.fmt.pix.width > 0 && fmt.fmt.pix.height > 0) {
        width = static_cast<int>(fmt.fmt.pix.width);
        height = static_cast<int>(fmt.fmt.pix.height);
    }
    ::close(fd);
}

/// Where the embedded cover PNG is unpacked to on disk. ffmpeg cannot read
/// qrc: URLs, so the resource is copied here once and reused.
QString coverTempPath()
{
    return QDir(QDir::tempPath()).filePath(QStringLiteral("kdeconnect-camera-idle.png"));
}

} // namespace

IdleWriter::IdleWriter(QObject *parent)
    : QObject(parent)
{
}

IdleWriter::~IdleWriter()
{
    stop();
}

bool IdleWriter::isRunning() const
{
    return m_running;
}

QString IdleWriter::devicePath() const
{
    return m_devicePath;
}

void IdleWriter::setDevicePath(const QString &path)
{
    m_devicePath = path;
}

void IdleWriter::setImagePath(const QString &path)
{
    m_imageOverride = path;
}

QString IdleWriter::coverPath() const
{
    // A test override bypasses resource extraction entirely.
    if (!m_imageOverride.isEmpty()) {
        return QFile::exists(m_imageOverride) ? m_imageOverride : QString();
    }

    QFile resource(QStringLiteral(":/camera/camera-idle.png"));
    if (!resource.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QByteArray data = resource.readAll();
    resource.close();
    if (data.isEmpty()) {
        return QString();
    }

    const QString tempFile = coverTempPath();
    const QFileInfo existing(tempFile);
    if (existing.exists() && existing.size() == data.size()) {
        return tempFile; // Already unpacked with matching size, reuse it.
    }

    QSaveFile out(tempFile);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
        return QString();
    }
    return tempFile;
}

bool IdleWriter::start()
{
    if (m_running) {
        return true; // Idempotent.
    }
    m_stderrTail.clear();

    // The cover is checked before the device so a broken/unreadable image is
    // reported synchronously as cover_read_failed / cover_extract_failed even
    // when no v4l2loopback node is present (see tests/idlewritertest.cpp).
    const QString cover = coverPath();
    if (cover.isEmpty()) {
        if (!m_imageOverride.isEmpty()) {
            Q_EMIT failed(QStringLiteral("cover_read_failed"));
        } else {
            Q_EMIT failed(QStringLiteral("cover_extract_failed"));
        }
        return false;
    }

    // Detect lazily so a v4l2loopback module loaded after construction is
    // picked up. An explicit override (tests) short-circuits detection.
    if (m_devicePath.isEmpty()) {
        m_devicePath = findVideoOutputDevice();
    }
    if (m_devicePath.isEmpty()) {
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "IdleWriter: no writable /dev/video device found";
        Q_EMIT failed(QStringLiteral("no_v4l2_device"));
        return false;
    }

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "IdleWriter: ffmpeg not found in PATH";
        Q_EMIT failed(QStringLiteral("ffmpeg_not_found"));
        return false;
    }

    int width = 1280;
    int height = 720;
    queryDeviceSize(m_devicePath, width, height);

    // Centre the logo at any consumer resolution on the same #232629
    // background used by the PNG, and letterbox it (decrease + pad). 5 fps is
    // plenty for a static image and keeps CPU usage near zero.
    const QString filter = QStringLiteral(
                               "scale=%1:%2:force_original_aspect_ratio=decrease,"
                               "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=0x232629,format=yuv420p")
                               .arg(width)
                               .arg(height);

    QProcess *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_process = process;
    m_running = true;

    connect(process,
            &QProcess::readyReadStandardOutput,
            this,
            [this, process] {
                // Channels are merged, so ffmpeg stderr arrives here. Keep a
                // small tail of it to attach to failure reports.
                m_stderrTail.append(process->readAllStandardOutput());
                static constexpr int maxTail = 4096;
                if (m_stderrTail.size() > maxTail) {
                    m_stderrTail.remove(0, m_stderrTail.size() - maxTail);
                }
            });

    connect(process, &QProcess::finished, this, &IdleWriter::handleProcessFinished);

    connect(process,
            &QProcess::errorOccurred,
            this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || !m_running) {
                    return;
                }
                m_running = false;
                m_process = nullptr;
                process->deleteLater();
                Q_EMIT failed(QStringLiteral("ffmpeg_failed_to_start"));
            });

    qCDebug(KDECONNECT_PLUGIN_CAMERA) << "IdleWriter: starting cover ffmpeg" << width << 'x' << height
                                      << "->" << m_devicePath;

    process->start(ffmpeg,
                   {QStringLiteral("-hide_banner"),
                    QStringLiteral("-loglevel"),
                    QStringLiteral("warning"),
                    QStringLiteral("-loop"),
                    QStringLiteral("1"),
                    QStringLiteral("-framerate"),
                    QStringLiteral("5"),
                    QStringLiteral("-i"),
                    cover,
                    QStringLiteral("-vf"),
                    filter,
                    QStringLiteral("-f"),
                    QStringLiteral("v4l2"),
                    m_devicePath});

    return m_running;
}

void IdleWriter::stop()
{
    QProcess *process = m_process.data();
    m_process = nullptr;
    const bool wasRunning = m_running;
    m_running = false;

    if (process) {
        // Idle frames are disposable: ffmpeg -loop 1 never exits on its own,
        // so there is no graceful closeWriteChannel path here, just a kill.
        process->kill();
        process->waitForFinished(1000);
        process->deleteLater();
    }

    if (wasRunning) {
        qCDebug(KDECONNECT_PLUGIN_CAMERA) << "IdleWriter: cover pipeline stopped";
    }
}

void IdleWriter::handleProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess *process = qobject_cast<QProcess *>(sender());
    m_process = nullptr;
    const bool wasRunning = m_running;
    m_running = false;
    if (process) {
        process->deleteLater();
    }

    if (!wasRunning) {
        return; // Already torn down by stop().
    }

    if (status == QProcess::CrashExit || exitCode != 0) {
        const QString tail = QString::fromLocal8Bit(m_stderrTail).trimmed();
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "IdleWriter: ffmpeg exited with code" << exitCode << status << tail;
        Q_EMIT failed(tail.isEmpty() ? QStringLiteral("ffmpeg_exit_%1").arg(exitCode) : tail);
    } else {
        // A clean exit is unexpected for a -loop 1 cover; report it as a
        // generic failure so the owner can decide whether to restart.
        Q_EMIT failed(QStringLiteral("ffmpeg_exit_0"));
    }
}

#include "moc_idlewriter.cpp"
