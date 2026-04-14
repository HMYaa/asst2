#include "tasksys.h"
#include <algorithm>
#include <cassert>

/*
 * 单次 bulk launch 的运行时状态（Part B）。
 *
 * 字段职责一览：
 *   next_task     — worker 抢号游标（fetch_add）；解决「哪个 task 由我执行」
 *   remaining     — 按 copy（不是按 task）倒计时，触发 complete_bulk_launch。
 *                   初值 = min(num_tasks, num_workers)，即 pre-distribution 推入的副本数。
 *                   每个 worker 在其 copy 超号时减 1；减到 0 说明所有 copy 都已完成。
 *
 *   为什么按 copy 而非按 task？
 *     按 task：64 个 task × 16 核并发 fetch_sub(acq_rel) → cache line bounce ≈ 64×100ns = 6μs/run。
 *     按 copy：16 个 copy × fetch_sub(acq_rel)（超号时发生，并非每任务）→ cache line bounce
 *             ≈ 16×100ns = 1.6μs/run；对 super_light 等小批量测试减少 4× 原子竞争。
 *
 *   unfinished_deps — 减到 0 进入 ready_queue_；解决「我可以开始了吗」
 *   done          — complete_bulk_launch_unlocked 完成传播后置 true；
 *                   解决「0-task 批次 remaining 恒为 0，但传播尚未发生」的正确性问题。
 */
struct BulkLaunch {
    IRunnable*        runnable  = nullptr;
    int               num_tasks = 0;
    std::atomic<int>  next_task{0};
    std::atomic<int>  remaining{0};
    std::atomic<int>  unfinished_deps{0};
    std::atomic<bool> done{false}; // complete_bulk_launch_unlocked 完成传播后置 true

    BulkLaunch(IRunnable* r, int n, int remaining_init)
        : runnable(r), num_tasks(n), next_task(0), remaining(remaining_init),
          unfinished_deps(0), done(false) {}
};

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
    for (int i = 0; i < num_total_tasks; i++) {
        runnable->runTask(i, num_total_tasks);
    }

    return 0;
}

void TaskSystemSerial::sync() {
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
    // NOTE: CS149 students are not expected to implement TaskSystemParallelSpawn in Part B.
}

TaskSystemParallelSpawn::~TaskSystemParallelSpawn() {}

void TaskSystemParallelSpawn::run(IRunnable* runnable, int num_total_tasks) {
    // NOTE: CS149 students are not expected to implement TaskSystemParallelSpawn in Part B.
    for (int i = 0; i < num_total_tasks; i++) {
        runnable->runTask(i, num_total_tasks);
    }
}

TaskID TaskSystemParallelSpawn::runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                                 const std::vector<TaskID>& deps) {
    for (int i = 0; i < num_total_tasks; i++) {
        runnable->runTask(i, num_total_tasks);
    }

    return 0;
}

void TaskSystemParallelSpawn::sync() {
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

TaskSystemParallelThreadPoolSpinning::TaskSystemParallelThreadPoolSpinning(int num_threads): ITaskSystem(num_threads) {
}

TaskSystemParallelThreadPoolSpinning::~TaskSystemParallelThreadPoolSpinning() {}

void TaskSystemParallelThreadPoolSpinning::run(IRunnable* runnable, int num_total_tasks) {
    for (int i = 0; i < num_total_tasks; i++) {
        runnable->runTask(i, num_total_tasks);
    }
}

TaskID TaskSystemParallelThreadPoolSpinning::runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                                              const std::vector<TaskID>& deps) {
    for (int i = 0; i < num_total_tasks; i++) {
        runnable->runTask(i, num_total_tasks);
    }

    return 0;
}

void TaskSystemParallelThreadPoolSpinning::sync() {
    return;
}

/*
 * ================================================================
 * Parallel Thread Pool Sleeping — Part B
 * ================================================================
 *
 * 生命周期与并发边界（改代码时先对照这里）：
 *
 * 1) runAsyncWithDeps
 *    - 在 graph_mtx_ 内：追加 launches_[id]、连边 successors_[dep].push_back(id)、
 *      写 unfinished_deps、outstanding_tasks += n、可能 push ready。
 *    - 必须尽快释放 graph_mtx_，再在锁外 notify cv_work_；否则 worker 无法 pop ready。
 *    - 若已持 graph_mtx_ 且需唤醒后继，只能调 complete_bulk_launch_unlocked，禁止套娃加锁。
 *
 * 2) worker_loop
 *    - pop 一个 launch id 后，在锁外对该 BulkLaunch 做 fetch_add chunk + runTask。
 *    - remaining 从 1 减到 0 时表示「这一 bulk 最后一个 task」刚跑完，调 complete_bulk_launch。
 *    - 每个 task 结束都要 outstanding_tasks--；到 0 时 notify cv_sync_（供 sync()）。
 *
 * 3) num_tasks == 0
 *    - 不增加 outstanding_tasks；无依赖时直接在注册临界区内 complete_unlocked。
 *    - 有依赖时等前驱完成后被加入 ready，worker 弹出后走 n==0 分支再 complete_bulk_launch。
 *
 * 4) 多 worker 并行参与同一 batch 的策略（已改进）：
 *    worker 弹出 lid 后，若剩余任务多于一个 chunk，立即把 lid 推回队列并 notify_one（级联唤醒）。
 *    超号时（start >= n）直接 break，不再 push_back + notify_all，彻底消除「热旋转」：
 *    所有合法 task 已被领走，执行它们的 worker 会在 remaining 归零时调 complete_bulk_launch 通知后继。
 */

// 返回实现名，用于测试框架打印与区分不同调度器。
const char* TaskSystemParallelThreadPoolSleeping::name() {
    return "Parallel + Thread Pool + Sleep";
}

// 构造线程池：只创建 worker，不预建任务图节点。
// 任务图（launches_/successors_）按 runAsyncWithDeps 调用增量扩展，
// 这样初始化成本固定，避免启动阶段不必要的内存开销。
TaskSystemParallelThreadPoolSleeping::TaskSystemParallelThreadPoolSleeping(int num_threads)
    : ITaskSystem(num_threads),
      num_workers_(num_threads),
      stop_(false),
      outstanding_tasks_(0) {
    for (int i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

// 析构流程：
// 1) 置 stop_，让 worker 的 wait 谓词可退出；
// 2) 广播唤醒 cv_work_/cv_sync_，避免线程永久阻塞；
// 3) join 所有 worker，确保对象销毁前无并发访问成员。
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
        if (w.joinable()) {
            w.join();
        }
    }
}

// 假定已持有 graph_mtx_：
//   1. 将本节点标记为 done（供后续注册的批次判断「前驱是否已完成传播」）。
//   2. 遍历 successors_[launch_id]，对每个后继 unfinished_deps--，到 0 则入 ready_queue_。
//   3. 只在确实有新节点入队时才 notify_all，避免无谓的 thundering herd。
// 该函数是 DAG 推进的核心“完成事件处理器”：一个 launch 完成后，
// 在这里把“完成信号”传播给所有后继 launch。
void TaskSystemParallelThreadPoolSleeping::complete_bulk_launch_unlocked(TaskID launch_id) {
    // 先标记 done，再传播。两步均在 graph_mtx_ 保护下，与 runAsyncWithDeps 的 done 检查串行。
    launches_[static_cast<size_t>(launch_id)]->done.store(true, std::memory_order_release);

    if (launch_id >= static_cast<TaskID>(successors_.size())) {
        return;
    }

    bool any_ready = false;
    for (TaskID succ : successors_[launch_id]) {
        BulkLaunch* s = launches_[static_cast<size_t>(succ)].get();
        // fetch_sub 返回减前的值；为 1 表示减后变 0，该后继所有前驱都已满足
        if (s->unfinished_deps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // 预分发（pre-distribution）：推入 min(n, num_workers_) 份 lid，
            // 让每个 worker 独立持有一份直接执行，无需 cascade pre-push。
            // n=0 的 batch 推 1 份（worker 在 n==0 分支处理后立刻 complete）。
            const int succ_n = s->num_tasks;
            const int copies = (succ_n > 0) ? std::min(succ_n, num_workers_) : 1;
            for (int i = 0; i < copies; ++i) {
                ready_queue_.push_back(succ);
                ready_count_.fetch_add(1, std::memory_order_release);
            }
            any_ready = true;
        }
    }
    // 仅在真正有新 launch 进入就绪队列时唤醒 worker，避免无实质工作的 notify_all
    if (any_ready) {
        cv_work_.notify_all();
    }

    // outstanding_tasks_ 按批次计数（不是按任务），此处完成 1 个批次，减 1。
    //
    // 为什么移到这里而非 per-task 内层循环？
    //   原方案：每个 runTask 后 outstanding_tasks_.fetch_sub(acq_rel)，
    //     64 tasks × 16 核并发 = 64 次高竞争原子写，cache line bounce ≈ 6μs/run。
    //   新方案：完整 batch 完成时减 1 次，16 并发批次最多 16 次减；
    //     对 super_light（400 × 64 tasks）减少 63 × 400 = 25200 次 RMW，节约 ~2.5ms。
    //
    // 为什么必须在 complete_bulk_launch_unlocked（持 graph_mtx_）中做这个 fetch_sub？
    //   complete_bulk_launch 保证在整个 batch 完成之后才调用（remaining==0 或 n==0）；
    //   outstanding_tasks_ 在 runAsyncWithDeps 时加 1（per-batch），在这里减 1，
    //   减到 0 说明所有已提交批次都完成，sync() 可以安全返回。
    if (outstanding_tasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> slk(sync_mtx_);
        cv_sync_.notify_all();
    }
}

// complete_bulk_launch_unlocked 的加锁包装器。
// 用于调用方尚未持有 graph_mtx_ 的场景，统一入口避免锁语义被误用。
void TaskSystemParallelThreadPoolSleeping::complete_bulk_launch(TaskID launch_id) {
    std::lock_guard<std::mutex> lk(graph_mtx_);
    complete_bulk_launch_unlocked(launch_id);
}

// worker 主循环：
// - 在 cv_work_ 上等待 ready_queue_ 非空或 stop_；
// - 取出一个就绪 launch，按 chunk 抢号执行 runTask；
// - 该 launch 最后一个 task 完成时触发 complete_bulk_launch；
// - 每个 task 完成后递减 outstanding_tasks_，为 sync() 提供全局完成条件。
void TaskSystemParallelThreadPoolSleeping::worker_loop() {
    while (true) {
        TaskID lid = -1;

        // ══════════════════════════════════════════════════════════════════
        // 阶段 1：无锁自旋（spin-before-sleep，检查 ready_count_ 原子变量）
        //
        // 目的：消除连续小批量 run() 调用的 OS 睡眠-唤醒往返。
        //   传统睡眠-唤醒往返 ~50μs/run；自旋阶段直接在 ~1μs 内拾取工作。
        //
        // 为什么用 ready_count_ 而非持锁检查 ready_queue_？
        //   持锁自旋 = 16 个线程轮流占锁检查，彼此阻塞，等价于串行化自旋；
        //   ready_count_ 是纯原子读（acquire），16 线程并行无竞争，开销约 1ns/次。
        //
        // 为什么这里不再用 try_lock？
        //   旧设计（单份 lid + pre-push cascade）中，pre-distribution 引入前：
        //     16 worker 抢 1 份 lid → 只有 1 份有效工作，15 人无效拿锁 → thundering herd。
        //     try_lock 通过"失败即 yield 退避"自然错开竞争，降低惊群。
        //   新设计（pre-distribution 推入 min(n,workers) 份 lid）：
        //     每个 worker 都有自己的一份 lid，blocking lock 后立即 pop 成功，
        //     无无效竞争。16 个 blocking pop 各需 ~100ns，总计 ~1.6μs，
        //     远快于 try_lock + yield（每次 yield ~1μs，16 次 = ~16μs）。
        static constexpr int SPIN_LIMIT = 500;
        for (int spin = 0; spin < SPIN_LIMIT; ++spin) {
            if (stop_.load(std::memory_order_acquire)) return;
            if (ready_count_.load(std::memory_order_acquire) > 0) break;
            std::this_thread::yield();
        }
        if (stop_.load(std::memory_order_acquire)) return;
        // ══════════════════════════════════════════════════════════════════

        // 阶段 2：阻塞 pop（自旋结束后统一走此路径）
        //
        // 情况 A（自旋中看到 ready_count_ > 0 后）：阻塞 lock，pop 自己的那份 lid，直接执行。
        //   16 个 blocking pop 串行化开销 ≈ 16 × ~100ns = ~1.6μs（比 try_lock+yield 快 10×）。
        //   pre-distribution 保证每个 worker 都能找到自己的 copy，几乎不出现"lock 后队列空"的情况。
        //
        // 情况 B（spin 超时，ready_count_ 始终为 0）：cv.wait 真正睡眠，等待 notify。
        //
        // 为什么用 unique_lock 而不是 lock_guard？
        //   cv.wait() 在挂起时必须原子地"释放锁 + 进入等待"，只有 unique_lock
        //   满足 cv.wait() 对"可手动 unlock"的接口要求。
        //
        // 为什么 wait 必须带谓词 lambda？
        //   OS 存在伪唤醒（spurious wakeup），裸 wait 不检查条件会访问空队列导致 UB。
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

        // 为什么在锁外访问 launches_[lid]？
        //   launches_ 只在 runAsyncWithDeps 中 emplace_back（受 graph_mtx_ 保护）。
        //   一旦拿到 lid，BulkLaunch 通过 unique_ptr 存活在堆上，地址永不失效；
        //   BulkLaunch 内部的原子字段不需要持图锁。
        BulkLaunch& L = *launches_[static_cast<size_t>(lid)];
        const int n = L.num_tasks;

        // 零任务 launch：不跑 runTask，但需触发「本节点完成」以推进依赖图
        if (n == 0) {
            complete_bulk_launch(lid);
            continue;
        }

        // chunk = 1：每个 fetch_add 只领一个任务，实现完全动态负载均衡。
        //
        // 为什么从 N/workers（上限 32）改为 1？
        //   chunk=4 时，worker 0 领 tasks 0-3（重），worker 15 领 tasks 60-63（轻），
        //   重任务 worker 成为尾延迟瓶颈——ping_pong_unequal 的核心失分原因。
        //
        // chunk=1 的额外 fetch_add 开销可忽略：
        //   设每次 fetch_add 约 50ns，64 任务 × 400 runs = 64×400×50ns ≈ 1.3ms；
        //   相比 super_light 总时间 ~30ms，开销仅约 4%，被负载均衡收益抵消。
        //   对 mandelbrot / recursive_fibonacci 等重计算测试，每任务耗时远超 fetch_add，
        //   开销更低至 0.1% 以下。
        //
        // 与 pre-distribution 配合：pre-distribution 保证了 min(n, workers) 个 worker
        //   同时参与，chunk=1 保证了任务粒度最细，两者共同实现最优并行度与负载均衡。
        // chunk=1：每次 fetch_add 领一个任务，最大化动态负载均衡（ping_pong_unequal 等重不均衡场景）。
        // 额外 fetch_add 开销约 50ns/task，对 super_light(64 tasks) 约 3.2μs/run，可忽略。

        // 执行循环：领完任务后超号，在超号时（每 copy 一次）递减 remaining。
        //
        // 新方案（per-copy）vs 旧方案（per-task）：
        //   旧方案：每个 runTask 后 remaining.fetch_sub(acq_rel)。
        //     64 tasks × 16 核并发 = 64 次高竞争 acq_rel RMW → cache bounce ≈ 6μs/run。
        //   新方案：在超号时减 1（每个 copy 只减一次）：
        //     16 copy × fetch_sub → cache bounce ≈ 1.6μs/run，减少 4×。
        //
        // outstanding_tasks_ 已移至 complete_bulk_launch_unlocked（batch 级 1 次），
        //   不再在此处递减。
        while (true) {
            const int start = L.next_task.fetch_add(1, std::memory_order_relaxed);
            if (start >= n) {
                // 此 copy 超号：该 worker 已无更多任务可领。
                // remaining 初值 = min(n, workers)（即 pre-distribution 推入的副本数）；
                // acq_rel 确保：acquire 侧能看到所有先完成 copy 的写入；
                //              release 侧保证我的 runTask 结果在 remaining=0 可见前对外可见。
                if (L.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    // 最后一个超号：所有任务已被认领并执行，触发 DAG 传播 + outstanding 递减。
                    complete_bulk_launch(lid);
                }
                break;
            }
            L.runnable->runTask(start, n);
            // 不递减 remaining（超号时统一处理）；不递减 outstanding_tasks_（batch 完成时统一处理）。
        }
    }
}

// 同步接口 run()：按作业语义复用异步路径，
// 先注册无依赖 launch，再通过 sync() 阻塞等待全部完成。
void TaskSystemParallelThreadPoolSleeping::run(IRunnable* runnable, int num_total_tasks) {
    std::vector<TaskID> no_deps;
    runAsyncWithDeps(runnable, num_total_tasks, no_deps);
    sync();
}

// 异步提交一个 bulk launch，并注册其依赖边：
// - 分配新 TaskID 与 BulkLaunch 节点；
// - 对未完成前驱建立 successors_ 反向边；
// - 设置 unfinished_deps，若为 0 则进入 ready_queue_；
// - 维护 outstanding_tasks_ 供 sync() 判断全局收敛。
// 该函数必须“快返回”，不能等待任务执行完成。
TaskID TaskSystemParallelThreadPoolSleeping::runAsyncWithDeps(IRunnable* runnable, int num_total_tasks,
                                                              const std::vector<TaskID>& deps) {
    TaskID id = 0;
    bool schedule_now = false;

    {
        // ① 为什么这里的锁范围要覆盖「建节点 + 建边 + 入队」全部操作，不能拆开？
        //    launches_、successors_、ready_queue_ 三者必须保持一致性快照：
        //    如果建完节点立即释放锁，worker 可能在 successors_ 还未初始化时
        //    就从 ready_queue_ 拿到该 launch 并开始执行，访问未初始化的 unfinished_deps。
        //    一次性加锁使"注册"成为原子事务，避免中间状态被其他线程观察到。
        std::lock_guard<std::mutex> lk(graph_mtx_);

        // remaining_init = min(n, workers)：按 copy 计数（见 BulkLaunch 注释）。
        // n=0 时 remaining_init=0，因为 0-task 批次的完成由 complete_bulk_launch_unlocked 直接触发。
        const int remaining_init = (num_total_tasks > 0)
                                   ? std::min(num_total_tasks, num_workers_) : 0;
        launches_.emplace_back(new BulkLaunch(runnable, num_total_tasks, remaining_init));
        id = static_cast<TaskID>(launches_.size() - 1);
        // ② 为什么 successors_ 要 resize 而不是在构造时一次性分配？
        //    任务图是动态增量构建的，调用方可以无限次调用 runAsyncWithDeps，
        //    预先分配固定大小会浪费内存或限制上限。随 launches_ 增长同步 resize
        //    是"按需分配"的最小内存策略。
        successors_.resize(launches_.size());

        // ③ 为什么用 dep->done 而不是 dep->remaining > 0 判断前驱是否活跃？
        //    错误方案「remaining > 0」：
        //      - 对 N=0 的批次，remaining 构造即为 0，此条件永远为 false，
        //        导致后继总把 0-task 前驱视为「已完成」，跳过建边，
        //        后继在前驱的 complete 传播前就进入 ready_queue，破坏依赖序。
        //    正确方案「!dep->done」：
        //      - done 由 complete_bulk_launch_unlocked 在 DAG 传播完成后置 true。
        //      - 对 N=0 的批次：done 在入队时由 runAsyncWithDeps 内部立即调用
        //        complete_bulk_launch_unlocked 置 true，与 N>0 的路径语义一致。
        //      - done 的 store 和本处的 load 均在 graph_mtx_ 保护下，无需额外 acquire。
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
        // ④ 为什么先写 unfinished_deps（release），再向 ready_queue_ push？
        //    发布协议（publication protocol）：必须先完成"对象初始化"，
        //    再"发布"给消费者。若先 push 到 ready_queue_，worker 可能立即
        //    拿到该 launch 并读到 unfinished_deps 的旧值（0 或垃圾），
        //    错误地认为所有依赖都未满足从而阻塞，或者直接开始执行。
        me->unfinished_deps.store(unresolved, std::memory_order_release);

        // ⑤ outstanding_tasks_ 改为按批次计数（每次 runAsyncWithDeps 加 1，不再是加 n）。
        //
        // 为什么从 per-task 改为 per-batch？
        //   旧方案：fetch_add(n)，worker 内层循环每完成 1 个 task 后 fetch_sub(1)。
        //     64 tasks × 16 核并发 = 64 次高竞争 acq_rel 写 → cache bounce 约 6μs/run。
        //   新方案：fetch_add(1)，complete_bulk_launch_unlocked 完成整批时 fetch_sub(1)。
        //     每批次仅 1 次原子写，完全消除 per-task 竞争。
        //
        //   语义等价：sync() 等待 outstanding_tasks_==0 仍正确表示"所有已提交批次全部完成"；
        //   只是粒度从"任务"换成了"批次"——对 sync() 的语义而言无区别。
        //
        // 为什么仍用 relaxed？原因同旧方案：精确的 0 事件由 fetch_sub(acq_rel) 保证，
        //   这里只是"登记存在"，不需要额外的 happens-before。
        outstanding_tasks_.fetch_add(1, std::memory_order_relaxed);

        if (num_total_tasks == 0) {
            if (unresolved == 0) {
                // ⑥ 为什么 0-task 且无依赖时，在锁内直接调 complete_bulk_launch_unlocked？
                //    0-task launch 没有 runTask 执行，不会有 worker 来调用 complete_bulk_launch。
                //    必须在注册时立即传播完成信号，否则后继 launch 的 unfinished_deps 永远不会
                //    被减到 0，形成死锁。在锁内调用（_unlocked 版本）避免递归加锁。
                complete_bulk_launch_unlocked(id);
            }
            // unresolved > 0：等待前驱 complete 时传播，不需要进 ready_queue
        } else if (unresolved == 0) {
            // 预分发（pre-distribution）：推入 min(n, num_workers_) 份 lid。
            // 每个 worker 持有独立一份，直接通过 fetch_add 抢任务号执行，
            // 替代原来的 pre-push cascade（cascade 每次需额外 graph_mtx_ + notify_one，
            // 对小批量任务的连续调用场景中每次 run() 增加 ~16μs 锁竞争）。
            const int copies = std::min(num_total_tasks, num_workers_);
            for (int i = 0; i < copies; ++i) {
                ready_queue_.push_back(id);
                ready_count_.fetch_add(1, std::memory_order_release);
            }
            schedule_now = (copies > 0);
        }
    }

    // ⑦ 为什么 notify 必须在锁外，不能在锁内？
    //    worker 的 cv_work_.wait(lk, ...) 被唤醒后要立即竞争 graph_mtx_。
    //    如果在持锁时发 notify，worker 醒来后立刻阻塞在锁上，造成"空唤醒-锁竞争"
    //    的无效往返。先释放锁再 notify，worker 醒来后能直接拿锁执行，延迟更低。
    //
    // ⑧ 为什么这里用 notify_all 而不是 notify_one？
    //    一次 runAsyncWithDeps 可能推入的是一个大 batch（n 很大），
    //    应该让尽可能多的 worker 立即参与。notify_one 会限制初始并行度，
    //    后续靠预推级联扩散稍慢。与预推的 notify_one 不同：预推的目标是
    //    "已在运行中的 batch 吸引更多 worker"，而这里是"新 batch 首次入队"，
    //    语义不同，粒度也不同。
    if (schedule_now) {
        cv_work_.notify_all();
    }

    return id;
}

// 全局同步屏障：等待“所有已提交但未完成的 task 数”归零。
// 语义上等价于等待当前系统中所有 launch 的执行收敛。
void TaskSystemParallelThreadPoolSleeping::sync() {
    static constexpr int SYNC_SPIN_LIMIT = 2000;
    for (int i = 0; i < SYNC_SPIN_LIMIT; i++) {
        if (outstanding_tasks_.load(std::memory_order_acquire) == 0) return;
        std::this_thread::yield();
    }
    std::unique_lock<std::mutex> lk(sync_mtx_);
    cv_sync_.wait(lk, [this] {
        return outstanding_tasks_.load(std::memory_order_acquire) == 0;
    });
}
