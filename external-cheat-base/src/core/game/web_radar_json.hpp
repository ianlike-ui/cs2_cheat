// ============================================================================
// web_radar_json.hpp —— 把游戏快照序列化成 JSON 的头文件（声明）
//
// 【这个文件是干什么的】
// 网页雷达是浏览器里的网页，它不懂 C++ 结构体，只认 JSON 字符串。
// 所以要把 GameSnapshot（游戏快照）翻译成 JSON。
// 真正的翻译逻辑在 web_radar_json.cpp 里，这个文件只做"声明"。
// ============================================================================

#pragma once

#include "core/game/fixed_map_radar.hpp"   // MapDefinition（地图定义）
#include "core/game/game_snapshot.hpp"     // GameSnapshot

#include <string>     // std::string

namespace game::web_radar_json
{
    // "发给网页时让谁可见"的策略（隐私选项）
    enum class TeamViewPolicy
    {
        all,               // 所有队伍都可见
        localTeamOnly,     // 只发本队
        opponentsOnly      // 只发对方
    };

    // 序列化选项：三个隐私开关
    struct SerializationOptions
    {
        bool includePlayerNames{ true };   // 是否带玩家名
        bool includeSteamIds{ false };     // 是否带 Steam ID（默认不带）
        TeamViewPolicy teamViewPolicy{ TeamViewPolicy::all };  // 队伍可见策略

        // 允许直接 == 比较（缓存命中判断要用）
        bool operator==(const SerializationOptions&) const = default;
    };

    // 序列化一帧"绝对状态"（非增量）快照。字段顺序固定（保证可缓存）。
    // 注意：uint64 的玩家 ID 在 JSON 里写成"字符串"，
    // 因为 JavaScript 的整数精度只有 2^53，直接传数字会丢精度。
    [[nodiscard]] std::string serializeSnapshotV1(
        const GameSnapshot& snapshot,
        const SerializationOptions& options = {});

    // 序列化一份地图定义（给网页雷达加载地图参数用）
    [[nodiscard]] std::string serializeMapDefinitionV1(
        const fixed_map_radar::MapDefinition& map);
}
