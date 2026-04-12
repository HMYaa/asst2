#ifndef _TASKSYS_H
#define _TASKSYS_H

#include "itasksys.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// 仅 Sleeping 实现使用；完整定义在 tasksys.cpp（此处 incomplete type 配合 unique_ptr 需在 .cpp 里定义析构）
struct BulkLaunch;

/*
 * TaskSystemSerial: This class is the student's implementation of a
 * serial task execution engine.  See definition of ITaskSystem in
 * itasksys.h for documentation of the ITaskSystem interface.
 */
class TaskSystemSerial: public ITaskSystem {
    public:
        TaskSystemSerial(int num_threads);
        ~TaskSystemSerial();
        const char* name();
        void run(IRunnable* runnable, int num_total_tasks);
        TaskID runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                const std::vector<TaskID>& deps);
        void sync();
};

/*
 * TaskSystemParallelSpawn: This class is the student's implementation of a
 * parallel task execution engine that spawns threads in every run()
 * call.  See definition of ITaskSystem in itasksys.h for documentation
 * of the ITaskSystem interface.
 */
class TaskSystemParallelSpawn: public ITaskSystem {
    public:
        TaskSystemParallelSpawn(int num_threads);
        ~TaskSystemParallelSpawn();
        const char* name();
        void run(IRunnable* runnable, int num_total_tasks);
        TaskID runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                const std::vector<TaskID>& deps);
        void sync();
};

/*
 * TaskSystemParallelThreadPoolSpinning: This class is the student's
 * implementation of a parallel task execution engine that uses a
 * thread pool. See definition of ITaskSystem in itasksys.h for
 * documentation of the ITaskSystem interface.
 */
class TaskSystemParallelThreadPoolSpinning: public ITaskSystem {
    public:
        TaskSystemParallelThreadPoolSpinning(int num_threads);
        ~TaskSystemParallelThreadPoolSpinning();
        const char* name();
        void run(IRunnable* runnable, int num_total_tasks);
        TaskID runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                const std::vector<TaskID>& deps);
        void sync();
};

/*
 * TaskSystemParallelThreadPoolSleeping（Part B 核心）
 *
 * 思路概要（与 README「等待 + 就绪」两阶段一致）：
 *  - 依赖按 bulk launch：后继的任意 task 都必须等依赖 launch 中全部 task 完成。
 *  - 图：每个 launch 一节点；successors_[dep] 存「dep 整批完成后要减 unfinished_deps 的后续 id」。
 *  - ready_queue_：unfinished_deps==0 且有待执行 work 的 launch（含 num_tasks==0 的特殊情况，见 cpp）。
 *  - worker 在锁外对单个 launch 用 next_task 原子抢 chunk，与 Part A 类似。
 *  - outstanding_tasks：已提交且未完成的 runTask 总数；sync() 只等其为 0。
 *  - run() = runAsyncWithDeps(空 deps) + sync()，符合作业「run 与 async 不混用」假设。
 *
 * 可改方向：就绪队列策略、chunk、notify 次数、或为 run() 单独做同步路径减少 async 簿记。
 */
class TaskSystemParallelThreadPoolSleeping: public ITaskSystem {
    public:
        TaskSystemParallelThreadPoolSleeping(int num_threads);
        ~TaskSystemParallelThreadPoolSleeping();
        const char* name();
        void run(IRunnable* runnable, int num_total_tasks);
        TaskID runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                const std::vector<TaskID>& deps);
        void sync();
    private:
        void worker_loop();
        void complete_bulk_launch(TaskID launch_id);
        void complete_bulk_launch_unlocked(TaskID launch_id);

        int num_workers_;  // 与池大小相同，用于算 chunk
        std::vector<std::unique_ptr<BulkLaunch>> launches_;  // 下标即 TaskID
        std::vector<std::vector<TaskID>> successors_;  // successors_[d]：依赖 d 的后续 launch
        std::deque<TaskID> ready_queue_;  // unfinished_deps==0 可被执行的 launch id

        std::mutex graph_mtx_;  // 图 + ready_queue；complete_unlocked 假定已持此锁
        std::condition_variable cv_work_;
        std::atomic<bool> stop_;

        std::vector<std::thread> workers_;

        std::atomic<int> outstanding_tasks_;  // 全局未完成 runTask 数，sync 等待归零
        std::mutex sync_mtx_;
        std::condition_variable cv_sync_;
};

#endif
