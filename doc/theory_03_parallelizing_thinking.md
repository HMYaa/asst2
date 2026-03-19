# 理论 03：并行化思维与 DAG 依赖分析

> 一句话：把一个计算问题拆解成并行可执行的任务图（DAG），是所有并行系统设计的起点——从 Lab 2 Part B 到 NCCL AllReduce 都是同一套思维。

## 为什么你需要学这个

Lab 2 Part B 要求你实现 `runAsyncWithDeps()`，支持带依赖关系的异步任务调度。你需要理解：

- 什么是任务之间的"依赖"？它从哪里来？
- 一组带依赖的任务形成了什么样的结构（DAG）？
- 调度器在什么时机触发新任务？这个判断逻辑怎么设计？
- Lab 2 测试中的 fan-in、reduction tree 等拓扑长什么样？

这一章建立的思维模型，也是你理解 NCCL 集合通信调度的基础。

---

## 核心概念

---

### 概念 1：数据依赖（Data Dependency）

**定义**

数据依赖是指两个计算操作之间因数据的产生和消费关系而形成的执行顺序约束。若操作 B 需要使用操作 A 产生的结果，则称 B 对 A 存在数据依赖（记作 A → B），A 必须在 B 之前完成。数据依赖分为三类：写后读（RAW / true dependency）、读后写（WAR / anti-dependency）、写后写（WAW / output dependency），其中 RAW 是唯一的"真依赖"，其余两种可通过寄存器重命名等技术消除。

**直觉**

```
做菜的例子：
  切菜 → 炒菜 → 装盘

  "炒菜"依赖"切菜"的结果（切好的菜）→ 必须先切后炒
  "装盘"依赖"炒菜"的结果（炒好的菜）→ 必须先炒后装

  但如果同时要做两道菜：
    切菜A → 炒菜A ─┐
                    ├→ 装盘（两道菜一起上桌）
    切菜B → 炒菜B ─┘
  
  切菜A 和 切菜B 之间没有依赖 → 可以并行
  炒菜A 和 炒菜B 之间没有依赖 → 可以并行
  装盘依赖 A 和 B 都炒好       → 必须等两道都完成
```

**机制**

在 Lab 2 Part B 中，依赖关系通过 `runAsyncWithDeps` 的第三个参数 `deps` 显式声明：

```cpp
TaskID launchA = t->runAsyncWithDeps(taskA, 128, noDeps);   // A 无依赖
TaskID launchB = t->runAsyncWithDeps(taskB, 2, {launchA});  // B 依赖 A
TaskID launchC = t->runAsyncWithDeps(taskC, 6, {launchA});  // C 依赖 A
TaskID launchD = t->runAsyncWithDeps(taskD, 32, {launchB, launchC}); // D 依赖 B 和 C
t->sync();
```

依赖语义的精确含义：

```
B 依赖 A 意味着：
  A 的所有 128 个 task 必须全部完成
  → 然后 B 的 2 个 task 才允许开始执行
  
  注意：不是 A 的第 1 个 task 完成就触发 B 的第 1 个 task
        而是 A 的最后一个 task 完成，B 才能开始
        因为依赖是 BulkLaunch 粒度的，不是单个 task 粒度的
```

**在 Lab 2 中的体现**

这个粒度设计简化了你的实现：你不需要跟踪单个 task 之间的依赖，只需跟踪 BulkLaunch 之间的依赖。判断条件很简单：

```
BulkLaunch X 是否可以开始执行？
  → X.deps 中的每一个 LaunchID，其 tasks_remaining 是否都 == 0？
  → 如果是，X 进入 ready 状态，展开所有 task 进入 ready_queue
```

---

### 概念 2：有向无环图（DAG, Directed Acyclic Graph）

**定义**

有向无环图是一个由有限个顶点和有向边组成的图，其中不存在从某个顶点出发经过若干条有向边又回到该顶点的路径（即无环）。在并行计算中，DAG 用于表示任务之间的依赖关系：顶点表示计算任务，有向边 A → B 表示 B 依赖 A 的结果。DAG 的无环性保证了存在至少一种合法的执行顺序（拓扑排序），不会出现循环等待（死锁）。

**直觉**

```
DAG 就是一个"工序流程图"：
  
  箭头表示"必须先做完前面的，才能做后面的"
  没有箭头连接的任务可以同时做

  A ─→ B ─→ D
  │         ↑
  └─→ C ───┘
  
  A 先做（无前驱）
  B 和 C 可以并行（各自只依赖 A）
  D 必须等 B 和 C 都完成
  
  "无环"保证不会出现：A 等 B，B 等 C，C 等 A（死锁）
```

**机制**

DAG 的几个关键属性：

**① 入度（In-degree）**：一个节点有多少条入边 = 它依赖多少个前驱。入度为 0 的节点可以立即执行。

**② 临界路径（Critical Path）**：DAG 中从起点到终点的最长路径。它决定了整个 DAG 的最短完成时间——即使有无限个处理器，也无法快于临界路径的长度。

```
示例：
  A(3ms) ─→ B(5ms) ─→ D(2ms)
  A(3ms) ─→ C(1ms) ─→ D(2ms)
  
  路径 A→B→D = 3+5+2 = 10ms（临界路径）
  路径 A→C→D = 3+1+2 = 6ms
  
  即使 B 和 C 并行执行：
    总时间 = A(3ms) + max(B(5ms), C(1ms)) + D(2ms) = 3+5+2 = 10ms
  
  无法快于 10ms，因为 B 在临界路径上
```

**③ 并行度（Parallelism）**：DAG 中同一时刻最多可以并行执行的任务数。

```
  A ───→ D
  B ───→ D    A、B、C 可以同时执行 → 并行度 = 3
  C ───→ D    D 只能在 A、B、C 全完成后执行
```

**在 Lab 2 中的体现**

Lab 2 Part B 的测试用例本质上就是不同形状的 DAG：

```
simple_test_async：
  A(4 tasks) → 就一个节点，无依赖
  
math_operations_in_tight_loop_async：
  A → B → C → D → ... → N
  线性链，并行度 = 1（每步依赖上一步）
  这个测试主要验证"能否正确处理串行依赖链"

math_operations_fan_in：
  A₁ ─┐
  A₂ ─┤
  A₃ ─┼→ Reduce
  ... ─┤
  Aₙ ─┘
  Fan-in 拓扑，所有 Aᵢ 可并行，Reduce 等它们全部完成
  
math_operations_reduction_tree：
  A₁  A₂  A₃  A₄  A₅  A₆  A₇  A₈
   \  /    \  /    \  /    \  /
   R₁₂    R₃₄    R₅₆    R₇₈
     \    /          \    /
     R₁₂₃₄         R₅₆₇₈
        \          /
        R₁₂₃₄₅₆₇₈
  二叉树，每层节点数减半，深度 = log₂(N)
```

> **RDMA 映射**：NCCL 的 Ring AllReduce 在 DAG 视角下是一条线性链：Step 0 → Step 1 → Step 2 → ... → Step 2(N-1)。每个 Step 依赖前一个 Step 完成后才能开始发送下一段数据。Tree AllReduce 则是一棵二叉树 DAG，和 `reduction_tree` 测试的拓扑结构一模一样。理解 Lab 2 Part B 就是在理解 NCCL 的底层调度逻辑。

---

### 概念 3：拓扑排序（Topological Sort）

**定义**

拓扑排序是将有向无环图的所有顶点排成一个线性序列，使得对于每条有向边 (u, v)，u 在序列中出现在 v 之前。一个 DAG 可能存在多种合法的拓扑排序。拓扑排序的经典算法是 Kahn 算法（BFS）：反复移除入度为 0 的顶点，将其加入结果序列，并更新剩余顶点的入度。

**直觉**

```
拓扑排序回答的问题：
  "在不违反依赖的前提下，这些任务可以按什么顺序做？"

示例 DAG：A→B, A→C, B→D, C→D

合法的拓扑序：
  A, B, C, D ✓（B 和 C 的顺序可以互换）
  A, C, B, D ✓
  
非法的拓扑序：
  B, A, C, D ✗（B 在 A 前面，违反 A→B）
  D, A, B, C ✗（D 在所有人前面，违反所有依赖）
```

**机制**

Kahn 算法（BFS 拓扑排序）：

```
算法步骤：
  1. 计算每个节点的入度
  2. 将所有入度为 0 的节点放入 ready_queue
  3. 循环：
     a. 从 ready_queue 取出一个节点 X
     b. 执行 X
     c. 对 X 的每个后继 Y：Y.in_degree--
     d. 若 Y.in_degree == 0，将 Y 放入 ready_queue
  4. 重复直到 ready_queue 为空

示例：
  DAG: A→B, A→C, B→D, C→D
  
  初始入度：A:0, B:1, C:1, D:2
  ready_queue = [A]
  
  取 A → 执行 A → B.in_degree=0, C.in_degree=0
  ready_queue = [B, C]
  
  取 B → 执行 B → D.in_degree=1
  取 C → 执行 C → D.in_degree=0
  ready_queue = [D]
  
  取 D → 执行 D → 完成
```

**在 Lab 2 中的体现**

Lab 2 Part B 的调度核心**本质上就是 Kahn 算法的并行版本**：

```
Lab 2 对 Kahn 算法的映射：

  "节点"       = BulkLaunch（一次 runAsyncWithDeps 调用）
  "入度"       = unmet_deps.size()（还有多少前驱未完成）
  "ready_queue" = 入度变为 0 的 BulkLaunch 展开出的所有单个 task
  "执行节点"    = Worker 线程从 ready_queue 取 task 执行
  "更新后继入度" = BulkLaunch 的最后一个 task 完成时，遍历后继并减入度

区别：
  经典 Kahn：单线程串行执行，ready_queue 中一次取一个
  Lab 2：多线程并行执行，ready_queue 中的 task 被多个 Worker 并发消费
```

这是 Lab 2 Part B 最核心的设计框架。理解了这一点，实现就是"把 Kahn 算法的数据结构用线程安全的方式管理起来"。

---

### 概念 4：并行度与临界路径的权衡

**定义**

在 DAG 调度中，并行度（Parallelism Degree）是指在给定时间点可以同时执行的最大任务数，由 DAG 的宽度决定。临界路径（Critical Path）是 DAG 中从任意源节点到任意汇节点的最长加权路径，其长度是使用无限处理器时的理论最短完成时间。平均并行度定义为：总工作量 / 临界路径长度。

**直觉**

```
两个极端：

极端 1：完全并行（宽而浅的 DAG）
  A₁  A₂  A₃  ...  A₁₀₀₀
  并行度 = 1000，临界路径 = 1 步
  16 个线程可以充分利用，加速比接近 16x

极端 2：完全串行（窄而深的 DAG）
  A₁ → A₂ → A₃ → ... → A₁₀₀₀
  并行度 = 1，临界路径 = 1000 步
  无论多少线程，加速比 = 1x（只有一个任务能在同一时刻运行）

实际场景：介于两者之间
  你的调度器能做的：让并行度 > 1 的时候尽量多跑任务
  你的调度器做不到的：缩短临界路径（那是算法设计者的事）
```

**机制**

用 Lab 2 的 `reduction_tree` 测试做量化分析：

```
假设 8 个初始的数学计算 launch（A₁到A₈），每个 launch 有 64 个 task

  层 0：A₁  A₂  A₃  A₄  A₅  A₆  A₇  A₈    并行度 = 8 × 64 = 512 tasks
  层 1：R₁₂    R₃₄    R₅₆    R₇₈           并行度 = 4 × 1 = 4 tasks（reduce 较轻）
  层 2：R₁₂₃₄        R₅₆₇₈                并行度 = 2 × 1 = 2 tasks
  层 3：R₁₂₃₄₅₆₇₈                          并行度 = 1 × 1 = 1 task

层 0 的 512 个 task 可以被 16 个 Worker 并行处理 → 充分利用
层 3 的 1 个 task 只有 1 个 Worker 在做 → 其余 15 个闲置

总工作量 = 512 + 4 + 2 + 1 = 519 tasks
临界路径 = 层0一个launch(64 tasks) + 层1 + 层2 + 层3 = 4 层深度
平均并行度 = 519 / 4 ≈ 130
```

**在 Lab 2 中的体现**

对于你的调度器实现：
- **层 0**：所有 8 个 launch 无依赖，应该立即全部展开到 ready_queue
- **层 0 → 层 1 的转换**：当 A₁和A₂的最后一个 task 都完成时，R₁₂ 的 `unmet_deps` 变空，应该立即被展开
- **关键性能点**：转换的速度。如果 A₁完成后等了很久才检查 R₁₂ 是否 ready，会浪费时间

你不需要做复杂的"临界路径分析"来决定优先执行哪个任务。Lab 2 的规模下，只要保证"依赖满足后立即执行"就足够了。

---

### 概念 5：异步执行模型（Asynchronous Execution）

**定义**

异步执行是指调用方发起一个操作后，不等待操作完成就立即返回继续执行后续代码。操作在后台异步执行，调用方后续通过显式的同步操作（如 barrier、sync、wait）来确认操作完成。异步模型的核心价值是允许调用方在等待期间提交更多工作或执行其他计算，从而提高系统吞吐量。

**直觉**

```
同步模型（Lab 2 Part A 的 run()）：
  你：帮我处理这 100 个任务
  系统：好的...（你站在这里等）...全部做完了，给你结果
  你：（终于）好的，那帮我处理下一批
  
  问题：你在等的过程中，什么都做不了
  
异步模型（Lab 2 Part B 的 runAsyncWithDeps()）：
  你：帮我处理这 100 个任务（launch A）
  系统：收到，我先记下来
  你：（立即继续）再帮我处理这 50 个任务（launch B，依赖 A）
  系统：收到
  你：再帮我处理这 20 个任务（launch C，依赖 B）
  系统：收到
  你：好了我都提交完了，告诉我什么时候全部做完 → sync()
  系统：...（在后台按依赖顺序执行 A→B→C）...全部做完了！
  
  好处：你一口气把所有工作和依赖关系告诉系统
       系统可以全局地看到整个 DAG，做更好的调度决策
```

**机制**

Lab 2 Part B 的异步接口设计：

```
1. runAsyncWithDeps(runnable, num_tasks, deps) → 返回 TaskID
   语义：
     - 不等待执行完成，立即返回
     - 返回的 TaskID 是这个 BulkLaunch 的唯一标识
     - deps 中的所有 Launch 必须完成后，本 Launch 才能开始
   
2. sync() → 阻塞调用
   语义：
     - 阻塞直到之前所有 runAsyncWithDeps 提交的 Launch 全部完成
     - sync 返回后，所有结果保证可见

3. run(runnable, num_tasks) → 同步调用
   语义：
     - 等价于 runAsyncWithDeps(runnable, num_tasks, {}) + sync()
     - 保持向后兼容 Part A 的行为
```

**调用者视角的时序：**

```
时间线 →

调用线程：  ┃runAsync(A)┃runAsync(B)┃runAsync(C)┃  sync()...等待...  ┃ 返回
            ┃  立即返回   ┃  立即返回   ┃  立即返回   ┃                    ┃
            
Worker们：  ┃             ┃  开始执行 A  ┃  A 完成,开始 B  ┃ B完成,开始 C ┃ C完成
            
注意：Worker 可能在第一个 runAsync 调用后就开始执行 A，
     也可能等到 sync() 之后才开始——两种都是合法的实现！
     关键是 sync() 返回时一切都必须完成。
```

**在 Lab 2 中的体现**

你的 `runAsyncWithDeps()` 实现应该：
1. 分配一个唯一的 `TaskID`（简单地用递增计数器）
2. 创建 BulkLaunch 记录，记录依赖关系
3. 如果依赖全部已满足（deps 为空或 deps 中的 launch 都已完成），直接展开到 ready_queue
4. 否则放入 waiting_map，等待后续触发
5. **立即返回**，不等执行完成

> **RDMA 映射**：`runAsyncWithDeps()` 的行为和 `ibv_post_send()` 极其相似——都是"提交工作请求后立即返回，不等执行完成"。`sync()` 对应 `ibv_poll_cq()` 轮询直到所有 WC 出现。`deps` 参数类似 RDMA 的 `IBV_SEND_FENCE` 标志，用于声明操作之间的顺序依赖。

---

### 概念 6：触发检查的时机——事件驱动调度

**定义**

事件驱动调度（Event-Driven Scheduling）是一种调度策略，调度器不主动轮询任务状态，而是在特定事件发生时被动触发调度决策。在 DAG 调度中，关键事件是"某个任务完成"，它可能使后续任务的依赖得到满足，从而触发新任务的调度。

**直觉**

```
两种方式检查"后续任务是否 ready"：

方式 A（轮询）：
  每隔 1ms 检查一遍所有 waiting 中的 launch
  → 浪费 CPU，而且有最多 1ms 的响应延迟

方式 B（事件驱动）：
  只在"某个 BulkLaunch 的最后一个 task 完成"这一刻才检查
  → 精准触发，零延迟，无浪费

Lab 2 应该用方式 B。
```

**机制**

触发链的完整时序：

```
事件：Worker 线程完成了 Launch A 的 task #127（假设 A 有 128 个 task）

① Worker 线程：
   lock(mtx_)
   launch_A.tasks_remaining--    // 127 → ... → 1 → 0
   
② 检查：tasks_remaining == 0？
   如果不是 0 → 释放锁，继续取下一个 task
   如果是 0  → Launch A 完全完成！进入步骤 ③

③ 遍历 Launch A 的后继列表（dependents）：
   for each successor_id in launch_A.dependents:
     successor = waiting_map[successor_id]
     successor.unmet_deps.remove(launch_A.id)
     
     if successor.unmet_deps.empty():
       // successor 的所有依赖都满足了！
       从 waiting_map 移除 successor
       将 successor 展开到 ready_queue：
         for i in 0..successor.num_total_tasks:
           ready_queue.push({successor, i})
       
④ cv_worker.notify_all()  // 唤醒可能在睡觉的 Worker
   unlock(mtx_)
   
⑤ 检查是否所有 Launch 都完成了（sync 需要）
   if all_launches_completed:
     cv_done.notify_one()  // 唤醒 sync() 中等待的主线程
```

**关键设计点：谁来执行步骤 ③？**

```
答案：完成 Launch A 最后一个 task 的那个 Worker 线程。

为什么是它？
  因为它是唯一知道"tasks_remaining 刚刚变成 0"的线程。
  
  其他 Worker 可能在：
    - 执行其他 task（忙碌中）
    - 睡眠等待新任务（cv_worker.wait）
    - 执行 Launch A 的其他 task（但那些 task 更早完成了）
  
  只有"最后一个完成者"能安全地做触发检查。
  这是一个非常典型的"最后完成者负责清理"模式。
```

**在 Lab 2 中的体现**

这个触发逻辑是 Part B 实现中最容易出 Bug 的地方：

```
Bug 1：忘记在 tasks_remaining == 0 时检查后继
  → 后继任务永远不会被调度，sync() 死锁

Bug 2：检查后继时没有持锁
  → 两个 Worker 同时完成同一个 Launch 的最后两个 task
  → 两个 Worker 都认为自己是"最后完成者"
  → 同一个后继被展开两次 → 任务重复执行，结果错误

Bug 3：展开后继后忘记 notify_all
  → 新的 task 进了 ready_queue，但所有 Worker 都在睡觉
  → 死锁（直到下一个不相关的事件偶然唤醒 Worker）

防御方式：在 mtx_ 保护下完成整个 ②③④ 序列
```

---

## 关键结论

- [ ] 依赖关系形成 DAG，Lab 2 Part B 的调度本质是并行化的 Kahn 算法（BFS 拓扑排序）
- [ ] 依赖是 BulkLaunch 粒度的（不是单个 task 粒度），简化了实现
- [ ] 临界路径决定了最短完成时间——这不是调度器能优化的，是 DAG 结构决定的
- [ ] 调度器能做的是：依赖满足后尽快触发后续任务（零延迟响应）
- [ ] 触发检查由"最后一个完成 task 的 Worker"执行，这是事件驱动模型的核心
- [ ] 整个触发链（检查 → 展开 → 通知）必须在锁保护下原子完成，否则会出竞态 Bug
- [ ] NCCL 的 Ring AllReduce 是线性 DAG，Tree AllReduce 是二叉树 DAG——同一套模型

---

## 自测

**[判断题 1]**
T/F：在 Lab 2 Part B 中，如果 Launch B 依赖 Launch A，那么 A 的第一个 task 完成后，B 的第一个 task 就可以开始执行。

<details>
<summary>答案</summary>

**F（错误）**。依赖是 BulkLaunch 粒度的。A 的**所有** task 都必须完成后，B 的**任何** task 才能开始。这是 `runAsyncWithDeps` 接口的语义定义。
</details>

---

**[判断题 2]**
T/F：`runAsyncWithDeps()` 调用返回后，对应的 task 可能已经开始执行了。

<details>
<summary>答案</summary>

**T（正确）**。`runAsyncWithDeps()` 是异步的，它只保证"返回前已将任务注册到调度系统"。如果任务无依赖，Worker 线程可能在 `runAsyncWithDeps()` 返回之前就从 ready_queue 中取到了 task 并开始执行。这是完全合法的行为。
</details>

---

**[思考题 1]**
以下 DAG 的临界路径长度是多少？最大并行度是多少？如果有 4 个 Worker 线程，理论最短完成时间是多少？

```
A(10ms) ──→ C(5ms) ──→ E(3ms)
B(8ms)  ──→ D(6ms) ──→ E(3ms)
```

<details>
<summary>参考思路</summary>

**临界路径：**
- 路径 A→C→E = 10+5+3 = 18ms
- 路径 B→D→E = 8+6+3 = 17ms
- 临界路径 = **18ms**（取最长路径）

**最大并行度：**
- A 和 B 可以并行 → 最大并行度 = **2**
- C 和 D 可以并行 → 最大并行度 = **2**
- E 只能单独执行 → 并行度 = 1

**4 个 Worker 的理论最短完成时间：**
- 第一层：A 和 B 并行，耗时 = max(10, 8) = 10ms
- 第二层：C 和 D 并行，耗时 = max(5, 6) = 6ms
- 第三层：E 单独，耗时 = 3ms
- 总时间 = 10 + 6 + 3 = **19ms**

注意这比临界路径 18ms 长了 1ms！原因是 C 必须等 A 完成（10ms），而 D 必须等 B 完成（8ms），所以 D 在第 8ms 就 ready 了，但 C 要到第 10ms 才 ready。D 从 8ms 开始执行到 14ms 结束，C 从 10ms 开始到 15ms 结束，所以 E 需要等到 max(14, 15) = 15ms 才能开始，E 完成于 18ms。

实际最短完成时间 = **18ms**（等于临界路径长度），因为 4 个 Worker 对于最大并行度 2 是足够的。
</details>

---

**[思考题 2]**
在你的 Part B 实现中，`runAsyncWithDeps()` 应该在什么时候把任务展开到 ready_queue？考虑以下两种设计：

- 设计 A：`runAsyncWithDeps()` 中检查 deps 是否已全部满足，满足则立即展开
- 设计 B：只在 `sync()` 被调用后才开始展开和执行任务

哪种设计更好？为什么？

<details>
<summary>参考思路</summary>

**设计 A 更好**，原因有二：

1. **更早开始执行**：如果 Launch A 无依赖，在 `runAsyncWithDeps(A)` 调用时就展开到 ready_queue，Worker 线程可以立即开始执行 A 的 task。如果等到 `sync()` 才展开，那 `runAsyncWithDeps(A)` 和 `sync()` 之间的时间就白白浪费了。

2. **流水线效应**：调用线程在执行 `runAsyncWithDeps(B)` 时，Worker 线程可能已经在执行 A 的 task 了。如果 A 在这段时间内完成，B 也可能直接进入 ready 状态——这种"提交和执行的重叠"是异步模型的核心优势。

设计 B 相当于退化成了"收集完所有任务后再统一执行"的批处理模式，失去了异步的好处。

不过，设计 A 带来了实现复杂度：你需要在 `runAsyncWithDeps()` 中就处理好线程安全问题——可能 Worker 线程正在修改 waiting_map（完成某个 launch 并触发后继），而主线程同时在 `runAsyncWithDeps()` 中向 waiting_map 添加新的 launch。所有这些操作都必须在同一把锁的保护下。
</details>

---

**[代码预测题]**
以下 Part B 的调用序列，执行完 sync() 后一共执行了多少个 task？

```cpp
std::vector<TaskID> noDeps;

TaskID a = t->runAsyncWithDeps(taskA, 100, noDeps);
TaskID b = t->runAsyncWithDeps(taskB, 50, {a});
TaskID c = t->runAsyncWithDeps(taskC, 50, {a});
TaskID d = t->runAsyncWithDeps(taskD, 10, {b, c});

t->sync();
```

<details>
<summary>分析</summary>

**总共执行 210 个 task。**

- Launch A：100 个 task，无依赖，最先执行
- Launch B：50 个 task，A 完成后执行
- Launch C：50 个 task，A 完成后执行（B 和 C 可以并行）
- Launch D：10 个 task，B 和 C 都完成后执行
- 总计：100 + 50 + 50 + 10 = 210

执行顺序为：A(100) → B(50) 和 C(50) 并行 → D(10)

在 16 线程下的理想时间：
- A：100/16 ≈ 7 个 task 的时间
- B 和 C 并行：max(50, 50)/16 ≈ 4 个 task 的时间
- D：10/16 ≈ 1 个 task 的时间
- 总计 ≈ 12 个单 task 的时间

串行时间 = 210 个单 task 的时间
加速比 ≈ 210/12 ≈ 17.5x（利用了任务间并行 + 任务内并行）
</details>
