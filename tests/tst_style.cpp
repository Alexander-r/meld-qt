#include "icons.h"
#include "settings.h"
#include "style.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace meld;

namespace {
constexpr int BaseFontSize = 11;
}

class TestStyle : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Stub settings with a known font
        QVERIFY(settingsDir_.isValid());
        MeldSettings::useSettingsFile(settingsDir_.filePath(QStringLiteral("meld-qt.ini")));
        MeldSettings *settings = MeldSettings::instance();
        settings->setValue(QStringLiteral("use-system-font"), false);
        settings->setValue(QStringLiteral("custom-font"), QStringLiteral("monospace %1").arg(BaseFontSize));
        QCOMPARE(settings->font().pointSize(), BaseFontSize);
    }

    void init()
    {
        resetFontZoom();
    }

    void defaultSizeIsTheConfiguredFontSize()
    {
        QCOMPARE(editorFont().pointSize(), BaseFontSize);
    }

    void zoomStepsByAPoint()
    {
        QList<int> sizes;
        for (int i = 0; i < 3; ++i) {
            updateFontZoom(1);
            sizes.append(editorFont().pointSize());
        }
        QCOMPARE(sizes, (QList<int>{BaseFontSize + 1, BaseFontSize + 2, BaseFontSize + 3}));

        for (int i = 0; i < 3; ++i)
            updateFontZoom(-1);
        QCOMPARE(editorFont().pointSize(), BaseFontSize);
    }

    void zoomReset()
    {
        updateFontZoom(1);
        QCOMPARE(editorFont().pointSize(), BaseFontSize + 1);

        updateFontZoom(std::nullopt);
        QCOMPARE(editorFont().pointSize(), BaseFontSize);
    }

    void zoomEmitsFontChangedCorrectly()
    {
        QSignalSpy spy(MeldSettings::instance(), &MeldSettings::changed);

        // Check that zooming emits the changed signal
        updateFontZoom(1);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("font"));

        // Resetting is a change here, but resetting again is not
        updateFontZoom(std::nullopt);
        updateFontZoom(std::nullopt);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("font"));

        // Neither is zooming out once we've hit the minimum font size
        updateFontZoom(-BaseFontSize);
        QCOMPARE(editorFont().pointSize(), 1);
        spy.clear();
        updateFontZoom(-1);
        QCOMPARE(spy.count(), 0);
        QCOMPARE(editorFont().pointSize(), 1);
    }

    void changingTheFontResetsTheZoom()
    {
        updateFontZoom(2);
        QCOMPARE(fontZoomOffset(), 2);
        MeldSettings::instance()->setValue(QStringLiteral("custom-font"), QStringLiteral("monospace, 14"));
        QCOMPARE(fontZoomOffset(), 0);
        QCOMPARE(editorFont().pointSize(), 14);
        MeldSettings::instance()->setValue(QStringLiteral("custom-font"), QStringLiteral("monospace %1").arg(BaseFontSize));
    }

    void parseFontDescription_data()
    {
        QTest::addColumn<QString>("description");
        QTest::addColumn<int>("size");
        QTest::addColumn<bool>("bold");
        QTest::addColumn<bool>("italic");

        QTest::newRow("family and size") << "monospace 11" << 11 << false << false;
        QTest::newRow("terminated family list") << "monospace, 14" << 14 << false << false;
        QTest::newRow("style options") << "Monospace Bold Italic 9" << 9 << true << true;
    }

    void parseFontDescription()
    {
        QFETCH(QString, description);
        QFETCH(int, size);
        QFETCH(bool, bold);
        QFETCH(bool, italic);

        const QFont font = MeldSettings::parseFontDescription(description);
        QCOMPARE(font.pointSize(), size);
        QCOMPARE(font.bold(), bold);
        QCOMPARE(font.italic(), italic);
    }

    void darkDetection()
    {
        QVERIFY(!shouldUseDark(QColor(0xfa, 0xfa, 0xfa)));
        QVERIFY(shouldUseDark(QColor(0x24, 0x24, 0x24)));
    }

    void interfaceStyleSetsThePalette()
    {
        // The offscreen platform can't switch its style, like KDE's
        MeldSettings *settings = MeldSettings::instance();
        const QColor systemWindow = QApplication::palette().color(QPalette::Window);
        const bool systemDark = shouldUseDark(systemWindow);

        settings->setValue(QStringLiteral("style-variant"), QStringLiteral("force-dark"));
        QVERIFY(shouldUseDark(QApplication::palette().color(QPalette::Window)));
        QVERIFY(shouldUseDark(QApplication::palette().color(QPalette::Base)));
        QVERIFY(!shouldUseDark(QApplication::palette().color(QPalette::WindowText)));
        // Comparisons follow, with the default scheme
        QVERIFY(currentTheme().dark);

        settings->setValue(QStringLiteral("style-variant"), QStringLiteral("force-light"));
        QVERIFY(!shouldUseDark(QApplication::palette().color(QPalette::Window)));
        QVERIFY(shouldUseDark(QApplication::palette().color(QPalette::WindowText)));
        QVERIFY(!currentTheme().dark);

        settings->setValue(QStringLiteral("style-variant"), QStringLiteral("default"));
        QCOMPARE(QApplication::palette().color(QPalette::Window), systemWindow);
        QCOMPARE(currentTheme().dark, systemDark);
    }

    void changeIconsFollowTheTheme()
    {
        // The change action arrows are drawn on the comparison's colours
        const auto centre = []() {
            const QImage image = icons::icon(QStringLiteral("meld-change-copy")).pixmap(QSize(16, 16)).toImage();
            return image.pixelColor(8, 8);
        };
        MeldSettings *settings = MeldSettings::instance();
        settings->setValue(QStringLiteral("style-variant"), QStringLiteral("force-light"));
        QVERIFY(!currentTheme().dark);
        QCOMPARE(centre(), QColor(Qt::black));

        settings->setValue(QStringLiteral("style-variant"), QStringLiteral("force-dark"));
        QVERIFY(currentTheme().dark);
        QCOMPARE(centre(), QColor(Qt::white));

        // The comparison's colours, rather than the interface's, decide
        settings->setValue(QStringLiteral("style-variant"), QStringLiteral("force-light"));
        settings->setValue(QStringLiteral("style-scheme"), QStringLiteral("meld-dark"));
        QVERIFY(currentTheme().dark);
        QCOMPARE(centre(), QColor(Qt::white));
        settings->resetValue(QStringLiteral("style-scheme"));
        settings->setValue(QStringLiteral("style-variant"), QStringLiteral("default"));
    }

private:
    QTemporaryDir settingsDir_;

};

QTEST_MAIN(TestStyle)
#include "tst_style.moc"
