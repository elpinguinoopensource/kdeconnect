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

    StreamWriter *writer = new StreamWriter(payload, width, height, fps, this);
    m_writer = writer;

    QPointer<StreamWriter> writerGuard = writer;
    connect(writer,
            &StreamWriter::finished,
            this,
            [this, writerGuard] {
                if (m_writer == writerGuard) {
                    m_writer = nullptr;
                }
                setStreaming(false);
                Q_EMIT streamStopped(QStringLiteral("ended"));
                if (writerGuard) {
                    writerGuard->deleteLater();
                }
            });
    connect(writer,
            &StreamWriter::failed,
            this,
            [this, writerGuard](const QString &reason) {
                qCWarning(KDECONNECT_PLUGIN_CAMERA) << "Stream pipeline failed:" << reason;
                if (m_writer == writerGuard) {
                    m_writer = nullptr;
                }
                setStreaming(false);
                Q_EMIT errorReceived(reason);
                Q_EMIT streamStopped(reason);
                if (writerGuard) {
                    writerGuard->deleteLater();
                }
            });

    setStreaming(true);
    Q_EMIT streamPacketReceived();

    writer->start();

    // start() can fail synchronously (missing ffmpeg or device) before returning.
    if (!writer->isRunning()) {
        if (m_writer == writer) {
            m_writer = nullptr;
            setStreaming(false);
        }
        writer->deleteLater();
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
    } else if (type == PACKET_TYPE_CAMERA_ERROR) {
        const QString error = np.get<QString>(QStringLiteral("error"));
        qCWarning(KDECONNECT_PLUGIN_CAMERA) << "Camera error:" << error;
        stopWriter();
        Q_EMIT errorReceived(error);
        // Any error (including disconnected/stopped) means the stream is over.
        setStreaming(false);
        Q_EMIT streamStopped(error);
    }
}

QString CameraPlugin::dbusPath() const
{
    return QLatin1String("/modules/kdeconnect/devices/%1/camera").arg(device()->id());
}

#include "cameraplugin.moc"
#include "moc_cameraplugin.cpp"
