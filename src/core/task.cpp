#include "task.h"

#include <QLoggingCategory>

#include <algorithm>
#include <exception>

namespace meld {

Q_LOGGING_CATEGORY(lcTask, "meld.task")

namespace {

class FunctionTask final : public Task {
public:
    explicit FunctionTask(std::function<bool()> function)
        : function_(std::move(function))
    {
    }

    StepResult step() override { return {function_ && function_(), {}}; }

private:
    std::function<bool()> function_;
};

class GeneratorTask final : public Task {
public:
    explicit GeneratorTask(TaskGenerator generator)
        : generator_(std::move(generator))
    {
    }

    StepResult step() override
    {
        if (!started_) {
            started_ = true;
            iterator_ = generator_.begin();
        } else if (iterator_ && *iterator_ != generator_.end()) {
            ++*iterator_;
        }
        if (!iterator_ || *iterator_ == generator_.end())
            return {false, {}};
        return {true, **iterator_};
    }

private:
    TaskGenerator generator_;
    std::optional<decltype(std::declval<TaskGenerator &>().begin())> iterator_;
    bool started_ = false;
};

//! Proxy used when a scheduler is added as a task of another scheduler,
//! so that the child's lifetime is not tied to the parent's task list.
class SchedulerTask final : public Task {
public:
    explicit SchedulerTask(SchedulerBase *scheduler)
        : scheduler_(scheduler)
    {
    }

    StepResult step() override { return scheduler_ ? scheduler_->step() : StepResult{}; }
    void detach() { scheduler_ = nullptr; }

private:
    SchedulerBase *scheduler_;
};

} // namespace

TaskPtr makeTask(std::function<bool()> function)
{
    return std::make_shared<FunctionTask>(std::move(function));
}

TaskPtr makeOneShotTask(std::function<void()> function)
{
    return std::make_shared<FunctionTask>([function = std::move(function)]() {
        if (function)
            function();
        return false;
    });
}

TaskPtr makeTask(TaskGenerator generator)
{
    return std::make_shared<GeneratorTask>(std::move(generator));
}

SchedulerBase::SchedulerBase() = default;

SchedulerBase::~SchedulerBase()
{
    for (SchedulerBase *parent : std::vector<SchedulerBase *>(parents_))
        parent->removeScheduler(this);
    for (SchedulerBase *child : std::vector<SchedulerBase *>(children_))
        removeScheduler(child);
    if (selfTask_)
        static_cast<SchedulerTask *>(selfTask_.get())->detach();
}

void SchedulerBase::connectRunnable(const void *owner, RunnableCallback callback)
{
    disconnectRunnable(owner);
    callbacks_.push_back({owner, std::move(callback)});
}

void SchedulerBase::disconnectRunnable(const void *owner)
{
    std::erase_if(callbacks_, [owner](const Callback &c) { return c.owner == owner; });
}

TaskPtr SchedulerBase::addTask(TaskPtr task, bool atFront)
{
    removeTask(task);

    if (atFront)
        tasks_.insert(tasks_.begin(), task);
    else
        tasks_.push_back(task);

    // Callbacks may modify the callback list, so iterate over a copy
    const std::vector<Callback> callbacks = callbacks_;
    for (const Callback &callback : callbacks)
        callback.callback(this);
    return task;
}

TaskPtr SchedulerBase::addTask(std::function<bool()> function, bool atFront)
{
    return addTask(makeTask(std::move(function)), atFront);
}

TaskPtr SchedulerBase::addTask(TaskGenerator generator, bool atFront)
{
    return addTask(makeTask(std::move(generator)), atFront);
}

TaskPtr SchedulerBase::addOneShot(std::function<void()> function, bool atFront)
{
    return addTask(makeOneShotTask(std::move(function)), atFront);
}

void SchedulerBase::removeTask(const TaskPtr &task)
{
    auto it = std::find(tasks_.begin(), tasks_.end(), task);
    if (it != tasks_.end())
        tasks_.erase(it);
}

void SchedulerBase::removeAllTasks()
{
    tasks_.clear();
}

TaskPtr SchedulerBase::asTask()
{
    if (!selfTask_)
        selfTask_ = std::make_shared<SchedulerTask>(this);
    return selfTask_;
}

void SchedulerBase::addScheduler(SchedulerBase *scheduler)
{
    if (std::find(children_.begin(), children_.end(), scheduler) != children_.end())
        return;
    children_.push_back(scheduler);
    scheduler->parents_.push_back(this);
    scheduler->connectRunnable(this, [this](SchedulerBase *child) { addTask(child->asTask()); });
    if (scheduler->tasksPending())
        addTask(scheduler->asTask());
}

void SchedulerBase::removeScheduler(SchedulerBase *scheduler)
{
    removeTask(scheduler->asTask());
    scheduler->disconnectRunnable(this);
    std::erase(children_, scheduler);
    std::erase(scheduler->parents_, this);
}

StepResult SchedulerBase::iteration()
{
    TaskPtr task = currentTask();
    if (!task)
        return {};

    StepResult result;
    try {
        result = task->step();
    } catch (const std::exception &e) {
        qCWarning(lcTask) << "Task failed:" << e.what();
        result = {};
    } catch (...) {
        qCWarning(lcTask) << "Task failed with an unknown exception";
        result = {};
    }

    if (result.keep)
        return result;
    removeTask(task);
    return {};
}

void SchedulerBase::completeTasks()
{
    while (tasksPending())
        iteration();
}

StepResult SchedulerBase::step()
{
    // Run an iteration of the current task
    if (!tasks_.empty()) {
        StepResult result = iteration();
        if (result.keep)
            return result;
    }
    return {tasksPending(), {}};
}

TaskPtr LifoScheduler::currentTask() const
{
    return tasks_.empty() ? TaskPtr() : tasks_.back();
}

TaskPtr FifoScheduler::currentTask() const
{
    return tasks_.empty() ? TaskPtr() : tasks_.front();
}

} // namespace meld
