/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QIODevice>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSharedPointer>
#include <QString>
#include <QTimer>

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

    /// Overrides the activity-watchdog timings: how long a freshly started
    /// pipeline may stay silent before it is declared stalled (graceMs), and
    /// how long a previously active pipeline may go without a single source
    /// byte before the same verdict (tickMs). Only meant for tests, which
    /// cannot afford to wait the production 15 s/5 s.
    void setActivityWatchdog(int graceMs, int tickMs);

    int width() const;
    int height() const;
    int fps() const;

Q_SIGNALS:
    /// The stream ended cleanly (source EOF or stop() after a successful run).
    void finished();
    /// The pipeline failed or was torn down abnormally.
    void failed(const QString &reason);
    /// Periodic congestion snapshot (every StatsIntervalMs while running).
    /// Feeds the phone's adaptive bitrate controller: the plugin forwards it as
    /// a kdeconnect.camera.stats packet so Android can lower the encoder bitrate
    /// BEFORE the drop-oldest buffer starts discarding frames.
    ///
    /// \param backlogBytes bytes still pending in ffmpeg's write queue
    /// \param paused true while the drain is paused at HighWaterMark
    void statsTicked(qint64 backlogBytes, bool paused);

private:
    // Memory bounds for the ffmpeg write queue while draining the source.
    // Kept small on purpose: this queue is pure added latency (bytes sitting
    // here are stale frames). At 4 Mbps, 512 KB ≈ 1 s. When the high mark is
    // hit we pause draining; the backlog then moves to the phone, whose
    // drop-oldest stream buffer trims it and (since the session calls
    // requestSync() after a drop) recovers with a fresh IDR.
    static constexpr qint64 HighWaterMark = 512 * 1024;
    static constexpr qint64 LowWaterMark = 256 * 1024;

    // Activity watchdog: detects a silent phone whose socket stayed open (the
    // TCP keepalive alone takes ~60 s to notice, and QSslSocket may never
    // notice at all). A freshly started pipeline gets StartGraceMs before the
    // watchdog can fire (ffmpeg probing + phone encoder ramp-up); after that,
    // ActivityTickMs without a single source byte means the stream is dead.
    static constexpr int StartGraceMs = 15000;
    static constexpr int ActivityTickMs = 5000;

    // Stats reporter: how often the congestion snapshot (write-queue backlog +
    // drain state) is emitted for the phone's adaptive bitrate controller.
    // 2 s is a compromise — fast enough to react before the phone's ~1.5 s
    // drop-oldest buffer overflows, slow enough that the extra packets are
    // negligible on the LAN (a few tens of bytes every two seconds).
    static constexpr int StatsIntervalMs = 2000;

    void connectSource();
    void disconnectSource();
    void drainSource();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus status);
    void handleActivityTimeout();
    void closeSource();

    QSharedPointer<QIODevice> m_source;
    QPointer<QProcess> m_process;
    QMetaObject::Connection m_sourceConnection;
    QByteArray m_buffer;
    QByteArray m_stderrTail;
    QFile m_dumpFile;
    QTimer m_activityTimer;
    /// Periodic congestion reporter (see statsTicked()); running only while the
    /// pipeline is up. Parented to `this` implicitly (member), so it dies with
    /// the writer and its connection to the plugin is dropped automatically.
    QTimer m_statsTimer;
    QElapsedTimer m_uptime;
    int m_activityGraceMs = StartGraceMs;
    int m_activityTickMs = ActivityTickMs;
    int m_width;
    int m_height;
    int m_fps;
    QString m_devicePath;
    bool m_running = false;
    bool m_draining = true;
    bool m_sourceConnected = false;
};
