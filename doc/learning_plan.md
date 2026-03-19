# CS149 Assignment 2 完整学习规划

> 面向具有 RDMA 驱动/协议背景、目标成为 AI Infra 高性能网络专家的学习者。
> 理论与实验双线并进，总周期约 12-14 天。

---

## 目标定位

你的当前技能栈：

```
RDMA 驱动/协议  ██████████  强
NCCL 基础       ████░░░░░░  中
GPU/CUDA 编程   ░░░░░░░░░░  待补
系统调度原理    ░░░░░░░░░░  待补（本课程核心）
```

完成本规划后的技能栈：

```
RDMA 驱动/协议  ██████████  强
NCCL 基础       ████████░░  强化
系统调度原理    ██████░░░░  掌握（理论 + 实现）
AI Infra 视角   ████░░░░░░  建立
```

市场定位：**AI 基础设施性能工程师（AI Infra Performance Engineer）**，2025-2028 年最稀缺岗位之一。

---

## 学习方法论

每个阶段遵循固定节奏：

```
理论预习（0.5天）→ 动手实验（1-2天）→ 验证 + 反思（0.5天）→ 进入下一阶段
```

理论决定设计决策质量。同样是线程池，理解 Work Stealing 的人和不理解的人写出来的代码，在 unequal workload 场景下性能差距可达 3x。

---

## 整体架构总览

```
Phase 0  理论：多核架构 + C++ 内存模型
         实验：tutorial.cpp + BlockingQueue
           ↓
Part A   Step 1  理论：Amdahl定律 + 任务分配策略
         Step 1  实验：TaskSystemParallelSpawn
           ↓
         Step 2  理论：Cache Coherence + False Sharing
         Step 2  实验：TaskSystemParallelThreadPoolSpinning
           ↓
         Step 3  理论：Memory Consistency + 尾延迟分析
         Step 3  实验：TaskSystemParallelThreadPoolSleeping  ← 重点
           ↓
Part B   理论：DAG 调度 + NCCL/Spark 对比
         实验：runAsyncWithDeps + sync
           ↓
         全量性能验收
```

---

## RDMA 心智映射表

> 建议打印贴在显示器旁边，每次实现前对照一遍。

| CS149 概念 | RDMA 对应物 | 说明 |
|---|---|---|
| Worker 线程死循环取任务 | RNIC 从 SQ 消费 WR | 执行者角色 |
| `atomic<int> fetch_add` 分配 task_id | SQ 的 producer/consumer 指针推进 | 无锁分配 |
| `notify_all()` 唤醒 Worker | Doorbell 写 RNIC 寄存器 | 通知机制 |
| `sync()` 主线程等待完成 | `ibv_poll_cq()` 轮询直到所有 WC | 完成同步 |
| `stop` flag + `notify_all()` 析构 | `ibv_destroy_qp()` 优雅关闭 | 生命周期管理 |
| Spinning 版的自旋等待 | RDMA Busy-Poll 模式 | 低延迟高 CPU |
| Sleeping 版的条件变量等待 | `ibv_get_cq_event()` 事件通知 | 低 CPU 高延迟 |
| Part B 的 BulkLaunch 依赖图 | RDMA SEND 的 Fence/Barrier 顺序 | 依赖约束 |

---

## Phase 0：热身（1-2 天）

### 理论预习（0.5 天）

**必读资源：**

- [tutorial/README.md](../tutorial/README.md) — 本项目自带，30 分钟读完
- CS149 Lecture: [A Modern Multi-Core Processor](https://gfxcourses.stanford.edu/cs149/fall25/lecture/multicore1/) — 重点：超线程、SIMD、内存带宽瓶颈

**核心理论问题（读完后能回答代表掌握）：**

1. 为什么 `std::atomic<int>` 的 `++` 是安全的，而普通 `int` 的 `++` 不是？
   - 答：普通 `++` 是 read-modify-write 三步操作，线程切换发生在中间会导致数据丢失；atomic 保证三步原子执行。

2. `std::condition_variable::wait()` 内部做了什么？为什么必须持有锁才能调用？
   - 答：`wait()` 原子地释放锁并将线程挂起，被唤醒时原子地重新持锁。若不持锁就调用，通知方可能在"释放锁"和"挂起"之间发出通知，导致通知丢失（lost wakeup）。

3. 什么是 spurious wakeup？C++ 标准为何允许它存在？
   - 答：线程在没有 `notify` 的情况下自行醒来。标准允许是因为某些操作系统实现（如 Linux futex）在信号处理等情况下无法避免，用 predicate 循环检查可以防御。

**C++ 内存模型关键点（本项目全程使用默认值即可）：**

- `memory_order_seq_cst`（默认）：最强保证，所有线程看到相同的全局操作顺序
- `memory_order_relaxed`：只保证原子性，不保证跨线程可见顺序——仅在性能优化阶段才考虑
- **结论**：本项目全程使用默认 `std::atomic<int>`，不需要手动指定 memory_order

### 实验操作（1 天）

**步骤 1：编译并运行 tutorial**

```bash
cd /home/yangxw/yxw/cs149/asst2/tutorial
make
./tutorial
```

期望输出：

```
==============================================================
Starting 8 threads to increment counter...
Final counter value: 80000...
==============================================================
==============================================================
Starting 3 threads for signal-and-waiting...
Lock re-acquired after wait()...
Lock re-acquired after wait()...
==============================================================
```

**步骤 2：改造 tutorial.cpp（理解 spurious wakeup 防护）**

将 [tutorial/tutorial.cpp](../tutorial/tutorial.cpp) 中的 `wait_fn` 从：

```cpp
thread_state->condition_variable_->wait(lk);  // 危险：无 predicate
```

改为：

```cpp
thread_state->condition_variable_->wait(lk, [thread_state]{
    return thread_state->counter_ < thread_state->num_waiting_threads_;
});
```

**步骤 3：徒手实现 BlockingQueue（不查资料）**

在不参考任何资料的情况下，实现以下接口：

```cpp
template<typename T>
class BlockingQueue {
    std::queue<T> q_;
    std::mutex mtx_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    int max_size_;
public:
    BlockingQueue(int max_size);
    void push(T val);   // 满时阻塞
    T pop();            // 空时阻塞
};
```

完成后对照 [tutorial/README.md](../tutorial/README.md) 检查自己的实现是否有 spurious wakeup 漏洞。

### 验证标准

- `./tutorial` 输出 `Final counter value: 80000` ✓
- 改造后的 `wait_fn` 不改变行为，仅增加安全性 ✓
- BlockingQueue 的 push/pop 能被多线程并发调用而不出现数据竞争 ✓

---

## Part A — Step 1：TaskSystemParallelSpawn（1-2 天）

**文件：** [part_a/tasksys.cpp](../part_a/tasksys.cpp)，[part_a/tasksys.h](../part_a/tasksys.h)

### 理论预习（0.5 天）

**必读资源：**

- CS149 Lecture: [Program Optimization 1: Work Distribution and Scheduling](https://gfxcourses.stanford.edu/cs149/fall25/lecture/perfopt1/)
- 重点：Amdahl 定律、静态分配 vs 动态分配

**Amdahl 定律的实践含义：**

```
理论加速比上限 = 1 / (串行比例 S + 并行比例/N)

本项目中"串行比例"包括：
  - 线程创建/销毁开销（ParallelSpawn 的致命伤）
  - 任务分配的锁竞争开销
  - 主线程等待时的开销

结论：当单个 task 执行时间 << 线程创建开销时，
      ParallelSpawn 可能比 Serial 更慢！
      
验证：运行 super_super_light 测试，观察 Always Spawn 版
      是否比 Serial 版反而更快？（应该更快，因为任务多）
      而 super_light 中 Spawn 版比 Serial 慢（任务轻，线程创建占主导）
```

**静态分配 vs 动态分配：**

| 策略 | 实现方式 | 适合场景 | 风险 |
|---|---|---|---|
| 静态分配 | 线程 i 处理 `[i*chunk, (i+1)*chunk)` | 任务耗时均匀 | 不均匀时负载失衡 |
| 动态分配 | `atomic<int>` 全局计数器，Worker 抢占 | 任务耗时不均（推荐）| 轻微原子操作开销 |

**结论：本项目使用动态分配，用 `std::atomic<int>` 实现 task_id 抢占。**

### 实验操作（1-2 天）

**需要修改的文件：**

- `part_a/tasksys.h`：添加 `int num_threads_` 成员
- `part_a/tasksys.cpp`：实现 `TaskSystemParallelSpawn::run()`

**核心实现逻辑（伪代码）：**

```
run(runnable, num_total_tasks):
    atomic<int> next_id = 0
    
    spawn num_threads_ 个线程，每个线程执行：
        while true:
            id = next_id.fetch_add(1)
            if id >= num_total_tasks: break
            runnable->runTask(id, num_total_tasks)
    
    join 所有线程   ← 同步语义的保证
```

**设计思考题（动手前先想清楚）：**

1. `num_threads_` 应该在构造函数中初始化还是 `run()` 中使用？
2. 如果 `num_total_tasks < num_threads_`，多余的线程会怎么样？代码是否正确处理了这种情况？

### 验证标准

```bash
cd part_a && make

# 正确性验证（必须 PASS）
./runtasks -n 8 simple_test_sync

# 性能验证
./runtasks -n 8 mandelbrot_chunked       # 计算密集，应有明显加速
./runtasks -n 8 super_super_light        # 轻量任务，观察 Spawn 版表现

# 用 harness 对比
python3 ../tests/run_test_harness.py -n 8 -t mandelbrot_chunked ping_pong_equal
```

**预期现象：**

- `mandelbrot_chunked`：单次 bulk launch + 计算密集，Spawn 版性能接近 ThreadPool 版
- `super_light`：大量 bulk launch + 轻量任务，Spawn 版因线程创建开销可能比 Serial 慢

这正是 Amdahl 定律的体现：**线程创建开销成为串行瓶颈**。

---

## Part A — Step 2：TaskSystemParallelThreadPoolSpinning（2 天）

**文件：** [part_a/tasksys.cpp](../part_a/tasksys.cpp)，[part_a/tasksys.h](../part_a/tasksys.h)

### 理论预习（0.5 天）

**必读资源：**

- CS149 Lecture: [Cache Coherence](https://gfxcourses.stanford.edu/cs149/fall25/lecture/cachecoherence/) — 重点：MSI/MESI 协议、False Sharing

**线程池设计的核心权衡：**

线程池解决了 ParallelSpawn 的线程创建开销，但引入了新问题：**Worker 如何知道何时有新任务？**

```
方案 A：Worker 检查 runnable 指针是否非 null
  风险：多个 Worker 可能同时进入同一批任务，next_task_id 需要重置时机复杂

方案 B：Worker 用 fetch_add 抢占 next_task_id，检查是否越界
  正确：每个 task_id 只被一个 Worker 取到（原子保证）
  这是正确方案
```

**Cache Coherence 的影响：**

```
atomic next_task_id 每次 fetch_add 触发 cache invalidation：
  核0 写 → 通知核1、核2...核15 各自的 cache 失效
  → 16 个 Worker 并发时，这个变量在核间频繁"弹跳"
  → 这是 Spinning 版的隐藏开销

False Sharing：
  若不同线程的局部变量在同一 cache line（64 bytes）内，
  写一个变量会导致整个 cache line 失效，影响无关变量的访问性能
  预防：关键共享变量之间用 alignas(64) 对齐隔离
```

### 实验操作（1-2 天）

**需要在 `tasksys.h` 中添加的成员变量：**

```cpp
class TaskSystemParallelThreadPoolSpinning: public ITaskSystem {
    std::vector<std::thread> workers_;
    IRunnable* current_runnable_;
    int num_total_tasks_;
    std::atomic<int> next_task_id_;
    std::atomic<int> tasks_done_;
    bool stop_;
    int num_threads_;
    // ...
};
```

**Worker 主循环设计（Spinning 版允许忙等）：**

```
Worker 主循环：
  while (!stop_):
      id = next_task_id_.fetch_add(1)
      if id < num_total_tasks_:
          current_runnable_->runTask(id, num_total_tasks_)
          tasks_done_.fetch_add(1)
      // else: 继续自旋（Spinning 版不 sleep）

主线程 run() 的同步等待：
  // 设置任务元数据
  current_runnable_ = runnable
  num_total_tasks_  = num_total_tasks
  next_task_id_     = 0
  tasks_done_       = 0
  
  // 自旋等待（Spinning 版允许）
  while (tasks_done_.load() < num_total_tasks_) {}
```

**析构时的正确顺序：**

```cpp
~TaskSystemParallelThreadPoolSpinning() {
    stop_ = true;                    // 1. 先设置 stop 标志
    for (auto& t : workers_)         // 2. 再 join 所有线程
        t.join();
}
```

**设计思考题：**

1. 如果两次 `run()` 调用之间，Worker 还在处理上一批任务，怎么办？
2. `stop_` 用普通 `bool` 还是 `atomic<bool>`？为什么？

### 验证标准

```bash
./runtasks -n 8 simple_test_sync
./runtasks -n 8 recursive_fibonacci          # 计算密集，线程池明显优于 Spawn
./runtasks -n 8 ping_pong_equal

python3 ../tests/run_test_harness.py -n 8
# 目标：ThreadPoolSpinning 在重型任务上明显快于 AlwaysSpawn
```

---

## Part A — Step 3：TaskSystemParallelThreadPoolSleeping（2-3 天）

**这是 Part A 最难、得分最高的部分（20分）。**

**文件：** [part_a/tasksys.cpp](../part_a/tasksys.cpp)，[part_a/tasksys.h](../part_a/tasksys.h)

### 理论预习（0.5 天）

**必读资源：**

- CS149 Lecture: [Implementing Synchronization + Memory Consistency](https://gfxcourses.stanford.edu/cs149/fall25/lecture/sync_consistency/)
- 重点：acquire/release 语义、为什么条件变量内部需要持锁

**尾延迟（Tail Latency）分析：**

```
Spinning 版的长尾来源：
  ① 主线程自旋等待，占用一个核，挤压 Worker 的调度时间片
  ② 16 个 Worker 同时 fetch_add，atomic 总线争用成为热点
  ③ 轻量任务（super_light）中，任务执行时间 << 调度开销

Sleeping 版如何解决：
  ① 主线程用 cv_done_.wait() 睡眠，彻底释放 CPU 给 Worker
  ② Worker 无任务时用 cv_worker_.wait() 睡眠，不抢占 CPU
  ③ 代价：notify/wakeup 有内核系统调用开销（~1-10 μs）

结论：
  轻量任务（执行时间 < 10μs）→ Sleeping 版优于 Spinning 版
  重量级任务（执行时间 >> 10μs）→ 两者性能接近
  这就是 super_super_light 测试中 Sleeping 版得分最高的原因
```

**条件变量正确使用模式（必须掌握）：**

```cpp
// ===== 等待方 =====
{
    std::unique_lock<std::mutex> lk(mtx_);
    cv.wait(lk, [&]{ return condition; });  // predicate 防止 spurious wakeup
    // wait 返回时：持有 lk + condition 为真
    // 在持锁状态下读取/修改共享变量
}

// ===== 通知方 =====
{
    std::lock_guard<std::mutex> lk(mtx_);
    // 修改共享变量（使 condition 变为真）
}
cv.notify_all();  // 在锁外 notify 性能略好，但在锁内也完全正确
```

**为什么 `wait()` 必须带 predicate？**

```
没有 predicate 的问题（spurious wakeup）：
  Worker 线程在没有 notify 的情况下被 OS 唤醒
  → 检查队列，发现没有任务，应该继续睡眠
  → 但没有 predicate 保护，Worker 直接继续执行了错误代码

有 predicate 的正确行为：
  cv.wait(lk, pred) 等价于：
  while (!pred()) { cv.wait(lk); }
  → 即使 spurious wakeup，predicate 为 false 则继续睡眠
```

### 实验操作（2 天）

**线程间通信时序：**

```
主线程 run()：
  1. 持锁，设置任务元数据（runnable、total、done=0、next_id=0）
  2. cv_worker_.notify_all()  ← 唤醒所有睡眠中的 Worker
  3. cv_done_.wait(lk, [&]{ return tasks_done_ == num_total_tasks_; })
  4. run() 返回

Worker 主循环：
  1. cv_worker_.wait(lk, [&]{ return next_task_id_ < num_total_tasks_ || stop_; })
  2. 若 stop_：退出循环
  3. 取 id = next_task_id_++，释放锁
  4. 执行 runTask(id, total)
  5. 持锁，++tasks_done_
  6. if tasks_done_ == num_total_tasks_: cv_done_.notify_one()
  7. 回到步骤 1
```

**需要在 `tasksys.h` 中添加的成员变量：**

```cpp
class TaskSystemParallelThreadPoolSleeping: public ITaskSystem {
    std::vector<std::thread> workers_;
    std::mutex mtx_;
    std::condition_variable cv_worker_;   // Worker 等待新任务
    std::condition_variable cv_done_;     // 主线程等待任务完成
    IRunnable* current_runnable_;
    int num_total_tasks_;
    int next_task_id_;
    int tasks_done_;
    bool stop_;
    int num_threads_;
};
```

**三个最易出错的陷阱：**

**陷阱 1：析构顺序**

```cpp
// 错误顺序（死锁风险）：
for (auto& t : workers_) t.join();  // Worker 还在 wait，永远不会退出
stop_ = true;

// 正确顺序：
{
    std::lock_guard<std::mutex> lk(mtx_);
    stop_ = true;           // 1. 设置 stop 标志（在锁内）
}
cv_worker_.notify_all();    // 2. 唤醒所有 Worker
for (auto& t : workers_)    // 3. 等待所有 Worker 退出
    t.join();
```

**陷阱 2：裸 wait 导致 spurious wakeup**

```cpp
// 错误：
cv_worker_.wait(lk);  // Worker 可能在没有任务时被误唤醒

// 正确：
cv_worker_.wait(lk, [&]{
    return next_task_id_ < num_total_tasks_ || stop_;
});
```

**陷阱 3：主线程 run() 的通知时机**

```cpp
// 错误：先 notify，再 wait
// 如果 Worker 在 notify 之前就全部完成了任务，cv_done_ 的通知会丢失
cv_worker_.notify_all();
cv_done_.wait(lk, [&]{ return tasks_done_ == num_total_tasks_; });

// 正确：wait 自带 predicate 检查，即使通知先于 wait 也不会错过
// （因为 wait 在 predicate 为 true 时不会睡眠）
```

### 验证标准

```bash
./runtasks -n 8 simple_test_sync
./runtasks -n 8 super_super_light        # Sleep版应明显优于Spin版
./runtasks -n 8 ping_pong_unequal        # 负载不均，动态分配 + Sleep 最优

python3 ../tests/run_test_harness.py -n 8
# 目标：[Parallel + Thread Pool + Sleep] 所有测试 PERF <= 1.2
```

---

## Part B：任务图调度（4-5 天）

**文件：** [part_b/tasksys.cpp](../part_b/tasksys.cpp)，[part_b/tasksys.h](../part_b/tasksys.h)

### 理论预习（1 天）

**必读资源：**

- CS149 Lecture: [Program Optimization 1](https://gfxcourses.stanford.edu/cs149/fall25/lecture/perfopt1/) — Producer-Consumer 模型部分
- [README.md](../README.md) 中 Part B 部分，重点理解 DAG 示例图

**DAG 调度理论：**

DAG（有向无环图）调度是分布式计算的核心问题，你已知的系统都在用它：

| 系统 | 节点含义 | 边的含义 | 调度策略 |
|---|---|---|---|
| CS149 Part B | Bulk Task Launch | 数据依赖 | 拓扑排序 + 工作抢占 |
| NCCL AllReduce | Ring Step | 前序 Step 完成 | 固定拓扑，线性顺序 |
| Apache Spark | RDD Transformation | Lineage 依赖 | Stage 划分 + 懒执行 |
| PyTorch Autograd | Tensor Operation | 梯度流 | 反向传播顺序 |
| RDMA SEND 序列 | Work Request | Fence/Barrier | 硬件顺序保证 |

**关键洞察：**

NCCL 的 Ring AllReduce 是**固定拓扑**的 DAG，每个 Step 依赖前一 Step（线性链）。Part B 要处理**任意拓扑**的 DAG（Fan-in、Fan-out、二叉树）。理解这个区别，你对 NCCL 的理解会更深一层。

**调度的核心不变式：**

```
一个 BulkLaunch 可以进入 ready_queue 当且仅当：
  它所有依赖的 BulkLaunch 都已经完全执行完毕

触发依赖检查的时机：
  某个 BulkLaunch 的最后一个 task 完成时（tasks_remaining 减到 0）
  → 遍历它的所有后继节点（dependents）
  → 对每个后继节点，从其 unmet_deps 集合中移除当前 launch_id
  → 若某后继的 unmet_deps 变为空，将其展开进 ready_queue
  → notify_all() 唤醒 Worker
```

**Part B 的测试场景分析：**

```
simple_test_async：
  launchA(4 tasks) ──────────────────→ 验证基础异步语义

math_operations_fan_in：
  launchA(64 tasks)
  launchB(64 tasks)   →（依赖所有上面的）→ reduce(1 task)
  ...
  launchN(64 tasks)
  验证：reduce 必须在所有 launchX 完成后才能开始

math_operations_reduction_tree：
         A     B     C     D
          \   /       \   /
           AB           CD
             \         /
               ABCD
  验证：二叉树结构，每层依赖上一层的两个节点
```

### 实验操作（3-4 天）

**数据流设计：**

```
调用 runAsyncWithDeps(r, n, deps)：
  1. 创建 BulkLaunch 记录，分配唯一 TaskID
  2. 若 deps 为空 → 直接展开到 ready_queue（每个 task_id 入队一个 item）
  3. 若 deps 非空 → 放入 waiting_map，登记依赖关系

Worker 完成一个 task：
  1. launch.tasks_remaining--
  2. 若变为 0：
     a. 将此 launch_id 加入 completed_set
     b. 遍历 launch.dependents，从各后继的 unmet_deps 移除此 id
     c. 若某后继的 unmet_deps 为空 → 展开到 ready_queue
     d. notify_all()

调用 sync()：
  等待所有已提交的 BulkLaunch 的 tasks_remaining 全部为 0
```

**需要在 `tasksys.h` 中添加的数据结构：**

```cpp
struct BulkLaunch {
    TaskID id;
    IRunnable* runnable;
    int num_total_tasks;
    int tasks_remaining;                  // 在 mtx_ 保护下访问
    std::set<TaskID> unmet_deps;          // 未完成的依赖 launch id
    std::vector<TaskID> dependents;       // 依赖我的后继 launch id
};

class TaskSystemParallelThreadPoolSleeping: public ITaskSystem {
    // ... Part A 的所有成员 ...
    std::map<TaskID, BulkLaunch*> waiting_map_;   // 有未满足依赖的 launch
    std::queue<std::pair<BulkLaunch*, int>> ready_queue_; // (launch, task_id)
    std::set<TaskID> all_launch_ids_;             // 所有已提交的 id
    std::set<TaskID> completed_launch_ids_;       // 已完成的 id
    int next_launch_id_;
};
```

**实现顺序建议：**

1. 先实现 `runAsyncWithDeps()` 的注册逻辑（不管依赖，全部放 waiting_map）
2. 实现 `sync()`（等待 completed == all）
3. 用 `runAsyncWithDeps() + sync()` 实现 `run()`
4. 实现无依赖的直接展开（deps 为空则直接进 ready_queue）
5. 实现依赖触发逻辑（tasks_remaining 归零时检查后继）
6. 运行 `simple_test_async` 验证，再运行 fan_in、tree 测试

### 验证标准

```bash
cd part_b && make

# 逐步验证（由简到难）
./runtasks -n 8 simple_test_async                # 第一步，基础异步
./runtasks -n 8 math_operations_in_tight_loop_async  # 串行依赖链
./runtasks -n 8 math_operations_fan_in           # Fan-in
./runtasks -n 8 math_operations_reduction_tree   # 二叉树依赖
./runtasks -n 8 spin_between_run_calls_async     # 混合轻重任务

python3 ../tests/run_test_harness.py -n 8 -a
# 目标：[Parallel + Thread Pool + Sleep] 所有 async test PERF <= 1.5
```

---

## 总体时间表

| 阶段 | 理论重点 | 实验内容 | 时长 | 验收命令 |
|---|---|---|---|---|
| Phase 0 | 多核架构、C++ 内存模型、并发原语 | tutorial + BlockingQueue | 1-2 天 | `./tutorial` 输出正确 |
| Part A Step 1 | Amdahl 定律、动态任务分配 | TaskSystemParallelSpawn | 1-2 天 | `simple_test_sync` PASS |
| Part A Step 2 | Cache Coherence、False Sharing | TaskSystemParallelThreadPoolSpinning | 2 天 | harness 重型任务性能提升 |
| Part A Step 3 | Memory Consistency、尾延迟 | TaskSystemParallelThreadPoolSleeping | 2-3 天 | harness PERF <= 1.2 |
| Part B | DAG 调度、NCCL/Spark 对比 | runAsyncWithDeps + sync | 4-5 天 | async harness PERF <= 1.5 |
| **总计** | | | **约 12-14 天** | |

---

## 终极验收标准

### Part A 全量验收

```bash
cd part_a
python3 ../tests/run_test_harness.py -n 8

# 期望输出格式：
# [Parallel + Thread Pool + Sleep]  super_super_light   PERF <= 1.2  (OK)
# [Parallel + Thread Pool + Sleep]  super_light         PERF <= 1.2  (OK)
# [Parallel + Thread Pool + Sleep]  ping_pong_equal     PERF <= 1.2  (OK)
# [Parallel + Thread Pool + Sleep]  ping_pong_unequal   PERF <= 1.2  (OK)
# [Parallel + Thread Pool + Sleep]  mandelbrot_chunked  PERF <= 1.2  (OK)
```

### Part B 全量验收

```bash
cd part_b
python3 ../tests/run_test_harness.py -n 8 -a

# 期望输出格式：
# [Parallel + Thread Pool + Sleep]  所有 async 测试  PERF <= 1.5  (OK)
```

**通过标准：** 两个 harness 中，`[Parallel + Thread Pool + Sleep]` 行所有测试均显示 `(OK)`。

---

## 参考资源索引

| 资源 | 链接 | 用途 |
|---|---|---|
| 本项目 tutorial | [tutorial/README.md](../tutorial/README.md) | C++ 并发 API 入门 |
| CS149 多核架构 | https://gfxcourses.stanford.edu/cs149/fall25/lecture/multicore1/ | Phase 0 理论 |
| CS149 调度优化 | https://gfxcourses.stanford.edu/cs149/fall25/lecture/perfopt1/ | Step 1 理论 |
| CS149 Cache Coherence | https://gfxcourses.stanford.edu/cs149/fall25/lecture/cachecoherence/ | Step 2 理论 |
| CS149 同步与一致性 | https://gfxcourses.stanford.edu/cs149/fall25/lecture/sync_consistency/ | Step 3 理论 |
| cppreference condition_variable | https://en.cppreference.com/w/cpp/thread/condition_variable | 条件变量 API |
| cppreference atomic | https://en.cppreference.com/w/cpp/atomic/atomic | 原子变量 API |
