// ============================================================================
// game_layout.hpp —— 游戏内存布局的"手工补充常量"集中地
//
// 【这个文件是干什么的】
// 前面说过，程序运行时要读游戏内存，地址 = 模块基地址 + 偏移量。
// 大部分偏移量来自 cs2-dumper 自动生成的 client_dll.hpp（编译期引用）。
// 但 cs2-dumper 并没有导出每一个容器/内存布局细节（比如"实体列表"是
// 怎么分块的、骨骼数组的步长是多少），这些值需要逆向时手工确认。
// 本文件就是把"不得不手工维护的少数常量"集中在一个地方，方便审计，
// 并且尽量与自动生成的偏移量组合使用（见下面的函数）。
//
// 【给 C/C++ 初学者的概念】
// - #pragma once：头文件只被编译一次的保险（防止重复包含报错）。
// - constexpr：编译期常量。写上它，编译器能在编译时就算出值。
// ============================================================================

#pragma once

// 引入 cs2-dumper 生成的偏移量头文件（里面是 CBasePlayerPawn 等类的字段偏移）
#include "client_dll.hpp"
#include <cstddef>      // std::ptrdiff_t 等
#include <cstdint>      // uintptr_t 等

// 注释：cs2-dumper 并没有导出外部读取器需要的每一个容器/布局细节。
// 把不可避免的值集中在一个可审计的位置，并尽可能与生成的 schema 偏移组合使用。
namespace game_layout
{
    // ------------------------------------------------------------------
    // 实体列表（EntityList）的分块结构常量
    // ------------------------------------------------------------------
    // CS2 把游戏里所有实体（玩家、投掷物、武器…）放在一个"大列表"里。
    // 这个列表被分成很多"块"（chunk），每块存若干实体。为了快速找实体，
    // 外部工具要理解它的分块方式。下面这些常量就是分块参数。
    constexpr uintptr_t ENTITY_LIST_CHUNK_ARRAY = 0x10;   // 块指针数组在列表结构体里的偏移
    constexpr uintptr_t ENTITY_IDENTITY_STRIDE = 0x70;    // 每块里两个实体之间的距离（字节）
    constexpr uint32_t ENTITY_CHUNK_SHIFT = 9;            // 用右移 9 位算出实体在哪个块
    constexpr uint32_t ENTITY_CHUNK_MASK = 0x1FF;         // 用 & 0x1FF 算出实体在块内位置
    constexpr uint32_t ENTITY_HANDLE_MASK = 0x7FFF;       // 实体句柄的最大值（32767 个实体）
    constexpr uint32_t FIRST_PLAYER_CONTROLLER = 1;       // 第一个玩家控制器的编号
    constexpr uint32_t LAST_PLAYER_CONTROLLER = 64;       // 最后一个玩家控制器编号（最多 64 人）
    constexpr uint32_t FIRST_WORLD_ENTITY =               // 非玩家实体的起始编号
        LAST_PLAYER_CONTROLLER + 1;
    constexpr uint32_t MAX_WORLD_ENTITIES = ENTITY_HANDLE_MASK;  // 世界实体的最大数量

    // ------------------------------------------------------------------
    // 骨骼与全局变量
    // ------------------------------------------------------------------
    constexpr uintptr_t BONE_ARRAY_IN_MODEL_STATE = 0x80; // 骨骼数组指针在模型状态里的偏移
    constexpr size_t BONE_STRIDE = 0x20;                  // 两个骨骼记录之间的距离（字节）
    constexpr uintptr_t GLOBAL_VARS_CURRENT_TIME = 0x2C;  // 全局变量里"当前时间"的偏移
    // CGlobalVarsBase::mapname（当前地图名）cs2-dumper 没导出。
    // 这个指针偏移是隔离维护的，用之前会先校验指向的字符串是否合法。
    constexpr uintptr_t GLOBAL_VARS_MAP_NAME = 0x188;

    // ------------------------------------------------------------------
    // 下面是"组合函数"：把手工常量与生成的 schema 偏移量相加
    // ------------------------------------------------------------------

    // 敌人"是否被标记（spotted）"标志位的偏移。
    // 直接从 cs2-dumper 生成的 EntitySpottedState_t 结构里取 m_bSpotted 字段的偏移。
    constexpr uintptr_t spottedFlagOffset()
    {
        return static_cast<uintptr_t>(
            cs2_dumper::schemas::client_dll::
                EntitySpottedState_t::m_bSpotted);
    }

    // 骨骼数组指针的偏移 = 模型状态字段偏移(m_modelState) + 骨骼数组在模型状态里的偏移
    constexpr uintptr_t boneArrayPointerOffset()
    {
        return static_cast<uintptr_t>(
            cs2_dumper::schemas::client_dll::
                CSkeletonInstance::m_modelState) +
            BONE_ARRAY_IN_MODEL_STATE;
    }

}
