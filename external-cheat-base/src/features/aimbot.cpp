// ============================================================================
// aimbot.cpp —— 自动瞄准 + 自动扳机 的实现
//
// 【工作原理（重要）】
// 瞄准的实现方式不是"直接写游戏的视角内存"，而是**模拟鼠标移动**：
//   1. 算出"当前视角"和"目标视角"（朝向敌人头部/胸部）的差值；
//   2. 把这个差值换算成鼠标要移动的像素量（除以鼠标灵敏度系数）；
//   3. 用 mouse_event 注入鼠标移动，游戏就像玩家自己甩鼠标一样。
// 这种方式比写内存难检测，但精度受灵敏度/DPI 影响，所以菜单里有
// "Mouse Sensitivity" 设置让玩家校准。
//
// 【平滑度】
// aimbotSmoothing 控制每帧只移动目标差值的一部分（指数平滑，
// timeBasedAlpha 用真实时间间隔计算混合系数），数值越大越"平滑"（慢）。
//
// 【安全约束】
// 程序只在游戏前台时才调用 update（见 main.cpp 数据线程），
// 且按住热键（默认 Shift）才生效。
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX  // 防止 Windows.h 定义 min/max 宏
#endif

#include "aimbot.hpp"
#include "esp.hpp"
#include "menu.hpp"
#include "recoil_patterns.hpp"   // 枪械弹道录制/回放
#include "weapon_names.hpp"      // 武器名（录制 UI/回放展示用）
#include "core/memory/game_layout.hpp"  // 实体表常量（武器解析用）
#include <Windows.h>
#include <iostream>
#include <cmath>
#include <cstdlib>   // rand（连跳人类化用）
#include <cstring>   // memcpy（读 Interception 鼠标事件）
#include <limits>
#include <algorithm>
#include <chrono>
#include <atomic>     // 延迟开火钩子的跨线程状态
#include <thread>    // 独立压枪线程
#include <mutex>     // 状态提示用
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{
    using SteadyClock = std::chrono::steady_clock;

    SteadyClock::time_point lastAimUpdate{};
    SteadyClock::time_point triggerTargetAcquired{};
    SteadyClock::time_point lastTriggerShot{};
    float aimResidualX = 0.0f;
    float aimResidualY = 0.0f;
    int32_t currentAimTargetIndex = -1;
    int32_t currentTriggerTargetIndex = -1;

    float timeBasedAlpha(
        SteadyClock::time_point& previous,
        float smoothing)
    {
        const auto now = SteadyClock::now();
        float deltaSeconds = 1.0f / 144.0f;
        if (previous.time_since_epoch().count() != 0) {
            deltaSeconds = std::chrono::duration<float>(
                now - previous).count();
        }
        previous = now;
        deltaSeconds = std::clamp(
            deltaSeconds,
            1.0f / 1000.0f,
            0.05f);
        const float safeSmoothing =
            std::max(1.0f, smoothing);
        if (safeSmoothing <= 1.01f) {
            return 1.0f;
        }
        const float responsePerSecond =
            60.0f / safeSmoothing;
        return 1.0f -
            std::exp(-responsePerSecond * deltaSeconds);
    }

    LONG consumeMouseDelta(float value, float& residual)
    {
        const float accumulated = value + residual;
        const LONG whole = static_cast<LONG>(
            std::trunc(accumulated));
        residual = accumulated - static_cast<float>(whole);
        return whole;
    }

    void resetAimState()
    {
        lastAimUpdate = {};
        aimResidualX = 0.0f;
        aimResidualY = 0.0f;
        currentAimTargetIndex = -1;
    }

    void resetTriggerState()
    {
        triggerTargetAcquired = {};
        currentTriggerTargetIndex = -1;
    }

    // 注入一次鼠标移动（压枪线程用）
    void injectMouseMove(LONG dx, LONG dy)
    {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        SendInput(1, &input, sizeof(INPUT));
    }

    // 注入一次键盘按键（按下/抬起，自动急停用）
    void injectKey(int vk, bool down)
    {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        if (!down) {
            input.ki.dwFlags = KEYEVENTF_KEYUP;
        }
        SendInput(1, &input, sizeof(INPUT));
    }
}

// Calculate head offset for side-facing enemies
// When enemy is facing sideways (angle 45-135 degrees to player),
// the head extends forward in the direction they are facing
vec3 calculateHeadOffset(
    const vec3& headPos,
    float enemyViewYaw,
    float angleToPlayer,
    const menu::RuntimeConfig& config)
{
    // Check if head offset is enabled
    if (!config.headOffsetEnabled) {
        return headPos;
    }

    // Check if enemy is within the angle range for offset
    if (angleToPlayer < config.headOffsetAngleMin ||
        angleToPlayer > config.headOffsetAngleMax) {
        return headPos;
    }

    // Calculate offset strength based on angle
    // Maximum offset at 90 degrees (perfectly sideways), less at edges
    float centerAngle =
        (config.headOffsetAngleMin + config.headOffsetAngleMax) / 2.0f;
    float angleRange =
        (config.headOffsetAngleMax - config.headOffsetAngleMin) / 2.0f;
    float angleFactor = angleRange > 0.0001f
        ? 1.0f - std::abs(angleToPlayer - centerAngle) / angleRange
        : 1.0f;
    angleFactor = std::max(0.0f, std::min(1.0f, angleFactor));  // Clamp to 0-1

    // Calculate offset direction (perpendicular to enemy's facing direction)
    // Enemy viewYaw: 0 = facing +X, 90 = facing +Y, etc.
    float yawRadians = enemyViewYaw * static_cast<float>(M_PI) / 180.0f;

    // Head extends in the direction enemy is facing
    float offsetX = std::cos(yawRadians) * config.headOffsetAmount * angleFactor;
    float offsetY = std::sin(yawRadians) * config.headOffsetAmount * angleFactor;

    vec3 adjustedHead = headPos;
    adjustedHead.x += offsetX;
    adjustedHead.y += offsetY;
    // Z offset is typically not needed as head height stays same

    return adjustedHead;
}

namespace
{
    vec3 targetPosition(
        const EnemyInfo& enemy,
        int selectedBone,
        const menu::RuntimeConfig& config)
    {
        vec3 position{};
        switch (selectedBone) {
            case 1:
                position = enemy.hasBones
                    ? enemy.bonePositions[BoneIndex::NECK]
                    : vec3{
                        enemy.headPosition.x,
                        enemy.headPosition.y,
                        enemy.headPosition.z - 5.0f
                    };
                break;
            case 2:
                if (enemy.hasBones) {
                    position =
                        enemy.bonePositions[BoneIndex::SPINE_2];
                } else {
                    position = {
                        (enemy.headPosition.x + enemy.position.x) /
                            2.0f,
                        (enemy.headPosition.y + enemy.position.y) /
                            2.0f,
                        (enemy.headPosition.z + enemy.position.z) /
                            2.0f
                    };
                }
                return position;
            case 3:  // 腹部（骨盆）
                position = enemy.hasBones
                    ? enemy.bonePositions[BoneIndex::PELVIS]
                    : vec3{
                        (enemy.headPosition.x + enemy.position.x) / 2.0f,
                        (enemy.headPosition.y + enemy.position.y) / 2.0f,
                        enemy.position.z };
                break;
            case 4:  // 左臂（肘）
                position = enemy.hasBones
                    ? enemy.bonePositions[BoneIndex::LEFT_ELBOW]
                    : vec3{
                        (enemy.headPosition.x + enemy.position.x) / 2.0f - 8.0f,
                        (enemy.headPosition.y + enemy.position.y) / 2.0f,
                        enemy.position.z };
                break;
            case 5:  // 右臂（肘）
                position = enemy.hasBones
                    ? enemy.bonePositions[BoneIndex::RIGHT_ELBOW]
                    : vec3{
                        (enemy.headPosition.x + enemy.position.x) / 2.0f + 8.0f,
                        (enemy.headPosition.y + enemy.position.y) / 2.0f,
                        enemy.position.z };
                break;
            case 6:  // 左腿（膝）
                position = enemy.hasBones
                    ? enemy.bonePositions[BoneIndex::LEFT_KNEE]
                    : vec3{ enemy.position.x - 6.0f, enemy.position.y, enemy.position.z };
                break;
            case 7:  // 右腿（膝）
                position = enemy.hasBones
                    ? enemy.bonePositions[BoneIndex::RIGHT_KNEE]
                    : vec3{ enemy.position.x + 6.0f, enemy.position.y, enemy.position.z };
                break;
            case 0:
            default:
                // 优先用骨骼头（模型精确：跟随敌人蹲/站/跳，瞄到真正的头），
                // 骨骼数据不可用时才用 headPosition（脚+视线偏移）的估算兜底。
                position = enemy.hasBones
                    ? enemy.bonePositions[BoneIndex::HEAD]
                    : enemy.headPosition;
                break;
        }

        return enemy.viewAngleKnown
            ? calculateHeadOffset(
                position,
                enemy.viewYaw,
                enemy.angleToPlayer,
                config)
            : position;
    }

    // 弧度 → 角度：后坐/弹道偏移字段（m_vecCsViewPunchAngle、m_predictableBaseAngle）
    // 在内存里是**弧度**，而 m_angEyeAngles 是**角度**（esp.cpp 按角度读）。压枪/
    // 魔法子弹把后坐弧度换算成角度再与视角相减。⚠️ 去换算会让补偿小 57 倍（等于没压）。
    constexpr float kRadiansToDegrees = 180.0f / static_cast<float>(M_PI);

    // 自动旋转（spinbot）：写模型朝向 yaw，他人可见、自己视角不受影响。
    // 转速由用户设置（0=停）。
    float spinYaw = 0.0f;
    SteadyClock::time_point lastSpinUpdate{};

    // 角色瞬移：目标位置与持续写入窗口（点一下瞬移后连续写一小段，对抗预测回弹）
    vec3 teleportTarget{};
    SteadyClock::time_point teleportHoldUntil{};
    SteadyClock::time_point lastTeleportUpdate{};
    // 连续移动速度 = 步长 × 20（单位/秒），默认步长 100 → 2000 单位/秒（约 8 倍奔跑速度）
    constexpr float kTeleportSpeedMul = 20.0f;

    // 魔法子弹"锁定写"状态：开火期间持续写修正角，停火后把用户原视角写回
    bool wasMagicBulletWriting = false;
    vec2 magicBulletBufferedView{};  // 开火瞬间缓冲的用户原视角（停火回正用）
    // 写内存自瞄的平滑角度：像鼠标自瞄一样把角度慢慢带向目标
    vec2 magicBulletSmoothAngle{};
    SteadyClock::time_point magicBulletLastSmoothUpdate{};

    // 连跳：上一帧是否落地（只用"落地瞬间"触发一次跳跃）
    bool wasOnGround = false;

    // ------------------------------------------------------------------
    // writeEyeAngles：把角度（度）写入本地视角
    // ------------------------------------------------------------------
    // 单位约定：esp.cpp 读 m_angEyeAngles 时把它当"角度"用，这里保持一致。
    // ⚠️ 部分 CS2 版本把该字段在内存里存成弧度：若写出后视角疯狂乱转，
    //    把 kAngleToEyeAngleStorage 改成 0.0174532925f（1°=π/180 rad）再试。
    void writeEyeAngles(const vec2& angleDeg)
    {
        // 俯仰角限制在 ±89°，防止写穿地板/天花板导致视角卡死
        const float pitch = std::clamp(angleDeg.x, -89.0f, 89.0f);
        // 单位约定：esp.cpp 读 m_angEyeAngles 时把它当"角度"用，这里保持一致。
        // ⚠️ 部分 CS2 版本把该字段在内存里存成弧度：若写出后视角疯狂乱转，
        //    把 kAngleToEyeAngleStorage 改成 0.0174532925f（1°=π/180 rad）再试。
        constexpr float kAngleToEyeAngleStorage = 1.0f;
        const vec3 write{
            pitch * kAngleToEyeAngleStorage,
            angleDeg.y * kAngleToEyeAngleStorage,
            0.0f
        };
        const uintptr_t pawn = esp::localPlayer.pawn;
        // 1) m_angEyeAngles：相机/瞄准视角（画面跟它走）。
        memory::Write<vec3>(
            pawn +
                cs2_dumper::schemas::client_dll::C_CSPlayerPawn::m_angEyeAngles,
            write);
        // 2) dwViewAngles（引擎全局视角）：usercmd 的开火视角很可能从它取。
        //    只写 m_angEyeAngles 时画面会动但子弹不跟（之前验证过），补写引擎
        //    视角才能让服务器收到修正后的瞄准角。⚠️ 不要写 v_angle（服务器复制的
        //    字段，会触发预测回滚）。
        memory::Write<vec3>(
            esp::modBase +
                cs2_dumper::offsets::client_dll::dwViewAngles,
            write);
    }

    // ------------------------------------------------------------------
    // writeShootAngles：只写"开枪旋转角"，不碰相机 → 外部静默
    // ------------------------------------------------------------------
    // 静默原理（对应 [原创]CS2外部静默原理剖析 一文）：在"本地相机旋转角不变"
    // 的前提下修改"开枪旋转角"。外部实现：开枪角由引擎全局视角 dwViewAngles
    // 驱动（之前验证过：只写 m_angEyeAngles 画面会动但子弹不跟），所以只写
    // dwViewAngles、不写 m_angEyeAngles（相机），画面/准星保持原样，子弹却按
    // 目标方向飞出。
    void writeShootAngles(const vec2& angleDeg)
    {
        const float pitch = std::clamp(angleDeg.x, -89.0f, 89.0f);
        constexpr float kAngleToEyeAngleStorage = 1.0f;
        const vec3 write{
            pitch * kAngleToEyeAngleStorage,
            angleDeg.y * kAngleToEyeAngleStorage,
            0.0f
        };
        memory::Write<vec3>(
            esp::modBase +
                cs2_dumper::offsets::client_dll::dwViewAngles,
            write);
    }

    // ------------------------------------------------------------------
    // writeModelYaw：只旋转"模型朝向"，不改视角
    // ------------------------------------------------------------------
    // 自动旋转写的是 CGameSceneNode 的模型旋转（m_angRotation / m_angAbsRotation），
    // 而不是 m_angEyeAngles / dwViewAngles。这样角色朝向会转（他人看得见），
    // 而自己的相机/视角完全不受影响；俯仰/翻滚保留原值（模型不倒地）。
    void writeModelYaw(float yawDeg)
    {
        const uintptr_t pawn = esp::localPlayer.pawn;
        const uintptr_t sceneNode = memory::Read<uintptr_t>(
            pawn +
                cs2_dumper::schemas::client_dll::C_BaseEntity::m_pGameSceneNode);
        if (!sceneNode) {
            return;
        }
        const vec3 rotation = memory::Read<vec3>(
            sceneNode +
                cs2_dumper::schemas::client_dll::CGameSceneNode::m_angAbsRotation);
        const vec3 write{ rotation.x, yawDeg, rotation.z };
        memory::Write<vec3>(
            sceneNode +
                cs2_dumper::schemas::client_dll::CGameSceneNode::m_angAbsRotation,
            write);
        memory::Write<vec3>(
            sceneNode +
                cs2_dumper::schemas::client_dll::CGameSceneNode::m_angRotation,
            write);
    }

    // ------------------------------------------------------------------
    // writePlayerPosition：把本地位置写到目标坐标（角色"瞬移"）
    // ------------------------------------------------------------------
    // 同时写场景节点原点（渲染用）与 pawn 的联网原点（m_vOldOrigin），
    // 让角色模型/坐标移动到目标点。在线对局位置由服务器权威判定，会被拉回；
    // 本地/离线环境可见效果。
    void writePlayerPosition(const vec3& position)
    {
        const uintptr_t pawn = esp::localPlayer.pawn;
        const uintptr_t sceneNode = memory::Read<uintptr_t>(
            pawn +
                cs2_dumper::schemas::client_dll::C_BaseEntity::m_pGameSceneNode);
        if (sceneNode) {
            memory::Write<vec3>(
                sceneNode +
                    cs2_dumper::schemas::client_dll::CGameSceneNode::m_vecAbsOrigin,
                position);
            memory::Write<vec3>(
                sceneNode +
                    cs2_dumper::schemas::client_dll::CGameSceneNode::m_vecOrigin,
                position);
        }
        memory::Write<vec3>(
            pawn +
                cs2_dumper::schemas::client_dll::C_BasePlayerPawn::m_vOldOrigin,
            position);
    }

    // ------------------------------------------------------------------
    // pickMagicBulletTarget：选魔法子弹目标，输出"目标角"（度，未做后坐补偿）
    // ------------------------------------------------------------------
    // 与 aimbot::update 相同的目标筛选，完全共用自瞄参数：aimbotFOV（范围）、
    // aimbotBone（骨骼）、aimbotVisibleOnly（仅已标记）、smartAim 系列
    // （智能瞄准：人物距离/视角距离/血量 优先级 + FOV 限制开关）、headOffset（头部偏移）。
    bool pickMagicBulletTarget(
        const menu::RuntimeConfig& config,
        const vec3& eyePos,
        const vec2& currentView,
        vec2& outAngle)
    {
        const esp::EnemySnapshot enemies = esp::getEnemySnapshot();
        if (!enemies) {
            return false;
        }

        float bestFov = std::numeric_limits<float>::infinity();
        vec2 bestFovAngle{};
        float bestSmart = std::numeric_limits<float>::infinity();
        vec2 bestSmartAngle{};

        for (const EnemyInfo& enemy : *enemies) {
            if (config.aimbotVisibleOnly &&
                (!enemy.visibilityKnown || !enemy.isSpotted)) {
                continue;
            }

            const vec2 angle = aimbot::calcAngle(
                eyePos,
                targetPosition(enemy, config.aimbotBone, config));
            const float fov = aimbot::getFOV(currentView, angle);
            if (!std::isfinite(fov)) {
                continue;
            }
            // FOV 范围筛选：与自瞄一致——普通始终限；智能瞄准由开关决定
            const bool limitByFov = config.smartAimEnabled
                ? config.smartAimUseFov
                : true;
            if (limitByFov && fov >= config.aimbotFOV) {
                continue;
            }

            if (config.smartAimEnabled) {
                float metric = enemy.distance;
                if (config.smartAimPriority == 1) {
                    metric = fov;  // 视角距离最近
                } else if (config.smartAimPriority == 2) {
                    metric = static_cast<float>(enemy.health) +
                        enemy.distance * 0.001f;  // 血量最低
                }
                if (metric < bestSmart) {
                    bestSmart = metric;
                    bestSmartAngle = angle;
                }
            } else if (fov < bestFov) {
                bestFov = fov;
                bestFovAngle = angle;
            }
        }

        if (config.smartAimEnabled) {
            if (std::isfinite(bestSmart)) {
                outAngle = bestSmartAngle;
                return true;
            }
        } else if (std::isfinite(bestFov)) {
            outAngle = bestFovAngle;
            return true;
        }
        return false;
    }
}

bool aimbot::init()
{
    // 复用 esp 已经验证过的只读进程句柄和模块基地址。
    // 在这里重新打开句柄可能因为一次瞬时 OpenProcess 失败而弄坏
    // 正在工作的 ESP 句柄。
    pID = esp::pID;
    modBase = esp::modBase;
    return pID != 0 && modBase != 0;
}

// 角度归一化到 [-180, 180]（角度相减后可能越界，循环折回）
float aimbot::normalizeAngle(float angle)
{
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

// 计算"从 src 看向 dst"需要的视角角度（pitch/yaw，度）。
// 数学：把 dst 相对 src 的差值看作直角三角形的两条边，
// 用 atan2 求角度。yaw 是水平角，pitch 是俯仰角。
vec2 aimbot::calcAngle(const vec3& src, const vec3& dst)
{
    // Delta = 目标位置 - 本地眼睛位置
    vec3 delta = { dst.x - src.x, dst.y - src.y, dst.z - src.z };

    float hyp = std::sqrt(delta.x * delta.x + delta.y * delta.y);  // 水平距离

    vec2 angle;
    // CS2 的正确公式：
    // Yaw = atan2(Δy, Δx) * 180/π
    angle.y = std::atan2(delta.y, delta.x) * (180.0f / static_cast<float>(M_PI));
    // Pitch = -atan2(Δz, sqrt(Δx² + Δy²)) * 180/π
    angle.x = -std::atan2(delta.z, hyp) * (180.0f / static_cast<float>(M_PI));

    return angle;
}



// FOV 距离：目标方向与当前准星方向的角度差（两个方向夹角的近似大小）。
// 用于判断目标是否在准星附近（在 FOV 范围内才瞄准）。
float aimbot::getFOV(const vec2& viewAngle, const vec2& aimAngle)
{
    float pitchDiff = normalizeAngle(aimAngle.x - viewAngle.x);
    float yawDiff = normalizeAngle(aimAngle.y - viewAngle.y);

    return std::sqrt(pitchDiff * pitchDiff + yawDiff * yawDiff);
}


// ----------------------------------------------------------------------------
// aimbot::update：自动瞄准主逻辑（数据线程每帧调用）
// ----------------------------------------------------------------------------
// 流程：
//   1. 前置检查：功能开启、输入未被抑制、游戏前台、热键按住、本地玩家缓存有效；
//   2. 遍历敌人快照，逐个计算"目标视角"（瞄准所选骨骼，头部可做侧向补偿），
//      用 FOV 距离筛选（普通模式始终限 FOV；智能瞄准默认不看 FOV，按距离/血量
//      排序，可开启 FOV 限制）；“仅已标记目标”两种模式都可选；
//   3. 目标选择带"保持"逻辑：如果上一帧的目标仍然有效且不比新目标差太多，
//      就继续用旧目标（防止在相邻的两个敌人之间来回抽搐）；
//   4. 把视角差值 × 平滑系数（指数时间平滑，与帧率无关）换算成鼠标移动量；
//   5. 用 SendInput 注入鼠标移动（方向注意 CS2 的 yaw/pitch 与屏幕的对应关系）。
void aimbot::update(const menu::RuntimeConfig& config)
{
    if (!config.aimbotEnabled ||
        config.inputSuppressed ||
        !sdl_renderer::isInputAllowed()) {
        resetAimState();
        return;
    }

    // 自瞄模式 1 = 魔法子弹：由 updateMagicBullet 处理（开火写角度），
    // 这里只负责"鼠标注入"路线。
    if (config.aimbotMode != 0) {
        resetAimState();
        return;
    }

    // Check if aimbot key is held (Shift)
    if (!(GetAsyncKeyState(config.aimbotKey) & 0x8000)) {
        resetAimState();
        return;
    }

    // Use cached local player data (updated once per frame in esp::updateEntities)
    if (!esp::localPlayer.isValid) {
        resetAimState();
        return;
    }

    const vec2& currentViewAngle = esp::localPlayer.viewAngle;
    const vec3& eyePos = esp::localPlayer.eyePosition;

    const esp::EnemySnapshot enemies =
        esp::getEnemySnapshot();
    if (!enemies) {
        resetAimState();
        return;
    }

    struct Candidate
    {
        bool valid = false;
        int32_t entityIndex = -1;
        vec2 angle{};
        float metric = std::numeric_limits<float>::infinity();
    };
    Candidate best{};
    Candidate retained{};

    for (const EnemyInfo& enemy : *enemies) {
        // “仅已标记目标”两种模式都生效：智能瞄准不再强制只看已标记，
        // 是否只看已标记由用户选项决定。
        if (config.aimbotVisibleOnly &&
            (!enemy.visibilityKnown || !enemy.isSpotted)) {
            continue;
        }

        const vec2 candidateAngle = calcAngle(
            eyePos,
            targetPosition(
                enemy,
                config.aimbotBone,
                config));
        const float fov = getFOV(
            currentViewAngle,
            candidateAngle);
        if (!std::isfinite(fov)) {
            continue;
        }

        // FOV 范围筛选：普通模式始终生效；智能瞄准模式由“启用瞄准范围限制”
        // 开关决定（关闭 = 忽略 FOV，任何方向的敌人都参与排序）。
        const bool limitByFov = config.smartAimEnabled
            ? config.smartAimUseFov
            : true;
        if (limitByFov && fov >= config.aimbotFOV) {
            continue;
        }

        float metric = fov;
        if (config.smartAimEnabled) {
            switch (config.smartAimPriority) {
            case 1:  // 视角距离最近：方向最贴近准星（视角夹角最小）
                metric = fov;
                break;
            case 2:  // 血量最低（距离作微调，血量相同时取近的）
                metric = static_cast<float>(enemy.health) +
                    enemy.distance * 0.001f;
                break;
            default: // 人物距离最近：离我（眼位→敌人脚底）最近的敌人
                metric = enemy.distance;
                break;
            }
        }

        const Candidate candidate{
            true,
            static_cast<int32_t>(enemy.entityIndex),
            candidateAngle,
            metric
        };
        if (!best.valid || metric < best.metric) {
            best = candidate;
        }
        if (candidate.entityIndex == currentAimTargetIndex) {
            retained = candidate;
        }
    }

    if (!best.valid) {
        resetAimState();
        return;
    }

    // Keep a still-valid target unless a replacement is materially better.
    // This prevents frame-to-frame oscillation between nearly equal players.
    Candidate selected = best;
    if (retained.valid) {
        const float hysteresis = config.smartAimEnabled
            ? std::max(1.0f, std::abs(best.metric) * 0.10f)
            : std::max(0.35f, config.aimbotFOV * 0.05f);
        if (retained.metric <= best.metric + hysteresis) {
            selected = retained;
        }
    }
    currentAimTargetIndex = selected.entityIndex;

    // Exponential time-based smoothing is stable across worker rates.
    // 开火瞬间提高回正速度：修"狙击枪视角还没转完、子弹就出去了"的滞后
    // （单发武器点一下即开火，平滑太慢会错过时机）。
    const bool firingNow =
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const float alpha = firingNow
        ? std::max(
            timeBasedAlpha(lastAimUpdate, config.aimbotSmoothing),
            0.55f)
        : timeBasedAlpha(lastAimUpdate, config.aimbotSmoothing);
    float deltaPitch =
        normalizeAngle(
            selected.angle.x - currentViewAngle.x) * alpha;
    float deltaYaw =
        normalizeAngle(
            selected.angle.y - currentViewAngle.y) * alpha;

    // Convert angle delta to mouse movement
    const float mouseSensitivityFactor =
        std::max(0.01f, config.mouseSensitivity) * 0.022f;

    // In CS2: Moving mouse RIGHT decreases Yaw, DOWN increases Pitch
    float moveX = -deltaYaw / mouseSensitivityFactor;
    float moveY = deltaPitch / mouseSensitivityFactor;

    // Move mouse if delta is significant
    // ------------------------------------------------------------------
    // 🎓 概念卡 ⑳：SendInput 与"输入注入"（模拟真实鼠标）
    // ------------------------------------------------------------------
    // 【是什么】SendInput 是 Windows 官方 API：把"输入事件"（鼠标移动、按键）
    // 送进系统输入队列，效果和用户真的动鼠标一模一样——游戏完全无法区分。
    // INPUT 结构体描述一次输入：type=INPUT_MOUSE 表示鼠标事件，
    // mi.dx/dy 是移动的像素量，dwFlags=MOUSEEVENTF_MOVE 表示"移动"。
    //
    // 【为什么在这里】这是"外部瞄准"的实现手段：算出视角差 → 换算成鼠标
    // 像素 → SendInput 注入。服务器看到的就是"玩家甩了一下鼠标"，
    // 这正是它比写游戏内存安全的原因（概念见《魔法子弹可行性分析》）。
    //
    // 【最小示例（把鼠标向右移动 100 像素）】
    //   INPUT input = {};
    //   input.type = INPUT_MOUSE;
    //   input.mi.dwFlags = MOUSEEVENTF_MOVE;
    //   input.mi.dx = 100;
    //   input.mi.dy = 0;
    //   SendInput(1, &input, sizeof(INPUT));   // 1 = 注入几条
    //
    // 【自己动手要点】
    // 1) dx/dy 是"增量"（相对当前鼠标位置移动多少），不是绝对坐标；
    // 2) SendInput 返回值 != 输入条数表示失败（可能被其他程序拦截）；
    // 3) 像素 → 角度的换算需要"每像素多少度"的系数（本项目用
    //    mouseSensitivity × 0.022f，游戏里灵敏度越高这个数越大）。
    // ------------------------------------------------------------------
    const LONG moveWholeX =
        consumeMouseDelta(moveX, aimResidualX);
    const LONG moveWholeY =
        consumeMouseDelta(moveY, aimResidualY);
    if (moveWholeX != 0 || moveWholeY != 0)
    {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = moveWholeX;
        input.mi.dy = moveWholeY;
        if (SendInput(1, &input, sizeof(INPUT)) != 1) {
            aimResidualX = 0.0f;
            aimResidualY = 0.0f;
        }
    }
}

// ----------------------------------------------------------------------------
// aimbot::updateTriggerbot：自动扳机（准星指到敌人就自动开枪）
// ----------------------------------------------------------------------------
// 流程：
//   1. 前置检查（功能开、热键按、本地缓存有效）；
//   2. 用本地玩家缓存的"准星指向实体编号"（m_iIDEntIndex，游戏自己算的，
//      不是几何猜的）查敌人快照，确认准星下确实是"存活的敌人"；
//   3. 确认后等 triggerbotDelay 毫秒（模拟人类反应延迟）再开枪；
//   4. 开枪用 SendInput 注入一次鼠标左键按下+抬起，
//      且两次开枪间隔至少 50ms（防止 0 延迟设置变成每秒几百发）。
void aimbot::updateTriggerbot(const menu::RuntimeConfig& config)
{
    if (!config.triggerbotEnabled ||
        config.inputSuppressed ||
        !sdl_renderer::isInputAllowed()) {
        resetTriggerState();
        return;
    }

    if (!(GetAsyncKeyState(config.triggerbotKey) & 0x8000)) {
        resetTriggerState();
        return;
    }

    if (!esp::localPlayer.isValid) {
        resetTriggerState();
        return;
    }

    const int32_t crosshairEntityIndex =
        esp::localPlayer.crosshairEntityIndex;
    if (crosshairEntityIndex <= 0) {
        resetTriggerState();
        return;
    }

    const esp::EnemySnapshot enemies =
        esp::getEnemySnapshot();
    if (!enemies) {
        resetTriggerState();
        return;
    }

    bool liveEnemyUnderCrosshair = false;
    for (const EnemyInfo& enemy : *enemies) {
        if (static_cast<int32_t>(enemy.entityIndex) ==
            crosshairEntityIndex) {
            liveEnemyUnderCrosshair = true;
            break;
        }
    }
    if (!liveEnemyUnderCrosshair) {
        resetTriggerState();
        return;
    }

    const auto now = SteadyClock::now();
    if (currentTriggerTargetIndex != crosshairEntityIndex) {
        currentTriggerTargetIndex = crosshairEntityIndex;
        triggerTargetAcquired = now;
    }

    const auto acquisitionDelay = std::chrono::milliseconds(
        std::max(0, config.triggerbotDelay));
    if (now - triggerTargetAcquired < acquisitionDelay) {
        return;
    }

    // The configured delay controls initial reaction time. A separate minimum
    // shot interval prevents a zero-delay setting from injecting hundreds of
    // clicks per second while the crosshair remains on one entity.
    constexpr auto MINIMUM_SHOT_INTERVAL =
        std::chrono::milliseconds(50);
    if (lastTriggerShot.time_since_epoch().count() != 0 &&
        now - lastTriggerShot < MINIMUM_SHOT_INTERVAL) {
        return;
    }

    if (!sdl_renderer::isInputAllowed()) {
        resetTriggerState();
        return;
    }

    INPUT clicks[2]{};
    clicks[0].type = INPUT_MOUSE;
    clicks[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    clicks[1].type = INPUT_MOUSE;
    clicks[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (SendInput(2, clicks, sizeof(INPUT)) == 2) {
        lastTriggerShot = now;
    } else {
        resetTriggerState();
    }
}

// ----------------------------------------------------------------------------
// aimbot::startRcsWorker / stopRcsWorker：独立压枪线程
// ----------------------------------------------------------------------------
// 【为什么用独立线程】压枪需要比实体采样更高的频率才"丝滑"：实体采样要遍历几百
// 个实体，频率受限。独立线程只读后坐相关的几个字段，跑 ~500Hz，每次只注入
// 待补偿量的一部分（平滑）——既提高检测频率，又减小单次偏移写入 → 弹着点更收敛。
//
// 【原理】子弹真实弹道偏移 = m_predictableBaseAngle（aimPunch，实测"角度"、稳定、
// 随连发涨到约 -5.4° 封顶）。压枪 = 高频读它，把"新增后坐"增量反向注入鼠标，
// 让游戏像"玩家手动压枪"生成 usercmd → 子弹真正跟随。
//
// 【换算】像素/度 = 1/(灵敏度×0.022)，灵敏度直接读 m_flMouseSensitivity。
// 【方向】默认 moveY = -dPitch（向上后坐 → 鼠标下压）；rcsInvert 翻转。
// 【平滑】每次注入待补偿量的 40%，余量下轮继续 → 单次移动小、顺滑。
namespace
{
    std::thread gRcsWorker;
    std::atomic<bool> gRcsWorkerRunning{false};

    // 状态提示（录音开始/结束等，游戏内顶部短暂显示）
    std::mutex gStatusMutex;
    std::string gStatusMessage;
    std::chrono::steady_clock::time_point gStatusUntil{};

    void setRcsStatusInternal(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(gStatusMutex);
        gStatusMessage = message;
        gStatusUntil = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
    }

    // 解析当前武器定义索引（与 esp.cpp entityAddress 同逻辑）
    int readCurrentWeaponIndex(uintptr_t pawn)
    {
        const uintptr_t weaponServices = memory::Read<uintptr_t>(
            pawn + cs2_dumper::schemas::client_dll::C_BasePlayerPawn::m_pWeaponServices);
        if (!weaponServices) {
            return 0;
        }
        const uint32_t handle = memory::Read<uint32_t>(
            weaponServices + cs2_dumper::schemas::client_dll::CPlayer_WeaponServices::m_hActiveWeapon);
        if (!handle || handle == 0xFFFFFFFF) {
            return 0;
        }
        const uintptr_t entityList = memory::Read<uintptr_t>(
            esp::modBase + cs2_dumper::offsets::client_dll::dwEntityList);
        if (!entityList) {
            return 0;
        }
        // ⚠️ 先取低 15 位为实体索引（句柄高位是序列号），再按 chunk 解析
        const uint32_t index = handle & game_layout::ENTITY_HANDLE_MASK;
        const uintptr_t listEntry = memory::Read<uintptr_t>(
            entityList + game_layout::ENTITY_LIST_CHUNK_ARRAY +
            sizeof(uintptr_t) * (index >> game_layout::ENTITY_CHUNK_SHIFT));
        if (!listEntry) {
            return 0;
        }
        const uintptr_t entity = memory::Read<uintptr_t>(
            listEntry + game_layout::ENTITY_IDENTITY_STRIDE *
                (index & game_layout::ENTITY_CHUNK_MASK));
        if (!entity) {
            return 0;
        }
        const uintptr_t itemView =
            entity +
            cs2_dumper::schemas::client_dll::C_EconEntity::m_AttributeManager +
            cs2_dumper::schemas::client_dll::C_AttributeContainer::m_Item;
        uint16_t weaponIndex = 0;
        memory::TryRead<uint16_t>(
            itemView +
                cs2_dumper::schemas::client_dll::C_EconItemView::m_iItemDefinitionIndex,
            weaponIndex);
        return weaponIndex;
    }

    // 把一段后坐增量（角度）换算成鼠标移动并注入
    void injectCompensation(
        float dPitchDeg, float dYawDeg,
        float sens, float direction, float strength, int maxMovePx)
    {
        if (!(sens > 0.05f) || sens > 50.0f) {
            sens = 1.0f;
        }
        const float pxPerDegree =
            1.0f / (std::max(0.01f, sens) * 0.022f);
        const float wantPitch = dPitchDeg * std::clamp(strength, 0.0f, 3.0f);
        const float wantYaw   = dYawDeg * std::clamp(strength, 0.0f, 3.0f);
        float moveX = +wantYaw * pxPerDegree * direction;
        float moveY = -wantPitch * pxPerDegree * direction;
        const float maxPx = static_cast<float>(std::clamp(maxMovePx, 5, 120));
        moveX = std::clamp(moveX, -maxPx, maxPx);
        moveY = std::clamp(moveY, -maxPx, maxPx);
        if (std::abs(moveX) >= 0.5f || std::abs(moveY) >= 0.5f) {
            injectMouseMove(static_cast<LONG>(moveX), static_cast<LONG>(moveY));
        }
    }

    void rcsWorkerLoop()
    {
        vec2 lastPunch{};           // live RCS 上帧 aimPunch（角度）
        bool hadBase = false;       // live RCS 基线
        int lastShotsFired = -1;    // 开火计数（检测新发）
        bool wasFiring = false;     // 上一轮是否开火
        int burstWeaponIndex = 0;   // 当前梭子武器索引
        int lastInjectedBullet = 0; // 回放：上次已预补偿的子弹号
        bool recordKeyWasDown = false;
        bool toggleKeyWasDown = false;
        bool csToggleKeyWasDown = false;
                bool spinToggleKeyWasDown = false;
        bool teleportToggleKeyWasDown = false;
        bool aimModeToggleKeyWasDown = false;
        bool trajToggleKeyWasDown = false;
        bool boneCycleKeyWasDown = false;
        bool prioPriorityKeyWasDown = false;
        bool vpKeyWasDown = false;

        // 自动急停状态：上一帧 W/A/S/D、排队中的相反键点按、以及抑制窗口
        bool csPrevW = false, csPrevA = false, csPrevS = false, csPrevD = false;
        int csTapVk = 0;
        SteadyClock::time_point csTapUntil{};
        int csSuppressVk = 0;                 // 我们注入的键（点按期间抑制其"释放"）
        SteadyClock::time_point csSuppressUntil{};

        while (gRcsWorkerRunning.load()) {
            const menu::RuntimeConfig cfg = menu::getRuntimeConfig();
            const bool gameActive =
                !cfg.inputSuppressed &&
                sdl_renderer::isInputAllowed();
            const bool rcsActive = gameActive && cfg.rcsEnabled;
            const float direction = cfg.rcsInvert ? -1.0f : 1.0f;

            // ---- 压枪开关热键：按一下切换（放在最前，不依赖 pawn，避免偶发失效）----
            const int toggleKey = cfg.rcsToggleKey;
            const bool toggleDown =
                toggleKey > 0 && (GetAsyncKeyState(toggleKey) & 0x8000);
            if (toggleDown && !toggleKeyWasDown) {
                menu::toggleRcs();
                // ⚠️ 菜单隐藏时 publishRuntimeConfig 不跑，快照会滞后——
                //    必须立即发布，热键才能马上生效。
                menu::publishRuntimeConfig();
                const menu::RuntimeConfig afterToggle =
                    menu::getRuntimeConfig();
                setRcsStatusInternal(
                    afterToggle.rcsEnabled ? "自动压枪：已开启" : "自动压枪：已关闭");
            }
            toggleKeyWasDown = toggleDown;

            // ---- 急停开关热键：按一下切换 ----
            const int csToggleKey = cfg.counterStrafToggleKey;
            const bool csToggleDown =
                csToggleKey > 0 && (GetAsyncKeyState(csToggleKey) & 0x8000);
            if (csToggleDown && !csToggleKeyWasDown) {
                menu::toggleCounterStraf();
                menu::publishRuntimeConfig();
                const menu::RuntimeConfig afterToggle =
                    menu::getRuntimeConfig();
                setRcsStatusInternal(
                    afterToggle.counterStrafEnabled ? "自动急停：已开启" : "自动急停：已关闭");
            }
            csToggleKeyWasDown = csToggleDown;

            // ---- 自动旋转开关热键：按一下切换 ----
            const int spinToggleKey = cfg.autoRotationToggleKey;
            const bool spinToggleDown =
                spinToggleKey > 0 && (GetAsyncKeyState(spinToggleKey) & 0x8000);
            if (spinToggleDown && !spinToggleKeyWasDown) {
                menu::toggleAutoRotation();
                menu::publishRuntimeConfig();
                const menu::RuntimeConfig afterToggle =
                    menu::getRuntimeConfig();
                setRcsStatusInternal(
                    afterToggle.autoRotationEnabled ? "自动旋转：已开启" : "自动旋转：已关闭");
            }
            spinToggleKeyWasDown = spinToggleDown;

            // ---- 角色瞬移开关热键：按一下切换 ----
            const int teleportKey = cfg.teleportToggleKey;
            const bool teleportKeyDown =
                teleportKey > 0 && (GetAsyncKeyState(teleportKey) & 0x8000);
            if (teleportKeyDown && !teleportToggleKeyWasDown) {
                menu::toggleTeleport();
                menu::publishRuntimeConfig();
                const menu::RuntimeConfig afterToggle =
                    menu::getRuntimeConfig();
                setRcsStatusInternal(
                    afterToggle.teleportEnabled ? "角色瞬移：已开启" : "角色瞬移：已关闭");
            }
            teleportToggleKeyWasDown = teleportKeyDown;

            // ---- 自瞄模式切换热键：按一下在 鼠标自瞄/魔法子弹 之间切换 ----
            const int aimModeKey = cfg.aimbotModeToggleKey;
            const bool aimModeKeyDown =
                aimModeKey > 0 && (GetAsyncKeyState(aimModeKey) & 0x8000);
            if (aimModeKeyDown && !aimModeToggleKeyWasDown) {
                menu::toggleAimMode();
                menu::publishRuntimeConfig();
                const menu::RuntimeConfig afterToggle =
                    menu::getRuntimeConfig();
                setRcsStatusInternal(
                    afterToggle.aimbotMode == 1
                        ? "自瞄模式：魔法子弹（开火写角度）"
                        : "自瞄模式：鼠标自瞄（平滑注入）");
            }
            aimModeToggleKeyWasDown = aimModeKeyDown;

            // ---- 弹道偏移开关热键：按一下切换 开枪自动弹道偏移 ----
            const int trajKey = cfg.magicTrajectoryToggleKey;
            const bool trajKeyDown =
                trajKey > 0 && (GetAsyncKeyState(trajKey) & 0x8000);
            if (trajKeyDown && !trajToggleKeyWasDown) {
                menu::toggleMagicTrajectory();
                menu::publishRuntimeConfig();
                const menu::RuntimeConfig afterToggle =
                    menu::getRuntimeConfig();
                setRcsStatusInternal(
                    afterToggle.magicTrajectoryEnabled
                        ? "弹道偏移：已开启"
                        : "弹道偏移：已关闭");
            }
            trajToggleKeyWasDown = trajKeyDown;

            // ---- 命中部位循环热键 ----
            static const char* hitboxNames[] = { "头部","脖子","胸部","腹部","左臂","右臂","左腿","右腿" };
            const int boneKey = cfg.boneCycleKey;
            const bool boneKeyDown =
                boneKey > 0 && (GetAsyncKeyState(boneKey) & 0x8000);
            if (boneKeyDown && !boneCycleKeyWasDown) {
                menu::cycleBone();
                menu::publishRuntimeConfig();
                const menu::RuntimeConfig afterToggle =
                    menu::getRuntimeConfig();
                const int b = std::clamp(afterToggle.aimbotBone, 0, 7);
                setRcsStatusInternal(
                    std::string("命中部位：") + hitboxNames[b]);
            }
            boneCycleKeyWasDown = boneKeyDown;

            // ---- 智能瞄准优先级循环热键 ----
            static const char* priorityNames[] = { "人物距离", "视角距离", "血量" };
            const int prioKey = cfg.smartAimPriorityCycleKey;
            const bool prioKeyDown =
                prioKey > 0 && (GetAsyncKeyState(prioKey) & 0x8000);
            if (prioKeyDown && !prioPriorityKeyWasDown) {
                menu::cycleSmartAimPriority();
                menu::publishRuntimeConfig();
                const menu::RuntimeConfig afterToggle =
                    menu::getRuntimeConfig();
                const int p = std::clamp(afterToggle.smartAimPriority, 0, 2);
                setRcsStatusInternal(
                    std::string("智能优先级：") + priorityNames[p]);
            }
            prioPriorityKeyWasDown = prioKeyDown;

            // ---- 视口模式循环热键 ----
            const int vpKey = cfg.viewportCycleKey;
            const bool vpKeyDown =
                vpKey > 0 && (GetAsyncKeyState(vpKey) & 0x8000);
            if (vpKeyDown && !vpKeyWasDown) {
                menu::cycleViewportMode();
                menu::publishRuntimeConfig();
                setRcsStatusInternal("视口模式：已循环切换");
            }
            vpKeyWasDown = vpKeyDown;

            const uintptr_t pawn = memory::Read<uintptr_t>(
                esp::modBase + cs2_dumper::offsets::client_dll::dwLocalPlayerPawn);
            if (!pawn) {
                hadBase = false; lastPunch = {};
                lastShotsFired = -1; wasFiring = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            int32_t shots = 0;
            memory::TryRead<int32_t>(
                pawn + cs2_dumper::schemas::client_dll::C_CSPlayerPawn::m_iShotsFired,
                shots);
            const bool firing =
                shots > 0 && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);

            vec3 punch{};
            const uintptr_t aimPunchServices = memory::Read<uintptr_t>(
                pawn + cs2_dumper::schemas::client_dll::C_CSPlayerPawn::m_pAimPunchServices);
            if (aimPunchServices) {
                memory::TryRead<vec3>(
                    aimPunchServices +
                        cs2_dumper::schemas::client_dll::CCSPlayer_AimPunchServices::m_predictableBaseAngle,
                    punch);
            }
            float sens = 0.0f;
            memory::TryRead<float>(
                pawn + cs2_dumper::schemas::client_dll::C_BasePlayerPawn::m_flMouseSensitivity,
                sens);

            // ---- 录音热键：第一次按开始，第二次按停止并保存（不要求 RCS 开）----
            const int recordKey = cfg.recoilRecordKey;
            const bool keyDown =
                recordKey > 0 && (GetAsyncKeyState(recordKey) & 0x8000);
            if (keyDown && !recordKeyWasDown) {
                if (recoil_patterns::isRecording()) {
                    const int recorded = recoil_patterns::recordingBulletCount();
                    const bool saved = recoil_patterns::stopRecording();
                    if (saved) {
                        setRcsStatusInternal(
                            "弹道录制完成，已保存 " + std::to_string(recorded) + " 发");
                    } else {
                        setRcsStatusInternal("弹道录制取消（不足 1 发）");
                    }
                } else if (gameActive) {
                    const int weaponIdx = readCurrentWeaponIndex(pawn);
                    const std::string weaponName = weaponIdx > 0
                        ? weapon_names::getWeaponName(weaponIdx)
                        : std::string("未知");
                    recoil_patterns::startRecording(weaponIdx, weaponName);
                    setRcsStatusInternal("开始录制弹道：" + weaponName);
                }
            }
            recordKeyWasDown = keyDown;

            // ---- 录音采样：每打一发记录 aimPunch（角度）----
            if (recoil_patterns::isRecording() &&
                firing && shots != lastShotsFired) {
                recoil_patterns::recordBullet(punch.x, punch.y);
            }

            // ---- 自动急停：松开 W/A/S/D → 快速点按相反键一次（不依赖 RCS）----
            if (gameActive && cfg.counterStrafEnabled) {
                const bool w = (GetAsyncKeyState(0x57) & 0x8000) != 0; // W
                const bool a = (GetAsyncKeyState(0x41) & 0x8000) != 0; // A
                const bool s = (GetAsyncKeyState(0x53) & 0x8000) != 0; // S
                const bool d = (GetAsyncKeyState(0x44) & 0x8000) != 0; // D
                const auto now = SteadyClock::now();

                // ⚠️ SendInput 注入会更新 GetAsyncKeyState —— 我们注入的点按也会被
                //    自己的释放检测看到。所以对自己注入的键设一个抑制窗口：窗口内
                //    忽略该键的"释放"，打断 松W→点S→S被当成松开→点W→… 的链式反应。
                const bool inSuppressWindow = now < csSuppressUntil;
                const bool suppressW = inSuppressWindow && csSuppressVk == 0x57;
                const bool suppressS = inSuppressWindow && csSuppressVk == 0x53;
                const bool suppressA = inSuppressWindow && csSuppressVk == 0x41;
                const bool suppressD = inSuppressWindow && csSuppressVk == 0x44;

                auto startTap = [&](int vk) {
                    if (csTapVk == 0) {
                        injectKey(vk, true);
                        csTapVk = vk;
                        csTapUntil = now + std::chrono::milliseconds(40);
                        csSuppressVk = vk;
                        csSuppressUntil = now + std::chrono::milliseconds(60);
                    }
                };
                if (csPrevW && !w && !s && !suppressW) startTap(0x53); // 松 W → 点 S
                if (csPrevS && !s && !w && !suppressS) startTap(0x57); // 松 S → 点 W
                if (csPrevA && !a && !d && !suppressA) startTap(0x44); // 松 A → 点 D
                if (csPrevD && !d && !a && !suppressD) startTap(0x41); // 松 D → 点 A
                csPrevW = w; csPrevA = a; csPrevS = s; csPrevD = d;

                // 完成已排队的点按（按住 ~40ms 后抬起）
                if (csTapVk != 0 && now >= csTapUntil) {
                    injectKey(csTapVk, false);
                    csTapVk = 0;
                }
            } else {
                csPrevW = csPrevA = csPrevS = csPrevD = false;
                csTapVk = 0;
                csSuppressVk = 0;
            }

            // ---- 开火检测 ----
            const bool newShot = firing && shots != lastShotsFired;
            lastShotsFired = shots;

            if (!rcsActive) {
                // RCS 关时只做录音，不做任何注入
                hadBase = false; lastPunch = {};
                wasFiring = false; lastInjectedBullet = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (!firing) {
                hadBase = false; lastPunch = {};
                wasFiring = false; lastInjectedBullet = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            // 新一梭子开始：取当前武器模式，预补偿第一发（已记录则）
            if (!wasFiring) {
                wasFiring = true;
                burstWeaponIndex = readCurrentWeaponIndex(pawn);
                lastInjectedBullet = 0;
                if (!recoil_patterns::isRecording()) {
                    const recoil_patterns::Pattern* first =
                        recoil_patterns::find(burstWeaponIndex);
                    if (first && !first->bullets.empty()) {
                        const vec2& b0 = first->bullets[0];
                        injectCompensation(
                            b0.x, b0.y, sens, direction,
                            cfg.rcsStrength, cfg.rcsMaxMovePx);
                        lastInjectedBullet = 1;
                    }
                }
            }

            // 本梭子已打子弹数（shots 在连发中单调递增）
            const int burstShotCount = shots;
            const int nextBullet = burstShotCount + 1;  // 下一发要打的子弹号

            // ---- 回放：已记录模式 → 前 N 发用记录弹道预补偿 ----
            const bool recording = recoil_patterns::isRecording();
            const recoil_patterns::Pattern* pattern =
                recording ? nullptr : recoil_patterns::find(burstWeaponIndex);
            if (pattern && !pattern->bullets.empty()) {
                const int recordedCount =
                    static_cast<int>(pattern->bullets.size());
                if (nextBullet <= recordedCount) {
                    if (nextBullet != lastInjectedBullet) {
                        lastInjectedBullet = nextBullet;
                        const vec2 prevPunch =
                            nextBullet == 1
                                ? vec2{}
                                : pattern->bullets[nextBullet - 2];
                        const vec2& curPunch =
                            pattern->bullets[nextBullet - 1];
                        injectCompensation(
                            curPunch.x - prevPunch.x,
                            curPunch.y - prevPunch.y,
                            sens, direction,
                            cfg.rcsStrength, cfg.rcsMaxMovePx);
                    }
                    // 已记录子弹：不走 live RCS
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                // 超出记录 → 回退 live；重设基线避免跳变
                lastInjectedBullet = recordedCount;
                hadBase = false; lastPunch = {};
            } else {
                lastInjectedBullet = 0;
            }

            // ---- live RCS（无记录 或 超出记录）----
            if (!hadBase) {
                lastPunch = { punch.x, punch.y };
                hadBase = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            const float dPitch = punch.x - lastPunch.x;
            const float dYaw   = punch.y - lastPunch.y;
            lastPunch = { punch.x, punch.y };
            if (std::abs(dPitch) >= 8.0f || std::abs(dYaw) >= 8.0f) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            injectCompensation(
                dPitch, dYaw, sens, direction,
                cfg.rcsStrength, cfg.rcsMaxMovePx);

            std::this_thread::sleep_for(std::chrono::milliseconds(2));  // ~500Hz
        }
    }
}

void aimbot::setRcsStatus(const char* message)
{
    setRcsStatusInternal(message ? message : "");
}

bool aimbot::getRcsStatus(std::string& outMessage)
{
    std::lock_guard<std::mutex> lock(gStatusMutex);
    if (gStatusUntil.time_since_epoch().count() == 0 ||
        std::chrono::steady_clock::now() >= gStatusUntil) {
        return false;
    }
    outMessage = gStatusMessage;
    return true;
}

void aimbot::startRcsWorker()
{
    if (gRcsWorkerRunning.load()) {
        return;
    }
    gRcsWorkerRunning = true;
    gRcsWorker = std::thread(rcsWorkerLoop);
}

void aimbot::stopRcsWorker()
{
    gRcsWorkerRunning = false;
    if (gRcsWorker.joinable()) {
        gRcsWorker.join();
    }
}

// ----------------------------------------------------------------------------
// aimbot::updateMagicBullet：魔法子弹（真实弹道修正 + 开火前写/停火回正）+ 自动旋转
// ----------------------------------------------------------------------------
// 【原理】CS2 子弹落点 = 服务器按 usercmd 视角角度算弹道 + 子弹偏移。客户端能改的
// 唯一杠杆是视角（m_angEyeAngles / v_angle，每个 tick 由它生成发给服务器的 usercmd）。
//   1) 真实弹道修正：写入角 = 目标角 − 预测射击偏移(CCSPlayer_AimPunchServices::
//      m_predictableBaseAngle)。这是子弹真正会飞向的偏移，不是相机视觉后座。
//      魔法子弹 = 按住瞄准键，把"完整视角"（相机 + 开枪角）平滑带向目标，看得见瞄准；
//      松开即还原用户视角。
//   2) 严格开火前写：按住开火期间以 240Hz 持续写，确保每一发 usercmd 发出前
//      视角都带着修正角；开火瞬间缓冲用户原视角。
//   3) 停火回正：松开扳机（或失去目标）的下一帧立刻把缓冲的原视角写回。
//   4) 自动旋转：写模型朝向 yaw（CGameSceneNode 旋转），他人可见、自己视角不受影响。
//
// 【诚实说明】散布仍由服务器随机算，远距离仍可能脱靶；写视角是检测高危区，
// 仅本地研究/娱乐用，别拿去打排位。
// 【跳起不准】出射角只决定"方向"；跳起（空中）时的散布惩罚由服务器按状态计算，
// 写角度无法消除（注释区结论：仅子弹出射角度是本地，散布/碰撞是服务器运算）。
void aimbot::updateMagicBullet(const menu::RuntimeConfig& config)
{
    const bool wantsSpin = config.autoRotationEnabled;
    // 自瞄模式 1 = 魔法子弹：启用自瞄且模式为"魔法子弹"时做"视图瞄准"
    const bool wantsBullet =
        config.aimbotEnabled && config.aimbotMode == 1;
    // 开火自动弹道偏移：两种自瞄模式都可生效（开启后开火瞬间把子弹拉到目标）
    const bool wantsTrajectory =
        config.aimbotEnabled && config.magicTrajectoryEnabled;
    if ((!wantsSpin && !wantsBullet && !wantsTrajectory) ||
        config.inputSuppressed ||
        !sdl_renderer::isInputAllowed() ||
        !memory::WritesAllowed()) {
        wasMagicBulletWriting = false;  // 禁用/抑制时复位，避免下次用过期缓冲
        return;
    }

    if (!esp::localPlayer.isValid) {
        return;
    }

    const vec3& eyePos = esp::localPlayer.eyePosition;
    const vec2 currentView = esp::localPlayer.viewAngle; // 本帧 esp 读到的视角
    const auto now = SteadyClock::now();

    // --- 目标选择 + 真实弹道修正（魔法子弹视图瞄准与弹道偏移共用） ---
    vec2 aimAngle{};
    const bool haveTarget =
        (wantsBullet || wantsTrajectory) &&
        pickMagicBulletTarget(
            config,
            eyePos,
            currentView,
            aimAngle);
    if (haveTarget) {
        // 真实弹道修正：写入角 = 目标角 − 预测射击偏移(aimPunch)。
        // 实测（rcs_debug.csv）m_predictableBaseAngle 是"角度"（稳定、随连发涨到约 -5.4°），
        // 与视角同单位直接相减即可，不再乘 57.3。
        const float recoilScale = std::clamp(
            config.magicBulletRecoil, 0.0f, 2.0f);
        const vec3& punch = esp::localPlayer.aimPunch;
        // 修正量限幅在 ±30°：即使方向假设有误，也不会把视角甩飞造成振荡
        const float corrPitch = std::clamp(
            punch.x * recoilScale,
            -30.0f, 30.0f);
        const float corrYaw = std::clamp(
            punch.y * recoilScale,
            -30.0f, 30.0f);
        aimAngle.x -= corrPitch;
        aimAngle.y -= corrYaw;
        aimAngle.y = normalizeAngle(aimAngle.y);
    }

    // --- 自动旋转推进 yaw（写模型朝向，不改视角，与魔法子弹互不干扰） ---
    if (wantsSpin) {
        float deltaSeconds = 1.0f / 144.0f;
        if (lastSpinUpdate.time_since_epoch().count() != 0) {
            deltaSeconds = std::chrono::duration<float>(
                now - lastSpinUpdate).count();
        }
        lastSpinUpdate = now;
        deltaSeconds = std::clamp(deltaSeconds, 1.0f / 1000.0f, 0.05f);
        spinYaw = normalizeAngle(
            spinYaw + std::max(0.0f, config.autoRotationSpeed) * deltaSeconds);
        writeModelYaw(spinYaw);
    }

    // --- 本帧写什么（只操作开枪角，与模型旋转互不干扰） ---
    // 写内存自瞄的关键：只要瞄准（范围内有目标）就把"开枪角"连续平滑带向目标，
    // 而不是等开火才写——这样第一发出膛时角度已经到位，不会"先开火再转弹道"。
    // 静默：只写 dwViewAngles（开枪角），相机(m_angEyeAngles)不动；
    // 非静默且正在开火：额外把相机也带向目标（能看到甩枪）。
    // 魔法子弹 = 按住瞄准键 → 平滑把"完整视角"（相机 m_angEyeAngles + 开枪角
    // dwViewAngles）带向目标，看得见瞄准；松开即还原用户视角。关键：按住键时视角
    // 先带到目标、之后再开火 → 第一发已经对准，不再"先开枪再转弹道"。
    const bool aiming =
        wantsBullet && haveTarget &&
        (GetAsyncKeyState(config.aimbotKey) & 0x8000);
    if (aiming) {
        if (!wasMagicBulletWriting) {
            magicBulletBufferedView = currentView;   // 记录按住时的用户视角（松开还原用）
            magicBulletSmoothAngle = currentView;     // 平滑从当前视角开始
            magicBulletLastSmoothUpdate = {};         // 首帧用默认间隔
        }
        wasMagicBulletWriting = true;

        // 平滑：按"瞄准平滑度"把视角慢慢带向目标
        const float alpha =
            timeBasedAlpha(magicBulletLastSmoothUpdate, config.aimbotSmoothing);
        magicBulletSmoothAngle.x +=
            normalizeAngle(aimAngle.x - magicBulletSmoothAngle.x) * alpha;
        magicBulletSmoothAngle.y +=
            normalizeAngle(aimAngle.y - magicBulletSmoothAngle.y) * alpha;

        // 写完整视角（相机 + 开枪角），看得见瞄准
        writeEyeAngles(magicBulletSmoothAngle);
    } else if (wasMagicBulletWriting) {
        // 松开瞄准键/失去目标：按"准心回正"选项决定是还原视角还是留在自瞄位置
        if (config.magicBulletRecenter) {
            writeEyeAngles(magicBulletBufferedView);
        }
        wasMagicBulletWriting = false;
        magicBulletSmoothAngle = {};
    }

    // 开枪自动弹道偏移（可选，两种自瞄模式都生效）：开火瞬间把"开枪角"写到目标角
    // （含后坐修正），子弹自行修正到命中部位——即使没按住瞄准键，开火时也被拉到目标。
    if (wantsTrajectory && haveTarget &&
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        writeShootAngles(aimAngle);
    }
}

// ----------------------------------------------------------------------------
// aimbot::updateTeleport：角色瞬移（写本地位置字段）
// ----------------------------------------------------------------------------
// 开启后有两种用法：
//   1) 按住移动键（W/A/S/D，空格=上升）：以"步长 × 20 单位/秒"的速度连续位移，
//      远快于游戏正常移动；
//   2) 菜单里点"前/后/左/右/上/下"按钮：按当前朝向单次跳一个步长，随后持续
//      写入约 350ms 对抗客户端预测回弹。
// ⚠️ 在线对局位置由服务器权威判定，会被拉回；本地/离线环境可见效果。
void aimbot::updateTeleport(const menu::RuntimeConfig& config)
{
    const int direction = menu::teleportRequest.exchange(
        0,
        std::memory_order_relaxed);
    if (config.inputSuppressed ||
        !sdl_renderer::isInputAllowed() ||
        !memory::WritesAllowed() ||
        !config.teleportEnabled ||
        !esp::localPlayer.isValid) {
        teleportHoldUntil = {};
        return;
    }

    const auto now = SteadyClock::now();
    const float step = std::max(0.0f, config.teleportStep);
    const float yaw = esp::localPlayer.viewAngle.y *
        (static_cast<float>(M_PI) / 180.0f);
    const float cosYaw = std::cos(yaw);
    const float sinYaw = std::sin(yaw);

    // ---- 1) 按住移动键 → 连续快速位移 ----
    float dt = 0.016f;
    if (lastTeleportUpdate.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(
            now - lastTeleportUpdate).count();
    }
    lastTeleportUpdate = now;
    dt = std::clamp(dt, 1.0f / 1000.0f, 0.05f);

    const float moveSpeed = step * kTeleportSpeedMul;
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    if (GetAsyncKeyState('W') & 0x8000) {
        dx += cosYaw * moveSpeed;
        dy += sinYaw * moveSpeed;
    }
    if (GetAsyncKeyState('S') & 0x8000) {
        dx -= cosYaw * moveSpeed;
        dy -= sinYaw * moveSpeed;
    }
    if (GetAsyncKeyState('A') & 0x8000) {
        dx += -sinYaw * moveSpeed;
        dy += cosYaw * moveSpeed;
    }
    if (GetAsyncKeyState('D') & 0x8000) {
        dx += sinYaw * moveSpeed;
        dy += -cosYaw * moveSpeed;
    }
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
        dz += moveSpeed;
    }
    const float movementSq = dx * dx + dy * dy + dz * dz;
    if (movementSq > 0.0001f) {
        // 斜向移动归一化：W+D 对角线时速度不叠加变快
        const float scale =
            moveSpeed * dt / std::sqrt(movementSq);
        dx *= scale;
        dy *= scale;
        dz *= scale;
        vec3 current = esp::localPlayer.position;
        const uintptr_t sceneNode = memory::Read<uintptr_t>(
            esp::localPlayer.pawn +
                cs2_dumper::schemas::client_dll::C_BaseEntity::m_pGameSceneNode);
        if (sceneNode) {
            memory::TryRead<vec3>(
                sceneNode +
                    cs2_dumper::schemas::client_dll::CGameSceneNode::m_vecAbsOrigin,
                current);
        }
        writePlayerPosition({
            current.x + dx,
            current.y + dy,
            current.z + dz
        });
    } else {
        teleportHoldUntil = {};
    }

    // ---- 2) 一次性跳步请求（菜单按钮） ----
    if (direction != 0) {
        float jumpDx = 0.0f, jumpDy = 0.0f, jumpDz = 0.0f;
        switch (direction) {
        case 1: jumpDx = cosYaw * step; jumpDy = sinYaw * step; break;   // 前
        case 2: jumpDx = -cosYaw * step; jumpDy = -sinYaw * step; break; // 后
        case 3: jumpDx = -sinYaw * step; jumpDy = cosYaw * step; break;  // 左
        case 4: jumpDx = sinYaw * step; jumpDy = -cosYaw * step; break;  // 右
        case 5: jumpDz = step; break;                                    // 上
        case 6: jumpDz = -step; break;                                   // 下
        default: break;
        }

        vec3 current = esp::localPlayer.position;
        const uintptr_t sceneNode = memory::Read<uintptr_t>(
            esp::localPlayer.pawn +
                cs2_dumper::schemas::client_dll::C_BaseEntity::m_pGameSceneNode);
        if (sceneNode) {
            memory::TryRead<vec3>(
                sceneNode +
                    cs2_dumper::schemas::client_dll::CGameSceneNode::m_vecAbsOrigin,
                current);
        }
        teleportTarget = {
            current.x + jumpDx,
            current.y + jumpDy,
            current.z + jumpDz
        };
        teleportHoldUntil = now + std::chrono::milliseconds(350);
    }

    // 跳步请求后持续写入一小段窗口，降低被预测立即拉回的概率
    if (teleportHoldUntil.time_since_epoch().count() != 0 &&
        now < teleportHoldUntil) {
        writePlayerPosition(teleportTarget);
    }
}

// ----------------------------------------------------------------------------
// aimbot::updateBhop：连跳（bunny hop）
// ----------------------------------------------------------------------------
// 【原理】读本地玩家 C_BaseEntity::m_fFlags（FL_ONGROUND=1 表示双脚着地）。
// 用户按住空格时，每次"落地瞬间"注入一次空格按下+抬起，实现连续起跳。
// bhopHumanize（0~100%）是随机跳过某次起跳的概率，让节奏更接近真人。
void aimbot::updateBhop(const menu::RuntimeConfig& config)
{
    if (!config.bhopEnabled ||
        config.inputSuppressed ||
        !sdl_renderer::isInputAllowed()) {
        wasOnGround = false;
        return;
    }

    if (!esp::localPlayer.isValid) {
        wasOnGround = false;
        return;
    }

    // 只有按住空格（起跳键）才自动连跳
    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        wasOnGround = false;
        return;
    }

    uint32_t flags = 0;
    if (!memory::TryRead<uint32_t>(
            esp::localPlayer.pawn +
                cs2_dumper::schemas::client_dll::C_BaseEntity::m_fFlags,
            flags)) {
        return;
    }
    const bool onGround = (flags & 1u) != 0;

    // 只在"落地瞬间"触发一次跳跃，避免每帧重复注入
    if (onGround && !wasOnGround) {
        const float skipChance =
            std::clamp(config.bhopHumanize, 0.0f, 100.0f) / 100.0f;
        const float roll =
            static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
        if (roll >= skipChance) {
            INPUT keys[2]{};
            keys[0].type = INPUT_KEYBOARD;
            keys[0].ki.wVk = VK_SPACE;
            keys[1].type = INPUT_KEYBOARD;
            keys[1].ki.wVk = VK_SPACE;
            keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, keys, sizeof(INPUT));
        }
    }
    wasOnGround = onGround;
}
