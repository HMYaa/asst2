# CS149 Lab 2 测试任务详解手册

这份文档不是简单列测试名，而是回答三个更重要的问题：

1. **这个测试到底在做什么计算？**
2. **它为什么会卡住某一类调度器实现？**
3. **如果这个测试慢了，我应该优先怀疑哪里？**

如果你现在看 `runtasks` 里的名字还是有点抽象，这篇文档的目标就是把它们变成你脑子里很具体的“任务画像”。

---

## 一、先记住一个总框架

这些测试大致可以分成 4 类：

| 类别 | 核心问题 | 代表测试 |
|------|----------|----------|
| 极轻任务 / 高频 launch | 调度器自己的固定成本贵不贵 | `super_super_light`, `super_light` |
| 均衡 vs 不均衡负载 | 分发策略是否能压住长尾 | `ping_pong_equal`, `ping_pong_unequal` |
| 重计算任务 | 真正的并行执行能力够不够 | `recursive_fibonacci`, `mandelbrot_chunked` |
| 依赖 / DAG 调度 | 依赖满足时机和 ready 迁移是否正确 | `fan_in`, `reduction_tree`, `spin_between_run_calls` |

所以以后看到某个测试慢，不要只说“性能差”，而是先问：

- 是 `run()` 本身太贵？
- 是 worker 取任务的路径太重？
- 是任务不均衡导致拖尾？
- 还是空闲线程在白白抢 CPU？

---

## 二、测试逐个拆开讲

---

### 1. `super_super_light`

## 它到底做什么？

这个测试本质上来自 `pingPongTest(equal_work=true, base_iters=0)`。

参考位置：
- `tests/README.md:5-6`
- `tests/tests.h:668-677`
- `tests/tests.h:587-666`

它会：

- 分配两个长度为 `32 * 1024` 的整型数组
- 一共执行 **400 次 bulk launch**
- 每次 launch 有 **64 个 task**
- 每个 task 负责自己那一段元素
- 每一轮只是把输入数组拷贝到输出数组，输入输出在下一轮交替

因为 `base_iters = 0`，所以每个元素几乎不做额外计算。

也就是说，这个测试的真实含义是：

> **任务本身几乎没成本，runtime 本身的成本被无限放大。**

## 为什么它重要？

它几乎专门用来打击这些实现：

- 每次 `run()` 都创建线程
- 每个 task 都有复杂封装
- worker 取任务路径太长
- 主线程等待方式太重

## 它最像在问什么？

> “如果 task 轻到几乎没有，你的线程池是不是反而成了负担？”

## 如果它慢，优先怀疑什么？

优先怀疑：

1. `run()` 固定成本太高
2. per-task 分发开销太高
3. 主线程 waiting 路径太重
4. 线程创建/销毁没有被线程池消掉

## 你应该记住的画像

这是一个：
- **极轻任务**
- **高频 launch**
- **完全均衡**
- **几乎纯测调度器开销**

---

### 2. `super_light`

## 它到底做什么？

这个测试与 `super_super_light` 的结构几乎一样：

- 两个 `32 * 1024` 的数组
- 400 次 bulk launch
- 每次 64 个 task

参考位置：
- `tests/README.md:8-9`
- `tests/tests.h:680-689`

不同点在于：

- 每个元素在拷贝前会做若干次轻量计算
- 这里 `base_iters = 32`

所以它不是纯拷贝，而是：

- 一点点计算
- 加上一点点内存操作

## 为什么它比 `super_super_light` 更难？

因为它进入了一个很有代表性的区间：

> **调度开销和任务计算开销开始接近同一个量级。**

这时最容易出现的现象是：

- 你的方向没错
- 线程池也已经起作用
- 但仍然明显落后参考实现

说明问题已经不是“完全不会做”，而是：

- 主线程在白等
- 尾部空转太多
- 共享原子访问仍然太频繁
- batch 状态切换路径还不够紧凑

## 如果它慢，优先怀疑什么？

1. 主线程没有参与干活
2. worker 在尾部空转太猛
3. `next_task_id` / `remaining_tasks` 仍然是热点
4. spinning 带来的资源争用开始变明显

## 你应该记住的画像

这是一个：
- **轻任务，不再是极轻任务**
- **仍然是高频 launch**
- **很适合看“调度和计算的分界点”**

---

### 3. `ping_pong_equal`

## 它到底做什么？

这个测试还是 `pingPongTest`，但数组规模变大了：

- `num_elements = 512 * 1024`
- `base_iters = 32`
- `equal_work = true`
- 64 tasks / 400 launches

参考位置：
- `tests/README.md:11-13`
- `tests/tests.h:692-707`

每个 task 负责一段连续元素区间，对每个元素做相同次数的增量操作后写入另一块 buffer。

## 为什么叫 equal？

因为这里设计成：

- 每个元素工作量差不多
- 每个 task 负责的元素数也差不多
- 所以不同 task 的耗时近似相等

也就是说，这不是在考“谁更会应对不均衡”，而是在考：

> **当任务本来就很均匀时，你能不能把机器稳定吃满。**

## 如果它慢，说明什么？

一般说明：

- worker 吞吐不够高
- 主线程 waiting 会干扰 worker
- 共享状态访问仍然偏重
- 线程池整体执行效率还不够好

## 你应该记住的画像

这是一个：
- **中量级任务**
- **负载均衡**
- **重点看整体吞吐和稳定并行**

---

### 4. `ping_pong_unequal`

## 它到底做什么？

它和 `ping_pong_equal` 是同一个测试框架，但把 `equal_work=false`。

参考位置：
- `tests/README.md:14-15`
- `tests/tests.h:698-713`
- `tests/tests.h:174-203`

这里每个元素实际做的迭代次数不同：

- 越靠前的元素，计算越重
- 越靠后的元素，计算越轻

而 task 拿到的是连续区间，所以不同 task 的总工作量就不一样了。

## 它真正考什么？

它不再主要考 launch 成本，而是在考：

> **当任务耗时不均匀时，你的调度器能不能避免长尾。**

如果你用静态分块：

- 有些线程早早做完
- 有些线程一直拖着最重的区间
- 最终整体时间被最慢线程决定

如果你用动态分发，比如 `next_task_id.fetch_add(1)`：

- 空闲线程可以继续拿后续任务
- 利用率会更高

## 如果它慢，优先怀疑什么？

1. 静态分配导致长尾
2. 动态分发粒度太粗
3. 任务单位太大，虽然动态但仍拖尾
4. worker 收尾阶段没有把空闲线程利用起来

## 你应该记住的画像

这是一个：
- **不均衡任务**
- **长尾问题测试**
- **最能体现动态调度价值的 workload**

---

### 5. `recursive_fibonacci`

## 它到底做什么？

每个 task 都计算一次递归 Fibonacci。

参考位置：
- `tests/README.md:17-18`
- `tests/tests.h:721-777`

参数：

- 每次 launch 有 256 个 task
- 一共 30 次 launch
- 每个 task 调 `slowFn(idx_)`
- 这是一个故意很慢的递归实现

## 为什么这个测试和前面不同？

因为这里 task 本身非常重。

所以：

- 原子操作的成本相对被淹没
- `run()` 的固定开销相对被淹没
- 真正重要的是 CPU 核有没有被吃满

它像是在问：

> “如果任务足够重，你的线程池是否真的在并行计算，而不是只是在并行调度？”

## 如果它慢，优先怀疑什么？

1. worker 没有真正并行跑满
2. 主线程没参与，少了一份算力
3. 同步逻辑不当限制了并行度
4. 线程数和机器有效并行度不匹配

## 你应该记住的画像

这是一个：
- **重计算任务**
- **调度开销相对不敏感**
- **更看“有没有真正并行起来”**

---

### 6. `math_operations_in_tight_for_loop`

## 它到底做什么？

每个 task 会对输出数组的一段元素做一堆数学运算：

- `exp`
- `log`
- multiply
- add

参考位置：
- `tests/README.md:20-21`
- `tests/tests.h:785-862`
- `tests/tests.h:248-284`

参数：

- 每次 launch：16 个 task
- 输出数组大小：512
- 总共：2000 次 launch

## 这个测试为什么很关键？

因为它不是极轻任务，也不是极重任务，而是：

- 每次 launch 有一定计算量
- 但 launch 次数又非常多

所以它会同时考两件事：

1. thread pool 复用是否真的节省了 create/join 成本
2. 你的 batch 发布路径是不是足够轻

它像是在问：

> “如果我要连续做 2000 轮中量计算，线程池有没有真正体现价值？”

## 如果它慢，优先怀疑什么？

1. `run()` 每次发布 batch 的成本太大
2. waiting 路径太重
3. worker 获取任务路径仍然偏贵
4. thread pool 相比 spawn 没明显节省成本

## 你应该记住的画像

这是一个：
- **中量计算**
- **高频 launch**
- **专门验证线程池复用价值**

---

### 7. `math_operations_in_tight_for_loop_fewer_tasks`

## 它到底做什么？

它还是上一个测试，但每次 launch 的 task 数从 16 变成了 9。

参考位置：
- `tests/README.md:23-24`
- `tests/tests.h:864-869`

含义是：

- 总工作量差不多
- 但每个 task 变得更粗
- 粒度更大
- 负载也会略微不均

## 它为什么有用？

这个测试特别适合帮你判断：

> **问题到底是“任务太细”，还是“整体结构本身就不够高效”。**

如果你的实现：

- 在 `math_operations_in_tight_for_loop` 上很慢
- 但在 `fewer_tasks` 上明显变好

那大概率说明：

- 你被 task 粒度过细拖垮了
- 调度成本在细粒度场景下太高

## 你应该记住的画像

这是一个：
- **粗粒度版本对照组**
- **专门帮助判断任务粒度问题**

---

### 8. `math_operations_in_tight_for_loop_fan_in`

## 它到底做什么？

这个测试包含两类工作：

1. 先启动很多独立的数学计算 bulk launch
2. 最后启动一个 reduce 任务，把前面的结果汇总

参考位置：
- `tests/README.md:26-27`
- `tests/tests.h:878-953`

参数：

- 计算任务：64 tasks / launch
- 共有 256 个这样的 launch
- 每个输出向量大小 2048
- 最后有 1 个 reduce task 依赖所有计算 launch

## 它考什么？

它在 Part B 里是典型的 **fan-in DAG**：

- 前面很多节点互相独立
- 最后一个节点依赖所有前驱

它像是在问：

> “你会不会在所有前驱都真的完成后，才正确地释放后面的归约任务？”

## 如果 async 版出错，常见原因是什么？

1. 依赖计数没减对
2. 前驱完成后没有正确把后继移入 ready 队列
3. `sync()` 没有真的等到 reduce 完成

## 你应该记住的画像

这是一个：
- **多前驱汇聚到单后继**
- **Part B 的经典 fan-in 依赖图**

---

### 9. `math_operations_in_tight_for_loop_reduction_tree`

## 它到底做什么？

它和 `fan_in` 很像，但不是“很多前驱直接汇到一个 reduce”，而是组织成一棵二叉归约树。

参考位置：
- `tests/README.md:29-30`
- `tests/tests.h:961-1097`

也就是说：

- 底层先有很多 math launch
- 然后成对 reduce
- reduce 的结果再成对 reduce
- 最后逐层汇到根节点

## 它为什么比 fan-in 更难？

因为它不只要求你处理“最后一个后继”，而是要求你能正确处理：

- 多层 ready 迁移
- 中间层完成触发下一层
- 依赖传播的层层释放

它像是在问：

> “你的 DAG 调度不是只会处理一层依赖，而是会不会正确地逐层传播完成事件？”

## 你应该记住的画像

这是一个：
- **多层树状依赖图**
- **Part B 更强的依赖传播测试**

---

### 10. `spin_between_run_calls`

## 它到底做什么？

这个测试非常有代表性，结构是：

1. 一个很轻的 launch
2. 一个只有 2 个 task 的中等计算 launch
3. 再一个很轻的 launch

参考位置：
- `tests/README.md:32-34`
- `tests/tests.h:1099-1171`

## 它真正考什么？

这个测试是专门设计来揭示：

> **当只有少量线程在干活时，其余 spinning 线程会不会反过来拖累有效工作线程。**

比如：

- 机器上有很多 worker
- 但中间那轮只有 2 个 task
- 真正有活的线程只有 2 个左右
- 其余线程如果一直 spin，就会继续消耗 CPU 资源

## 为什么它对 sleeping 版特别重要？

因为 Sleeping 版的目标就是：

- 没活线程睡下去
- 不要和有活线程抢资源

所以这个测试可以很好地回答：

> “Sleeping 版到底解决了 Spinning 版的什么真实问题？”

## 你应该记住的画像

这是一个：
- **专门暴露 spinning 空转代价的测试**
- **Sleeping 版的主场**

---

### 11. `mandelbrot_chunked`

## 它到底做什么？

这是一次性的单次大任务：

- 只做 1 次 bulk launch
- 一共 128 个 task
- 每个 task 负责若干图像行
- 计算 Mandelbrot 分形图

参考位置：
- `tests/README.md:35-36`
- `tests/tests.h:1180-1241`

## 它为什么和前面的很多测试不同？

因为这里只有 **一次 launch**。

这意味着：

- thread pool 的“复用”优势没那么大
- spawn 和 thread pool 的差距未必非常明显
- 真正重要的是单次大任务能否高效并行执行

它像是在问：

> “如果是一次性的大型计算任务，你能不能把 task 切好并稳定跑满机器？”

## 如果它慢，优先怀疑什么？

1. worker 并行执行效率不高
2. 数据切分不理想
3. 线程利用率不够
4. 不是先怀疑 launch 固定开销

## 你应该记住的画像

这是一个：
- **单次大任务**
- **重计算**
- **重点看执行本身，不是线程池复用**

---

## 三、最实用的一张速查表

| 测试 | 最核心的画像 | 主要在拷问什么 |
|------|--------------|----------------|
| `super_super_light` | 极轻任务 + 高频 launch | 调度器自己的固定成本 |
| `super_light` | 轻任务 + 高频 launch | 调度与计算的平衡点 |
| `ping_pong_equal` | 均衡中量任务 | 整体吞吐 |
| `ping_pong_unequal` | 不均衡任务 | 动态调度与长尾 |
| `recursive_fibonacci` | 重计算任务 | 真正并行度 |
| `math_operations_in_tight_for_loop` | 中量计算 + 2000 次 launch | 线程池复用收益 |
| `math_operations_in_tight_for_loop_fewer_tasks` | 更粗粒度对照组 | 是否被细粒度调度成本拖垮 |
| `fan_in` | 多前驱汇一后继 | 依赖释放是否正确 |
| `reduction_tree` | 多层 DAG | 依赖传播是否正确 |
| `spin_between_run_calls` | 少量线程干活，其余线程空闲 | spinning 空转伤害 |
| `mandelbrot_chunked` | 单次大任务 | 大任务并行执行效率 |

---

## 四、如果某个测试慢了，该怎么反推

### 如果 `super_super_light` 慢
先怀疑：
- `run()` 固定成本
- task 分发路径
- waiting 路径
- 线程创建成本是否还存在

### 如果 `super_light` 慢
先怀疑：
- 主线程纯等待
- worker 尾部空转
- 共享原子热点

### 如果 `ping_pong_unequal` 慢
先怀疑：
- 分发策略太静态
- 动态分发粒度不合适
- 长尾没有被抹平

### 如果 `recursive_fibonacci` 慢
先怀疑：
- 并行度没吃满
- CPU 利用率不足
- 主线程没参与计算

### 如果 `spin_between_run_calls` 慢
先先别慌，这通常说明：
- spinning 的等待策略本来就会在这种场景吃亏
- 这是 Sleeping 版该赢的测试

---

## 五、建议你的学习顺序

如果你现在想把这些 workload 真正吃透，建议按下面顺序理解：

1. `super_super_light`
   - 先理解“调度器也有成本”

2. `super_light`
   - 再理解“不是方向对了就够，还要足够精简”

3. `ping_pong_equal` / `ping_pong_unequal`
   - 对比理解“吞吐”和“负载均衡”是两回事

4. `recursive_fibonacci`
   - 理解“重任务时，瓶颈从调度转向真正并行执行”

5. `spin_between_run_calls`
   - 理解“为什么 sleeping 不是锦上添花，而是解决真实问题”

6. `fan_in` / `reduction_tree`
   - 为 Part B 建立 DAG 调度直觉

---

## 六、你复习时最值得反复问自己的 5 个问题

- [ ] 这个测试主要是在测任务本身，还是在测 runtime 自己？
- [ ] 这个测试里的 task 是均衡的还是不均衡的？
- [ ] 这个测试更看 launch 固定成本，还是更看 worker 吞吐？
- [ ] 这个测试里空闲线程会不会成为负担？
- [ ] 这个测试如果是 async，依赖图形状是什么？

---

## 七、一句话版本

如果你只记一句：

> `super_super_light` 看调度器自己贵不贵，`super_light` 看调度和计算的平衡，`ping_pong_unequal` 看长尾，`recursive_fibonacci` 看真并行，`spin_between_run_calls` 看 spinning 的代价，`fan_in/reduction_tree` 看 Part B 的依赖调度。`
