// False Sharing 性能对比测试
// 编译: g++ -O2 -std=c++17 -pthread false_sharing_demo.cpp -o false_sharing_demo
// 运行: ./false_sharing_demo

#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <iostream>
#include <cstring>

// Cache line 大小（x86_64 通常是 64 字节）
constexpr size_t CACHE_LINE_SIZE = 64;
constexpr int NUM_THREADS = 16;
constexpr int ITERATIONS = 10000000;  // 每个线程的更新次数

// ============================================
// 版本 1: 有 False Sharing 问题
// 16 个 atomic<int> 紧挨着，共 64 字节 = 恰好一个 cache line
// ============================================
struct BadStats {
    std::atomic<int> counter[NUM_THREADS];  // 16 * 4 = 64 字节
};

// ============================================
// 版本 2: 修复 False Sharing
// 每个 counter 独占一个 cache line (64 字节对齐 + padding)
// ============================================
struct alignas(CACHE_LINE_SIZE) PaddedCounter {
    std::atomic<int> value{0};
    char padding[CACHE_LINE_SIZE - sizeof(std::atomic<int>)];  // 填充到 64 字节
};

struct GoodStats {
    PaddedCounter counter[NUM_THREADS];  // 每个元素保证在独立的 cache line
};

// ============================================
// 测试函数
// ============================================
template<typename Stats>
auto run_test(const char* name) {
    Stats stats;

    // 初始化
    for (int i = 0; i < NUM_THREADS; i++) {
        if constexpr (std::is_same_v<Stats, BadStats>) {
            stats.counter[i].store(0);
        } else {
            stats.counter[i].value.store(0);
        }
    }

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ITERATIONS; i++) {
                if constexpr (std::is_same_v<Stats, BadStats>) {
                    stats.counter[t].fetch_add(1, std::memory_order_relaxed);
                } else {
                    stats.counter[t].value.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 验证结果正确性
    long long total = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        if constexpr (std::is_same_v<Stats, BadStats>) {
            total += stats.counter[i].load();
        } else {
            total += stats.counter[i].value.load();
        }
    }

    std::cout << name << ":\n";
    std::cout << "  耗时: " << duration.count() << " ms\n";
    std::cout << "  总更新次数: " << total << " (预期: " << (long long)NUM_THREADS * ITERATIONS << ")\n";

    return duration.count();
}

// ============================================
// 内存布局分析
// ============================================
void analyze_memory_layout() {
    std::cout << "========== 内存布局分析 ==========\n\n";

    std::cout << "BadStats (有 False Sharing):\n";
    std::cout << "  sizeof(std::atomic<int>) = " << sizeof(std::atomic<int>) << " bytes\n";
    std::cout << "  NUM_THREADS = " << NUM_THREADS << "\n";
    std::cout << "  总大小 = " << sizeof(BadStats) << " bytes\n";
    std::cout << "  Cache line 数量 = " << (sizeof(BadStats) + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE << "\n";
    std::cout << "  问题: 所有 " << NUM_THREADS << " 个线程同时写同一个 cache line!\n\n";

    std::cout << "PaddedCounter:\n";
    std::cout << "  sizeof(PaddedCounter) = " << sizeof(PaddedCounter) << " bytes\n";
    std::cout << "  alignof(PaddedCounter) = " << alignof(PaddedCounter) << " bytes\n";
    std::cout << "  保证每个 counter 在独立的 cache line\n\n";

    std::cout << "GoodStats:\n";
    std::cout << "  总大小 = " << sizeof(GoodStats) << " bytes\n";
    std::cout << "  Cache line 数量 = " << (sizeof(GoodStats) + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE << "\n\n";
}

// ============================================
// 实际地址查看
// ============================================
void show_addresses() {
    std::cout << "========== 实际地址验证 ==========\n\n";

    BadStats bad;
    GoodStats good;

    std::cout << "BadStats 各 counter 的地址:\n";
    for (int i = 0; i < NUM_THREADS; i++) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(&bad.counter[i]);
        uintptr_t cache_line = addr / CACHE_LINE_SIZE;
        std::cout << "  counter[" << i << "] @ " << addr
                  << " -> cache line " << cache_line
                  << (i == 0 ? "" : (cache_line == reinterpret_cast<uintptr_t>(&bad.counter[0]) / CACHE_LINE_SIZE ? " ❌ 同一行!" : " ✅ 不同行"))
                  << "\n";
    }

    std::cout << "\nGoodStats 各 counter 的地址:\n";
    for (int i = 0; i < NUM_THREADS; i++) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(&good.counter[i]);
        uintptr_t cache_line = addr / CACHE_LINE_SIZE;
        std::cout << "  counter[" << i << "] @ " << addr
                  << " -> cache line " << cache_line
                  << (i == 0 ? "" : (cache_line == reinterpret_cast<uintptr_t>(&good.counter[0]) / CACHE_LINE_SIZE ? " ❌ 同一行!" : " ✅ 不同行"))
                  << "\n";
    }
    std::cout << "\n";
}

// ============================================
// 主函数
// ============================================
int main() {
    std::cout << "========================================\n";
    std::cout << "False Sharing 性能对比测试\n";
    std::cout << "========================================\n";
    std::cout << "Thread count: " << NUM_THREADS << "\n";
    std::cout << "Iterations per thread: " << ITERATIONS << "\n";
    std::cout << "Cache line size: " << CACHE_LINE_SIZE << " bytes\n\n";

    // 内存布局分析
    analyze_memory_layout();

    // 地址验证
    show_addresses();

    std::cout << "========== 性能测试 ==========\n\n";

    // 运行测试
    auto bad_time = run_test<BadStats>("版本 1: 有 False Sharing");
    std::cout << "\n";
    auto good_time = run_test<GoodStats>("版本 2: 修复 False Sharing");

    // 加速比
    std::cout << "\n========== 结果汇总 ==========\n";
    std::cout << "加速比: " << (double)bad_time / good_time << "x\n";
    std::cout << "(修复后更快 = 这个数字 > 1)\n\n";

    // 理论解释
    std::cout << "========== 原理说明 ==========\n";
    std::cout << "\nFalse Sharing 发生的原因:\n";
    std::cout << "1. CPU 缓存以 cache line (64 字节) 为单位\n";
    std::cout << "2. 16 个 atomic<int> 紧挨着，占据同一个 cache line\n";
    std::cout << "3. 每个线程写自己的计数器时，触发 MESI 协议的 Modify 状态\n";
    std::cout << "4. 导致其他 15 个 CPU 的缓存行无效化 (Invalidate)\n";
    std::cout << "5. 结果: 所有线程串行化访问同一个 cache line，性能衰减 5-20 倍\n\n";

    std::cout << "修复方法:\n";
    std::cout << "- 使用 alignas(64) 让每个计数器独占一个 cache line\n";
    std::cout << "- padding 填充到 64 字节，确保没有两个计数器在同一行\n\n";

    return 0;
}
