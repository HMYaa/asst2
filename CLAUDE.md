# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

本文件为 Claude Code (claude.ai/code) 在此代码仓库中工作时提供指导。

## 仓库目的

这是斯坦福 CS149 作业 2：分阶段构建 C++ 任务执行库。

- `part_a/` 实现同步批量任务 launch 策略。
- `part_b/` 扩展睡眠线程池设计以支持带依赖的异步批量 launch。
- 主要工作集中在 `part_a/tasksys.cpp`、`part_a/tasksys.h`、`part_b/tasksys.cpp` 和 `part_b/tasksys.h`。
- **不要**修改提供的 `Makefile` 文件；README 明确警告这可能破坏评分系统。

## 构建和运行命令

### Part A

```bash
cd part_a && make
```

运行单个测试：

```bash
cd part_a && ./runtasks -n 16 simple_test_sync
```

运行其他单个测试：

```bash
cd part_a && ./runtasks -n 16 mandelbrot_chunked
```

查看可用测试名称 / 参数：

```bash
cd part_a && ./runtasks -h
```

清理构建产物：

```bash
cd part_a && make clean
```

### Part B

```bash
cd part_b && make
```

运行异步/依赖测试：

```bash
cd part_b && ./runtasks -n 16 simple_test_async
```

依赖密集型测试示例：

```bash
cd part_b && ./runtasks -n 16 strict_diamond_deps_async
cd part_b && ./runtasks -n 16 strict_graph_deps_large_async
```

查看可用测试名称 / 参数：

```bash
cd part_b && ./runtasks -h
```

清理构建产物：

```bash
cd part_b && make clean
```

### 性能测试工具

在 `part_a/` 或 `part_b/` 目录下运行：

```bash
python3 ../tests/run_test_harness.py
```

只运行选定测试：

```bash
python3 ../tests/run_test_harness.py -t super_light super_super_light
```

运行异步测试用例：

```bash
python3 ../tests/run_test_harness.py -a
```

## 重要构建上下文

- `part_a/Makefile` 使用 `-O0` 编译。
- `part_b/Makefile` 使用 `-O3` 编译。
- 两个二进制文件都名为 `runtasks`，将 `../tests/main.cpp` 与本地 `tasksys.cpp` 链接编译。
- 评分在 Linux/AWS ARM (`c7g.4xlarge`) 上进行；本地 macOS 参考二进制文件仅供对比参考。

## clangd / LSP 配置

- 仓库根目录包含 `compile_commands.json`，覆盖 `part_a/` 和 `part_b/` 的构建。
- 仓库根目录还包含 `.clangd`，其中指定了 GCC 11 标准库包含路径。
- 此项目需要 `.clangd` 解决方案，因为本地 clangd 解析的是 GCC **12** 标准库路径，尽管 `/usr/bin/g++` 指向的是 GCC **11**。
- 问题症状：clangd/LSP 报错 `'vector' file not found`，随后引发一系列 `std`-undeclared 错误。
- 如果 LSP 再次出现问题，用以下命令验证：

```bash
clangd --check="/home/yangxw/yxw/cs149/asst2/part_b/tasksys.cpp" --compile-commands-dir="/home/yangxw/yxw/cs149/asst2"
clangd --check="/home/yangxw/yxw/cs149/asst2/part_a/tasksys.cpp" --compile-commands-dir="/home/yangxw/yxw/cs149/asst2"
```

- 预期结果是两个文件都显示 `All checks completed, 0 errors`。
- 不要以为单独升级 clangd 就能解决问题：在 clangd 14 和 18 上都复现了相同的标准库发现问题，直到 `.clangd` 明确添加了 GCC 11 包含目录。

## 高层架构

### 核心抽象层

共享契约在 `part_a/itasksys.h` 和 `part_b/itasksys.h` 中：

- `IRunnable::runTask(task_id, num_total_tasks)` 定义批量 launch 中一个逻辑任务实例。
- `ITaskSystem::run(...)` 是**同步**批量 launch API。
- `ITaskSystem::runAsyncWithDeps(...)` 加上 `sync()` 添加带 DAG 依赖的**异步**批量 launch。

关键设计点是运行时调度的是**批量 launch** 的多个索引任务，而非任意闭包。

### 学生实现的执行模型

`tasksys.h` 声明了四个实现，代表作业的递进过程：

1. `TaskSystemSerial`
   - 基线版本：调用方线程直接运行每个任务。

2. `TaskSystemParallelSpawn`
   - 在每次 `run()` 调用中创建工作线程进行并行批量 launch。
   - 作为第一个正确的并行实现很有用，但在轻量级工作负载上线程创建/join 开销占主导。

3. `TaskSystemParallelThreadPoolSpinning`
   - 工作线程创建一次并反复轮询获取任务。
   - 主要权衡：降低了 launch 开销，但空闲工作线程和可能的主线程会在自旋时消耗 CPU。

4. `TaskSystemParallelThreadPoolSleeping`
   - 性能和 Part B 功能的最终设计。
   - 使用睡眠/唤醒同步而非纯自旋。
   - 在 Part B 中，此实现是唯一需要正确异步依赖支持的实现。

### 测试如何驱动架构

`tests/main.cpp` 是两部分的可执行入口点。

- 它依次构造每个任务系统实现。
- 它从 `tests/tests.h` 分派命名测试。
- 对于每个实现，它多次运行测试并报告最短运行时间。
- CLI 上的测试名称直接来自 `tests/main.cpp` 中的字符串表。

这意味着大多数调试应将 `runtasks` 视为调度器实现周围的小型工具，而不是应用程序逻辑。

### 工作负载结构及其含义

`tests/README.md` 解释了评分工作负载的性能特征：

- `super_super_light`, `super_light`：大量微小 launch；调度器开销往往是瓶颈。
- `ping_pong_equal` / `unequal`：突出负载均衡质量。
- `recursive_fibonacci`：重型计算，使 launch 开销不那么重要。
- `mandelbrot_chunked`：单次大型批量 launch；线程池复用不那么重要。
- `*_async`, `strict_*deps*`, `reduction_tree`, `fan_in`：练习 Part B 依赖跟踪和就绪状态转换。

分析性能回退时，始终问自己工作负载是否受限于：
- launch 开销
- 共享调度器状态的竞争
- 自旋导致的 CPU 浪费
- 负载不均衡
- 或依赖簿记

### Part B 调度模型

来自 `README.md` 的 Part B API 语义比任何特定实现都重要：

- `runAsyncWithDeps()` 必须在注册批量 launch 后立即返回。
- 批量 launch 必须等到其依赖列表中的**所有** launch 完全完成后才能开始。
- `sync()` 等待**所有先前** launch 的完成。
- 推荐的心智模型是：
  - 一个等待结构，用于依赖尚未满足的 launch
  - 和一个就绪队列，用于工作线程现在可以执行的任务

自然簿记单元通常是**批量 launch 记录**：runnable 指针、任务计数、剩余任务数、依赖计数器和后继列表。

### 当前 Part B 实现关键结构（`part_b/tasksys.h`）

- `BulkLaunch` 结构体（定义在 `tasksys.cpp`，`.h` 前向声明 + `unique_ptr`）：每次 `runAsyncWithDeps()` 一条记录，下标即 `TaskID`。
- **两把锁分工**：
  - `graph_mtx_`：保护依赖图（`launches_`、`successors_`、`ready_queue_`）+ `cv_work_` 通知 worker。
  - `sync_mtx_`：仅供 `sync()` 在 `cv_sync_` 上等待 `outstanding_tasks_` 归零。
- `outstanding_tasks_`：per-batch 粒度原子计数（而非 per-task），`sync()` 等其归零。
- `ready_count_`：镜像 `ready_queue_.size()` 的原子整数，允许 worker 在不持 `graph_mtx_` 的情况下判断是否有可用工作，减少自旋时的惊群竞争。
- `complete_bulk_launch_unlocked()`：假定调用方已持 `graph_mtx_`，传播依赖并可能触发 `cv_sync_` 通知。

## 行为变更前值得阅读的文件

- `README.md` — 作业语义、评分期望和允许的假设。
- `tests/README.md` — 性能数字背后的工作负载意图。
- `tests/main.cpp` — 确切的 CLI 可见测试名称和工具流程。
- `part_a/tasksys.*` / `part_b/tasksys.*` — 实际的调度器实现。
- `doc/theory_08_task_scheduler_lifecycle.md` — 任务生命周期、条件变量语义和 RDMA 映射（有助于理解调度器的状态机）。
- `doc/design_part_b_dag_scheduler.md` — Part B DAG 调度器完整设计文档：BulkLaunch 一生的状态机、双锁设计原因、六条并发关键路径逐行分析。
- `doc/design_task_scheduler_master.md` — Part A 调度器设计全景（Spinning vs Sleeping 权衡、线程池到条件变量的递进推导）。
- `doc/part_b_scheduler_checklist_v1_v2.md` — 每次迭代/调试前的自检清单（V1 提交前并发正确性 7 条，V2 卡住/长尾/惊群诊断流程）。
- `doc/experience_summary.md` / `doc/concurrency_design_experience_notes.md` — 已踩过的坑及修正认识（stop 语义、锁粒度、伪唤醒等）。
- `doc/mastery_guide_part_b.md` — Part B 掌握度提升指南：从"读懂代码"到"能白板推导等效设计"的五级练习路径，含破坏性实验清单。

## 仓库特定工作风格

### 教练模式（实现时）

**目标**：以"学生亲手实现、教练最小纠偏"为第一原则，优先提升独立并发编程能力。

行为准则：
- 默认不直接给完整实现代码；先提 2-4 个关键问题确认理解，再进入分步指导。
- 用户贴代码后，优先输出三项：并发正确性风险、最小修改建议、验证方法。
- 反馈遵循"最小改动"原则：尽量在用户现有结构上修正，不大改、不重写。
- 每轮聚焦一个目标（如 Spawn 的 run、Sleeping 的 wait/notify 语义），避免一次覆盖全部模块。
- **先正确性后性能**：先保证无 data race/死锁/漏通知，再讨论 tail latency 与吞吐。

固定反馈模板：
1. 目标是否达成（1-2 句）
2. 必修风险点（按严重度排序，最多 3 条）
3. 最小修改清单（可直接动手的 patch 思路）
4. 自测清单（边界条件 + 压测建议）
5. 下一轮提交要求（建议贴 20-80 行关键代码）

### 调试教练模式（调试时）

当用户遇到 bug 时，首先分类现象类型：
- **死锁/崩溃/漏通知/逻辑错误** vs **纯性能退化**（尾延迟、锁竞争、自旋等待）

诊断前要求最小可验证证据（例如，`gdb` 的 `info threads`、主线程 `bt`、工作线程 `bt`）。每轮一个假设 + 一个验证动作。

### 关键语义假设

来自 README.md 的重要约束：
- **run() 与 runAsyncWithDeps() 互斥**：程序只会调用 `run()` 或只会调用 `runAsyncWithDeps()`，不需要处理混合调用的情况。
- 这意味着 `run()` 可以直接用 `runAsyncWithDeps()` + `sync()` 实现。

### 常见陷阱：condition_variable 谓词

`std::condition_variable::wait()` **必须**带谓词。没有它，伪唤醒会导致访问空队列：

```cpp
// 错误 - 缺少谓词
cv.wait(lock);

// 正确 - 带谓词
cv.wait(lock, [this]() { return stop_ || !task_queue_.empty(); });
```

这是 `TaskSystemParallelThreadPoolSleeping` 中 CPU 空转或挂起的最常见原因。
