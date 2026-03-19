# 理论 06：数据局部性与争用

> 一句话：并行程序的性能不只取决于"计算快不快"，还取决于"数据在不在该在的地方"和"多个线程是否在抢同一个资源"。

## 为什么你需要学这个

Lab 2 的不同测试用例表现差异巨大——同一个线程池实现，在 `recursive_fibonacci` 上可能有接近 16x 的加速比，但在 `super_super_light` 上可能比串行还慢。原因往往不在于算法逻辑，而在于：

- 数据访问的局部性好不好（cache 命中率）
- 多个线程是否在争抢同一个共享资源（Contention）
- 通信开销（数据在核间传递的延迟）是否超过了计算本身

这一章帮你诊断"代码逻辑正确但性能不达标"的问题。

---

## 核心概念

---

### 概念 1：数据局部性（Data Locality）

**定义**

数据局部性是指程序访问数据地址在时间和空间上表现出的聚集特性。时间局部性（Temporal Locality）指近期访问过的数据很可能在不久后再次被访问；空间局部性（Spatial Locality）指与当前访问地址相邻的地址很可能即将被访问。良好的局部性使程序的数据访问大部分落在 CPU 缓存中（cache hit），而非频繁访问慢速的主存（cache miss）。

**直觉**

```
局部性好 = 你需要的东西都在手边（L1 cache，1ns）
局部性差 = 你每次都得去仓库取（DRAM，80ns）

类比：
  你在写报告，引用的资料：
    ✓ 好的局部性：资料就摊在桌上，随手拿 → 效率高
    ✗ 差的局部性：每查一条数据都要去图书馆借 → 来回跑

在并行场景下，局部性还有"核间"维度：
  你的数据在自己核的 L1 cache → 1ns
  你的数据在其他核的 L1 cache → 20ns（通过一致性协议转移）
  你的数据不在任何 cache → 80ns（去 DRAM）
```

**机制**

影响 Lab 2 性能的两种局部性场景：

**场景 A：task 内部的数据局部性（影响单个 task 的速度）**

```
ping_pong_equal 测试：
  每个 task 操作一段连续的数组区域
  → 空间局部性好：加载一个 cache line 后，相邻的元素都在
  → 加速比接近理想值

mandelbrot_chunked 测试：
  每个 task 计算连续几行的像素
  → 空间局部性好（连续行在内存中连续）
  → 但不同 task 的计算量差异大（边缘 vs 中心）
```

**场景 B：task 之间共享数据的局部性（影响调度开销）**

```
16 个 Worker 共享 next_task_id_：
  这个变量在任何时刻只在 1 个核的 L1 中是 Modified
  其他 15 个核的副本是 Invalid
  → "核间局部性"极差：每次访问都是 cache miss（对本核而言）
  
解决思路：减少共享变量的访问频率
  比如：Worker 一次领取 4 个 task（而非每次领 1 个）
  fetch_add(4) → 一次原子操作换 4 个 task → 竞争频率降为 1/4
```

**在 Lab 2 中的体现**

你不需要在 Lab 2 中做复杂的局部性优化。但理解局部性帮你解释性能差异：

| 测试 | 数据局部性 | 调度局部性 | 综合表现 |
|------|----------|----------|---------|
| `recursive_fibonacci` | 极好（纯计算，无数据）| 无所谓（task 很重）| 加速比接近理想 |
| `ping_pong_equal` | 好（连续数组）| 一般 | 加速比较好 |
| `super_super_light` | 差（频繁 cache miss）| 差（调度开销主导）| 加速比很低 |

---

### 概念 2：争用（Contention）

**定义**

争用（Contention）是指多个线程同时竞争同一个共享资源（如锁、原子变量、内存总线）时产生的串行化瓶颈。当 N 个线程同时尝试获取同一把锁时，N-1 个线程必须等待；当 N 个线程同时执行同一个原子变量的 read-modify-write 时，硬件会将这些操作串行化。争用程度随线程数增加而加剧，是并行程序可扩展性（Scalability）的主要限制因素。

**直觉**

```
争用 = 多人争抢同一个资源时排队等待的现象

超市只有 1 个收银台（= 1 个共享的 atomic 变量）：
  1 个顾客 → 不用排队，立即结账
  16 个顾客 → 排队等 15 人，大部分时间在等

争用的代价不只是"等待时间"：
  等待时，你的 CPU 核心在做什么？
    Spinning 版：空转（while 循环），浪费电和 cache 带宽
    Sleeping 版：睡觉（无开销），但唤醒需要时间
```

**机制**

Lab 2 中的三类争用：

**① 原子变量争用（Atomic Contention）**

```
next_task_id_.fetch_add(1)：
  硬件实现：总线锁/cache line 独占
  16 个线程并发 → 实际串行执行
  吞吐率上限 ≈ 1 / cache_line_transfer_latency ≈ 30-50M ops/sec

影响 Lab 2 的场景：
  task 执行时间 < 100ns（如 super_super_light）时
  fetch_add 的串行化开销可能超过 task 计算本身
```

**② 锁争用（Lock Contention）**

```
Sleeping 版的 mtx_ 锁：
  所有 Worker 在以下时刻需要持锁：
    - wait predicate 检查
    - 获取 task_id（next_task_id_++）
    - 更新 tasks_done_
    - 检查是否完成并 notify
  
  如果每次持锁时间 = 1μs，16 个线程串行持锁：
    最坏情况：16 × 1μs = 16μs 才能轮一圈
    
  优化：尽量缩短持锁时间（critical section 越短越好）
    获取 id → 立即解锁 → 执行 task → 重新加锁更新 done
    而不是：加锁 → 获取 id → 执行 task → 更新 done → 解锁
```

**③ 内存总线争用（Memory Bus Contention）**

```
多个核同时从 DRAM 加载数据时，共享的内存控制器成为瓶颈。

影响 Lab 2 的场景：
  ping_pong_equal 测试：16 个 Worker 同时读写大数组
  如果数组不在 cache 中 → 所有核竞争内存带宽
  
  AWS c7g.4xlarge 内存带宽 ≈ 30-50 GB/s
  16 个核各自需要 ~5 GB/s → 总需求 ~80 GB/s → 超过上限
  → 内存带宽成为瓶颈，即使 CPU 有空闲
```

**在 Lab 2 中的体现**

诊断争用问题的思路：

```
症状：加速比远低于预期，且增加线程数后反而变慢

排查步骤：
  1. 是否计算密集型？（recursive_fibonacci）
     → 如果是，加速比应接近线性。不是 → 不是争用问题
  
  2. 是否轻量 task？（super_super_light）
     → atomic 争用可能是瓶颈。验证：减少 fetch_add 频率看性能是否提升
  
  3. 是否大量内存访问？（ping_pong_equal）
     → 内存带宽可能是瓶颈。验证：用更少线程（如 4 线程）看加速比
     → 如果 4 线程接近 4x 但 16 线程远不到 16x → 带宽饱和
```

---

### 概念 3：通信开销与计算通信比

**定义**

在并行计算中，通信开销是指线程/进程之间为了同步状态和传递数据所消耗的时间。计算通信比（Computation-to-Communication Ratio，也称算术强度 Arithmetic Intensity）是有用计算量与通信量的比值。当计算通信比低时，通信成为瓶颈，并行效率下降。

**直觉**

```
计算通信比的直觉：

  高比值（计算密集）：
    每搬运 1 字节数据，做 100 次浮点运算
    → 通信开销可忽略，并行效率高
    → 例：recursive_fibonacci
  
  低比值（通信密集）：
    每搬运 1 字节数据，做 0.1 次运算
    → 大部分时间在搬运数据（或等锁），不在计算
    → 例：super_super_light（任务本身是纯拷贝）
```

**机制**

Lab 2 的"通信"不是网络通信，而是核间通信——具体来说就是：

```
Lab 2 中的"通信"类型：

1. 任务分配通信：
   fetch_add(next_task_id_) → cache line bouncing
   每个 task 分配 1 次 → 开销 ~30ns/task

2. 完成通知通信：
   tasks_done_++ → cache line bouncing
   每个 task 完成 1 次 → 开销 ~30ns/task
   
3. 条件变量通信（Sleeping 版）：
   notify_all + wait → 涉及内核调用
   每次 run() 2 次（一次唤醒 Worker，一次等完成）→ 开销 ~10μs/run

4. 数据通信（task 本身操作的数据）：
   如果 task 操作的数组太大放不进 cache → DRAM 访问
   延迟 ~80ns/cache-miss
```

**计算通信比与 Lab 2 的加速比关系：**

```
task 计算时间 = T_compute
task 通信开销 = T_comm ≈ 60ns（fetch_add + done++）

实际每 task 耗时 = T_compute + T_comm

加速比 ≈ (N × T_compute) / (N/P × T_compute + T_comm × N/P + 固定开销)

简化后的关键比值 = T_compute / T_comm

  T_compute >> T_comm（如 fibonacci，T_compute ~1ms）
    → 通信可忽略 → 加速比 ≈ P（线程数）
  
  T_compute ≈ T_comm（如 super_light，T_compute ~100ns）
    → 通信占一半 → 加速比 ≈ P/2
  
  T_compute << T_comm（如 super_super_light，T_compute ~10ns）
    → 通信主导 → 加速比 << P，可能 < 1
```

**在 Lab 2 中的体现**

这个分析框架能帮你预判一个测试的加速比上限：

| 测试 | 估计 T_compute | T_comm | 比值 | 预期加速比 |
|------|---------------|--------|------|-----------|
| `recursive_fibonacci` | ~1 ms | ~60 ns | 16000:1 | ~16x |
| `mandelbrot_chunked` | ~100 μs | ~60 ns | 1600:1 | ~16x |
| `ping_pong_equal` | ~10 μs | ~60 ns | 160:1 | ~14x |
| `super_light` | ~1 μs | ~60 ns | 16:1 | ~8x |
| `super_super_light` | ~100 ns | ~60 ns | 1.6:1 | ~2-4x |

> **RDMA 映射**：RDMA 的核心设计目标就是提高计算通信比。在传统 TCP/IP 中，每次发送数据都涉及多次内存拷贝和内核调用（通信开销大）。RDMA 通过 zero-copy 和 kernel bypass 将通信开销降到接近硬件极限（~1μs 的网络延迟），使得计算通信比大幅提高——这和 Lab 2 中用线程池替代 Spawn（减少调度开销）是同一种优化思路。

---

### 概念 4：减少争用的常见策略

**定义**

减少争用的核心方法包括：（1）减少共享——让线程尽可能使用私有数据；（2）减少临界区长度——持锁时间越短，其他线程等待越少；（3）细粒度锁——用多把锁保护不同的数据，而非一把大锁保护所有数据；（4）无锁算法——使用原子操作替代锁，避免线程阻塞。

**直觉**

```
减少争用的四种策略类比：

1. 减少共享 = 每个人有自己的工具箱，不用排队借工具
2. 缩短临界区 = 用完公共打印机立刻让位，不要在打印机旁边改文件
3. 细粒度锁 = 不是锁整个图书馆，而是每个书架一把锁
4. 无锁 = 传送带模式，每人取自己的号码牌（fetch_add），不用排队
```

**机制**

在 Lab 2 Sleeping 版中可以应用的策略：

**策略 1：缩短临界区**

```cpp
// 差的实现（持锁执行 task）
lk.lock();
int id = next_task_id_++;
runnable->runTask(id, total);  // 持锁执行 task！其他线程全在等
tasks_done_++;
lk.unlock();

// 好的实现（只在必要时持锁）
lk.lock();
int id = next_task_id_++;
auto* r = current_runnable_;
int t = num_total_tasks_;
lk.unlock();                   // 取完 id 立即释放锁

r->runTask(id, t);             // 不持锁执行 task

lk.lock();
tasks_done_++;                 // 重新加锁更新计数
if (tasks_done_ == t) cv_done_.notify_one();
lk.unlock();
```

**策略 2：批量获取任务（减少 fetch_add 频率）**

```
普通方式：每个 task 一次 fetch_add → N 次原子操作
批量方式：一次 fetch_add(BATCH_SIZE) → N/BATCH_SIZE 次原子操作

Worker：
  int start = next_task_id_.fetch_add(4);  // 一次领 4 个 task
  for (int id = start; id < min(start+4, total); id++)
      runnable->runTask(id, total);

优点：原子操作频率降为 1/4
缺点：最后一批可能领多了（比如领了 4 个但只剩 2 个）→ 需要边界检查
      批量大小选择需要权衡：太大 → 负载不均；太小 → 争用未减少
```

Lab 2 中这是一个可选优化。简单的逐个 `fetch_add(1)` 对大多数测试已经够用。

**策略 3：分离 "分配" 和 "完成通知" 的锁**

```
如果 next_task_id_ 和 tasks_done_ 用不同的保护机制：
  next_task_id_：使用 atomic（无锁，Worker 直接 fetch_add）
  tasks_done_：使用 atomic
  其他共享状态：使用 mutex

这样 Worker 取 task 时不需要加 mutex（只 fetch_add），
只在需要检查条件变量或更新复杂状态时才加锁。
→ 锁的持有时间大大缩短
```

**在 Lab 2 中的体现**

优化优先级（投入产出比从高到低）：

```
1. 正确实现 Sleeping 版的条件变量逻辑    → 必须做，否则不工作
2. 在锁外执行 runTask                     → 必须做，否则性能很差
3. 用 atomic 做 task_id 分配              → 推荐做，减少锁争用
4. 批量获取任务                           → 可选，对轻量任务有帮助
5. False Sharing 对齐                     → 可选，10-30% 改善
6. 细粒度锁                              → 不推荐，复杂度高，Lab 2 规模不需要
```

---

## 关键结论

- [ ] 数据局部性决定了单个 task 的执行速度——cache hit vs miss 可差 80 倍
- [ ] 争用（Contention）是并行可扩展性的主要杀手：atomic 争用、锁争用、内存带宽争用
- [ ] 计算通信比是判断"值不值得并行"的核心指标：T_compute >> T_comm 才值得
- [ ] `super_super_light` 性能差的根因是计算通信比太低（~1.6:1），不是你的代码有 Bug
- [ ] 缩短临界区是最有效的减少锁争用策略——在锁外执行 `runTask()`
- [ ] 批量获取任务和 False Sharing 对齐是进阶优化，保证正确性后再考虑

---

## 自测

**[判断题 1]**
T/F：在 Lab 2 中，如果所有 task 的执行时间都相同，那么 Spinning 版和 Sleeping 版的性能应该一样。

<details>
<summary>答案</summary>

**F（错误）**。即使 task 耗时均匀，两者的差异仍然来自：

1. **Spinning 版的主线程空转**：主线程 `while(done < total){}` 占用一个核心的 CPU 资源，抢夺 Worker 线程的调度时间片
2. **Spinning 版的无效 cache bouncing**：当所有 task 执行完毕但 run() 尚未被下一次调用时，Worker 继续空转 fetch_add，无意义地 bounce cache line
3. **轻量任务场景**：如果 task 很轻，上述两个开销占比会变得显著

在重型 task（如 `recursive_fibonacci`）场景下两者确实接近。
</details>

---

**[判断题 2]**
T/F：使用批量获取（一次 `fetch_add(4)` 领取 4 个 task）总是比逐个获取（`fetch_add(1)`）更快。

<details>
<summary>答案</summary>

**F（错误）**。批量获取在以下场景可能更慢：

1. **task 数量不是批量大小的整数倍**：最后一批领多了，部分 task 被"浪费"（id >= total），需要额外的边界检查
2. **task 耗时不均匀时加剧负载不均**：一个线程领了 4 个重型 task，其他线程已经空闲了
3. **task 总数少于线程数 × 批量大小**：比如只有 10 个 task、16 线程、每次领 4 个 → 只有 3 个线程拿到任务，13 个线程空闲

在 Lab 2 中，大多数测试的 task 数量远大于线程数，逐个 `fetch_add(1)` 已经足够好。
</details>

---

**[思考题]**
Lab 2 的 Sleeping 版中，`run()` 函数的结构大致如下：

```cpp
void run(IRunnable* runnable, int num_total_tasks) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        current_runnable_ = runnable;
        num_total_tasks_ = num_total_tasks;
        next_task_id_ = 0;
        tasks_done_ = 0;
    }
    cv_worker_.notify_all();
    
    std::unique_lock<std::mutex> lk(mtx_);
    cv_done_.wait(lk, [this]{ return tasks_done_ == num_total_tasks_; });
}
```

这个实现有一个性能问题：`notify_all()` 之后立即又 `lock(mtx_)`。为什么这可能导致性能下降？如何改进？

<details>
<summary>参考思路</summary>

**问题分析：**

`notify_all()` 唤醒了 16 个 Worker 线程，它们全部尝试 `lock(mtx_)` 来进入 `cv_worker_.wait()` 的 predicate 检查。与此同时，主线程也在竞争 `mtx_` 来进入 `cv_done_.wait()`。

最坏情况：主线程抢到了锁，进入 `cv_done_.wait()`（释放锁 + 睡眠）。然后 16 个 Worker 依次获取锁、检查 predicate、获取 task_id。因为锁是串行化的，16 个 Worker 排队获取第一个 task 就花了 16 × lock_time。

**改进方案：**

将 `next_task_id_` 改为 `std::atomic<int>`，Worker 获取 task_id 时不需要持锁：

```cpp
// Worker 循环
while (true) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_worker_.wait(lk, [this]{ return has_work_ || stop_; });
    if (stop_) return;
    lk.unlock();  // 尽快释放锁
    
    while (true) {
        int id = next_task_id_.fetch_add(1);  // 无锁获取 task
        if (id >= num_total_tasks_) break;
        current_runnable_->runTask(id, num_total_tasks_);
        
        if (tasks_done_.fetch_add(1) + 1 == num_total_tasks_) {
            std::lock_guard<std::mutex> lk(mtx_);
            cv_done_.notify_one();
        }
    }
}
```

这样 Worker 被唤醒后只需 lock 一次（检查 predicate），然后就在无锁循环中连续获取 task，大大减少了锁争用。
</details>

---

**[代码预测题]**
以下代码试图通过减少锁的范围来提高性能，但引入了一个 Bug。你能找到吗？

```cpp
void run(IRunnable* runnable, int num_total_tasks) {
    // 不加锁设置任务（试图减少锁争用）
    current_runnable_ = runnable;
    num_total_tasks_ = num_total_tasks;
    next_task_id_ = 0;
    tasks_done_ = 0;
    
    cv_worker_.notify_all();
    
    std::unique_lock<std::mutex> lk(mtx_);
    cv_done_.wait(lk, [this]{ return tasks_done_ == num_total_tasks_; });
}
```

<details>
<summary>分析</summary>

**Bug：任务元数据的写入不在锁保护内，破坏了 happens-before 关系。**

具体问题：

1. `current_runnable_ = runnable` 等写操作没有在 `mtx_` 保护下。Worker 线程在 `cv_worker_.wait()` 返回后（此时持有 `mtx_`），读取 `current_runnable_`。但主线程的写入和 Worker 的读取之间没有 mutex 的 unlock→lock 构成的 happens-before 关系。

2. 更糟糕的是：`notify_all()` 可能在 Worker 的 `wait()` 还没开始时就发出了。如果 Worker 此时还在处理上一批任务的最后一个 task，等它回到 `wait()` 时 notify 已经错过了。由于 predicate 检查 `has_work`（假设有这个标志），而 `has_work` 也没在锁内设置，Worker 可能看到旧值。

**修复：所有共享变量的修改必须在 `mtx_` 保护内。** 这就是 theory_05 中讲的"happens-before 链不能断"原则。

```cpp
void run(IRunnable* runnable, int num_total_tasks) {
    {
        std::lock_guard<std::mutex> lk(mtx_);  // 必须在锁内
        current_runnable_ = runnable;
        num_total_tasks_ = num_total_tasks;
        next_task_id_ = 0;
        tasks_done_ = 0;
    }
    cv_worker_.notify_all();
    
    std::unique_lock<std::mutex> lk(mtx_);
    cv_done_.wait(lk, [this]{ return tasks_done_ == num_total_tasks_; });
}
```
</details>
