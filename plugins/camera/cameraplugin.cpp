/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "cameraplugin.h"

#include <KPluginFactory>

#include <QDBusConnection>
#include <QPointer>
#include <QVariantMap>

#include "plugin_camera_debug.h"
#include "streamwriter.h"
#include "idlewriter.h"

K_PLUGIN_CLASS_WITH_JSON(CameraPlugin, "kdeconnect_camera.json")

CameraPlugin::~CameraPlugin()
{
    // The device is going away: tear the pipeline down (stop() also closes the payload).
    stopWriter();
}

bool CameraPlugin::streaming() const
{
    return m_streamActive;
}

void CameraPlugin::setStreaming(bool streaming)
{
    if (m_streamActive == streaming) {
        return;
    }
    m_streamActive = streaming;
    Q_EMIT streamingChanged(m_streamActive);
}

QVariantList CameraPlugin::listCameras()
{
    return m_cameras;
}

void CameraPlugin::refreshCameraList()
{
    NetworkPacket np(PACKET_TYPE_CAMERA_LIST);
    sendPacket(np);
}

void CameraPlugin::startCamera(const QString &cameraId, int width, int height, int fps, int bitrate)
{
    QVariantMap body;
    if (!cameraId.isEmpty()) {
        body.insert(QStringLiteral("cameraId"), cameraId);
    }
    body.insert(QStringLiteral("width"), width);
    body.insert(QStringLiteral("height"), height);
    body.insert(QStringLiteral("fps"), fps);
    body.insert(QStringLiteral("bitrate"), bitrate);

    NetworkPacket np(PACKET_TYPE_CAMERA_START, body);
    sendPacket(np);

    // Stream is not up until the first packet arrives (handled in DESK-2).
    setStreaming(false);
}

void CameraPlugin::stopCamera()
{
    NetworkPacket np(PACKET_TYPE_CAMERA_STOP);
    sendPacket(np);

    // The host is authoritative for teardown: stop the local pipeline right away.
    const bool wasActive = m_streamActive;
    stopWriter();
    setStreaming(false);
    if (wasActive) {
        Q_EMIT streamStopped(QStringLiteral("stopped"));
    }
    resumeIdle();
}

void CameraPlugin::stopWriter()
{
    if (!m_writer) {
        return;
    }
    StreamWriter *writer = m_writer;
    m_writer = nullptr;
    writer->stop(); // also closes the payload device
    writer->deleteLater();
}

void CameraPlugin::ensureIdle()
{
    if (!m_idle) {
        m_idle = new IdleWriter(this);
        connect(
            m_idle,
            &IdleWriter::failed,
            this,
            [](const QString &reason) {
                // The cover is cosmetic: log only, never spam user-visible errors.
                qCWarning(KDECONNECT_PLUGIN_CAMERA) << "Idle cover writer failed:" << reason;
            });
    }
}

void CameraPlugin::resumeIdle()
{
    if (m_writer) {
        // A live stream owns the v4l2 node (e.g. a previous writer's failed()
        // racing a new stream packet): never start the cover on top of it.
        return;
    }
    if (!config()->getBool(QStringLiteral("showCover"), true)) {
        return;
    }
    ensureIdle();
    if (!m_idle->isRunning()) {
        m_idle->start(); // failure is logged by IdleWriter via failed()
    }
}

void CameraPlugin::connected()
{
    // Device just became reachable and no stream has run yet: show the cover.
    resumeIdle();
}

void CameraPlugin::handleStreamPacket(const NetworkPacket &np)
{
    QSharedPointer<QIODevice> payload = np.payload();

    if (m_writer) {
        // A stream is already running: ignore the new one but do not leak its socket.
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "Stream already active, ignoring duplicate stream packet";
        if (payload) {
            payload->close();
        }
        return;
    }

    if (!payload) {
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "Stream packet without payload";
        Q_EMIT errorReceived(QStringLiteral("no_payload"));
        return;
    }

    const int width = np.get<int>(QStringLiteral("width"));
    const int height = np.get<int>(QStringLiteral("height"));
    const int fps = np.get<int>(QStringLiteral("fps"));

    // Serialise the shared /dev/video node BEFORE constructing the StreamWriter:
    // its constructor auto-detects the v4l2loopback device by opening it, and
    // v4l2loopback rejects a second output opener while the cover ffmpeg is
    // still alive. stop() reaps the child (kill + waitForFinished), so by this
    // point the node is free for the live pipeline.
    if (m_idle && m_idle->isRunning()) {
        m_idle->stop();
    }

    StreamWriter *writer = new StreamWriter(payload, width, height, fps, this);
    m_writer = writer;

    QPointer<StreamWriter> writerGuard = writer;
    connect(writer,
            &StreamWriter::finished,
            this,
            [this, writerGuard] {
                const bool wasCurrent = (m_writer == writerGuard);
                if (wasCurrent) {
                    m_writer = nullptr;
                }
                setStreaming(false);
                Q_EMIT streamStopped(QStringLiteral("ended"));
                if (writerGuard) {
                    writerGuard->deleteLater();
                }
                // Only hand the node back to the cover when this writer was the
                // active one (another stream may already own it otherwise).
                if (wasCurrent) {
                    resumeIdle();
                }
            });
    connect(writer,
            &StreamWriter::failed,
            this,
            [this, writerGuard](const QString &reason) {
                qCWarning(KDECONNECT_PLUGIN_CAMERA) << "Stream pipeline failed:" << reason;
                const bool wasCurrent = (m_writer == writerGuard);
                if (wasCurrent) {
                    m_writer = nullptr;
                }
                setStreaming(false);
                Q_EMIT errorReceived(reason);
                Q_EMIT streamStopped(reason);
                if (writerGuard) {
                    writerGuard->deleteLater();
                }
                if (wasCurrent) {
                    resumeIdle();
                }
            });

    setStreaming(true);
    Q_EMIT streamPacketReceived();

    // The idle cover was already stopped above (before the StreamWriter
    // constructor probed the node), so the /dev/video node is free for the
    // live ffmpeg that writer->start() spawns.
    writer->start();

    // start() can fail synchronously (missing ffmpeg or device) before returning.
    if (!writer->isRunning()) {
        if (m_writer == writer) {
            m_writer = nullptr;
            setStreaming(false);
        }
        writer->deleteLater();
        // The live pipeline never owned the node: bring the cover back.
        resumeIdle();
    }
}

void CameraPlugin::receivePacket(const NetworkPacket &np)
{
    const QString type = np.type();

    if (type == PACKET_TYPE_CAMERA_LIST) {
        m_cameras = np.get<QVariantList>(QStringLiteral("cameras"));
        qCDebug(KDECONNECT_PLUGIN_CAMERA) << "Camera list received:" << m_cameras.size();
        Q_EMIT cameraListReceived(m_cameras);
    } else if (type == PACKET_TYPE_CAMERA_STREAM) {
        handleStreamPacket(np);
    } else if (type == PACKET_TYPE_CAMERA_STOP) {
        // The phone stopped the capture: end the local pipeline. The payload of a
        // stop packet is not a stream, nothing to close here.
        qCDebug(KDECONNECT_PLUGIN_CAMERA) << "Stream stopped by remote device";
        const bool wasActive = m_streamActive;
        stopWriter();
        setStreaming(false);
        if (wasActive) {
            Q_EMIT streamStopped(QStringLiteral("stopped"));
        }
        resumeIdle();
    } else if (type == PACKET_TYPE_CAMERA_ERROR) {
        const QString error = np.get<QString>(QStringLiteral("error"));
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "Camera error:" << error;
        stopWriter();
        Q_EMIT errorReceived(error);
        // Any error (including disconnected/stopped) means the stream is over.
        setStreaming(false);
        Q_EMIT streamStopped(error);
        resumeIdle();
    }
}

QString CameraPlugin::dbusPath() const
{
    return QLatin1String("/modules/kdeconnect/devices/%1/camera").arg(device()->id());
}

#include "cameraplugin.moc"
#include "moc_cameraplugin.cpp"
