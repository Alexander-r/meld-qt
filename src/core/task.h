// Classes to implement scheduling for cooperative threads.
// Python generators become C++23 std::generator coroutines.

#pragma once

#include <QString>

#include <functional>
#include <generator>
#include <memory>
#include <optional>
#include <vector>

namespace meld {

//! The status reported by a single step of a task
struct StepResult {
    //! Whether the task has more work to do
    bool keep = false;
    //! Optional human-readable progress message
    QString status;
};

class Task {
public:
    virtual ~Task() = default;
    virtual StepResult step() = 0;
};

using TaskPtr = std::shared_ptr<Task>;

//! Coroutine tasks yield progress messages (possibly empty) to continue
using TaskGenerator = std::generator<QString>;

//! A task that is called repeatedly while it returns true
TaskPtr makeTask(std::function<bool()> function);
//! A task that runs exactly once
TaskPtr makeOneShotTask(std::function<void()> function);
//! A task that is iterated until the generator is exhausted
TaskPtr makeTask(TaskGenerator generator);

/*!
 * Base class with common functionality for schedulers.
 *
 * A scheduler is itself a task, so schedulers can be nested: the window
 * scheduler runs the schedulers of each comparison tab.
 */
class SchedulerBase : public Task {
public:
    SchedulerBase();
    ~SchedulerBase() override;

    SchedulerBase(const SchedulerBase &) = delete;
    SchedulerBase &operator=(const SchedulerBase &) = delete;

    using RunnableCallback = std::function<void(SchedulerBase *)>;
    //! Called whenever a task is added ("runnable" signal)
    void connectRunnable(const void *owner, RunnableCallback callback);
    void disconnectRunnable(const void *owner);

    //! Add a task, first removing any existing instance of it
    TaskPtr addTask(TaskPtr task, bool atFront = false);
    TaskPtr addTask(std::function<bool()> function, bool atFront = false);
    TaskPtr addTask(TaskGenerator generator, bool atFront = false);
    TaskPtr addOneShot(std::function<void()> function, bool atFront = false);

    void removeTask(const TaskPtr &task);
    void removeAllTasks();

    //! Adds a subscheduler as a child task of this scheduler
    void addScheduler(SchedulerBase *scheduler);
    void removeScheduler(SchedulerBase *scheduler);

    bool tasksPending() const { return !tasks_.empty(); }
    int taskCount() const { return int(tasks_.size()); }
    virtual TaskPtr currentTask() const = 0;

    //! Perform one iteration of the current task
    StepResult iteration();
    //! Run all of the scheduler's current tasks to completion
    void completeTasks();

    //! The task object representing this scheduler inside a parent
    TaskPtr asTask();

    StepResult step() override;

protected:
    std::vector<TaskPtr> tasks_;

private:
    struct Callback {
        const void *owner;
        RunnableCallback callback;
    };
    std::vector<Callback> callbacks_;
    std::shared_ptr<Task> selfTask_;
    std::vector<SchedulerBase *> parents_;
    std::vector<SchedulerBase *> children_;
};

//! Scheduler calling most recently added tasks first
class LifoScheduler : public SchedulerBase {
public:
    TaskPtr currentTask() const override;
};

//! Scheduler calling tasks in the order they were added
class FifoScheduler : public SchedulerBase {
public:
    TaskPtr currentTask() const override;
};

} // namespace meld
