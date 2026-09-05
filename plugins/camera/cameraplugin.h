/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QObject>
#include <QVariantList>

#include <core/kdeconnectplugin.h>

#define PACKET_TYPE_CAMERA_LIST QStringLiteral("kdeconnect.camera.list")
#define PACKET_TYPE_CAMERA_START QStringLiteral("kdeconnect.camera.start")
#define PACKET_TYPE_CAMERA_STREAM QStringLiteral("kdeconnect.camera.stream")
#define PACKET_TYPE_CAMERA_STOP QStringLiteral("kdeconnect.camera.stop")
#define PACKET_TYPE_CAMERA_ERROR QStringLiteral("kdeconnect.camera.error")
#define PACKET_TYPE_CAMERA_STATS QStringLiteral("kdeconnect.camera.stats")

class StreamWriter;
class IdleWriter;

class CameraPlugin : public KdeConnectPlugin
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device.camera")
    Q_PROPERTY(bool streaming READ streaming NOTIFY streamingChanged)
    Q_PROPERTY(QString devicePath READ devicePath NOTIFY devicePathChanged)

public:
    using KdeConnectPlugin::KdeConnectPlugin;
    ~CameraPlugin() override;

    bool streaming() const;

    /// v4l2loopback device the live stream is being written to, empty when no
    /// stream is active (or detection found no usable node).
    QString devicePath() const;

    Q_SCRIPTABLE QVariantList listCameras();
    Q_SCRIPTABLE void refreshCameraList();
    Q_SCRIPTABLE void startCamera(const QString &cameraId, int width, int height, int fps, int bitrate);
    Q_SCRIPTABLE void stopCamera();

    QString dbusPath() const override;
    void receivePacket(const NetworkPacket &np) override;
    void connected() override;

Q_SIGNALS:
    Q_SCRIPTABLE void streamingChanged(bool streaming);
    Q_SCRIPTABLE void devicePathChanged(const QString &devicePath);
    Q_SCRIPTABLE void streamPacketReceived();
    Q_SCRIPTABLE void streamStopped(const QString &reason);
    Q_SCRIPTABLE void cameraListReceived(const QVariantList &cameras);
    Q_SCRIPTABLE void errorReceived(const QString &error);

private:
    void setStreaming(bool streaming);
    void setDevicePath(const QString &devicePath);
    void handleStreamPacket(const NetworkPacket &np);
    /// Stops and destroys the active StreamWriter (if any); closes the payload.
    void stopWriter();
    /// Creates the IdleWriter cover feed lazily (once) and hooks its failure
    /// signal to the debug log.
    void ensureIdle();
    /// Restart the idle cover feed after a live pipeline ends. No-op while a
    /// live stream is active or when the "showCover" config is disabled.
    void resumeIdle();

    QVariantList m_cameras;
    bool m_streamActive = false;
    QString m_devicePath;
    StreamWriter *m_writer = nullptr;
    IdleWriter *m_idle = nullptr;
};
