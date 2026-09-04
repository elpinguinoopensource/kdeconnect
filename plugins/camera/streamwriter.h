/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSharedPointer>
#include <QString>

/**
 * Pipes a live H.264 Annex-B byte stream coming from a QIODevice (the payload of a
 * kdeconnect.camera.stream packet, an already connected QSslSocket) into an ffmpeg
 * child process which writes it out to a v4l2loopback /dev/video device.
 *
 * The source device is injected via the constructor to keep the class unit-testable
 * (tests can pass a QBuffer or any other QIODevice).
 *
 * The source payload is never closed while the stream is active, but it is always
 * closed when the stream ends (stop(), ffmpeg failure/exit, source closed remotely)
 * so the socket cannot leak.
 */
class StreamWriter : public QObject
{
    Q_OBJECT
public:
    StreamWriter(QSharedPointer<QIODevice> source, int width, int height, int fps, QObject *parent = nullptr);
    ~StreamWriter() override;

    Q_DISABLE_COPY_MOVE(StreamWriter)

    /// Spawns ffmpeg. Emits failed() (and stays inactive) if the payload is not
    /// open, ffmpeg is missing, or no v4l2loopback device is usable.
    void start();

    /// Tears the pipeline down: flushes pending bytes, closes ffmpeg stdin, waits
    /// for ffmpeg to exit (killing it if needed) and closes the source device.
    /// Safe to call when the writer was never started or already stopped.
    void stop();

    bool isRunning() const;

    /// v4l2loopback device fed by ffmpeg, empty if auto-detection found none.
    QString devicePath() const;

    /// Overrides the auto-detected output device. Only meant for tests.
    void setDevicePath(const QString &path);

    int width() const;
    int height() const;
    int fps() const;

Q_SIGNALS:
    /// The stream ended cleanly (source EOF or stop() after a successful run).
    void finished();
    /// The pipeline failed or was torn down abnormally.
    void failed(const QString &reason);

private:
    // Memory bounds for the ffmpeg write queue while draining the source.
    // Kept small on purpose: this queue is pure added latency (bytes sitting
    // here are stale frames). At 4 Mbps, 512 KB ≈ 1 s. When the high mark is
    // hit we pause draining; the backlog then moves to the phone, whose
    // drop-oldest stream buffer trims it and (since the session calls
    // requestSync() after a drop) recovers with a fresh IDR.
    static constexpr qint64 HighWaterMark = 512 * 1024;
    static constexpr qint64 LowWaterMark = 256 * 1024;

    void connectSource();
    void disconnectSource();
    void drainSource();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus status);
    void closeSource();

    QSharedPointer<QIODevice> m_source;
    QPointer<QProcess> m_process;
    QMetaObject::Connection m_sourceConnection;
    QByteArray m_buffer;
    QByteArray m_stderrTail;
    QFile m_dumpFile;
    int m_width;
    int m_height;
    int m_fps;
    QString m_devicePath;
    bool m_running = false;
    bool m_draining = true;
    bool m_sourceConnected = false;
};
