#include <QTest>
#include <QThread>
#include <functional>
#include <sys/stat.h>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include "services/previewservice.h"

class TestPreviewService : public QObject
{
    Q_OBJECT

private:
    static bool batAvailable()
    {
        return !QStandardPaths::findExecutable("bat").isEmpty()
            || !QStandardPaths::findExecutable("batcat").isEmpty();
    }

    static QString findTrashEntryUri(const QString &originalPath)
    {
        QProcess proc;
        proc.start("gio", {
            "list",
            "-l",
            "-u",
            "-a",
            "trash::orig-path",
            "trash:///"
        });
        if (!proc.waitForFinished(5000) || proc.exitCode() != 0)
            return {};

        const QStringList lines = QString::fromUtf8(proc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            if (line.contains("trash::orig-path=" + originalPath))
                return line.section('\t', 0, 0).trimmed();
        }

        return {};
    }

private slots:
    void testLoadTextPreview()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.path() + "/notes.txt";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("alpha\nbeta\ngamma\n");
        file.close();

        PreviewService service;
        const QVariantMap preview = service.loadTextPreview(path, 1024, 20);

        QCOMPARE(preview.value("error").toString(), QString());
        QCOMPARE(preview.value("isBinary").toBool(), false);
        QVERIFY(preview.value("content").toString().contains("beta"));
        if (batAvailable()) {
            QCOMPARE(preview.value("usesBat").toBool(), true);
            QVERIFY(preview.value("html").toString().contains("alpha"));
        }
    }

    // Runs fn on a worker thread and reports whether it returned in time.
    // A stuck thread is leaked on purpose: deleting a running QThread qFatals.
    static bool returnsWithin(int ms, std::function<void()> fn)
    {
        QThread *thread = QThread::create(std::move(fn));
        thread->start();
        const bool finished = thread->wait(ms);
        if (finished)
            delete thread;
        return finished;
    }

    void testAnsiToHtmlSurvivesBareEscape()
    {
        // bat passes non-CSI escapes (e.g. ESC ( B) through verbatim; the
        // converter must consume them instead of spinning forever.
        QString html;
        QVERIFY2(returnsWithin(3000, [&html]() {
            html = PreviewService::ansiToHtml(QByteArray("hi \x1b(B there\n"));
        }), "ansiToHtml did not return on a bare ESC byte");
        QVERIFY(html.contains("hi"));
        QVERIFY(html.contains("there"));
    }

    void testFifoPreviewDoesNotBlock()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString fifo = dir.path() + "/notes";
        QCOMPARE(::mkfifo(fifo.toLocal8Bit().constData(), 0600), 0);

        PreviewService service;
        QVariantMap preview;
        QVERIFY2(returnsWithin(3000, [&]() {
            preview = service.loadTextPreview(fifo, 1024, 20);
        }), "loadTextPreview blocked on a FIFO");
        QVERIFY(!preview.value("error").toString().isEmpty());
    }

    void testBatOutputIsBoundedByMaxBytes()
    {
        if (!batAvailable())
            QSKIP("bat not found in PATH");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + "/huge.json";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[");
        file.write(QByteArray(1024 * 1024, '1'));   // one 1 MB line
        file.write("]");
        file.close();

        PreviewService service;
        const QVariantMap preview = service.loadTextPreview(path, 1024, 20);
        QCOMPARE(preview.value("usesBat").toBool(), true);
        // highlighting 1 KB of input can never need anything near 64 KB of HTML
        QVERIFY2(preview.value("html").toString().size() < 64 * 1024,
                 qPrintable(QString::number(preview.value("html").toString().size())));
    }

    void testBinaryPreviewDetection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.path() + "/blob.bin";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromHex("89504e470d0a1a0a00000000"));
        file.close();

        PreviewService service;
        const QVariantMap preview = service.loadTextPreview(path, 1024, 20);

        QCOMPARE(preview.value("isBinary").toBool(), true);
        QCOMPARE(preview.value("content").toString(), QString());
    }

    void testDirectoryPreview()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QDir root(dir.path());
        QVERIFY(root.mkdir("Folder"));
        QFile file(root.filePath("alpha.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("hello");
        file.close();

        PreviewService service;
        const QVariantMap preview = service.loadDirectoryPreview(dir.path(), 20);
        const QStringList entries = preview.value("entries").toStringList();

        QVERIFY(entries.contains("Folder/"));
        QVERIFY(entries.contains("alpha.txt"));
    }

    void testLocalPreviewPathForRegularFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.path() + "/doc.pdf";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("dummy");
        file.close();

        PreviewService service;
        QCOMPARE(service.localPreviewPath(path), path);
    }

    void testLoadPdfPreview()
    {
        PreviewService service;
        if (!service.pdfPreviewAvailable())
            QSKIP("PDF preview support is unavailable in this build");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.path() + "/preview.pdf";
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        QPainter painter(&writer);
        painter.drawText(QPointF(72.0, 100.0), QStringLiteral("Preview Test"));
        painter.end();

        const QVariantMap preview = service.loadPdfPreview(path);
        QCOMPARE(preview.value("error").toString(), QString());
        QCOMPARE(preview.value("localPath").toString(), path);
        QVERIFY(preview.value("pageCount").toInt() >= 1);
    }

    void testTrashPreviewCacheKeepsOnlyCurrentFile()
    {
        if (QStandardPaths::findExecutable("gio").isEmpty())
            QSKIP("gio not found in PATH");

        const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString dirPath = QDir::homePath() + "/.cache/hyprfm-test-preview-evict-" + uniqueId;
        QDir().mkpath(dirPath);
        QStringList uris;
        for (const char *name : {"one.txt", "two.txt"}) {
            const QString filePath = dirPath + "/" + name;
            QFile file(filePath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(name);
            file.close();
            QProcess trashProc;
            trashProc.start("gio", {"trash", filePath});
            if (!trashProc.waitForFinished(5000) || trashProc.exitCode() != 0)
                QSKIP("gio trash failed in this environment");
            const QString uri = findTrashEntryUri(filePath);
            if (uri.isEmpty())
                QSKIP("Could not find trashed file URI");
            uris.append(uri);
        }
        QDir().rmdir(dirPath);

        PreviewService service;
        const QString first = service.localPreviewPath(uris.at(0));
        QVERIFY(QFile::exists(first));
        const QString second = service.localPreviewPath(uris.at(1));
        QVERIFY(QFile::exists(second));
        QVERIFY2(!QFile::exists(first), "previous trash preview copy was not evicted");
    }

    void testTrashTextPreview()
    {
        if (QStandardPaths::findExecutable("gio").isEmpty())
            QSKIP("gio not found in PATH");

        const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString dirPath = QDir::homePath() + "/.cache/hyprfm-test-preview-service-" + uniqueId;
        QDir().mkpath(dirPath);

        const QString filePath = dirPath + "/preview.txt";
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("trash preview text");
        file.close();

        QProcess trashProc;
        trashProc.start("gio", {"trash", filePath});
        if (!trashProc.waitForFinished(5000) || trashProc.exitCode() != 0)
            QSKIP("gio trash failed in this environment");

        const QString trashUri = findTrashEntryUri(filePath);
        if (trashUri.isEmpty())
            QSKIP("Could not find trashed file URI");

        PreviewService service;
        const QVariantMap preview = service.loadTextPreview(trashUri, 1024, 20);

        QCOMPARE(preview.value("error").toString(), QString());
        QCOMPARE(preview.value("isBinary").toBool(), false);
        QVERIFY(preview.value("content").toString().contains("trash preview text"));
        if (batAvailable())
            QCOMPARE(preview.value("usesBat").toBool(), true);

        const QString cachedPath = service.localPreviewPath(trashUri);
        QVERIFY(!cachedPath.isEmpty());
        QVERIFY(QFileInfo::exists(cachedPath));

        QProcess removeProc;
        removeProc.start("gio", {"remove", "-f", trashUri});
        removeProc.waitForFinished(5000);
        QDir(dirPath).removeRecursively();
    }
};

QTEST_MAIN(TestPreviewService)
#include "tst_previewservice.moc"
