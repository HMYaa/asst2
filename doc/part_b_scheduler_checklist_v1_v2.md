# Part B 调度器自检清单（V1 + V2）

本文档用于 `part_b/tasksys.cpp` 的日常迭代与调试复盘。  
目标：先保正确性，再做性能优化；每次改动都能快速闭环。

---

## V1：提交前并发自检（功能 + 语义）

### A. 核心正确性（7 条）

- [ ] **发布原子性**：`runAsyncWithDeps()` 中“建节点 + 建边 + 写 `unfinished_deps` + 入 `ready_queue_`”在同一把 `graph_mtx_` 内完成。
- [ ] **就绪条件唯一**：只有 `unfinished_deps` 从 `1 -> 0` 时入队，不存在绕过依赖直接入队。
- [ ] **0-task 闭环**：`num_tasks == 0` 一定触发 `complete_bulk_launch`（注册时或 worker 分支），不会卡死后继。
- [ ] **complete 单次触发**：普通 batch 仅在 `remaining.fetch_sub(...) == 1` 时 complete，避免重复传播/重复 `outstanding--`。
- [ ] **wait 带谓词**：`cv_work_`、`cv_sync_` 的 `wait` 全带谓词，防伪唤醒。
- [ ] **notify 时机**：能锁外 notify 的都锁外，减少空唤醒后抢锁。
- [ ] **sync 语义稳定**：`outstanding_tasks_` 加减粒度一致（当前为 per-batch），`sync()` 只在归零后返回。

### B. 最小测试组合（每次改动后）

- [ ] `./runtasks -n 16 simple_test_async`
- [ ] `./runtasks -n 16 strict_diamond_deps_async`
- [ ] `./runtasks -n 16 strict_graph_deps_large_async`
- [ ] `./runtasks -n 16 super_light`

### C. 固定原则

- [ ] **先正确性后性能**：若优化影响依赖顺序、完成判定或 wait/notify 协议，先回退再重做。

---

## V2：调试版自检（卡住 / 长尾 / 惊群）

当出现“偶发卡住、性能忽高忽低、尾延迟变大”时，进入本节。

### A. 先判现象类型（不要混着查）

- [ ] **正确性类**：死锁、漏通知、任务重复/漏执行、依赖错序。
- [ ] **性能类**：CPU 空转高、P99 长尾、线程多反而变慢。

> 规则：每轮只验证一个假设，避免同时改多处导致证据失真。

### B. 六个低侵入计数器（建议临时加）

- [ ] `cnt_ready_push`：每次 `ready_queue_.push_back` 递增。
- [ ] `cnt_ready_pop`：每次 `ready_queue_.pop_front` 递增。
- [ ] `cnt_complete`：每次 `complete_bulk_launch_unlocked` 递增。
- [ ] `cnt_wait_work`：worker 进入 `cv_work_.wait` 前递增。
- [ ] `cnt_wakeup_work`：worker 从 `cv_work_.wait` 返回后递增。
- [ ] `cnt_zero_task_complete`：走 `n==0` 分支时递增。

期望关系（粗检查）：

- [ ] 长时间运行后 `cnt_ready_push >= cnt_ready_pop`，且差值不应无限增长。
- [ ] `cnt_complete` 应接近已提交 batch 数（异常偏小通常说明收尾路径丢失）。
- [ ] `cnt_wait_work` 与 `cnt_wakeup_work` 不应出现极端失衡（提示漏通知或无效唤醒）。

### C. 三个关键断言（调试期开启）

- [ ] `assert(d < id)`：依赖只能指向历史节点（你已有）。
- [ ] 在 pop 后断言 `ready_count_ >= 0`（防计数下溢）。
- [ ] 在 `complete` 路径对同一 batch 的重复 complete 做防护（例如临时 debug 标记）。

### D. 卡住排查顺序（固定流程）

- [ ] 看 `sync()` 卡住时 `outstanding_tasks_` 是否长期 > 0。
- [ ] 若 > 0：查是否有 batch 从未触发 `complete`（看 `cnt_complete` 与提交数）。
- [ ] 若 complete 数正常：查依赖传播是否丢失（后继 `unfinished_deps` 未降到 0）。
- [ ] 若传播正常：查 worker 是否睡死在 `cv_work_`（`cnt_wait_work` 高但 wakeup 低）。

### E. 长尾排查顺序（固定流程）

- [ ] 看 `notify_all` 频率是否过高（可能惊群）。
- [ ] 看 `ready_count_` 在空闲期是否长期为 0 但仍高锁竞争（说明无效探测）。
- [ ] 看 `remaining` 收尾是否拖到很后（最后一个 copy 成为尾部瓶颈）。
- [ ] 分别对比 `-n 8/16/32`，确认是扩展性问题还是单机偶发抖动。

### F. V2 最小实验脚本（手动执行）

- [ ] 功能回归：`./runtasks -n 16 strict_diamond_deps_async`
- [ ] 图规模：`./runtasks -n 16 strict_graph_deps_large_async`
- [ ] 轻任务延迟：`./runtasks -n 16 super_light`
- [ ] 线程扩展：`for n in 8 16 32; do ./runtasks -n $n super_light; done`

### G. 调试结论模板（每轮 4 句）

1. 现象：卡住/长尾/吞吐回退（选一）。
2. 证据：本轮 1-2 个关键计数器或测试输出。
3. 结论：最可能根因（只写一个）。
4. 下一步：只做一个最小改动并复测。

---

## 快速使用建议

- 日常开发先跑 **V1**。
- 一旦出现“偶发/抖动/难复现”，切到 **V2**。
- V2 计数器在问题定位后及时移除，避免污染性能结果。

