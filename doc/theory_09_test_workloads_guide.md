# 理论 09：Part A / Part B 测试任务画像与性能含义

> 一句话：不要把 `runtasks` 里的测试只当成“跑分项”，它们其实分别在拷问你的调度器：**固定开销、负载均衡、空转成本、依赖调度** 到底做得怎么样。

---

## 这篇文档解决什么问题

- 我知道 `super_super_light` 和 `super_light` 分别在测什么
- 我知道为什么 `ping_pong_equal` 和 `ping_pong_unequal` 要分开存在
- 我知道哪些测试主要考线程池，哪些测试主要考真正的并行执行
- 我知道 `spin_between_run_calls` 为什么专门针对 spinning 版不友好
- 我能从“某个测试慢了”反推出应该先怀疑调度器的哪一部分

---

## 一、先建立总视角：这些测试到底在考什么

这些测试本质上可以按 4 类能力来理解：


| 类别   | 它在考什么                 | 典型测试                                                           |
| ---- | --------------------- | -------------------------------------------------------------- |
| 固定开销 | 一次 `run()` 本身贵不贵      | `super_super_light`, `super_light`                             |
| 吞吐能力 | 多个 worker 是否稳定并行干活    | `ping_pong_equal`, `recursive_fibonacci`, `mandelbrot_chunked` |
| 负载均衡 | 任务不均匀时会不会出现长尾         | `ping_pong_unequal`                                            |
| 等待策略 | 没活时 spinning 是否浪费 CPU | `spin_between_run_calls`, Sleeping 相关 async 测试                 |


所以以后你看到某个测试慢，不要笼统说“性能差”，而要先判断：

- 是 launch 固定成本太大？
- 是任务分发路径太重？
- 是负载不均没处理好？
- 还是空闲线程在抢 CPU？

---

### 概念 1：`super_super_light` —— 极轻任务，专门测调度器固定成本

**定义**

`super_super_light` 是一个包含 **64 个 task / 400 次 bulk launch** 的超轻量测试。每个 task 只做很小的数组拷贝操作，几乎没有额外计算。

**直觉**

这个测试像是在问：

> “如果任务本身几乎不花时间，你的 runtime 还值得存在吗？”

**机制**

参考定义：

- [tests/README.md:5-6](../tests/README.md#L5-L6)
- [tests.h:668-677](../tests/tests.h#L668-L677)

它本质上是 `pingPongTest(equal_work=true, base_iters=0)`：

- `num_tasks = 64`
- `num_bulk_task_launches = 400`
- `base_iters = 0`
- 每次 launch 都要重新完成一次完整调度流程

也就是说，真正被放大的不是计算量，而是：

- `run()` 的调用开销
- 任务分发开销
- 主线程等待开销
- worker 是否重复创建

**在 Lab 2 中的体现**

- `TaskSystemParallelSpawn` 如果每次都创建/销毁线程，会很容易吃亏
- `ThreadPoolSpinning` 如果调度路径足够短，往往能在这个测试表现很好
- `ThreadPoolSleeping` 如果 sleep / wakeup 成本过大，也可能输给 spinning
- 学完后你应该能说出：这个测试主要不是测“算得快不快”，而是测“调度器自己贵不贵”

---

### 概念 2：`super_light` —— 轻任务，但已进入“调度开销与计算开销同量级”区域

**定义**

`super_light` 与 `super_super_light` 结构类似，同样是 **64 个 task / 400 次 launch**，但每个元素会做一些轻量增量计算，计算量明显高于 `super_super_light`。

**直觉**

它不是“超轻”，也不是“重计算”，而是最容易暴露下面这种问题：

> “你的调度器方向对了，但尾部空转和等待方式还不够精炼。”

**机制**

参考定义：

- [tests/README.md:8-9](../tests/README.md#L8-L9)
- [tests.h:680-689](../tests/tests.h#L680-L689)

参数：

- `num_elements = 32 * 1024`
- `base_iters = 32`
- 每次 task 比 `super_super_light` 多了一定计算量

因此这个测试很适合观察：

- thread pool 是否真正有收益
- spinning 是否开始因为空转而吃亏
- 主线程是否在白白等待
- `next_task_id.fetch_add(1)` 路径是否已经足够轻

**在 Lab 2 中的体现**

如果你的 `ThreadPoolSpinning`：

- 在 `super_super_light` 很好
- 但在 `super_light` 仍然明显慢于参考实现

通常意味着：

- 调度大方向对了
- 但尾部空转、主线程不干活、共享状态访问路径仍有成本
- 学完后你应该能说出：`super_light` 是最适合定位“调度 vs 计算平衡点”的测试之一

---

### 概念 3：`ping_pong_equal` —— 均衡任务下看整体吞吐

**定义**

`ping_pong_equal` 是一个中等偏重的、负载均衡的任务集。它有 **64 个 task / 400 次 launch**，每个 task 的工作量设计成尽量均匀。

**直觉**

这个测试像是在问：

> “如果每份工作都差不多重，你的线程池能不能稳定把机器吃满？”

**机制**

参考定义：

- [tests/README.md:11-13](../tests/README.md#L11-L13)
- [tests.h:692-707](../tests/tests.h#L692-L707)

特点：

- 元素规模比 `super_light` 大得多：`512 * 1024`
- 每次 launch 的总工作量更大
- task 间工作量近似均衡

这时测试重点就不再只是 launch 固定开销，而是：

- worker 是否能持续并行执行
- thread pool 复用是否有效
- 主线程和 worker 是否有不必要竞争

**在 Lab 2 中的体现**

如果你在这个测试慢，优先怀疑：

- 整体吞吐不足
- 共享状态争用太多
- 主线程 busy-wait 干扰 worker
- 线程池内状态切换路径不够紧凑
- 学完后你应该能说出：这个测试主要看“吞吐能力”，不是只看 launch 固定成本

---

### 概念 4：`ping_pong_unequal` —— 不均衡任务下看动态调度能力

**定义**

`ping_pong_unequal` 与 `ping_pong_equal` 结构基本相同，但故意让不同 task 的工作量不同，以考察调度器对不均衡负载的适应能力。

**直觉**

它是在问：

> “如果有些任务重、有些任务轻，你的分发策略会不会让某些线程早早闲下来，而另一些线程拖尾？”

**机制**

参考定义：

- [tests/README.md:14-15](../tests/README.md#L14-L15)
- [tests.h:698-713](../tests/tests.h#L698-L713)
- 负载不均具体逻辑见 [tests.h:174-203](../tests/tests.h#L174-L203)

当 `equal_work=false` 时：

- 较低索引元素需要更多迭代
- 不同 task 负责的区间因此耗时不同

如果你是静态切分，很容易发生：

- 有些线程先做完
- 有些线程继续跑重任务
- 最终出现尾部拖延（tail latency）

**在 Lab 2 中的体现**

动态分配方案，如：

- `next_task_id_.fetch_add(1)`

通常会比静态分块更适合这个测试。

> **[RDMA 映射]**
> 这和网络数据面里“不同流/不同 packet burst 的处理时长不一致”很像。静态分配会放大长尾，动态领取 work 更能压平 idle gap。

- 学完后你应该能说出：`ping_pong_unequal` 的核心不是测 launch 开销，而是测负载均衡

---

### 概念 5：`recursive_fibonacci` —— 重计算任务，调度开销相对不敏感

**定义**

`recursive_fibonacci` 是一个典型的 compute-intensive 测试。每个 task 都用非常低效的递归方式计算 Fibonacci，因此计算量远大于调度开销。

**直觉**

这个测试是在问：

> “如果任务本身很重，你的线程池有没有真正并行起来？”

**机制**

参考定义：

- [tests/README.md:17-18](../tests/README.md#L17-L18)
- [tests.h:721-777](../tests/tests.h#L721-L777)

特点：

- `256 tasks`
- `30 bulk launches`
- `slowFn()` 是递归计算，计算量大

这类测试里：

- thread create/join 的固定开销相对不敏感
- queue / atomic / CV 的开销相对被淹没
- 更能体现“是否真正并行计算”

**在 Lab 2 中的体现**

如果你在这个测试也很差，往往说明不是“小优化问题”，而是：

- worker 没吃满 CPU
- 调度逻辑有严重问题
- 正确性或同步路径限制了并行度
- 学完后你应该能说出：重任务测试里，调度器不是完全不重要，但通常不是第一瓶颈

---

### 概念 6：`math_operations_in_tight_for_loop` —— 中等计算量 + 大量 launch，专门看 thread pool 复用价值

**定义**

这是一个大量重复发起 bulk launch 的中量级计算测试。每个 task 会执行一组 exp / log / multiply / add 操作。

**直觉**

它像是在问：

> “如果每次 launch 都不算太重，但你要连续做 2000 次，线程池是否真的帮你省下了 launch 成本？”

**机制**

参考定义：

- [tests/README.md:20-21](../tests/README.md#L20-L21)
- [tests.h:785-862](../tests/tests.h#L785-L862)

特点：

- `16 tasks per bulk launch`
- `2000 bulk launches`
- 每个 task 都有一定计算量，但不是极重

这是一个典型的“thread pool 该比 spawn 更强”的场景：

- launch 次数很多
- 每次 launch 不够重，无法完全掩盖 create/join 成本

**在 Lab 2 中的体现**

如果你的 thread pool 在这个测试没有明显优于 spawn，优先怀疑：

- `run()` 的批次发布成本太大
- 主线程等待逻辑太重
- worker 唤醒 / 获取任务路径不够精简
- 学完后你应该能说出：这个测试既看计算，也很看“高频 launch 下的线程池复用是否值钱”

---

### 概念 7：`math_operations_in_tight_for_loop_fewer_tasks` —— 任务更粗、数量更少、略不均匀

**定义**

这是上一个测试的变种，但把每次 launch 的任务数从 16 减少到了 9，因此每个 task 的负担更大，且分配略微不均。

**直觉**

它是在看：

> “当 task 更粗时，调度开销是不是自然下降了？而你还能否保持合理的负载均衡？”

**机制**

参考定义：

- [tests/README.md:23-24](../tests/README.md#L23-L24)
- [tests.h:864-869](../tests/tests.h#L864-L869)

这个测试相比前一个：

- task 数更少
- 每个 task 更重
- launch 级固定成本相对被摊薄

所以它能帮你判断：

- 你的问题到底是任务过细导致的
- 还是整体调度结构本身就不够高效

**在 Lab 2 中的体现**

如果某实现：

- 在 `math_operations...` 慢
- 在 `fewer_tasks` 变好很多

往往说明主要问题在于：

- 任务粒度太细时调度成本过高

---

### 概念 8：`math_operations_in_tight_for_loop_fan_in` —— 异步依赖图中的 fan-in 模式

**定义**

该测试先并行运行大量独立的数学任务，然后用一个单独的 reduce 任务汇总结果。异步版本构成一个典型的 fan-in DAG。

**直觉**

它像是在问：

> “你能不能先把很多前置任务并行跑完，再在依赖满足后启动最终汇聚任务？”

**机制**

参考定义：

- [tests/README.md:26-27](../tests/README.md#L26-L27)
- [tests.h:878-953](../tests/tests.h#L878-L953)

结构：

- 先有很多 bulk launch，可并行启动
- 最后一个 reduce launch 依赖所有前面的 launch

这个测试对 Part B 特别关键，因为它验证：

- 依赖计数是否正确
- 所有前驱完成后才能解锁后继
- `sync()` 是否真的等到了最后的 reduce 完成

**在 Lab 2 中的体现**

如果 async 版在这里出错，通常说明：

- 依赖 bookkeeping 不完整
- ready queue / waiting queue 语义没分清

---

### 概念 9：`math_operations_in_tight_for_loop_reduction_tree` —— 二叉树型 DAG，考验依赖传播是否正确

**定义**

这个测试不是单点 fan-in，而是把 reduce 过程组织成二叉树，形成多层依赖图。

**直觉**

它像是在问：

> “你不只是会处理‘很多前驱汇到一个后继’，还会不会正确地逐层传播依赖完成事件？”

**机制**

参考定义：

- [tests/README.md:29-30](../tests/README.md#L29-L30)
- [tests.h:961-1097](../tests/tests.h#L961-L1097)

特点：

- 初始层是很多 math bulk launches
- 中间层不断做 2-to-1 reduce
- 最终变成单个根节点结果

它特别适合暴露：

- 依赖传播延迟
- 完成计数更新错误
- DAG 状态转移 bug

**在 Lab 2 中的体现**

这是比 fan-in 更强的 Part B 依赖调度测试。

> **[RDMA 映射]**
> 这很像分层归约（tree reduction）或多级聚合路径。错误不一定在叶子，而常出在“中间层什么时候被判 ready”。

---

### 概念 10：`spin_between_run_calls` —— 专门用来揭示 spinning 空转伤害

**定义**

该测试在两个轻量 launch 之间插入一个只有 2 个 medium task 的 launch，故意制造“少量线程干活、其他线程没活”的场景。

**直觉**

这个测试是在直接问：

> “没活干的那些线程，会不会站在旁边一直抢 CPU，影响真正干活的人？”

**机制**

参考定义：

- [tests/README.md:32-34](../tests/README.md#L32-L34)
- [tests.h:1099-1171](../tests/tests.h#L1099-L1171)

执行模式：

1. 先一个极轻任务 launch
2. 再一个只有 `2` 个 task 的中等任务 launch
3. 再一个轻任务 launch

问题在于：

- 假设机器有很多 worker
- 但中间那轮只有 2 个 task
- 真正有用的线程只有极少数
- 其余线程如果继续 spinning，就会跟这 2 个有效线程争资源

**在 Lab 2 中的体现**

这个测试是 `ThreadPoolSleeping` 的主场，原因很直接：

- sleeping 版能让没活线程睡下去
- spinning 版天然会比较吃亏

所以它不是在证明 spinning “错了”，而是在揭示：

- spinning 的等待策略有明确代价
- 某些 workload 下这种代价会非常明显
- 学完后你应该能说出：这个测试主要不是测调度固定成本，而是测“空闲线程是否碍事”

---

### 概念 11：`mandelbrot_chunked` —— 单次大任务，更看执行本身而非线程池复用

**定义**

`mandelbrot_chunked` 用一次 bulk launch 计算 Mandelbrot 图像。每个 task 负责一部分图像行，是一个明显的 compute-intensive 单次大任务。

**直觉**

它在问：

> “对于一次性的重型并行任务，你的执行框架能不能稳定跑起来？”

**机制**

参考定义：

- [tests/README.md:35-36](../tests/README.md#L35-L36)
- [tests.h:1180-1241](../tests/tests.h#L1180-L1241)

特点：

- 只有一次 launch
- `128 tasks`
- 每个 task 计算量大

因此：

- thread pool 的“复用”价值不明显
- spawn 和 thread pool 的差距可能不会很大
- 更看 worker 是否真正并行算图

**在 Lab 2 中的体现**

这个测试慢，优先怀疑：

- 并行执行本身不够好
- 分配策略或数据切分造成利用率不足
- 不是先怀疑 launch 固定成本

---

## 二、把测试和典型瓶颈对上号

下面这张表是最实用的“反推表”。以后某个测试慢了，先照这个表定位。


| 测试                       | 优先怀疑什么                                          |
| ------------------------ | ----------------------------------------------- |
| `super_super_light`      | `run()` 固定成本、thread create/join、per-task 调度路径太重 |
| `super_light`            | 调度尾部空转、主线程不参与工作、waiting 路径仍有额外成本                |
| `ping_pong_equal`        | 整体吞吐不足、主线程干扰 worker、共享状态争用                      |
| `ping_pong_unequal`      | 静态分配、动态调度不够灵活、长尾明显                              |
| `recursive_fibonacci`    | CPU 没吃满、真正并行度不够、同步逻辑限制执行                        |
| `math_operations...`     | 高频 launch 下 thread pool 复用收益不明显、批次发布开销过高        |
| `spin_between_run_calls` | spinning 空转影响有用线程、sleeping 语义不够正确               |
| `mandelbrot_chunked`     | 单次大任务并行执行效率低、不是 launch 固定成本问题                   |


---

## 三、Part A 学习顺序建议

如果你现在主要做 Part A，我建议按下面顺序理解测试：

1. **先看 `super_super_light`**
  - 学会识别：调度器是否比任务本身还贵
2. **再看 `super_light`**
  - 学会识别：正确实现和高效实现之间的差别
3. **再看 `ping_pong_unequal`**
  - 学会识别：动态分发的价值
4. **最后看 `spin_between_run_calls`**
  - 学会理解：为什么 Sleeping 版不是“只是更复杂”，而是能解决 Spinning 的真实问题

---

## 四、3 个最重要的复盘问题

### 思考题 1

为什么一个在 `super_super_light` 上表现很好的 `ThreadPoolSpinning`，仍然可能在 `super_light` 上明显落后参考实现？

参考答案 因为 `super_light` 已经进入“调度开销与计算开销同量级”的区域。方向对了不等于已经足够精炼，主线程不参与干活、尾部空转、共享状态访问路径过重等问题会在这里暴露出来。

### 思考题 2

为什么 `ping_pong_unequal` 更能体现 `next_task_id.fetch_add(1)` 这种动态分发策略的价值？

参考答案 因为不同 task 的工作量不一样，静态分块会导致某些线程先闲、某些线程拖尾，而动态领取任务能让空闲线程继续接手剩余工作，缩短尾部。

### 思考题 3

为什么 `spin_between_run_calls` 对 Spinning 版不友好，却对 Sleeping 版很关键？

参考答案 因为该测试故意让中间阶段只有少数线程有实际工作，其余线程如果一直 spinning，会与真正干活的线程竞争 CPU。Sleeping 版能让这些闲线程睡下去，从而减少干扰。

---

## 五、一页速查版

### 按工作负载类型记忆

- **极轻任务**：`super_super_light`
- **轻任务**：`super_light`
- **均衡中量任务**：`ping_pong_equal`
- **不均衡任务**：`ping_pong_unequal`
- **重计算任务**：`recursive_fibonacci`
- **中量计算 + 高频 launch**：`math_operations_in_tight_for_loop`
- **少任务粗粒度变种**：`math_operations_in_tight_for_loop_fewer_tasks`
- **Fan-in DAG**：`math_operations_in_tight_for_loop_fan_in`
- **Reduction Tree DAG**：`math_operations_in_tight_for_loop_reduction_tree`
- **专测 spinning 空转问题**：`spin_between_run_calls`
- **单次大任务**：`mandelbrot_chunked`

### 按“看什么”记忆

- 看 launch 固定成本：`super_super_light`
- 看调度与计算平衡：`super_light`
- 看整体吞吐：`ping_pong_equal`
- 看负载均衡：`ping_pong_unequal`
- 看真正并行算力：`recursive_fibonacci`, `mandelbrot_chunked`
- 看 thread pool 复用收益：`math_operations_in_tight_for_loop`
- 看 waiting strategy：`spin_between_run_calls`

---

## 六、建议你下一步怎么用这份文档

1. 跑 `super_super_light` 和 `super_light`
  - 不只看快慢，要先判断：问题是 launch 还是尾部空转
2. 跑 `ping_pong_unequal`
  - 判断你的任务分发是不是已经足够动态
3. 等你开始做 Sleeping，再重点盯 `spin_between_run_calls`
  - 这是最能解释“为什么 sleeping 值得实现”的测试

---

## 七、与现有文档的衔接

建议结合下面几篇一起看：

- [theory_07_part_a_framework_walkthrough.md](theory_07_part_a_framework_walkthrough.md)
- [theory_02_work_distribution.md](theory_02_work_distribution.md)
- [theory_05_synchronization.md](theory_05_synchronization.md)
- [theory_06_locality_contention.md](theory_06_locality_contention.md)
- [experience_summary.md](experience_summary.md)

