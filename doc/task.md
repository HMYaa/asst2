
## CS149 Assignment 2：从零构建任务执行库

这是 **Stanford CS149 并行编程课程** 的第二个实验，核心目标是在多核 CPU 上实现一个高效的 C++ 任务调度执行库。

---

### 🏗️ 项目架构

```
asst2/
├── part_a/          # A部分：同步批量任务启动
│   ├── itasksys.h   # 核心接口定义（ITaskSystem / IRunnable）
│   ├── tasksys.h    # 学生实现的类声明
│   └── tasksys.cpp  # 学生实现的类（当前需填充 TODO）
├── part_b/          # B部分：支持 DAG 任务图的异步调度
├── tests/           # 测试用例（tests.h / main.cpp）
├── doc/             # 配套学习文档（已有8篇理论笔记）
└── tutorial/        # C++ 并发原语教程
```

---

### 📐 核心接口（`itasksys.h`）

```cpp
// 可运行任务抽象接口
class IRunnable {
    virtual void runTask(int task_id, int num_total_tasks) = 0;
};

// 任务系统接口
class ITaskSystem {
    virtual void run(IRunnable*, int num_total_tasks) = 0;           // 同步批量执行
    virtual TaskID runAsyncWithDeps(IRunnable*, int, vector<TaskID>&) = 0; // 异步+依赖
    virtual void sync() = 0;                                          // 等待全部完成
};
```

---

### 📋 需要实现的三个类（递进难度）

| 类名 | 策略 | 难点 | 分值 |
|------|------|------|------|
| `TaskSystemParallelSpawn` | 每次 `run()` 都 spawn 线程 | 动态 vs 静态任务分配 | 10分 |
| `TaskSystemParallelThreadPoolSpinning` | 预创建线程池，工作线程自旋等待 | `run()` 如何知道所有任务完成？ | 20分 |
| `TaskSystemParallelThreadPoolSleeping` | 线程池 + 条件变量睡眠 | 避免 CPU 空转，消灭竞态条件 | 20分 |

---

### 🔗 Part B：任务图（DAG）调度

Part B 扩展 `TaskSystemParallelThreadPoolSleeping`，支持依赖关系声明：

```
launchA (128 tasks)
    ├── launchB (2 tasks)   ─┐
    └── launchC (6 tasks)   ─┤─→ launchD (32 tasks)
```

关键设计：维护**等待队列**（依赖未满足）和**就绪队列**（可立即执行），在任务完成时检查依赖图，将新就绪的任务推入就绪队列。

---

### 🧪 测试体系

测试覆盖多种工作负载特征：

| 测试名 | 特征 | 考察重点 |
|--------|------|----------|
| `super_super_light` | 极轻量任务，400次批量启动 | 线程池启动开销 |
| `mandelbrot_chunked` | 单次启动128个计算密集型任务 | 并行加速比 |
| `ping_pong_equal` | 均匀负载，64任务×400次 | 负载均衡 |
| `ping_pong_unequal` | 非均匀负载 | 动态调度优势 |
| `math_operations_*` | 含依赖关系的任务图 | Part B 调度正确性 |

---

### 📊 性能评分标准

```
PERF = 学生实现时间 / 参考实现时间

< 1.0  → 比参考更快 ✅
≤ 1.2  → Part A 满分 ✅
≤ 1.5  → Part B 满分 ✅
```

---

### 📚 学习资料（`doc/` 目录）

项目已有8篇配套理论文档：
- `theory_01` ~ `theory_06`：多核架构、工作分配、缓存一致性、同步原语、局部性等
- `theory_07`：Part A 框架源码走读
- `theory_08`：Task Scheduler 完整生命周期分析

---

**当前代码状态**：Part A 的3个并行类均只有串行 `for` 循环占位，`TaskSystemParallelThreadPoolSleeping` 已有线程池骨架但 `run()` 逻辑存在缺陷（主线程调用 `worker_thread()` 会因 `stop_` 标志逻辑导致死循环），需要重新设计。
