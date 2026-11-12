#include "accelerators.h"

#include <QSet>
#include <QTest>

using namespace meld;

class TestAccelerators : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void acceleratorParse()
    {
        for (const auto &[name, keys] : viewAccelerators()) {
            QVERIFY2(!keys.isEmpty(), qPrintable(name));
            for (const QString &key : keys) {
                const QKeySequence sequence = QKeySequence::fromString(key, QKeySequence::PortableText);
                QVERIFY2(sequence.count() == 1, qPrintable(name + QStringLiteral(": ") + key));
                QVERIFY2(sequence[0].key() != Qt::Key_unknown, qPrintable(name + QStringLiteral(": ") + key));
                // Round-tripping catches typos that Qt parses leniently
                QCOMPARE(sequence.toString(QKeySequence::PortableText), key);
            }
        }
    }

    void acceleratorDuplication()
    {
        const QSet<QKeySequence> allowedDuplicates{
            // Allowed because they're different copy actions across views
            QKeySequence(QStringLiteral("Alt+Left")),
            QKeySequence(QStringLiteral("Alt+Right")),
            // Allowed because they activate different popovers across views
            QKeySequence(QStringLiteral("F8")),
            // Allowed because they're different panel show/hide across views
            QKeySequence(QStringLiteral("F9")),
        };

        QSet<QKeySequence> accels;
        for (const auto &[name, keys] : viewAccelerators()) {
            for (const QString &key : keys) {
                const QKeySequence accel = QKeySequence::fromString(key, QKeySequence::PortableText);
                if (!allowedDuplicates.contains(accel))
                    QVERIFY2(!accels.contains(accel), qPrintable(QStringLiteral("Duplicate accelerator ") + key));
                accels.insert(accel);
            }
        }
    }

    void shortcutsForAction()
    {
        QCOMPARE(meld::shortcutsForAction(QStringLiteral("view.find-next")),
                 (QList<QKeySequence>{QKeySequence(Qt::CTRL | Qt::Key_G), QKeySequence(Qt::Key_F3)}));
        QVERIFY(meld::shortcutsForAction(QStringLiteral("view.no-such-action")).isEmpty());
    }
};

QTEST_MAIN(TestAccelerators)
#include "tst_accelerators.moc"
