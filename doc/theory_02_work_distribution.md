# 理论 02：工作分配与调度

> 一句话：如何把 N 个任务分给 P 个线程，使总完成时间最短——这是 Lab 2 每一行代码背后的核心问题。

## 为什么你需要学这个

Lab 2 要实现三种任务调度系统（ParallelSpawn、ThreadPoolSpinning、ThreadPoolSleeping），它们的本质区别就是**工作分配策略**和**调度开销**的不同权衡。这一章的理论直接决定你写 `run()` 时的核心架构选择：

- 任务是预先切块分好（静态），还是让线程自己抢（动态）？
- 线程创建的开销有多大？什么时候值得用线程池？
- 理论上的加速上限是多少？实际差距来自哪里？

---

## 核心概念

---

### 概念 1：Amdahl 定律（Amdahl's Law）

**定义**

Amdahl 定律是一个描述并行计算加速比上限的定量模型。设程序中串行部分占比为 S（0 ≤ S ≤ 1），可并行部分占比为 (1-S)，使用 P 个处理器时，最大加速比为：

```
Speedup(P) = 1 / (S + (1-S)/P)
```

当 P → ∞ 时，加速比上限为 1/S。即程序中哪怕只有 5% 是串行的，加速比永远不会超过 20x，无论用多少核。

**直觉**

```
你有一条 100 米赛道：
  前 10 米只能一个人跑（串行部分，S = 0.1）
  后 90 米可以 16 个人并排跑（并行部分）

理想情况（16 人）：
  串行 10 米：不变，还是 1 个人跑的时间
  并行 90 米：16 人并排 → 时间缩短为 90/16 = 5.625 米等效
  总时间 = 10 + 5.625 = 15.625（原来是 100）
  加速比 = 100 / 15.625 = 6.4x（远小于 16x！）

结论：串行部分只占 10%，加速比就被拉到 6.4x，远低于 16x 的理想值。
```

**机制**

Amdahl 定律的数学推导：

```
T_serial = 1（归一化为 1）

T_parallel(P) = S × 1 + (1-S) × (1/P)
              = S + (1-S)/P

Speedup(P) = T_serial / T_parallel(P)
           = 1 / (S + (1-S)/P)
```

代入 Lab 2 的真实数字（16 核）：

| 串行占比 S | 加速比上限 | 效率（加速比/核数）|
|-----------|-----------|-------------------|
| 0% | 16.0x | 100% |
| 1% | 13.9x | 87% |
| 5% | 9.1x | 57% |
| 10% | 6.4x | 40% |
| 25% | 3.4x | 21% |
| 50% | 1.9x | 12% |

**关键洞察**：当 S > 5% 时，加速比已经严重下降。在 Lab 2 中，"串行部分"不是你显式写的串行代码——而是以下隐性开销：

```
Lab 2 中的隐性串行开销：
  ① 线程创建/销毁  → ParallelSpawn 的主要瓶颈
  ② atomic 竞争    → 16 线程 fetch_add 排队串行化
  ③ mutex 保护     → Sleeping 版中锁的争用
  ④ 主线程等待     → run() 同步等待所有 task 完成
  ⑤ 负载不均衡     → 最慢的线程决定总时间（长尾效应）
```

**在 Lab 2 中的体现**

`super_super_light` 测试：64 个 task，每个 task 只做一次数组复制（极轻量），400 次 bulk launch。

```
task 执行时间 ≈ 几百 ns
线程创建开销 ≈ 10-50 μs（ParallelSpawn 版）
400 次 launch × 16 线程 × 创建+销毁 ≈ 巨大的串行开销

结果：ParallelSpawn 版可能比 Serial 版还慢！
      因为"串行部分"（线程管理）远大于"并行部分"（实际计算）
```

这是 Amdahl 定律最直观的体现：**当并行带来的开销大于并行节省的时间，不如不并行**。

---

### 概念 2：静态任务分配（Static Assignment）

**定义**

静态任务分配是在任务开始执行前，将所有任务按照预定的规则一次性分配给各个工作线程的策略。分配方案在运行时不再变化。常见方式包括块分配（block partition）和交错分配（interleaved/cyclic partition）。

**直觉**

```
午餐分饭：
  静态分配 = 厨房提前把 100 份饭按座位号分好
    座位 1-25：给第一桌
    座位 26-50：给第二桌
    ...
  
  优点：没有排队（零分配开销）
  缺点：如果第一桌的人吃得慢（任务不均匀），其他桌干等
```

**机制**

两种常见的静态分配方式：

**块分配（Block Partition）：**

```
num_total_tasks = 100, num_threads = 4

线程 0 → task [0,  25)
线程 1 → task [25, 50)
线程 2 → task [50, 75)
线程 3 → task [75, 100)

代码：
  int chunk = num_total_tasks / num_threads;
  int start = thread_id * chunk;
  int end = (thread_id == num_threads - 1) ? num_total_tasks : start + chunk;
  for (int i = start; i < end; i++)
      runnable->runTask(i, num_total_tasks);
```

**交错分配（Interleaved/Cyclic）：**

```
线程 0 → task 0, 4, 8, 12, ...
线程 1 → task 1, 5, 9, 13, ...
线程 2 → task 2, 6, 10, 14, ...
线程 3 → task 3, 7, 11, 15, ...

代码：
  for (int i = thread_id; i < num_total_tasks; i += num_threads)
      runnable->runTask(i, num_total_tasks);
```

两者对比：

| 方式 | 负载均衡 | Cache 局部性 | 适用场景 |
|------|---------|-------------|---------|
| 块分配 | 差（最后一块可能更大）| 好（连续地址空间）| 任务均匀 + 访问连续内存 |
| 交错分配 | 略好（交叉抵消部分不均匀）| 差（跳跃访问）| 任务耗时有规律性变化 |

**在 Lab 2 中的体现**

Lab 2 的 `ping_pong_equal` 测试：所有 task 耗时相同 → 静态分配表现良好。
Lab 2 的 `ping_pong_unequal` 测试：不同 task 耗时差异大 → 静态分配会导致负载不均衡，快的线程闲着等慢的。

```
ping_pong_unequal 中静态块分配的问题：
  
  线程 0：████████████████████████  耗时 20ms（分到了重型 task）
  线程 1：████████                  耗时  8ms
  线程 2：██████                    耗时  6ms
  线程 3：████                      耗时  4ms
  
  总时间 = max(20, 8, 6, 4) = 20ms（被最慢线程拖累）
  理想时间 = (20+8+6+4) / 4 = 9.5ms
  效率 = 9.5 / 20 = 47.5%（超过一半的计算资源被浪费）
```

---

### 概念 3：动态任务分配（Dynamic Assignment）

**定义**

动态任务分配是在运行时按需将任务分配给空闲工作线程的策略。线程完成当前任务后，自主从全局任务队列中获取下一个任务。这种策略通过延迟绑定（late binding）实现自动负载均衡，代价是需要同步机制（如原子操作或锁）来协调任务获取。

**直觉**

```
超市排队：
  静态 = 进超市时被分到固定的收银台，不管那个台排多长
  动态 = 谁空了谁喊"下一位"，顾客去最先空出来的窗口

动态分配天然实现了负载均衡：快的线程多做事，慢的少做。
代价是每次"喊下一位"要花一点时间（原子操作的开销）。
```

**机制**

Lab 2 推荐的动态分配实现——全局原子计数器：

```
shared: atomic<int> next_task_id = 0

Worker 线程逻辑：
  loop:
    // fetch_add 是原子操作：读取当前值并立即+1，整个过程不被打断
    id = next_task_id.fetch_add(1)   
    
    // 如果返回的 id 已经 >= 总任务数，说明所有任务都被领完了
    if id >= num_total_tasks: break  
    
    // 执行拿到的这个 id 对应的任务
    runnable->runTask(id, num_total_tasks)
```

**为什么这种方式有效且没有遗漏？**

因为 `std::atomic` 的 `fetch_add` 保证了**绝对的串行化分配**。
假设 `next_task_id` 初始为 0，三个线程同时执行 `fetch_add(1)`：
- 硬件总线锁会强行给这三次操作排序
- 线程 A 拿到 0，`next_task_id` 变 1
- 线程 B 拿到 1，`next_task_id` 变 2
- 线程 C 拿到 2，`next_task_id` 变 3

**重点**：
- **不会重复**：没有任何两个线程会拿到相同的 id。
- **不会遗漏**：id 一定是从 0 到 `num_total_tasks - 1` 连续发出的。
- **任务完成次序无所谓**：线程拿到 id 后，谁先做完 `runTask` 并不重要。我们只要保证"每个 task_id 都恰好被分配给了一个线程去执行"即可，具体是哪个线程、什么次序完成的，不影响最终结果，只要最后都做完就行。

```
fetch_add(1) 的语义：
  ① 原子地读取当前值
  ② 原子地写入 当前值 + 1
  ③ 返回旧值（作为该线程"抢到"的 task_id）

关键保证：
  - 不会有两个线程拿到同一个 task_id（原子性保证）
  - 不需要额外的 mutex（硬件指令直接完成）
  - 每次调用只涉及一条 CPU 指令（x86: lock xadd; ARM: ldxr/stxr 循环）
```

**动态 vs 静态的量化对比（ping_pong_unequal 场景）：**

```
动态分配：
  线程 0：████████████  做了 12 个 task
  线程 1：██████████    做了 10 个 task
  线程 2：██████████    做了 10 个 task
  线程 3：████████████  做了 12 个 task（有些 task 轻有些重，自然平衡）
  
  总时间 ≈ 总工作量 / 线程数 ≈ 理想值
  效率接近 100%（每个线程都在做有用功）
```

**在 Lab 2 中的体现**

你在 Lab 2 的三个实现中都应该使用动态分配（原子计数器），原因：

- `ping_pong_unequal`：任务耗时不均，静态分配无法拿到好成绩
- `mandelbrot_chunked`：Mandelbrot 集边缘的 task 比中心的计算量大得多（迭代次数不同），需要动态均衡
- 即使是 `ping_pong_equal`（任务均匀），动态分配的额外开销极小（一次 fetch_add ≈ 20ns），不会比静态差

> **RDMA 映射**：RDMA 的 SQ（Send Queue）本身就是动态分配的模型——应用层不断向 SQ Post WR，RNIC 按 FIFO 顺序消费。但 RDMA 是单生产者单消费者（一个 QP 一个 RNIC），而 Lab 2 是单生产者多消费者（主线程 + N 个 Worker），所以需要原子操作来解决多消费者竞争。

---

### 概念 4：调度开销（Scheduling Overhead）

**定义**

调度开销是指并行系统中为管理任务分配、线程同步和通信所消耗的时间，这些时间不直接贡献于有用计算。调度开销包括线程创建/销毁、锁获取/释放、原子操作、条件变量通知/等待等。当调度开销与任务计算量相当或更大时，并行化反而导致性能下降。

**直觉**

```
场景：你要处理 100 份文件。

如果并行（10 个人分工）：
  开会讨论怎么分（分配开销）= 1 小时
  每人处理 10 份文件，同时进行
  
如果串行（1 个人干）：
  自己直接干（无分配开销）= 0 小时

【情况 A：重型任务】每份文件处理要 30 分钟
  串行总耗时 = 100 × 30分钟 = 50 小时
  并行总耗时 = 1小时(开会) + 10份×30分钟 = 6 小时
  结论：并行远快于串行（开会开得值）

【情况 B：轻量任务】每份文件处理只要 1 分钟
  串行总耗时 = 100 × 1分钟 = 1.6 小时
  并行总耗时 = 1小时(开会) + 10份×1分钟 = 1.16 小时
  结论：并行勉强快一点，开销占比太大

【情况 C：极轻任务】每份文件处理只要 1 秒钟
  串行总耗时 = 100 × 1秒 = 1.6 分钟
  并行总耗时 = 1小时(开会) + 10秒 = 1 小时 10 秒
  结论：并行远慢于串行！（调度开销压倒了计算时间）
```

**机制**

Lab 2 中的三种实现，调度开销依次递减：

```
                调度开销组成
                
ParallelSpawn（每次 run 创建线程）：
  ┌────────────────────────────────────────┐
  │  线程创建 × N  │  任务执行  │  线程 join × N │
  │  ~10-50 μs×16  │           │  ~10 μs×16     │
  └────────────────────────────────────────┘
  每次 run() 固定开销 ≈ 200-1000 μs
  
ThreadPoolSpinning（线程池 + 自旋）：
  ┌──────────────────────────────┐
  │  设置任务元数据  │  任务执行  │  自旋等待完成  │
  │  ~100 ns       │           │  ~100 ns       │
  └──────────────────────────────┘
  每次 run() 固定开销 ≈ 几百 ns（但自旋浪费 CPU）
  
ThreadPoolSleeping（线程池 + 睡眠）：
  ┌──────────────────────────────────────┐
  │  设置任务 + notify  │  任务执行  │  wait 等待完成  │
  │  ~1-10 μs          │           │  ~1-10 μs      │
  └──────────────────────────────────────┘
  每次 run() 固定开销 ≈ 几 μs（但不浪费 CPU）
```

**盈亏平衡点分析：**

```
设 task 执行时间 = T_task，线程数 = 16

ParallelSpawn 值得并行的条件：
  16 × T_task（串行总时间）> T_task + 调度开销（~500 μs）
  → T_task > 500 / 15 ≈ 33 μs
  → 每个 task 至少 33 μs 的计算量，Spawn 才值得

ThreadPoolSpinning 值得的条件：
  T_task > 调度开销 / 15 ≈ 0.01 μs = 10 ns
  → 几乎所有 task 都值得并行（除了空操作）

ThreadPoolSleeping 值得的条件：
  T_task > ~0.5 μs
  → 大多数 task 都值得并行
```

**在 Lab 2 中的体现**

| 测试 | 单 task 耗时（估计）| ParallelSpawn | ThreadPoolSpinning | ThreadPoolSleeping |
|------|---------------------|---------------|--------------------|--------------------|
| `super_super_light` | ~100 ns | 可能比 Serial 慢 | 有加速 | 有加速 |
| `super_light` | ~1 μs | 勉强有加速 | 有加速 | 有加速 |
| `ping_pong_equal` | ~10 μs | 有加速 | 有加速 | 有加速 |
| `recursive_fibonacci` | ~1 ms | 有加速（但创建开销占比小）| 有加速 | 有加速 |
| `mandelbrot_chunked` | ~100 μs | 有加速 | 有加速 | 有加速 |

**结论**：轻量任务（super_light 以下）是区分三种实现的关键战场。重型任务（fibonacci、mandelbrot）三种方案差距不大。

---

### 概念 5：Work Stealing（工作窃取）

**定义**

工作窃取（Work Stealing）是一种去中心化的动态负载均衡策略。每个工作线程维护一个私有的双端队列（deque）。线程优先从自己的队列头部取任务执行；当本地队列为空时，随机选择另一个线程，从其队列的尾部"窃取"任务。该策略由 Cilk 运行时首先实现，兼顾了局部性和负载均衡。

**直觉**

```
动态分配（Lab 2 的方案）：
  所有人排一个队取任务 → 队伍（atomic 计数器）是瓶颈

Work Stealing：
  每个人有自己的任务堆 → 先做自己的
  做完了？去别人堆里偷一个 → 只在空闲时才有竞争

  优点：几乎没有全局竞争（大多数时候取自己的任务，O(1) 且无锁）
  缺点：实现复杂（需要 lock-free deque）
```

**机制**

```
线程 0 的 deque:  [TaskA, TaskB, TaskC]  ← push 端（本地取）
线程 1 的 deque:  [TaskD, TaskE]
线程 2 的 deque:  []                      ← 空了！

线程 2 的行为：
  1. 检查自己的 deque → 空
  2. 随机选一个 victim（比如线程 0）
  3. 从线程 0 的 deque 尾部偷 TaskA
  4. 执行 TaskA

为什么从尾部偷？
  本地线程从头部取（栈行为，时间局部性好）
  窃取从尾部取（避免和本地线程竞争同一端）
  → 偷来的通常是较"大"的任务（递归分解场景下，底部任务粒度更粗）
```

**在 Lab 2 中的体现**

Lab 2 **不要求实现** Work Stealing。简单的全局 `atomic<int>` 计数器对 Lab 2 的测试规模已经足够。

但理解 Work Stealing 的价值在于：

1. 它是 Intel TBB、Cilk、Go runtime 等工业级调度器的核心策略
2. 在 Lab 2 的 16 线程规模下，全局 atomic 计数器的竞争还不算严重（~20ns/次）
3. 但如果扩展到 256 核或更多，全局 atomic 会成为严重瓶颈，此时 Work Stealing 是必选项

> **RDMA 映射**：NCCL 在多 GPU AllReduce 时，不同 GPU 各自处理自己的数据分片（类似本地 deque），只在需要交换时通过 RDMA 与其他 GPU 通信（类似 steal）。Ring AllReduce 中每个 GPU 只和相邻两个 GPU 交互，而不是全局广播——这正是"去中心化"思想的体现。

---

### 概念 6：线程池（Thread Pool）

**定义**

线程池是一种预先创建并维护一组工作线程的并发编程模式。线程在创建后持续存在，等待任务到达后执行，执行完毕后回到空闲状态继续等待，而非创建新线程执行一次后销毁。线程池避免了频繁的线程创建和销毁开销，是实现高吞吐任务调度系统的标准做法。

**直觉**

```
没有线程池（ParallelSpawn）：
  来一批快递 → 临时招 16 个快递员 → 送完 → 全部辞退
  下一批快递 → 再招 16 个 → 送完 → 再辞退
  每次招人 + 辞退的 HR 手续 ≈ 几十微秒

有线程池（ThreadPool）：
  开工第一天就招好 16 个快递员 → 常驻
  来快递了 → 直接派活
  没快递了 → 在休息室等着（Spinning: 站着等；Sleeping: 坐着等）
  关门那天 → 才辞退
```

**机制**

线程池的生命周期：

```
阶段 1：构造（一次性开销）
  TaskSystem(num_threads):
    for i in 0..num_threads:
      workers[i] = std::thread(worker_loop, this)  // 创建 N 个 Worker 线程

阶段 2：运行（多次调用，零线程创建开销）
  run(runnable, total):
    设置任务元数据（runnable_, total_, next_id=0, done=0）
    通知 Worker 线程开始工作
    等待所有 task 完成
    返回

阶段 3：析构（一次性开销）
  ~TaskSystem():
    设置 stop = true
    通知所有 Worker 退出
    for each worker: join()
```

**Spinning 版 vs Sleeping 版的 Worker 循环对比：**

```
Spinning 版（CPU 一直在转）：          Sleeping 版（CPU 有时休息）：
                                      
while (!stop_) {                      while (true) {
  id = next_id_.fetch_add(1);           unique_lock lk(mtx_);
  if (id < total_) {                    cv_.wait(lk, [&]{
    runnable_->runTask(id, total_);       return has_work || stop_;
    done_++;                            });
  }                                     if (stop_) break;
  // else: 继续空转                       id = next_id_++;
}                                       lk.unlock();
                                        runnable_->runTask(id, total_);
                                        lock → done_++ → maybe notify
                                      }

CPU 使用率：100%（即使无任务）          CPU 使用率：有任务时 100%，无任务时 0%
唤醒延迟：0（Worker 一直在检查）       唤醒延迟：~1-10 μs（内核调度 + CV notify）
适用场景：任务持续不断               适用场景：任务间有间隙
```

**在 Lab 2 中的体现**

你需要在 `TaskSystemParallelThreadPoolSpinning` 和 `TaskSystemParallelThreadPoolSleeping` 中分别实现这两种线程池。核心区别总结：

| 维度 | Spinning | Sleeping |
|------|----------|----------|
| Worker 空闲时行为 | 空转（while 循环不断检查） | 睡眠（cv.wait 释放 CPU） |
| 主线程等待完成 | 空转（while done < total） | 睡眠（cv_done.wait） |
| CPU 占用 | 始终 100%（所有核） | 按需（有任务才跑） |
| 轻量任务性能 | 差（空转抢走 Worker 的 CPU） | 好（不抢 CPU 资源） |
| 重型任务性能 | 好（零唤醒延迟） | 同样好（唤醒开销可忽略） |
| Lab 2 得分 | 中 | 高（super_light 等测试拉开差距） |

---

## 关键结论

- [ ] Amdahl 定律决定了并行加速的硬上限，串行部分 5% 就会让 16 核加速比降到 9x
- [ ] Lab 2 中的"串行部分"主要是隐性开销：线程创建、atomic 竞争、锁争用、主线程等待
- [ ] 动态分配（`atomic<int> fetch_add`）是 Lab 2 的推荐方案，能自动均衡不同耗时的 task
- [ ] 静态分配在任务不均匀时表现极差（`ping_pong_unequal` 测试会暴露这个问题）
- [ ] 线程池消除了重复线程创建开销，是 Spinning/Sleeping 版优于 ParallelSpawn 版的根本原因
- [ ] Work Stealing 是工业级方案但 Lab 2 不需要，全局 atomic 计数器对 16 线程已经够用
- [ ] Spinning 和 Sleeping 的本质区别是"CPU 空闲时做什么"——空转 vs 睡眠

---

## 自测

**[判断题 1]**
T/F：Amdahl 定律中的"串行部分"只包括程序员显式写的串行代码，不包括线程同步开销。

<details>
<summary>答案</summary>

**F（错误）**。任何不能被多线程并行执行的时间都算"串行部分"，包括线程创建/销毁、mutex 锁排队、atomic 操作的实际串行化等。在 Lab 2 中，这些隐性串行开销往往比显式串行代码更大。
</details>

---

**[判断题 2]**
T/F：在 `ping_pong_equal` 测试中，动态分配（atomic 计数器）比静态块分配性能一定更好。

<details>
<summary>答案</summary>

**F（错误）**。`ping_pong_equal` 的任务耗时均匀，静态分配也能很好地负载均衡。动态分配多了 atomic 操作的开销（每 task 约 20ns），所以在任务均匀且极轻量的场景下，静态分配可能略快。

但差距极小（ns 级），且动态分配在任务不均匀时优势巨大，所以综合考虑，Lab 2 建议统一用动态分配。
</details>

---

**[思考题 1]**
假设你的 Lab 2 `TaskSystemParallelSpawn::run()` 中，每次调用创建 16 个线程并 join。如果一个测试有 400 次 bulk launch，每次 launch 有 64 个 task，每个 task 耗时 500ns。估算：

1. 串行版本的总时间
2. ParallelSpawn 版的总时间（假设线程创建+join 开销 = 50μs/线程）
3. ThreadPool 版的总时间

<details>
<summary>参考思路</summary>

**串行版本：**
- 400 × 64 × 500ns = 12,800,000 ns = **12.8 ms**

**ParallelSpawn 版：**
- 每次 launch 的计算时间：64 × 500ns / 16 = 2,000 ns = 2 μs
- 每次 launch 的线程开销：16 × 50 μs = 800 μs（创建）+ 类似量（join）≈ 1,600 μs
- 每次 launch 总时间：2 + 1,600 = 1,602 μs
- 400 次 launch：400 × 1,602 μs = 640,800 μs ≈ **641 ms**
- **比串行版慢 50 倍！** 线程创建开销完全主导。

**ThreadPool 版：**
- 线程创建：一次性，16 × 50 μs = 800 μs（可忽略）
- 每次 launch 的计算时间：2 μs + 通知/唤醒开销 ~5 μs ≈ 7 μs
- 400 次 launch：400 × 7 μs = 2,800 μs ≈ **2.8 ms**
- **比串行快 ~4.6 倍**。没有达到理想 16x 是因为唤醒开销和 atomic 竞争。

这个计算解释了为什么 `super_super_light` 测试中：
- ParallelSpawn 惨不忍睹
- ThreadPool 才能体现真正的加速
</details>

---

**[思考题 2]**
Lab 2 的 `mandelbrot_chunked` 测试只有 1 次 bulk launch（128 个 task）。在这种场景下，ParallelSpawn 和 ThreadPool 版的性能差距会是多少？为什么？

<details>
<summary>参考思路</summary>

差距**很小**。

原因：
- 只有 1 次 bulk launch，所以 ParallelSpawn 只创建 16 个线程一次，开销 ~800 μs
- mandelbrot 每个 task 的计算量很大（~100 μs 级别），128 个 task 的总计算时间 >> 800 μs 的线程创建开销
- 线程创建开销占总时间的比例很小，Amdahl 定律中的 S 很小

这就是为什么课程设计了多种测试——在 `mandelbrot_chunked` 上所有方案差不多，但在 `super_super_light`（400 次 launch）上差距巨大。你需要一个在**所有测试**上都表现良好的实现。
</details>

---

**[代码预测题]**
以下动态分配代码有一个微妙的 Bug，你能找到吗？

```cpp
void TaskSystemParallelSpawn::run(IRunnable* runnable, int num_total_tasks) {
    std::atomic<int> next_id(0);
    
    auto worker = [&](int thread_id) {
        while (true) {
            int id = next_id++;       // 注意这里
            if (id >= num_total_tasks) break;
            runnable->runTask(id, num_total_tasks);
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads_; i++)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();
}
```

<details>
<summary>分析</summary>

**这段代码实际上是正确的**，但有一个性能相关的注意点：

`next_id++` 等价于 `next_id.fetch_add(1, std::memory_order_seq_cst)`，返回旧值。这是正确的动态分配实现。

但如果线程数 >> 任务数（比如 16 线程，2 个 task），那么最多 16 个线程会"无效地"调用 `fetch_add`，将 `next_id` 推到 2, 3, 4, ..., 17，其中 14 次 `fetch_add` 的结果 ≥ 2 被丢弃。这些 fetch_add 操作虽然是原子的，但每次都会触发 cache line bounce，产生几百 ns 的白白浪费。

在 Lab 2 中这不是致命问题（任务数通常远大于线程数），但在极端场景下可以优化为"先 load 再 fetch_add"的两步检查，减少不必要的原子写操作。

如果你没找到 Bug——恭喜，说明你已经掌握了动态分配的核心模式。
</details>
