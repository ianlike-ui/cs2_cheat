// ============================================================================
// aimbot.hpp —— 自动瞄准模块的头文件（声明）
//
// 【这个模块是干什么的】
// 自动瞄准（Aimbot）：按住热键时，如果准星附近有敌人，自动把视角转向
// 敌人的头/胸等目标骨骼。自动扳机（Triggerbot）：准星指到敌人时自动开枪。
//
// 【重要前提（设计约束）】
// 瞄准靠"模拟鼠标移动"实现（把鼠标挪过去），而不是直接写游戏内存，
// 因为写视角内存通常会被 VAC 检测。而且游戏不在前台时程序会强制关闭
// 瞄准功能（见 main.cpp 数据线程里的逻辑）。
//
// 【给 C++ 初学者的概念】
// - namespace：命名空间，把相关函数归组（这里叫 aimbot）。
// - inline 变量：头文件里定义的全局变量。
// - vec2 / vec3：2D / 3D 向量（定义在 utils/math/vector.hpp）。
// - 前向声明 struct RuntimeConfig：只告诉编译器"有这么个类型"，
//   避免头文件互相包含循环依赖。
// ============================================================================

#pragma once

#include "core/memory/memory.hpp"    // 内存读取（其实瞄准主要用鼠标，这里需要进程信息）
#include "utils/math/vector.hpp"     // vec2 / vec3
#include "offsets.hpp"               // cs2-dumper 生成的偏移量
#include "client_dll.hpp"            // cs2-dumper 生成的类字段偏移
#include <cstdint>
#include <string>                    // 状态提示消息

// 前向声明（避免包含整个 menu.hpp 造成循环依赖）
namespace menu
{
    struct RuntimeConfig;
}

namespace aimbot
{
    // 游戏进程 ID 和 client.dll 基地址（esp::init 初始化后赋值）
    inline uintptr_t pID = 0;
    inline uintptr_t modBase = 0;

    // 初始化瞄准模块（在 esp::init 之后调用）
    bool init();

    // 更新自动瞄准——每帧（数据线程）调用
    void update(const menu::RuntimeConfig& config);

    // 更新自动扳机——每帧（数据线程）调用
    void updateTriggerbot(const menu::RuntimeConfig& config);

    // 启动/停止独立压枪线程（~500Hz 高频读后坐 + 平滑注入，和实体采样解耦）
    void startRcsWorker();
    void stopRcsWorker();

    // 弹道录制/压枪状态提示（游戏内顶部短暂显示）
    void setRcsStatus(const char* message);
    bool getRcsStatus(std::string& outMessage);

    // 更新魔法子弹——每帧（数据线程）调用；开枪时把视角写到敌人身上
    void updateMagicBullet(const menu::RuntimeConfig& config);

    // 更新角色瞬移——每帧（数据线程）调用；把本地位置写到目标坐标
    void updateTeleport(const menu::RuntimeConfig& config);

    // 更新连跳（bhop）——每帧（数据线程）调用；按住空格落地自动起跳
    void updateBhop(const menu::RuntimeConfig& config);

    // 计算"从 src 看向 dst"需要的视角角度（pitch, yaw）
    vec2 calcAngle(const vec3& src, const vec3& dst);

    // 计算目标方向与当前准星方向的角度差（FOV 距离，度）
    float getFOV(const vec2& viewAngle, const vec2& aimAngle);

    // 把角度归一化到 [-180, 180]（防止角度越界）
    float normalizeAngle(float angle);
}
