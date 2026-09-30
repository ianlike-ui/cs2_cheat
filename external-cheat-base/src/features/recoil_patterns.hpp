// ============================================================================
// recoil_patterns.hpp —— 枪械后坐弹道录制 / 回放模式表
// ============================================================================
// 【这个模块做什么】
// 1. 用快捷键录制某把枪整梭子每一发的后坐偏移（m_predictableBaseAngle 逐发采样，
//    存的是"每一发子弹相对瞄准方向偏了多少"，角度单位，与 aimPunch 一致）；
// 2. 持久化保存到 %LOCALAPPDATA%\AegisCS2\recoil_patterns.txt（每枪一份）；
// 3. 之后遇到同款枪，压枪线程取已记录的前 N 发直接按记录弹道回放补偿，
//    N 发之后回退到实时后坐方案。
//
// 【线程安全】压枪线程负责录音（每发 recordBullet）和回放（find），UI 线程负责
// 查看/清除/保存。内部用互斥锁保护，两方安全。
// ============================================================================

#pragma once

#include <string>
#include <vector>
#include "utils/math/vector.hpp"

namespace recoil_patterns
{
    // 一把枪的记录：每发子弹的 aimPunch（x=pitch, y=yaw，角度）。
    struct Pattern
    {
        int weaponIndex = 0;          // 武器定义索引（如 AK-47=7）
        std::string weaponName;       // 武器名（UI 显示用）
        std::vector<vec2> bullets;    // bullets[N] = 第 N+1 发的累积后坐（角度）
    };

    // ---- 查询（压枪线程回放用，线程安全）----
    // 返回当前武器已录制的模式；无则 nullptr。指针在锁内使用期间有效。
    const Pattern* find(int weaponIndex);

    // 给 UI 用的快照（复制一份，不持有内部锁）
    std::vector<Pattern> snapshotAll();

    // ---- 录音（压枪线程调用）----
    bool isRecording();
    int recordingWeaponIndex();
    int recordingBulletCount();
    void startRecording(int weaponIndex, const std::string& weaponName);
    void recordBullet(float pitch, float yaw);   // 每打一发调用一次
    // 停止录音：把录到的 >=1 发的模式入库并保存，返回是否保存成功
    bool stopRecording();
    void cancelRecording();

    // ---- 管理（UI 线程调用）----
    void removePattern(int weaponIndex);
    void clearAll();

    // ---- 持久化 ----
    void loadFromDisk();
    void saveToDisk();
}
