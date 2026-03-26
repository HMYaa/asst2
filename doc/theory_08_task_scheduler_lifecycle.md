# 概念 8：Task Scheduler 运行生命周期

**定义**
Task Scheduler 是一种将并发任务分配给工作线程的协调机制，通过共享任务队列和同步原语实现任务的并行执行与结果聚合。

**直觉**
想象餐厅的**接单-后厨-传菜**模式：
- 前台接单（主线程 `run()` 接收任务）
- 任务单挂在公告板（任务队列）
- 后厨空闲的厨师去取单（Worker 线程抢占）
- 做完后打铃通知（同步机制）

---

## 核心逻辑：任务生命周期

```
┌─────────────────────────────────────────────────────────────────────┐
│                        TaskScheduler 完整生命周期                    │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   主线程（调用方）                    Worker 线程池                  │
│   ──────────────────                 ────────────────────            │
│                                                                     │
│   1. run(runnable, N) ──────────→   创建 N 个 lambda 闭包           │
│          │                              ↓                           │
│          │                          入队到 task_queue_              │
│          │                              ↓                           │
│          │                          notify_all() 唤醒等待线程        │
│          │                              ↓                           │
│   2. 主线程自己 ──────────────────→  抢到任务的 Worker 执行 task()   │
│          │                              ↓                           │
│          │                          --remaining_tasks             │
│          │                              ↓                           │
│          │               若 remaining_tasks == 0                   │
│          │                              ↓                           │
│          │               done_cv_.notify_one() 唤醒主线程            │
│          │                              ↓                           │
│   3. done_cv_.wait() ◀────────────   等待主线程被唤醒               │
│          │                                                        │
│          ↓                                                        │
│   4. 返回（任务全部完成）                                          │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 关键原语详解

### 1. `std::mutex` - 互斥锁

**问题**：为什么需要锁？
```
无锁场景（错误）：
线程A: 读取 queue.front()     线程B: queue.pop()
线程A: queue.pop()           线程B: 读取 queue.front()  
       ↓                            ↓
   结果：两个线程可能处理同一个任务，或某个任务丢失
```

**mutex 的本质**：
- 保证**同一时刻只有一个线程**能进入临界区（被保护的代码段）
- 防止 data race（数据竞争）

### 2. `std::condition_variable` - 条件变量 ⭐（你的模糊点）

**定义**
条件变量是线程同步机制，允许一个线程**阻塞等待**直到另一个线程通知某个条件为真。

**状态机转换**（这是关键！）：

```
┌────────────────────────────────────────────────────────────────────┐
│                   Worker 线程状态机                                 │
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
│   [Running] ──wait()──→ [Blocked on CV] ◀──notify_one()──┐        │
│       ↑                                    │                   │   │
│       │                                    │                   │   │
│       │                                    │                   │   │
│       └───wake up, re-acquire mutex ───────┘                   │   │
│                                                                    │   │
│   条件：pred() 返回 true ──→ 继续执行任务                         │   │
│                                                                    │   │
│   条件：pred() 返回 false ──→ 重新 wait() 释放 mutex            │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

**wait() 的精确语义**（你可能混淆的地方）：

```cpp
// 这行代码实际做了两件事（原子性地）：
queue_cv_.wait(lock, [this]() { return stop_ || !task_queue_.empty(); });

// 等价于：
while (!pred()) {  // pred = [this]() { stop_ || !task_queue_.empty() }
    cv_.wait(lock);  // 1. 释放 lock
                     // 2. 阻塞等待
                     // 3. 被唤醒后重新获取 lock
                     // 4. 再次检查 pred()
}
```

**为什么需要 predicate？**
- ** spurious wakeup（伪唤醒）**：操作系统可能随机唤醒等待线程，即使没有 notify
- predicate 确保真正满足条件才继续执行

### 3. `std::atomic` - 原子变量

**为什么用 atomic 而不是 mutex？**
```cpp
std::atomic<int> remaining_tasks{num_total_tasks};

// 无锁递增/递减，高性能
--remaining_tasks;  // 原子操作，保证可见性
```

**可见性保证**：
- 线程 A 修改 `remaining_tasks`
- 线程 B 读取 `remaining_tasks`
- ** Happens-Before** 保证：修改之前的操作对读取可见

---

## 在 Lab 2 中的体现

| 类 | 关键代码 | 说明 |
|---|---------|------|
| `TaskSystemParallelThreadPoolSleeping` | `worker_thread()` | 典型的 CV 用法 |
| `run()` | `queue_cv_.notify_all()` | 入队后唤醒所有 Worker |
| `run()` | `done_cv_.wait()` | 主线程等待任务完成 |
| `run()` | `remaining_tasks` | atomic 计数器 |

**自测题**：
```cpp
// 以下代码有什么问题？
void worker_thread() {
    while (!stop_) {
        queue_cv_.wait(lock);  // 缺少 predicate！
        if (!task_queue_.empty()) {
            auto task = task_queue_.front();
            task_queue_.pop();
            task();
        }
    }
}
```
<details>
<summary>答案</summary>

**问题**：伪唤醒会导致访问空队列！

正确写法：
```cpp
while (true) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait(lock, [this]() { return stop_ || !task_queue_.empty(); });
    // ...
}
```
</details>

---

## [RDMA 映射]

| Task Scheduler 概念 | RDMA 数据面对应 | 类比说明 |
|--------------------|----------------|---------|
| `run()` 提交任务 | `ibv_post_send()` / `ibv_post_recv()` | 将"任务描述"放入硬件队列 |
| `task_queue_` | Send Queue / Recv Queue | 软件侧的缓冲层 |
| `notify_all()` | WQE 入队触发 DMA 传输 | 通知硬件有工作要做 |
| Worker 抢占任务 | RNIC 从 SQ 取 WR | 硬件轮询队列 |
| `done_cv_.wait()` | `ibv_poll_cq()` 阻塞轮询 | 等待完成事件 |
| `remaining_tasks == 0` | CQ 中出现 N 个 CQE | 所有操作完成 |

> **关键区别**：Task Scheduler 是**软件轮询 + 调度**，RDMA 是**硬件轮询 + DMA**，但模式惊人相似。

---

## 下一步：学习路径

在动手实现 `TaskSystemParallelThreadPoolSleeping` 之前，你必须彻底理解：

- [ ] `std::condition_variable` 的 spurious wakeup 和 predicate 必要性
- [ ] RAII 锁管理（`std::unique_lock` vs `std::lock_guard`）
- [ ] atomic 的内存顺序语义（`memory_order_seq_cst` 等）

这三个是避免 CPU 空转和长尾延迟的关键。
