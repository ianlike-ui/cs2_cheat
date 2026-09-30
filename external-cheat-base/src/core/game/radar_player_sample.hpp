// ============================================================================
// radar_player_sample.hpp —— 玩家采样数据的"清洗与保持"逻辑
//
// 【这个文件是干什么的】
// 从游戏内存读到的原始数据是"脏"的：可能读到一半游戏内存变了（撕裂读取）、
// 可能是非法数值（NaN/无穷大/天文数字）、可能坐标是 0,0,0（实体初始化中）。
// 本文件负责：
//   1. 校验数据合法性（坐标不能是 NaN、不能超过世界大小等）；
//   2. 判断两帧读到的是不是同一个玩家（用稳定的身份字段比对）；
//   3. 玩家"休眠/死亡"时保持上一次已知位置（防止标记乱飘）。
//
// 【给 C++ 初学者的概念】
// - [[nodiscard]]：提醒调用者"别丢掉这个返回值"（编译器会警告）。
// - constexpr：编译期就能算出的函数。
// - friend：允许某个函数/类访问本类型的私有成员。
// ============================================================================

#pragma once

#include "core/game/game_snapshot.hpp"   // WorldPosition 等定义

#include <cmath>        // std::isfinite（判断是否是正常数字）
#include <cstdint>      // 整数类型
#include <optional>     // std::optional

namespace game::radar_player_sample
{
    // ------------------------------------------------------------------
    // 数据合法性阈值常量
    // ------------------------------------------------------------------
    // 世界坐标绝对值超过这个数就认为是损坏数据。同时仍然接受恰好为
    // {0,0,0} 的合法原点（当没有更好数据可用时）。见 choosePosition 注释。
    inline constexpr float kWorldCoordinateLimit = 10000000.0f;   // 世界坐标上限
    inline constexpr float kZeroPositionEpsilon = 0.001f;         // "几乎为0"的判断阈值
    inline constexpr float kMaximumAbsoluteYawDegrees = 1000000.0f; // 朝向角的合法上限
    inline constexpr std::uint64_t kLastKnownMaximumAgeMs = 150;  // "上次已知位置"最大可用时长(ms)

    // 一次"读位置"的结果：是否成功 + 读到的值
    struct PositionRead
    {
        bool succeeded{};            // 读成功了吗
        WorldPosition value{};       // 读到的坐标
    };

    // 一个玩家的"身份信息"：用来判断两次采样是不是同一个人
    struct PlayerSampleIdentity
    {
        std::uint64_t playerId{};            // 不透明玩家 ID（见 game_snapshot.hpp）
        std::uintptr_t controllerAddress{};  // 玩家控制器对象的内存地址
        std::uintptr_t pawnAddress{};        // 玩家实体（pawn）的内存地址
        std::uint32_t pawnHandle{};          // 实体句柄
        std::uint8_t team{};                 // 队伍
        std::uint64_t steamId{};             // Steam ID（可能未知）
        bool steamIdKnown{};                 // Steam ID 是否已知

        // friend constexpr operator==：允许两个身份直接 == 比较
        [[nodiscard]] friend constexpr bool operator==(
            const PlayerSampleIdentity&,
            const PlayerSampleIdentity&) noexcept = default;
    };

    // 两个身份是否"稳定身份相同"（用除 Steam ID 外的字段比较）
    [[nodiscard]] inline constexpr bool hasSameStableIdentity(
        const PlayerSampleIdentity& left,
        const PlayerSampleIdentity& right) noexcept
    {
        return left.playerId == right.playerId &&
            left.controllerAddress == right.controllerAddress &&
            left.pawnAddress == right.pawnAddress &&
            left.pawnHandle == right.pawnHandle &&
            left.team == right.team;
    }

    // 两个身份是否"Steam ID 冲突"（都是已知的且不一样 → 说明换了个人）
    [[nodiscard]] inline constexpr bool hasConflictingSteamIdentity(
        const PlayerSampleIdentity& left,
        const PlayerSampleIdentity& right) noexcept
    {
        return left.steamIdKnown && right.steamIdKnown &&
            left.steamId != right.steamId;
    }

    // 这次观察到的身份和缓存的身份是否"兼容"：
    // 稳定身份相同，且没有 Steam ID 冲突。
    // 注意：一次失败的 Steam ID 读取只是"未知"，不是"换了人"的证据，
    // 所以稳定身份仍然可用，直到观察到不同的已知 Steam ID。
    [[nodiscard]] inline constexpr bool isCompatibleIdentityObservation(
        const PlayerSampleIdentity& current,
        const PlayerSampleIdentity& cached) noexcept
    {
        return hasSameStableIdentity(current, cached) &&
            !hasConflictingSteamIdentity(current, cached);
    }

    // 坐标是否合法（不是 NaN/无穷大，绝对值在合理范围内）
    [[nodiscard]] inline bool isValidPosition(
        const WorldPosition& position) noexcept
    {
        // std::isfinite 判断是不是"正常有限数字"（排除 NaN 和无穷大）
        if (!std::isfinite(position.x) ||
            !std::isfinite(position.y) ||
            !std::isfinite(position.z) ||
            std::abs(position.x) >= kWorldCoordinateLimit ||
            std::abs(position.y) >= kWorldCoordinateLimit ||
            std::abs(position.z) >= kWorldCoordinateLimit) {
            return false;
        }

        return true;
    }

    // 朝向角（yaw）是否可用（有限且绝对值合理）
    [[nodiscard]] inline bool isUsableYaw(const float yaw) noexcept
    {
        return std::isfinite(yaw) &&
            std::abs(yaw) <= kMaximumAbsoluteYawDegrees;
    }

    // 坐标是否"几乎为零"（所有分量都接近 0）
    [[nodiscard]] inline bool isZeroPosition(
        const WorldPosition& position) noexcept
    {
        return std::abs(position.x) <= kZeroPositionEpsilon &&
            std::abs(position.y) <= kZeroPositionEpsilon &&
            std::abs(position.z) <= kZeroPositionEpsilon;
    }

    // 坐标是否"合法且非零"
    [[nodiscard]] inline bool isNonZeroValidPosition(
        const WorldPosition& position) noexcept
    {
        return isValidPosition(position) && !isZeroPosition(position);
    }

    // 在两个位置候选里选一个更好的：
    // SceneNode 的绝对原点（m_vecAbsOrigin）是实体的权威世界位置；
    // m_vOldOrigin 只是当 SceneNode 读不到/读不完整时的备胎。
    // 特殊处理：实体初始化时 SceneNode 会短暂出现 {0,0,0}，
    // 这时优先用旧的合法非零位置，但也不全局拒绝 0（因为 0 可能是合法坐标）。
    [[nodiscard]] inline std::optional<WorldPosition> choosePosition(
        const PositionRead& absoluteOrigin,    // 从 SceneNode 读的绝对原点
        const PositionRead& oldOrigin) noexcept// 旧原点（备胎）
    {
        const bool absoluteValid = absoluteOrigin.succeeded &&
            isValidPosition(absoluteOrigin.value);
        const bool oldValid = oldOrigin.succeeded &&
            isValidPosition(oldOrigin.value);

        // 绝对位置有效且非零 → 用它（第一优先）
        if (absoluteValid && !isZeroPosition(absoluteOrigin.value)) {
            return absoluteOrigin.value;
        }
        // 否则旧位置有效且非零 → 用旧的
        if (oldValid && !isZeroPosition(oldOrigin.value)) {
            return oldOrigin.value;
        }
        // 绝对位置有效（哪怕为零）→ 用绝对位置
        if (absoluteValid)
            return absoluteOrigin.value;
        // 旧位置有效 → 用旧的
        if (oldValid)
            return oldOrigin.value;
        // 都不行 → 返回空
        return std::nullopt;
    }

    // 是否可以在采样中断时复用"上次已知位置"：
    // 身份兼容 + 采样时间在当前时间之前 + 数据不太旧（≤ maximumAgeMs）
    [[nodiscard]] inline constexpr bool canReuseLastKnown(
        const PlayerSampleIdentity& currentIdentity,
        const PlayerSampleIdentity& cachedIdentity,
        const std::uint64_t nowMs,
        const std::uint64_t sampledAtMs,
        const std::uint64_t maximumAgeMs =
            kLastKnownMaximumAgeMs) noexcept
    {
        return isCompatibleIdentityObservation(
                currentIdentity,
                cachedIdentity) &&
            sampledAtMs <= nowMs &&
            nowMs - sampledAtMs <= maximumAgeMs;
    }

    // 一个玩家缓存的"上次已知状态"
    struct PlayerLastKnownState
    {
        PlayerSampleIdentity identity{};   // 身份
        bool identityKnown{};              // 身份是否已确定
        bool stateKnown{};                 // 是否有过有效状态
        bool wasAlive{};                   // 上次是否存活
        bool wasDormant{};                 // 上次是否休眠
        std::optional<WorldPosition> position;      // 上次已知位置
        std::uint64_t positionSampledAtMs{};        // 该位置的采样时间
        std::optional<float> yaw;                   // 上次已知朝向
        std::uint64_t yawSampledAtMs{};             // 该朝向的采样时间
        std::uint64_t lastObservedAtMs{};           // 最后观察到的时间
    };

    // 输出：一帧里该玩家最终保留的位置和朝向
    struct RetainedPlayerSample
    {
        std::optional<WorldPosition> position;
        std::optional<float> yaw;
    };

    // ====================================================================
    // retainPlayerSample：核心函数——处理一帧的玩家样本，返回要用的位置/朝向
    // ====================================================================
    // 设计意图（见函数内注释）：
    //   - 活跃玩家：只用短缓存桥接一次撕裂读取；
    //   - 休眠玩家：保留客户端最后一次已知状态；
    //   - 死亡玩家：冻结在最后存活位置（尸体不该移动标记）；
    //   - 同句柄复活：当作"新的状态世代"，防止尸体坐标泄漏到复活点。
    [[nodiscard]] inline RetainedPlayerSample retainPlayerSample(
        PlayerLastKnownState& state,          // 缓存状态（会被更新）
        const PlayerSampleIdentity& identity, // 这次读到的身份
        const std::uint64_t nowMs,            // 当前时间
        const bool alive,                     // 是否存活
        const bool dormant,                   // 是否休眠
        std::optional<WorldPosition> currentPosition,  // 这次读到的位置
        std::optional<float> currentYaw) noexcept      // 这次读到的朝向
    {
        // 先把非法数据剔除（不在源头污染缓存）
        if (currentPosition && !isValidPosition(*currentPosition))
            currentPosition.reset();          // reset() = 变成"空"
        if (currentYaw && !isUsableYaw(*currentYaw))
            currentYaw.reset();

        // 身份是否变了（与缓存不一致）？
        const bool identityChanged = !state.identityKnown ||
            !isCompatibleIdentityObservation(identity, state.identity);
        // "同身份复活"：身份没变，但之前死了、现在活了 → 视为新世代
        const bool revivedWithSameIdentity = !identityChanged &&
            state.stateKnown && !state.wasAlive && alive;
        if (identityChanged || revivedWithSameIdentity)
        {
            state = {};                        // 清空缓存
            state.identity = identity;
            state.identityKnown = true;
        }
        else if (identity.steamIdKnown)
        {
            // 身份没变：如果之前 Steam ID 未知而现在知道了，就补上；
            // 但如果已知身份对应的一次读取失败/未知，保留已有身份
            state.identity.steamId = identity.steamId;
            state.identity.steamIdKnown = true;
        }

        state.lastObservedAtMs = nowMs;
        RetainedPlayerSample result;
        // 休眠或死亡 → 冻结最后已知状态
        const bool freezeLastKnown = dormant || !alive;
        if (freezeLastKnown)
        {
            // 如果这是第一次验证到的样本就已经是休眠/死亡状态，先捕获一次；
            // 之后尸体的移动或休眠内存变化都不能再移动标记
            if (!state.position && currentPosition)
            {
                state.position = currentPosition;
                state.positionSampledAtMs = nowMs;
            }
            if (!state.yaw && currentYaw)
            {
                state.yaw = currentYaw;
                state.yawSampledAtMs = nowMs;
            }
            result.position = state.position;   // 用冻结的位置
            result.yaw = state.yaw;             // 用冻结的朝向
        }
        else
        {
            // 活跃玩家：用新位置；读不到时短暂复用上次已知位置
            if (currentPosition)
            {
                state.position = currentPosition;
                state.positionSampledAtMs = nowMs;
                result.position = currentPosition;
            }
            else if (state.position && canReuseLastKnown(
                identity,
                state.identity,
                nowMs,
                state.positionSampledAtMs))
            {
                result.position = state.position;   // 复用（要求不太旧）
            }

            if (currentYaw)
            {
                state.yaw = currentYaw;
                state.yawSampledAtMs = nowMs;
                result.yaw = currentYaw;
            }
            else if (state.yaw && canReuseLastKnown(
                identity,
                state.identity,
                nowMs,
                state.yawSampledAtMs))
            {
                result.yaw = state.yaw;
            }
        }

        // 记录本帧状态（供下一帧判断"复活"等）
        state.stateKnown = true;
        state.wasAlive = alive;
        state.wasDormant = dormant;
        return result;
    }
}
