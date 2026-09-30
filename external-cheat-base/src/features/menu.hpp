// ============================================================================
// menu.hpp —— 设置菜单（按 F4 打开的界面）与所有配置项
//
// 【这个文件是干什么的】
// 1. 定义了程序里"所有用户可调设置"的变量（ESP 开关、颜色、瞄准参数、热键…）；
// 2. 用 ImGui 画出整个设置界面：左侧导航栏（总览/战斗/视觉/世界&雷达/系统）
//    + 右侧各页的内容；
// 3. 提供"运行时配置"（RuntimeConfig）的打包/读取机制：
//    数据线程每帧只读一份不可变快照，避免和界面线程抢同一批变量；
// 4. 提供热键绑定（点击按钮后按任意键，ESC 取消；允许同一按键绑定多个功能）；
// 5. 把部分设置持久化到 %LOCALAPPDATA%\AegisCS2\settings-v1.ini（下次启动恢复）。
//
// 【安全设计（值得注意）】
// - 公网中继的 token 放在进程内存里、永不落盘，退出时用 SecureZeroMemory 清零；
// - 界面线程用互斥锁 + "快照"和采样线程交换配置，不会读到改了一半的值。
//
// 【给 C++ 初学者的概念】
// - inline 变量：在头文件里定义的"全局变量"（C++17 特性），大家共享同一份。
// - std::array<char, N>：定长字符数组，这里用来装用户输入的 URL/token。
// - ImGui 的 UI 调用（Checkbox/SliderFloat/Combo…）只负责"画界面和读用户操作"，
//   不直接干活——真正的效果在别的模块看到这些变量后才会发生。
// ============================================================================

#pragma once

#include "imgui.h"
#include "core/renderer/sdl_renderer.h"
#include "core/memory/memory.hpp"
#include "core/diagnostics.hpp"
#include "core/performance_metrics.hpp"
#include "features/web_radar/public_relay_config.hpp"
#include "features/web_radar/snapshot_recorder.hpp"
#include "recoil_patterns.hpp"   // 枪械弹道录制/回放模式表
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace menu
{
    // ------------------------------------------------------------------
    // PublicRelayConnectionSettings：公网中继的连接凭据（URL + 房间 + token）
    // ------------------------------------------------------------------
    // 设计要点（见代码里的注释）：
    //   1. 240Hz 采样循环会频繁读取这份配置，所以把 URL/房间/token 打包成
    //      一次分配的一块不可变内存，避免每次采样都重新拼字符串；
    //   2. token 是敏感凭据：对象销毁时用 SecureZeroMemory 把内存清零
    //      （普通析构不会清空，可能有残留被扫描出来）。
    // RuntimeConfig 会被 240Hz 的采样循环拷贝。把完整的连接配置放在一块
    // 不可变分配里，而不是反复分配 URL/房间/token 字符串，并在释放那块
    // 分配前清掉 token。
    class PublicRelayConnectionSettings final
    {
    public:
        PublicRelayConnectionSettings(
            const std::string_view endpointUrl,
            const std::string_view room,
            const std::string_view token)
            : endpointUrl_(endpointUrl),
              room_(room),
              token_(token)
        {
        }

        ~PublicRelayConnectionSettings()
        {
            if (!token_.empty()) {
                SecureZeroMemory(token_.data(), token_.size());
            }
        }

        PublicRelayConnectionSettings(
            const PublicRelayConnectionSettings&) = delete;
        PublicRelayConnectionSettings& operator=(
            const PublicRelayConnectionSettings&) = delete;
        PublicRelayConnectionSettings(
            PublicRelayConnectionSettings&&) = delete;
        PublicRelayConnectionSettings& operator=(
            PublicRelayConnectionSettings&&) = delete;

        [[nodiscard]] std::string_view endpointUrl() const noexcept
        {
            return endpointUrl_;
        }

        [[nodiscard]] std::string_view room() const noexcept
        {
            return room_;
        }

        [[nodiscard]] std::string_view token() const noexcept
        {
            return token_;
        }

    private:
        std::string endpointUrl_;
        std::string room_;
        std::string token_;
    };

    // ImGui edits these values on the render thread. The worker copies one
    // coherent snapshot under this mutex at the start of each update pass.
    inline std::mutex configMutex;
    inline diagnostics::StartupReport startupReport;

    inline void setStartupReport(diagnostics::StartupReport report)
    {
        startupReport = std::move(report);
    }

    // ------------------------------------------------------------------
    // RuntimeConfig：给数据线程用的"运行时配置快照"
    // ------------------------------------------------------------------
    // 界面线程改的是上面那批全局变量；数据线程读的是这份快照。
    // 每帧由 buildRuntimeConfig() 打包一次，采样循环拿到的永远是"某一瞬间
    // 的完整一致配置"，不会读到改了一半的值。
    struct RuntimeConfig
    {
        bool espEnabled = true;
        bool espWeapon = true;
        bool espFlashIndicator = false;
        bool antiFlash = false;
        bool espViewAngle = true;
        bool localRadarEnabled = false;
        bool localRadarShowNames = true;
        float localRadarAnchorX = 0.02f;
        float localRadarAnchorY = 0.08f;
        float localRadarSize = 0.32f;
        float localRadarMarkerSize = 12.0f;
        bool webRadarEnabled = false;
        bool webRadarLanAccess = false;
        bool webRadarPauseWhenUnfocused = true;
        bool webRadarIncludePlayerNames = true;
        bool webRadarIncludeSteamIds = false;
        int webRadarTeamViewPolicy = 0;
        uint16_t webRadarPort = 22006;
        bool radarRecordingEnabled = false;
        int radarRefreshRateHz = 20;
        bool publicRelayEnabled = false;
        bool publicRelayIncludePlayerNames = true;
        bool publicRelayIncludeSteamIds = false;
        int publicRelayTeamViewPolicy = 0;
        std::shared_ptr<const PublicRelayConnectionSettings>
            publicRelayConnection;
        bool espWallCheck = true;
        bool espSkeleton = true;
        bool grenadeESP = false;
        bool droppedWeaponESP = false;
        bool bombTimer = true;

        bool headOffsetEnabled = true;
        float headOffsetAmount = 5.0f;
        float headOffsetAngleMin = 45.0f;
        float headOffsetAngleMax = 135.0f;
        bool aimbotEnabled = false;
        float aimbotFOV = 10.0f;
        float aimbotSmoothing = 5.0f;
        int aimbotBone = 0;
        bool aimbotVisibleOnly = true;
        int aimbotKey = VK_SHIFT;
        bool smartAimEnabled = false;
        int smartAimPriority = 0;
        bool smartAimUseFov = false;
        float mouseSensitivity = 1.0f;

        // 自瞄模式切换热键（0鼠标/1魔法子弹）
        int aimbotModeToggleKey = VK_F12;
        // 命中部位循环热键（0头..7右腿）
        int boneCycleKey = VK_HOME;
        // 智能瞄准优先级循环热键（0人物距离/1视角距离/2血量）
        int smartAimPriorityCycleKey = VK_END;
        // 视口模式循环热键（0自动/1全客户端/2 4:3/3 16:10）
        int viewportCycleKey = VK_PRIOR;

        bool triggerbotEnabled = false;
        int triggerbotDelay = 50;
        int triggerbotKey = 0x46;

        // 自动压枪（RCS）：独立线程 ~500Hz 读 aimPunch（真实弹道偏移），增量反向注入鼠标。
        bool rcsEnabled = false;
        bool rcsInvert = false;          // 方向反了时翻转补偿方向
        float rcsInjectRatio = 1.0f;     // 每次注入待补偿量的比例（1.0=下一发前全额回正；<1=平滑但滞后）
        int rcsMaxMovePx = 40;           // 单次鼠标移动上限（px）
        float rcsStrength = 1.0f;        // 补偿强度倍率（punch 与实际弹道有偏差时微调）

        // 弹道录制热键：第一次按开始录音，第二次按停止并保存
        int recoilRecordKey = VK_F6;
        // 压枪开关热键：按一下切换 自动压枪 的开关
        int rcsToggleKey = VK_F7;

        // 自瞄模式：0=鼠标自瞄（平滑注入） 1=魔法子弹（开火写开枪角，即点即中）
        int aimbotMode = 0;
        // 魔法子弹（自瞄模式 1）专属参数；目标选择/范围/骨骼等共用自瞄的选项
        float magicBulletRecoil = 1.0f;     // 后坐补偿强度（写角 = 目标角 − 后坐×scale）
        bool magicTrajectoryEnabled = false; // 开枪自动弹道偏移：开火时把子弹拉到目标
        int magicTrajectoryToggleKey = VK_INSERT; // 切换弹道偏移的热键
        bool magicBulletRecenter = true;    // 魔法自瞄完毕后准心回正（false=留在自瞄位置）

        // 自动旋转（spinbot，与魔法子弹共用写点）
        bool autoRotationEnabled = false;
        float autoRotationSpeed = 360.0f;   // 旋转速度（度/秒，0=停）
        int autoRotationToggleKey = VK_F10; // 切换自动旋转的快捷键

        // 连跳（bhop）
        bool bhopEnabled = false;
        float bhopHumanize = 0.0f;          // 0~100：随机跳过某次起跳的概率（更像真人）

        // 自动急停（counter-strafe）：松开移动键时自动快速点按相反键，快速停住
        bool counterStrafEnabled = false;
        int counterStrafToggleKey = VK_F8;  // 切换自动急停的快捷键

        // 角色瞬移（本地写位置字段；在线对局由服务器权威判定，会被拉回）
        bool teleportEnabled = false;
        float teleportStep = 100.0f;   // 步长：按钮=单次跳步距离；按住移动键时每秒移动 20×步长
        int teleportToggleKey = VK_F11; // 切换瞬移的快捷键

        bool inputSuppressed = false;

        [[nodiscard]] bool radarSnapshotEnabled() const noexcept
        {
            return localRadarEnabled || webRadarEnabled ||
                publicRelayEnabled || radarRecordingEnabled;
        }
    };

    // Current tab index
    inline int currentTab = 0;

    // ESP Settings
    inline bool espEnabled = true;
    inline bool espBox = true;
    inline bool espHealth = true;
    inline bool espDistance = true;  // Default ON
    inline bool espWeapon = true;    // Weapon display - Default ON
    inline bool espViewAngle = true; // View angle indicator - Default ON
    inline bool espViewAngleText = false; // Show angle degree text
    inline bool espFlashIndicator = false; // Flashbang eye indicator - Default OFF
    inline bool espWallCheck = true; // CS2 spotted-state indicator
    inline bool espSnaplines = false;

    // Skeleton ESP
    inline bool espSkeleton = true;
    inline float espSkeletonColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  // White

    // Colors
    inline float espBoxColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f };          // Red - spotted state
    inline float espWallColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };         // Green - not spotted or unknown
    inline float espDistanceColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    inline float espWeaponColor[4] = { 0.0f, 1.0f, 1.0f, 1.0f };       // Cyan color for weapon
    inline float espFlashNormalColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f };  // Red - normal eye state
    inline float espFlashColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };        // Yellow - flashed eye state
    inline float espSnaplinesColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };

    // Visual Settings
    inline int snaplinesOrigin = 0; // 0=Bottom, 1=Center, 2=Top

    // Aimbot Settings
    inline bool aimbotEnabled = false;     // Aimbot enabled
    inline float aimbotFOV = 10.0f;        // Field of view for aimbot (degrees)
    inline float aimbotSmoothing = 5.0f;   // Smoothing factor (1.0 = instant, higher = smoother)
    inline int aimbotBone = 0;             // 0=Head, 1=Neck, 2=Chest
    inline bool aimbotVisibleOnly = true;  // Only aim at enemies flagged as spotted
    inline int aimbotKey = VK_SHIFT;       // Aimbot activation key (default: Shift key)
    inline bool aimbotShowFOV = true;      // Show FOV circle on screen
    inline float aimbotFOVColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };  // Color-key overlays use opaque primitives

    // Head Offset Settings (for side-facing enemies)
    inline bool headOffsetEnabled = true;      // Enable head offset compensation
    inline float headOffsetAmount = 5.0f;      // Offset amount in game units (0-15)
    inline float headOffsetAngleMin = 45.0f;   // Minimum angle for offset (degrees)
    inline float headOffsetAngleMax = 135.0f;  // Maximum angle for offset (degrees)

    // Smart Aim Settings (auto-lock spotted enemies by priority)
    inline bool smartAimEnabled = false;      // Smart aim mode (auto-selects best target by priority)
    inline int smartAimPriority = 0;          // 0=Distance first, 1=Health first
    inline bool smartAimUseFov = false;       // 智能瞄准时是否启用瞄准范围(FOV)限制（关闭=忽略范围）

    // 命中部位循环 / 智能瞄准优先级循环 热键
    inline int boneCycleKey = VK_HOME;        // 循环命中部位（头/脖/胸/腹/双臂/双腿）
    inline int smartAimPriorityCycleKey = VK_END; // 循环智能瞄准优先级（人物距离/视角距离/血量）

    // 自瞄模式切换热键（0鼠标/1魔法子弹）
    inline int aimbotModeToggleKey = VK_F12;

    // Triggerbot Settings
    inline bool triggerbotEnabled = false; // Triggerbot enabled
    inline int triggerbotDelay = 50;       // Delay before shooting (milliseconds)
    inline int triggerbotKey = 0x46;       // Triggerbot activation key (default: F key, 0x46 = 'F')

    // Recoil Control (RCS) Settings
    inline bool rcsEnabled = false;  // 自动压枪（开枪时反向补偿后座）
    inline bool rcsInvert = false;   // 补偿方向反了？打开它
    inline float rcsInjectRatio = 1.0f;  // 每次注入待补偿量比例（1.0=全额即时）
    inline int rcsMaxMovePx = 40;        // 单次鼠标移动上限（px）
    inline float rcsStrength = 1.0f;     // 补偿强度倍率

    // 弹道录制热键（Hotkeys 页可改）
    inline int recoilRecordKey = VK_F6;
    // 压枪开关热键（Hotkeys 页可改）
    inline int rcsToggleKey = VK_F7;

    // Magic Bullet (角度写入) Settings
    inline int aimbotMode = 0;                // 自瞄模式：0=鼠标自瞄 1=魔法子弹（开火写角度）
    inline float magicBulletRecoil = 1.0f;    // 后坐补偿强度（写角 = 目标角 − 后坐×scale）
    inline bool magicTrajectoryEnabled = false; // 开枪自动弹道偏移：开火时把子弹拉到目标
    inline int magicTrajectoryToggleKey = VK_INSERT; // 切换弹道偏移的热键
    inline bool magicBulletRecenter = true;    // 魔法自瞄完毕后准心回正（false=留在自瞄位置）

    // Auto-Rotation (spinbot) Settings：旋转模型朝向，自己视角不受影响
    inline bool autoRotationEnabled = false;  // 自动旋转（他人看到角色在转，自己画面不动）
    inline float autoRotationSpeed = 360.0f;  // 旋转速度（度/秒，0=停）
    inline int autoRotationToggleKey = VK_F10; // 切换自动旋转的快捷键

    // Bhop (连跳) Settings
    inline bool bhopEnabled = false;          // 连跳
    inline float bhopHumanize = 0.0f;         // 随机跳过起跳概率（0~100）

    // Counter-Strafe (自动急停) Settings
    inline bool counterStrafEnabled = false;  // 松开移动键自动点按相反键
    inline int counterStrafToggleKey = VK_F8; // 切换急停的快捷键

    // Teleport (角色瞬移) Settings：写本地位置字段产生"瞬移"（本地/离线可见）
    inline bool teleportEnabled = false;
    inline float teleportStep = 100.0f;   // 步长：按钮=单次跳步距离；按住移动键时每秒移动 20×步长
    inline int teleportToggleKey = VK_F11; // 切换瞬移的快捷键
    // 方向请求：1前 2后 3左 4右 5上 6下。菜单按钮写入，数据线程消费后清零。
    inline std::atomic<int> teleportRequest{ 0 };

    // Input Settings
    inline float mouseSensitivity = 1.0f;  // In-game mouse sensitivity (for aim/trigger mouse conversion)

    // Viewport mapping: 0=auto black-bar detection, 1=full client,
    // 2=force 4:3 black bars, 3=force 16:10 black bars.
    inline int viewportMode = 0;
    inline int viewportCycleKey = VK_PRIOR; // 循环视口模式（自动/全客户端/4:3/16:10）

    // The local overlay and browser use the same fixed north-up map catalogue
    // and full GameSnapshot. Position values are normalized to the available
    // viewport so resolution and black-bar changes do not move the panel out
    // of bounds.
    inline bool localRadarEnabled = false;
    inline bool localRadarShowNames = true;
    inline float localRadarAnchorX = 0.02f;
    inline float localRadarAnchorY = 0.08f;
    inline float localRadarSize = 0.32f;
    inline float localRadarMarkerSize = 12.0f;

    // Fixed-map browser Radar settings. The HTTP service is local-only unless
    // the user explicitly enables LAN access; stream URLs always carry a token.
    inline bool webRadarEnabled = false;
    inline bool webRadarLanAccess = false;
    inline bool webRadarPauseWhenUnfocused = true;
    inline bool webRadarIncludePlayerNames = true;
    inline bool webRadarIncludeSteamIds = false;
    inline int webRadarTeamViewPolicy = 0;
    inline int webRadarPort = 22006;
    inline bool radarRecordingEnabled = false;

    // Public Relay credentials intentionally live only in process memory and
    // are never included in any settings persistence path. The producer token
    // is rendered with ImGui's password mode and is never copied to status.
    inline bool publicRelayEnabled = false;
    inline bool publicRelayIncludePlayerNames = true;
    inline bool publicRelayIncludeSteamIds = false;
    inline int publicRelayTeamViewPolicy = 0;
    inline std::array<char, 512> publicRelayUrl{};
    inline std::array<char, 65> publicRelayRoom{};
    inline std::array<char, 513> publicRelayToken{};
    inline std::shared_ptr<const PublicRelayConnectionSettings>
        publicRelayConnectionSnapshot;

    struct WebRadarUiStatus
    {
        bool running = false;
        size_t viewers = 0;
        std::uint64_t publishedFrames = 0;
        std::uint64_t sentFrames = 0;
        std::uint64_t replacedFrames = 0;
        std::uint64_t publishedBytes = 0;
        double maximumSendLatencyMilliseconds = 0.0;
        std::string viewerUrl;
        std::string bindAddress = "127.0.0.1";
        std::string error;
    };

    inline std::mutex webRadarStatusMutex;
    inline WebRadarUiStatus webRadarStatus;

    inline void setWebRadarStatus(WebRadarUiStatus status)
    {
        std::lock_guard<std::mutex> lock(webRadarStatusMutex);
        webRadarStatus = std::move(status);
    }

    inline WebRadarUiStatus getWebRadarStatus()
    {
        std::lock_guard<std::mutex> lock(webRadarStatusMutex);
        return webRadarStatus;
    }

    inline std::mutex recorderStatusMutex;
    inline web_radar::SnapshotRecorderStatus recorderStatus;

    inline void setRecorderStatus(
        web_radar::SnapshotRecorderStatus status)
    {
        std::lock_guard<std::mutex> lock(recorderStatusMutex);
        recorderStatus = std::move(status);
    }

    inline web_radar::SnapshotRecorderStatus getRecorderStatus()
    {
        std::lock_guard<std::mutex> lock(recorderStatusMutex);
        return recorderStatus;
    }

    struct PublicRelayUiStatus
    {
        web_radar::PublicRelayState state =
            web_radar::PublicRelayState::disabled;
        std::uint64_t framesSent = 0;
        std::uint64_t replacedFrames = 0;
        std::uint64_t droppedFrames = 0;
        std::uint64_t reconnects = 0;
        std::string error;
    };

    inline std::mutex publicRelayStatusMutex;
    inline PublicRelayUiStatus publicRelayStatus;

    inline void setPublicRelayStatus(PublicRelayUiStatus status)
    {
        std::lock_guard<std::mutex> lock(publicRelayStatusMutex);
        publicRelayStatus = std::move(status);
    }

    inline PublicRelayUiStatus getPublicRelayStatus()
    {
        std::lock_guard<std::mutex> lock(publicRelayStatusMutex);
        return publicRelayStatus;
    }

    // Misc Settings
    inline bool antiFlash = false;          // Memory writes are opt-in
    inline bool bombTimer = true;            // Show bomb timer on screen
    inline bool grenadeESP = false;          // Show grenade positions
    inline bool droppedWeaponESP = false;    // Show dropped weapon positions

    // Menu Toggle Key
    inline int menuToggleKey = VK_F4;        // Menu toggle key (default: F4)
    inline int exitKey = VK_F9;              // Exit key (default: F9)

    // Hotkey binding state
    inline bool isBindingKey = false;
    inline int* bindingKeyTarget = nullptr;
    inline const char* bindingKeyName = nullptr;
    inline bool bindingWaitingForRelease = false;
    inline std::string bindingError;
    // 重复绑定时只是黄色提示（不阻止）；真正错误时才用红色展示
    inline bool bindingErrorIsWarning = false;
    inline bool suppressHotkeysUntilRelease = false;

    inline RuntimeConfig buildRuntimeConfig()
    {
        RuntimeConfig config{};
        config.espEnabled = espEnabled;
        config.espWeapon = espWeapon;
        config.espFlashIndicator = espFlashIndicator;
        config.antiFlash = antiFlash;
        config.espViewAngle = espViewAngle;
        config.localRadarEnabled = localRadarEnabled;
        config.localRadarShowNames = localRadarShowNames;
        config.localRadarAnchorX = std::clamp(
            localRadarAnchorX,
            0.0f,
            1.0f);
        config.localRadarAnchorY = std::clamp(
            localRadarAnchorY,
            0.0f,
            1.0f);
        config.localRadarSize = std::clamp(
            localRadarSize,
            0.18f,
            0.65f);
        config.localRadarMarkerSize = std::clamp(
            localRadarMarkerSize,
            6.0f,
            24.0f);
        config.webRadarEnabled = webRadarEnabled;
        config.webRadarLanAccess = webRadarLanAccess;
        config.webRadarPauseWhenUnfocused =
            webRadarPauseWhenUnfocused;
        config.webRadarIncludePlayerNames =
            webRadarIncludePlayerNames;
        config.webRadarIncludeSteamIds =
            webRadarIncludeSteamIds;
        config.webRadarTeamViewPolicy = std::clamp(
            webRadarTeamViewPolicy,
            0,
            2);
        config.webRadarPort = static_cast<uint16_t>(
            std::clamp(webRadarPort, 1024, 65535));
        config.radarRecordingEnabled = radarRecordingEnabled;
        config.publicRelayEnabled = publicRelayEnabled;
        config.publicRelayIncludePlayerNames =
            publicRelayIncludePlayerNames;
        config.publicRelayIncludeSteamIds =
            publicRelayIncludeSteamIds;
        config.publicRelayTeamViewPolicy = std::clamp(
            publicRelayTeamViewPolicy,
            0,
            2);
        config.publicRelayConnection = publicRelayConnectionSnapshot;
        config.espWallCheck = espWallCheck;
        config.espSkeleton = espSkeleton;
        config.grenadeESP = grenadeESP;
        config.droppedWeaponESP = droppedWeaponESP;
        config.bombTimer = bombTimer;

        config.headOffsetEnabled = headOffsetEnabled;
        config.headOffsetAmount = headOffsetAmount;
        config.headOffsetAngleMin = headOffsetAngleMin;
        config.headOffsetAngleMax = headOffsetAngleMax;
        config.aimbotEnabled = aimbotEnabled;
        config.aimbotFOV = aimbotFOV;
        config.aimbotSmoothing = aimbotSmoothing;
        config.aimbotBone = aimbotBone;
        config.aimbotVisibleOnly = aimbotVisibleOnly;
        config.aimbotKey = aimbotKey;
        config.smartAimEnabled = smartAimEnabled;
        config.smartAimPriority = smartAimPriority;
        config.smartAimUseFov = smartAimUseFov;
        config.mouseSensitivity = mouseSensitivity;
        config.aimbotModeToggleKey = aimbotModeToggleKey;
        config.boneCycleKey = boneCycleKey;
        config.smartAimPriorityCycleKey = smartAimPriorityCycleKey;
        config.viewportCycleKey = viewportCycleKey;

        config.triggerbotEnabled = triggerbotEnabled;
        config.triggerbotDelay = triggerbotDelay;
        config.triggerbotKey = triggerbotKey;

        config.rcsEnabled = rcsEnabled;
        config.rcsInvert = rcsInvert;
        config.rcsInjectRatio = rcsInjectRatio;
        config.rcsMaxMovePx = rcsMaxMovePx;
        config.rcsStrength = rcsStrength;
        config.recoilRecordKey = recoilRecordKey;
        config.rcsToggleKey = rcsToggleKey;
        config.aimbotMode = aimbotMode;
        config.magicBulletRecoil = magicBulletRecoil;
        config.magicTrajectoryEnabled = magicTrajectoryEnabled;
        config.magicTrajectoryToggleKey = magicTrajectoryToggleKey;
        config.magicBulletRecenter = magicBulletRecenter;
        config.autoRotationEnabled = autoRotationEnabled;
        config.autoRotationSpeed = autoRotationSpeed;
        config.autoRotationToggleKey = autoRotationToggleKey;
        config.bhopEnabled = bhopEnabled;
        config.bhopHumanize = bhopHumanize;
        config.counterStrafEnabled = counterStrafEnabled;
        config.counterStrafToggleKey = counterStrafToggleKey;
        config.teleportEnabled = teleportEnabled;
        config.teleportStep = teleportStep;
        config.teleportToggleKey = teleportToggleKey;

        config.inputSuppressed =
            isBindingKey || suppressHotkeysUntilRelease;
        return config;
    }

    inline RuntimeConfig runtimeConfigSnapshot = buildRuntimeConfig();

    inline RuntimeConfig getRuntimeConfig()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        return runtimeConfigSnapshot;
    }

    // 压枪开关热键：翻转 自动压枪 的启用状态（压枪线程调用，锁保护）
    inline void toggleRcs()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        rcsEnabled = !rcsEnabled;
    }

    // 急停开关热键：翻转 自动急停 的启用状态（压枪线程调用，锁保护）
    inline void toggleCounterStraf()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        counterStrafEnabled = !counterStrafEnabled;
    }

    // 自动旋转开关热键：翻转 自动旋转 的启用状态（压枪线程调用，锁保护）
    inline void toggleAutoRotation()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        autoRotationEnabled = !autoRotationEnabled;
    }

    // 角色瞬移开关热键：翻转 瞬移 的启用状态（压枪线程调用，锁保护）
    inline void toggleTeleport()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        teleportEnabled = !teleportEnabled;
    }

    // 自瞄模式切换热键：在 鼠标自瞄 / 魔法子弹 之间切换（压枪线程调用，锁保护）
    inline void toggleAimMode()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        aimbotMode = (aimbotMode == 0) ? 1 : 0;
    }

    // 弹道偏移开关热键：翻转 开枪自动弹道偏移（压枪线程调用，锁保护）
    inline void toggleMagicTrajectory()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        magicTrajectoryEnabled = !magicTrajectoryEnabled;
    }

    // 命中部位循环：0头 1脖子 2胸 3腹 4左臂 5右臂 6左腿 7右腿
    inline void cycleBone()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        aimbotBone = (aimbotBone + 1) % 8;
    }

    // 智能瞄准优先级循环：0人物距离 1视角距离 2血量
    inline void cycleSmartAimPriority()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        smartAimPriority = (smartAimPriority + 1) % 3;
    }

    // 视口模式循环：0自动 1全客户端 2 4:3 3 16:10（渲染线程直接读全局，这里加锁保护）
    inline void cycleViewportMode()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        viewportMode = (viewportMode + 1) % 4;
    }

    // ------------------------------------------------------------------
    // publishRuntimeConfig：把界面上的变量打包成 RuntimeConfig 快照
    // ------------------------------------------------------------------
    // 同时负责公网中继凭据的快照管理：界面输入框里的 URL/房间/token
    // 有变化时，生成一个新的 PublicRelayConnectionSettings（不可变对象）。
    // 凭据只存在进程内存里，绝不写进持久化设置文件。
    inline void publishRuntimeConfig()
    {
        // 整个函数体都在 configMutex 内：渲染线程（每帧）与压枪线程（按下热键）
        // 可能并发调用，若只在最后给 runtimeConfigSnapshot 加锁，前面对
        // publicRelayConnectionSnapshot（std::shared_ptr）的读写会与其它线程竞争，
        // 触发 shared_ptr 控制块的竞态 → 闪退。整体加锁后串行化，安全。
        std::lock_guard<std::mutex> lock(configMutex);
        const std::string_view currentUrl(publicRelayUrl.data());
        const std::string_view currentRoom(publicRelayRoom.data());
        const std::string_view currentToken(publicRelayToken.data());
        if (currentUrl.empty() && currentRoom.empty() && currentToken.empty()) {
            publicRelayConnectionSnapshot.reset();
        } else if (!publicRelayConnectionSnapshot ||
                   publicRelayConnectionSnapshot->endpointUrl() != currentUrl ||
                   publicRelayConnectionSnapshot->room() != currentRoom ||
                   publicRelayConnectionSnapshot->token() != currentToken) {
            publicRelayConnectionSnapshot =
                std::make_shared<const PublicRelayConnectionSettings>(
                    currentUrl,
                    currentRoom,
                    currentToken);
        }
        runtimeConfigSnapshot = buildRuntimeConfig();
    }

    // ------------------------------------------------------------------
    // 设置持久化：读/写 %LOCALAPPDATA%\AegisCS2\settings-v1.ini
    // ------------------------------------------------------------------
    // 用 Windows 自带的 INI 读写 API（Get/WritePrivateProfileStringW），
    // 存的是纯文本键值对，保存/恢复少量设置（视口模式、雷达位置等）。
    // 注意：只持久化"非敏感"的设置，token 类凭据从不写入。

    // 设置文件的路径（在 LOCALAPPDATA\AegisCS2 下）
    inline std::filesystem::path persistentSettingsPath()
    {
        std::array<wchar_t, 32768> localAppData{};
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        std::filesystem::path directory =
            length > 0 && length < localAppData.size()
                ? std::filesystem::path(
                    std::wstring_view(localAppData.data(), length)) /
                    L"AegisCS2"
                : std::filesystem::temp_directory_path() / L"AegisCS2";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return directory / L"settings-v1.ini";
    }

    inline int readPersistentInt(
        const wchar_t* key,
        const int fallback,
        const std::filesystem::path& path)
    {
        return static_cast<int>(GetPrivateProfileIntW(
            L"settings",
            key,
            fallback,
            path.c_str()));
    }

    inline float readPersistentFloat(
        const wchar_t* key,
        const float fallback,
        const std::filesystem::path& path)
    {
        std::array<wchar_t, 64> fallbackText{};
        std::array<wchar_t, 64> value{};
        swprintf_s(fallbackText.data(), fallbackText.size(), L"%.6f", fallback);
        GetPrivateProfileStringW(
            L"settings",
            key,
            fallbackText.data(),
            value.data(),
            static_cast<DWORD>(value.size()),
            path.c_str());
        wchar_t* end = nullptr;
        const float parsed = std::wcstof(value.data(), &end);
        return end != value.data() && std::isfinite(parsed)
            ? parsed
            : fallback;
    }

    // 读取一个 "r,g,b,a" 颜色字符串；缺失或解析失败时用 fallback
    inline void readPersistentColor(
        const wchar_t* key,
        float out[4],
        const float fallback[4],
        const std::filesystem::path& path)
    {
        std::array<wchar_t, 64> value{};
        GetPrivateProfileStringW(
            L"settings",
            key,
            L"",
            value.data(),
            static_cast<DWORD>(value.size()),
            path.c_str());
        if (value[0] == L'\0' ||
            swscanf_s(
                value.data(),
                L"%f,%f,%f,%f",
                &out[0],
                &out[1],
                &out[2],
                &out[3]) != 4) {
            std::copy(fallback, fallback + 4, out);
        }
    }

    inline void loadPersistentSettings()
    {
        const std::filesystem::path path = persistentSettingsPath();
        if (readPersistentInt(L"schema", 0, path) != 1) {
            publishRuntimeConfig();
            return;
        }

        // ---- 全局 / 热键 ----
        menuToggleKey = readPersistentInt(
            L"menu_toggle_key", menuToggleKey, path);
        exitKey = readPersistentInt(
            L"exit_key", exitKey, path);
        viewportMode = std::clamp(
            readPersistentInt(L"viewport_mode", viewportMode, path),
            0,
            3);

        // ---- ESP ----
        espEnabled = readPersistentInt(
            L"esp_enabled", espEnabled ? 1 : 0, path) != 0;
        espBox = readPersistentInt(
            L"esp_box", espBox ? 1 : 0, path) != 0;
        espHealth = readPersistentInt(
            L"esp_health", espHealth ? 1 : 0, path) != 0;
        espDistance = readPersistentInt(
            L"esp_distance", espDistance ? 1 : 0, path) != 0;
        espWeapon = readPersistentInt(
            L"esp_weapon", espWeapon ? 1 : 0, path) != 0;
        espViewAngle = readPersistentInt(
            L"esp_view_angle", espViewAngle ? 1 : 0, path) != 0;
        espViewAngleText = readPersistentInt(
            L"esp_view_angle_text", espViewAngleText ? 1 : 0, path) != 0;
        espFlashIndicator = readPersistentInt(
            L"esp_flash_indicator", espFlashIndicator ? 1 : 0, path) != 0;
        espWallCheck = readPersistentInt(
            L"esp_wall_check", espWallCheck ? 1 : 0, path) != 0;
        espSnaplines = readPersistentInt(
            L"esp_snaplines", espSnaplines ? 1 : 0, path) != 0;
        espSkeleton = readPersistentInt(
            L"esp_skeleton", espSkeleton ? 1 : 0, path) != 0;
        snaplinesOrigin = std::clamp(
            readPersistentInt(
                L"esp_snaplines_origin", snaplinesOrigin, path),
            0,
            2);
        readPersistentColor(
            L"esp_box_color", espBoxColor, espBoxColor, path);
        readPersistentColor(
            L"esp_wall_color", espWallColor, espWallColor, path);
        readPersistentColor(
            L"esp_distance_color", espDistanceColor, espDistanceColor, path);
        readPersistentColor(
            L"esp_weapon_color", espWeaponColor, espWeaponColor, path);
        readPersistentColor(
            L"esp_flash_normal_color",
            espFlashNormalColor,
            espFlashNormalColor,
            path);
        readPersistentColor(
            L"esp_flash_color", espFlashColor, espFlashColor, path);
        readPersistentColor(
            L"esp_snaplines_color",
            espSnaplinesColor,
            espSnaplinesColor,
            path);
        readPersistentColor(
            L"esp_skeleton_color",
            espSkeletonColor,
            espSkeletonColor,
            path);

        // ---- 自动瞄准 ----
        aimbotEnabled = readPersistentInt(
            L"aimbot_enabled", aimbotEnabled ? 1 : 0, path) != 0;
        aimbotFOV = std::clamp(
            readPersistentFloat(L"aimbot_fov", aimbotFOV, path),
            1.0f, 180.0f);
        aimbotSmoothing = std::clamp(
            readPersistentFloat(
                L"aimbot_smoothing", aimbotSmoothing, path),
            1.0f, 20.0f);
        aimbotBone = std::clamp(
            readPersistentInt(L"aimbot_bone", aimbotBone, path),
            0, 2);
        aimbotVisibleOnly = readPersistentInt(
            L"aimbot_visible_only",
            aimbotVisibleOnly ? 1 : 0,
            path) != 0;
        aimbotKey = readPersistentInt(L"aimbot_key", aimbotKey, path);
        aimbotShowFOV = readPersistentInt(
            L"aimbot_show_fov", aimbotShowFOV ? 1 : 0, path) != 0;
        readPersistentColor(
            L"aimbot_fov_color", aimbotFOVColor, aimbotFOVColor, path);
        headOffsetEnabled = readPersistentInt(
            L"head_offset_enabled",
            headOffsetEnabled ? 1 : 0,
            path) != 0;
        headOffsetAmount = std::clamp(
            readPersistentFloat(
                L"head_offset_amount", headOffsetAmount, path),
            0.0f, 15.0f);
        headOffsetAngleMin = std::clamp(
            readPersistentFloat(
                L"head_offset_angle_min", headOffsetAngleMin, path),
            0.0f, 90.0f);
        headOffsetAngleMax = std::clamp(
            readPersistentFloat(
                L"head_offset_angle_max", headOffsetAngleMax, path),
            90.0f, 180.0f);
        smartAimEnabled = readPersistentInt(
            L"smart_aim_enabled", smartAimEnabled ? 1 : 0, path) != 0;
        smartAimPriority = std::clamp(
            readPersistentInt(L"smart_aim_priority", smartAimPriority, path),
            0, 2);
        smartAimUseFov = readPersistentInt(
            L"smart_aim_use_fov", smartAimUseFov ? 1 : 0, path) != 0;
        mouseSensitivity = std::clamp(
            readPersistentFloat(
                L"mouse_sensitivity", mouseSensitivity, path),
            0.1f, 10.0f);

        // ---- 自瞄模式切换热键 / 延迟开火容差 ----
        aimbotModeToggleKey = readPersistentInt(
            L"aimbot_mode_toggle_key", aimbotModeToggleKey, path);
        boneCycleKey = readPersistentInt(
            L"bone_cycle_key", boneCycleKey, path);
        smartAimPriorityCycleKey = readPersistentInt(
            L"smart_aim_priority_cycle_key", smartAimPriorityCycleKey, path);
        viewportCycleKey = readPersistentInt(
            L"viewport_cycle_key", viewportCycleKey, path);

        // ---- 自动扳机 ----
        triggerbotEnabled = readPersistentInt(
            L"triggerbot_enabled",
            triggerbotEnabled ? 1 : 0,
            path) != 0;
        triggerbotDelay = std::clamp(
            readPersistentInt(L"triggerbot_delay", triggerbotDelay, path),
            0, 500);
        triggerbotKey = readPersistentInt(
            L"triggerbot_key", triggerbotKey, path);

        // ---- RCS 压枪参数 + 弹道录制热键 ----
        rcsEnabled = readPersistentInt(
            L"rcs_enabled", rcsEnabled ? 1 : 0, path) != 0;
        rcsInvert = readPersistentInt(
            L"rcs_invert", rcsInvert ? 1 : 0, path) != 0;
        rcsInjectRatio = std::clamp(
            readPersistentFloat(
                L"rcs_inject_ratio", rcsInjectRatio, path),
            0.1f, 1.0f);
        rcsMaxMovePx = std::clamp(
            readPersistentInt(
                L"rcs_max_move_px", rcsMaxMovePx, path),
            5, 120);
        rcsStrength = std::clamp(
            readPersistentFloat(L"rcs_strength", rcsStrength, path),
            0.0f, 3.0f);
        recoilRecordKey = readPersistentInt(
            L"recoil_record_key", recoilRecordKey, path);
        rcsToggleKey = readPersistentInt(
            L"rcs_toggle_key", rcsToggleKey, path);
        counterStrafEnabled = readPersistentInt(
            L"counter_straf_enabled",
            counterStrafEnabled ? 1 : 0,
            path) != 0;
        counterStrafToggleKey = readPersistentInt(
            L"counter_straf_toggle_key", counterStrafToggleKey, path);

        // ---- 角色瞬移 ----
        teleportEnabled = readPersistentInt(
            L"teleport_enabled", teleportEnabled ? 1 : 0, path) != 0;
        teleportStep = std::clamp(
            readPersistentFloat(L"teleport_step", teleportStep, path),
            0.0f, 2000.0f);
        teleportToggleKey = readPersistentInt(
            L"teleport_toggle_key", teleportToggleKey, path);

        // ---- 自瞄模式 / 魔法子弹 / 自动旋转 / 连跳 ----
        aimbotMode = std::clamp(
            readPersistentInt(L"aimbot_mode", aimbotMode, path),
            0, 1);
        magicBulletRecoil = std::clamp(
            readPersistentFloat(
                L"magic_bullet_recoil", magicBulletRecoil, path),
            0.0f, 2.0f);
        magicTrajectoryEnabled = readPersistentInt(
            L"magic_trajectory_enabled",
            magicTrajectoryEnabled ? 1 : 0,
            path) != 0;
        magicTrajectoryToggleKey = readPersistentInt(
            L"magic_trajectory_toggle_key", magicTrajectoryToggleKey, path);
        magicBulletRecenter = readPersistentInt(
            L"magic_bullet_recenter", magicBulletRecenter ? 1 : 0, path) != 0;
        autoRotationEnabled = readPersistentInt(
            L"auto_rotation_enabled",
            autoRotationEnabled ? 1 : 0,
            path) != 0;
        autoRotationSpeed = std::clamp(
            readPersistentFloat(
                L"auto_rotation_speed", autoRotationSpeed, path),
            0.0f, 3600.0f);
        autoRotationToggleKey = readPersistentInt(
            L"auto_rotation_toggle_key", autoRotationToggleKey, path);
        bhopEnabled = readPersistentInt(
            L"bhop_enabled", bhopEnabled ? 1 : 0, path) != 0;
        bhopHumanize = std::clamp(
            readPersistentFloat(L"bhop_humanize", bhopHumanize, path),
            0.0f, 100.0f);

        // ---- 杂项 / 世界 ESP ----
        antiFlash = readPersistentInt(
            L"anti_flash", antiFlash ? 1 : 0, path) != 0;
        bombTimer = readPersistentInt(
            L"bomb_timer", bombTimer ? 1 : 0, path) != 0;
        grenadeESP = readPersistentInt(
            L"grenade_esp", grenadeESP ? 1 : 0, path) != 0;
        droppedWeaponESP = readPersistentInt(
            L"dropped_weapon_esp",
            droppedWeaponESP ? 1 : 0,
            path) != 0;

        // ---- 雷达 / 中继（原有项 + 补充的开关） ----
        localRadarEnabled = readPersistentInt(
            L"local_radar_enabled",
            localRadarEnabled ? 1 : 0,
            path) != 0;
        localRadarShowNames = readPersistentInt(
            L"local_radar_names",
            localRadarShowNames ? 1 : 0,
            path) != 0;
        localRadarAnchorX = std::clamp(
            readPersistentFloat(
                L"local_radar_anchor_x",
                localRadarAnchorX,
                path),
            0.0f,
            1.0f);
        localRadarAnchorY = std::clamp(
            readPersistentFloat(
                L"local_radar_anchor_y",
                localRadarAnchorY,
                path),
            0.0f,
            1.0f);
        localRadarSize = std::clamp(
            readPersistentFloat(
                L"local_radar_size",
                localRadarSize,
                path),
            0.18f,
            0.65f);
        localRadarMarkerSize = std::clamp(
            readPersistentFloat(
                L"local_radar_marker_size",
                localRadarMarkerSize,
                path),
            6.0f,
            24.0f);
        webRadarEnabled = readPersistentInt(
            L"web_radar_enabled",
            webRadarEnabled ? 1 : 0,
            path) != 0;
        webRadarLanAccess = readPersistentInt(
            L"web_radar_lan_access",
            webRadarLanAccess ? 1 : 0,
            path) != 0;
        webRadarPort = std::clamp(
            readPersistentInt(L"web_radar_port", webRadarPort, path),
            1024,
            65535);
        webRadarPauseWhenUnfocused = readPersistentInt(
            L"pause_shared_radar_unfocused",
            webRadarPauseWhenUnfocused ? 1 : 0,
            path) != 0;
        webRadarIncludePlayerNames = readPersistentInt(
            L"web_radar_player_names",
            webRadarIncludePlayerNames ? 1 : 0,
            path) != 0;
        webRadarIncludeSteamIds = readPersistentInt(
            L"web_radar_steam_ids",
            webRadarIncludeSteamIds ? 1 : 0,
            path) != 0;
        webRadarTeamViewPolicy = std::clamp(
            readPersistentInt(
                L"web_radar_team_policy",
                webRadarTeamViewPolicy,
                path),
            0,
            2);
        radarRecordingEnabled = readPersistentInt(
            L"radar_recording_enabled",
            radarRecordingEnabled ? 1 : 0,
            path) != 0;
        publicRelayIncludePlayerNames = readPersistentInt(
            L"public_relay_player_names",
            publicRelayIncludePlayerNames ? 1 : 0,
            path) != 0;
        publicRelayIncludeSteamIds = readPersistentInt(
            L"public_relay_steam_ids",
            publicRelayIncludeSteamIds ? 1 : 0,
            path) != 0;
        publicRelayTeamViewPolicy = std::clamp(
            readPersistentInt(
                L"public_relay_team_policy",
                publicRelayTeamViewPolicy,
                path),
            0,
            2);

        publishRuntimeConfig();
    }

    inline void writePersistentValue(
        const wchar_t* key,
        const std::wstring_view value,
        const std::filesystem::path& path)
    {
        const std::wstring owned(value);
        WritePrivateProfileStringW(
            L"settings",
            key,
            owned.c_str(),
            path.c_str());
    }

    inline void savePersistentSettings()
    {
        const std::filesystem::path path = persistentSettingsPath();
        writePersistentValue(L"schema", L"1", path);

        const auto writeFloat = [&path](
            const wchar_t* key,
            const float value) {
            std::array<wchar_t, 64> text{};
            swprintf_s(text.data(), text.size(), L"%.6f", value);
            writePersistentValue(key, text.data(), path);
        };
        const auto writeColor = [&path](
            const wchar_t* key,
            const float color[4]) {
            std::array<wchar_t, 64> text{};
            swprintf_s(
                text.data(),
                text.size(),
                L"%.4f,%.4f,%.4f,%.4f",
                color[0],
                color[1],
                color[2],
                color[3]);
            writePersistentValue(key, text.data(), path);
        };
        const auto writeBool = [&path](
            const wchar_t* key,
            const bool value) {
            writePersistentValue(key, value ? L"1" : L"0", path);
        };

        // ---- 全局 / 热键 ----
        writePersistentValue(
            L"menu_toggle_key",
            std::to_wstring(menuToggleKey),
            path);
        writePersistentValue(
            L"exit_key",
            std::to_wstring(exitKey),
            path);
        writePersistentValue(
            L"viewport_mode",
            std::to_wstring(std::clamp(viewportMode, 0, 3)),
            path);

        // ---- ESP ----
        writeBool(L"esp_enabled", espEnabled);
        writeBool(L"esp_box", espBox);
        writeBool(L"esp_health", espHealth);
        writeBool(L"esp_distance", espDistance);
        writeBool(L"esp_weapon", espWeapon);
        writeBool(L"esp_view_angle", espViewAngle);
        writeBool(L"esp_view_angle_text", espViewAngleText);
        writeBool(L"esp_flash_indicator", espFlashIndicator);
        writeBool(L"esp_wall_check", espWallCheck);
        writeBool(L"esp_snaplines", espSnaplines);
        writeBool(L"esp_skeleton", espSkeleton);
        writePersistentValue(
            L"esp_snaplines_origin",
            std::to_wstring(std::clamp(snaplinesOrigin, 0, 2)),
            path);
        writeColor(L"esp_box_color", espBoxColor);
        writeColor(L"esp_wall_color", espWallColor);
        writeColor(L"esp_distance_color", espDistanceColor);
        writeColor(L"esp_weapon_color", espWeaponColor);
        writeColor(L"esp_flash_normal_color", espFlashNormalColor);
        writeColor(L"esp_flash_color", espFlashColor);
        writeColor(L"esp_snaplines_color", espSnaplinesColor);
        writeColor(L"esp_skeleton_color", espSkeletonColor);

        // ---- 自动瞄准 ----
        writeBool(L"aimbot_enabled", aimbotEnabled);
        writeFloat(L"aimbot_fov", aimbotFOV);
        writeFloat(L"aimbot_smoothing", aimbotSmoothing);
        writePersistentValue(
            L"aimbot_bone",
            std::to_wstring(std::clamp(aimbotBone, 0, 2)),
            path);
        writeBool(L"aimbot_visible_only", aimbotVisibleOnly);
        writePersistentValue(
            L"aimbot_key",
            std::to_wstring(aimbotKey),
            path);
        writeBool(L"aimbot_show_fov", aimbotShowFOV);
        writeColor(L"aimbot_fov_color", aimbotFOVColor);
        writeBool(L"head_offset_enabled", headOffsetEnabled);
        writeFloat(L"head_offset_amount", headOffsetAmount);
        writeFloat(L"head_offset_angle_min", headOffsetAngleMin);
        writeFloat(L"head_offset_angle_max", headOffsetAngleMax);
        writeBool(L"smart_aim_enabled", smartAimEnabled);
        writePersistentValue(
            L"smart_aim_priority",
            std::to_wstring(std::clamp(smartAimPriority, 0, 2)),
            path);
        writeBool(L"smart_aim_use_fov", smartAimUseFov);
        writeFloat(L"mouse_sensitivity", mouseSensitivity);
        writePersistentValue(
            L"aimbot_mode_toggle_key",
            std::to_wstring(aimbotModeToggleKey),
            path);
        writePersistentValue(
            L"bone_cycle_key",
            std::to_wstring(boneCycleKey),
            path);
        writePersistentValue(
            L"smart_aim_priority_cycle_key",
            std::to_wstring(smartAimPriorityCycleKey),
            path);
        writePersistentValue(
            L"viewport_cycle_key",
            std::to_wstring(viewportCycleKey),
            path);
        // ---- 自动扳机 ----
        writeBool(L"triggerbot_enabled", triggerbotEnabled);
        writePersistentValue(
            L"triggerbot_delay",
            std::to_wstring(std::clamp(triggerbotDelay, 0, 500)),
            path);
        writePersistentValue(
            L"triggerbot_key",
            std::to_wstring(triggerbotKey),
            path);

        // ---- RCS 压枪参数 + 弹道录制热键 ----
        writeBool(L"rcs_enabled", rcsEnabled);
        writeBool(L"rcs_invert", rcsInvert);
        writeFloat(L"rcs_inject_ratio", rcsInjectRatio);
        writePersistentValue(
            L"rcs_max_move_px",
            std::to_wstring(std::clamp(rcsMaxMovePx, 5, 120)),
            path);
        writeFloat(L"rcs_strength", rcsStrength);
        writePersistentValue(
            L"recoil_record_key",
            std::to_wstring(recoilRecordKey),
            path);
        writePersistentValue(
            L"rcs_toggle_key",
            std::to_wstring(rcsToggleKey),
            path);
        writeBool(L"counter_straf_enabled", counterStrafEnabled);
        writePersistentValue(
            L"counter_straf_toggle_key",
            std::to_wstring(counterStrafToggleKey),
            path);
        // ---- 角色瞬移 ----
        writeBool(L"teleport_enabled", teleportEnabled);
        writeFloat(L"teleport_step", teleportStep);
        writePersistentValue(
            L"teleport_toggle_key",
            std::to_wstring(teleportToggleKey),
            path);

        // ---- 自瞄模式 / 魔法子弹 / 自动旋转 / 连跳 ----
        writePersistentValue(
            L"aimbot_mode",
            std::to_wstring(std::clamp(aimbotMode, 0, 1)),
            path);
        writeFloat(L"magic_bullet_recoil", magicBulletRecoil);
        writeBool(L"magic_trajectory_enabled", magicTrajectoryEnabled);
        writePersistentValue(
            L"magic_trajectory_toggle_key",
            std::to_wstring(magicTrajectoryToggleKey),
            path);
        writeBool(L"magic_bullet_recenter", magicBulletRecenter);
        writeBool(L"auto_rotation_enabled", autoRotationEnabled);
        writeFloat(L"auto_rotation_speed", autoRotationSpeed);
        writePersistentValue(
            L"auto_rotation_toggle_key",
            std::to_wstring(autoRotationToggleKey),
            path);
        writeBool(L"bhop_enabled", bhopEnabled);
        writeFloat(L"bhop_humanize", bhopHumanize);

        // ---- 杂项 / 世界 ESP ----
        writeBool(L"anti_flash", antiFlash);
        writeBool(L"bomb_timer", bombTimer);
        writeBool(L"grenade_esp", grenadeESP);
        writeBool(L"dropped_weapon_esp", droppedWeaponESP);

        // ---- 雷达 / 中继（原有项 + 补充的开关） ----
        writeBool(L"local_radar_enabled", localRadarEnabled);
        writeBool(L"local_radar_names", localRadarShowNames);
        writeFloat(L"local_radar_anchor_x", localRadarAnchorX);
        writeFloat(L"local_radar_anchor_y", localRadarAnchorY);
        writeFloat(L"local_radar_size", localRadarSize);
        writeFloat(L"local_radar_marker_size", localRadarMarkerSize);
        writeBool(L"web_radar_enabled", webRadarEnabled);
        writeBool(L"web_radar_lan_access", webRadarLanAccess);
        writePersistentValue(
            L"web_radar_port",
            std::to_wstring(std::clamp(webRadarPort, 1024, 65535)),
            path);
        writeBool(L"pause_shared_radar_unfocused", webRadarPauseWhenUnfocused);
        writeBool(L"web_radar_player_names", webRadarIncludePlayerNames);
        writeBool(L"web_radar_steam_ids", webRadarIncludeSteamIds);
        writePersistentValue(
            L"web_radar_team_policy",
            std::to_wstring(std::clamp(webRadarTeamViewPolicy, 0, 2)),
            path);
        writeBool(L"radar_recording_enabled", radarRecordingEnabled);
        writeBool(L"public_relay_player_names", publicRelayIncludePlayerNames);
        writeBool(L"public_relay_steam_ids", publicRelayIncludeSteamIds);
        writePersistentValue(
            L"public_relay_team_policy",
            std::to_wstring(std::clamp(publicRelayTeamViewPolicy, 0, 2)),
            path);
    }

    // ------------------------------------------------------------------
    // 周期自动保存：主渲染循环每帧调用（内部有 5 秒时间闸）。
    // 设置一改最多 5 秒就落盘一次，异常退出/断电也不会丢太多改动；
    // 正常退出时 main.cpp 仍会再调一次 savePersistentSettings()。
    // ------------------------------------------------------------------
    inline void autosavePersistentSettings()
    {
        static DWORD lastSaveTick = 0;
        const DWORD now = GetTickCount();
        if (now - lastSaveTick >= 5000) {
            lastSaveTick = now;
            savePersistentSettings();
        }
    }

    // ------------------------------------------------------------------
    // 热键绑定相关
    // ------------------------------------------------------------------
    // GetKeyName：虚拟键码 → 人类可读的名字（比如 0x46 → "F"）
    // GetPressedKey：检测当前按下的键（绑定热键时用）
    // UpdateKeyBinding：绑定流程的主循环（每帧调用）：
    //   点按钮 → 等待用户松开所有键（防把"点击按钮的鼠标"当成热键）→
    //   等待按新键 → 与已有热键重复时只提示、不阻止（允许重复绑定）→
    //   保存 → 屏蔽热键直到松开。

    // Convert virtual key code to key name
    inline const char* GetKeyName(int vkCode)
    {
        static char keyName[32];

        switch (vkCode)
        {
        // Special keys
        case VK_LBUTTON: return "Mouse1";
        case VK_RBUTTON: return "Mouse2";
        case VK_MBUTTON: return "Mouse3";
        case VK_XBUTTON1: return "Mouse4";
        case VK_XBUTTON2: return "Mouse5";
        case VK_BACK: return "Backspace";
        case VK_TAB: return "Tab";
        case VK_RETURN: return "Enter";
        case VK_SHIFT: return "Shift";
        case VK_CONTROL: return "Ctrl";
        case VK_MENU: return "Alt";
        case VK_PAUSE: return "Pause";
        case VK_CAPITAL: return "CapsLock";
        case VK_ESCAPE: return "Escape";
        case VK_SPACE: return "Space";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_END: return "End";
        case VK_HOME: return "Home";
        case VK_LEFT: return "Left";
        case VK_UP: return "Up";
        case VK_RIGHT: return "Right";
        case VK_DOWN: return "Down";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_LSHIFT: return "LShift";
        case VK_RSHIFT: return "RShift";
        case VK_LCONTROL: return "LCtrl";
        case VK_RCONTROL: return "RCtrl";
        case VK_LMENU: return "LAlt";
        case VK_RMENU: return "RAlt";

        // Function keys
        case VK_F1: return "F1";
        case VK_F2: return "F2";
        case VK_F3: return "F3";
        case VK_F4: return "F4";
        case VK_F5: return "F5";
        case VK_F6: return "F6";
        case VK_F7: return "F7";
        case VK_F8: return "F8";
        case VK_F9: return "F9";
        case VK_F10: return "F10";
        case VK_F11: return "F11";
        case VK_F12: return "F12";

        // Numpad
        case VK_NUMPAD0: return "Num0";
        case VK_NUMPAD1: return "Num1";
        case VK_NUMPAD2: return "Num2";
        case VK_NUMPAD3: return "Num3";
        case VK_NUMPAD4: return "Num4";
        case VK_NUMPAD5: return "Num5";
        case VK_NUMPAD6: return "Num6";
        case VK_NUMPAD7: return "Num7";
        case VK_NUMPAD8: return "Num8";
        case VK_NUMPAD9: return "Num9";
        case VK_MULTIPLY: return "Num*";
        case VK_ADD: return "Num+";
        case VK_SUBTRACT: return "Num-";
        case VK_DECIMAL: return "Num.";
        case VK_DIVIDE: return "Num/";

        // Letters A-Z (0x41 - 0x5A)
        case 0x41: return "A";
        case 0x42: return "B";
        case 0x43: return "C";
        case 0x44: return "D";
        case 0x45: return "E";
        case 0x46: return "F";
        case 0x47: return "G";
        case 0x48: return "H";
        case 0x49: return "I";
        case 0x4A: return "J";
        case 0x4B: return "K";
        case 0x4C: return "L";
        case 0x4D: return "M";
        case 0x4E: return "N";
        case 0x4F: return "O";
        case 0x50: return "P";
        case 0x51: return "Q";
        case 0x52: return "R";
        case 0x53: return "S";
        case 0x54: return "T";
        case 0x55: return "U";
        case 0x56: return "V";
        case 0x57: return "W";
        case 0x58: return "X";
        case 0x59: return "Y";
        case 0x5A: return "Z";

        // Numbers 0-9 (0x30 - 0x39)
        case 0x30: return "0";
        case 0x31: return "1";
        case 0x32: return "2";
        case 0x33: return "3";
        case 0x34: return "4";
        case 0x35: return "5";
        case 0x36: return "6";
        case 0x37: return "7";
        case 0x38: return "8";
        case 0x39: return "9";

        default:
            sprintf_s(keyName, "Key(0x%02X)", vkCode);
            return keyName;
        }
    }

    // Check for key press during binding
    inline int GetPressedKey()
    {
        // Check mouse buttons
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return VK_LBUTTON;
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) return VK_RBUTTON;
        if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) return VK_MBUTTON;
        if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) return VK_XBUTTON1;
        if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) return VK_XBUTTON2;

        // Check all keyboard keys
        for (int i = 0x08; i <= 0xFE; i++)
        {
            // Skip some keys that shouldn't be used
            if (i == VK_ESCAPE) continue;  // Escape cancels binding

            if (GetAsyncKeyState(i) & 0x8000)
                return i;
        }

        return 0;
    }

    inline bool AnyBindableKeyDown()
    {
        for (int key = 0x01; key <= 0xFE; ++key) {
            if (GetAsyncKeyState(key) & 0x8000) {
                return true;
            }
        }
        return false;
    }

    inline bool ConfiguredHotkeysReleased()
    {
        const int keys[] = {
            menuToggleKey,
            exitKey,
            aimbotKey,
            triggerbotKey
        };
        for (int key : keys) {
            if (key > 0 && key <= 0xFF &&
                (GetAsyncKeyState(key) & 0x8000)) {
                return false;
            }
        }
        return true;
    }

    inline const char* FindHotkeyConflict(
        const int* target,
        int candidate)
    {
        struct Binding
        {
            const char* name;
            const int* key;
        };
        const Binding bindings[] = {
            { "菜单开关", &menuToggleKey },
            { "退出程序", &exitKey },
            { "瞄准键", &aimbotKey },
            { "扳机键", &triggerbotKey },
            { "弹道录制", &recoilRecordKey },
            { "压枪开关", &rcsToggleKey },
            { "急停开关", &counterStrafToggleKey },
            { "旋转开关", &autoRotationToggleKey },
            { "瞬移开关", &teleportToggleKey },
            { "自瞄模式", &aimbotModeToggleKey },
            { "弹道偏移", &magicTrajectoryToggleKey },
            { "命中部位", &boneCycleKey },
            { "智能优先级", &smartAimPriorityCycleKey },
            { "视口模式", &viewportCycleKey }
        };
        for (const Binding& binding : bindings) {
            if (binding.key != target && *binding.key == candidate) {
                return binding.name;
            }
        }
        return nullptr;
    }

    // Render hotkey button
    inline void RenderHotkeyButton(const char* label, int* keyCode, const char* tooltip = nullptr)
    {
        const float dpiScale = sdl_renderer::getDpiScale();
        ImGui::Text("%s:", label);
        ImGui::SameLine(150.0f * dpiScale);

        char buttonLabel[64];
        if (isBindingKey && bindingKeyTarget == keyCode)
        {
            sprintf_s(buttonLabel, "[按键...]##%s", label);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.0f, 1.0f));
        }
        else
        {
            sprintf_s(buttonLabel, "%s##%s", GetKeyName(*keyCode), label);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
        }

        if (ImGui::Button(buttonLabel, ImVec2(100.0f * dpiScale, 0.0f)))
        {
            isBindingKey = true;
            bindingKeyTarget = keyCode;
            bindingKeyName = label;
            bindingWaitingForRelease = true;
            bindingError.clear();
            bindingErrorIsWarning = false;
            suppressHotkeysUntilRelease = true;
        }

        ImGui::PopStyleColor();

        if (tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
    }

    // Update key binding (call every frame)
    inline void UpdateKeyBinding()
    {
        if (!isBindingKey || bindingKeyTarget == nullptr)
            return;

        // Check for escape to cancel
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            isBindingKey = false;
            bindingKeyTarget = nullptr;
            bindingKeyName = nullptr;
            bindingWaitingForRelease = false;
            suppressHotkeysUntilRelease = true;
            return;
        }

        // Do not capture the mouse click that opened the binding button, or
        // any modifier that was already held at that moment.
        if (bindingWaitingForRelease) {
            if (!AnyBindableKeyDown()) {
                bindingWaitingForRelease = false;
            }
            return;
        }

        int pressedKey = GetPressedKey();
        if (pressedKey != 0)
        {
            // 允许重复绑定：同一按键可分配给多个功能。若与其它热键重复，
            // 只显示黄色提示、照常保存（不做硬性拦截）。
            if (const char* conflict =
                    FindHotkeyConflict(bindingKeyTarget, pressedKey)) {
                bindingError =
                    std::string("提示：该按键也已用于 ") + conflict +
                    "（已允许重复绑定）";
                bindingErrorIsWarning = true;
            } else {
                bindingError.clear();
                bindingErrorIsWarning = false;
            }

            *bindingKeyTarget = pressedKey;
            isBindingKey = false;
            bindingKeyTarget = nullptr;
            bindingKeyName = nullptr;
            bindingWaitingForRelease = false;
            suppressHotkeysUntilRelease = true;
            savePersistentSettings();  // 热键一改就落盘，避免改完马上退出丢失
        }
    }

    inline void BeginCard(
        const char* id,
        const char* title,
        const char* subtitle,
        float height,
        float width = 0.0f)
    {
        const float dpiScale = sdl_renderer::getDpiScale();
        ImGui::PushStyleColor(
            ImGuiCol_ChildBg,
            ImVec4(0.070f, 0.090f, 0.125f, 1.0f));
        ImGui::PushStyleColor(
            ImGuiCol_Border,
            ImVec4(0.145f, 0.185f, 0.240f, 1.0f));
        ImGui::BeginChild(
            id,
            ImVec2(
                width > 0.0f ? width * dpiScale : 0.0f,
                height * dpiScale),
            true);
        ImGui::TextColored(
            ImVec4(0.330f, 0.800f, 1.000f, 1.0f),
            "%s",
            title);
        if (subtitle && subtitle[0] != '\0') {
            ImGui::TextColored(
                ImVec4(0.500f, 0.570f, 0.670f, 1.0f),
                "%s",
                subtitle);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    inline void EndCard()
    {
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    inline void StatusValue(
        const char* label,
        bool enabled,
        const char* enabledText = "开",
        const char* disabledText = "关")
    {
        ImGui::TextColored(
            ImVec4(0.610f, 0.665f, 0.750f, 1.0f),
            "%s",
            label);
        ImGui::SameLine();
        ImGui::TextColored(
            enabled
                ? ImVec4(0.250f, 0.900f, 0.600f, 1.0f)
                : ImVec4(0.930f, 0.420f, 0.430f, 1.0f),
            "%s",
            enabled ? enabledText : disabledText);
    }

    inline void RenderOverview()
    {
        const float dpiScale = sdl_renderer::getDpiScale();
        const float availableWidth =
            ImGui::GetContentRegionAvail().x;
        const float gap = 10.0f * dpiScale;
        const float cardWidth =
            std::max(180.0f * dpiScale,
                (availableWidth - gap) * 0.5f);

        ImGui::BeginGroup();
        BeginCard(
            "##QuickControls",
            "快捷开关",
            "你最可能在局中切换的功能。",
            240.0f,
            cardWidth / dpiScale);
        ImGui::Checkbox("玩家 ESP", &espEnabled);
        ImGui::Checkbox("自动瞄准", &aimbotEnabled);
        ImGui::Checkbox("自动扳机", &triggerbotEnabled);
        ImGui::Checkbox("本地地图雷达", &localRadarEnabled);
        ImGui::Checkbox("网页雷达", &webRadarEnabled);
        ImGui::Checkbox("炸弹计时器", &bombTimer);
        EndCard();
        ImGui::EndGroup();

        if (availableWidth >= 430.0f * dpiScale) {
            ImGui::SameLine(0.0f, gap);
        }
        ImGui::BeginGroup();
        BeginCard(
            "##SessionStatus",
            "会话状态",
            "实时渲染器与安全信息。",
            240.0f,
            cardWidth / dpiScale);
        StatusValue(
            "渲染器",
            sdl_renderer::isAcceleratedRenderer(),
            "硬件",
            "软件");
        StatusValue(
            "游戏焦点",
            sdl_renderer::isGameForeground(),
            "活动",
            "暂停");
        StatusValue(
            "单显示器",
            sdl_renderer::isGameOnSingleMonitor(),
            "正常",
            "请移游戏");
        StatusValue(
            "写内存",
            memory::WritesAllowed(),
            "已解锁",
            "已锁定");
        ImGui::Spacing();
        ImGui::Text(
            "%u x %u  |  目标 %d Hz",
            VIEWPORT_W,
            VIEWPORT_H,
            sdl_renderer::getTargetRefreshRate());
        ImGui::Text(
            "叠加层 %.0f FPS",
            ImGui::GetIO().Framerate);
        EndCard();
        ImGui::EndGroup();

        ImGui::Spacing();
        BeginCard(
            "##SafetySummary",
            "安全运行模式",
            "仅在 CS2 客户端位于前台时才注入输入。",
            130.0f);
        ImGui::TextWrapped(
            "CS2 失去焦点时叠加层会暂停实体读取。写内存默认开启；"
            "加 --no-memory-writes 可保持只读。");
        EndCard();
    }

    // ------------------------------------------------------------------
    // RenderHitboxPicker：可视化"打击部位"选择——画一个简化人体，
    // 点击身体部位选定瞄准的命中部位。骨骼编号 0..7 分别对应：
    //   0头 1脖子 2胸 3腹 4左臂 5右臂 6左腿 7右腿
    // ------------------------------------------------------------------
    inline void RenderHitboxPicker(int* bone)
    {
        const float dpi = sdl_renderer::getDpiScale();
        const float W = 120.0f * dpi;
        const float H = 200.0f * dpi;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        struct Part { const char* name; int bone; float x, y, w, h; };
        const Part parts[] = {
            { "头部", 0, W*0.5f - 15, 0,          30, 32 },    // 头（圆化）
            { "脖子", 1, W*0.5f - 8,  32,         16, 12 },     // 脖子
            { "胸部", 2, W*0.5f - 22, 44,         44, 44 },     // 胸
            { "腹部", 3, W*0.5f - 18, 88,         36, 30 },     // 腹
            { "左臂", 4, W*0.5f - 34, 44,         12, 66 },     // 左臂
            { "右臂", 5, W*0.5f + 22, 44,         12, 66 },     // 右臂
            { "左腿", 6, W*0.5f - 21, 122,        18, 74 },     // 左腿
            { "右腿", 7, W*0.5f + 3,  122,        18, 74 },     // 右腿
        };

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool clicked = ImGui::IsMouseClicked(0);
        const bool inBox =
            mouse.x >= origin.x && mouse.x < origin.x + W &&
            mouse.y >= origin.y && mouse.y < origin.y + H;

        for (const Part& p : parts) {
            const ImVec2 r0(origin.x + p.x, origin.y + p.y);
            const ImVec2 r1(origin.x + p.x + p.w, origin.y + p.y + p.h);
            const bool selected = (*bone == p.bone);
            const bool hover =
                inBox &&
                mouse.x >= r0.x && mouse.x < r1.x &&
                mouse.y >= r0.y && mouse.y < r1.y;
            const ImU32 fill = selected
                ? IM_COL32(70, 170, 255, 200)
                : hover ? IM_COL32(110, 120, 140, 170)
                        : IM_COL32(70, 80, 95, 150);
            // 头用大圆角做成圆状，其它用矩形
            const float round = (p.bone == 0) ? p.w * 0.5f : 3.0f * dpi;
            dl->AddRectFilled(r0, r1, fill, round);
            dl->AddRect(r0, r1, IM_COL32(170, 190, 210, 200), round, 0, 1.2f * dpi);
            if (hover && clicked) {
                *bone = p.bone;
            }
        }

        ImGui::Dummy(ImVec2(W, H));
        const char* names[] = { "头部","脖子","胸部","腹部","左臂","右臂","左腿","右腿" };
        ImGui::TextColored(
            ImVec4(0.6f, 0.7f, 0.9f, 1.0f),
            "命中部位：%s",
            (*bone >= 0 && *bone < 8) ? names[*bone] : "?");
    }

    // Render Aimbot tab content
    inline void RenderAimbotTab()
    {
        ImGui::Checkbox("启用自动瞄准", &aimbotEnabled);

        if (aimbotEnabled)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 自瞄模式：鼠标自瞄 / 魔法子弹（开火写角度，共用下方自瞄参数）
            const char* aimModes[] = { "鼠标自瞄（平滑注入）", "魔法子弹（开火写角度）" };
            ImGui::Combo("自瞄模式", &aimbotMode, aimModes, IM_ARRAYSIZE(aimModes));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("鼠标自瞄：按住瞄准键，平滑移动鼠标把准星带向目标。\n魔法子弹：按住瞄准键，视角按平滑度带向目标（看得见瞄准、松开还原），子弹走写入的击杀角。\n游戏内可随时按 %s 在两种模式间切换。", GetKeyName(aimbotModeToggleKey));
            if (aimbotMode == 1) {
                ImGui::Indent();
                ImGui::SliderFloat("后坐补偿", &magicBulletRecoil, 0.0f, 2.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("写入角度对后坐的补偿强度：写角 = 目标角 − 后坐×此值。若弹道整体偏低/偏高，试着微调它");
                ImGui::Checkbox("开枪自动弹道偏移", &magicTrajectoryEnabled);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("开火瞬间把子弹角度拉到目标（含后坐修正）——即使没按住瞄准键，子弹也会修正到命中部位。\n鼠标自瞄 / 魔法子弹两种模式都生效。");
                RenderHotkeyButton("弹道偏移", &magicTrajectoryToggleKey, "按一下切换 开枪自动弹道偏移 的开启/关闭（不用开菜单）");
                ImGui::Checkbox("自瞄完毕后准心回正", &magicBulletRecenter);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("勾选：松开瞄准键后准心回到你原来视角（默认）。\n取消：松开瞄准键后准心留在自瞄位置（锁定目标，需移动鼠标才离开）。");
                ImGui::TextColored(
                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "魔法子弹：按住瞄准键，视角按上方「瞄准平滑度」平滑带向目标（看得见瞄准），"
                    "松开即还原；共用上方的瞄准范围/骨骼/仅已标记/智能瞄准参数");
                ImGui::Unindent();
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }

            ImGui::Checkbox("智能瞄准（自动锁定）", &smartAimEnabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("按优先级自动锁定最佳目标（人物距离 / 视角距离 / 血量）\n可通过下方选项决定是否限制瞄准范围、是否只看已标记目标");

            if (smartAimEnabled) {
                ImGui::Indent();
                const char* priorityItems[] = { "人物距离最近", "视角距离最近", "血量最低" };
                ImGui::Combo("优先级", &smartAimPriority, priorityItems, IM_ARRAYSIZE(priorityItems));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("人物距离：离我（眼位→敌人脚底）最近的敌人\n视角距离：方向最贴近准星（视角夹角最小）的敌人\n血量：血量最低的敌人");
                RenderHotkeyButton("智能优先级", &smartAimPriorityCycleKey, "循环切换 人物距离/视角距离/血量 优先级");
                ImGui::Unindent();
            }

            // 瞄准范围(FOV)：普通模式始终限制；智能瞄准模式下可开关
            if (smartAimEnabled) {
                ImGui::Checkbox("启用瞄准范围限制(FOV)", &smartAimUseFov);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("勾选后只锁定瞄准范围(FOV)内的目标；取消则忽略范围，任何方向的敌人都参与排序");
                if (smartAimUseFov) {
                    ImGui::Indent();
                    ImGui::SliderFloat("瞄准范围(FOV)", &aimbotFOV, 1.0f, 180.0f, "%.1f°");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("瞄准搜索范围：1~180°。越大搜索越广，130° 以上几乎等于不限方向；越小越只锁定准星附近的敌人");
                    ImGui::Unindent();
                }
            } else {
                ImGui::SliderFloat("瞄准范围(FOV)", &aimbotFOV, 1.0f, 180.0f, "%.1f°");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("瞄准搜索范围：1~180°。越大搜索越广，130° 以上几乎等于不限方向；越小越只锁定准星附近的敌人");
            }

            ImGui::SliderFloat("瞄准平滑度", &aimbotSmoothing, 1.0f, 20.0f, "%.1f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("1.0 = 瞬时锁定，越大越平滑（慢）；两种自瞄模式都生效——鼠标自瞄平滑移动鼠标，魔法子弹模式按此把写入角度平滑带向目标");

            ImGui::Text("目标部位（点击人体选择）");
            RenderHitboxPicker(&aimbotBone);
            RenderHotkeyButton("命中部位", &boneCycleKey, "循环切换 头/脖/胸/腹/双臂/双腿（按一下换一个）");

            // 仅已标记目标：普通与智能瞄准都可选
            ImGui::Checkbox("仅已标记目标", &aimbotVisibleOnly);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("使用 CS2 的 spotted 标记，并非几何射线检测；关闭后智能瞄准也可锁定未标记的敌人");

            ImGui::Checkbox("显示瞄准范围圈", &aimbotShowFOV);
            if (aimbotShowFOV) {
                ImGui::SameLine();
                ImGui::ColorEdit4("##FOVColor", aimbotFOVColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "头部偏移");

            ImGui::Checkbox("启用（侧身补偿）", &headOffsetEnabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("敌人侧身时补偿头部位置");

            if (headOffsetEnabled) {
                ImGui::Indent();
                ImGui::SliderFloat("偏移量", &headOffsetAmount, 0.0f, 15.0f, "%.1f 单位");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("头部位置偏移量（建议 5-8）");

                ImGui::SliderFloat("最小角度", &headOffsetAngleMin, 0.0f, 90.0f, "%.0f°");
                ImGui::SliderFloat("最大角度", &headOffsetAngleMax, 90.0f, 180.0f, "%.0f°");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("偏移生效的角度范围（45-135 = 侧身）");

                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "0=正对你，90=侧身，180=背对");
                ImGui::Unindent();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "输入");
            ImGui::BeginDisabled(aimbotMode == 1);
            ImGui::SliderFloat("鼠标灵敏度", &mouseSensitivity, 0.1f, 10.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("请与你游戏内的鼠标灵敏度保持一致；（魔法子弹模式无需注入鼠标）");
            ImGui::EndDisabled();

            ImGui::Spacing();
            RenderHotkeyButton("瞄准键", &aimbotKey, "按住激活自动瞄准");
            RenderHotkeyButton("自瞄模式", &aimbotModeToggleKey, "按一下切换 鼠标自瞄 / 魔法子弹 模式（不用开菜单）");
            if (aimbotMode == 0) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "按住 %s 瞄准", GetKeyName(aimbotKey));
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "魔法子弹：按住 %s 瞄准（视角带向目标），松开还原", GetKeyName(aimbotKey));
            }
        }
    }

    // Render Weapon & Movement tab content（压枪 / 魔法子弹 / 自动旋转 / 连跳）
    inline void RenderWeaponTab()
    {
        // ------------------------------------------------------------------
        // 自动压枪（RCS）：读后座视角偏移 → 反向注入鼠标
        // ------------------------------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "自动压枪（RCS）");

        ImGui::Checkbox("启用自动压枪", &rcsEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "开火时每帧读当前后座视角偏移并反向注入鼠标压回。\n"
                "偏移量直接来自游戏实时数据（每发不同、自动适配各种枪与灵敏度），"
                "无需手动调强度。");

        if (rcsEnabled)
        {
            ImGui::Indent();
            ImGui::Checkbox("反向", &rcsInvert);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("压枪方向反了（越压越飘）时打开它");
            ImGui::SliderFloat("注入比例", &rcsInjectRatio, 0.2f, 1.0f, "%.0f%%");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "每次把待补偿量注入多少。100% = 下一发前全额回正（弹着点最集中）；"
                    "调低 = 分多步平滑送出，但可能来不及在下一发前补完（弹着点变大）。");
            ImGui::SliderInt("单次移动上限", &rcsMaxMovePx, 10, 80, "%d px");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("单次注入鼠标移动的像素上限，防止一次移动过大");
            ImGui::SliderFloat("补偿强度", &rcsStrength, 0.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "补偿量倍率。若子弹整体朝后坐方向偏（补偿不足）调大；"
                    "反向偏（过压）调小。1.0 为默认。");
            ImGui::Unindent();
        }
        RenderHotkeyButton("压枪开关", &rcsToggleKey,
            "按一下切换自动压枪的开启/关闭");
        // ------------------------------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "弹道录制");

        if (recoil_patterns::isRecording()) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                "录音中… 已录 %d 发（再按 %s 停止）",
                recoil_patterns::recordingBulletCount(),
                GetKeyName(recoilRecordKey));
        } else {
            ImGui::TextColored(
                ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "按 %s 开始录制当前枪弹道，再按一次停止并保存",
                GetKeyName(recoilRecordKey));
        }
        RenderHotkeyButton("弹道录制", &recoilRecordKey,
            "第一次按开始录制当前枪的后坐弹道，第二次按停止并保存");

        const std::vector<recoil_patterns::Pattern> patterns =
            recoil_patterns::snapshotAll();
        static int selectedPatternIndex = -1;
        if (!patterns.empty()) {
            if (selectedPatternIndex >= static_cast<int>(patterns.size())) {
                selectedPatternIndex = -1;
            }
            std::string previewStr = "选择已录制的枪";
            if (selectedPatternIndex >= 0) {
                const auto& p = patterns[selectedPatternIndex];
                previewStr = p.weaponName + " (" +
                    std::to_string(p.bullets.size()) + "发)";
            }
            if (ImGui::BeginCombo("已录制", previewStr.c_str())) {
                for (size_t i = 0; i < patterns.size(); ++i) {
                    const bool selected = static_cast<int>(i) == selectedPatternIndex;
                    const std::string label = patterns[i].weaponName + " (" +
                        std::to_string(patterns[i].bullets.size()) + "发)";
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        selectedPatternIndex = static_cast<int>(i);
                    }
                }
                ImGui::EndCombo();
            }
            if (selectedPatternIndex >= 0) {
                const auto& p = patterns[selectedPatternIndex];
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                const float size = 160.0f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRect(
                    origin,
                    ImVec2(origin.x + size, origin.y + size),
                    IM_COL32(90, 110, 130, 120));
                dl->AddLine(
                    ImVec2(origin.x + size / 2, origin.y),
                    ImVec2(origin.x + size / 2, origin.y + size),
                    IM_COL32(90, 110, 130, 80));
                dl->AddLine(
                    ImVec2(origin.x, origin.y + size / 2),
                    ImVec2(origin.x + size, origin.y + size / 2),
                    IM_COL32(90, 110, 130, 80));
                // 范围取各发里最大绝对值，把整条弹道画进方块
                float maxAbs = 1.0f;
                for (const vec2& b : p.bullets) {
                    maxAbs = std::max(maxAbs,
                        std::max(std::abs(b.x), std::abs(b.y)));
                }
                const float half = size * 0.45f;
                for (size_t i = 0; i < p.bullets.size(); ++i) {
                    const float px = origin.x + size / 2 +
                        p.bullets[i].y / maxAbs * half;    // yaw → 横向
                    const float py = origin.y + size / 2 +
                        p.bullets[i].x / maxAbs * half;    // pitch → 纵向（上抬=负pitch，画在上方）
                    if (i > 0) {
                        const float pxx = origin.x + size / 2 +
                            p.bullets[i - 1].y / maxAbs * half;
                        const float pyy = origin.y + size / 2 +
                            p.bullets[i - 1].x / maxAbs * half;
                        dl->AddLine(
                            ImVec2(pxx, pyy), ImVec2(px, py),
                            IM_COL32(120, 200, 255, 120));
                    }
                    dl->AddCircleFilled(
                        ImVec2(px, py), 3.0f,
                        i == 0 ? IM_COL32(255, 220, 60, 255)
                               : IM_COL32(120, 200, 255, 255));
                }
                ImGui::Dummy(ImVec2(size, size));
                ImGui::TextColored(
                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "已录 %d 发：前 %d 发用记录弹道回放，之后回退实时方案",
                    static_cast<int>(p.bullets.size()),
                    static_cast<int>(p.bullets.size()));
                ImGui::SameLine();
                if (ImGui::Button("清除该枪")) {
                    recoil_patterns::removePattern(p.weaponIndex);
                    selectedPatternIndex = -1;
                }
            }
        } else {
            ImGui::TextColored(
                ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                "还没有已录制的弹道模式");
        }
        if (ImGui::Button("清除全部记录")) {
            recoil_patterns::clearAll();
            selectedPatternIndex = -1;
        }

        // ------------------------------------------------------------------
        // 自动旋转（spinbot）：旋转模型朝向，独立于自瞄/魔法子弹
        // ------------------------------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.7f, 1.0f), "自动旋转");

        ImGui::Checkbox("启用自动旋转", &autoRotationEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "旋转角色模型朝向(yaw)：他人看到的角色会持续转动，"
                "你自己的视角/瞄准完全不受影响。\n"
                "写入的是模型旋转字段，不碰视角；按 %s 可快速开关。",
                GetKeyName(autoRotationToggleKey));
        ImGui::SliderFloat("旋转速度", &autoRotationSpeed, 0.0f, 3600.0f, "%.0f°/s");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("模型旋转速度：0=停，范围 0~3600°/秒（默认 360°，1 秒转一圈）");
        RenderHotkeyButton("旋转开关", &autoRotationToggleKey,
            "按一下切换自动旋转的开启/关闭（不用开菜单）");

        // ------------------------------------------------------------------
        // 连跳（bhop）：落地瞬间自动起跳
        // ------------------------------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "连跳（Bunny Hop）");

        ImGui::Checkbox("启用连跳", &bhopEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("按住空格：每次落地瞬间自动再跳，实现连跳");
        if (bhopEnabled)
        {
            ImGui::Indent();
            ImGui::SliderFloat("人类化", &bhopHumanize, 0.0f, 100.0f, "%.0f%%");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("随机跳过某次起跳的概率，越高越像真人（0 = 每次都跳）");
            ImGui::Unindent();
        }

        // ------------------------------------------------------------------
        // 自动急停（counter-strafe）：松开移动键自动快速点按相反键
        // ------------------------------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.8f, 1.0f), "自动急停");

        ImGui::Checkbox("启用自动急停", &counterStrafEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "松开 W/A/S/D 时，自动快速点按相反方向键一次，让角色急停（更快的停止）。\n"
                "例如松开 D 会点一下 A。");
        RenderHotkeyButton("急停开关", &counterStrafToggleKey,
            "按一下切换自动急停的开启/关闭");

        // ------------------------------------------------------------------
        // 角色瞬移（Teleport）：写本地位置字段，产生"瞬移"（本地/离线可见；
        // 在线对局位置由服务器权威判定，会被拉回）
        // ------------------------------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(
            ImVec4(0.9f, 0.7f, 0.7f, 1.0f),
            "角色瞬移");

        ImGui::Checkbox("启用瞬移", &teleportEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "开启后按住 W/A/S/D 移动键会以远快于游戏的速度连续位移"
                "（空格=上升）。\n也可以点下面的按钮做单次跳步。"
                "按 %s 可快速开关。\n"
                "⚠️ 位置由服务器权威判定：在线会被拉回，仅本地/离线研究有效。",
                GetKeyName(teleportToggleKey));

        ImGui::SliderFloat(
            "步长",
            &teleportStep,
            0.0f,
            500.0f,
            "%.0f 单位");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "按钮=单次跳步距离；按住移动键时每秒移动 20×步长"
                "（默认 100 → 2000 单位/秒，约 8 倍奔跑速度）");

        const float teleportButtonWidth = 72.0f * sdl_renderer::getDpiScale();
        const auto teleportButton = [teleportButtonWidth](const char* label, int direction) {
            if (ImGui::Button(label, ImVec2(teleportButtonWidth, 0.0f))) {
                teleportRequest.store(direction);
            }
            ImGui::SameLine();
        };
        teleportButton("前", 1);
        teleportButton("后", 2);
        teleportButton("左", 3);
        teleportButton("右", 4);
        teleportButton("上", 5);
        if (ImGui::Button("下", ImVec2(teleportButtonWidth, 0.0f))) {
            teleportRequest.store(6);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("点一下按当前朝向移动一个步长（前/后/左/右按视角，上/下垂直）；可连续点");
        RenderHotkeyButton("瞬移开关", &teleportToggleKey,
            "按一下切换角色瞬移的开启/关闭（不用开菜单）");
    }

    // Render Triggerbot tab content
    inline void RenderTriggerbotTab()
    {
        ImGui::Checkbox("启用自动扳机", &triggerbotEnabled);

        if (triggerbotEnabled)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::SliderInt("延迟(ms)", &triggerbotDelay, 0, 500, "%d ms");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("开枪前的延迟（毫秒）");

            ImGui::Spacing();
            RenderHotkeyButton("扳机键", &triggerbotKey, "按住激活自动扳机");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "按住 %s 激活", GetKeyName(triggerbotKey));
            ImGui::TextColored(
                ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "只有准星指向存活敌人时才开火。");
        }
    }

    // Render ESP tab content
    inline void RenderESPTab()
    {
        ImGui::Checkbox("启用 ESP", &espEnabled);

        if (espEnabled)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 边框
            ImGui::Checkbox("边框", &espBox);
            if (espBox) {
                ImGui::SameLine();
                ImGui::ColorEdit4("##BoxColor", espBoxColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
            }

            // 血量条
            ImGui::Checkbox("血量条", &espHealth);

            // 武器显示
            ImGui::Checkbox("武器", &espWeapon);
            if (espWeapon) {
                ImGui::SameLine();
                ImGui::ColorEdit4("##WeaponColor", espWeaponColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
            }

            // 朝向指示
            ImGui::Checkbox("朝向指示（用边框色）", &espViewAngle);
            if (espViewAngle) {
                ImGui::Indent();
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "正对你：红");
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.0f, 1.0f), "部分：橙");
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "侧身：黄");
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "背对：绿");
                ImGui::Checkbox("显示角度数值", &espViewAngleText);
                ImGui::Unindent();
            }

            // CS2 spotted 状态标记（有意不描述为射线检测，是保守的游戏状态信号）
            ImGui::Checkbox("已标记检测（三角）", &espWallCheck);
            if (espWallCheck) {
                ImGui::Indent();
                ImGui::Text("已标记颜色：");
                ImGui::SameLine();
                ImGui::ColorEdit4("##BoxColor2", espBoxColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
                ImGui::Text("未标记 / 未知：");
                ImGui::SameLine();
                ImGui::ColorEdit4("##WallColor", espWallColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
                ImGui::TextColored(
                    ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "使用 CS2 的 spotted 状态；绝不假设远处目标可见。");
                ImGui::Unindent();
            }

            // 距离
            ImGui::Checkbox("距离", &espDistance);
            if (espDistance) {
                ImGui::SameLine();
                ImGui::ColorEdit4("##DistanceColor", espDistanceColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
            }

            // 闪光弹眼部指示
            ImGui::Checkbox("闪光弹眼部指示", &espFlashIndicator);
            if (espFlashIndicator) {
                ImGui::Indent();
                ImGui::Text("正常眼：");
                ImGui::SameLine();
                ImGui::ColorEdit4("##FlashNormalColor", espFlashNormalColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
                ImGui::Text("被闪眼：");
                ImGui::SameLine();
                ImGui::ColorEdit4("##FlashColor", espFlashColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
                ImGui::Unindent();
            }

            // 瞄准线
            ImGui::Checkbox("瞄准线", &espSnaplines);
            if (espSnaplines) {
                ImGui::SameLine();
                ImGui::ColorEdit4("##SnaplinesColor", espSnaplinesColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
                ImGui::Indent();
                const char* origins[] = { "底部", "中心", "顶部" };
                ImGui::Combo("起点", &snaplinesOrigin, origins, IM_ARRAYSIZE(origins));
                ImGui::Unindent();
            }

            // 骨架
            ImGui::Checkbox("骨架", &espSkeleton);
            if (espSkeleton) {
                ImGui::SameLine();
                ImGui::ColorEdit4("##SkeletonColor", espSkeletonColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
            }
        }
    }

    // Local and browser modes consume the same complete GameSnapshot and map
    // catalogue. Neither mode rotates or distance-crops the north-up map.
    inline void RenderRadarTab()
    {
        ImGui::TextColored(
            ImVec4(0.330f, 0.800f, 1.000f, 1.0f),
            "本地固定地图叠加层");
        ImGui::TextWrapped(
            "在游戏叠加层内绘制完整的北向上地图。它使用与网页雷达相同的图片和校准，"
            "只有玩家方向标记会旋转。");
        ImGui::Spacing();

        ImGui::Checkbox("启用本地地图叠加层", &localRadarEnabled);
        if (localRadarEnabled) {
            ImGui::Checkbox("显示玩家名", &localRadarShowNames);
            ImGui::SliderFloat(
                "水平位置",
                &localRadarAnchorX,
                0.0f,
                1.0f,
                "%.2f");
            ImGui::SliderFloat(
                "垂直位置",
                &localRadarAnchorY,
                0.0f,
                1.0f,
                "%.2f");
            ImGui::SliderFloat(
                "地图大小",
                &localRadarSize,
                0.18f,
                0.65f,
                "%.2f");
            ImGui::SliderFloat(
                "玩家标记大小",
                &localRadarMarkerSize,
                6.0f,
                24.0f,
                "%.0f px");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(
            ImVec4(0.330f, 0.800f, 1.000f, 1.0f),
            "内嵌浏览器雷达");
        ImGui::TextWrapped(
            "通过内嵌的 CivetWeb 服务把同一张固定地图提供给本地浏览器或可信局域网观看者。");
        ImGui::Spacing();

        ImGui::Checkbox("启用网页雷达", &webRadarEnabled);
        ImGui::InputInt("HTTP 端口", &webRadarPort, 1, 100);
        webRadarPort = std::clamp(webRadarPort, 1024, 65535);
        ImGui::Checkbox("允许本局域网观看者", &webRadarLanAccess);

        ImGui::Checkbox(
            "CS2 失去焦点时暂停浏览器/中继采样",
            &webRadarPauseWhenUnfocused);
        ImGui::TextWrapped(
            "本地叠加层在 CS2 失焦时总是暂停；只有显式共享的观看者可以选择后台采样。");
        ImGui::Checkbox(
            "共享玩家名",
            &webRadarIncludePlayerNames);
        const char* teamPolicies[] = {
            "全部队伍",
            "仅本队",
            "仅对手"
        };
        ImGui::Combo(
            "共享队伍",
            &webRadarTeamViewPolicy,
            teamPolicies,
            IM_ARRAYSIZE(teamPolicies));
        ImGui::Checkbox(
            "共享 Steam ID（个人资料链接）",
            &webRadarIncludeSteamIds);

        if (webRadarLanAccess) {
            ImGui::Spacing();
            ImGui::TextColored(
                ImVec4(0.930f, 0.650f, 0.260f, 1.0f),
                "局域网模式");
            ImGui::TextWrapped(
                "任何拿到带令牌链接的人都能查看数据流。只应在可信的私有网络上使用；"
                "不要把这个端口暴露到公网。");
        }

        const WebRadarUiStatus status = getWebRadarStatus();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("服务：%s", status.running ? "运行中" : "已停止");
        ImGui::Text("监听：%s:%d", status.bindAddress.c_str(), webRadarPort);
        ImGui::Text("观看者：%zu", status.viewers);
        ImGui::Text(
            "帧：发布 %llu | 发送 %llu | 替换 %llu",
            static_cast<unsigned long long>(status.publishedFrames),
            static_cast<unsigned long long>(status.sentFrames),
            static_cast<unsigned long long>(status.replacedFrames));
        ImGui::Text(
            "流量：%.1f MB | 最大发送延迟 %.1f ms",
            static_cast<double>(status.publishedBytes) /
                (1024.0 * 1024.0),
            status.maximumSendLatencyMilliseconds);

        if (!status.error.empty()) {
            ImGui::TextColored(
                ImVec4(0.930f, 0.420f, 0.430f, 1.0f),
                "错误：%s",
                status.error.c_str());
        }

        const bool canOpen = status.running && !status.viewerUrl.empty();
        ImGui::BeginDisabled(!canOpen);
        if (ImGui::Button("打开雷达")) {
            ShellExecuteA(
                nullptr,
                "open",
                status.viewerUrl.c_str(),
                nullptr,
                nullptr,
                SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::Button("复制观看链接")) {
            ImGui::SetClipboardText(status.viewerUrl.c_str());
        }
        ImGui::EndDisabled();

        if (canOpen) {
            ImGui::TextWrapped("%s", status.viewerUrl.c_str());
            if (webRadarLanAccess) {
                ImGui::TextWrapped(
                    "其他设备访问时，把此链接中的 127.0.0.1 换成这台电脑的局域网 IPv4 地址。");
            }
        }

        ImGui::Spacing();
        ImGui::Checkbox(
            "录制脱敏雷达快照",
            &radarRecordingEnabled);
        const web_radar::SnapshotRecorderStatus recording =
            getRecorderStatus();
        if (recording.recording) {
            ImGui::Text(
                "录制中：%llu 帧（%.1f MB），替换 %llu",
                static_cast<unsigned long long>(recording.framesWritten),
                static_cast<double>(recording.bytesWritten) /
                    (1024.0 * 1024.0),
                static_cast<unsigned long long>(recording.replacedFrames));
        }
        if (!recording.path.empty()) {
            ImGui::TextWrapped("文件：%s", recording.path.c_str());
        }
        if (!recording.lastError.empty()) {
            ImGui::TextColored(
                ImVec4(0.930f, 0.420f, 0.430f, 1.0f),
                "%s",
                recording.lastError.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(
            ImVec4(0.330f, 0.800f, 1.000f, 1.0f),
            "公网中继（出站 WSS）");
        ImGui::TextWrapped(
            "通过经过认证、TLS 保护的出站连接发布快照。无需入站端口或局域网模式。");
        ImGui::Checkbox("启用公网中继", &publicRelayEnabled);

        ImGui::BeginDisabled(publicRelayEnabled);
        ImGui::InputTextWithHint(
            "中继 WSS 地址",
            "wss://radar.example.com/api/v1/publish",
            publicRelayUrl.data(),
            publicRelayUrl.size(),
            ImGuiInputTextFlags_CharsNoBlank |
                ImGuiInputTextFlags_AutoSelectAll);
        ImGui::InputTextWithHint(
            "中继房间",
            "match-room",
            publicRelayRoom.data(),
            publicRelayRoom.size(),
            ImGuiInputTextFlags_CharsNoBlank |
                ImGuiInputTextFlags_AutoSelectAll);
        ImGui::InputTextWithHint(
            "生产者令牌",
            "粘贴生产者专用令牌",
            publicRelayToken.data(),
            publicRelayToken.size(),
            ImGuiInputTextFlags_Password |
                ImGuiInputTextFlags_CharsNoBlank |
                ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::Button("清除中继凭据")) {
            std::fill(publicRelayUrl.begin(), publicRelayUrl.end(), '\0');
            std::fill(publicRelayRoom.begin(), publicRelayRoom.end(), '\0');
            SecureZeroMemory(
                publicRelayToken.data(),
                publicRelayToken.size());
        }
        ImGui::EndDisabled();

        ImGui::Checkbox(
            "通过公网中继共享玩家名",
            &publicRelayIncludePlayerNames);
        ImGui::Combo(
            "中继队伍",
            &publicRelayTeamViewPolicy,
            teamPolicies,
            IM_ARRAYSIZE(teamPolicies));
        ImGui::Checkbox(
            "通过公网中继共享 Steam ID",
            &publicRelayIncludeSteamIds);
        ImGui::TextWrapped(
            "生产者令牌只保留在内存中，绝不在状态或日志中显示。请使用生产者令牌，不要用观看者令牌。");

        const PublicRelayUiStatus relayStatus = getPublicRelayStatus();
        const char* relayState = "已禁用";
        switch (relayStatus.state) {
        case web_radar::PublicRelayState::connecting:
            relayState = "连接中";
            break;
        case web_radar::PublicRelayState::connected:
            relayState = "已连接";
            break;
        case web_radar::PublicRelayState::backoff:
            relayState = "退避重试";
            break;
        case web_radar::PublicRelayState::retiring:
            relayState = "停止中";
            break;
        case web_radar::PublicRelayState::failed:
            relayState = "失败";
            break;
        case web_radar::PublicRelayState::disabled:
            break;
        }
        ImGui::Text("中继：%s", relayState);
        ImGui::Text(
            "发送帧：%llu | 替换：%llu | 丢弃：%llu | 重连：%llu",
            static_cast<unsigned long long>(relayStatus.framesSent),
            static_cast<unsigned long long>(relayStatus.replacedFrames),
            static_cast<unsigned long long>(relayStatus.droppedFrames),
            static_cast<unsigned long long>(relayStatus.reconnects));
        if (!relayStatus.error.empty()) {
            ImGui::TextColored(
                ImVec4(0.930f, 0.420f, 0.430f, 1.0f),
                "中继错误：%s",
                relayStatus.error.c_str());
        }
    }

    // Render Hotkeys tab content
    inline void RenderHotkeysTab()
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "全局按键");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        RenderHotkeyButton("菜单开关", &menuToggleKey, "显示/隐藏菜单的按键");
        RenderHotkeyButton("退出程序", &exitKey, "退出程序的按键");
        ImGui::TextColored(
            ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "其余功能的热键已分散到各自的功能区（如瞄准、武器、瞬移页）。");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "点击按钮后按下任意键即可绑定");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "按 ESC 取消绑定");
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "允许同一按键绑定到多个功能");
        if (!bindingError.empty()) {
            ImGui::TextColored(
                bindingErrorIsWarning
                    ? ImVec4(1.0f, 0.85f, 0.35f, 1.0f)
                    : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                "%s",
                bindingError.c_str());
        }
    }

    // Render Settings tab content
    inline void RenderMiscTab()
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "杂项功能");
        ImGui::Separator();
        ImGui::Spacing();

        if (!memory::WritesAllowed()) {
            ImGui::BeginDisabled();
        }
        ImGui::Checkbox("防闪光", &antiFlash);
        if (!memory::WritesAllowed()) {
            antiFlash = false;
            ImGui::EndDisabled();
            ImGui::TextColored(
                ImVec4(1.0f, 0.65f, 0.1f, 1.0f),
                "写内存已被锁定（--no-memory-writes）。重启且不带该参数即可启用。");
        }
        ImGui::Checkbox("炸弹计时器", &bombTimer);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "世界 ESP");
        ImGui::Spacing();
        ImGui::Checkbox("投掷物 ESP", &grenadeESP);
        ImGui::Checkbox("掉落武器 ESP", &droppedWeaponESP);
    }

    inline void RenderSettingsTab()
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "性能");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(
            ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
            "叠加层目标：%d FPS（%s）",
            sdl_renderer::getTargetRefreshRate(),
            sdl_renderer::isVsyncEnabled()
                ? "垂直同步"
                : "按节奏回退");
        ImGui::Text(
            "渲染器：%s",
            sdl_renderer::isAcceleratedRenderer()
                ? "硬件加速"
                : "软件回退（限制为 60 FPS）");
        if (!sdl_renderer::isGameOnSingleMonitor()) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.5f, 0.1f, 1.0f),
                "请把 CS2 完整放到单个显示器上，混合 DPI 映射才可靠。");
        }
        if (!sdl_renderer::isDpiAwarenessReliable()) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.25f, 0.25f, 1.0f),
                "无法使用按显示器 DPI 感知。");
        }
        const char* viewportModes[] = {
            "自动检测黑边",
            "全客户端（拉伸）",
            "强制 4:3 黑边",
            "强制 16:10 黑边"
        };
        ImGui::Combo(
            "游戏视口",
            &viewportMode,
            viewportModes,
            IM_ARRAYSIZE(viewportModes));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "推荐使用自动。只有当受保护或过暗的画面导致黑边检测失败时，才用强制模式。");
        }
        RenderHotkeyButton("视口模式", &viewportCycleKey, "循环切换 自动/全客户端/4:3/16:10 视口模式");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "系统信息");
        ImGui::Spacing();

        ImGui::Text("分辨率：%dx%d", WIDTH, HEIGHT);
        ImGui::Text(
            "游戏视口：%dx%d 位于 (%d, %d)",
            VIEWPORT_W,
            VIEWPORT_H,
            VIEWPORT_X,
            VIEWPORT_Y);
        ImGui::Text("FPS：%.1f", ImGui::GetIO().Framerate);
        const float frameRate = ImGui::GetIO().Framerate;
        ImGui::Text(
            "帧时间：%.3f ms",
            frameRate > 0.0f ? 1000.0f / frameRate : 0.0f);

        const auto samplingMetrics =
            performance_metrics::samplingDuration.snapshot();
        const auto serializationMetrics =
            performance_metrics::serializationDuration.snapshot();
        const auto renderMetrics =
            performance_metrics::renderCpuDuration.snapshot();
        const memory::ReadMetrics readMetrics = memory::GetReadMetrics();
        ImGui::Text(
            "采样：%d Hz | 雷达：%d Hz | 平均 %.2f ms | P95 %.2f | P99 %.2f",
            performance_metrics::samplingRateHz.load(
                std::memory_order_relaxed),
            performance_metrics::radarRateHz.load(
                std::memory_order_relaxed),
            samplingMetrics.averageMilliseconds,
            samplingMetrics.p95Milliseconds,
            samplingMetrics.p99Milliseconds);
        ImGui::Text(
            "渲染 CPU：平均 %.2f ms | P95 %.2f | P99 %.2f",
            renderMetrics.averageMilliseconds,
            renderMetrics.p95Milliseconds,
            renderMetrics.p99Milliseconds);
        ImGui::Text(
            "雷达 JSON：平均 %.2f ms | P95 %.2f | 最大 %.2f",
            serializationMetrics.averageMilliseconds,
            serializationMetrics.p95Milliseconds,
            serializationMetrics.maximumMilliseconds);
        ImGui::Text(
            "RPM：%llu 次调用 | %.1f MB | %llu 次失败",
            static_cast<unsigned long long>(readMetrics.calls),
            static_cast<double>(readMetrics.bytesRequested) /
                (1024.0 * 1024.0),
            static_cast<unsigned long long>(readMetrics.failures));
        ImGui::Text(
            "错过截止：采样 %llu | 渲染 %llu",
            static_cast<unsigned long long>(
                performance_metrics::missedSamplingDeadlines.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                performance_metrics::missedRenderDeadlines.load(
                    std::memory_order_relaxed)));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(
            startupReport.ready()
                ? ImVec4(0.250f, 0.900f, 0.600f, 1.0f)
                : ImVec4(0.930f, 0.420f, 0.430f, 1.0f),
            "启动自检：%s",
            startupReport.ready() ? "就绪" : "请注意");
        ImGui::Text(
            "管理员 %s | SDL %s | 网页资源 %s | 地图 %s",
            startupReport.administrator ? "正常" : "失败",
            startupReport.sdlRuntimePresent ? "正常" : "失败",
            startupReport.webRadarBundlePresent ? "正常" : "失败",
            startupReport.mapMetadataPresent ? "正常" : "失败");
        if (!startupReport.installationError.empty()) {
            ImGui::TextWrapped(
                "%s",
                startupReport.installationError.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "CS2 外部 ESP v2.0");
        ImGui::Text("SDL2 + ImGui 覆盖层");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "github.com/tiansongyu/cs2_cheat");
    }

    inline void RenderPageHeader(
        const char* title,
        const char* description)
    {
        ImGui::TextColored(
            ImVec4(0.930f, 0.960f, 1.000f, 1.0f),
            "%s",
            title);
        ImGui::TextColored(
            ImVec4(0.500f, 0.570f, 0.670f, 1.0f),
            "%s",
            description);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    inline void RenderCombatPage()
    {
        RenderPageHeader(
            "战斗辅助",
            "目标选择与输入自动化。所有输入都受前台焦点控制。");
        BeginCard(
            "##AimbotCard",
            "自动瞄准",
            "基于真实骨骼目标，带稳定的目标保持逻辑。",
            590.0f);
        RenderAimbotTab();
        EndCard();
        ImGui::Spacing();
        BeginCard(
            "##TriggerCard",
            "自动扳机",
            "使用准星下的真实实体，不靠角度猜测。",
            190.0f);
        RenderTriggerbotTab();
        EndCard();
        ImGui::Spacing();
        BeginCard(
            "##WeaponCard",
            "武器与移动",
            "自动压枪、魔法子弹、自动旋转与连跳。",
            340.0f);
        RenderWeaponTab();
        EndCard();
    }

    inline void RenderPlayerVisualsPage()
    {
        RenderPageHeader(
            "玩家视觉",
            "配置围绕已校验的存活敌人绘制的信息。");
        BeginCard(
            "##PlayerEspCard",
            "玩家 ESP",
            "边框、血量、骨骼、装备与威胁方向。",
            650.0f);
        RenderESPTab();
        EndCard();
    }

    inline void RenderWorldPage()
    {
        RenderPageHeader(
            "世界与对局",
            "共享网页雷达、炸弹状态与移动中的世界实体。");
        BeginCard(
            "##RadarCard",
            "固定地图雷达",
            "本地叠加层、CivetWeb 与中继共用同一套北向上的地图模型。",
            870.0f);
        RenderRadarTab();
        EndCard();
        ImGui::Spacing();
        BeginCard(
            "##WorldUtilityCard",
            "对局工具",
            "炸弹计时、投掷物、掉落装备与防闪光。",
            255.0f);
        RenderMiscTab();
        EndCard();
    }

    inline void RenderSystemPage()
    {
        RenderPageHeader(
            "系统",
            "显示映射、性能诊断与按键绑定。");
        BeginCard(
            "##DisplayCard",
            "显示与渲染",
            "感知显示器的视口映射与实时诊断。",
            460.0f);
        RenderSettingsTab();
        EndCard();
        ImGui::Spacing();
        BeginCard(
            "##HotkeyCard",
            "按键绑定",
            "允许重复绑定；绑定时输入暂停。",
            330.0f);
        RenderHotkeysTab();
        EndCard();
    }

    inline bool NavigationButton(
        const char* label,
        int page,
        const ImVec2& size)
    {
        const bool selected = currentTab == page;
        if (selected) {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4(0.075f, 0.330f, 0.470f, 1.0f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered,
                ImVec4(0.085f, 0.390f, 0.540f, 1.0f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive,
                ImVec4(0.100f, 0.440f, 0.600f, 1.0f));
        } else {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4(0.055f, 0.072f, 0.100f, 1.0f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered,
                ImVec4(0.095f, 0.130f, 0.175f, 1.0f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive,
                ImVec4(0.110f, 0.155f, 0.205f, 1.0f));
        }

        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(3);
        if (pressed) {
            currentTab = page;
        }
        return pressed;
    }

// ----------------------------------------------------------------------------
// menu::render：整个设置菜单的主渲染函数（每帧由渲染线程调用）
// ----------------------------------------------------------------------------
// 布局：左侧固定宽度的导航栏（5 个页面按钮 + 底部状态）+
// 右侧内容区（按 currentTab 切换页面）。窗口可缩放/拖动，
// 位置超出屏幕时会自动夹回屏幕内；有弹出菜单时把整屏设为可交互
// （防止第一次点击穿透到游戏）。
// 每帧最后调用 publishRuntimeConfig()，把最新的设置同步给采样线程。
    inline void render()
    {
        if (!sdl_renderer::menuVisible) return;

        // Update key binding
        UpdateKeyBinding();

        const float dpiScale = sdl_renderer::getDpiScale();
        const float margin = std::max(8.0f, 16.0f * dpiScale);
        const float availableWidth =
            std::max(1.0f, static_cast<float>(WIDTH) - margin * 2.0f);
        const float availableHeight =
            std::max(1.0f, static_cast<float>(HEIGHT) - margin * 2.0f);
        const float defaultWidth =
            std::min(920.0f * dpiScale, availableWidth);
        const float defaultHeight =
            std::min(720.0f * dpiScale, availableHeight);
        const float minimumWidth =
            std::min(680.0f * dpiScale, availableWidth);
        const float minimumHeight =
            std::min(500.0f * dpiScale, availableHeight);

        ImGui::SetNextWindowSizeConstraints(
            ImVec2(minimumWidth, minimumHeight),
            ImVec2(availableWidth, availableHeight));
        ImGui::SetNextWindowSize(
            ImVec2(defaultWidth, defaultHeight),
            ImGuiCond_FirstUseEver
        );
        ImGui::SetNextWindowPos(
            ImVec2(
                (static_cast<float>(WIDTH) - defaultWidth) / 2.0f,
                (static_cast<float>(HEIGHT) - defaultHeight) / 2.0f),
            ImGuiCond_FirstUseEver
        );

        ImGui::Begin(
            "Aegis // CS2 覆盖层",
            nullptr,
            ImGuiWindowFlags_NoCollapse);

        // Saved ImGui positions may belong to another monitor/resolution.
        // Clamp without resetting a valid user-selected position or size.
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 clampedPos(
            std::clamp(
                windowPos.x,
                margin,
                std::max(margin, static_cast<float>(WIDTH) - windowSize.x - margin)),
            std::clamp(
                windowPos.y,
                margin,
                std::max(margin, static_cast<float>(HEIGHT) - windowSize.y - margin)));
        if (clampedPos.x != windowPos.x || clampedPos.y != windowPos.y) {
            ImGui::SetWindowPos(clampedPos);
        }
        {
            const ImVec2 interactivePosition = ImGui::GetWindowPos();
            const ImVec2 interactiveSize = ImGui::GetWindowSize();
            sdl_renderer::setInteractiveRect(
                interactivePosition.x,
                interactivePosition.y,
                interactiveSize.x,
                interactiveSize.y);
        }

        const float sidebarWidth = 184.0f * dpiScale;
        const float navigationHeight = 42.0f * dpiScale;

        ImGui::PushStyleColor(
            ImGuiCol_ChildBg,
            ImVec4(0.038f, 0.052f, 0.075f, 1.0f));
        ImGui::BeginChild(
            "##Navigation",
            ImVec2(sidebarWidth, 0.0f),
            true);
        ImGui::TextColored(
            ImVec4(0.330f, 0.800f, 1.000f, 1.0f),
            "AEGIS");
        ImGui::TextColored(
            ImVec4(0.500f, 0.570f, 0.670f, 1.0f),
            "CS2 覆盖层");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const ImVec2 navigationSize(
            ImGui::GetContentRegionAvail().x,
            navigationHeight);
        NavigationButton("总览", 0, navigationSize);
        NavigationButton("战斗", 1, navigationSize);
        NavigationButton("玩家视觉", 2, navigationSize);
        NavigationButton("世界与雷达", 3, navigationSize);
        NavigationButton("系统", 4, navigationSize);

        const float footerHeight = 104.0f * dpiScale;
        if (ImGui::GetContentRegionAvail().y > footerHeight) {
            ImGui::SetCursorPosY(
                ImGui::GetWindowHeight() - footerHeight);
        }
        ImGui::Separator();
        ImGui::TextColored(
            sdl_renderer::isGameForeground()
                ? ImVec4(0.250f, 0.900f, 0.600f, 1.0f)
                : ImVec4(0.930f, 0.650f, 0.260f, 1.0f),
            sdl_renderer::isGameForeground()
                ? "游戏中"
                : "输入暂停");
        ImGui::TextColored(
            ImVec4(0.500f, 0.570f, 0.670f, 1.0f),
            "%s 菜单 | %s 退出",
            GetKeyName(menuToggleKey),
            GetKeyName(exitKey));
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::SameLine(0.0f, 12.0f * dpiScale);
        ImGui::BeginChild(
            "##PageContent",
            ImVec2(0.0f, 0.0f),
            false);
        switch (currentTab) {
        case 1:
            RenderCombatPage();
            break;
        case 2:
            RenderPlayerVisualsPage();
            break;
        case 3:
            RenderWorldPage();
            break;
        case 4:
            RenderSystemPage();
            break;
        default:
            currentTab = 0;
            RenderPageHeader(
                "总览",
                "快捷开关与当前会话的实时状态。");
            RenderOverview();
            break;
        }
        ImGui::EndChild();

        ImGui::End();
        if (ImGui::IsPopupOpen(
                nullptr,
                ImGuiPopupFlags_AnyPopup)) {
            // Popups may extend beyond the main menu rectangle. Keep the whole
            // overlay interactive while one is open so their first click cannot
            // pass through to CS2.
            sdl_renderer::setInteractiveRect(
                0.0f,
                0.0f,
                static_cast<float>(WIDTH),
                static_cast<float>(HEIGHT));
        }
        publishRuntimeConfig();
    }
}
