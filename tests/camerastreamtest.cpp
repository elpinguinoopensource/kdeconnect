/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include <QBuffer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include "plugins/camera/streamwriter.h"

/**
 * Plumbing-only tests for StreamWriter: ffmpeg is not available in the CI
 * container, so these cover the failure paths, safe teardown and metadata
 * passthrough instead of a real pipeline run.
 */
class CameraStreamTest : public QObject
{
    Q_OBJECT

private:
    static QSharedPointer<QIODevice> makeSource()
    {
        QByteArray blob;
        // Fake H.264 Annex-B start code plus some payload bytes.
        blob.append("\x00\x00\x00\x01\x67");
        blob.append(1024, '\x42');
        auto source = QSharedPointer<QBuffer>::create();
        source->setData(blob);
        source->open(QIODevice::ReadOnly);
        return source;
    }

private Q_SLOTS:
    void testFfmpegMissingEmitsFailed()
    {
        if (!QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
            QSKIP("ffmpeg is installed, failure path not reachable");
        }
        auto source = makeSource();
        StreamWriter writer(source, 1280, 720, 30);
        writer.setDevicePath(QStringLiteral("/tmp/kdeconnect-camera-test.video")); // Pretend a device exists
        QSignalSpy failedSpy(&writer, &StreamWriter::failed);
        QVERIFY(failedSpy.isValid());

        writer.start();

        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.takeAt(0).at(0).toString(), QStringLiteral("ffmpeg_not_found"));
        QVERIFY(!writer.isRunning());
        QVERIFY(!source->isOpen()); // Payload must be closed on failure
    }

    void testMissingDeviceEmitsFailed()
    {
        auto source = makeSource();
        StreamWriter writer(source, 640, 480, 24);
        writer.setDevicePath(QString()); // No v4l2loopback device
        QSignalSpy failedSpy(&writer, &StreamWriter::failed);

        writer.start();

        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.takeAt(0).at(0).toString(), QStringLiteral("no_v4l2_device"));
        QVERIFY(!source->isOpen());
    }

    void testStopOnNeverStartedIsSafe()
    {
        auto source = makeSource();
        StreamWriter writer(source, 320, 240, 15);
        writer.stop();
        writer.stop(); // Double stop must be harmless too
        QVERIFY(!writer.isRunning());
        QVERIFY(!source->isOpen()); // stop() always closes the payload
    }

    void testMetadataPassthrough()
    {
        auto source = makeSource();
        StreamWriter writer(source, 1920, 1080, 60);
        QCOMPARE(writer.width(), 1920);
        QCOMPARE(writer.height(), 1080);
        QCOMPARE(writer.fps(), 60);
        QCOMPARE(writer.devicePath(), writer.devicePath()); // Device auto-detection must not crash
    }

    void testClosedSourceFailsToStart()
    {
        auto source = makeSource();
        source->close();
        StreamWriter writer(source, 1280, 720, 30);
        QSignalSpy failedSpy(&writer, &StreamWriter::failed);

        writer.start();

        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.takeAt(0).at(0).toString(), QStringLiteral("no_open_payload"));
    }
};

QTEST_GUILESS_MAIN(CameraStreamTest)

#include "camerastreamtest.moc"
