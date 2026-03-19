# 理论 04：缓存一致性（Cache Coherence）

> 一句话：多核 CPU 每个核都有自己的缓存副本，当一个核修改数据时，其他核的缓存如何保持同步——这决定了你的 atomic 操作到底有多快。

## 为什么你需要学这个

Lab 2 中 16 个 Worker 线程共享 `next_task_id_`、`tasks_done_` 等变量。每次一个 Worker 执行 `fetch_add`，其他 15 个 Worker 缓存中的副本就失效了。这个"失效 → 重新获取"的过程有多慢，直接决定了 Spinning 版线程池的性能上限。

更隐蔽的是 **False Sharing**——两个看似无关的变量因为在同一个 cache line 里，互相拖慢对方——这是 Lab 2 性能调优中最常见也最难发现的问题。

---

## 核心概念

---

### 概念 1：缓存一致性问题（Coherence Problem）

**定义**

缓存一致性问题是指在多处理器系统中，多个处理器核心各自的缓存可能持有同一内存地址的不同副本，当某个核心修改了自己缓存中的数据时，其他核心的缓存副本变得过时（stale），如果不加以管理，程序将读到错误的值。缓存一致性协议负责保证：任何处理器在任何时刻读取某个地址时，都能获得该地址最近一次写入的值。

**直觉**

```
没有缓存一致性的世界：

  核 0 的 L1 cache：  counter = 5
  核 1 的 L1 cache：  counter = 5
  主存（DRAM）：      counter = 5
  
  核 0 执行：counter = 10（只写了自己的 L1 cache）
  
  核 0 的 L1 cache：  counter = 10  ← 新值
  核 1 的 L1 cache：  counter = 5   ← 旧值！
  主存（DRAM）：      counter = 5   ← 也还是旧值！
  
  核 1 读 counter → 得到 5 → 错误！
  
  缓存一致性协议的作用：
    核 0 写 counter 时，自动通知核 1"你的副本过期了"
    核 1 下次读 counter 时，去核 0 那里或主存拿最新值
```

**机制**

硬件通过**总线嗅探（Bus Snooping）**或**目录协议（Directory Protocol）**来维护一致性。Lab 2 的测试机（16 核 ARM Graviton3）使用目录协议，但概念上用总线嗅探更容易理解。

基本思路：所有缓存都监听总线上的读写操作，当发现其他核心访问了自己缓存中的数据时，做出相应的动作（失效自己的副本，或提供最新数据）。

**在 Lab 2 中的体现**

```
Worker 0 执行 next_task_id_.fetch_add(1)：
  ① 获取 next_task_id_ 所在 cache line 的独占权
  ② 其他 15 个核的 cache 中该 line 被标记为 Invalid
  ③ Worker 0 修改值，cache line 变为 Modified
  ④ Worker 1 接下来执行 fetch_add → 发现自己的副本 Invalid
  ⑤ 通过一致性协议从 Worker 0 的 cache 获取最新值（~20ns）
  ⑥ 重复...

16 个 Worker 轮流执行 fetch_add = 该 cache line 在 16 个核之间反复跳转
这就是"cache line bouncing"，是高并发 atomic 的核心开销来源
```

---

### 概念 2：MSI 协议

**定义**

MSI 协议是最基础的缓存一致性协议，为每个缓存行（cache line）维护三种状态：Modified（已修改，本核独占且与主存不一致）、Shared（共享，多核持有一致的只读副本）、Invalid（无效，本核的副本已过期）。任何状态转换都通过总线事务触发，保证全局一致性。

**直觉**

```
三种状态的现实类比——"共享文档编辑"：

  Invalid(I)：你没有这个文档（或你的版本已过期）
              → 要看就得去服务器下载最新版
  
  Shared(S)：你有最新版文档的只读副本，别人也有
             → 可以直接读，不能改
             → 类似 Google Docs 的"仅查看"权限
  
  Modified(M)：你正在编辑这个文档，且只有你有最新版
               → 可以随意读写（速度最快，不需要网络通信）
               → 别人的副本都标记为过期（Invalid）
               → 类似"独占编辑锁"
```

**机制**

MSI 状态转换图（每个 cache line 的状态机）：

```
           本核读（PrRd）
     ┌──────────────────┐
     │                  ↓
   ┌───┐   本核写    ┌───┐    其他核读    ┌───┐
   │ I │ ──────────→ │ M │ ────────────→ │ S │
   └───┘             └───┘               └───┘
     ↑                 │                   │
     │   其他核写        │    其他核写        │  本核写
     │←────────────────┘    │               │
     │←──────────────────────┘               │
     │                                       │
     │←──────────────────────────────────────┘
                                        ↑
                                本核读    │
                               ┌────────┘
                               └→(自身)
```

关键转换解读：

| 当前状态 | 事件 | 新状态 | 动作 |
|---------|------|--------|------|
| I | 本核读 | S | 从主存/其他核加载，~50-80ns |
| I | 本核写 | M | 从主存/其他核加载并独占，~50-80ns |
| S | 本核读 | S | 直接读 L1，~1ns（最快路径）|
| S | 本核写 | M | 发送 Invalidate 给所有持有该 line 的核，~20ns |
| M | 本核读 | M | 直接读 L1，~1ns（最快路径）|
| M | 本核写 | M | 直接写 L1，~1ns（最快路径）|
| M | 其他核读 | S | 提供数据给请求核，自己降级为 S |
| M/S | 其他核写 | I | 本核副本失效 |

**在 Lab 2 中的体现**

当 Worker 线程执行 `next_task_id_.fetch_add(1)` 时：

```
Worker 0 的视角：
  cache line 状态 = Invalid（其他 Worker 刚改过）
  → 通过一致性协议获取最新值（I → M），~20-50ns
  → 执行 fetch_add（M 状态下读写，~1ns）
  → 完成，cache line 状态 = Modified

Worker 1 的视角：
  cache line 状态 = Invalid（Worker 0 刚把它改成 M，本核被 Invalidate 了）
  → 同样需要获取最新值（I → M），~20-50ns
  → ...

结论：每次 fetch_add 的实际延迟 ≈ 20-50ns（I → M 转换）
     而不是 1ns（如果是独占的 M 状态下）
     16 个 Worker 串行轮流，每秒只能完成约 20-50M 次 fetch_add
```

---

### 概念 3：MESI 协议

**定义**

MESI 协议是 MSI 的扩展，增加了第四种状态 Exclusive（独占但未修改）。Exclusive 状态表示该 cache line 只有本核持有且与主存一致——此时写入可以直接从 E 转为 M，无需发送总线 Invalidate 消息（因为没有其他核持有副本）。MESI 优化了"先读后写"这一常见模式的性能。

**直觉**

```
MSI 的问题：
  核 0 读一个只有自己用的变量 → 进入 Shared 状态
  核 0 写这个变量 → 从 S 转 M，必须发 Invalidate 广播
  但实际上没有别人有副本！Invalidate 白发了。

MESI 的改进：
  核 0 读一个只有自己用的变量 → 进入 Exclusive 状态（不是 Shared）
  核 0 写这个变量 → 从 E 直接转 M，不需要 Invalidate
  → 省了一次总线事务
```

**机制**

```
MESI 四种状态：
  M（Modified）：独占 + 已修改（与 MSI 相同）
  E（Exclusive）：独占 + 未修改（MESI 新增）
  S（Shared）：  共享（与 MSI 相同）
  I（Invalid）： 无效（与 MSI 相同）

关键区别：
  当只有一个核读某个 cache line 时：
    MSI：进入 S 状态
    MESI：进入 E 状态（因为侦测到没有其他核持有副本）
  
  后续写操作：
    MSI：S → M，需要 Invalidate 广播
    MESI：E → M，静默转换（Silent Transition），零额外开销
```

**在 Lab 2 中的体现**

MESI 的 Exclusive 状态对 Lab 2 的影响不大，因为 `next_task_id_` 等热点变量几乎始终处于多核竞争状态（16 个 Worker 都在访问），很少有机会进入 E 状态。

但对于每个 Worker 线程自己的局部状态（如循环计数器），MESI 的 E→M 无广播优化会显著提高性能——前提是这些局部变量没有被 False Sharing 干扰（下面讲）。

---

### 概念 4：False Sharing（伪共享）

**定义**

False Sharing 是指多个处理器核心访问不同的变量，但这些变量恰好位于同一个缓存行（cache line，通常 64 字节）内。当某个核心写入自己的变量时，整个缓存行被标记为 Modified，导致其他核心的同一缓存行副本被 Invalidate，即使其他核心访问的是该行内完全不同的变量。结果是：逻辑上无关的操作产生了和真正的数据共享相同的一致性开销。

**直觉**

```
False Sharing 的现实类比：

  你和同事在同一张大桌子（cache line = 64 bytes）上各写各的文件。
  
  每次你写字时，桌子的"版本号"就变了，
  同事被迫停下来确认"你有没有动到我的文件"。
  
  实际上你只动了桌子左边，同事的文件在右边，互不相干。
  但因为操作系统（缓存一致性协议）只能追踪到"整张桌子"的粒度，
  它无法区分"同一桌子的不同位置"。
  
  结果：你们俩虽然在做完全独立的事，速度却被彼此拖慢了。
```

**机制**

**错误示例——导致 False Sharing 的代码：**

```cpp
struct TaskSystem {
    std::atomic<int> next_task_id_;    // offset 0-3   ┐
    std::atomic<int> tasks_done_;      // offset 4-7   ├─ 同一个 cache line（64 bytes）
    bool stop_;                        // offset 8     │
    int num_total_tasks_;              // offset 12-15 ┘
};
```

这些变量在同一个 64 字节的 cache line 里。当 Worker A 修改 `tasks_done_` 时，Worker B 正在读 `next_task_id_` 的缓存也被 Invalidate 了，即使 B 根本不关心 `tasks_done_`。

**量化影响：**

```
没有 False Sharing（各在独立 cache line）：
  fetch_add(next_task_id_) 延迟 ≈ 20ns（只有竞争同一变量的核受影响）

有 False Sharing（在同一 cache line）：
  fetch_add(next_task_id_) 延迟 ≈ 20ns（竞争）
  + tasks_done_ 的修改也触发 next_task_id_ 的 Invalidation
  + 总延迟可能增加 50-100%
```

**修复方案：**

```cpp
// 方案 1：手动填充（Padding）
struct alignas(64) TaskSystem {
    alignas(64) std::atomic<int> next_task_id_;    // 独占 cache line 0
    alignas(64) std::atomic<int> tasks_done_;      // 独占 cache line 1
    alignas(64) bool stop_;                        // 独占 cache line 2
    int num_total_tasks_;
};

// 方案 2：结构体对齐
struct PaddedAtomic {
    alignas(64) std::atomic<int> value;
};
PaddedAtomic next_task_id_;
PaddedAtomic tasks_done_;
```

**在 Lab 2 中的体现**

Lab 2 的评分标准是 PERF <= 1.2（不超过参考实现的 120%）。如果你的实现基本正确但性能差了一点点，False Sharing 往往是首要排查对象。

实际建议：

```
Lab 2 的规模（16 线程）下，False Sharing 的影响通常在 10-30% 范围内。
如果你的 PERF 在 1.0-1.3 之间，是否修复 False Sharing 取决于具体测试。
如果 PERF > 1.5，问题不在 False Sharing，而在算法设计层面。

优先级：先确保算法正确且逻辑合理，最后再考虑 False Sharing 优化。
```

> **RDMA 映射**：RDMA 的 QP 结构（Send Queue、Receive Queue、Completion Queue）在内存中通常经过精心的 cache line 对齐。RNIC 通过 DMA 写入 CQ Entry 时，如果 CQE 和应用层频繁访问的数据在同一 cache line 上，就会产生 False Sharing。这就是为什么 `ibv_cq` 的内部布局需要仔细设计。

---

### 概念 5：Cache Line Bouncing（缓存行弹跳）

**定义**

Cache Line Bouncing 是指一个频繁被多个处理器核心读写的缓存行，在这些核心的私有缓存之间反复迁移的现象。每次迁移都涉及一致性协议的通信开销（Invalidate + 数据传输），导致每次访问的延迟从 L1 级别（~1ns）退化到 L3 甚至跨核传输级别（~20-100ns）。

**直觉**

```
一本只有一本的参考书，被 16 个学生争抢：

  学生 A 拿走看 → 学生 B 要看 → A 不得不还回去 → B 拿走 →
  学生 C 要看 → B 还回去 → C 拿走 → ...

  每次"传递"需要 20ns
  16 个学生轮流传，一轮 = 16 × 20ns = 320ns
  每个学生一轮只看了一次 → 吞吐率 = 16 / 320ns = 50M 次/秒

  如果每人有自己的副本（只读场景）：
    每人 1ns 看一次 → 吞吐率 = 16 / 1ns = 16G 次/秒
    快了 320 倍！
```

**机制**

Lab 2 中 cache line bouncing 的热点变量：

```
热度排名（从高到低）：

1. next_task_id_（最热）：
   每个 task 分配一次 fetch_add → 如果有 10000 个 task，
   bouncing 10000 次，每次 ~20ns → 总开销 ~200μs

2. tasks_done_（次热）：
   每个 task 完成一次 fetch_add/increment → 同样 bouncing

3. mutex 内部状态（中等）：
   Sleeping 版中 lock/unlock 涉及原子操作
   但频率低于 next_task_id_（因为锁的粒度可以设计得粗一些）

4. condition_variable 内部状态（低）：
   notify/wait 的频率远低于单个 task 的分配
```

**在 Lab 2 中的体现**

这是 Spinning 版和 Sleeping 版在轻量任务上的性能差异的根本原因：

```
Spinning 版：
  16 个 Worker 不停地 fetch_add(next_task_id_)
  即使没有新任务，也在无意义地 bouncing → CPU 空转 + cache 污染
  主线程也在 spin-wait tasks_done_ → 又多了一个竞争者

Sleeping 版：
  无任务时 Worker 睡眠，不会 bounce 任何 cache line
  有任务时才 fetch_add → bouncing 次数 = 实际 task 数量（无浪费）
  主线程也在 cv_done.wait 睡眠 → 不参与竞争

结论：Sleeping 版在轻量任务上显著减少了无效的 cache line bouncing
```

---

## 关键结论

- [ ] 每个核有独立的 L1/L2 cache，共享数据需要一致性协议来保证正确性
- [ ] MSI/MESI 协议通过 Invalidate 消息使其他核的缓存副本失效，这不是免费的（~20ns/次）
- [ ] `atomic<int>` 的 `fetch_add` 每次执行都会触发 cache line bouncing，延迟 ~20-50ns
- [ ] False Sharing 是最隐蔽的性能 Bug：不同变量在同一 cache line 里导致互相干扰
- [ ] 修复 False Sharing 用 `alignas(64)` 让关键变量独占 cache line
- [ ] Spinning 版的"空转"不只是浪费 CPU——还在无意义地 bounce cache line，拖慢真正干活的 Worker
- [ ] 优先保证算法正确，最后再调 False Sharing——这是 10-30% 级别的优化，不是量级优化

---

## 自测

**[判断题 1]**
T/F：在 MSI 协议中，一个 cache line 可以同时在两个核的缓存中处于 Modified 状态。

<details>
<summary>答案</summary>

**F（错误）**。Modified 状态意味着"只有本核有最新数据"，这是独占的。如果两个核同时 M，就无法确定谁的版本更新，一致性被破坏。一致性协议保证任何时刻最多一个核持有 M 状态。
</details>

---

**[判断题 2]**
T/F：如果一个变量只被一个线程读写，它永远不会因为 cache coherence 产生额外开销。

<details>
<summary>答案</summary>

**不完全正确（取决于 False Sharing）**。如果该变量独占一个 cache line，确实不会有一致性开销（始终在 M 或 E 状态）。但如果和其他线程频繁修改的变量共享同一个 cache line，就会因为 False Sharing 而被反复 Invalidate，即使自己从未被其他线程访问。
</details>

---

**[思考题 1]**
在 Lab 2 的 Spinning 版线程池中，假设有 16 个 Worker 线程，每个 task 执行时间为 100ns。单次 `fetch_add` 由于 cache line bouncing 的延迟为 30ns。那么实际的 task 吞吐率（tasks/sec）大约是多少？和理想情况（无 bouncing 开销）相比，效率损失了多少？

<details>
<summary>参考思路</summary>

**理想情况（无 bouncing）：**
- 16 个 Worker 各自独立执行 task
- 每个 Worker 的吞吐率 = 1 task / 100ns
- 总吞吐率 = 16 / 100ns = 160M tasks/sec

**有 bouncing 时：**
- 每个 task 的实际处理时间 = 100ns（计算）+ 30ns（fetch_add bouncing）= 130ns
- 但 fetch_add 是串行瓶颈：16 个线程排队执行 fetch_add
- fetch_add 吞吐率 = 1 / 30ns ≈ 33M/sec（全局上限）
- 16 个 Worker 的计算吞吐率 = 160M/sec

瓶颈取决于哪个更小：
- 计算吞吐率：160M/sec
- fetch_add 吞吐率：33M/sec
- 瓶颈 = **33M/sec**（fetch_add 成为瓶颈！）

效率 = 33M / 160M = **20.6%**

结论：当 task 执行时间（100ns）和 fetch_add 延迟（30ns）在同一数量级时，atomic 操作成为严重瓶颈。这就是 `super_super_light` 测试中 Spinning 版表现差的原因。
</details>

---

**[代码预测题]**
以下代码在 16 线程运行时，`total` 的最终值会是正确的 160000 吗？有没有性能问题？

```cpp
struct Counters {
    std::atomic<int> per_thread_count[16];  // 每线程一个计数器
};

Counters counters;

void worker(int tid) {
    for (int i = 0; i < 10000; i++) {
        counters.per_thread_count[tid].fetch_add(1);
    }
}

// 16 个线程各自执行 worker(0), worker(1), ..., worker(15)
// 最后 total = sum of per_thread_count[0..15]
```

<details>
<summary>分析</summary>

**正确性：是的，total = 160000。** 每个线程只写自己的 `per_thread_count[tid]`，没有数据竞争。

**性能问题：有严重的 False Sharing！**

`std::atomic<int>` 大小为 4 字节。`per_thread_count[16]` 是连续的 64 字节（16×4），恰好**完整覆盖一个 cache line**。

这意味着所有 16 个线程的计数器在同一个 cache line 里。线程 0 修改 `per_thread_count[0]` 会 Invalidate 线程 1 的 `per_thread_count[1]` 的缓存，即使它们是不同的变量。

修复：

```cpp
struct alignas(64) PaddedCounter {
    std::atomic<int> count;
};

PaddedCounter per_thread_count[16];  // 每个计数器独占一个 cache line
```

修复后性能可能提升 **5-10 倍**（因为消除了无意义的 cache line bouncing）。
</details>
