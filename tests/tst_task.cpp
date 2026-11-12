// Tests of the cooperative schedulerd

#include "core/task.h"

#include <QTest>

using namespace meld;

namespace {

// Coroutine parameters must be passed by value: references would dangle
TaskGenerator counter(QStringList *log, QString name, int count)
{
    for (int i = 0; i < count; ++i) {
        log->append(QStringLiteral("%1%2").arg(name).arg(i));
        co_yield QStringLiteral("%1 step %2").arg(name).arg(i);
    }
}

} // namespace

class TestTask : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void fifoOrder()
    {
        QStringList log;
        FifoScheduler scheduler;
        scheduler.addTask(counter(&log, QStringLiteral("a"), 2));
        scheduler.addTask(counter(&log, QStringLiteral("b"), 2));
        scheduler.completeTasks();
        QCOMPARE(log, (QStringList{"a0", "a1", "b0", "b1"}));
        QVERIFY(!scheduler.tasksPending());
    }

    void lifoOrder()
    {
        QStringList log;
        LifoScheduler scheduler;
        scheduler.addTask(counter(&log, QStringLiteral("a"), 2));
        scheduler.addTask(counter(&log, QStringLiteral("b"), 2));
        scheduler.completeTasks();
        QCOMPARE(log, (QStringList{"b0", "b1", "a0", "a1"}));
    }

    void statusMessages()
    {
        QStringList log;
        FifoScheduler scheduler;
        scheduler.addTask(counter(&log, QStringLiteral("a"), 1));
        const StepResult result = scheduler.iteration();
        QVERIFY(result.keep);
        QCOMPARE(result.status, QStringLiteral("a step 0"));
        QVERIFY(!scheduler.iteration().keep);
        QVERIFY(!scheduler.tasksPending());
    }

    void functionTasksRepeatWhileTrue()
    {
        int calls = 0;
        FifoScheduler scheduler;
        scheduler.addTask([&calls]() { return ++calls < 3; });
        scheduler.addOneShot([&calls]() { calls += 100; });
        scheduler.completeTasks();
        QCOMPARE(calls, 103);
    }

    void addingExistingTaskMovesIt()
    {
        QStringList log;
        FifoScheduler scheduler;
        const TaskPtr a = makeTask(counter(&log, QStringLiteral("a"), 1));
        scheduler.addTask(a);
        scheduler.addTask(counter(&log, QStringLiteral("b"), 1));
        scheduler.addTask(a);
        QCOMPARE(scheduler.taskCount(), 2);
        scheduler.completeTasks();
        QCOMPARE(log, (QStringList{"b0", "a0"}));
    }

    void exceptionsRemoveTask()
    {
        FifoScheduler scheduler;
        scheduler.addTask([]() -> bool { throw std::runtime_error("boom"); });
        scheduler.iteration();
        QVERIFY(!scheduler.tasksPending());
    }

    void subSchedulers()
    {
        QStringList log;
        LifoScheduler window;
        int runnable = 0;
        window.connectRunnable(this, [&runnable](SchedulerBase *) { ++runnable; });
        FifoScheduler doc1;
        FifoScheduler doc2;
        window.addScheduler(&doc1);
        window.addScheduler(&doc2);

        doc1.addTask(counter(&log, QStringLiteral("a"), 2));
        doc2.addTask(counter(&log, QStringLiteral("b"), 2));
        QCOMPARE(runnable, 2);
        // The most recently runnable document scheduler runs first
        window.completeTasks();
        QCOMPARE(log, (QStringList{"b0", "b1", "a0", "a1"}));

        window.removeScheduler(&doc1);
        doc1.addTask(counter(&log, QStringLiteral("c"), 1));
        QVERIFY(!window.tasksPending());
    }

    void destroyedSubSchedulerIsDetached()
    {
        LifoScheduler window;
        {
            FifoScheduler doc;
            window.addScheduler(&doc);
            doc.addOneShot([]() {});
            QVERIFY(window.tasksPending());
        }
        QVERIFY(!window.tasksPending());
        window.completeTasks();
    }
};

QTEST_GUILESS_MAIN(TestTask)
#include "tst_task.moc"
