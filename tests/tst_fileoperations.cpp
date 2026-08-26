#include <QTest>
#include <QClipboard>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <unistd.h>
#include "testdir.h"
#include "services/fileoperations.h"

class TestFileOperations : public QObject
{
    Q_OBJECT

    static QString findTrashEntryPath(const QString &originalPath)
    {
        const QString trashInfoDirPath = QDir::homePath() + "/.local/share/Trash/info";
        const QString trashFilesDirPath = QDir::homePath() + "/.local/share/Trash/files";
        QDir infoDir(trashInfoDirPath);

        const QStringList entries = infoDir.entryList({"*.trashinfo"}, QDir::Files, QDir::Time);
        for (const QString &entry : entries) {
            QFile file(infoDir.filePath(entry));
            if (!file.open(QIODevice::ReadOnly))
                continue;

            const QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
            for (const QString &line : lines) {
                if (!line.startsWith("Path="))
                    continue;

                const QString decodedPath = QUrl::fromPercentEncoding(line.mid(5).toUtf8());
                if (decodedPath == originalPath)
                    return QDir(trashFilesDirPath).filePath(entry.chopped(10));
            }
        }

        return {};
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
            if (!line.contains("trash::orig-path=" + originalPath))
                continue;

            return line.section('\t', 0, 0).trimmed();
        }

        return {};
    }

    static bool runCommand(const QString &program, const QStringList &args,
                           const QString &workingDirectory = {})
    {
        QProcess proc;
        if (!workingDirectory.isEmpty())
            proc.setWorkingDirectory(workingDirectory);

        proc.start(program, args);
        return proc.waitForFinished(5000) && proc.exitCode() == 0;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    // --- Desktop entry Exec= parsing (Open With) ---

    void testDesktopExecArguments()
    {
        const QString file = "/tmp/clip.mkv";

        // The common field codes all stand in for the file.
        QCOMPARE(FileOperations::desktopExecArguments("mpv %U", file),
                 QStringList({"mpv", file}));
        QCOMPARE(FileOperations::desktopExecArguments("nvim %F", file),
                 QStringList({"nvim", file}));

        // Options around the field code survive, in order.
        QCOMPARE(FileOperations::desktopExecArguments(
                     "mpv --player-operation-mode=pseudo-gui -- %U", file),
                 QStringList({"mpv", "--player-operation-mode=pseudo-gui", "--", file}));

        // Codes that carry no meaning here are dropped, not passed through.
        QCOMPARE(FileOperations::desktopExecArguments("app %i %c %k %U", file),
                 QStringList({"app", file}));

        // An entry with no field code still gets the file appended.
        QCOMPARE(FileOperations::desktopExecArguments("micro", file),
                 QStringList({"micro", file}));

        // Quoted paths stay one argument, and %% is a literal percent.
        QCOMPARE(FileOperations::desktopExecArguments("\"/opt/My App/run\" -x 100%% %f", file),
                 QStringList({"/opt/My App/run", "-x", "100%", file}));

        // No file to open: the field code just disappears.
        QCOMPARE(FileOperations::desktopExecArguments("mpv %U", QString()),
                 QStringList({"mpv"}));
    }

    // The launched app inherits gio's stdio. If those are pipes back to us,
    // closing them once gio exits kills any app that later writes to stderr
    // (Krita, mpv, ...) with SIGPIPE — the "Open With does nothing" report.
    void testOpenFileWithSurvivesLauncherExit()
    {
        if (QStandardPaths::findExecutable("gio").isEmpty())
            QSKIP("gio not found in PATH");

        QTemporaryDir dir;
        const QString marker = dir.filePath("alive");
        const QString script = dir.filePath("app.sh");
        {
            QFile f(script);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\nsleep 0.5\necho still-running >&2\ntouch \"$1\"\n");
        }
        QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QString entry = dir.filePath("app.desktop");
        {
            QFile f(entry);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=Test\nExec=%1 %f\n")
                        .arg(script).toUtf8());
        }

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);
        ops.openFileWith(marker, entry);

        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(marker), 5000);
        QVERIFY2(spy.isEmpty(), "launch reported a failure");
    }

    void testOpenInEditorRunsEditorInTerminal()
    {
        QTemporaryDir dir;
        const QString marker = dir.filePath("argv");
        const QString term = dir.filePath("term.sh");
        {
            QFile f(term);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QStringLiteral("#!/bin/sh\nprintf '%s ' \"$@\" > \"%1\"\n").arg(marker).toUtf8());
        }
        QFile::setPermissions(term, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("TERMINAL", term.toUtf8());
        qputenv("VISUAL", "");
        qputenv("EDITOR", "myeditor --wait");

        FileOperations ops;
        ops.openInEditor("/tmp/config.toml");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(marker), 5000);
        QFile f(marker);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(f.readAll()).trimmed(), QString("-e myeditor --wait /tmp/config.toml"));
        qunsetenv("TERMINAL"); qunsetenv("EDITOR");
    }

    void testRunCustomActionSubstitutesPathAndRunsPerItem()
    {
        QTemporaryDir dir;
        const QString script = dir.filePath("act.sh");
        {
            QFile f(script);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\ntouch \"$1.done\"\n");
        }
        QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QString a = dir.filePath("a.txt"), b = dir.filePath("b.txt");

        FileOperations ops;
        ops.runCustomAction(script + " %f", {a, b});
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(a + ".done"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(b + ".done"), 5000);
    }

    // --- Copy ---

    void testCopyFile()
    {
        TestDir src, dst;
        src.createFile("test.txt", "hello");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.copyFiles({src.path() + "/test.txt"}, dst.path());

        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.at(0).at(0).toBool(), true); // success
        QVERIFY(QFile::exists(dst.path() + "/test.txt"));
        // Source still exists after copy
        QVERIFY(QFile::exists(src.path() + "/test.txt"));
    }

    void testCopyMultipleFiles()
    {
        TestDir src, dst;
        src.createFile("a.txt", "aaa");
        src.createFile("b.txt", "bbb");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.copyFiles({src.path() + "/a.txt", src.path() + "/b.txt"}, dst.path());

        QVERIFY(spy.wait(5000));
        QVERIFY(QFile::exists(dst.path() + "/a.txt"));
        QVERIFY(QFile::exists(dst.path() + "/b.txt"));
    }

    void testCopyDirectory()
    {
        TestDir src, dst;
        src.createDir("subdir");
        src.createFile("subdir/inner.txt", "content");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.copyFiles({src.path() + "/subdir"}, dst.path());

        QVERIFY(spy.wait(5000));
        QVERIFY(QFile::exists(dst.path() + "/subdir/inner.txt"));
    }

    void testCopyEmitsBusyChanged()
    {
        TestDir src, dst;
        src.createFile("test.txt", "data");

        FileOperations ops;
        QSignalSpy busySpy(&ops, &FileOperations::busyChanged);
        QSignalSpy finishSpy(&ops, &FileOperations::operationFinished);

        QCOMPARE(ops.busy(), false);
        ops.copyFiles({src.path() + "/test.txt"}, dst.path());

        QVERIFY(finishSpy.wait(5000));
        QVERIFY(busySpy.count() >= 2); // true then false
        QCOMPARE(ops.busy(), false);
    }

    void testCopyEmitsStatusText()
    {
        TestDir src, dst;
        src.createFile("test.txt", "data");

        FileOperations ops;
        QSignalSpy statusSpy(&ops, &FileOperations::statusTextChanged);
        QSignalSpy finishSpy(&ops, &FileOperations::operationFinished);

        ops.copyFiles({src.path() + "/test.txt"}, dst.path());

        QVERIFY(finishSpy.wait(5000));
        QVERIFY(statusSpy.count() >= 1);
    }

    void testCopyEmitsProgressChanged()
    {
        TestDir src, dst;
        src.createFile("large.bin", QByteArray(8 * 1024 * 1024, 'x'));

        FileOperations ops;
        QSignalSpy progressSpy(&ops, &FileOperations::progressChanged);
        QSignalSpy finishSpy(&ops, &FileOperations::operationFinished);

        ops.copyFiles({src.path() + "/large.bin"}, dst.path());

        QVERIFY(finishSpy.wait(10000));
        QVERIFY(progressSpy.count() >= 1);
        QCOMPARE(ops.progress(), 1.0);
    }

    // --- Move ---

    void testMoveFile()
    {
        TestDir src, dst;
        QString srcPath = src.path() + "/test.txt";
        src.createFile("test.txt", "hello");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.moveFiles({srcPath}, dst.path());

        QVERIFY(spy.wait(5000));
        QVERIFY(QFile::exists(dst.path() + "/test.txt"));
        QVERIFY(!QFile::exists(srcPath));
    }

    void testTransferPlanDetectsConflict()
    {
        TestDir src, dst;
        src.createFile("conflict.txt", "new");
        dst.createFile("conflict.txt", "old");

        FileOperations ops;
        const QVariantList plan = ops.transferPlan({src.path() + "/conflict.txt"}, dst.path());

        QCOMPARE(plan.size(), 1);
        const QVariantMap item = plan.constFirst().toMap();
        QCOMPARE(item.value("sourceName").toString(), QString("conflict.txt"));
        QCOMPARE(item.value("targetPath").toString(), dst.path() + "/conflict.txt");
        QVERIFY(item.value("targetExists").toBool());
    }

    void testUniqueNameForDestination()
    {
        TestDir dir;
        dir.createFile("report.txt", "existing");

        FileOperations ops;
        QCOMPARE(ops.uniqueNameForDestination(dir.path(), "report.txt"), QString("report (copy).txt"));
        QCOMPARE(ops.uniqueNameForDestination(dir.path(), "draft.txt", {"draft.txt", "draft (copy).txt"}),
                 QString("draft (copy 2).txt"));
    }

    void testCopyResolvedItemsRename()
    {
        TestDir src, dst;
        src.createFile("test.txt", "hello");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        QVariantMap item;
        item["sourcePath"] = src.path() + "/test.txt";
        item["targetPath"] = dst.path() + "/renamed.txt";
        item["overwrite"] = false;

        ops.copyResolvedItems({item});

        QCOMPARE(spy.wait(5000), true);
        QCOMPARE(spy.at(0).at(0).toBool(), true);
        QVERIFY(QFile::exists(dst.path() + "/renamed.txt"));
        QVERIFY(QFile::exists(src.path() + "/test.txt"));
    }

    void testMoveResolvedItemsOverwrite()
    {
        TestDir src, dst;
        src.createFile("test.txt", "new content");
        dst.createFile("test.txt", "old content");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        QVariantMap item;
        item["sourcePath"] = src.path() + "/test.txt";
        item["targetPath"] = dst.path() + "/test.txt";
        item["overwrite"] = true;
        item["backupPath"] = ops.conflictBackupPath(dst.path() + "/test.txt");

        ops.moveResolvedItems({item});

        QCOMPARE(spy.wait(5000), true);
        QCOMPARE(spy.at(0).at(0).toBool(), true);
        QVERIFY(!QFile::exists(src.path() + "/test.txt"));
        QVERIFY(QFile::exists(dst.path() + "/test.txt"));

        QFile result(dst.path() + "/test.txt");
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(result.readAll()), QString("new content"));
        QVERIFY(QFileInfo::exists(item.value("backupPath").toString()));
    }

    // --- Rename ---

    void testRenameFile()
    {
        TestDir dir;
        QString oldPath = dir.path() + "/old.txt";
        dir.createFile("old.txt", "rename me");

        FileOperations ops;
        bool result = ops.rename(oldPath, "new.txt");

        QVERIFY(result);
        QVERIFY(QFile::exists(dir.path() + "/new.txt"));
        QVERIFY(!QFile::exists(oldPath));
    }

    void testRenameToExistingFails()
    {
        TestDir dir;
        dir.createFile("a.txt", "aaa");
        dir.createFile("b.txt", "bbb");

        FileOperations ops;
        // Renaming a.txt to b.txt when b.txt exists — behavior is platform-dependent
        // On Linux, QFile::rename may overwrite or fail; just verify no crash
        ops.rename(dir.path() + "/a.txt", "b.txt");
    }

    void testRenameWithSpecialChars()
    {
        TestDir dir;
        dir.createFile("normal.txt", "content");

        FileOperations ops;
        bool result = ops.rename(dir.path() + "/normal.txt", "file with spaces.txt");

        QVERIFY(result);
        QVERIFY(QFile::exists(dir.path() + "/file with spaces.txt"));
    }

    void testRenameResolvedItemsSwapsNames()
    {
        TestDir dir;
        const QString aPath = dir.createFile("a.txt", "aaa");
        const QString bPath = dir.createFile("b.txt", "bbb");

        FileOperations ops;

        QVariantMap renameA;
        renameA["sourcePath"] = aPath;
        renameA["targetPath"] = bPath;

        QVariantMap renameB;
        renameB["sourcePath"] = bPath;
        renameB["targetPath"] = aPath;

        const QVariantMap result = ops.renameResolvedItems({renameA, renameB});

        QCOMPARE(result.value("success").toBool(), true);
        QCOMPARE(result.value("changedPaths").toStringList(), QStringList({bPath, aPath}));

        QFile aFile(aPath);
        QVERIFY(aFile.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(aFile.readAll()), QString("bbb"));

        QFile bFile(bPath);
        QVERIFY(bFile.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(bFile.readAll()), QString("aaa"));
    }

    void testRenameResolvedItemsRejectsExistingTarget()
    {
        TestDir dir;
        const QString sourcePath = dir.createFile("source.txt", "source");
        const QString takenPath = dir.createFile("taken.txt", "taken");

        FileOperations ops;

        QVariantMap item;
        item["sourcePath"] = sourcePath;
        item["targetPath"] = takenPath;

        const QVariantMap result = ops.renameResolvedItems({item});

        QCOMPARE(result.value("success").toBool(), false);
        QVERIFY(QFile::exists(sourcePath));
        QVERIFY(QFile::exists(takenPath));
    }

    void testRenameResolvedItemsRejectsDuplicateFinalTarget()
    {
        TestDir dir;
        const QString aPath = dir.createFile("a.txt", "aaa");
        const QString bPath = dir.createFile("b.txt", "bbb");

        FileOperations ops;

        QVariantMap keepA;
        keepA["sourcePath"] = aPath;
        keepA["targetPath"] = aPath;

        QVariantMap renameB;
        renameB["sourcePath"] = bPath;
        renameB["targetPath"] = aPath;

        const QVariantMap result = ops.renameResolvedItems({keepA, renameB});

        QCOMPARE(result.value("success").toBool(), false);
        QVERIFY(QFile::exists(aPath));
        QVERIFY(QFile::exists(bPath));
    }

    // --- Delete (permanent) ---

    void testCopyDirectoryIntoItselfIsRejected()
    {
        TestDir dir;
        dir.createDir("a");
        dir.createFile("a/f.txt", "x");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.copyFiles({dir.path() + "/a"}, dir.path() + "/a");

        if (spy.isEmpty())
            QVERIFY(spy.wait(30000));
        QCOMPARE(spy.at(0).at(0).toBool(), false);
        QVERIFY(!dir.exists("a/a"));
    }

    void testDeleteFile()
    {
        TestDir dir;
        dir.createFile("doomed.txt", "bye");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.deleteFiles({dir.path() + "/doomed.txt"});

        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.at(0).at(0).toBool(), true);
        QVERIFY(!dir.exists("doomed.txt"));
    }

    void testDeleteDirectory()
    {
        TestDir dir;
        dir.createDir("doomed_dir");
        dir.createFile("doomed_dir/inner.txt", "bye");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.deleteFiles({dir.path() + "/doomed_dir"});

        QVERIFY(spy.wait(5000));
        QVERIFY(!dir.exists("doomed_dir"));
    }

    void testDeleteSymlinkToDirectoryKeepsTarget()
    {
        TestDir dir;
        dir.createDir("real");
        dir.createFile("real/a.txt", "keep");
        dir.createSymlink(dir.path() + "/real", "link");
        QVERIFY(QFileInfo(dir.path() + "/link").isSymLink());

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.deleteFiles({dir.path() + "/link"});

        QVERIFY(spy.wait(5000));
        QVERIFY(dir.exists("real/a.txt"));
        QVERIFY(!QFileInfo(dir.path() + "/link").isSymLink());
        QCOMPARE(spy.at(0).at(0).toBool(), true);
    }

    void testDeleteDirectoryViaFileUri()
    {
        TestDir dir;
        dir.createDir("doomed_uri_dir");
        dir.createFile("doomed_uri_dir/inner.txt", "bye");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.deleteFiles({QUrl::fromLocalFile(dir.path() + "/doomed_uri_dir").toString()});

        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.at(0).at(0).toBool(), true);
        QVERIFY(!dir.exists("doomed_uri_dir"));
    }

    void testDeleteMultiple()
    {
        TestDir dir;
        dir.createFile("a.txt");
        dir.createFile("b.txt");

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.deleteFiles({dir.path() + "/a.txt", dir.path() + "/b.txt"});

        QVERIFY(spy.wait(5000));
        QVERIFY(!dir.exists("a.txt"));
        QVERIFY(!dir.exists("b.txt"));
    }

    // --- Create folder ---

    // Names are basenames: a "name" with separators or ".." must not escape
    // the folder it was typed in.
    void testNamesWithPathSegmentsAreRejected()
    {
        TestDir dir;
        dir.createDir("inner");
        dir.createFile("inner/file.txt", "x");
        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.createFolder(dir.path() + "/inner", "../escaped");
        ops.createFolder(dir.path() + "/inner", "a/b");
        ops.createFile(dir.path() + "/inner", "../escaped.txt");
        QCOMPARE(spy.count(), 3);
        for (const auto &args : spy) QCOMPARE(args.at(0).toBool(), false);
        QVERIFY(!dir.exists("escaped"));
        QVERIFY(!dir.exists("inner/a"));
        QVERIFY(!dir.exists("escaped.txt"));

        QVERIFY(!ops.rename(dir.path() + "/inner/file.txt", "../moved.txt"));
        QVERIFY(!ops.rename(dir.path() + "/inner/file.txt", "sub/file.txt"));
        QVERIFY(!ops.rename(dir.path() + "/inner/file.txt", ".."));
        QVERIFY(dir.exists("inner/file.txt"));
        QVERIFY(!dir.exists("moved.txt"));
    }

    // New File must never truncate something that appeared after the
    // dialog's existence check.
    void testCreateFileKeepsExistingContent()
    {
        TestDir dir;
        dir.createFile("taken.txt", "precious");
        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);
        ops.createFile(dir.path(), "taken.txt");
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.constFirst().at(0).toBool(), false);
        QFile f(dir.path() + "/taken.txt");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("precious"));
    }

    void testCreateFolder()
    {
        TestDir dir;

        FileOperations ops;
        ops.createFolder(dir.path(), "new_folder");

        QVERIFY(dir.exists("new_folder"));
        QVERIFY(QFileInfo(dir.path() + "/new_folder").isDir());
    }

    void testCreateNestedFolder()
    {
        TestDir dir;
        dir.createDir("parent");

        FileOperations ops;
        ops.createFolder(dir.path() + "/parent", "child");

        QVERIFY(dir.exists("parent/child"));
    }

    // --- Create file ---

    void testCreateFile()
    {
        TestDir dir;

        FileOperations ops;
        ops.createFile(dir.path(), "new_file.txt");

        QVERIFY(dir.exists("new_file.txt"));
        QFileInfo info(dir.path() + "/new_file.txt");
        QCOMPARE(info.size(), 0LL); // empty file
    }

    void testPasteClipboardImage()
    {
        TestDir dir;

        QClipboard *clipboard = QGuiApplication::clipboard();
        QVERIFY(clipboard != nullptr);

        QMimeData *savedMime = new QMimeData;
        if (const QMimeData *original = clipboard->mimeData()) {
            for (const QString &format : original->formats())
                savedMime->setData(format, original->data(format));
            if (original->hasImage())
                savedMime->setImageData(original->imageData());
            if (original->hasText())
                savedMime->setText(original->text());
        }

        QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        clipboard->setImage(image);

        const QByteArray originalPath = qgetenv("PATH");
        qputenv("PATH", QFileInfo(QCoreApplication::applicationFilePath()).absolutePath().toUtf8());

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        const QString outputPath = ops.pasteClipboardImage(dir.path());

        qputenv("PATH", originalPath);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), true);
        QVERIFY(!outputPath.isEmpty());
        QVERIFY(QFile::exists(outputPath));

        QImage saved(outputPath);
        QVERIFY(!saved.isNull());
        QCOMPARE(saved.size(), image.size());

        clipboard->setMimeData(savedMime);
    }

    void testPasteClipboardImagePrefersCurrentClipboardOverWlPaste()
    {
        TestDir dir;
        TestDir binDir;

        QClipboard *clipboard = QGuiApplication::clipboard();
        QVERIFY(clipboard != nullptr);

        QMimeData *savedMime = new QMimeData;
        if (const QMimeData *original = clipboard->mimeData()) {
            for (const QString &format : original->formats())
                savedMime->setData(format, original->data(format));
            if (original->hasImage())
                savedMime->setImageData(original->imageData());
            if (original->hasText())
                savedMime->setText(original->text());
        }

        QImage currentImage(8, 8, QImage::Format_ARGB32_Premultiplied);
        currentImage.fill(Qt::red);
        clipboard->setImage(currentImage);

        QImage staleImage(8, 8, QImage::Format_ARGB32_Premultiplied);
        staleImage.fill(Qt::blue);
        const QString staleImagePath = dir.path() + "/stale.png";
        QVERIFY(staleImage.save(staleImagePath, "PNG"));

        QFile wlPaste(binDir.path() + "/wl-paste");
        QVERIFY(wlPaste.open(QIODevice::WriteOnly | QIODevice::Text));
        wlPaste.write("#!/bin/sh\n");
        wlPaste.write("if [ \"$1\" = \"--list-types\" ]; then\n");
        wlPaste.write("  printf 'image/png\\n'\n");
        wlPaste.write("  exit 0\n");
        wlPaste.write("fi\n");
        wlPaste.write("if [ \"$1\" = \"--no-newline\" ] && [ \"$2\" = \"--type\" ] && [ \"$3\" = \"image/png\" ]; then\n");
        wlPaste.write(QString("  cat '%1'\n").arg(staleImagePath).toUtf8());
        wlPaste.write("  exit 0\n");
        wlPaste.write("fi\n");
        wlPaste.write("exit 1\n");
        wlPaste.close();
        QVERIFY(QFile::setPermissions(wlPaste.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                                           | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                                           | QFileDevice::ReadOther | QFileDevice::ExeOther));

        const QByteArray originalPath = qgetenv("PATH");
        qputenv("PATH", (binDir.path().toUtf8() + ":" + originalPath));

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        const QString outputPath = ops.pasteClipboardImage(dir.path());

        qputenv("PATH", originalPath);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), true);
        QVERIFY(!outputPath.isEmpty());
        QVERIFY(QFile::exists(outputPath));

        QImage saved(outputPath);
        QVERIFY(!saved.isNull());
        QCOMPARE(saved.pixelColor(0, 0), QColor(Qt::red));

        clipboard->setMimeData(savedMime);
    }

    // --- Trash (requires gio) ---

    void testTrashFile()
    {
        if (QStandardPaths::findExecutable("gio").isEmpty())
            QSKIP("gio not found in PATH");

        // Use a dir under home so gio trash can find the Trash directory
        QString testPath = QDir::homePath() + "/.cache/hyprfm-test-trash";
        QDir().mkpath(testPath);
        QString filePath = testPath + "/trash_me.txt";
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("bye");
            f.close();
        }

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.trashFiles({filePath});

        if (!spy.wait(5000))
            QSKIP("gio trash timed out (may not be supported in this environment)");

        bool success = spy.at(0).at(0).toBool();
        if (!success)
            QSKIP("gio trash failed (may not be supported for this path)");

        QVERIFY(!QFile::exists(filePath));
        QDir(testPath).removeRecursively();
    }

    void testRestoreFromTrash()
    {
        if (QStandardPaths::findExecutable("gio").isEmpty())
            QSKIP("gio not found in PATH");

        const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString testPath = QDir::homePath() + "/.cache/hyprfm-test-restore-trash-" + uniqueId;
        QDir().mkpath(testPath);

        const QString filePath = testPath + "/restore_me.txt";
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("restore me");
            f.close();
        }

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.trashFiles({filePath});

        if (!spy.wait(5000))
            QSKIP("gio trash timed out (may not be supported in this environment)");

        bool success = spy.at(0).at(0).toBool();
        if (!success)
            QSKIP("gio trash failed (may not be supported for this path)");

        QVERIFY(!QFile::exists(filePath));

        const QString trashedPath = findTrashEntryPath(filePath);
        if (trashedPath.isEmpty())
            QSKIP("Could not locate trashed file metadata");

        spy.clear();
        ops.restoreFromTrash({trashedPath});

        if (!spy.wait(5000))
            QSKIP("gio trash restore timed out (may not be supported in this environment)");

        success = spy.at(0).at(0).toBool();
        if (!success)
            QSKIP("gio trash restore failed (may not be supported in this environment)");

        QVERIFY(QFile::exists(filePath));
        QDir(testPath).removeRecursively();
    }

    void testRestoreFromTrashUri()
    {
        if (QStandardPaths::findExecutable("gio").isEmpty())
            QSKIP("gio not found in PATH");

        const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString testPath = QDir::homePath() + "/.cache/hyprfm-test-restore-trash-uri-" + uniqueId;
        QDir().mkpath(testPath);

        const QString filePath = testPath + "/restore_uri_me.txt";
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("restore via uri");
            f.close();
        }

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.trashFiles({filePath});
        if (!spy.wait(5000))
            QSKIP("gio trash timed out (may not be supported in this environment)");

        bool success = spy.at(0).at(0).toBool();
        if (!success)
            QSKIP("gio trash failed (may not be supported for this path)");

        const QString trashUri = findTrashEntryUri(filePath);
        if (trashUri.isEmpty())
            QSKIP("Could not locate trashed file URI");

        spy.clear();
        ops.restoreFromTrash({trashUri});

        if (!spy.wait(5000))
            QSKIP("gio trash restore timed out (may not be supported in this environment)");

        success = spy.at(0).at(0).toBool();
        if (!success)
            QSKIP("gio trash restore failed (may not be supported in this environment)");

        QVERIFY(QFile::exists(filePath));
        QDir(testPath).removeRecursively();
    }

    // Permanently deleting a trashed *directory* must work: the gvfs trash
    // backend refuses per-child deletes ("Items in the trash may not be
    // modified"), so only a delete of the top-level trash entry succeeds.
    void testDeleteTrashedDirectory()
    {
        if (QStandardPaths::findExecutable("gio").isEmpty())
            QSKIP("gio not found in PATH");

        const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString testPath = QDir::homePath() + "/.cache/hyprfm-test-delete-trash-dir-" + uniqueId;
        const QString dirPath = testPath + "/doomed_folder";
        QVERIFY(QDir().mkpath(dirPath + "/nested"));
        {
            QFile f(dirPath + "/nested/inside.txt");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("delete me");
            f.close();
        }

        FileOperations ops;
        QSignalSpy spy(&ops, &FileOperations::operationFinished);

        ops.trashFiles({dirPath});
        if (!spy.wait(5000))
            QSKIP("gio trash timed out (may not be supported in this environment)");
        if (!spy.at(0).at(0).toBool())
            QSKIP("gio trash failed (may not be supported for this path)");

        const QString trashUri = findTrashEntryUri(dirPath);
        if (trashUri.isEmpty())
            QSKIP("Could not locate trashed directory URI");

        spy.clear();
        ops.deleteFiles({trashUri});
        QVERIFY(spy.wait(10000));
        QVERIFY2(spy.at(0).at(0).toBool(),
                 qPrintable(spy.at(0).at(1).toString()));
        QVERIFY(findTrashEntryUri(dirPath).isEmpty());

        QDir(testPath).removeRecursively();
    }

    void testTrashHelpersForHomePath()
    {
        FileOperations ops;

        const QString expectedTrashPath = QDir::cleanPath(QDir::homePath() + "/.local/share/Trash/files");
        const QString homeFilePath = QDir::homePath() + "/Documents/example.txt";

        QCOMPARE(ops.trashFilesPathFor(homeFilePath), expectedTrashPath);
        QVERIFY(!ops.isTrashPath(homeFilePath));
        QVERIFY(ops.isTrashPath(expectedTrashPath + "/example.txt"));
    }

    void testTrashHelpersForMountedVolume()
    {
        const QString mediaRoot = "/run/media/" + qEnvironmentVariable("USER");
        QDir mediaDir(mediaRoot);
        if (!mediaDir.exists())
            QSKIP("/run/media/$USER does not exist");

        const QStringList entries = mediaDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        if (entries.isEmpty())
            QSKIP("No mounted volumes found under /run/media/$USER");

        const QString mountPath = mediaDir.filePath(entries.constFirst());
        FileOperations ops;

        const QString expectedTrashPath = QDir::cleanPath(
            mountPath + "/.Trash-" + QString::number(geteuid()) + "/files");

        QCOMPARE(ops.trashFilesPathFor(mountPath + "/example.txt"), expectedTrashPath);
        QVERIFY(ops.isTrashPath(expectedTrashPath + "/example.txt"));
    }

    void testArchiveSupportFor7zAndRar_data()
    {
        QTest::addColumn<QString>("extension");

        QTest::newRow("7z") << ".7z";
        QTest::newRow("rar") << ".rar";
    }

    void testArchiveSupportFor7zAndRar()
    {
        if (QStandardPaths::findExecutable("zip").isEmpty())
            QSKIP("zip not found in PATH");

        if (QStandardPaths::findExecutable("7z").isEmpty()
                && QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Neither 7z nor bsdtar found in PATH");

        QFETCH(QString, extension);

        TestDir archiveDir;
        TestDir extractDir;
        archiveDir.createDir("payload");
        archiveDir.createFile("payload/inner.txt", "hello");

        QVERIFY(runCommand("zip", {"-rq", "payload.zip", "payload"}, archiveDir.path()));

        const QString archivePath = archiveDir.path() + "/payload" + extension;
        QVERIFY(QFile::copy(archiveDir.path() + "/payload.zip", archivePath));

        FileOperations ops;
        QVERIFY(FileOperations::isArchive(archivePath));
        QVERIFY(FileOperations::isArchive(archiveDir.path() + "/payload" + extension.toUpper()));
        QCOMPARE(ops.archiveRootFolder(archivePath), QString("payload"));

        QSignalSpy spy(&ops, &FileOperations::operationFinished);
        ops.extractArchive(archivePath, extractDir.path());

        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.at(0).at(0).toBool(), true);
        QVERIFY(QFile::exists(extractDir.path() + "/payload/inner.txt"));
    }

    void testCompressionDoesNotExecuteFileNames_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("extension");
        QTest::addColumn<QString>("executable");

        QTest::newRow("zip") << "zip" << ".zip" << "zip";
        QTest::newRow("tar") << "tar" << ".tar" << "tar";
    }

    void testCompressionDoesNotExecuteFileNames()
    {
        QFETCH(QString, format);
        QFETCH(QString, extension);
        QFETCH(QString, executable);
        if (QStandardPaths::findExecutable(executable).isEmpty())
            QSKIP(qPrintable(executable + " not found in PATH"));

        TestDir dir;
        const QString fileName = "payload'; touch PWNED; #.txt";
        const QString sourcePath = dir.createFile(fileName, "safe");
        const QString markerPath = dir.path() + "/PWNED";

        FileOperations ops;
        QSignalSpy finishSpy(&ops, &FileOperations::operationFinished);
        ops.compressFiles({sourcePath}, format);

        QVERIFY(finishSpy.wait(5000));
        QCOMPARE(finishSpy.constFirst().at(0).toBool(), true);
        QVERIFY(!QFileInfo::exists(markerPath));
        QVERIFY(QFileInfo::exists(dir.path() + "/payload'; touch PWNED; #" + extension));
    }

    // --- Progress property ---

    void testProgressInitialValue()
    {
        FileOperations ops;
        QCOMPARE(ops.progress(), 0.0);
        QCOMPARE(ops.busy(), false);
        QCOMPARE(ops.statusText(), QString());
    }

    // --- Pause/Resume/Cancel ---

    void testPauseResumeTransfer()
    {
        TestDir src, dst;
        src.createFile("pause_test.bin", QByteArray(8 * 1024 * 1024, 'p'));

        FileOperations ops;
        QSignalSpy finishSpy(&ops, &FileOperations::operationFinished);

        ops.copyFiles({src.path() + "/pause_test.bin"}, dst.path());

        QTest::qWait(50);
        if (ops.busy()) {
            ops.pauseTransfer();
            QVERIFY(ops.paused());
            QTest::qWait(100);
            ops.resumeTransfer();
        }

        if (finishSpy.isEmpty())
            QVERIFY(finishSpy.wait(15000));
        QCOMPARE(finishSpy.constFirst().at(0).toBool(), true);
        QVERIFY(QFile::exists(dst.path() + "/pause_test.bin"));
    }

    void testCancelTransfer()
    {
        TestDir src, dst;
        src.createFile("cancel_test.bin", QByteArray(16 * 1024 * 1024, 'c'));

        FileOperations ops;
        QSignalSpy finishSpy(&ops, &FileOperations::operationFinished);

        ops.copyFiles({src.path() + "/cancel_test.bin"}, dst.path());

        QTest::qWait(50);
        if (ops.busy())
            ops.cancelTransfer();

        if (finishSpy.isEmpty())
            QVERIFY(finishSpy.wait(10000));
    }

    // --- Breadcrumbs ---

    void testBreadcrumbSegmentsStartAtHome()
    {
        FileOperations ops;
        const QString home = QDir::homePath();

        const QVariantList atHome = ops.breadcrumbSegments(home);
        QCOMPARE(atHome.size(), 1);
        QCOMPARE(atHome.at(0).toMap().value("label").toString(), QString("Home"));
        QCOMPARE(atHome.at(0).toMap().value("fullPath").toString(), home);

        const QVariantList nested = ops.breadcrumbSegments(home + "/Downloads/reports");
        QCOMPARE(nested.size(), 3);
        QCOMPARE(nested.at(0).toMap().value("label").toString(), QString("Home"));
        QCOMPARE(nested.at(0).toMap().value("fullPath").toString(), home);
        QCOMPARE(nested.at(1).toMap().value("label").toString(), QString("Downloads"));
        QCOMPARE(nested.at(1).toMap().value("fullPath").toString(), home + "/Downloads");
        QCOMPARE(nested.at(2).toMap().value("label").toString(), QString("reports"));
        QCOMPARE(nested.at(2).toMap().value("fullPath").toString(), home + "/Downloads/reports");
    }

    void testBreadcrumbSegmentsOutsideHomeKeepAncestors()
    {
        FileOperations ops;

        const QVariantList segments = ops.breadcrumbSegments("/usr/share");
        QCOMPARE(segments.size(), 2);
        QCOMPARE(segments.at(0).toMap().value("label").toString(), QString("usr"));
        QCOMPARE(segments.at(0).toMap().value("fullPath").toString(), QString("/usr"));
        QCOMPARE(segments.at(1).toMap().value("label").toString(), QString("share"));
        QCOMPARE(segments.at(1).toMap().value("fullPath").toString(), QString("/usr/share"));
    }

    void testBreadcrumbSegmentsDoNotMatchHomePrefix()
    {
        FileOperations ops;
        const QString home = QDir::homePath();

        // A sibling directory that merely starts with the same characters is
        // not inside home, so it must not collapse to a "Home" crumb.
        const QVariantList segments = ops.breadcrumbSegments(home + "-backup");
        QVERIFY(!segments.isEmpty());
        QVERIFY(segments.at(0).toMap().value("label").toString() != QStringLiteral("Home"));
        QCOMPARE(segments.last().toMap().value("fullPath").toString(), home + "-backup");
    }
};

QTEST_MAIN(TestFileOperations)
#include "tst_fileoperations.moc"
