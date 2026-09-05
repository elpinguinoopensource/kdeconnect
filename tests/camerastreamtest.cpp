/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include <QBuffer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

#include <cstring>

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

    /// True when a real ffmpeg + v4l2loopback output node are usable, i.e. the
    /// integration tests below can run. They are skipped in CI containers and
    /// on machines whose loopback node is capture-only (exclusive_caps=1).
    static bool pipelineRunnable()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
            return false;
        }
        auto probe = makeSource();
        StreamWriter writer(probe, 320, 240, 30);
        return !writer.devicePath().isEmpty();
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

    // ---- Activity watchdog -------------------------------------------------
    // These run the real pipeline (ffmpeg + v4l2loopback node) and are skipped
    // where either is missing. The payload is deliberately too small to reach
    // the 128 KB probesize, so ffmpeg stays alive waiting for more input while
    // the source runs dry: exactly the "phone went silent" scenario.

    /// QIODevice that hands out bytes pushed from the outside, emitting
    /// readyRead() on every push. Stands in for a phone that keeps streaming.
    class ByteSource : public QIODevice
    {
    public:
        ByteSource() { open(QIODevice::ReadOnly); }
        void push(const QByteArray &bytes)
        {
            m_bytes.append(bytes);
            Q_EMIT readyRead();
        }

    protected:
        qint64 readData(char *data, qint64 maxlen) override
        {
            const qint64 n = qMin(maxlen, static_cast<qint64>(m_bytes.size()));
            memcpy(data, m_bytes.constData(), size_t(n));
            m_bytes.remove(0, int(n));
            return n;
        }
        qint64 writeData(const char *, qint64) override
        {
            return 0;
        }
        qint64 bytesAvailable() const override { return m_bytes.size() + QIODevice::bytesAvailable(); }

    private:
        QByteArray m_bytes;
    };

    void testWatchdogFailsSilentStream()
    {
        if (!pipelineRunnable()) {
            QSKIP("needs ffmpeg and a v4l2loopback output node");
        }
        auto source = makeSource();
        StreamWriter writer(source, 1280, 720, 30);
        writer.setActivityWatchdog(300, 200);
        QSignalSpy failedSpy(&writer, &StreamWriter::failed);
        QVERIFY(failedSpy.isValid());

        writer.start();
        QVERIFY2(writer.isRunning(), QStringLiteral("pipeline should be up before the watchdog fires"));

        // start() drains the 1 KB payload (below the watchdog tick), after which
        // the source goes dry; grace+tick elapse well within the 3 s wait.
        QVERIFY2(failedSpy.wait(3000), QStringLiteral("watchdog never fired on a silent stream"));
        QCOMPARE(failedSpy.takeAt(0).at(0).toString(), QStringLiteral("stream_stalled"));
        QVERIFY(!writer.isRunning());
        QVERIFY(!source->isOpen()); // The stalled payload must be closed.
    }

    void testWatchdogKeepsQuietWhileSourceIsActive()
    {
        if (!pipelineRunnable()) {
            QSKIP("needs ffmpeg and a v4l2loopback output node");
        }
        auto source = QSharedPointer<ByteSource>::create();
        StreamWriter writer(source, 1280, 720, 30);
        writer.setActivityWatchdog(200, 400);
        QSignalSpy failedSpy(&writer, &StreamWriter::failed);

        QTimer pump;
        int pushed = 0;
        connect(&pump, &QTimer::timeout, &pump, [&] {
            source->push(QByteArray(64, '\x11')); // Garbage, but "activity".
            ++pushed;
        });
        pump.start(100);

        writer.start();
        QVERIFY(writer.isRunning());
        QTest::qWait(1500); // Several tick windows worth of activity.
        pump.stop();

        QCOMPARE(failedSpy.count(), 0); // Activity must never be mistaken for a stall.
        QVERIFY(pushed > 5);
        QVERIFY(writer.isRunning());

        writer.stop();
        QVERIFY(!writer.isRunning());
    }
};

QTEST_GUILESS_MAIN(CameraStreamTest)

#include "camerastreamtest.moc"
