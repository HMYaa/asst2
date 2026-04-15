# Task Scheduler 设计全景文档

> **作者视角**：20 年高性能并发实战，见过 RDMA 网卡驱动到 GC Pause 的全栈并发问题。  
> **目标读者**：已完成 Part A 实现，希望从"能跑"升级到"真正理解每一个设计决策"的开发者。

---

## 目录

1. [问题定义与约束](#1-问题定义与约束)
2. [四种实现的演进路径](#2-四种实现的演进路径)
3. [核心状态机：Sleeping 版本精解](#3-核心状态机sleeping-版本精解)
4. [并发正确性：七个关键决策点](#4-并发正确性七个关键决策点)
5. [性能工程：瓶颈识别与优化](#5-性能工程瓶颈识别与优化)
6. [潜在 Bug 清单（你的代码中实际存在的）](#6-潜在-bug-清单你的代码中实际存在的)
7. [内存序选择指南](#7-内存序选择指南)
8. [从 Part A 到 Part B 的架构演进](#8-从-part-a-到-part-b-的架构演进)

---

## 1. 问题定义与约束

### 1.1 核心契约

```
run(IRunnable* r, int N)：
  - 语义：执行 r->runTask(0..N-1)，所有任务完成后才返回
  - 约束：调用方（主线程）阻塞直到 100% 完成
  - 不变量：相邻两次 run() 调用之间不存在任务重叠
```

这个约束极其关键。它意味着：

- **不需要跨批次的并发保护**（每次 run() 结束时系统处于静止状态）
- 可以用"批次发布-等待完成"的简单双阶段协议，而不需要复杂的任务队列

### 1.2 工作负载分类（决定设计选择）


| 工作负载类型                | 特征            | 瓶颈           | 最适合的实现                     |
| --------------------- | ------------- | ------------ | -------------------------- |
| `super_super_light`   | 任务极轻(纳秒级)，批次多 | **调度开销**     | ThreadPoolSleeping（最低发布延迟） |
| `super_light`         | 任务轻，批次多       | fetch_add 争用 | Sleeping + Chunk           |
| `ping_pong_equal`     | 中等任务，均匀分布     | 负载均衡         | 任意 ThreadPool              |
| `ping_pong_unequal`   | 任务重量不等        | 负载不均         | 小 chunk + 动态窃取             |
| `recursive_fibonacci` | 任务极重          | 计算本身         | 任意均可                       |
| `mandelbrot_chunked`  | 单次大批次         | 并行度          | ThreadPool（线程复用不重要）        |


---

## 2. 四种实现的演进路径

### 2.1 Serial → Spawn：引入并行

```
Serial：      主线程串行 for(0..N)

Spawn：       主线程 spawn N 个 worker
              每个 worker 原子抢号 fetch_add(1) 执行
              主线程 join 所有 worker
```

**Spawn 的关键设计**：使用原子计数器 `next_task_id_` 做无锁工作窃取（Work Stealing 的最简形式）。

```
优点：实现简单、正确性易验证
缺点：每次 run() 的线程创建开销 ≈ 5-50μs（取决于 OS）
     对 super_light 这类"批次多但每批任务轻"的场景灾难性
```

**实际测量（典型值，Linux x86）**：

- `pthread_create` + `join` 开销：约 10-30μs/线程
- `super_super_light` 的单批任务耗时：< 1μs
- 结论：**调度开销 >> 计算时间，吞吐接近串行**

### 2.2 Spawn → ThreadPoolSpinning：消除线程创建开销

```
ThreadPoolSpinning：  构造时创建 N 个 worker，永久存活
                      worker 不断自旋检查 batch_active 标志
                      run() 设置任务参数后立即返回去等待
```

**核心状态机**：

```
Worker 自旋版：
  while(true):
    if stop → exit
    if !batch_active → pause(); continue   ← CPU 空转
    task_id = fetch_add(1)
    if task_id < N → execute; fetch_sub(remaining)
    else → pause()                          ← 也空转
```

**Spinning 的代价**：空闲时 N 个 worker 线程各消耗 100% 的一个逻辑核。  
在 16 核机器上空跑时，16 个线程 = 16 核空转。这正是需要 Sleeping 版本的原因。

### 2.3 ThreadPoolSpinning → ThreadPoolSleeping：消除空转

核心思想：用 `condition_variable` 替换自旋等待。

```
等待时：   系统调用 futex_wait → 线程挂起，OS 收回 CPU
唤醒时：   notify_all → futex_wake → 线程被调度回来
代价：     唤醒延迟 ≈ 5-15μs（系统调用 + 上下文切换 + 调度延迟）
收益：     空闲时 CPU 使用率接近 0%
```

---

## 3. 核心状态机：Sleeping 版本精解

你当前实现采用了"三阶段 Worker + 单锁"方案，是一个相当成熟的设计。

### 3.1 三阶段状态机图

```
                    ┌─────────────────────────────────────┐
                    │           Worker 状态机              │
                    └─────────────────────────────────────┘

  ┌──────┐  stop/batch_active   ┌──────┐  lk.unlock()   ┌──────┐
  │  A   │─────────────────────►│  B   │───────────────►│  B'  │
  │ 睡眠 │                      │ 就绪 │                 │ 执行 │
  └──────┘                      └──────┘                 └──────┘
     ▲                                                       │
     │  !batch_active             lk.lock()                  │
     │                         ┌──────┐◄─────────────────────┘
     └─────────────────────────│  C   │  号抢完，等批次真正结束
                               │ 等尾 │
                               └──────┘
```

### 3.2 Worker 线程代码设计（先看执行端）

下面给出与你当前实现等价的 Worker 设计骨架（强调业务逻辑，不是逐行贴源码）：

```cpp
worker_loop() {
    unique_lock<mutex> lk(mtx);
    while (!stop) {
        // A: 等待“有批次可执行”或“线程池停止”
        cv_work.wait(lk, [] { return stop || batch_active; });
        if (stop) break;

        // 读取本批次上下文（建议在持锁阶段取局部副本）
        IRunnable* local_runnable = current_runnable;
        int local_n = num_total_tasks_;

        // B: 锁外抢号 + 执行，降低锁竞争
        lk.unlock();
        while (true) {
            start = next_task_id_.fetch_add(chunk);
            if (start >= local_n) break;
            for (task_id in [start, min(start + chunk, local_n))) {
                local_runnable->runTask(task_id, local_n);
                if (remaining_tasks.fetch_sub(1) == 1) {
                    lock_guard<mutex> g(mtx);
                    batch_active = false;
                    cv_done.notify_one();   // 通知主线程 run() 返回
                    cv_work.notify_all();   // 通知 C 段等尾的 worker
                }
            }
        }

        // C: 号已抢完但本批次可能未结束，避免回 A 形成忙等
        lk.lock();
        cv_work.wait(lk, [] { return stop || !batch_active; });
    }
}
```

这段设计对应三个目标：

- **正确性目标**：run() 返回前，所有任务都完成；
- **性能目标**：B 段锁外执行，减少临界区；chunk 降低 `fetch_add` 热点；
- **能耗目标**：A/C 都睡眠等待，避免 spinning 空转。

#### 3.2.1 为什么“回 A”会忙等？（你问的 169-171）

先看 A 段谓词：

```cpp
cv_work.wait(lk, [] { return stop || batch_active; });
```

它的含义是：只要 `batch_active == true`，A 段就**不会睡眠**，而是立刻返回继续执行。

如果一个 worker 在 B 段发现“号抢完了（`start >= n`）”，但其他慢线程还在跑，系统状态是：

- `next_task_id_ >= n`（已经没有号可抢）
- `batch_active == true`（批次尚未真正结束）

这时如果它不去 C，直接回 A，会出现下面的空转环：

```text
回 A -> 谓词(stop || batch_active) 立刻为真 -> 立即返回
     -> 再进 B 抢号（start>=n） -> 再回 A
     -> ...重复...
```

这就是“忙等”：线程一直在 A/B 之间快速循环，持续占用 CPU，但没有任何有效任务执行。

C 段就是为了解这个问题：

```cpp
cv_work.wait(lk, [] { return stop || !batch_active; });
```

它强制这个“已抢空号”的 worker 睡到“本批次真正结束（`batch_active=false`）”再继续，从而切断 A/B 空转环。

### 3.3 run() 的发布协议（再看生产端）

```cpp
// 发布一批任务的正确顺序（你当前的实现）

// Step 1：持锁写入所有共享状态（原子发布）
{
    std::lock_guard<std::mutex> lk(mtx);
    current_runnable = runnable;        // 写 runnable 指针
    num_total_tasks_ = num_total_tasks; // 写任务总数
    next_task_id_.store(0);             // 重置任务计数器
    remaining_tasks.store(N);           // 重置剩余计数
    batch_active.store(true);           // 最后开门（发布屏障）
}

// Step 2：通知所有睡眠的 worker
cv_work.notify_all();

// Step 3：等待完成
{
    std::unique_lock<std::mutex> lk(mtx);
    cv_done.wait(lk, [this]{ return remaining_tasks == 0; });
}
```

**为什么 Step 1 要持锁？**

这是一个"发布协议"（Publication Protocol）问题。如果不持锁：

- Worker 在 A 阶段被唤醒，读到 `batch_active = true`
- 但此时 `current_runnable` 还没写完（race condition！）
- Worker 解引用野指针 → 未定义行为

持锁保证了"所有写操作在 notify 之前形成 happens-before 关系"。

### 3.4 从线程视角看一次完整批次（图 + 文字）

你可以把一次批次想成三类线程在接力：

1. **主线程（Producer）**：发布任务批次，然后阻塞等待 `cv_done`
2. **Worker-快（Fast Worker）**：很快抢完号，先进入 C 段等收尾
3. **Worker-慢（Slow Worker）**：仍在 B 段执行最后几个 task

一个典型时序如下（推荐按列读）：

```text
时间轴↓     主线程(run)                  快worker(F)                    慢worker(S)
-----------------------------------------------------------------------------------------
T0         发布批次:
           写 runnable/N/计数器
           batch_active=true
           notify_all(cv_work)  ----->    从A醒来，进入B抢号             从A醒来，进入B抢号
           wait(cv_done)

T1                                      很快抢到最后一个chunk后
                                        再抢号: start>=N
                                        进入C: wait(!batch_active)       仍在B执行runTask

T2                                                                         继续执行最后几个task

T3                                                                         完成“最后一个任务”
                                                                           remaining: 1->0
                                                                           batch_active=false
                          <------------------- notify_one(cv_done)         notify_all(cv_work) --->
           run() 被唤醒返回                   F 从C被唤醒，回到A等待下一批
-----------------------------------------------------------------------------------------
```

图里的关键点是两条通知分工：

- `notify_one(cv_done)`：只负责叫醒主线程，让 `run()` 返回；
- `notify_all(cv_work)`：负责叫醒 C 段“等尾”的 worker，让它们不要卡在旧批次尾部。

这就是你标记的 `L181`（`cv_work.notify_all()`）的业务意义：

- 它不是给“还在 B 段干活的线程”用的；
- 它是专门给“已经在 C 段等尾”的线程一个**批次结束广播**；
- 没有这句，C 段线程可能继续睡着，无法及时回到 A 段准备下一批。

### 3.5 "最后一个任务"的收尾协议

```cpp
// 在 B 阶段内层循环，每完成一个任务后：
if (remaining_tasks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    // 我是最后一个完成任务的 worker
    std::lock_guard<std::mutex> g(mtx);
    batch_active.store(false, std::memory_order_release);
    cv_done.notify_one();     // 唤醒 run()
    cv_work.notify_all();     // 唤醒睡在 C 段的其他 workers
}
```

**为什么 fetch_sub 用 `acq_rel`？**

- `acquire`：我看到的 remaining_tasks 值是最新的（所有先完成的任务可见）
- `release`：我的任务完成结果对后续观察者可见

**为什么这里还要持锁做 notify？**

不持锁可能导致"通知丢失"（lost wakeup）。
详见第 4 节。

---

## 4. 并发正确性：七个关键决策点

### 决策点 1：为什么 run() 的发布必须持锁？

**错误想法**：原子操作本身已经是线程安全的，不需要锁。

**反例**：

```
run() 线程:                          Worker A（刚从 C 唤醒）:
current_runnable = new_runnable
                                     读 batch_active = false（旧批）
next_task_id_.store(0)               ← Worker 进入 A 段 wait
remaining_tasks.store(N)
batch_active.store(true)             ← Worker 的 wait 谓词变 true，唤醒
notify_all()
                                     读 current_runnable = new_runnable ✓
                                     但如果 current_runnable 写在 batch_active 之后...
```

**正确理解**：锁提供的不只是"原子性"，更是**可见性顺序保证**。所有写在同一临界区完成，意味着 worker 持同一把锁检查时，要么全看到，要么全看不到。

### 决策点 2：C 阶段为什么不能直接回 A？

**错误想法**：fetch_add 超出 N 就代表本批完成了，直接回 A 等下批。

**反例**：

```
有 4 个任务，chunk=2，2 个 worker

Worker-1：fetch_add(2) → start=0，执行 task[0,1]
Worker-2：fetch_add(2) → start=2，执行 task[2,3]
Worker-3：fetch_add(2) → start=4 ≥ N，break 退出 B
          （此时 batch_active 仍为 true，Worker-1/2 还在跑）

Worker-3 直接回 A：cv_work.wait(lk, [] { return stop || batch_active; })
谓词 = (false || true) = true → 立即返回！
Worker-3 又进入 B，fetch_add(2) → start=6 ≥ N，又 break...
→ CPU 空转，完全失去 Sleep 意义
```

**C 段的本质**：是一个"尾等待"屏障，确保抢完号但任务还未全部完成的 worker 睡下，而不是空转。

### 决策点 3：cv_work.notify_all() 在收尾时为什么必须在持锁下调用？

**"通知丢失"竞争（Lost Wakeup Race）**：

```
时序 1（有 bug，不持锁通知）：

Worker-X（在 C 段）：            收尾 worker（在 B 段）：
  读 !batch_active = false         
  （准备睡眠，但还未 futex_wait）    batch_active = false
                                   cv_work.notify_all()  ← 通知在 wait 之前！
  进入 futex_wait                 
  ← 永远没有线程再通知它了！死锁！
```

**持锁通知的保证**：

```
时序 2（正确，持锁通知）：

Worker-X（在 C 段）：            收尾 worker（在 B 段）：
  尝试 lk.lock()（阻塞）            持锁：batch_active = false
                                   持锁：cv_work.notify_all()
                                   释放锁
  获得锁，进入 cv_work.wait()
  → 谓词 !batch_active = true → 立即返回 ✓
```

mutex 保证了"谓词检查"和"进入 futex_wait"是原子的。

### 决策点 4：cv_done 的等待条件为何是 `remaining_tasks == 0` 而不是 `!batch_active`？

看似等价，实则有细微差别：

```
// 方案 A（你当前的实现）：
cv_done.wait(lk, [this]{ return remaining_tasks == 0; });

// 方案 B（看起来也对）：
cv_done.wait(lk, [this]{ return !batch_active; });
```

**方案 B 的潜在问题**：如果构造函数结束后，在第一次 `run()` 之前，`batch_active` 已经是 false。此时方案 B 会立即返回，绕过等待。

方案 A 更稳健：`remaining_tasks == 0` 在初始化（构造函数）时也为 0，但 `run()` 在进入等待前会先 `store(N)`，所以不存在初始状态误判问题。

### 决策点 5：B 段的 fetch_add 为何用 `memory_order_relaxed`？

```cpp
const int start = next_task_id_.fetch_add(chunk, std::memory_order_relaxed);
```

**可以用 relaxed 的理由**：

- `next_task_id_` 只需要"每个 task_id 最多被一个线程拿到"
- 这个保证由 fetch_add 的原子性本身提供，不需要额外的内存顺序
- 读到的 start 值可能是"旧"的吗？不会——fetch_add 本身是原子 RMW

**注意**：fetch_sub 用了 `acq_rel`，是因为它需要确保"我完成的任务结果"对后续检查 remaining_tasks 的代码可见（release），同时也需要看到所有"之前完成的任务"（acquire，用于正确判断是否是最后一个）。

### 决策点 6：worker lambda 捕获 `num_threads` 而非用成员变量

```cpp
workers.emplace_back([this, num_threads] {
    // ...
    int chunk = (n + num_threads - 1) / num_threads;
```

**为什么不用 `this->num_threads_`？**

如果基类 `ITaskSystem` 没有存储 `num_threads_`，而子类成员变量在构造期间就捕获了 lambda，可以直接捕获值更安全。更重要的是：lambda 捕获值在闭包创建时固定，避免了假设"成员变量不会在生命周期内变化"的隐式依赖。

### 决策点 7：析构函数的正确顺序

```cpp
~TaskSystemParallelThreadPoolSleeping() {
    stop.store(true, std::memory_order_release);  // Step 1: 设置终止标志
    cv_work.notify_all();                          // Step 2: 唤醒所有睡眠的 worker
    for (auto& w : workers) {
        if (w.joinable()) w.join();               // Step 3: 等待所有 worker 退出
    }
}
```

**顺序不能颠倒**：

- 先 join 再 stop：所有 worker 还在睡眠，join 永远不会返回 → **死锁**
- 先 stop 再 notify 再 join：worker 唤醒后检查 stop 退出 ✓

---

## 5. 性能工程：瓶颈识别与优化

### 5.1 Chunk 化的收益与代价

**不使用 Chunk（fetch_add(1)）**：

- 每个任务：1 次 LOCK XADD（原子 RMW）≈ 40-100ns（L3 miss + 总线仲裁）
- 16 线程 × 1000 任务 = 16000 次全局原子操作
- `next_task_id`_ 所在缓存行在所有核心间来回 bounce

**使用 Chunk（fetch_add(k)）**：

- 每 k 个任务：1 次原子操作
- 全局原子次数降为 1/k
- 代价：chunk 末尾可能有空洞（最后一个 chunk 未被填满）

**Chunk 大小的选择（你当前的实现）**：

```cpp
int chunk = (n + num_threads - 1) / num_threads;
chunk = std::clamp(chunk, 1, 32);
```

这个策略的逻辑：

- 目标：每个线程恰好领 1 轮（理想的静态均分）
- 上限 32：防止 ping_pong_unequal 负载不均（有些任务重达 100x，大 chunk 会让一个线程独自背负）
- 实际效果：对均匀任务很好，对不均匀任务需要调小

**更精细的自适应策略**（工业级做法）：

```
轻任务（< 1μs）：     chunk = min(32, N/threads)  → 减少原子争用
中等任务（1-100μs）： chunk = 4..16               → 均衡开销与不均
重任务（> 1ms）：      chunk = 1                   → 最大动态均衡
```

### 5.2 主线程参与执行 vs 纯等待

你代码中有注释掉的"主线程参与执行"版本，这是一个经典权衡：

**纯等待（你当前激活的版本）**：

```
缺点：主线程睡眠在 cv_done，浪费一个逻辑核
优点：唤醒路径简单，无状态竞争
```

**主线程参与执行**（已注释）：

```
优点：主线程也执行任务，等价于多一个 worker，对重任务有约 (N+1)/N 的加速
缺点：主线程既是 producer 又是 consumer，需要小心确保它不会提前返回
      （必须让 remaining_tasks 减到 0 才返回，不能仅凭 fetch_add 结果判断）
```

**建议**：先保持当前版本（正确性优先），性能测试后如果 Sleeping 版本的分数不如 Spinning，再开启主线程执行。

### 5.3 唤醒延迟的来源

```
notify_all() 调用后，worker 真正开始执行 runTask 的延迟组成：

1. futex_wake 系统调用：      ~1μs
2. OS 调度器选择线程：        ~0.5-2μs（取决于调度策略和 CPU 数量）
3. 上下文切换 + cache warm：  ~1-5μs（L1/L2 缓存未命中）
4. 从 wait 返回执行谓词检查： ~0.1μs

总计：约 2-8μs（低负载环境），可能达到 20-50μs（高负载/NUMA 环境）
```

对于 `super_super_light`（任务本身 < 0.1μs），这个延迟是不可接受的。这也是 Spinning 版本在某些极轻任务测试上反而更快的原因。

---

## 6. 潜在 Bug 清单（你的代码中实际存在的）

### Bug #1（严重性：中）：C 阶段可能错过下一批的 notify

**场景**：

```
时序：
1. 批次 K 的最后一个任务完成：batch_active = false, cv_work.notify_all()
2. run() 的 cv_done 唤醒，返回
3. 新的 run() 立刻被调用：batch_active = true，notify_all()  ← 这个 notify 发出时...
4. Worker-X 从 B 段结束，尝试 lk.lock() 进入 C 段
5. Worker-X 持锁，进入 cv_work.wait(lk, [this]{ return !batch_active; })
   此时 batch_active = true（新批次），谓词 = false，Worker-X 开始等待
6. 新批次的 notify_all（Step 3）已经发出，Worker-X 错过了！
7. Worker-X 必须等到新批次完成（Step 3 的 batch_active 再次变 false）才能唤醒
   但它对新批次没有贡献，只是在末尾唤醒了一下
```

**后果**：Worker-X 整个新批次期间不执行任何任务，只在批次结束时被唤醒。对于长时间运行的测试（反复调用 run()）可能导致个别 worker 长期"失联"。

**影响评估**：在 Part A 中，由于 `run()` 是同步的，这个窗口极小（通常 < 1μs），实际影响轻微。在 Part B 的快速异步批次中可能更明显。

**修复方案**：在 C 阶段，收到 `!batch_active` 时，立即循环检查是否有新批次就绪（即 A 段的谓词），而不是盲目回 A 睡下。实际上，当前 A→B→C→A 的流程已经处理了这个问题（回到 A 时会检查 `batch_active`），问题只在于 C 阶段 wait 结束后直接 loop 回 A 时，如果新批次已经 notify_all 过了，A 段的 wait 谓词成立会立即返回，不需要依赖另一次 notify。**所以这个 bug 实际上已经通过"谓词检查"被隐式处理了**。让我更精确地说明：如果 Worker-X 在 C 等待时，外部已经设置了新的 `batch_active = true`，那么 C 段的谓词 `!batch_active` 是 false，它确实会持续等待。但 A 段在 C 段结束后，会检查 `batch_active = true`，立即获取新批次任务。Worker-X 只是在新批次中途加入，不会错过整个批次。

### Bug #2（严重性：低）：num_total_tasks_ 不是原子变量但在 B 段无锁读

```cpp
// run() 中持锁写：
num_total_tasks_ = num_total_tasks;

// Worker B 段无锁读：
const int n = num_total_tasks_;
```

**为什么暂时没有问题**：Worker 进入 B 段之前，必须经过 A 段的 `cv_work.wait()`，而 wait 返回时持有锁。A 段退出持锁（`lk.unlock()`）之前读到的 `num_total_tasks_` 是受锁保护的可见值。

但是，B 段读的 `const int n = num_total_tasks_` 是在 `lk.unlock()` **之后**。这个读取没有锁保护，理论上是数据竞争（data race）。

**实际是否触发 UB？** 在 x86/arm64 上，普通变量的自然对齐读写是原子的（所谓"天然对齐"），所以不会读到撕裂值。但从 C++ 标准角度，这是未定义行为。

**修复方案**：将 `num_total_tasks_` 改为 `std::atomic<int>`，或在 `lk.unlock()` 之前读取并存入局部变量：

```cpp
// 正确做法：在持锁区间读入局部变量
lk.unlock();
// n 和 runnable 在 unlock 后使用局部变量副本
// 但 lk.unlock() 后读取 num_total_tasks_ 本身就是问题所在

// 更好的做法：unlock 前读入局部变量
IRunnable* local_runnable = current_runnable;
int local_n = num_total_tasks_;
lk.unlock();
// 后续使用 local_runnable 和 local_n
```

### Bug #3（已知，已接受）：Worker 在 B 段读 `current_runnable` 无锁

同 Bug #2 的分析，`current_runnable->runTask(...)` 中读 `current_runnable` 是无锁的。在 Part A 的单批次语义下这是安全的（run() 阻塞保证不会并发写 current_runnable），但在形式上是 data race。

---

## 7. 内存序选择指南

### 7.1 你的代码中使用的内存序汇总


| 位置                 | 变量                        | 内存序                      | 理由                                   |
| ------------------ | ------------------------- | ------------------------ | ------------------------------------ |
| Worker A 段：检查 stop | stop.load                 | `acquire`                | 看到 stop=true 后，后续读取所有数据都有效           |
| Worker B 段：抢号      | next_task_id_.fetch_add   | `relaxed`                | 只需原子性，不需要内存顺序                        |
| Worker B 段：完成计数    | remaining_tasks.fetch_sub | `acq_rel`                | acquire：确认是真正最后一个；release：我的工作结果对外可见 |
| 收尾：设置标志            | batch_active.store(false) | `release`                | 保证 runTask 结果在 false 可见之前已可见         |
| run()：发布标志         | batch_active.store(true)  | `release`（在锁内等价 seq_cst） | 发布屏障                                 |
| 析构：设置 stop         | stop.store(true)          | `release`                | 保证 stop=true 对所有 worker 可见           |


### 7.2 锁内的原子操作：一个常见困惑

```cpp
{
    std::lock_guard<std::mutex> lk(mtx);
    batch_active.store(false, std::memory_order_release);
}
```

**问题**：锁内的 store 可以用 `relaxed` 吗？

**答案**：可以，但不推荐。原因：

- `std::mutex::unlock()` 本身提供 release 语义，锁内所有写在 unlock 前对锁外可见
- 但显式用 `release` 提高了代码的可读性和可审查性
- 混用 `relaxed` 和锁会让代码维护者困惑（"这是故意的还是遗漏的？"）

### 7.3 进阶：何时不能用 `relaxed`

```
原则：当你的操作结果被其他线程作为"屏障/信号"使用时，需要 release/acquire 配对

typical pattern:
  producer:  data = ...         (任意顺序)
             flag.store(true, release)  ← "数据已就绪"

  consumer:  if (flag.load(acquire)) ← "我看到 flag=true"
               use(data)              ← "我一定能看到最新的 data"
```

---

## 8. 从 Part A 到 Part B 的架构演进

### 8.1 Part B 新增的核心问题

```
Part A：run(r, N) → 等待完成 → 返回（单批次同步）
Part B：runAsyncWithDeps(r, N, deps) → 立即返回（多批次异步 + DAG 依赖）
```

**最大的架构变化**：不再有"每次 run() 结束时系统静止"的保证。需要支持：

1. 多个批次同时处于"已注册但依赖未满足"的状态
2. 某批次所有依赖完成后，自动转移到"可执行队列"
3. `sync()` 等待所有已注册批次完成

### 8.2 Part B 需要的新数据结构

```cpp
struct LaunchRecord {
    IRunnable*          runnable;
    int                 num_total_tasks;
    std::atomic<int>    remaining_tasks;
    std::atomic<int>    dep_count;         // 剩余未完成的依赖数
    std::vector<TaskID> successors;        // 完成后需要通知的后继批次
    TaskID              id;
};

// 等待依赖满足的批次
std::unordered_map<TaskID, LaunchRecord*> pending_map;

// 依赖已满足，可以执行的任务队列
std::queue<Task>  ready_queue;

// 所有批次完成的计数（供 sync() 使用）
std::atomic<int>  total_batches_pending;
```

### 8.3 Part A Sleeping 版本的哪些部分可以复用


| 组件          | Part A 的实现                          | Part B 的变化                                                                 |
| ----------- | ----------------------------------- | -------------------------------------------------------------------------- |
| Worker 执行循环 | 从共享计数器抢号执行                          | 从 `ready_queue` 取任务执行（任务粒度从 batch 级别降到 task 级别）                            |
| 完成通知        | `remaining_tasks == 0` 通知 `cv_done` | `remaining_tasks == 0` 触发后继批次的 `dep_count--`，若 dep_count==0 则入 ready_queue |
| 线程池管理       | 不变                                  | 不变                                                                         |
| run()       | 发布一批，等待完成                           | 用 `runAsyncWithDeps() + sync()` 实现                                         |


### 8.4 Part B 关键的状态转移

```
批次状态机：
  REGISTERED → WAITING → READY → RUNNING → DONE

  REGISTERED：runAsyncWithDeps() 调用时注册
  WAITING：   dep_count > 0，等待依赖完成
  READY：     dep_count == 0，入 ready_queue
  RUNNING：   worker 正在执行其中的任务
  DONE：      remaining_tasks == 0
```

```
关键转移触发：
  批次 X 完成（DONE）时：
    for each successor S in X.successors:
        if (S.dep_count.fetch_sub(1) == 1):
            S.status = READY
            push S 到 ready_queue
            notify_all(cv_work)
```

---

## 附录：调试检查清单

在提交之前，用以下场景验证你的实现：

- `num_total_tasks = 0`：run() 是否正确跳过并立即返回？
- `num_total_tasks = 1`：单任务是否正确完成？
- `num_total_tasks = 1, num_threads = 16`：15 个线程空转时是否正确睡眠？
- 连续调用 1000 次 run()（`super_super_light` 模拟）：是否有线程永久睡眠不醒？
- 析构时若有任务在执行：是否等任务完成后再 join？（应该：`stop` 只在 wait 谓词中检查，不中断 runTask）
- `valgrind --tool=helgrind`：是否有数据竞争报告？
- `gdb` 死锁检查：`info threads` 是否所有线程都在预期的等待点？

---

*文档版本：v1.0 | 对应代码：Part A 已完成版本 | 下次更新：Part B 设计决策*