// ============================================================================
// performance_metrics.hpp —— 性能统计模块
//
// ----------------------------------------------------------------------------
// 🎓 概念卡 ㉑：直方图 与 P95/P99 百分位（性能分析的基本功）
// ----------------------------------------------------------------------------
// 【是什么】
// - 直方图（Histogram）：把数值按大小分到几个"桶"里，每桶只记一个计数。
//   例如耗时桶：≤50us、≤100us、≤250us……每个耗时进来就"投"进对应桶。
// - 百分位（Percentile）：把全部样本从小到大排，第 P95 个位置的数值表示
//   "95% 的情况耗时不高于这个值"。P50 是中位数，P99 更能暴露偶发卡顿。
//
// 【为什么在这里】平均耗时会被骗：大部分 0.1ms、偶尔一帧 50ms，
// 平均可能只有 0.2ms，但你实际体验卡顿。P99 = 50ms 才暴露真相。
// 全存下每次耗时太浪费内存，所以用桶近似（误差 = 桶宽）。
//
// 【最小示例（动手做一个简易直方图）】
//   long buckets[5] = {0};            // 5 个桶
//   void record(double ms) {
//       int b = ms < 1 ? 0 : ms < 5 ? 1 : ms < 20 ? 2 : ms < 100 ? 3 : 4;
//       buckets[b]++;
//   }
//   // 之后：哪个桶的计数高，就知道耗时的分布长什么样
//
// 【自己动手要点】
// 1) P95 ≈ "最慢的 5% 之外的边界"，P99 更严格；看服务性能优先看 P99；
// 2) 桶边界要贴合数据分布（本项目 1ms 以内多，所以前几桶分得很细）；
// 3) 百分位是"近似"的：桶越细越准，但占内存越多，要权衡。
// ----------------------------------------------------------------------------
//
// 【这个文件是干什么的】
// 给菜单里的"性能面板"提供数据：采样耗时、序列化耗时、渲染耗时，
// 以及它们的平均值 / P50 / P95 / P99 / 最大值。
//
// 【为什么要 P95/P99 而不是只看平均？】
// 平均耗时会被"大部分时间很快、偶尔一次很慢"骗过去。
// P99 = "99% 的情况耗时不高于这个值"，更能暴露卡顿问题。
// 比如平均 0.1ms 但 P99 是 50ms，说明有 1% 的帧在严重卡顿。
//
// 【实现思路（给初学者）】
// 不保存每次的耗时（内存和时间都浪费），而是用"直方图"：
// 把耗时按大小分到 15 个"桶"里（比如 ≤50us、≤100us、≤250us……），
// 每桶只记一个计数。算百分位数时从最小桶开始累加计数，
// 数到目标位置，那个桶的上界就是近似的百分位值。这是空间换时间。
//
// 【多线程考虑】
// 数据线程在记录、渲染线程在读取，所以计数器都用原子变量。
// ============================================================================

#pragma once

#include <array>        // std::array（定长数组）
#include <atomic>       // 原子变量
#include <chrono>       // 时间
#include <cstddef>      // size_t
#include <cstdint>      // uint64_t
#include <limits>       // std::numeric_limits

namespace performance_metrics
{
    // 一次统计的"快照"（给菜单显示用）
    struct DurationSnapshot
    {
        std::uint64_t samples{};              // 采样总数
        double averageMilliseconds{};         // 平均耗时（毫秒）
        double p50Milliseconds{};             // P50（中位数）
        double p95Milliseconds{};             // P95
        double p99Milliseconds{};             // P99
        double maximumMilliseconds{};         // 最大值
    };

    // ------------------------------------------------------------------
    // DurationHistogram：耗时直方图（核心数据结构）
    // ------------------------------------------------------------------
    class DurationHistogram final
    {
    public:
        // record：记录一次耗时
        // 模板参数 Rep/Period 来自 std::chrono 的时长类型，任何精度都能用
        template <typename Rep, typename Period>
        void record(const std::chrono::duration<Rep, Period> duration) noexcept
        {
            // 统一换算成微秒（1/1000 毫秒）
            const auto measured = std::chrono::duration_cast<
                std::chrono::microseconds>(duration).count();
            const std::uint64_t microseconds = measured <= 0
                ? 0
                : static_cast<std::uint64_t>(measured);

            // 累计样本数和总耗时（原子累加）
            samples_.fetch_add(1, std::memory_order_relaxed);
            totalMicroseconds_.fetch_add(
                microseconds,
                std::memory_order_relaxed);

            // 更新最大值：compare_exchange_weak 是"如果还是旧值就换成新值，
            // 否则重试"的原子操作，多线程竞争时安全
            std::uint64_t maximum = maximumMicroseconds_.load(
                std::memory_order_relaxed);
            while (microseconds > maximum &&
                   !maximumMicroseconds_.compare_exchange_weak(
                       maximum,             // 预期值（失败时会自动更新）
                       microseconds,        // 要设置的新值
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }

            // 找到这个耗时属于哪个桶，桶计数 +1
            buckets_[bucketIndex(microseconds)].fetch_add(
                1,
                std::memory_order_relaxed);
        }

        // snapshot：算出一份统计快照（给菜单显示）
        [[nodiscard]] DurationSnapshot snapshot() const noexcept
        {
            DurationSnapshot result;
            result.samples = samples_.load(std::memory_order_relaxed);
            if (result.samples == 0) {            // 还没有任何样本
                return result;
            }

            // 平均 = 总微秒 / 样本数 / 1000（转毫秒）
            result.averageMilliseconds =
                static_cast<double>(totalMicroseconds_.load(
                    std::memory_order_relaxed)) /
                static_cast<double>(result.samples) /
                1000.0;
            // 各百分位
            result.p50Milliseconds = percentileMilliseconds(
                result.samples,
                50);
            result.p95Milliseconds = percentileMilliseconds(
                result.samples,
                95);
            result.p99Milliseconds = percentileMilliseconds(
                result.samples,
                99);
            result.maximumMilliseconds =
                static_cast<double>(maximumMicroseconds_.load(
                    std::memory_order_relaxed)) /
                1000.0;
            return result;
        }

        // reset：清零所有统计
        void reset() noexcept
        {
            samples_.store(0, std::memory_order_relaxed);
            totalMicroseconds_.store(0, std::memory_order_relaxed);
            maximumMicroseconds_.store(0, std::memory_order_relaxed);
            for (auto& bucket : buckets_) {
                bucket.store(0, std::memory_order_relaxed);
            }
        }

    private:
        // ------------------------------------------------------------------
        // 15 个桶的上界（微秒）。最后一个桶是"无穷大"，装所有超大的值。
        // 50us, 100us, 250us, 500us, 1ms, 2ms, 4ms, 8ms, 12ms, 16ms,
        // 24ms, 33ms, 50ms, 100ms, ∞
        // ------------------------------------------------------------------
        inline static constexpr std::array<std::uint64_t, 15>
            upperBoundsMicroseconds{
                50, 100, 250, 500, 1'000,
                2'000, 4'000, 8'000, 12'000, 16'000,
                24'000, 33'000, 50'000, 100'000,
                std::numeric_limits<std::uint64_t>::max()
            };

        // 根据耗时（微秒）算出它属于第几个桶
        [[nodiscard]] static constexpr std::size_t bucketIndex(
            const std::uint64_t microseconds) noexcept
        {
            for (std::size_t index = 0;
                 index < upperBoundsMicroseconds.size();
                 ++index) {
                if (microseconds <= upperBoundsMicroseconds[index]) {
                    return index;
                }
            }
            return upperBoundsMicroseconds.size() - 1;   // 兜底：最后一个桶
        }

        // 计算第 percentile 百分位的近似值（毫秒）。
        // 方法：从最小桶开始累加计数，数到"目标名次"停在哪桶，
        // 那桶的上界就是近似值（最后一桶则用实际最大值）。
        [[nodiscard]] double percentileMilliseconds(
            const std::uint64_t samples,
            const std::uint64_t percentile) const noexcept
        {
            // 目标名次：例如 1000 个样本的 P95 = 第 950 个
            const std::uint64_t target =
                (samples * percentile + 99) / 100;
            std::uint64_t accumulated = 0;
            for (std::size_t index = 0;
                 index < upperBoundsMicroseconds.size();
                 ++index) {
                accumulated += buckets_[index].load(
                    std::memory_order_relaxed);
                if (accumulated >= target) {
                    const std::uint64_t upper =
                        upperBoundsMicroseconds[index];
                    // 最后一桶（无穷大）：用实际最大值代替
                    if (upper == std::numeric_limits<std::uint64_t>::max()) {
                        return static_cast<double>(maximumMicroseconds_.load(
                            std::memory_order_relaxed)) /
                            1000.0;
                    }
                    return static_cast<double>(upper) / 1000.0;  // 微秒→毫秒
                }
            }
            return 0.0;
        }

        // 成员变量（全部原子，多线程安全）
        std::atomic<std::uint64_t> samples_{0};            // 样本数
        std::atomic<std::uint64_t> totalMicroseconds_{0};  // 总微秒
        std::atomic<std::uint64_t> maximumMicroseconds_{0};// 最大微秒
        // 每个桶的计数（和上界数组长度一致）
        std::array<std::atomic<std::uint64_t>,
            upperBoundsMicroseconds.size()> buckets_{};
    };

    // ------------------------------------------------------------------
    // 全局统计对象（inline = 头文件里定义全局变量不报重复定义）
    // ------------------------------------------------------------------
    inline DurationHistogram samplingDuration;       // 内存采样耗时
    inline DurationHistogram serializationDuration;  // 快照序列化耗时
    inline DurationHistogram renderCpuDuration;      // 渲染 CPU 耗时
    inline std::atomic<std::uint64_t> missedSamplingDeadlines{0}; // 错过采样截止时间次数
    inline std::atomic<std::uint64_t> missedRenderDeadlines{0};   // 错过渲染截止时间次数
    inline std::atomic<int> samplingRateHz{0};       // 当前采样频率
    inline std::atomic<int> radarRateHz{0};          // 当前雷达频率

    // 新一轮会话开始时清零全部统计
    inline void resetSession() noexcept
    {
        samplingDuration.reset();
        serializationDuration.reset();
        renderCpuDuration.reset();
        missedSamplingDeadlines.store(0, std::memory_order_relaxed);
        missedRenderDeadlines.store(0, std::memory_order_relaxed);
        samplingRateHz.store(0, std::memory_order_relaxed);
        radarRateHz.store(0, std::memory_order_relaxed);
    }
}
