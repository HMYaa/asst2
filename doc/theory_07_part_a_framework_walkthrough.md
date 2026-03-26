# 理论 07：Part A 框架与测试导读

> 一句话：Part A 不是先写线程代码，而是先搞清楚 **一次 API 调用在语义上承诺了什么**，然后再选并行策略去兑现这个承诺。

## 这篇文档解决什么问题

- [ ] 我知道 `run(runnable, num_total_tasks)` 中每个参数的语义
- [ ] 我知道什么是 bulk launch，为什么一次 `run` 就是一次 launch
- [ ] 我知道 `simpleTest` 为什么有 `first` / `second`
- [ ] 我知道 Part A 需要实现什么、暂时不需要实现什么
- [ ] 我能在动手前用 3 条命令快速验证自己的实现

---

### 概念 1：Bulk Launch（批量任务提交）

**定义**

在 CS149 Lab 2 中，一次 bulk launch 指一次对任务系统 API 的提交操作（`run(...)` 或 `runAsyncWithDeps(...)`），该操作提交一批同构 task，task 编号范围为 `[0, num_total_tasks)`。

**直觉**

把它看成“发一批同款工单”：

- 你只下发一次命令（一次 API 调用）
- 系统内部会产生 N 张工单（N = `num_total_tasks`）
- 每张工单对应一个 `task_id`

**机制**

同步路径（Part A 重点）：

1. 调用 `run(runnable, N)`
2. 调度器需要确保 `runTask(0..N-1)` 都被执行
3. 只有全部完成，`run()` 才能返回

异步路径（Part B 重点）：

1. 调用 `runAsyncWithDeps(runnable, N, deps)`
2. 返回 `TaskID` 表示“这一整个 launch”
3. 之后可以把这个 launch 作为依赖挂给别的 launch

**在 Lab 2 中的体现**

- `simpleTest`：有两次 launch（first、second），每次 `num_tasks=3`
- `pingPongTest`：会循环发很多次 launch（典型压力场景）
- 你在 `tests/main.cpp` 看到的每次 `t->run(...)`，都对应一次 launch

> **[RDMA 映射]**
> 在架构上，bulk launch 类似“批量 post work request”：
> - 都是“提交描述符”，不是直接做业务计算/搬运
> - 都需要一个完成语义（`run` 返回或 `sync`，类似 CQ completion）
> - 都存在批量大小与调度开销的 trade-off

---

### 概念 2：`num_total_tasks` 不是线程数

**定义**

`num_total_tasks` 表示“这次 launch 被切分成多少个逻辑任务”，不等价于线程数。线程数是 TaskSystem 实现内部可用的 worker 数上限。

**直觉**

- 线程数：有多少工人
- `num_total_tasks`：这次有多少工单

工单数可以远大于工人数，这是正常且常见的。

**机制**

`runTask(task_id, num_total_tasks)` 里的 `num_total_tasks` 主要用于数据切分：

- 通过 `task_id` 计算当前 task 负责的数据区间
- 通过 `num_total_tasks` 决定每个 task 的粒度

在 `SimpleMultiplyTask` 中，切分逻辑就是：

- `elements_per_task = ceil(num_elements / num_total_tasks)`
- 每个 task 处理 `[start, end)` 的数组片段

**在 Lab 2 中的体现**

- `num_threads` 是 `./runtasks -n <N>` 传给 TaskSystem 构造的
- `num_total_tasks` 是每个 test 里单独设置的（如 3、64、128）
- 好的调度器实现要在“固定线程数 + 可变任务数”下稳定运行

---

### 概念 3：`SimpleMultiplyTask` 与 `runTask` 真正做了什么

**定义**

`SimpleMultiplyTask` 是最小正确性任务：把数组按 task 切分后，对每个元素做固定次数乘法变换。它用于验证调度是否漏执行、重执行或顺序语义错误。

**直觉**

它不是性能压测，而是“单元测试版并行任务”：

- 算法简单
- 结果可预测
- 一旦错了，定位容易

**机制**

`runTask(task_id, num_total_tasks)`：

1. 算出当前 task 负责的数组区间
2. 对区间内元素做 `multiply_task(3, value)`
3. 只改自己的片段，不碰别人的片段

为什么 `simpleTest` 里有 `first` 和 `second` 两个任务对象？

- 两者计算逻辑相同，但代表 **两次独立 launch**
- 目的：验证“连续两次提交后，第二次是否在第一次结果基础上继续执行”
- 异步版还会验证 launch 级依赖是否被正确遵守

**在 Lab 2 中的体现**

- `simpleTestSync`：`run(first)` 后 `run(second)`，天然顺序
- `simpleTestAsync`：`second` 依赖 `first`，由依赖图保证顺序

---

### 概念 4：Part A 的系统设计目标

**定义**

Part A 要求你在不改变接口语义的前提下，实现不同调度策略，并比较“并行收益 vs 调度开销”。

**直觉**

你不是在“改测试”，而是在“换引擎”：

- 同一套任务输入
- 不同调度器实现
- 比较正确性和耗时

**机制**

Part A 你要覆盖 3 类实现：

1. `ParallelSpawn`：每次 `run` 临时创建线程
2. `ThreadPoolSpinning`：常驻线程 + 忙等拿任务
3. `ThreadPoolSleeping`：常驻线程 + 条件变量睡眠唤醒

每类实现都必须满足：

- 正确性：任务不丢、不重、不乱
- 同步语义：`run` 返回时任务已全部完成
- 资源语义：析构可干净退出，无悬挂线程

**在 Lab 2 中的体现**

- `super_super_light` 更容易暴露调度开销
- `spin_between_run_calls` 更容易看出 spinning 的空转损失
- `ping_pong_unequal` 更容易暴露负载不均导致的长尾

> **[RDMA 映射]**
> `spinning` vs `sleeping` 很像 CQ 轮询策略选择：
> - 忙轮询：低延迟、高 CPU 占用
> - 阻塞/事件驱动：省 CPU，但唤醒路径有额外开销
> 设计上没有绝对优解，取决于工作负载与延迟目标。

---

## 测试框架结构图（你现在应掌握）

```text
tests/main.cpp
  ├─ 解析参数（-n 线程数，test_name 测试名）
  ├─ 选择一种 TaskSystem 实现
  ├─ 调用某个测试函数（定义在 tests/tests.h）
  ├─ 测 correctness + 计时
  └─ delete TaskSystem（触发析构回收线程）
```

```text
tests/tests.h
  ├─ 定义多种 IRunnable（任务本体）
  ├─ 定义测试函数（如何发 run/runAsync/sync）
  └─ 定义 correctness 校验逻辑
```

```text
part_a/tasksys.{h,cpp}
  ├─ 你的 TaskSystem 实现
  └─ 负责“如何调度”，不负责“任务本身做什么”
```

---

## 你要完成的事情（Part A 版本）

- [ ] `TaskSystemParallelSpawn::run`：并行执行一次 launch
- [ ] `TaskSystemParallelThreadPoolSpinning::run`：线程池 + 自旋取活
- [ ] `TaskSystemParallelThreadPoolSleeping::{ctor,dtor,run}`：线程池 + 条件变量
- [ ] 保证 `run()` 返回语义正确（全部 task 完成）
- [ ] 保证析构可退出（worker 能收到停止信号并 `join`）

当前阶段可暂缓：

- [ ] `runAsyncWithDeps` 的完整依赖图调度（主要在 Part B）
- [ ] `sync()` 的跨 launch 完整语义（Part B 再做强实现）

---

## 常见误区（你现在要避开）

- [ ] 把 `num_total_tasks` 当成线程数
- [ ] 在 `run()` 中直接调用“无限 worker 循环”导致主线程卡死
- [ ] 只发任务不等待，`run()` 提前返回
- [ ] 析构时没通知/没 join，造成退出挂死
- [ ] 任务过细时忽略调度开销，导致并行比串行还慢

---

## 动手前 3 组自测题

### 思考题

如果 `num_threads=8`、`num_total_tasks=3`，调度器应该怎么做才符合语义？

<details>
<summary>参考答案</summary>
语义只要求 task_id=0,1,2 各执行一次并在 `run()` 返回前完成。允许只有 3 个线程实际干活，其余线程空闲。
</details>

### 判断题

“一次 `run(runnable, 64)` 等价于 64 次独立 launch。”对 / 错？

<details>
<summary>参考答案</summary>
错。它是一次 launch，内部包含 64 个 task。依赖关系与完成语义都按 launch 粒度管理。
</details>

### 代码预测题

`simpleTest` 中 `first` 和 `second` 都是 `SimpleMultiplyTask`，若只执行 `first` 不执行 `second`，最终检查会通过吗？

<details>
<summary>参考答案</summary>
不会。校验逻辑按两次变换计算 expected，少一次 launch 会导致结果偏小。
</details>

---

## 建议的下一步（比“直接写代码”更稳）

1. 先只跑：`simple_test_sync`，确保语义清晰
2. 再跑：`super_super_light`，观察调度开销
3. 最后跑：`spin_between_run_calls`，验证 sleeping 价值

当你能用自己的话解释这三组现象时，再写 `ThreadPoolSleeping`，出错概率会低很多。

---

### 概念 5：ThreadPoolSleeping 状态机与时序

**定义**

`ThreadPoolSleeping` 是一种“常驻 worker + 条件变量唤醒”的调度器。worker 在无任务时阻塞休眠，在有任务或停止信号时被唤醒。

**直觉**

把线程池想成“夜班值守”：

- 没活时，值守员睡觉（阻塞等待）
- 来活时，被叫醒处理
- 下班时，统一广播“收工”，所有值守员退出

**机制**

建议的状态机（worker 视角）：

```text
[WAIT]
  条件变量 wait(lock, stop || !queue.empty())
   ├─ stop && queue.empty -> [EXIT]
   └─ queue 非空 -> pop 一个任务 -> [RUN_TASK] -> 回到 [WAIT]
```

建议的状态机（run 调用视角）：

```text
[SUBMIT]
  把 task_id=0..N-1 对应闭包压入队列
  -> notify_all / notify_n
  -> [WAIT_DONE]
  等待“本次 launch 完成计数 == N”
  -> [RETURN]
```

建议的状态机（析构视角）：

```text
[STOP]
  持锁设置 stop=true
  -> notify_all
  -> join 全部 worker
  -> [DESTROYED]
```

**在 Lab 2 中的体现**

为什么你之前会遇到“run 卡住”？

- 如果 `run()` 里直接调用 `worker_thread()`（无限循环）
- 而 `worker_thread()` 退出条件依赖 `stop=true`
- 但 `stop` 只在析构时设置
- 则主线程会被困在 worker 循环，`run()` 无法正常返回

正确职责边界应是：

- worker 循环：只由线程池生命周期管理（构造创建，析构停止）
- `run()`：只负责“提交本轮任务 + 等待本轮完成”

> **[RDMA 映射]**
> 这和 control-plane / data-plane 边界类似：
> - worker 常驻循环类似数据面轮询器
> - `run()` 类似控制面下发一批工作并等待完成条件
> 混淆两者生命周期，通常会导致死锁或尾延迟异常。

---

## ThreadPoolSleeping 最短实现检查清单

- [ ] worker 线程在构造阶段一次性创建，不在每次 `run` 里重复创建
- [ ] `run` 提交 N 个 task 后，必须等待这 N 个 task 完成再返回
- [ ] 完成计数器与等待条件只覆盖“本次 launch”，避免跨轮串扰
- [ ] 队列操作全部在同一把 mutex 保护下进行
- [ ] 析构时先 `stop=true` 再 `notify_all`，最后 `join`
- [ ] 任何路径都不允许“主线程进入无限 worker 循环”

---

## 再加 2 题（实现前必答）

### 判断题

“`run()` 为了帮忙干活，可以直接调用一次 `worker_thread()`。”对 / 错？

<details>
<summary>参考答案</summary>
通常错。除非 `worker_thread()` 被设计为“单次取活函数”而非无限循环。若它是常驻循环，主线程会被困住，`run()` 不能按语义返回。
</details>

### 思考题

为什么 `done_cv.wait(lock, predicate)` 比“while + sleep_for”更适合等待本轮完成？

<details>
<summary>参考答案</summary>
`condition_variable` 可事件驱动唤醒，避免固定轮询周期带来的 CPU 空转与额外尾延迟；`predicate` 还能防止虚假唤醒导致的逻辑错误。
</details>
