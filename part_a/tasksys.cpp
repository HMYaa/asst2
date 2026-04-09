#include "tasksys.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <iostream>

// For CS149 we only need the x86 "pause" instruction used in spin-wait loops.
// Using inline asm avoids clangd/x86 intrinsic header diagnostics on this setup.
static inline void cs149_pause() {
    __asm__ __volatile__("pause");
}

IRunnable::~IRunnable() {}

ITaskSystem::ITaskSystem(int num_threads) {}
ITaskSystem::~ITaskSystem() {}

/*
 * ================================================================
 * Serial task system implementation
 * ================================================================
 */

const char* TaskSystemSerial::name() {
    return "Serial";
}

TaskSystemSerial::TaskSystemSerial(int num_threads): ITaskSystem(num_threads) {
}

TaskSystemSerial::~TaskSystemSerial() {}

void TaskSystemSerial::run(IRunnable* runnable, int num_total_tasks) {
    for (int i = 0; i < num_total_tasks; i++) {
        runnable->runTask(i, num_total_tasks);
    }
}

TaskID TaskSystemSerial::runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                          const std::vector<TaskID>& deps) {
    // You do not need to implement this method.
    return 0;
}

void TaskSystemSerial::sync() {
    // You do not need to implement this method.
    return;
}

/*
 * ================================================================
 * Parallel Task System Implementation
 * ================================================================
 */

const char* TaskSystemParallelSpawn::name() {
    return "Parallel + Always Spawn";
}

TaskSystemParallelSpawn::TaskSystemParallelSpawn(int num_threads): ITaskSystem(num_threads) {
    //
    // TODO: CS149 student implementations may decide to perform setup
    // operations (such as thread pool construction) here.
    // Implementations are free to add new class member variables
    // (requiring changes to tasksys.h).
    //
    num_threads_ = num_threads;
    worker_threads_ = std::vector<std::thread>();
}

TaskSystemParallelSpawn::~TaskSystemParallelSpawn() {}

void TaskSystemParallelSpawn::run(IRunnable* runnable, int num_total_tasks) {


    //
    // TODO: CS149 students will modify the implementation of this
    // method in Part A.  The implementation provided below runs all
    // tasks sequentially on the calling thread.
    //
    if (num_total_tasks <= 0) {
        return;
    }

    int min_threads = std::min(num_threads_, num_total_tasks);
    next_task_id_.store(0);

    worker_threads_.clear();
    worker_threads_.reserve(min_threads);

    for(int i = 0; i < min_threads; i++ ) {
        worker_threads_.emplace_back([this, runnable, num_total_tasks]() {
            while(true) {
                // printf("next_task_id_: %d\n", next_task_id_.load());
                int task_id = next_task_id_.fetch_add(1);
                if (task_id >= num_total_tasks) break;
                runnable->runTask(task_id, num_total_tasks);
            }
        });
    }

    for (auto &thread : worker_threads_) {
        thread.join();
    }

}

TaskID TaskSystemParallelSpawn::runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                                 const std::vector<TaskID>& deps) {
    // You do not need to implement this method.
    return 0;
}

void TaskSystemParallelSpawn::sync() {
    // You do not need to implement this method.
    return;
}

/*
 * ================================================================
 * Parallel Thread Pool Spinning Task System Implementation
 * ================================================================
 */

const char* TaskSystemParallelThreadPoolSpinning::name() {
    return "Parallel + Thread Pool + Spin";
}

TaskSystemParallelThreadPoolSpinning::TaskSystemParallelThreadPoolSpinning(int num_threads): 
    ITaskSystem(num_threads),
    stop(false),next_task_id_(0),num_total_tasks_(0),
    remaining_tasks(0),batch_active(false), current_runnable(nullptr), 
    workers() {
    //
    // TODO: CS149 student implementations may decide to perform setup
    // operations (such as thread pool construction) here.
    // Implementations are free to add new class member variables
    // (requiring changes to tasksys.h).
    //
 
    for (size_t i = 0; i < num_threads; i ++) {
        workers.emplace_back([this]{
            while(true) {
                if (stop.load(std::memory_order_acquire)) {
                    break;
                }
                // 如果当前没有任务，则继续自旋
                if (!batch_active.load(std::memory_order_acquire)) {
                    cs149_pause();
                    continue;
                }
// 瓶颈：全局原子访问次数约降为 1/k，对 super_light/ping_pong 很有效。        
                int task_id = next_task_id_.fetch_add(1);
/*
-  优化思路是“减少全局抢号频率”
先给结论（按收益/改动比排序）
第一优先：Chunk 化领取任务（强烈推荐）
不是每次 fetch_add(1)，改成每次 fetch_add(k)，一个线程一次领一段 [start, start+k) 再本地循环执行。
这样全局原子访问次数约降为 1/k，对 super_light/ping_pong 很有效。

第二优先：主线程参与执行（而不是纯等待）
run() 里主线程别只 pause，也参与“抢 chunk 干活”。
这能补回一部分吞吐（尤其轻任务和短批次）。

第三优先：自适应 chunk 大小
固定 k=8/16/32 先试；更进一步可按 num_total_tasks / (num_threads * c) 动态算。
太小没效果，太大又会负载不均（特别是 ping_pong_unequal）。
*/

                // 如果任务id小于总任务数，则执行任务
                if (task_id < num_total_tasks_) {
                    current_runnable->runTask(task_id, num_total_tasks_);
                    // 优雅的结束，确保所有任务都执行完毕
                    if (remaining_tasks.fetch_sub(1) == 1) {
                        batch_active.store(false, std::memory_order_release);
                    }
                } else {
                    // 如果任务id大于等于总任务数，则继续自旋
                    cs149_pause();
                }
            }    
        });
    }
}

TaskSystemParallelThreadPoolSpinning::~TaskSystemParallelThreadPoolSpinning() {
    stop.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

// void TaskSystemParallelThreadPoolSpinning::enqueue(std::function<void()> task) { 
//     while (spin_lock.test_and_set(std::memory_order_acquire)) {
//         _mm_pause();
//     }
    
//     tasks.push(std::move(task));
//     spin_lock.clear(std::memory_order_release);
// }

void TaskSystemParallelThreadPoolSpinning::run(IRunnable* runnable, int num_total_tasks) {

    //std::cout << "batch start, num_total_tasks: " << num_total_tasks << std::endl;
    //
    // TODO: CS149 students will modify the implementation of this
    // method in Part A.  The implementation provided below runs all
    // tasks sequentially on the calling thread.
    //
    if (num_total_tasks <= 0) {
        return;
    }

    current_runnable = runnable;
    num_total_tasks_ = num_total_tasks;
    next_task_id_.store(0, std::memory_order_release);
    remaining_tasks.store(num_total_tasks, std::memory_order_release);
    batch_active.store(true, std::memory_order_release);
    // 风险：主线程只等不干活
    while (batch_active.load(std::memory_order_acquire)) {
        cs149_pause();
    }
    // // 优化：主线程参与执行（而不是纯等待）
    // while (batch_active.load(std::memory_order_acquire)) {
    //     int task_id = next_task_id_.fetch_add(1);
    //     if (task_id < num_total_tasks_) {
    //         current_runnable->runTask(task_id, num_total_tasks_);
    //         if (remaining_tasks.fetch_sub(1) == 1) {
    //             batch_active.store(false, std::memory_order_release);
    //         }
    //     } else {
    //         cs149_pause();
    //     }
    // }
}

TaskID TaskSystemParallelThreadPoolSpinning::runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                                              const std::vector<TaskID>& deps) {
    // You do not need to implement this method.
    return 0;
}

void TaskSystemParallelThreadPoolSpinning::sync() {
    // You do not need to implement this method.
    return;
}

/*
 * ================================================================
 * Parallel Thread Pool Sleeping Task System Implementation
 * ================================================================
 */

const char* TaskSystemParallelThreadPoolSleeping::name() {
    return "Parallel + Thread Pool + Sleep";
}

TaskSystemParallelThreadPoolSleeping::TaskSystemParallelThreadPoolSleeping(int num_threads): ITaskSystem(num_threads)
, workers(),current_runnable(nullptr),next_task_id_(0),
num_total_tasks_(0),remaining_tasks(0),batch_active(false), stop(false), mtx(), cv_work(), cv_done() {
    //
    // TODO: CS149 student implementations may decide to perform setup
    // operations (such as thread pool construction) here.
    // Implementations are free to add new class member variables
    // (requiring changes to tasksys.h).
    //

    for (size_t i = 0; i < num_threads; i ++) {
        workers.emplace_back([this]{
            while(true) {
                IRunnable* runnable = nullptr;
                int total = 0;
                int task_id = -1;
                    // 1) 锁内：等待 + 领任务
                {
                    std::unique_lock<std::mutex> lk(mtx);
        
                    cv_work.wait(lk, [this]{ 
                            return stop.load(std::memory_order_acquire) 
                                || (batch_active.load(std::memory_order_acquire) 
                                && next_task_id_.load(std::memory_order_acquire) < num_total_tasks_); });
                    
                    if (stop.load(std::memory_order_acquire)) 
                        break;

                    task_id = next_task_id_.fetch_add(1, std::memory_order_relaxed);
                    if (task_id >= num_total_tasks_) {
                        continue;// 伪唤醒/被别人抢完，回去等
                    }
                    runnable = current_runnable;
                    total = num_total_tasks_;
                }// 这里自动解锁
                
                // 2) 锁外：执行任务
                runnable->runTask(task_id, total);
                
                // 3) 锁内：更新计数 + 完成通知
                {
                    std::unique_lock<std::mutex> lk(mtx);
                    int left = remaining_tasks.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    if (left == 0) {
                        batch_active.store(false, std::memory_order_release);
                        cv_done.notify_one();
                    }
                }
            }
        });
    }
}

TaskSystemParallelThreadPoolSleeping::~TaskSystemParallelThreadPoolSleeping() {
    //
    // TODO: CS149 student implementations may decide to perform cleanup
    // operations (such as thread pool shutdown construction) here.
    // Implementations are free to add new class member variables
    // (requiring changes to tasksys.h).
    //
    stop.store(true, std::memory_order_release);
    cv_work.notify_all();

    for (auto &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void TaskSystemParallelThreadPoolSleeping::run(IRunnable* runnable, int num_total_tasks) {


    //
    // TODO: CS149 students will modify the implementation of this
    // method in Parts A and B.  The implementation provided below runs all
    // tasks sequentially on the calling thread.
    //
    {
        std::lock_guard<std::mutex> lk(mtx);
        current_runnable = runnable;
        num_total_tasks_ = num_total_tasks;
        next_task_id_.store(0, std::memory_order_release);
        remaining_tasks.store(num_total_tasks, std::memory_order_release);
        batch_active.store(true, std::memory_order_release);
        if (num_total_tasks <= 0) {
            batch_active.store(false, std::memory_order_release);
            current_runnable = nullptr;
            num_total_tasks_ = 0;
            return;
        }
    }
    cv_work.notify_all();
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv_done.wait(lk, [this]{ return remaining_tasks.load(std::memory_order_acquire) == 0; });

    }
}

TaskID TaskSystemParallelThreadPoolSleeping::runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                                    const std::vector<TaskID>& deps) {


    //
    // TODO: CS149 students will implement this method in Part B.
    //

    return 0;
}

void TaskSystemParallelThreadPoolSleeping::sync() {

    //
    // TODO: CS149 students will modify the implementation of this method in Part B.
    //

    return;
}
