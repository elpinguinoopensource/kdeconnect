/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

/**
 * Keeps a v4l2loopback /dev/video device alive with a static cover image
 * (the embedded KDE Connect logo, plugins/camera/camera-idle.png) while no
 * live camera stream is running.
 *
 * Solves two problems at once:
 *  - The webcam shows a branded "idle screen" instead of a black frame.
 *  - When the live ffmpeg (StreamWriter) dies, v4l2loopback would otherwise
 *    freeze the last frame and stall open capture apps; a continuously fed
 *    idle writer keeps the node live so the transition back to video is
 *    immediate and restarts visibly update the webcam.
 *
 * Limitations (v1, by design):
 *  - No crash-restart/watchdog loop: if the cover ffmpeg exits or errors,
 *    failed() is emitted and the writer stays inactive until start() is
 *    called again by the owner.
 *  - The cover is a static image, so a low framerate (5 fps) is enough.
 */
class IdleWriter : public QObject
{
    Q_OBJECT
public:
    explicit IdleWriter(QObject *parent = nullptr);
    ~IdleWriter() override;

    Q_DISABLE_COPY_MOVE(IdleWriter)

    /// Start looping the embedded cover image into the v4l2loopback device.
    /// No-op if already running. Returns false (and stays inactive) if ffmpeg
    /// or a video-output device is unavailable.
    bool start();

    /// Tear down the ffmpeg child. Safe to call when not running.
    void stop();

    bool isRunning() const;

    /// v4l2loopback device fed by ffmpeg, empty until detection succeeds
    /// (detection is lazy, done inside start()) or was overridden.
    QString devicePath() const;

    /// Overrides the auto-detected output device. Only meant for tests.
    void setDevicePath(const QString &path);

    /// Test override: use this PNG instead of the embedded qrc copy.
    void setImagePath(const QString &path);

    /// Builds the ffmpeg argument vector for the cover feed. Factored out of
    /// start() and public so it can be asserted without a device or a running
    /// ffmpeg: the "-re" pacing flag MUST precede "-i", otherwise ffmpeg
    /// ignores the logical -framerate and floods the v4l2 node at full speed
    /// (v4l2loopback never blocks a writer with no reader), burning CPU.
    static QStringList ffmpegArgs(const QString &cover, const QString &device, int width, int height);

Q_SIGNALS:
    /// The cover writer failed or was torn down abnormally.
    void failed(const QString &reason);

private:
    /// Extracted qrc copy of the cover image, or the test override.
    /// Returns an empty string if the cover is not usable.
    QString coverPath() const;

    void handleProcessFinished(int exitCode, QProcess::ExitStatus status);

    QPointer<QProcess> m_process;
    QByteArray m_stderrTail;
    QString m_devicePath;
    QString m_imageOverride;
    bool m_running = false;
};
