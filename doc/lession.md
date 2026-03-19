# CS149 Lab 2 相关理论课程梳理

课程主页：https://gfxcourses.stanford.edu/cs149/fall25
视频资源：Stanford CS149 2023 版本 YouTube 频道（Fall 2025 视频不对外公开）

---

## 一、章节总览：Lab 2 涉及哪几节课

Lab 2 的实现横跨多核并行调度的多个层面，对应课程中 **6 节核心讲座**，按重要程度分为三档：

| 优先级 | 日期 | 课程标题 | 对应 Lab 2 的哪个部分 |
|--------|------|----------|----------------------|
| ★★★ 必看 | Oct 07 | Program Optimization 1: Work Distribution and Scheduling | Part A 全部：任务分配策略核心 |
| ★★★ 必看 | Nov 13 | Implementing Synchronization + Memory Consistency | Part A Step 3：条件变量睡眠实现 |
| ★★☆ 重要 | Nov 11 | Cache Coherence | Part A Step 2-3：性能调优 |
| ★★☆ 重要 | Sep 25 | A Modern Multi-Core Processor (Part I) | Phase 0 热身：硬件基础 |
| ★☆☆ 推荐 | Oct 09 | Program Optimization 2: Locality and Communication | Part A Step 2：Contention 分析 |
| ★☆☆ 推荐 | Oct 02 | Parallelizing Code: An Example Thought Process | Part B：任务依赖图的思维模型 |

**额外扩展（不影响做题，但对你的职业方向有价值）：**

| 日期 | 课程标题 | 价值说明 |
|------|----------|---------|
| Oct 30 | Mapping AI Applications to the Datacenter Computer | 把单机 Task Scheduler 映射到 AI 数据中心调度，与 NCCL 直接相关 |
| Nov 20 | Fine-Grained Locking and Lock-Free Programming | Lock-Free Queue 是 Task Scheduler 的终极优化方向 |

---

## 二、各章节详细内容与推荐学习时长

---

### 章节 1：A Modern Multi-Core Processor (Part I)

**日期：** Sep 25
**链接：** https://gfxcourses.stanford.edu/cs149/fall25/lecture/multicore1/
**对应 Lab 2：** Phase 0 热身 / 理解所有 Part 的硬件背景
**推荐时长：** 1.5 小时（完整看，不跳）

**核心内容：**

这节课建立整个课程的硬件基础。Lab 2 的所有优化本质上都是在利用或规避多核处理器的特性。

- **多核并行（Multi-core）**：为什么单核频率提升遇到瓶颈，转向多核。理解"8 核不等于 8 倍性能"的根本原因。
- **超线程（Hyperthreading / SMT）**：一个物理核可以运行 2 个硬件线程，这是为什么 AWS c7g.4xlarge 有 16 个 execution contexts 的原因。
- **SIMD（单指令多数据）**：和 Lab 2 关联较少，但这是 Lab 1 的核心，了解即可。
- **内存带宽 vs 计算带宽**：`super_light` 测试是内存密集型，`recursive_fibonacci` 是计算密集型，这节课帮你理解为什么两者的并行加速比不同。

**对你的 RDMA 背景的额外价值：**
这节课讲的 cache 层次结构（L1/L2/L3）与 RDMA 的 zero-copy 设计直接相关——RDMA 绕过内核和 CPU 缓存的设计，正是为了避免这节课描述的内存层次开销。

---

### 章节 2：Program Optimization 1: Work Distribution and Scheduling

**日期：** Oct 07
**链接：** https://gfxcourses.stanford.edu/cs149/fall25/lecture/perfopt1/
**对应 Lab 2：** Part A（全部三个实现）+ Part B 的任务调度策略
**推荐时长：** 2 小时（最重要的一节，细读 PPT，标注重点）

**核心内容：**

这是 Lab 2 最直接的理论来源，建议在开始写代码之前看完。

**① Amdahl 定律（Amdahl's Law）**

```
加速比 = 1 / (S + P/N)
S = 串行部分比例，P = 并行部分比例，N = 处理器数量

实践含义：
  Lab 2 中的"串行部分"包括：
    - 线程创建/销毁（ParallelSpawn 的瓶颈）
    - 全局 task_id 计数器的原子操作竞争
    - 主线程等待所有 Worker 完成的时间
  
  → 这解释了为什么 super_super_light 测试中，
    Always Spawn 版可能比 Serial 版更慢
```

**② 任务分配策略对比**

| 策略 | 机制 | 适合场景 | Lab 2 中的实现 |
|------|------|---------|----------------|
| 静态分配 | 预先按线程数均分 | 任务耗时均匀 | 不推荐 |
| 动态分配 | `atomic<int>` 抢占 task_id | 任务耗时不均 | 推荐方案 |
| Work Stealing | 空闲线程从忙碌线程偷任务 | 复杂不均衡场景 | 了解概念，不要求实现 |

**③ 调度开销（Scheduling Overhead）分析**

课程会量化分析：何时并行化是值得的，何时开销超过收益。这直接对应 Lab 2 不同测试用例的性能差异。

**学完这节课能解决的 Lab 2 问题：**
- 为什么要用 `fetch_add` 而不是预先分块？
- `ping_pong_unequal` 测试为什么需要动态分配才能拿到高分？
- `super_super_light` 中为什么线程池版本比 Spawn 版本快？

---

### 章节 3：Program Optimization 2: Locality and Communication

**日期：** Oct 09
**链接：** https://gfxcourses.stanford.edu/cs149/fall25/lecture/perfopt2/
**对应 Lab 2：** Part A Step 2（ThreadPoolSpinning）性能调优
**推荐时长：** 1 小时（重点看 Contention 和 False Sharing 相关部分）

**核心内容：**

**① 通信开销（Communication Overhead）**

多线程共享数据时，"通信"的代价并不为零。
- 共享的 `atomic<int> next_task_id_` 每次 `fetch_add` 都会在核间触发一次 cache invalidation
- 16 个 Worker 并发抢占同一个原子变量：这个变量会在 16 个核的 L1 cache 之间高速"弹跳"
- 结果：高并发轻量任务下，Spinning 版的原子操作开销反而成为瓶颈

**② 争用（Contention）**

```
多个线程同时竞争同一资源 → 实际变成串行执行 → 并行加速失效

在 Lab 2 中的表现：
  next_task_id_.fetch_add(1) 虽然是原子操作，
  但底层需要总线锁（Bus Lock）或 Cache Line 独占（MESI Exclusive 状态）
  → 16 线程并发时，这个操作排队串行化
```

**③ 与 RDMA 的关联（你的背景加分点）**

这节课讲的"避免 Contention"是 RDMA 高性能设计的核心原则之一：
- RDMA 每个 QP 有独立的 SQ/CQ，正是为了避免多核之间的 contention
- NCCL 为每对 GPU 维护独立的通信 channel，同理

**可以选择性观看的部分：** Message Passing 模型（MPI 相关）与 Lab 2 关系不大，可以快速跳过。

---

### 章节 4：Cache Coherence

**日期：** Nov 11
**链接：** https://gfxcourses.stanford.edu/cs149/fall25/lecture/cachecoherence/
**对应 Lab 2：** Part A Step 2-3 性能优化 / 理解 Spinning 版本的性能上限
**推荐时长：** 1.5 小时（重点：MSI 协议状态机 + False Sharing）

**核心内容：**

**① Cache Coherence 协议（MSI / MESI）**

```
MSI 状态机：
  Modified（M）：该核独占，数据已修改，其他核 cache 无效
  Shared  （S）：多核共享，只读状态
  Invalid （I）：本核 cache 中的数据已过期

在 Lab 2 中的行为：
  Worker A 执行 next_task_id_.fetch_add(1)
    → 将 next_task_id_ 的 cache line 置为 Modified
    → 其他 15 个 Worker 的 next_task_id_ cache line 变为 Invalid
    → 下一个 Worker 发起 fetch_add 时，必须从 RAM 或其他核的 L3 重新加载
    → 这就是"cache line bouncing"，是高并发 atomic 的隐藏代价
```

**② False Sharing（伪共享）—— Lab 2 最常见的性能 Bug**

```
场景：
  // 错误的设计
  int thread_done[16];  // 16 个线程各自的完成计数
  // thread_done[0] 和 thread_done[1] 在同一个 64-byte cache line 中！
  // 线程 0 更新 thread_done[0] → 导致线程 1 的 thread_done[1] cache 失效
  // 即使两个线程访问的是不同的内存地址！

修复方案：
  struct alignas(64) PaddedInt { int value; };
  PaddedInt thread_done[16];  // 每个 int 独占一个 cache line
```

**③ 对 RDMA 背景的加分价值**

Cache Coherence 是理解 RDMA Write 语义（`IBV_WR_RDMA_WRITE`）内存可见性的理论基础：
- RDMA Write 完成后，目标端的 CPU 看到数据，需要什么内存屏障？
- 这正是 Memory Consistency 问题，下一节课会讲。

---

### 章节 5：Implementing Synchronization + Memory Consistency

**日期：** Nov 13
**链接：** https://gfxcourses.stanford.edu/cs149/fall25/lecture/sync_consistency/
**对应 Lab 2：** Part A Step 3（ThreadPoolSleeping）核心实现 / 理解条件变量的底层保证
**推荐时长：** 2 小时（最难的理论章节，建议看两遍）

**核心内容：**

**① 锁的底层实现**

```
Test-and-Set / Compare-and-Swap（CAS）：
  lock():   while (test_and_set(&flag)) {}  // 自旋直到获得锁
  unlock(): flag = 0;

这正是 Spinning 版线程池的底层机制！
Sleeping 版的 std::mutex 则会在自旋失败后调用 syscall 让内核接管
```

**② 内存一致性模型（Memory Consistency）**

```
顺序一致性（Sequential Consistency，SC）：
  所有线程看到相同的全局操作顺序
  → C++ std::atomic 默认提供 SC（memory_order_seq_cst）

宽松一致性（Relaxed Consistency）：
  允许 CPU 对无依赖指令重排序以提高性能
  → 某些场景可用 memory_order_relaxed / memory_order_acquire/release

Lab 2 实践指导：
  全程使用默认的 atomic（SC），不要手动指定 memory_order
  除非你完全理解 acquire/release 语义，否则优化收益不值风险
```

**③ Acquire/Release 语义（理解条件变量为何有效）**

```
为什么 condition_variable::wait() 返回后，Worker 线程能看到主线程写的任务数据？

主线程写数据：
  {
    lock_guard<mutex> lk(mtx_);  // 这个 lock 包含一个 release fence
    runnable_ = new_task;         // 写操作在 release fence 之前，保证可见
    next_task_id_ = 0;
  }
  cv.notify_all();

Worker 线程读数据：
  unique_lock<mutex> lk(mtx_);
  cv.wait(lk, pred);             // 获取锁包含一个 acquire fence
  // 在 acquire fence 之后读取数据，保证看到主线程的写
  auto id = next_task_id_++;

acquire/release 形成了 happens-before 关系，这是正确性的保证。
```

**④ Condition Variable 的正确使用（Lab 2 最关键的代码模式）**

这节课会从理论角度解释为什么：
1. `wait()` 必须持锁
2. `wait()` 必须用 predicate 形式
3. `notify()` 可以在锁内也可以在锁外

**对 RDMA 背景的深度价值：**
这节课讲的 acquire/release 语义，正是 RDMA Write 后需要显式内存屏障的理论依据。RDMA `IBV_SEND_FENCE` 标志对应的就是 release fence 的概念。

---

### 章节 6：Parallelizing Code: An Example Thought Process

**日期：** Oct 02
**链接：** https://gfxcourses.stanford.edu/cs149/fall25/lecture/thoughtprocess/
**对应 Lab 2：** Part B（任务依赖图的设计思维）
**推荐时长：** 1 小时（重点看依赖分析和 DAG 构建部分）

**核心内容：**

**① 并行化的思维流程**

课程给出了一套系统的并行化分析方法：
1. 识别可并行的计算单元
2. 分析数据依赖（哪些操作必须顺序执行）
3. 将依赖关系表示为 DAG
4. 决定调度策略（拓扑排序执行 DAG）

这正是 Lab 2 Part B 的设计框架。

**② DAG 调度的核心不变式**

```
一个节点（BulkLaunch）可以开始执行，当且仅当：
  它所有的前驱节点（依赖的 Launch）都已经完成

对应 Part B 的实现：
  waiting_map: 存储有未满足依赖的 BulkLaunch
  ready_queue: 存储可立即执行的 task
  
  触发转移的事件：
    某个 BulkLaunch 的最后一个 task 完成
    → 遍历后继节点，尝试将满足条件的移入 ready_queue
```

**③ Fan-in / Fan-out / 二叉树拓扑**

课程会用具体例子分析不同 DAG 形状下的并行度（Parallelism Degree）。这直接对应 Lab 2 的三个测试：
- `math_operations_fan_in`：多个并行节点汇聚到一个 Reduce 节点
- `math_operations_reduction_tree`：二叉树结构，逐层聚合

---

## 三、建议学习顺序与时间安排

| 学习顺序 | 课程 | 时长 | 何时看 |
|---------|------|------|--------|
| 1 | Sep 25: Modern Multi-Core Processor | 1.5h | 开始 Phase 0 之前 |
| 2 | Oct 07: Work Distribution and Scheduling | 2h | 开始写 Part A 之前（必须） |
| 3 | Nov 13: Synchronization + Memory Consistency | 2h | 开始写 Step 3（Sleeping）之前（必须） |
| 4 | Nov 11: Cache Coherence | 1.5h | Step 2 写完后，性能调优时 |
| 5 | Oct 09: Locality and Communication | 1h | Step 2 写完后，按需 |
| 6 | Oct 02: Parallelizing Code | 1h | 开始写 Part B 之前 |
| **扩展** | Oct 30: AI Datacenter | 1h | 全部完成后，拓宽视野 |
| **扩展** | Nov 20: Lock-Free Programming | 1.5h | 对 Lock-Free Queue 优化感兴趣时 |

**总计核心理论学习时长：约 9 小时**
**总计（含扩展）：约 11.5 小时**
