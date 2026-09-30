// ============================================================================
// fixed_map_radar.hpp —— "固定全地图雷达"的数学逻辑
//
// 【这个文件是干什么的】
// 游戏里叠加的雷达是一张"北向上"的固定地图图片。要把玩家的世界坐标
// (world.x, world.y, world.z) 画到这张图上，需要：
//   1. 世界坐标 → 图片像素坐标（投影，Source 引擎的 overview 公式）；
//   2. 多层地图（Nuke 的上下两层）时，根据玩家高度 z 选哪层图；
//   3. 玩家朝向（yaw）→ 雷达图标旋转角度。
//
// 【概念速览】std::optional 见概念卡 ⑰、lambda 见概念卡 ⑫（或《搭建路线图与概念索引.md》）。
// std::numeric_limits<float>::max()：float 能表示的最大正数（"无穷大哨兵值"）。
// ============================================================================

#pragma once

#include "core/game/game_snapshot.hpp"   // GameSnapshot、WorldPosition 等

#include <algorithm>    // std::clamp、std::find_if
#include <cmath>        // std::isfinite、std::fmod、std::cos 等
#include <cstddef>      // std::size_t
#include <limits>       // std::numeric_limits
#include <optional>     // std::optional
#include <string>       // std::string
#include <vector>       // std::vector

namespace game::fixed_map_radar
{
    // ------------------------------------------------------------------
    // MapLevel：一张地图的一个"楼层"（比如 Nuke 的 ground/underground）
    // ------------------------------------------------------------------
    // 每一层只负责一段高度范围 [minimumZ, maximumZ)。
    // 注：[a, b) 是数学记法，表示"从 a 到 b，含 a 不含 b"。
    // 在分界点上，上层的图拥有这个点。如果高度落在所有范围之外，
    // 就选"最近的一层"，保证实体不会消失。
    struct MapLevel
    {
        std::string id;           // 楼层 ID（如 "default"、"lower"）
        std::string imagePath;    // 这层图对应的图片文件路径

        float minimumZ{ -std::numeric_limits<float>::max() };  // 本层高度下限
        float maximumZ{ std::numeric_limits<float>::max() };   // 本层高度上限
    };

    // ------------------------------------------------------------------
    // MapDefinition：一张完整地图的定义
    // ------------------------------------------------------------------
    // 核心是 Source 引擎的 overview 变换公式（把世界坐标映射到图片像素）：
    //   pixelX = (worldX - originX) / scale
    //   pixelY = (originY - worldY) / scale
    // 注意 pixelY 是 originY 减 worldY（世界 Y 向上，图片 Y 向下，所以是反的）。
    // origin/scale 是每个地图的校准参数，由外部脚本从开源数据生成。
    struct MapDefinition
    {
        std::string id;                 // 地图 ID（如 "de_dust2"）
        std::string displayName;        // 显示名（如 "Dust2"）
        std::string imagePath;          // 主图路径

        float originX{};                // 世界坐标 → 图片的平移参数 X
        float originY{};                // 世界坐标 → 图片的平移参数 Y
        float scale{ 1.0f };            // 缩放比例（米/像素）
        float pixelWidth{ 1024.0f };    // 图片宽度（像素）
        float pixelHeight{ 1024.0f };   // 图片高度（像素）
        std::vector<MapLevel> levels;   // 楼层列表（单层地图为空）
    };

    // 一个玩家在地图上的"归一化位置"（0~1 范围，方便叠加层使用）
    struct NormalizedPosition
    {
        float x{};                       // 归一化 X（0=最左，1=最右）
        float y{};                       // 归一化 Y（0=最上，1=最下）
        bool valid{};                    // 投影是否成功
        bool inside{};                   // 是否在地图图片范围内
        std::optional<std::size_t> levelIndex;  // 选中的楼层下标
    };

    // 屏幕上朝向箭头的方向（归一化向量）
    struct ScreenDirection
    {
        float x{};
        float y{};
        bool valid{};
    };

    // 坐标的每个分量都是正常有限数字吗？
    [[nodiscard]] inline bool isFinite(const WorldPosition& position) noexcept
    {
        return std::isfinite(position.x)
            && std::isfinite(position.y)
            && std::isfinite(position.z);
    }

    // 地图定义是否合法（所有数值正常、尺寸为正、缩放为正）
    [[nodiscard]] inline bool isValid(const MapDefinition& map) noexcept
    {
        return std::isfinite(map.originX)
            && std::isfinite(map.originY)
            && std::isfinite(map.scale)
            && std::isfinite(map.pixelWidth)
            && std::isfinite(map.pixelHeight)
            && map.scale > 0.0f
            && map.pixelWidth > 0.0f
            && map.pixelHeight > 0.0f;
    }

    // 楼层定义是否合法（上下限正常且下限 < 上限）
    [[nodiscard]] inline bool isValid(const MapLevel& level) noexcept
    {
        return std::isfinite(level.minimumZ)
            && std::isfinite(level.maximumZ)
            && level.minimumZ < level.maximumZ;
    }

    // ------------------------------------------------------------------
    // selectReferenceZ：选一个"参考高度"来决定显示哪层地图
    // ------------------------------------------------------------------
    // 多层地图（Nuke）需要知道玩家大概在哪层，才能显示对应的楼层图。
    // 规则（注释里也说明原因）：
    //   - 优先用"存活的本机玩家"的高度；
    //   - 其次是"被观察的存活玩家"；
    //   - 再按本队存活的玩家；
    //   - 最后任何存活玩家。
    // 特别地：死亡的本机玩家不能把多层地图钉在它死的那层（可能正在看别的玩家）。
    [[nodiscard]] inline std::optional<float> selectReferenceZ(
        const GameSnapshot& snapshot) noexcept
    {
        // 小工具函数：从玩家快照里取合法的高度（没有就返回空）
        const auto validZ = [](const PlayerSnapshot& player)
            -> std::optional<float>
        {
            if (!player.position || !isFinite(*player.position))
                return std::nullopt;
            return player.position->z;
        };

        // 小工具函数：按玩家 ID 在快照的玩家列表里找到对应的玩家
        const auto findPlayer = [&snapshot](
            const std::optional<std::uint64_t>& id) -> const PlayerSnapshot*
        {
            if (!id)
                return nullptr;
            const auto player = std::find_if(
                snapshot.players.begin(),
                snapshot.players.end(),
                [id](const PlayerSnapshot& candidate)
                {
                    return candidate.id == *id;
                });
            return player == snapshot.players.end() ? nullptr : &*player;
        };

        const PlayerSnapshot* localPlayer =
            findPlayer(snapshot.localPlayerId);
        const PlayerSnapshot* observedPlayer =
            findPlayer(snapshot.observedPlayerId);

        // 1. 存活的本机玩家
        if (localPlayer && localPlayer->alive)
        {
            if (const auto z = validZ(*localPlayer))
                return z;
        }

        // 2. 被观察的存活玩家
        if (observedPlayer && observedPlayer->alive)
        {
            if (const auto z = validZ(*observedPlayer))
                return z;
        }

        // 3. 本队任意存活玩家（参考队 = 本机玩家队伍或快照里的本队）
        const Team referenceTeam = localPlayer &&
                (localPlayer->team == Team::Terrorists ||
                 localPlayer->team == Team::CounterTerrorists)
            ? localPlayer->team
            : snapshot.localTeam;
        if (referenceTeam == Team::Terrorists ||
            referenceTeam == Team::CounterTerrorists)
        {
            for (const PlayerSnapshot& player : snapshot.players)
            {
                if (player.alive && player.team == referenceTeam)
                {
                    if (const auto z = validZ(player))
                        return z;
                }
            }
        }

        // 4. 任意存活玩家
        for (const PlayerSnapshot& player : snapshot.players)
        {
            if (player.alive)
            {
                if (const auto z = validZ(player))
                    return z;
            }
        }

        // 兜底（回合结束/死亡镜头）：没人活着时有位置就用本机玩家最后的高度，
        // 再退到任意玩家的位置
        if (localPlayer)
        {
            if (const auto z = validZ(*localPlayer))
                return z;
        }
        for (const PlayerSnapshot& player : snapshot.players)
        {
            if (const auto z = validZ(player))
                return z;
        }
        return std::nullopt;   // 实在没有就返回空
    }

    // ------------------------------------------------------------------
    // selectLevel：根据高度 z 选择楼层
    // ------------------------------------------------------------------
    // 如果 z 正好落在某层范围 → 返回这层。
    // 如果超出所有范围 → 返回"离得最近"的一层（防止实体消失）。
    // 返回下标（可选），空表示选不出来。
    [[nodiscard]] inline std::optional<std::size_t> selectLevel(
        const MapDefinition& map,
        float z) noexcept
    {
        if (!std::isfinite(z) || map.levels.empty())
            return std::nullopt;

        std::optional<std::size_t> nearestIndex;
        float nearestDistance = std::numeric_limits<float>::infinity();

        for (std::size_t index = 0; index < map.levels.size(); ++index)
        {
            const MapLevel& level = map.levels[index];
            if (!isValid(level))
                continue;

            // 正好落在范围内 → 直接选它
            if (z >= level.minimumZ && z < level.maximumZ)
                return index;

            // 否则算"离这层的距离"，记录最近的一层
            const float distance = z < level.minimumZ
                ? level.minimumZ - z
                : z - level.maximumZ;

            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestIndex = index;
            }
        }

        return nearestIndex;
    }

    // ------------------------------------------------------------------
    // selectLevelWithHysteresis：带"滞后"的楼层选择
    // ------------------------------------------------------------------
    // 问题：玩家站在两层分界线附近，z 值轻微抖动会导致楼层来回切换，
    // 画面闪个不停。解法：记录上一次选的楼层，只要 z 还在旧楼层边界附近
    // （±hysteresis 范围内），就继续用旧楼层，只有明确跑远了才切换。
    // 这叫"滞后"（hysteresis），是控制系统的经典技巧。
    [[nodiscard]] inline std::optional<std::size_t>
    selectLevelWithHysteresis(
        const MapDefinition& map,
        const float z,
        const std::optional<std::size_t> previousLevel,
        const float hysteresis = 24.0f) noexcept
    {
        const std::optional<std::size_t> selected = selectLevel(map, z);
        // 各种"无法比较"的情况直接返回朴素结果
        if (!selected || !previousLevel ||
            *previousLevel >= map.levels.size() ||
            *selected == *previousLevel ||
            !std::isfinite(hysteresis) || hysteresis <= 0.0f) {
            return selected;
        }

        const MapLevel& previous = map.levels[*previousLevel];
        if (!isValid(previous)) {
            return selected;
        }
        // 还在旧楼层边界附近 → 维持旧楼层
        if (z >= previous.minimumZ - hysteresis &&
            z < previous.maximumZ + hysteresis) {
            return previousLevel;
        }
        return selected;   // 跑远了 → 切到新楼层
    }

    // ------------------------------------------------------------------
    // levelSelectionConfidence：楼层选择的"置信度"
    // ------------------------------------------------------------------
    // 玩家离楼层边界越近，楼层判断越不可靠（可能刚换层）。
    // 返回 0~1 的置信度：离边界越远越接近 1。
    // 网页雷达可以据此把标记调透明一点，表示"位置可信度低"。
    [[nodiscard]] inline float levelSelectionConfidence(
        const MapDefinition& map,
        const float z,
        const std::optional<std::size_t> levelIndex,
        const float boundaryRange = 24.0f) noexcept
    {
        if (!levelIndex || *levelIndex >= map.levels.size() ||
            !std::isfinite(z) || !std::isfinite(boundaryRange) ||
            boundaryRange <= 0.0f) {
            return 0.0f;
        }
        const MapLevel& level = map.levels[*levelIndex];
        if (!isValid(level)) {
            return 0.0f;
        }
        // 到最近边界的距离（下限和上限哪个近用哪个）
        const float boundaryDistance = std::min(
            std::abs(z - level.minimumZ),
            std::abs(level.maximumZ - z));
        // 距离 / 范围，夹在 0~1 之间。距离 ≥ range 就是 1（完全可信）
        return std::clamp(boundaryDistance / boundaryRange, 0.0f, 1.0f);
    }

    // ------------------------------------------------------------------
    // project：世界坐标 → 地图上的归一化位置（核心投影函数）
    // ------------------------------------------------------------------
    [[nodiscard]] inline NormalizedPosition project(
        const MapDefinition& map,
        const WorldPosition& world) noexcept
    {
        NormalizedPosition result;                 // 默认全部无效
        if (!isValid(map) || !isFinite(world))
            return result;

        // Source 引擎 overview 公式：世界坐标 → 图片像素坐标
        const float pixelX = (world.x - map.originX) / map.scale;
        const float pixelY = (map.originY - world.y) / map.scale;
        // 归一化：除以图片宽高 → 0~1 范围
        result.x = pixelX / map.pixelWidth;
        result.y = pixelY / map.pixelHeight;

        // 归一化结果必须还是正常数字
        if (!std::isfinite(result.x) || !std::isfinite(result.y))
            return NormalizedPosition{};

        result.valid = true;
        // 是否在地图图片范围内（0~1 之间）
        result.inside = result.x >= 0.0f && result.x <= 1.0f
            && result.y >= 0.0f && result.y <= 1.0f;
        // 顺便选楼层（多层地图）
        result.levelIndex = selectLevel(map, world.z);
        return result;
    }

    // 把角度归一化到 [0, 360) 度（比如 370° → 10°）
    [[nodiscard]] inline float wrapDegrees(float degrees) noexcept
    {
        if (!std::isfinite(degrees))
            return std::numeric_limits<float>::quiet_NaN();   // 非法输入返回 NaN

        float wrapped = std::fmod(degrees, 360.0f);  // 取 360 的余数
        if (wrapped < 0.0f)
            wrapped += 360.0f;                       // 负数转正
        if (wrapped == 0.0f)
            return 0.0f; // 把 -0.0 归一化成 +0.0
        return wrapped;
    }

    // ------------------------------------------------------------------
    // yawToCssRotation：游戏朝向角 → CSS 旋转角度（给网页用）
    // ------------------------------------------------------------------
    // 坐标系说明：
    //   CS 的 yaw = 0 指向"东"，且正方向是逆时针；
    //   CSS 的旋转是顺时针；图标默认朝上时，东 = 90 度。
    // spriteOffsetDegrees 允许给"画法不同"的图标做一次性修正。
    // 公式：90 - yaw + offset，再归一化到 [0,360)。
    [[nodiscard]] inline float yawToCssRotation(
        float yawDegrees,
        float spriteOffsetDegrees = 0.0f) noexcept
    {
        if (!std::isfinite(yawDegrees)
            || !std::isfinite(spriteOffsetDegrees))
        {
            return std::numeric_limits<float>::quiet_NaN();
        }

        return wrapDegrees(90.0f - yawDegrees + spriteOffsetDegrees);
    }

    // ------------------------------------------------------------------
    // yawToScreenDirection：游戏朝向角 → 屏幕方向向量（给叠加层画箭头用）
    // ------------------------------------------------------------------
    // 在北向上的固定地图里：CS 的 yaw=0 指向屏幕右方（东），
    // 正方向朝屏幕上方（北）。
    // 结果向量与图标本身的画法无关（是纯数学方向）。
    [[nodiscard]] inline ScreenDirection yawToScreenDirection(
        float yawDegrees) noexcept
    {
        if (!std::isfinite(yawDegrees))
            return {};

        constexpr float degreesToRadians =
            3.14159265358979323846f / 180.0f;     // 角度→弧度系数
        const float radians = yawDegrees * degreesToRadians;
        return ScreenDirection{
            std::cos(radians),                    // 屏幕 X 分量
            -std::sin(radians),                   // 屏幕 Y 分量（Y 轴向下所以取负）
            true
        };
    }
}
