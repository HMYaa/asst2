#include "tasksys.h"
#include <algorithm>
#include <cassert>

struct BulkLaunch {
    IRunnable* runnable = nullptr;
    int num_tasks = 0;
    std::atomic<int> next_task{0};        // task 抢号游标
    std::atomic<int> remaining{0};        // 仍未完成的 worker copy 数
    std::atomic<int> unfinished_deps{0};  // 仍未满足的前驱数
    std::atomic<bool> done{false};        // 本 launch 是否完成并已传播

    BulkLaunch(IRunnable* r, int n, int remaining_init)
        : runnable(r), num_tasks(n), next_task(0), remaining(remaining_init),
          unfinished_deps(0), done(false) {}
};

IRunnable::~IRunnable() {}
ITaskSystem::ITaskSystem(int num_threads) {}
ITaskSystem::~ITaskSystem() {}

const char* TaskSystemSerial::name() { return "Serial"; }
TaskSystemSerial::TaskSystemSerial(int num_threads) : ITaskSystem(num_threads) {}
TaskSystemSerial::~TaskSystemSerial() {}

void TaskSystemSerial::run(IRunnable* runnable, int num_total_tasks) {
    for (int i = 0; i < num_total_tasks; i++) runnable->runTask(i, num_total_tasks);
}

TaskID TaskSystemSerial::runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                          const std::vector<TaskID>& deps) {
    for (int i = 0; i < num_total_tasks; i++) runnable->runTask(i, num_total_tasks);
    return 0;
}

void TaskSystemSerial::sync() {}

const char* TaskSystemParallelSpawn::name() { return "Parallel + Always Spawn"; }
TaskSystemParallelSpawn::TaskSystemParallelSpawn(int num_threads) : ITaskSystem(num_threads) {}
TaskSystemParallelSpawn::~TaskSystemParallelSpawn() {}

void TaskSystemParallelSpawn::run(IRunnable* runnable, int num_total_tasks) {
    for (int i = 0; i < num_total_tasks; i++) runnable->runTask(i, num_total_tasks);
}

TaskID TaskSystemParallelSpawn::runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                                 const std::vector<TaskID>& deps) {
    for (int i = 0; i < num_total_tasks; i++) runnable->runTask(i, num_total_tasks);
    return 0;
}

void TaskSystemParallelSpawn::sync() {}

const char* TaskSystemParallelThreadPoolSpinning::name() { return "Parallel + Thread Pool + Spin"; }
TaskSystemParallelThreadPoolSpinning::TaskSystemParallelThreadPoolSpinning(int num_threads)
    : ITaskSystem(num_threads) {}
TaskSystemParallelThreadPoolSpinning::~TaskSystemParallelThreadPoolSpinning() {}

void TaskSystemParallelThreadPoolSpinning::run(IRunnable* runnable, int num_total_tasks) {
    for (int i = 0; i < num_total_tasks; i++) runnable->runTask(i, num_total_tasks);
}

TaskID TaskSystemParallelThreadPoolSpinning::runAsyncWithDeps(
    IRunnable* runnable, int num_total_tasks, const std::vector<TaskID>& deps) {
    for (int i = 0; i < num_total_tasks; i++) runnable->runTask(i, num_total_tasks);
    return 0;
}

void TaskSystemParallelThreadPoolSpinning::sync() {}

const char* TaskSystemParallelThreadPoolSleeping::name() {
    return "Parallel + Thread Pool + Sleep";
}

// 创建 worker 线程池，任务图按需扩展。
TaskSystemParallelThreadPoolSleeping::TaskSystemParallelThreadPoolSleeping(int num_threads)
    : ITaskSystem(num_threads), num_workers_(num_threads), stop_(false), outstanding_tasks_(0) {
    for (int i = 0; i < num_threads; ++i) workers_.emplace_back([this] { worker_loop(); });
}

// 停机协议：stop -> notify -> join。
TaskSystemParallelThreadPoolSleeping::~TaskSystemParallelThreadPoolSleeping() {
    stop_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(graph_mtx_);
        cv_work_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(sync_mtx_);
        cv_sync_.notify_all();
    }
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

// 持有 graph_mtx_：标记完成，传播到后继，可运行后继入队，并尝试唤醒 sync。
void TaskSystemParallelThreadPoolSleeping::complete_bulk_launch_unlocked(TaskID launch_id) {
    launches_[static_cast<size_t>(launch_id)]->done.store(true, std::memory_order_release);

    if (launch_id < static_cast<TaskID>(successors_.size())) {
        bool any_ready = false;
        for (TaskID succ : successors_[launch_id]) {
            BulkLaunch* s = launches_[static_cast<size_t>(succ)].get();
            if (s->unfinished_deps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                const int succ_n = s->num_tasks;
                const int copies = (succ_n > 0) ? std::min(succ_n, num_workers_) : 1;
                for (int i = 0; i < copies; ++i) {
                    ready_queue_.push_back(succ);
                    ready_count_.fetch_add(1, std::memory_order_release);
                }
                any_ready = true;
            }
        }
        if (any_ready) cv_work_.notify_all();
    }

    if (outstanding_tasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> slk(sync_mtx_);
        cv_sync_.notify_all();
    }
}

void TaskSystemParallelThreadPoolSleeping::complete_bulk_launch(TaskID launch_id) {
    std::lock_guard<std::mutex> lk(graph_mtx_);
    complete_bulk_launch_unlocked(launch_id);
}

// worker：取 ready launch，抢号执行；最后一个 copy 完成时触发传播。
void TaskSystemParallelThreadPoolSleeping::worker_loop() {
    while (true) {
        TaskID lid = -1;

        static constexpr int SPIN_LIMIT = 500;
        for (int spin = 0; spin < SPIN_LIMIT; ++spin) {
            if (stop_.load(std::memory_order_acquire)) return;
            if (ready_count_.load(std::memory_order_acquire) > 0) break;
            std::this_thread::yield();
        }
        if (stop_.load(std::memory_order_acquire)) return;
        // 等任务 + 取任务
        {
            std::unique_lock<std::mutex> lk(graph_mtx_);
            if (ready_queue_.empty()) {
                cv_work_.wait(lk, [this] {
                    return stop_.load(std::memory_order_acquire) || !ready_queue_.empty();
                });
            }
            if (stop_.load(std::memory_order_acquire)) return;
            lid = ready_queue_.front();
            ready_queue_.pop_front();
            ready_count_.fetch_sub(1, std::memory_order_relaxed);
        }
        // 执行任务
        BulkLaunch& L = *launches_[static_cast<size_t>(lid)];
        const int n = L.num_tasks;
        // 0-task batch 直接完成
        if (n == 0) {
            complete_bulk_launch(lid);
            continue;
        }
        // N-task batch 抢号执行
        while (true) {
            const int start = L.next_task.fetch_add(1, std::memory_order_relaxed);
            if (start >= n) {
                if (L.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    complete_bulk_launch(lid);
                }
                break;
            }
            L.runnable->runTask(start, n);
        }
    }
}

// 同步接口复用异步路径。
void TaskSystemParallelThreadPoolSleeping::run(IRunnable* runnable, int num_total_tasks) {
    std::vector<TaskID> no_deps;
    runAsyncWithDeps(runnable, num_total_tasks, no_deps);
    sync();
}

// 注册 launch，建依赖边；若依赖满足则发布到 ready 队列。
// 输入：runnable, num_total_tasks, deps
// 输出：新 launch 的 TaskID
TaskID TaskSystemParallelThreadPoolSleeping::runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                                              const std::vector<TaskID>& deps) {
    TaskID id = 0;
    bool schedule_now = false;
    {
        std::lock_guard<std::mutex> lk(graph_mtx_);

        const int remaining_init =
            (num_total_tasks > 0) ? std::min(num_total_tasks, num_workers_) : 0;
        launches_.emplace_back(new BulkLaunch(runnable, num_total_tasks, remaining_init));
        id = static_cast<TaskID>(launches_.size() - 1);
        successors_.resize(launches_.size());
        // 建依赖边
        int unresolved = 0;
        for (TaskID d : deps) {
            assert(d < id);
            BulkLaunch* dep = launches_[static_cast<size_t>(d)].get();
            if (!dep->done.load(std::memory_order_acquire)) {
                ++unresolved;
                successors_[static_cast<size_t>(d)].push_back(id);
            }
        }

        BulkLaunch* me = launches_[static_cast<size_t>(id)].get();
        me->unfinished_deps.store(unresolved, std::memory_order_release);
        outstanding_tasks_.fetch_add(1, std::memory_order_relaxed);
        // 0-task batch 和 N-task batch 分别处理
        // 提前梳理没有依赖的情况，避免在 worker 中重复判断
        if (num_total_tasks == 0) {
            // 1. 0-task batch 并且**没有依赖**，直接完成
            if (unresolved == 0) complete_bulk_launch_unlocked(id);
            // 2. 0-task batch 并且有依赖，等待依赖完成
        } else if (unresolved == 0) {
            // 3. N-task batch 并且**没有依赖**，推到 ready_queue_，等待 worker 抢号执行
            const int copies = std::min(num_total_tasks, num_workers_);
            for (int i = 0; i < copies; ++i) {
                ready_queue_.push_back(id);
                ready_count_.fetch_add(1, std::memory_order_release);
            }
            schedule_now = (copies > 0);
        }
        // 4.N-task batch 并且有依赖，等待依赖完成
    }

    if (schedule_now) cv_work_.notify_all();
    return id;
}

// 等待全局未完成 launch 计数归零。
void TaskSystemParallelThreadPoolSleeping::sync() {
    static constexpr int SYNC_SPIN_LIMIT = 2000;
    for (int i = 0; i < SYNC_SPIN_LIMIT; ++i) {
        if (outstanding_tasks_.load(std::memory_order_acquire) == 0) return;
        std::this_thread::yield();
    }
    std::unique_lock<std::mutex> lk(sync_mtx_);
    cv_sync_.wait(lk, [this] { return outstanding_tasks_.load(std::memory_order_acquire) == 0; });
}
