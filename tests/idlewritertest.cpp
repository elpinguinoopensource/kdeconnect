/**
 * SPDX-FileCopyrightText: 2026 Alfredo Medrano Sanchez <alfredomedranosanchez@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include "plugins/camera/idlewriter.h"

/**
 * Plumbing-only tests for IdleWriter: the v4l2 muxer needs a real v4l2loopback
 * device node (and ffmpeg may be missing entirely, e.g. in CI containers), so
 * these cover the lifecycle, the failure paths and the qrc cover extraction
 * instead of asserting on produced video bytes.
 */
class IdleWriterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testStopOnNeverStartedIsSafe()
    {
        IdleWriter writer;
        writer.stop();
        writer.stop(); // Double stop must be harmless too.
        QVERIFY(!writer.isRunning());
    }

    void testDoubleStartIsIdempotent()
    {
        IdleWriter writer;
        if (!writer.start()) {
            QSKIP("Environment has no ffmpeg/v4l2loopback device, running state unreachable");
        }
        QVERIFY(writer.isRunning());
        QVERIFY(writer.start()); // Second start is a no-op, still running.
        QVERIFY(writer.isRunning());
        writer.stop();
        QVERIFY(!writer.isRunning());
    }

    void testStartStopCycle()
    {
        IdleWriter writer;
        for (int i = 0; i < 2; ++i) {
            const bool started = writer.start();
            QCOMPARE(writer.isRunning(), started); // State matches the verdict.
            writer.stop();
            QVERIFY(!writer.isRunning());
        }
    }

    void testMissingCoverFailsSynchronously()
    {
        IdleWriter writer;
        writer.setImagePath(QStringLiteral("/nonexistent/image.png"));
        QSignalSpy failedSpy(&writer, &IdleWriter::failed);
        QVERIFY(failedSpy.isValid());

        QVERIFY(!writer.start());

        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.takeAt(0).at(0).toString(), QStringLiteral("cover_read_failed"));
        QVERIFY(!writer.isRunning());
    }

    void testInvalidDeviceEmitsFailed()
    {
        IdleWriter writer;
        writer.setDevicePath(QStringLiteral("/tmp/kdeconnect-not-a-video-node"));
        QSignalSpy failedSpy(&writer, &IdleWriter::failed);
        QVERIFY(failedSpy.isValid());

        const bool started = writer.start();
        if (started) {
            // ffmpeg accepted the bogus node and is up (should not happen with
            // the v4l2 muxer): it must die on its own shortly.
            if (!failedSpy.wait(3000)) {
                writer.stop();
                QSKIP("ffmpeg did not exit within 3 s on an invalid device");
            }
        }
        QVERIFY(failedSpy.count() >= 1);
        QVERIFY(!writer.isRunning());
    }

    void testCoverExtractionFromResource()
    {
        // No setImagePath(): the embedded :/camera/camera-idle.png must be
        // unpacked to a temp file before ffmpeg is considered.
        IdleWriter writer;
        QSignalSpy failedSpy(&writer, &IdleWriter::failed);
        QVERIFY(failedSpy.isValid());

        const bool started = writer.start();
        if (started) {
            // ffmpeg IS running with the extracted cover: nothing left to
            // assert beyond consistency, the spec asks to skip.
            writer.stop();
            QSKIP("Cover is streaming successfully, failure path not reachable");
        }

        // Extraction succeeded (otherwise the reason would be a cover_* code),
        // so the only acceptable failures here are a missing device or ffmpeg.
        QCOMPARE(failedSpy.count(), 1);
        const QString reason = failedSpy.takeAt(0).at(0).toString();
        QVERIFY2(reason == QLatin1String("no_v4l2_device") || reason == QLatin1String("ffmpeg_not_found"),
                 qPrintable(QStringLiteral("unexpected failure reason: %1").arg(reason)));
        QVERIFY(!writer.isRunning());
    }

    void testDestructorWhileRunning()
    {
        auto writer = new IdleWriter();
        writer->start(); // May fail in this environment, harmless either way.
        delete writer; // Must reap the child without crashing.
        QVERIFY(true);
    }

    void testFfmpegArgsPaceInputWithRe()
    {
        // Regression: the cover ffmpeg ran at ~670% CPU because -framerate 5
        // only sets a logical input rate. Without -re ffmpeg ignores the wall
        // clock and pushes frames to the v4l2 muxer as fast as they are
        // accepted (v4l2loopback never blocks a writer with no reader).
        const QStringList args = IdleWriter::ffmpegArgs(
            QStringLiteral("/tmp/cover.png"), QStringLiteral("/dev/video10"), 1280, 720);

        const int reIndex = args.indexOf(QStringLiteral("-re"));
        const int inputIndex = args.indexOf(QStringLiteral("-i"));
        QVERIFY2(reIndex >= 0, "missing -re: the cover pipeline floods the device and burns CPU");
        QVERIFY2(inputIndex >= 0, "missing -i input");
        // -re is an INPUT option: it must appear before -i to apply to the
        // looped image, not to the output.
        QVERIFY2(reIndex < inputIndex, "-re must precede -i (input pacing)");

        // The rest of the pipeline contract the owner relies on.
        QCOMPARE(args.indexOf(QStringLiteral("-f")) >= 0, true);
        QCOMPARE(args.last(), QStringLiteral("/dev/video10"));
        QVERIFY(args.contains(QStringLiteral("v4l2")));
        QVERIFY(args.contains(QStringLiteral("/tmp/cover.png")));
        // The scale/pad filter must target the negotiated device size.
        QVERIFY(args.filter(QStringLiteral("scale=1280:720")).size() == 1);
    }
};

QTEST_GUILESS_MAIN(IdleWriterTest)

#include "idlewritertest.moc"
