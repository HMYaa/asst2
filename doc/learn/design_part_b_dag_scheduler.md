# Part B：DAG 依赖任务调度器 设计全景文档

> **作者视角**：20 年高性能并发实战，见过 Spark DAGScheduler、TensorFlow XLA、RDMA CM 的全套依赖调度实现。  
> **前置**：请先阅读 `design_task_scheduler_master.md`（Part A 设计）。本文假定你已理解线程池、condition_variable、原子操作的基本用法。

---

## 目录

1. [从 Part A 到 Part B：问题升级了什么](#1-从-part-a-到-part-b问题升级了什么)
2. [核心数据结构设计](#2-核心数据结构设计)
3. [API 语义精解](#3-api-语义精解)
4. [状态机：一个 BulkLaunch 的一生](#4-状态机一个-bulklaunch-的一生)
5. [六条并发关键路径逐行分析](#5-六条并发关键路径逐行分析)
6. [双锁设计：为什么需要两把锁](#6-双锁设计为什么需要两把锁)
7. [七个设计决策点（含争议与取舍）](#7-七个设计决策点含争议与取舍)
8. [潜在风险与 Bug 分析](#8-潜在风险与-bug-分析)
9. [与工业级调度器的对比](#9-与工业级调度器的对比)
10. [调试与验证清单](#10-调试与验证清单)

---

## 1. 从 Part A 到 Part B：问题升级了什么

### 1.1 核心约束变化对照


| 维度      | Part A       | Part B                    |
| ------- | ------------ | ------------------------- |
| 调用语义    | `run()` 同步阻塞 | `runAsyncWithDeps()` 立即返回 |
| 批次关系    | 串行、不重叠       | 并发、DAG 依赖                 |
| "完成"定义  | run() 返回即完成  | `sync()` 等待所有批次           |
| 状态静止点   | 每次 run() 结束  | 只有 sync() 之后              |
| 最难的并发问题 | 空转 vs 睡眠权衡   | **依赖图的并发安全更新**            |


### 1.2 新增的语义契约（来自 README）

```
1. runAsyncWithDeps(r, N, deps) 必须立即返回（不等任何任务执行）
2. 一个 launch 的任意 task 必须等 deps 中「所有 launch 的所有 task」全部完成后才能开始
3. sync() 等待「截至此刻提交的所有 launch」的所有 task 完成
4. run() 与 runAsyncWithDeps() 不混用（作业保证，简化实现）
5. deps 中的 TaskID 必须是之前调用返回的合法 ID（d < current_id）
```

**约束 4 最重要**：意味着 `run() = runAsyncWithDeps(no_deps) + sync()`，无需为 `run()` 单独维护同步路径。

### 1.3 新增的核心问题

```
问题 1：如何知道一个 launch 的所有前驱「都」完成了？
→ 每个 launch 维护一个 unfinished_deps 计数器，前驱完成时 fetch_sub，到 0 时入就绪队列。

问题 2：如何不阻塞 runAsyncWithDeps() 的调用者？
→ 只注册图结构，立即返回；实际调度推迟给 worker 线程。

问题 3：sync() 如何知道"所有任务"完成了？
→ 全局 outstanding_tasks_ 计数器：每提交 1 个 task 加 1，每完成 1 个 task 减 1，到 0 即完成。
```

---

## 2. 核心数据结构设计

### 2.1 BulkLaunch：一个批次的完整运行时状态

```cpp
struct BulkLaunch {
    IRunnable*        runnable;       // 任务执行体（只读）
    int               num_tasks;      // 该批次任务总数 N（只读）
    std::atomic<int>  next_task{0};   // 工作窃取游标：下一个待执行的 task_id
    std::atomic<int>  remaining{0};   // 还剩几个 task 未执行（倒计时）
    std::atomic<int>  unfinished_deps{0}; // 还有几个前驱批次未完成
};
```

**字段分工**：

- `next_task`：解决"哪个 task 由我执行"（水平扩展，worker 间竞争）
- `remaining`：解决"何时宣告本批次完成"（用于触发后继解锁）
- `unfinished_deps`：解决"我是否可以开始"（用于阻止提前执行）

**为什么 `next_task` 和 `remaining` 分开而不合并？**

因为它们回答的是两个不同问题：

- `next_task = N` 只表示"号已经抢完"，但最后一个 task 可能还没执行完
- `remaining = 0` 才表示"所有 task 执行结束"

如果合并，无法区分"号抢完"和"任务完成"，会导致提前触发后继解锁。

### 2.2 任务图结构

```cpp
// in TaskSystemParallelThreadPoolSleeping:
std::vector<std::unique_ptr<BulkLaunch>> launches_;   // 下标 = TaskID
std::vector<std::vector<TaskID>>         successors_;  // successors_[d] = 依赖 d 的后继集合
std::deque<TaskID>                       ready_queue_; // unfinished_deps==0 的 launch id
```

**数据模型（可视化）**：

```
TaskID:    0        1        2        3        4
           ┌──┐   ┌──┐   ┌──┐   ┌──┐   ┌──┐
           │L0│──►│L1│   │L2│──►│L3│──►│L4│
           └──┘   └──┘   └──┘   └──┘   └──┘
             └─────────────►│L3│
                            └──┘

successors_[0] = [1, 3]   // L0 完成后通知 L1 和 L3
successors_[2] = [3]       // L2 完成后通知 L3
successors_[1] = [4]
successors_[3] = [4]

L3.unfinished_deps = 2    // 需要 L0 和 L2 都完成
L4.unfinished_deps = 2    // 需要 L1 和 L3 都完成
```

**为什么用 `successors_`（反向边）而不是 `predecessors_`（正向边）？**

因为触发时机是"前驱完成"，需要找"谁依赖我" → 反向边天然适合。

正向边（predecessors）适合"我想开始时，检查我的依赖是否都完成"，但这需要轮询，效率低。

### 2.3 全局计数器

```cpp
std::atomic<int> outstanding_tasks_;  // 所有已提交但未完成的 task 总数
```

这是 `sync()` 的唯一等待条件。它与"批次完成"是不同的概念：

```
outstanding_tasks_ = Σ(每个已提交 launch 的 remaining)

sync() 等待：outstanding_tasks_ == 0
          = 所有已提交 launch 的 remaining 全为 0
          = 所有 task 均已执行完毕
```

---

## 3. API 语义精解

### 3.1 runAsyncWithDeps() 的逻辑

```
输入：runnable, N, deps[]
输出：新 launch 的 TaskID

操作（在 graph_mtx_ 内原子完成）：
  1. 分配新 BulkLaunch 节点，id = launches_.size()
  2. 过滤 deps：只保留「注册时仍未完成（remaining > 0）」的前驱
     → 已完成的 dep 不需要等待，直接忽略
  3. 对保留的前驱 d：successors_[d].push_back(id)（建反向边）
  4. unfinished_deps = 过滤后剩余数量（unresolved）
  5. outstanding_tasks_ += N（新增 N 个待执行任务）
  6. 若 unresolved == 0：push_back(id) 到 ready_queue_（可以立即调度）
  7. 若 N == 0 && unresolved == 0：立即 complete（触发后继解锁）
  
  释放锁后：notify cv_work_（不在锁内 notify，避免阻塞 worker）
```

**关键设计**：「已完成的 dep 不占 unfinished_deps」是一个重要优化，避免了"注册时需要扫描所有 dep 的状态"的 O(N²) 问题。

### 3.2 sync() 的语义

```cpp
void sync() {
    std::unique_lock<std::mutex> lk(sync_mtx_);
    cv_sync_.wait(lk, [this] {
        return outstanding_tasks_.load(std::memory_order_acquire) == 0;
    });
}
```

**等待的是什么**：`outstanding_tasks_ == 0`，即所有已提交 task 均已被 `runTask()` 执行完毕。

**注意**：sync() 使用的是独立的 `sync_mtx_`，不是 `graph_mtx_`。这使得 worker 在执行 task 和更新完成状态时不会被 sync() 的等待者阻塞。（详见第 6 节）

### 3.3 run() = runAsyncWithDeps + sync()

```cpp
void run(IRunnable* r, int N) {
    std::vector<TaskID> no_deps;
    runAsyncWithDeps(r, N, no_deps);
    sync();
}
```

这个实现的代价：每次 `run()` 都要走完整的 async 路径（建图、outstanding_tasks_ 增减），比 Part A 的轻量同步路径略重。对 `super_super_light` 测试（大量极轻任务的 run() 调用）有轻微影响，但符合"先正确后优化"原则。

---

## 4. 状态机：一个 BulkLaunch 的一生

```
                         unresolved > 0
                         ┌─────────────────────────┐
runAsyncWithDeps()       │                         │
      │                  │     ┌─────────────┐     │ 前驱 complete_bulk
      ▼                  │     │  WAITING    │─────┘ 且 unfinished_deps 减到 0
  REGISTERED ────────────┘     └──────┬──────┘
      │                               │ push to ready_queue_
      │ unresolved == 0               ▼
      └──────────────────────►  ┌─────────────┐
                                │   READY     │ ← 在 ready_queue_ 里
                                └──────┬──────┘
                                       │ worker 从队列取出
                                       ▼
                                ┌─────────────┐
                                │   RUNNING   │ worker 执行 runTask
                                └──────┬──────┘
                                       │ remaining 减到 0
                                       ▼
                                ┌─────────────┐
                                │    DONE     │───► complete_bulk_launch()
                                └─────────────┘     触发后继的 unfinished_deps--
```

### 4.1 WAITING → READY 的触发机制

```
事件：前驱批次 P 执行完最后一个 task

complete_bulk_launch(P):
  持 graph_mtx_
  for each succ in successors_[P]:
    if succ.unfinished_deps.fetch_sub(1) == 1:  ← 减到 0
      ready_queue_.push_back(succ)
  cv_work_.notify_all()
```

**为什么 fetch_sub 用 `acq_rel`？**

- `acquire`：确保我看到 P 的所有任务都完成了（happens-before 关系）
- `release`：确保我对 unfinished_deps 的修改对其他观察者可见

### 4.2 RUNNING 的并发细节

一个 RUNNING 状态的 BulkLaunch 可以被**多个 worker 并发执行**：

```
Worker-1: start = next_task.fetch_add(chunk) → [0, chunk)
Worker-2: start = next_task.fetch_add(chunk) → [chunk, 2*chunk)
Worker-3: start = next_task.fetch_add(chunk) → [2*chunk, 3*chunk)
...
```

每个 worker 独立执行自己领到的那段 task_id，`remaining` 被所有 worker 并发递减。

---

## 5. 六条并发关键路径逐行分析

### 路径 1：注册一个新 Launch（无依赖）

```cpp
// runAsyncWithDeps() 内，graph_mtx_ 已持
launches_.emplace_back(new BulkLaunch(r, N));  // ① 分配节点
id = launches_.size() - 1;
successors_.resize(launches_.size());           // ② 扩展后继表

// deps 为空，unresolved = 0
me->unfinished_deps.store(0);
outstanding_tasks_.fetch_add(N, relaxed);       // ③ 增加全局计数

ready_queue_.push_back(id);                     // ④ 直接入就绪队列
schedule_now = true;

// 释放 graph_mtx_
cv_work_.notify_all();                          // ⑤ 在锁外 notify
```

**关键顺序**：`outstanding_tasks_` 在入队之前增加。如果先入队后增加，可能 worker 执行完再减，sync() 看到 0 但计数还没加上来，误以为完成了。

### 路径 2：注册一个新 Launch（有未完成的依赖）

```cpp
for (TaskID d : deps) {
    BulkLaunch* dep = launches_[d].get();
    if (dep->remaining.load(acquire) > 0) {     // ① 检查前驱是否还活着
        ++unresolved;
        successors_[d].push_back(id);            // ② 建反向边
    }
    // 已完成的 dep：remaining == 0，直接跳过，不建边
}

me->unfinished_deps.store(unresolved, release);  // ③ 设置计数

// unresolved > 0：不入 ready_queue_，等待前驱触发
```

**为什么"已完成的 dep"不需要建边？**

如果 dep 已完成（remaining == 0），complete_bulk_launch(dep) 已经执行完了，不会再调用。如果建了边，这条边永远不会被遍历，但会浪费内存。

**竞争分析**（持 graph_mtx_ 时）：

```
在 graph_mtx_ 保护下检查 dep->remaining：
  情况 A：remaining > 0 → 建边，等待触发
  情况 B：remaining == 0 → 不建边，dep 已完成

情况 A 的子情况：建完边后，dep 的最后一个 task 才完成
  → complete_bulk_launch(dep) 需要 graph_mtx_，此时被阻塞
  → 等我们释放 graph_mtx_ 后，complete_bulk_launch(dep) 执行
  → 它遍历 successors_[dep]，找到我们刚建的边，对 id 的 unfinished_deps--
  → 正确触发 ✓
```

### 路径 3：Worker 执行 Task（核心热路径）

```cpp
// 1. 从就绪队列取 lid（持 graph_mtx_，耗时极短）
{
    std::unique_lock<std::mutex> lk(graph_mtx_);
    cv_work_.wait(lk, [this] { return stop_ || !ready_queue_.empty(); });
    lid = ready_queue_.front();
    ready_queue_.pop_front();
}

// 2. 锁外执行（主要工作，不阻塞其他 worker）
BulkLaunch& L = *launches_[lid];
const int n = L.num_tasks;

int chunk = clamp((n + num_workers_ - 1) / num_workers_, 1, 32);

while (true) {
    const int start = L.next_task.fetch_add(chunk, relaxed);  // 无锁抢号
    if (start >= n) {
        // 号抢完了，但可能别的 worker 还在执行
        if (L.remaining.load(acquire) > 0) {
            // 把 lid 推回队列，让其他空闲 worker 接手（实际是避免当前线程干等）
            std::lock_guard<std::mutex> lk(graph_mtx_);
            ready_queue_.push_back(lid);
            cv_work_.notify_all();
        }
        break;
    }
    
    const int lim = std::min(start + chunk, n);
    for (int task_id = start; task_id < lim; ++task_id) {
        L.runnable->runTask(task_id, n);              // 执行任务（锁外）
        
        if (L.remaining.fetch_sub(1, acq_rel) == 1) {
            complete_bulk_launch(lid);                 // 本批最后一个 task 完成
        }
        if (outstanding_tasks_.fetch_sub(1, acq_rel) == 1) {
            std::lock_guard<std::mutex> slk(sync_mtx_);
            cv_sync_.notify_all();                     // 所有 task 全部完成
        }
    }
}
```

### 路径 4：触发后继批次就绪

```cpp
void complete_bulk_launch_unlocked(TaskID launch_id) {
    // 假定已持 graph_mtx_
    for (TaskID succ : successors_[launch_id]) {
        BulkLaunch* s = launches_[succ].get();
        if (s->unfinished_deps.fetch_sub(1, acq_rel) == 1) {  // 减到 0
            ready_queue_.push_back(succ);                        // 入就绪队列
        }
    }
    cv_work_.notify_all();  // 唤醒 worker
}
```

**注意**：即使没有任何 successor 入队，这里也会 `notify_all()`。这是一个轻微的过度唤醒，但对正确性无害。可以优化为只在 `ready_queue_` 非空时 notify。

### 路径 5：零任务 Launch 的处理

```cpp
// 在 runAsyncWithDeps() 中：
if (num_total_tasks == 0) {
    if (unresolved == 0) {
        complete_bulk_launch_unlocked(id);  // 立即触发后继（不需要 worker 参与）
    }
    // else: 入 WAITING 状态，等待前驱完成后触发
}
```

```cpp
// 在 worker_loop() 中：
if (n == 0) {
    complete_bulk_launch(lid);  // worker 从队列取出 0-task launch，立即触发后继
    continue;
}
```

**为什么需要两处处理？**

- `unresolved == 0`：注册时可以立刻处理，不需要过 ready_queue_
- `unresolved > 0`：需要等前驱完成，前驱会把它推入 ready_queue_，worker 再处理

### 路径 6：sync() 等待完成

```cpp
void sync() {
    std::unique_lock<std::mutex> lk(sync_mtx_);
    cv_sync_.wait(lk, [this] {
        return outstanding_tasks_.load(acquire) == 0;
    });
}
```

**简洁是因为有 outstanding_tasks_ 做全局计数**：不需要枚举所有 launch 的状态，一个数字搞定。

---

## 6. 双锁设计：为什么需要两把锁

### 6.1 两把锁的分工

```
graph_mtx_：  保护图结构（launches_, successors_, ready_queue_）
              被持有：注册 launch 时、worker 取任务时、complete_bulk_launch 时
              持有时长：极短（几个指针操作）

sync_mtx_：   仅用于 cv_sync_ 的等待/通知
              被持有：notify cv_sync_ 时（极短）、sync() 等待时（可能很长）
```

### 6.2 为什么不能只用一把锁？

**如果用 graph_mtx_ 代替 sync_mtx_**：

```
死锁场景：
  sync() 持 graph_mtx_ 等待 cv_sync_（等 outstanding_tasks_ == 0）
  worker 执行完最后一个 task，需要持 graph_mtx_ 调 complete_bulk_launch()
  → 但 sync() 持着 graph_mtx_，worker 被阻塞
  → outstanding_tasks_ 永远不会到 0
  → sync() 永远不会唤醒
  → 死锁！
```

**正确方案**：sync() 不持 graph_mtx_，有独立的 sync_mtx_ 只保护 cv_sync_。

```
无死锁：
  sync() 持 sync_mtx_ 等待 cv_sync_
  worker 执行完最后一个 task，持 graph_mtx_ complete_bulk_launch（不阻塞）
  outstanding_tasks_ 减到 0，worker 持 sync_mtx_ notify cv_sync_
  sync() 唤醒，返回 ✓
```

### 6.3 两锁的加锁顺序（防死锁原则）

```
规则：代码中只有一处同时持两把锁的情形，且顺序固定：
  graph_mtx_ → sync_mtx_（先持图锁，再持同步锁）

实际代码：worker 在持 graph_mtx_ 执行 complete 时，
          在锁外先释放 graph_mtx_，再单独持 sync_mtx_ notify。

→ 实际上没有同时持两把锁的代码路径，不存在死锁风险。
```

---

## 7. 七个设计决策点（含争议与取舍）

### 决策 1：`outstanding_tasks_` 增加的时机

```cpp
// 在 graph_mtx_ 内：
outstanding_tasks_.fetch_add(num_total_tasks, std::memory_order_relaxed);

// 入队在同一临界区内
if (unresolved == 0) {
    ready_queue_.push_back(id);
}
```

**为什么增加在入队之前？**

如果先入队，worker 可能在同一毫秒内取出并执行完所有 task，outstanding_tasks_ 此时还没增加，sync() 看到 0 误认为完成。

先增加后入队，保证了 outstanding_tasks_ 在任何 worker 看到任务之前已经反映了这批任务的存在。

**为什么可以用 `relaxed`？**

因为这个 fetch_add 本身在 graph_mtx_ 的临界区内，mutex 提供了必要的内存顺序。

### 决策 2："已完成的 dep 不建边"的正确性

```cpp
if (dep->remaining.load(std::memory_order_acquire) > 0) {
    // 建边
}
```

**竞争窗口分析**：

```
时刻 T1：graph_mtx_ 被我持有
时刻 T2：worker 完成了 dep 的最后一个 task，尝试调 complete_bulk_launch(dep)
         → complete_bulk_launch 需要 graph_mtx_，被阻塞

时刻 T1 内：
  dep->remaining 检查：如果此时为 1（即将变 0），我们建了边
  unresolved++
  successors_[dep].push_back(id)
  me->unfinished_deps = 1

时刻 T3：我释放 graph_mtx_
时刻 T4：worker 获得 graph_mtx_，执行 complete_bulk_launch_unlocked(dep)
         遍历 successors_[dep]，找到 id，unfinished_deps-- → 0
         id 入 ready_queue_ ✓
```

**结论**：即使在检查 remaining 的瞬间值为 1，也能正确处理，因为"建边"和"complete"在锁的保护下是串行的。

### 决策 3：notify 在锁外 vs 锁内

```cpp
// runAsyncWithDeps() 中：
{
    std::lock_guard<std::mutex> lk(graph_mtx_);
    // ... 修改 ready_queue_ ...
}  // 释放锁
if (schedule_now) {
    cv_work_.notify_all();  // 锁外 notify
}
```

**锁外 notify 是否安全？**

是的。`condition_variable` 的 `wait()` 将"持锁检查谓词"和"进入 futex_wait"设计为原子操作。

```
正确时序：
  Worker：持 graph_mtx_，检查谓词 ready_queue_.empty() → true，
          原子地：释放锁 + 进入 futex_wait

  Caller：持锁修改 ready_queue_，释放锁，notify_all()
  
  两条路径被 graph_mtx_ 串行化：
  - 如果 worker 先进 futex_wait，notify_all 会唤醒它
  - 如果 caller 先释放锁，worker 检查谓词时 ready_queue_ 已非空，直接返回（不睡）
```

**锁外 notify 的优点**：避免 worker 唤醒后立即尝试获锁被阻塞（被唤醒后的第一件事是重新获锁），减少不必要的竞争。

### 决策 4：ready_queue_ 使用 deque 而非 queue

```cpp
std::deque<TaskID> ready_queue_;
```

**deque 的优势**：

- 支持 `push_back`（正常入队）和 `push_back`（push-back-after-overshoot）
- 支持 `pop_front`（FIFO，保证先注册的 launch 优先执行）
- 内存分段分配，不需要 resize 时的整体拷贝

**FIFO 语义**：先注册的 launch 优先执行，有利于 DAG 的拓扑序推进（减少"半完成批次"占用内存的时间）。

### 决策 5："号抢完但 remaining>0"时把 lid 推回队列

```cpp
if (start >= n) {
    if (L.remaining.load(acquire) > 0) {
        std::lock_guard<std::mutex> lk(graph_mtx_);
        ready_queue_.push_back(lid);
        cv_work_.notify_all();
    }
    break;
}
```

**这个策略的本质**：当前 worker 没有任务可执行，但批次还没完成，把 lid 放回队列让其他空闲 worker 接手执行"尾巴任务"（实际上尾巴已经被其他 worker 持有，此处是让空闲 worker 重新检查）。

**与 Part A C 阶段的比较**：


|      | Part A（三阶段 ABC）    | Part B（push-back）          |
| ---- | ------------------ | -------------------------- |
| 思路   | 号抢完后进 C 阶段睡眠，等批次结束 | 把 lid 推回队列，worker 继续轮询     |
| 开销   | 睡眠唤醒的系统调用延迟        | 频繁的 lock/unlock + pop/push |
| 适用场景 | 单批次，批次较重           | 多批次，需要快速响应新批次              |
| 尾延迟  | 低（最后一个任务完成即唤醒）     | 可能高（多个 worker 反复 push/pop） |


**潜在问题**：对于 `ping_pong_unequal`（任务执行时间差异极大），可能有多个 worker 反复推回再弹出同一个 lid，造成 busy-wait 式的热路径。见第 8 节分析。

### 决策 6：`launches_` 使用 `unique_ptr` 而非直接对象

```cpp
std::vector<std::unique_ptr<BulkLaunch>> launches_;
```

**为什么不用 `std::vector<BulkLaunch>`？**

`BulkLaunch` 包含多个 `std::atomic` 成员，而 `atomic` 类型**不可移动、不可拷贝**（C++ 规范限制）。`std::vector` 扩容时需要移动元素，这对 atomic 成员非法。

`unique_ptr` 方案：vector 存储指针，扩容只移动指针（指针可以移动），BulkLaunch 对象本身不移动，其地址稳定。

### 决策 7：`successors_` 和 `launches_` 同步扩展

```cpp
launches_.emplace_back(new BulkLaunch(r, N));
id = launches_.size() - 1;
successors_.resize(launches_.size());  // 始终与 launches_ 等长
```

**为什么要立即 resize？**

如果不立即 resize，后续代码 `successors_[d].push_back(id)` 或 `complete_bulk_launch_unlocked` 访问 `successors_[launch_id]` 可能越界。

通过保持 `successors_.size() == launches_.size()` 的不变量，可以安全地用 launch_id 作为下标。

---

## 8. 潜在风险与 Bug 分析

### 风险 1（严重性：中）：push-back 策略引起的"半热旋转"

**场景**：

```
批次 L 有 N=1000 个任务，chunk=32

Worker-1 取到 lid，fetch_add(32) → start=[0,32)，执行
Worker-2 取到 lid，fetch_add(32) → start=[32,64)，执行
...
Worker-16 取到 lid，fetch_add(32) → start=[480,512)，执行
Worker-1 完成后重新循环，从队列取新 lid（但 L 的号已经分配完了）

如果 L.remaining > 0（Worker-16 还没跑完）：
Worker-1 push_back(lid)，notify
Worker-2 同样弹出，overshoot，remaining > 0，push_back，notify
...
→ 多个 worker 反复 pop/push 同一个 lid，直到 Worker-16 完成
```

**实际开销**：每次 push/pop 都需要 lock/unlock，约 50-200ns/次。对于 `ping_pong_unequal` 的长尾任务，可能造成数百微秒的无效开销。

**影响评估**：在正确性上无问题；在性能上对不均匀任务有影响。

**改进方案**（不改变接口）：

```cpp
// 方案 A：push back 之前检查是否有其他 ready 批次
if (start >= n && L.remaining.load(acquire) > 0 && ready_queue_.empty()) {
    // 只有没有其他任务时才 push back（避免多次重复 push）
    ...
}

// 方案 B（Part A 风格）：
// 当前 worker 释放 lid 后，去等待 ready_queue_ 有新内容
// 由 complete_bulk_launch 时 notify_all 唤醒
// 缺点：需要一个"当前 batch 完成"的信号，设计更复杂
```

### 风险 2（严重性：低）：outstanding_tasks_ 与 0-task launch 的语义

**场景**：

```
调用序列：
  id1 = runAsyncWithDeps(r1, 0, [])  // 0-task launch
  id2 = runAsyncWithDeps(r2, 5, [id1])  // 依赖 id1
  sync()
```

`outstanding_tasks_` 的变化：

```
  after id1：fetch_add(0) → outstanding_tasks_ = 0
  after id2：fetch_add(5) → outstanding_tasks_ = 5
  sync() 等待 outstanding_tasks_ == 0
```

0-task 的 id1 不影响 outstanding_tasks_，但它的完成触发了 id2 的就绪。`sync()` 正确等待了 id2 的 5 个 task 完成。

**潜在陷阱**：如果最后一批是 0-task 且所有前驱都已完成：

```
  id3 = runAsyncWithDeps(r3, 0, [id1, id2])  // 最后一批，0 tasks
  sync()
```

此时 outstanding_tasks_ 在 id2 完成后已经变为 0，sync() 可能在 id3 的 `complete_bulk_launch` 触发之前就返回了。但由于 id3 没有任务，这在语义上是正确的——sync() 等待的是"所有已提交的 task 执行完毕"，0-task launch 没有任务，不需要等待。

**结论**：这是正确的行为，但在读代码时容易让人困惑。建议在注释中明确说明 0-task 不增加 outstanding_tasks_。

### 风险 3（严重性：低）：complete_bulk_launch_unlocked 中的多余 notify

```cpp
void complete_bulk_launch_unlocked(TaskID launch_id) {
    for (TaskID succ : successors_[launch_id]) {
        if (s->unfinished_deps.fetch_sub(1, acq_rel) == 1) {
            ready_queue_.push_back(succ);
        }
    }
    cv_work_.notify_all();  // 即使没有 succ 入队，也 notify
}
```

**影响**：每次任何 batch 完成，即使它没有任何后继，也会触发一次 `notify_all()`，唤醒所有睡眠的 worker 仅为了让它们重新检查 ready_queue_（发现是空的，继续睡）。

对于"长链 DAG"（A→B→C→D→...）的测试，这会导致每个节点完成时都有一次无效的全员唤醒。

**简单优化**：

```cpp
void complete_bulk_launch_unlocked(TaskID launch_id) {
    bool any_ready = false;
    for (TaskID succ : successors_[launch_id]) {
        if (s->unfinished_deps.fetch_sub(1, acq_rel) == 1) {
            ready_queue_.push_back(succ);
            any_ready = true;
        }
    }
    if (any_ready) {
        cv_work_.notify_all();  // 只在真的有新任务时 notify
    }
}
```

### 风险 4（严重性：极低）：析构时的双重 notify

```cpp
~TaskSystemParallelThreadPoolSleeping() {
    stop_.store(true, release);
    {
        std::lock_guard<std::mutex> lk(graph_mtx_);
        cv_work_.notify_all();       // 唤醒等待 cv_work_ 的 worker
    }
    {
        std::lock_guard<std::mutex> lk(sync_mtx_);
        cv_sync_.notify_all();       // 唤醒等待 cv_sync_ 的 sync()
    }
    for (auto& w : workers_) w.join();
}
```

**析构时在持锁情况下 notify 是否必要？**

技术上，`notify_all()` 不需要在锁内调用。这里在锁内 notify 是为了确保 worker 的谓词检查能立即看到 `stop_ = true`（虽然 memory_order_release 已经保证了这点）。属于"防御性编码"，无害。

---

## 9. 与工业级调度器的对比

### 9.1 与 TensorFlow/PyTorch 执行引擎的类比


| 概念                   | 本实现              | TF/PT 执行引擎            |
| -------------------- | ---------------- | --------------------- |
| BulkLaunch           | Op（算子）节点         | Kernel/Op             |
| successors_          | 数据流边             | Tensor 依赖边            |
| unfinished_deps      | ref_count        | pending_count         |
| ready_queue_         | 就绪队列             | Executor::ready_queue |
| complete_bulk_launch | PropagateOutputs | 通知下游                  |
| outstanding_tasks_   | 全局运行数            | global_step_count     |
| sync()               | Session.run 阻塞   | future.get()          |


### 9.2 与 RDMA 数据面的映射

```
RDMA 发送侧（Post WR）  ←→  runAsyncWithDeps()：立即入 SQ，不等完成
RDMA 轮询（Poll CQ）    ←→  worker_loop()：轮询完成，触发后继
RDMA QP 依赖（RNR）     ←→  unfinished_deps：前驱未就绪时阻止发送
Work Completion（WC）   ←→  complete_bulk_launch：完成事件触发链式通知
```

### 9.3 本实现未覆盖的工业级特性


| 特性                  | 本实现     | 工业级                       |
| ------------------- | ------- | ------------------------- |
| 优先级调度               | 无（FIFO） | 多级队列，高优先先执行               |
| 任务窃取（Work Stealing） | 无（全局队列） | per-thread deque，Cilk 风格  |
| 资源限额                | 无       | 内存/显存配额管理                 |
| 取消                  | 无       | CancellationToken         |
| 执行超时                | 无       | deadline-aware scheduling |
| NUMA 感知             | 无       | 本地内存优先分配                  |
| 动态依赖                | 无       | 运行时动态图修改                  |


---

## 10. 调试与验证清单

### 10.1 关键边界测试

```bash
# 零任务 launch
cd part_b && ./runtasks -n 8 simple_test_async

# 长链依赖（A→B→C→...→Z）
cd part_b && ./runtasks -n 8 super_light_async

# 扇入（多个前驱汇聚到一个后继）
cd part_b && ./runtasks -n 8 fan_in_async

# 钻石依赖（A→B,C→D）
cd part_b && ./runtasks -n 8 strict_diamond_deps_async

# 大规模 DAG
cd part_b && ./runtasks -n 8 strict_graph_deps_large_async
```

### 10.2 并发正确性验证（手动检查列表）

- `runAsyncWithDeps` 是否真的立即返回？（在返回前 sleep 100ms 验证）
- `sync()` 是否等待了所有批次？（在 sync 之前加 sleep 验证时序）
- 零依赖的批次是否立即进入 ready_queue_？
- 前驱批次完成后，后继是否被正确唤醒？
- 多个前驱的 AND 语义是否正确？（unfinished_deps 减到 0 才入队）
- 析构时若有任务在执行，是否等任务完成后再 join？
- 0-task 批次的完成是否正确传播给后继？

### 10.3 性能基准

```bash
# 跑全部异步测试
cd part_b && python3 ../tests/run_test_harness.py -a

# 只跑特定测试
cd part_b && python3 ../tests/run_test_harness.py -t strict_diamond_deps_async reduction_tree_async
```

### 10.4 常见死锁场景的 gdb 诊断

```bash
# 编译为可调试版本（修改 Makefile 加 -g -O0）
# 运行后 Ctrl+C 进入 gdb
gdb -p $(pgrep runtasks)
(gdb) info threads          # 查看所有线程状态
(gdb) thread apply all bt   # 所有线程的调用栈

# 典型死锁征兆：
# - 所有 worker 都停在 __futex_wait（等待 cv_work_）
# - sync() 也停在 __futex_wait（等待 cv_sync_）
# - 没有任何线程在执行 runTask
# → 通常是 outstanding_tasks_ 计数不对或 notify 路径有问题
```

---

## 附录：实现骨架（伪代码级）

```cpp
// Part B 核心骨架（不是完整代码，仅展示结构）

TaskID runAsyncWithDeps(r, N, deps):
  LOCK graph_mtx_:
    id = append new BulkLaunch(r, N)
    for d in deps:
      if launches[d].remaining > 0:
        successors[d].append(id)
        unresolved++
    launches[id].unfinished_deps = unresolved
    outstanding_tasks += N
    if N == 0 and unresolved == 0:
      complete_bulk_launch_unlocked(id)  // 0-task 立即完成
    elif unresolved == 0:
      ready_queue.push_back(id)
      schedule_now = true
  UNLOCK
  if schedule_now: cv_work.notify_all()
  return id

worker_loop():
  while true:
    LOCK graph_mtx_: wait until stop or ready_queue not empty; pop lid
    UNLOCK
    
    if launches[lid].num_tasks == 0:
      complete_bulk_launch(lid); continue
    
    chunk = clamp(N/workers, 1, 32)
    loop:
      start = launches[lid].next_task.fetch_add(chunk)
      if start >= N:
        if remaining > 0: push_back(lid); notify
        break
      for task_id in [start, min(start+chunk, N)):
        runTask(task_id, N)
        if remaining.fetch_sub(1) == 1: complete_bulk_launch(lid)
        if outstanding.fetch_sub(1) == 1: notify cv_sync_

complete_bulk_launch_unlocked(id):
  // 已持 graph_mtx_
  for succ in successors[id]:
    if succ.unfinished_deps.fetch_sub(1) == 1:
      ready_queue.push_back(succ)
  cv_work.notify_all()

sync():
  LOCK sync_mtx_: wait until outstanding_tasks == 0
  UNLOCK
```

---

*文档版本：v1.0 | 对应代码：Part B 已完成版本*