// ============================================================================
// runtime_timing.hpp —— 采样频率的计算规则
//
// 【这个文件是干什么的】
// 程序里所有"多久采样一次数据"的策略都集中在这里，用纯函数（编译期就能算）
// 表达，方便单元测试。核心思想是"按需分配"：
//   - 需要低延迟的功能（自动瞄准）开着 → 240 Hz 高频采样；
//   - 只画 ESP → 跟随显示器刷新率（避免画面和游戏不同步）；
//   - 没人看雷达 → 低频（4 Hz）甚至不采样，省 CPU。
//
// 【给 C++ 初学者的概念】
// - struct：一种"把几个变量打包"的结构体。这里的 struct 没有函数，
//   只是用来把一组相关参数打包传参，相当于"参数包"。
// - constexpr 函数：能在编译期求值的函数，效率极高，且可被测试。
// - std::clamp(value, min, max)：把 value 限制在 min 和 max 之间。
// ============================================================================

#pragma once

#include <algorithm>    // std::clamp
#include <chrono>       // std::chrono::milliseconds / nanoseconds

namespace runtime_timing
{
    // ------------------------------------------------------------------
    // SamplingDemand：描述"当前需要什么样的采样频率"的一组条件
    // ------------------------------------------------------------------
    struct SamplingDemand
    {
        bool latencySensitive = false;    // 延迟敏感（瞄准/扳机开着）
        bool displaySynchronized = false; // 需要和显示器同步（画 ESP）
        bool periodic = false;            // 周期性功能（炸弹计时、防闪光）
        int idleRate = 20;                // 空闲时的兜底频率（Hz）
    };

    // ------------------------------------------------------------------
    // RadarDemand：描述"雷达当前被谁使用"的一组条件
    // ------------------------------------------------------------------
    struct RadarDemand
    {
        bool localOverlay = false;    // 游戏内叠加的本地雷达开着
        bool embeddedService = false; // 内嵌网页服务开着
        bool embeddedViewers = false; // 有网页观众正在看
        bool publicRelay = false;     // 公网中继开着
        bool recording = false;       // 正在录制
        bool foreground = true;       // 游戏是否在前台
    };

    // ------------------------------------------------------------------
    // dataSamplingRate：根据需求算出"实体数据采样频率"（Hz）
    // ------------------------------------------------------------------
    constexpr int dataSamplingRate(
        int displayRefreshRate,              // 显示器刷新率（如 144）
        const SamplingDemand demand)
    {
        if (demand.latencySensitive) {       // 最优先：瞄准/扳机 → 240 Hz
            return 240;
        }
        if (demand.displaySynchronized) {    // 画 ESP → 跟随刷新率（60~240）
            return std::clamp(displayRefreshRate, 60, 240);
        }
        if (demand.periodic) {               // 周期性功能 → 60 Hz
            return 60;
        }
        return std::clamp(demand.idleRate, 1, 60);  // 兜底：空闲频率
    }

    // ------------------------------------------------------------------
    // radarSamplingRate：根据"谁在看雷达"算出雷达采样频率（Hz）
    // ------------------------------------------------------------------
    // 规则：
    //   前台 + 有人看/在用 → 20 Hz（实时观感最好）
    //   后台 + 有观众/中继 → 10 Hz（省点 CPU 但还能看）
    //   只有服务开着但没人看 → 4 Hz（保持心跳，几乎不耗资源）
    //   完全没人用 → 0（不采样）
    constexpr int radarSamplingRate(const RadarDemand demand)
    {
        if (demand.foreground &&
            (demand.localOverlay || demand.embeddedViewers ||
             demand.publicRelay || demand.recording)) {
            return 20;
        }
        if (!demand.foreground &&
            (demand.embeddedViewers || demand.publicRelay)) {
            return 10;
        }
        if (demand.embeddedService) {
            return 4;
        }
        return 0;
    }

    // 玩家元信息（名字、血条等）的刷新间隔：
    // 延迟敏感时 33ms 刷一次（≈30Hz，让敌人刚探出掩体就尽快进入列表，降低"漏"检测延迟），
    // 否则 250ms（减少不必要的内存读取）。
    constexpr std::chrono::milliseconds metadataRefreshInterval(
        const bool latencySensitive)
    {
        return latencySensitive
            ? std::chrono::milliseconds(33)
            : std::chrono::milliseconds(250);
    }

    // 把采样频率（Hz）换算成"每次间隔多少纳秒"：
    // 1 秒 = 10 亿纳秒，除以频率就是每次的间隔。
    // std::max(1, rate) 防止频率为 0 时除以零。
    constexpr std::chrono::nanoseconds intervalForRate(int rate)
    {
        return std::chrono::nanoseconds(
            1'000'000'000LL / std::max(1, rate));
    }
}
