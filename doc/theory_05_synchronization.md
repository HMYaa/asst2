# 理论 05：同步原语与内存一致性

> 一句话：mutex 和 condition_variable 在底层是怎么实现的，以及为什么用了锁之后线程一定能看到最新数据——这是 Lab 2 Sleeping 版线程池的理论根基。

## 为什么你需要学这个

Lab 2 Part A Step 3（ThreadPoolSleeping）是得分最高、也最难正确实现的部分。你需要同时使用 `mutex`、`condition_variable`、`atomic`，并理解：

- 条件变量的 `wait()` 内部到底做了什么？为什么必须持锁？
- 为什么锁的 unlock 和 lock 构成了 happens-before 关系？
- 主线程写的数据，Worker 被唤醒后一定能看到吗？为什么？
- Spinning 版里的 `while(done < total){}` 安全吗？会不会被编译器优化掉？

---

## 核心概念

---

### 概念 1：锁的底层实现（Hardware Primitives for Locking）

**定义**

锁（Lock/Mutex）的底层实现依赖于硬件提供的原子 read-modify-write 指令，如 Test-and-Set（TAS）、Compare-and-Swap（CAS）和 Load-Linked/Store-Conditional（LL/SC，ARM 架构使用）。这些指令保证在多核环境下对同一内存地址的读取和修改是不可分割的原子操作，从而可以构建互斥语义。

**直觉**

```
你需要一个机制，让"检查门锁"和"锁上门"成为一个不可打断的动作。

没有原子操作：
  线程 A：看看锁是否空闲？ → 是
  （此刻 CPU 切换到线程 B）
  线程 B：看看锁是否空闲？ → 是
  线程 B：锁上！
  线程 A：锁上！
  → 两个线程都认为自己持有锁 → 灾难

有原子操作（Test-and-Set）：
  线程 A：原子地（检查 + 锁上）→ 成功，返回"之前是空闲的"
  线程 B：原子地（检查 + 锁上）→ 失败，返回"之前已被锁"
  → 只有 A 获得锁 → 正确
```

**机制**

**Test-and-Set（TAS）实现的自旋锁：**

```cpp
class SpinLock {
    std::atomic<bool> locked_{false};
public:
    void lock() {
        while (locked_.exchange(true)) {
            // exchange 原子地将 locked_ 设为 true，返回旧值
            // 旧值为 true → 别人持有锁 → 继续自旋
            // 旧值为 false → 我获得了锁 → 退出循环
        }
    }
    void unlock() {
        locked_.store(false);
    }
};
```

**Compare-and-Swap（CAS）实现更通用：**

```cpp
void lock() {
    bool expected = false;
    while (!locked_.compare_exchange_weak(expected, true)) {
        expected = false;  // CAS 失败时 expected 被修改，需要重置
    }
}
```

**std::mutex 的实际实现（简化版）：**

```
std::mutex 不是纯自旋锁，它是"两阶段"策略：

阶段 1（用户态自旋）：
  尝试 CAS 获取锁
  失败 → 自旋若干次（通常几十到几百次）
  如果短时间内获得锁 → 无需进入内核，最快路径

阶段 2（内核态睡眠）：
  自旋仍未获得锁 → 调用 futex(FUTEX_WAIT) 系统调用
  线程进入内核等待队列，释放 CPU
  锁被释放时，持有者调用 futex(FUTEX_WAKE) 唤醒等待者

这就是为什么 std::mutex 在低竞争时几乎和自旋锁一样快，
但在高竞争时不会像纯自旋锁那样浪费 CPU。
```

**在 Lab 2 中的体现**

```
Spinning 版：不用 mutex，Worker 直接 fetch_add 抢任务（类似纯自旋）
Sleeping 版：用 mutex + condition_variable（内部是两阶段锁 + futex）

Sleeping 版中 lock/unlock 的开销：
  无竞争时：~25ns（纯用户态 CAS）
  有竞争时：~1-10μs（涉及内核态 futex）

这就是为什么 Sleeping 版每次 run() 的固定开销比 Spinning 版大，
但在轻量任务场景下总体更好——因为它不浪费 CPU。
```

---

### 概念 2：条件变量的内部机制

**定义**

条件变量（Condition Variable）是一种允许线程等待某个条件成立的同步原语。`wait(lock)` 操作原子地释放关联的互斥锁并将线程挂起到等待队列中；被 `notify_one()` 或 `notify_all()` 唤醒后，自动重新获取锁。这种"释放锁 + 挂起"的原子性保证了不会丢失在释放锁和挂起之间发生的通知（lost wakeup）。

**直觉**

```
条件变量解决的问题：

  你想等一个包裹到达。没有条件变量时：
    while (包裹没到) {
        开门看看 → 没有 → 关门
        开门看看 → 没有 → 关门
        开门看看 → 没有 → 关门
        ...（反复开关门，累死）
    }

  有条件变量时：
    告诉快递员"到了给我打电话"
    挂掉电话 → 睡觉
    电话响了 → 起床 → 开门取包裹

  "挂掉电话 + 睡觉"必须是原子的！
  如果你先挂电话、再准备睡觉，快递员可能在这个间隙打来
  你错过了电话（lost wakeup），然后永远睡下去...
```

**机制**

`condition_variable::wait(lock, predicate)` 的内部实现（伪代码）：

```cpp
void wait(unique_lock<mutex>& lk, function<bool()> pred) {
    while (!pred()) {           // ① 检查条件（持锁状态下）
        // 原子地执行以下两步：
        lk.unlock();            // ② 释放锁
        enqueue_self_and_sleep(); // ③ 加入等待队列并挂起
        // --- 线程在此处睡眠 ---
        // 被 notify 唤醒后：
        lk.lock();              // ④ 重新获取锁
        // 回到 while 循环顶部，重新检查 pred()
    }
    // pred() 为 true 且持有锁 → 返回
}
```

**为什么必须持锁调用 wait？**

```
错误做法（不持锁调用 wait）：

  时间线 →
  Worker：检查 ready_queue 为空... 准备调用 wait...
  主线程：                        ← 这个间隙插入！
          往 ready_queue 放了任务
          调用 notify_all()
  Worker：                        调用 wait() → 睡着了
  
  Worker 错过了 notify！因为 notify 发生在 wait 之前。
  主线程以为已经通知了，不会再次 notify。
  → Worker 永远睡着 → 死锁

正确做法（持锁调用 wait）：
  Worker 先 lock()，再检查条件，再 wait()
  主线程必须 lock() 后才能修改 ready_queue 和 notify
  → 要么 Worker 检查时就看到了新任务（不需要 wait）
  → 要么 Worker 先 wait 了，主线程后 notify（Worker 被正确唤醒）
  不可能错过
```

**为什么需要 predicate（防止 spurious wakeup）？**

```
spurious wakeup = 操作系统在没有 notify 的情况下唤醒了线程

原因（Linux futex 实现细节）：
  信号处理（signal）可能导致 futex_wait 提前返回
  内核实现为了简化，允许偶尔的虚假唤醒

没有 predicate：
  cv.wait(lk);        // 被虚假唤醒
  // 直接开始执行 → 但条件并没有满足 → 错误！

有 predicate：
  cv.wait(lk, [&]{ return has_work || stop; });
  // 被虚假唤醒 → 检查 predicate → false → 继续睡
  // 被真正 notify → 检查 predicate → true → 返回
```

**在 Lab 2 中的体现**

Sleeping 版线程池需要两个条件变量：

```
cv_worker_：Worker 等待新任务
  wait 条件：next_task_id_ < num_total_tasks_ || stop_
  notify 时机：run() 设置新任务后，或析构时

cv_done_：主线程等待所有任务完成
  wait 条件：tasks_done_ == num_total_tasks_
  notify 时机：最后一个 task 完成时

这两个 CV 共用同一把 mutex（mtx_），
保护所有共享状态的一致性访问。
```

---

### 概念 3：内存一致性模型（Memory Consistency Model）

**定义**

内存一致性模型定义了多处理器系统中，一个处理器的内存操作对其他处理器可见的顺序约束。顺序一致性（Sequential Consistency, SC）要求所有处理器看到相同的全局内存操作顺序，且每个处理器的操作按程序顺序出现。宽松一致性模型（Relaxed Consistency）允许处理器对无依赖的内存操作进行重排序以提高性能，但需要显式的内存屏障（Memory Fence/Barrier）来在必要时强制顺序。

**直觉**

```
问题的根源：CPU 为了性能会"乱序执行"。

你写的代码：
  data = 42;        // 写 ①
  ready = true;     // 写 ②

CPU 可能的实际执行顺序：
  ready = true;     // 写 ② 先执行了！（因为 ready 在 cache 中，更快）
  data = 42;        // 写 ① 后执行

另一个核看到的：
  if (ready) {
    use(data);      // 读到了 ready=true，但 data 还是旧值！→ Bug！
  }

这就是为什么需要内存屏障（Memory Fence）：
  data = 42;
  std::atomic_thread_fence(std::memory_order_release);  // 屏障：①必须在②之前可见
  ready.store(true);
  
  // 另一个核：
  if (ready.load()) {
    std::atomic_thread_fence(std::memory_order_acquire); // 屏障：②之后的读一定看到①
    use(data);      // 保证看到 data=42
  }
```

**机制**

C++ 提供三种主要的内存序（Memory Order）：

| Memory Order | 保证强度 | 性能 | 何时用 |
|---|---|---|---|
| `seq_cst`（默认）| 最强：所有线程看到相同的全局顺序 | 最慢 | 默认选择，最安全 |
| `acquire/release` | 中等：形成 happens-before 关系 | 中等 | 生产者-消费者模式 |
| `relaxed` | 最弱：只保证原子性，不保证顺序 | 最快 | 纯计数器（不用于同步）|

**Acquire/Release 语义的核心思想：**

```
Release（释放语义）：
  "我之前写的所有数据，在这个点之后对其他线程可见"
  类似"提交事务"——提交之后，别人才能看到你做的修改

Acquire（获取语义）：
  "从这个点开始，我能看到对方 Release 之前写的所有数据"
  类似"刷新缓存"——获取之后，保证看到最新的数据

Release + Acquire 构成 happens-before 关系：
  线程 A：写 data → Release（unlock 或 atomic store）
  线程 B：Acquire（lock 或 atomic load）→ 读 data
  
  保证：B 读到的 data 一定是 A 写的值（或更新的值）
```

**在 Lab 2 中的体现**

好消息：你不需要手动使用 acquire/release。Lab 2 中用到的同步原语内部已经包含了正确的内存屏障：

```
std::mutex::lock()    → 内含 Acquire 语义
std::mutex::unlock()  → 内含 Release 语义
std::atomic<int> 默认  → seq_cst（最强保证）
condition_variable::wait() 返回 → 内含 Acquire（因为重新 lock 了）

所以在 Sleeping 版中：
  主线程 unlock 后 → Release：所有写入（runnable, total, next_id）对 Worker 可见
  Worker lock 后  → Acquire：保证读到主线程的最新写入
  → 数据一致性由 mutex 的 acquire/release 语义自动保证
```

**Spinning 版的安全性：**

```
Worker 的自旋循环：
  while (tasks_done_.load() < num_total_tasks_) {}

这安全吗？
  如果 tasks_done_ 是 std::atomic<int> → 安全
  因为 atomic load 默认是 seq_cst，保证能看到其他核的 store

  如果 tasks_done_ 是普通 int → 不安全！
  编译器可能把 load 优化为只读一次寄存器，后续循环不再从内存读取
  → 永远看不到其他线程的更新 → 死循环
```

> **RDMA 映射**：RDMA Write 完成后，目标端 CPU 的缓存中可能还是旧数据（因为 DMA 绕过了 CPU cache）。这就是为什么 RDMA Write 后需要通过 `IBV_SEND_SIGNALED` + Completion 机制来确保目标端 CPU 看到最新数据。本质上是同一个"内存可见性"问题，只是 RDMA 涉及跨机器，而 Lab 2 在单机多核内。

---

### 概念 4：Happens-Before 关系

**定义**

Happens-Before 是并发程序中定义操作之间可见性和顺序的偏序关系。若操作 A happens-before 操作 B（记作 A ≺ B），则 A 的效果（包括所有内存写入）保证对 B 可见。Happens-before 关系可以通过以下方式建立：（1）同一线程内的程序顺序；（2）mutex 的 unlock ≺ 后续的 lock；（3）condition_variable 的 notify ≺ 对应的 wait 返回；（4）atomic store(release) ≺ 对应的 load(acquire)。

**直觉**

```
happens-before 回答的问题：
  "线程 B 读数据时，线程 A 写的值一定能被看到吗？"

如果 A 的写 happens-before B 的读 → 一定能看到
如果没有 happens-before 关系 → 看不看得到是随机的（数据竞争/未定义行为）

建立 happens-before 的最常见方式：
  线程 A：写 data → unlock(mtx)
  线程 B：lock(mtx) → 读 data
  
  unlock ≺ lock → 写 data ≺ 读 data → B 一定看到 A 写的值
```

**机制**

Lab 2 Sleeping 版中的 happens-before 链：

```
主线程 run()：
  ① mtx_.lock()
  ② current_runnable_ = runnable     // 写
  ③ num_total_tasks_ = total         // 写
  ④ next_task_id_ = 0                // 写
  ⑤ tasks_done_ = 0                  // 写
  ⑥ mtx_.unlock()                    // Release → happens-before ⑦
  ⑦ cv_worker_.notify_all()

Worker 线程：
  ⑧ cv_worker_.wait(lk, pred)  返回  // Acquire ← happens-after ⑥
  ⑨ 读 next_task_id_                 // 保证看到 ④ 写的 0
  ⑩ 读 current_runnable_             // 保证看到 ② 写的 runnable
  ⑪ 执行 runTask()

happens-before 链：②③④⑤ ≺ ⑥(unlock) ≺ ⑧(wait返回/lock) ≺ ⑨⑩⑪

结论：Worker 被唤醒后，一定能看到主线程设置的所有任务元数据。
这不是巧合，而是 mutex 的 acquire/release 语义保证的。
```

**在 Lab 2 中的体现**

如果你在 Sleeping 版中把某些共享变量的修改放在了锁的保护**之外**：

```cpp
// 错误示例
current_runnable_ = runnable;      // 在锁外写
{
    std::lock_guard<std::mutex> lk(mtx_);
    // 只在锁内修改部分变量
    next_task_id_ = 0;
}
cv_worker_.notify_all();
```

问题：`current_runnable_` 的写不在 happens-before 链中（在锁外），Worker 可能读到旧值。这种 Bug 在大多数情况下"碰巧正确"，但在极端时序下会崩溃，极难调试。

**规则：所有共享变量的修改，必须在同一把锁的保护内完成。**

---

### 概念 5：notify_one vs notify_all 的选择

**定义**

`notify_one()` 唤醒等待在条件变量上的**一个**线程（如果有的话），`notify_all()` 唤醒**所有**等待的线程。选择哪个取决于：有多少个等待者可能因为条件变为真而应该被唤醒。如果只有一个等待者的条件会变为真，使用 `notify_one` 更高效；如果多个等待者的条件可能同时变为真，必须使用 `notify_all` 以避免遗漏。

**直觉**

```
notify_one = 喊一个人起床
notify_all = 拉响所有人的闹钟

什么时候用 notify_one：
  你刚做好 1 份饭 → 只需叫 1 个人来吃
  
什么时候用 notify_all：
  你刚放了 100 个任务进队列 → 所有空闲 Worker 都应该醒来抢活干
```

**机制**

Lab 2 中的两个通知场景：

```
场景 1：run() 提交新任务 → 用 notify_all
  原因：新任务有多个（num_total_tasks 可能很大），
       所有空闲 Worker 都应该醒来同时处理。
       如果用 notify_one，只唤醒 1 个 Worker，其余继续睡，
       导致并行度远低于预期。

场景 2：最后一个 task 完成，通知主线程 → 用 notify_one
  原因：只有一个主线程在 cv_done_.wait() 等待。
       notify_one 就够了，notify_all 也不会错（只是多余）。

场景 3：析构函数，通知所有 Worker 退出 → 用 notify_all
  原因：所有 Worker 都需要被唤醒以检查 stop_ 标志并退出循环。
```

**常见错误：在 run() 提交任务时用了 notify_one**

```
错误表现：
  16 个 Worker，提交了 1000 个 task
  notify_one → 只唤醒 1 个 Worker
  这个 Worker 做完 1 个 task 后...没人 notify 其他 Worker
  → 只有 1 个 Worker 在干活，其余 15 个在睡觉
  → 性能退化为接近串行
  
  （除非 Worker 完成 task 后会 notify 其他 Worker——
   但这增加了实现复杂度，不如直接 notify_all 简单可靠）
```

**在 Lab 2 中的体现**

简单策略：

```
run() 中：notify_all（唤醒所有 Worker）
task 完成且 tasks_done_ == total：notify_one（只通知主线程）
析构中：notify_all（唤醒所有 Worker 退出）
```

这个策略不是性能最优的（可能存在"惊群效应"——唤醒了 16 个 Worker 但只有 3 个 task），但对 Lab 2 的规模足够好，且实现最简单。

---

## 关键结论

- [ ] `std::mutex` 底层是 CAS + futex 的两阶段实现，低竞争时很快（~25ns）
- [ ] `condition_variable::wait()` 原子地释放锁并挂起线程，这个原子性防止了 lost wakeup
- [ ] `wait()` 必须带 predicate，因为 spurious wakeup 在 Linux 上是真实存在的
- [ ] `mutex` 的 lock/unlock 自带 acquire/release 语义，保证跨线程的数据可见性
- [ ] 共享变量的修改必须在同一把锁内完成，否则 happens-before 链断裂，可能读到旧值
- [ ] 通知 Worker 有新任务用 `notify_all`，通知主线程完成用 `notify_one`
- [ ] Lab 2 全程使用默认的 `atomic`（seq_cst）和 `mutex`，不需要手动管理 memory order

---

## 自测

**[判断题 1]**
T/F：`std::mutex` 的性能在任何情况下都比 `std::atomic` 的 CAS 操作差。

<details>
<summary>答案</summary>

**F（错误）**。在无竞争时，`std::mutex::lock()` 只是一次 CAS（~25ns），和直接用 atomic CAS 差不多。mutex 开销主要在高竞争时的内核态 futex 调用。

但 mutex 的优势是：当竞争激烈时，等待的线程会进入内核睡眠而非自旋，不浪费 CPU。纯 atomic CAS 自旋在高竞争时会浪费大量 CPU 资源。
</details>

---

**[判断题 2]**
T/F：在 Lab 2 的 Spinning 版中，`tasks_done_` 必须是 `std::atomic<int>` 而非普通 `int`，即使它已经被 volatile 修饰。

<details>
<summary>答案</summary>

**T（正确）**。`volatile` 只告诉编译器"不要优化掉这个读取"，但不提供以下保证：（1）原子性（`int++` 在多线程下仍可能丢失更新），（2）内存顺序（编译器和 CPU 仍可能重排与其他变量的读写顺序）。`std::atomic<int>` 同时提供原子性和内存序保证。

在 C++ 标准中，`volatile` 从来不是为多线程同步设计的（它是为内存映射 I/O 设计的），不能替代 `atomic`。
</details>

---

**[思考题]**
以下 Sleeping 版 Worker 循环有一个微妙的 Bug，你能找到吗？

```cpp
void worker_loop() {
    while (true) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_worker_.wait(lk, [this]{ return has_work_ || stop_; });
        if (stop_) return;
        
        int id = next_task_id_++;
        if (id >= num_total_tasks_) continue;  // 没抢到任务
        
        lk.unlock();
        current_runnable_->runTask(id, num_total_tasks_);
        lk.lock();
        
        tasks_done_++;
        if (tasks_done_ == num_total_tasks_) {
            has_work_ = false;
            cv_done_.notify_one();
        }
    }
}
```

<details>
<summary>分析</summary>

**Bug 在 `continue` 那一行。**

当 `id >= num_total_tasks_` 时，执行 `continue` 跳回循环顶部。但此时 `lk` 仍然持有锁（因为还没有调用 `lk.unlock()`）。回到循环顶部后，`std::unique_lock<std::mutex> lk(mtx_)` 会尝试再次 lock 一个已经被自己持有的 `mtx_` → **死锁**（std::mutex 不是递归锁）。

等等，实际上 `unique_lock` 在 `continue` 时离开作用域再重新进入，会先析构（unlock）再构造（lock），所以不会死锁。

真正的 Bug 更微妙：当 `id >= num_total_tasks_` 时 `continue`，线程会回到 `cv_worker_.wait()`。但 `has_work_` 仍然为 true（还没有被任何人重置为 false），所以 predicate 立即为 true，`wait` 不会阻塞，线程又执行 `next_task_id_++`，拿到一个更大的无效 id，又 `continue`...

**结果是无限循环地递增 `next_task_id_` 而不做任何有用功**，直到其他 Worker 完成所有 task 并将 `has_work_` 设为 false。

修复：在 `continue` 之前不要无条件回到 wait，或者让 predicate 更精确地检查是否还有未分配的 task。

这个例子说明：**predicate 的设计必须精确反映"是否真的有工作可做"，而不只是"run() 是否被调用过"。**
</details>

---

**[代码预测题]**
以下代码在多线程环境下是否安全？为什么？

```cpp
int data = 0;
std::atomic<bool> ready{false};

// 线程 A
data = 42;
ready.store(true);  // 默认 memory_order_seq_cst

// 线程 B
while (!ready.load()) {}  // 默认 memory_order_seq_cst
assert(data == 42);       // 这个 assert 会失败吗？
```

<details>
<summary>分析</summary>

**安全，assert 不会失败。**

原因：`ready.store(true)` 使用默认的 `memory_order_seq_cst`，它同时包含 release 语义。`ready.load()` 同样是 `seq_cst`，包含 acquire 语义。

happens-before 链：
- `data = 42` ≺ `ready.store(true)`（同一线程，程序顺序）
- `ready.store(true)` ≺ `ready.load()` 返回 true（seq_cst store ≺ load）
- `ready.load()` ≺ `assert(data == 42)`（同一线程，程序顺序）

所以 `data = 42` happens-before `assert(data == 42)`，B 一定看到 42。

但如果把 store/load 改为 `memory_order_relaxed`，就不安全了——relaxed 不建立 happens-before 关系，B 可能看到 `data` 的旧值 0。

这就是为什么 Lab 2 中统一使用默认的 `atomic`（seq_cst）是最安全的选择。
</details>
