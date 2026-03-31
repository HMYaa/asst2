#include "tasksys.h"
#include <atomic>
#include <emmintrin.h> // 必须包含这个头文件以使用 _mm_pause()
#include <condition_variable>
#include <mutex>
#include <iostream>

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

                if (!batch_active.load(std::memory_order_acquire)) {
                    _mm_pause();
                    continue;
                }
                
                int task_id = next_task_id_.fetch_add(1);

                if (task_id < num_total_tasks_) {
                    current_runnable->runTask(task_id, num_total_tasks_);

                    if (remaining_tasks.fetch_sub(1) == 1) {
                        batch_active.store(false, std::memory_order_release);
                    }
                } else {
                    _mm_pause();
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
    std::condition_variable done_cv;
    std::mutex done_mtx;

    current_runnable = runnable;
    num_total_tasks_ = num_total_tasks;
    next_task_id_.store(0, std::memory_order_release);
    remaining_tasks.store(num_total_tasks, std::memory_order_release);
    batch_active.store(true, std::memory_order_release);

    while (batch_active.load(std::memory_order_acquire)) {
        _mm_pause();
    }
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

TaskSystemParallelThreadPoolSleeping::TaskSystemParallelThreadPoolSleeping(int num_threads): ITaskSystem(num_threads) {
    //
    // TODO: CS149 student implementations may decide to perform setup
    // operations (such as thread pool construction) here.
    // Implementations are free to add new class member variables
    // (requiring changes to tasksys.h).
    //
}

TaskSystemParallelThreadPoolSleeping::~TaskSystemParallelThreadPoolSleeping() {
    //
    // TODO: CS149 student implementations may decide to perform cleanup
    // operations (such as thread pool shutdown construction) here.
    // Implementations are free to add new class member variables
    // (requiring changes to tasksys.h).
    //
}

void TaskSystemParallelThreadPoolSleeping::run(IRunnable* runnable, int num_total_tasks) {


    //
    // TODO: CS149 students will modify the implementation of this
    // method in Parts A and B.  The implementation provided below runs all
    // tasks sequentially on the calling thread.
    //

    for (int i = 0; i < num_total_tasks; i++) {
        runnable->runTask(i, num_total_tasks);
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
