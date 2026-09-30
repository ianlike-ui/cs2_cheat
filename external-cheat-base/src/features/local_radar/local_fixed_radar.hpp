// ============================================================================
// local_fixed_radar.hpp —— 游戏内叠加的"固定全地图雷达"（声明）
//
// 【这个文件是干什么的】
// 除了网页雷达，程序还能在游戏画面上直接叠加一张"北向上"的固定地图雷达，
// 实时显示所有玩家和 C4 的位置/朝向。渲染由 ImGui 的绘图列表完成。
// 真正的实现在 local_fixed_radar.cpp。
// ============================================================================

#pragma once

#include "core/game/game_snapshot.hpp"   // GameSnapshot（玩家、C4 等数据）

namespace local_fixed_radar
{
    // 雷达的渲染配置（位置、大小、是否显示名字）
    struct RenderConfig
    {
        float anchorX{ 0.02f };    // 雷达左上角在屏幕上的水平位置比例（0~1）
        float anchorY{ 0.08f };    // 雷达左上角的垂直位置比例
        float sizeFraction{ 0.32f };// 雷达边长占视口短边的比例
        float markerSize{ 12.0f };  // 玩家标记（圆点）的基础大小
        bool showNames{ true };     // 是否在标记旁显示玩家名
    };

    // 必须在 SDL/ImGui 渲染线程里调用。
    // 渲染器消费的是一份"不可变快照"（和网页雷达同一份），它绝不直接读游戏内存。
    void render(
        const game::GameSnapshot& snapshot,
        const RenderConfig& config);

    // 在 SDL_Renderer 被销毁前释放当前的地图纹理。
    // 即使没有加载任何地图，调用它也是安全的。
    void reset() noexcept;
}
