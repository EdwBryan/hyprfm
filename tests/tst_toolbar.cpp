// Headless test for the toolbar tab strip: tabs keep a minimum width and the
// strip scrolls instead of squeezing them.
#include <QTest>
#include <QWheelEvent>
#include "viewharness.h"
#include "models/tablistmodel.h"

class TestToolbar : public QObject
{
    Q_OBJECT

    struct ToolbarHarness : ViewHarness {
        TabListModel &tabs = *new TabListModel(&view);   // dies with the view, after the QML
        bool loadToolbar(int tabCount)
        {
            while (tabs.rowCount() < tabCount)
                tabs.addTab();
            extraContext = [this](QQmlContext *ctx) { ctx->setContextProperty("tabModel", &tabs); };
            return load(QStringLiteral("src/qml/components/Toolbar.qml"), "activeTab");
        }
    };

private slots:
    void testTabsKeepMinimumWidthAndStripScrolls()
    {
        ToolbarHarness h;
        QVERIFY(h.loadToolbar(12));
        QTest::qWait(400);   // enter animations
        QQuickItem *strip = h.item("tabStrip");
        QVERIFY(strip);
        for (int i = 0; i < 12; ++i) {
            QQuickItem *tab = h.item(QStringLiteral("toolbarTab_%1").arg(i));
            QVERIFY2(tab, qPrintable(QStringLiteral("tab %1 missing").arg(i)));
            QVERIFY2(tab->width() >= 120, qPrintable(QStringLiteral("tab %1 is %2 px").arg(i).arg(tab->width())));
        }
        QVERIFY(strip->property("contentWidth").toReal() > strip->width());

        // Activating the last tab scrolls it into view.
        h.tabs.setActiveIndex(11);
        QQuickItem *last = h.item("toolbarTab_11");
        QTRY_VERIFY(strip->property("contentX").toReal() > 0);
        QTRY_VERIFY(last->x() + last->width() <= strip->property("contentX").toReal() + strip->width() + 0.5);
    }

    void testWheelOverStripScrollsIt()
    {
        ToolbarHarness h;
        QVERIFY(h.loadToolbar(12));
        QTest::qWait(400);
        QQuickItem *strip = h.item("tabStrip");
        QVERIFY(strip);
        h.tabs.setActiveIndex(0);
        QTRY_COMPARE(strip->property("contentX").toReal(), 0.0);
        const QPoint pos = h.center(strip);
        QWheelEvent ev(pos, h.view.mapToGlobal(pos), QPoint(), QPoint(0, -120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&h.view, &ev);
        QTRY_VERIFY(strip->property("contentX").toReal() > 0);
    }

    void testHorizontalSwipeScrollsStrip()
    {
        ToolbarHarness h;
        QVERIFY(h.loadToolbar(12));
        QTest::qWait(400);
        QQuickItem *strip = h.item("tabStrip");
        QVERIFY(strip);
        h.tabs.setActiveIndex(0);
        QTRY_COMPARE(strip->property("contentX").toReal(), 0.0);
        const QPoint pos = h.center(strip);
        QWheelEvent ev(pos, h.view.mapToGlobal(pos), QPoint(-60, 0), QPoint(),   // swipe left = scroll right
                       Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
        QCoreApplication::sendEvent(&h.view, &ev);
        QTRY_VERIFY(strip->property("contentX").toReal() > 0);
    }

    void testDragTabReordersIt()
    {
        ToolbarHarness h;
        QVERIFY(h.loadToolbar(3));
        for (int i = 0; i < 3; ++i)
            h.tabs.tabAt(i)->navigateTo(QStringLiteral("/t%1").arg(i));
        QTest::qWait(400);
        QQuickItem *first = h.item("toolbarTab_0");
        QQuickItem *third = h.item("toolbarTab_2");
        QVERIFY(first && third);

        h.drag(h.center(first), h.center(third) + QPoint(20, 0));

        QCOMPARE(h.tabs.tabAt(0)->currentPath(), QString("/t1"));
        QCOMPARE(h.tabs.tabAt(1)->currentPath(), QString("/t2"));
        QCOMPARE(h.tabs.tabAt(2)->currentPath(), QString("/t0"));
        QCOMPARE(h.tabs.activeIndex(), 2);
    }

    void testReorderSlidesDisplacedTabs()
    {
        ToolbarHarness h;
        QVERIFY(h.loadToolbar(3));
        for (int i = 0; i < 3; ++i)
            h.tabs.tabAt(i)->navigateTo(QStringLiteral("/t%1").arg(i));
        QTest::qWait(400);
        QQuickItem *first = h.item("toolbarTab_0");
        QQuickItem *second = h.item("toolbarTab_1");
        QVERIFY(first && second);

        // Press tab 0 and move it into tab 1's slot in one step: tab 1 is
        // displaced left and must start from its old position (offset > 0)
        // and animate back to 0.
        const QPoint from = h.center(first);
        QTest::mousePress(&h.view, Qt::LeftButton, {}, from);
        QTest::mouseMove(&h.view, h.center(second) + QPoint(10, 0), 10);
        QCOMPARE(h.tabs.tabAt(0)->currentPath(), QString("/t1"));
        // The layout repositions on the next polish; the displaced tab then
        // starts from its old spot (positive offset) and eases back to 0.
        QTRY_VERIFY(second->property("slideOffset").toReal() > 1);
        QTRY_COMPARE(second->property("slideOffset").toReal(), 0.0);
        QTest::mouseRelease(&h.view, Qt::LeftButton, {}, h.center(second) + QPoint(10, 0));
    }

    void testFewTabsShareTheWidth()
    {
        ToolbarHarness h;
        QVERIFY(h.loadToolbar(3));
        QTest::qWait(400);
        QQuickItem *strip = h.item("tabStrip");
        QQuickItem *tab = h.item("toolbarTab_0");
        QVERIFY(strip && tab);
        QVERIFY(tab->width() > 200);   // 900 px / 3 tabs
        QCOMPARE(strip->property("contentX").toReal(), 0.0);
    }
};

QTEST_MAIN(TestToolbar)
#include "tst_toolbar.moc"
