// Whole-window harness: everything main.cpp wires up, loading the real
// Main.qml headless. Used for behaviour that only shows with the full item
// tree in place (event routing between overlays, toolbar, views).
#include <QTest>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QWheelEvent>
#include "models/bookmarkmodel.h"
#include "models/devicemodel.h"
#include "models/filesystemmodel.h"
#include "models/recentfilesmodel.h"
#include "models/searchproxymodel.h"
#include "models/searchresultsmodel.h"
#include "models/tablistmodel.h"
#include "providers/iconprovider.h"
#include "providers/pdfpreviewprovider.h"
#include "providers/thumbnailprovider.h"
#include "services/clipboardmanager.h"
#include "services/configmanager.h"
#include "services/dependencychecker.h"
#include "services/diskusageservice.h"
#include "services/draghelper.h"
#include "services/fileoperations.h"
#include "services/gitstatusservice.h"
#include "services/metadataextractor.h"
#include "services/previewservice.h"
#include "services/remoteaccessservice.h"
#include "services/runtimefeaturesservice.h"
#include "services/searchservice.h"
#include "services/themeloader.h"
#include "services/undomanager.h"

class TestMainWindow : public QObject
{
    Q_OBJECT

    struct App {
        QTemporaryDir home;
        QTemporaryDir moduleDir;
        QQmlApplicationEngine engine;
        QObject owner;   // parents everything so teardown order is sane
        TabListModel *tabModel = nullptr;
        BookmarkModel *bookmarks = nullptr;
        QQuickWindow *window = nullptr;

        bool load()
        {
            QDir().mkpath(moduleDir.path() + "/HyprFM");
            QFile src(QStringLiteral(TEST_MODULE_DIR "/HyprFM/qmldir"));
            QFile dst(moduleDir.path() + "/HyprFM/qmldir");
            if (!src.open(QIODevice::ReadOnly) || !dst.open(QIODevice::WriteOnly))
                return false;
            for (const QByteArray &line : src.readAll().split('\n'))
                if (!line.startsWith("prefer "))
                    dst.write(line + '\n');
            dst.close();
            QFile::link(QStringLiteral(TEST_MODULE_DIR "/HyprFM/qml"), moduleDir.path() + "/HyprFM/qml");

            QQuickStyle::setStyle("Basic");
            const QString configDir = home.path() + "/.config/hyprfm";
            QDir().mkpath(configDir);
            const QStringList themeDirs {QStringLiteral(TEST_SOURCE_DIR "/themes")};

            auto *config = new ConfigManager(configDir + "/config.toml", &owner, themeDirs);
            auto *theme = new ThemeLoader(&owner);
            theme->loadTheme(config->theme(), themeDirs);
            tabModel = new TabListModel(&owner);
            bookmarks = new BookmarkModel(&owner);
            auto *fileOps = new FileOperations(&owner);
            auto *undoManager = new UndoManager(fileOps, &owner);
            auto *clipboard = new ClipboardManager(&owner);
            auto *fsModel = new FileSystemModel(&owner);
            fsModel->setRootPath(QStringLiteral(TEST_SOURCE_DIR "/src"));
            auto *splitFsModel = new FileSystemModel(&owner);
            auto *millerParentModel = new FileSystemModel(&owner);
            auto *millerPreviewModel = new FileSystemModel(&owner);
            auto *searchResults = new SearchResultsModel(&owner);
            auto *searchProxy = new SearchProxyModel(&owner);
            searchProxy->setSourceModel(searchResults);
            auto *splitSearchResults = new SearchResultsModel(&owner);
            auto *splitSearchProxy = new SearchProxyModel(&owner);
            splitSearchProxy->setSourceModel(splitSearchResults);
            auto *searchService = new SearchService(&owner);
            searchService->setResultsModel(searchResults);
            auto *splitSearchService = new SearchService(&owner);
            splitSearchService->setResultsModel(splitSearchResults);
            auto *previewService = new PreviewService(&owner);
            auto *metadataExtractor = new MetadataExtractor(&owner);
            auto *diskUsageService = new DiskUsageService(&owner);
            auto *remoteAccessService = new RemoteAccessService(&owner);
            auto *runtimeFeatures = new RuntimeFeaturesService(&owner);
            auto *recentFiles = new RecentFilesModel(configDir + "/recents.json", &owner);
            auto *devices = new DeviceModel(&owner, true);
            auto *dependencies = new DependencyChecker(&owner);
            auto *iconProvider = new IconProvider(config->iconTheme());
            engine.addImageProvider("thumbnail", new ThumbnailProvider);
            engine.addImageProvider("icon", iconProvider);
            engine.addImageProvider("pdfpreview", new PdfPreviewProvider);
            auto *dragHelper = new DragHelper(iconProvider, &owner);

            engine.addImportPath(moduleDir.path());
            engine.addImportPath(QStringLiteral(TEST_SOURCE_DIR "/src/qml"));
            QQmlContext *ctx = engine.rootContext();
            ctx->setContextProperty("config", config);
            ctx->setContextProperty("theme", theme);
            ctx->setContextProperty("tabModel", tabModel);
            ctx->setContextProperty("bookmarks", bookmarks);
            ctx->setContextProperty("fileOps", fileOps);
            ctx->setContextProperty("undoManager", undoManager);
            ctx->setContextProperty("clipboard", clipboard);
            ctx->setContextProperty("dragHelper", dragHelper);
            ctx->setContextProperty("fsModel", fsModel);
            ctx->setContextProperty("splitFsModel", splitFsModel);
            ctx->setContextProperty("millerParentModel", millerParentModel);
            ctx->setContextProperty("millerPreviewModel", millerPreviewModel);
            ctx->setContextProperty("devices", devices);
            ctx->setContextProperty("recentFiles", recentFiles);
            ctx->setContextProperty("searchProxy", searchProxy);
            ctx->setContextProperty("searchResults", searchResults);
            ctx->setContextProperty("searchService", searchService);
            ctx->setContextProperty("splitSearchProxy", splitSearchProxy);
            ctx->setContextProperty("splitSearchResults", splitSearchResults);
            ctx->setContextProperty("splitSearchService", splitSearchService);
            ctx->setContextProperty("previewService", previewService);
            ctx->setContextProperty("metadataExtractor", metadataExtractor);
            ctx->setContextProperty("diskUsageService", diskUsageService);
            ctx->setContextProperty("remoteAccessService", remoteAccessService);
            ctx->setContextProperty("runtimeFeatures", runtimeFeatures);
            ctx->setContextProperty("dependencies", dependencies);

            engine.load(QUrl::fromLocalFile(QStringLiteral(TEST_SOURCE_DIR "/src/qml/Main.qml")));
            if (engine.rootObjects().isEmpty())
                return false;
            window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            if (!window)
                return false;
            window->resize(1100, 700);
            window->show();
            return QTest::qWaitForWindowExposed(window);
        }

        static QQuickItem *findItem(QQuickItem *parent, const QString &name)
        {
            for (QQuickItem *child : parent->childItems()) {
                if (child->objectName() == name)
                    return child;
                if (QQuickItem *hit = findItem(child, name))
                    return hit;
            }
            return nullptr;
        }
        QQuickItem *item(const QString &name) { return findItem(window->contentItem(), name); }
        QPoint center(QQuickItem *it) { return it->mapToScene(QPointF(it->width() / 2, it->height() / 2)).toPoint(); }
        void wheel(const QPoint &pos, const QPoint &angle)
        {
            QWheelEvent ev(pos, window->mapToGlobal(pos), QPoint(), angle,
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window, &ev);
        }
    };

private slots:
    void testWheelOverTabStripScrollsTabsInTheFullWindow()
    {
        App app;
        QVERIFY(app.load());
        while (app.tabModel->rowCount() < 12)
            app.tabModel->addTab();
        QTest::qWait(700);   // tab bar height animation + enter animations
        QQuickItem *strip = app.item("tabStrip");
        QVERIFY(strip);
        app.tabModel->setActiveIndex(0);
        QTRY_COMPARE(strip->property("contentX").toReal(), 0.0);
        QVERIFY(strip->property("contentWidth").toReal() > strip->width());

        app.wheel(app.center(strip), QPoint(0, -120));
        QTRY_VERIFY2(strip->property("contentX").toReal() > 0,
                     qPrintable(QStringLiteral("contentX stayed %1").arg(strip->property("contentX").toReal())));
    }

    void testInlineBookmarkRenameCommitsOnReturnAndCancelsOnEscape()
    {
        App app;
        QVERIFY(app.load());
        app.bookmarks->setBookmarks({"~/Documents"});
        QQuickItem *sidebar = app.item("sidebarPanel");
        QVERIFY(sidebar);

        QVERIFY(QMetaObject::invokeMethod(sidebar, "startBookmarkRename", Q_ARG(QVariant, 0)));
        QQuickItem *input = app.item("bookmarkRenameInput");
        QVERIFY(input);
        QTRY_VERIFY(input->hasActiveFocus());
        for (QChar c : QStringLiteral("Work"))   // replaces the selected auto name
            QTest::keyClick(app.window, c.toLatin1());
        QTest::keyClick(app.window, Qt::Key_Return);
        QTRY_COMPARE(sidebar->property("renamingBookmarkIndex").toInt(), -1);
        QCOMPARE(app.bookmarks->data(app.bookmarks->index(0), BookmarkModel::NameRole).toString(),
                 QString("Work"));

        QVERIFY(QMetaObject::invokeMethod(sidebar, "startBookmarkRename", Q_ARG(QVariant, 0)));
        QTRY_VERIFY(input->hasActiveFocus());
        for (QChar c : QStringLiteral("Nope"))
            QTest::keyClick(app.window, c.toLatin1());
        QTest::keyClick(app.window, Qt::Key_Escape);
        QTRY_COMPARE(sidebar->property("renamingBookmarkIndex").toInt(), -1);
        QCOMPARE(app.bookmarks->data(app.bookmarks->index(0), BookmarkModel::NameRole).toString(),
                 QString("Work"));
    }
};

QTEST_MAIN(TestMainWindow)
#include "tst_mainwindow.moc"
