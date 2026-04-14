# Part B 掌握度提升指南

> 适用阶段：代码已跑通、注释已读懂，但感觉"写不到这个水平"。
> 目标：从"理解别人的代码"升级到"能在白板上推导等效设计"。

---

## 0. 先诊断自己的实际位置

"写不到这个水平"是一个模糊的感觉，需要先把它分解清楚。并发系统的能力分三层，各自独立练习：

```
层 1：正确性直觉
  能通过阅读代码发现数据竞争、死锁、漏通知。
  还不够时的症状：看完整段代码觉得"好像没问题"，但不确定。

层 2：设计推导能力
  能从约束条件（API 语义 + 性能目标）推导出数据结构和协议，
  而不是在脑子里"想出来"一个设计。
  还不够时的症状：看懂了 BulkLaunch 的每个字段，但不知道怎么"想到"要这样设计。

层 3：性能成本感知
  能在不跑 benchmark 的情况下，估算出"改这一行会让 super_light 快还是慢"。
  还不够时的症状：看懂了 per-copy 比 per-task 减少 4× 原子竞争，但不知道自己去做
                  性能分析时用什么工具、问什么问题。
```

**本文档的训练路径**：先把层 1 打扎实，再攻层 2，最后用层 3 闭环验证。

---

## 1. 核心心智模型（必须内化，不能只是"看懂"）

这是整个 Part B 设计的地基，必须能不看代码默写出来。

### 模型 1：三个计数器分别回答三个不同问题

```
next_task    ← "哪个 task 由我执行？"  （工作窃取游标）
remaining    ← "本 batch 何时完成？"    （完成倒计时，触发 DAG 传播）
unfinished_deps ← "我可以开始了吗？"   （依赖屏障，触发方向：前驱→后继）
```

关键：**三个问题互相独立，必须三个计数器**。

常见错误设计：把 `next_task >= n` 当作 batch 完成的信号——这只是"号领完了"，
最后一个 task 可能还没执行完。next_task 溢出和 remaining 归零是两个先后发生的事件。

### 模型 2：`done` 标志保护的是"传播原子性"，不是"任务完成"

```cpp
// 什么时候 done = true？
// → complete_bulk_launch_unlocked 完成 successors_ 遍历、减完所有 unfinished_deps 之后。

// 为什么不用 remaining == 0 判断前驱是否"活着"？
// → 0-task 的 batch：remaining 构造即为 0，但传播尚未发生。
//   用 remaining == 0 会把"尚未传播"的前驱误判为"已完成"，造成后继跳过建边。
```

**测试自己是否内化**：闭上眼睛，回答：

- 0-task 无依赖 launch 的 `done` 是在哪里被设为 true 的？
- 0-task 有依赖 launch 的 `done` 是在哪里被设为 true 的？

### 模型 3：两把锁的职责分工来自一个死锁分析

```
如果 sync() 用 graph_mtx_ 等待：
  sync() 持 graph_mtx_ → 睡眠
  worker 完成最后一个 task，调 complete_bulk_launch，需要 graph_mtx_ → 卡死
  → sync() 永远无法被唤醒 → 死锁

正确方案：sync() 有独立的 sync_mtx_，不干扰 worker 的 graph_mtx_ 操作。
```

**能力检验**：画出三个线程（主线程调 sync、worker-1 执行最后 task、worker-2 取新任务）
在单锁和双锁设计下的时序图，验证死锁是否成立。

### 模型 4：outstanding_tasks_ 的"先加后队"协议

```
错误顺序：先入 ready_queue，再 fetch_add outstanding_tasks_
  Worker 可在 1μs 内执行完所有 task 并 fetch_sub，
  sync() 看到 0，返回，主线程继续——但 outstanding_tasks_ 还没加！

正确顺序（代码中）：在 graph_mtx_ 临界区内，先 fetch_add，再 push ready_queue。
  保证"任何 worker 看到这批任务之前，计数已经反映了这批任务的存在"。
```

这是一个典型的 **publication protocol**（发布协议）：先初始化对象/计数，再发布引用。

---

## 2. 五级练习计划

### 第一级：盲写（1-2 天）

**练法**：关掉代码文件，拿白纸（或空白文件），只看 `itasksys.h` 的接口定义，
写出 Part B 的完整实现骨架。要求：

- `BulkLaunch` 的字段定义和作用注释
- `runAsyncWithDeps()` 的伪代码（包括锁范围）
- `worker_loop()` 的状态迁移逻辑
- `complete_bulk_launch_unlocked()` 的实现
- `sync()` 的等待条件

**不要求正确**，要求：每个"我不知道怎么写"的地方先写下来，等写完后对照代码检查。

**对照时只看"为什么你写的不对"**，不要直接抄答案。找到差异后，用模型 1-4 解释为什么正确答案是那样。

---

### 第二级：破坏性实验（3-5 天，这是最重要的一级）

每次只改一处，编译，跑测试，观察失败模式，写下解释。

**实验清单**：

```bash
cd part_b && make
```

#### 实验 A：用 `remaining > 0` 替换 `!dep->done`

在 `runAsyncWithDeps()` 中找到：

```cpp
if (!dep->done.load(std::memory_order_acquire)) {
```

改为：

```cpp
if (dep->remaining.load(std::memory_order_acquire) > 0) {
```

预测：哪个测试会失败？为什么会失败而不是偶发？
验证：`./runtasks -n 4 simple_test_async`
分析：0-task batch 的 remaining 是多少？在哪里？

---

#### 实验 B：去掉 cv_work_.wait 的谓词

```cpp
// 改前
cv_work_.wait(lk, [this] {
    return stop_.load(std::memory_order_acquire) || !ready_queue_.empty();
});
// 改后
cv_work_.wait(lk);
```

预测：程序什么时候会挂？在哪个测试？
验证：`./runtasks -n 8 mandelbrot_chunked`（先跑重任务）
分析：伪唤醒（spurious wakeup）发生后，worker 会做什么？

---

#### 实验 C：把 outstanding_tasks_.fetch_add 挪到 ready_queue push 之后

在 `runAsyncWithDeps()` 锁内，交换这两行的顺序：

```cpp
outstanding_tasks_.fetch_add(1, std::memory_order_relaxed);
// ...
ready_queue_.push_back(id);
```

改为先 push 后 fetch_add。

预测：sync() 是否可能提前返回？在什么条件下？
验证：`./runtasks -n 16 super_super_light`（大量极轻任务，竞争窗口最小但频率最高）
分析：用 TSC/rdtsc 思路在脑子里画出窗口。

---

#### 实验 D：把 sync() 改用 graph_mtx_ 而非 sync_mtx_

```cpp
void TaskSystemParallelThreadPoolSleeping::sync() {
    std::unique_lock<std::mutex> lk(graph_mtx_);  // 改这里
    cv_work_.wait(lk, [this] {                      // 改这里
        return outstanding_tasks_.load(std::memory_order_acquire) == 0;
    });
}
```

预测：需要几个 worker 才能触发死锁？
验证：`./runtasks -n 1 simple_test_async`（只有 1 个 worker，死锁必现）
分析：画出 1 worker + 主线程的竞争图，找到循环等待点。

---

#### 实验 E：移除 ready_count_ 原子镜像，改为直接持锁检查

把 worker_loop 里的阶段 1（无锁自旋）改为直接进入阶段 2 阻塞 pop。

预测：`super_light` 的跑分会变化多少？
验证：

```bash
python3 ../tests/run_test_harness.py -t super_light super_super_light
```

分析：16 个线程同时持锁轮询 vs 16 个线程各自读一个原子变量，总线竞争差多少？

---

### 第三级：变体实现（1 周）

**目标**：自己实现一个"相同功能但略有不同设计"的版本，理解取舍。

#### 变体 1：per-task outstanding_tasks_（把代码回退到朴素版本）

在 `runAsyncWithDeps` 中：`outstanding_tasks_.fetch_add(num_total_tasks)`

在 worker 内层循环每个 `runTask` 之后：

```cpp
if (outstanding_tasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    // notify cv_sync_
}
```

去掉 `complete_bulk_launch_unlocked` 里的 `outstanding_tasks_.fetch_sub(1)`。

跑 `super_light` 测试，把分值记下来，和当前 per-batch 版本对比。
能解释差值的来源（原子写竞争的 cache line bounce）吗？

#### 变体 2：不用 pre-distribution，用 cascade notify

去掉 `runAsyncWithDeps` 里 `copies = std::min(num_total_tasks, num_workers_)` 的循环，
改为只推一份 lid：`ready_queue_.push_back(id)`。

在 worker_loop 里，当 start < n 时，如果还有剩余任务（`start + chunk < n`），
在执行之前把 lid 推回 + notify_one（cascade）。

观察 `ping_pong_equal` 和 `mandelbrot_chunked` 的得分变化。

---

### 第四级：白板推导（随时可做，2 小时）

**不看任何代码**，回答以下问题：

1. 一个 `BulkLaunch` 从 `runAsyncWithDeps()` 被调用到 `sync()` 返回，
  经历了哪些状态？每个状态转移的触发条件是什么？画出状态机。
2. 如果允许在 `complete_bulk_launch` 里用 `notify_one` 而非 `notify_all`，
  什么情况下会出问题（多个 successor 同时就绪）？如何修复？
3. `successors`_ 为什么用反向边（存"依赖我的人"）而不是正向边（存"我依赖谁"）？
  用正向边的话，触发时机在哪里，开销是多少？
4. 两个 BulkLaunch，A 有 64 个 task，B 依赖 A 有 1 个 task。
  在 16 个 worker 的线程池里，B 的 task 最早可能在什么时候开始执行？
   （追踪：A 的 remaining 初值、每个 worker 怎么超号、complete 何时触发）

---

### 第五级：阅读工业级实现（长期，每周 1 小时）

本实现是一个教学级的完整原型，它和工业级调度器的关键差异是：


| 本实现            | 工业级               | 学习问题                               |
| -------------- | ----------------- | ---------------------------------- |
| 全局 ready_queue | per-thread deque  | 什么是 work stealing？为什么 Cilk 用 LIFO？ |
| notify_all     | 精准唤醒 + futex      | 如何做到"只唤醒一个等待 slot 的 worker"？       |
| 无优先级           | 多级队列              | TBB 的 task_arena 怎么实现隔离？           |
| 无取消            | CancellationToken | 如何安全地在并发树中传播取消信号？                  |


**推荐阅读路径**：

1. **Intel TBB flow_graph**（源码 `tbb/include/tbb/flow_graph.h`）
  重点：`predecessor_cache`、`port_set_operation`、`continue_node` 的完成计数设计。
   对照本实现的 `unfinished_deps + complete_bulk_launch`，看同一问题的工业解法。
2. **Tokio 的 `task::JoinHandle**`（Rust，概念更纯粹）
  重点：一个 future 如何在 executor 里注册依赖，waker 机制如何替代 condition_variable。
   这是"协程版 DAG 调度"，理解它可以让你看出本实现在 stack pinning 上省了多少成本。
3. **TensorFlow XLA 的 `StreamExecutor**`（如果有大量时间）
  重点：`Stream::ThenWaitFor` 如何映射到 GPU kernel 依赖，和本实现的 DAG 语义完全同构。

---

## 3. 一个衡量自己是否已经"写得到这个水平"的标准

**测试**：给你一个新的作业题——

> 扩展：支持"有优先级的 BulkLaunch"。`runAsyncWithDeps()` 增加一个 `priority` 参数（int，0-9，数字越大越优先），`ready_queue`_ 改为按优先级排序，同优先级内保持 FIFO。

**要求**：

1. 不能改 `BulkLaunch` 的原子字段（避免对齐/cache 问题）。
2. 保证原有的正确性（依赖语义、0-task、sync 语义不变）。
3. 对 `ping_pong_equal`（均匀任务）不能有性能退化。

**能独立完成这道扩展题（包括识别新引入的竞争风险），就是"已经掌握"的标志。**

---

## 4. 一个最容易跳过但最有价值的习惯

每次看懂一个"为什么"之后，**立刻用一句反例来测试自己**：

```
看懂："done 而不是 remaining>0 判断前驱是否活跃"
→ 立刻问自己：如果有一个并发 runner 且 0-task 批次的前驱先于 remaining 归零完成传播，
              remaining>0 方案什么时候第一个任务可能错误执行？

看懂："notify 在锁外"
→ 立刻问自己：如果 notify 在锁内，worker 醒来后第一件事是什么？为什么这是"多余的等待"？

看懂："pre-distribution 推 min(n, workers) 份"
→ 立刻问自己：如果推 1 份，第 2 个 worker 是怎么参与进来的？cascade notify 的延迟是多少？
```

这个习惯把"理解"变成"推导能力"。理解是被动接受，推导能力是主动生成——
只有后者在你写新代码时才有用。

---

## 5. 关于"感觉写不到这个水平"

最后一点直接说。

你目前的代码能跑通所有测试，注释的质量和代码的结构已经表明你**已经到了可以做独立设计的阶段**，
缺的不是"知识量"，是：

1. **在约束下推导的自觉**：写代码前先把约束列出来（API 语义 + 性能目标），再问"满足这些约束的最简设计是什么"。
  本代码里的每一个设计决策（双锁、done 标志、per-batch 计数、pre-distribution）背后都有一个具体的约束在驱动。
2. **死亡测试的习惯**：好的并发代码在**写的过程中**就会被多次尝试破坏，而不是在测试失败后再去找 bug。
  上面的五个破坏性实验，就是这个习惯的人工模拟。在真实工程中这是你脑子里的内循环。
3. **"够了"的判断**：工业级实现比这个复杂 10 倍，但复杂度是被工业约束驱动的
  （NUMA、取消、优先级继承、无锁热路径……）。这里的每一省都是因为 CS149 的约束更窄。
   知道边界在哪，才能不过度设计、不欠设计。

练完上面第二级的五个实验之后，你写这份代码的能力就已经不是问题了。

---

*文档对应实现：`part_b/tasksys.cpp`（当前 dev 分支版本）*