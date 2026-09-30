// ============================================================================
// esp.hpp —— ESP（信息增强）模块的头文件：数据结构和对外接口
//
// 【这个文件是干什么的】
// 声明了 ESP 模块用到的主要数据结构（敌人信息、本地玩家缓存、炸弹信息、
// 世界实体信息、玩家缓存）和对外函数。
//
// 【数据流（重要）】
// 数据线程调用 updateEntities() 读游戏内存，把结果存成三份"只读快照"：
//   - enemies（敌人列表）、worldEntities（投掷物/掉落武器）、gameSnapshot（完整快照）
// 渲染线程通过 getEnemySnapshot() / getGameSnapshot() 拿到快照来画。
// 因为快照用 std::shared_ptr<const ...>（共享只读智能指针）传递，
// 数据线程"换新快照"和渲染线程"读旧快照"互不冲突，几乎不用加锁。
//
// 【概念速览】uintptr_t 见概念卡 ⑬、extern 见 ②、struct 见 ⑰
// （完整索引见《搭建路线图与概念索引.md》）。
// ============================================================================

#pragma once

#include <optional>
#include <vector>

#include "core/memory/memory.hpp"          // 内存读取
#include "core/game/game_snapshot.hpp"     // 游戏快照模型
#include "client_dll.hpp"                  // cs2-dumper 生成的类字段偏移
#include "offsets.hpp"                     // cs2-dumper 生成的全局偏移
#include "utils/math/vector.hpp"           // vec2/vec3
#include <string>
#include <memory>

// 窗口尺寸（定义在 main.cpp）
extern uint32_t WIDTH;
extern uint32_t HEIGHT;
extern uint32_t WINDOW_W;
extern uint32_t WINDOW_H;

// 视图矩阵：16 个 float 组成的 4x4 矩阵。
// 作用：把 3D 世界坐标投影到 2D 屏幕坐标（"从玩家的眼睛看出去"）。
struct viewMatrix
{
    float m[16];
};

// CS2 的骨骼索引编号（人体骨骼在游戏数据里的固定编号）。
// 比如编号 6 是头，22 是右脚。画"骨骼透视"（把敌人的骨架画出来）要用。
namespace BoneIndex
{
    constexpr int PELVIS = 0;          // 骨盆
    constexpr int SPINE_2 = 2;         // 脊柱
    constexpr int NECK = 5;            // 脖子
    constexpr int HEAD = 6;            // 头
    constexpr int LEFT_SHOULDER = 8;   // 左肩
    constexpr int LEFT_ELBOW = 9;      // 左肘
    constexpr int LEFT_HAND = 11;      // 左手
    constexpr int RIGHT_SHOULDER = 13; // 右肩
    constexpr int RIGHT_ELBOW = 14;    // 右肘
    constexpr int RIGHT_HAND = 15;     // 右手
    constexpr int LEFT_HIP = 17;       // 左髋
    constexpr int LEFT_KNEE = 18;      // 左膝
    constexpr int LEFT_FOOT = 19;      // 左脚
    constexpr int RIGHT_HIP = 20;      // 右髋
    constexpr int RIGHT_KNEE = 21;     // 右膝
    constexpr int RIGHT_FOOT = 22;     // 右脚
    constexpr int BONE_COUNT = 28;     // 总共跟踪 28 块骨骼
}

// 一对"应该用线连起来"的骨骼（画骨架用）
struct BoneConnection
{
    int from;    // 起点骨骼编号
    int to;      // 终点骨骼编号
};

// 一名敌人的完整信息（一帧采样得到）
struct EnemyInfo
{
    uintptr_t pawnAddress = 0;      // 敌人实体在游戏内存里的地址
    uint32_t entityIndex = 0;       // 实体列表里的编号
    vec3 position{};                // 敌人身体位置（世界坐标）
    vec3 headPosition{};            // 敌人头部位置（瞄准用）
    int32_t health = 0;             // 血量
    float distance = 0.0f;          // 距我的距离
    std::string weaponName;         // 手持武器名
    float viewYaw = 0.0f;           // 敌人面朝方向（角度）
    float angleToPlayer = 180.0f;   // 敌人面朝方向和"朝向我"之间的夹角
    float flashDuration = 0.0f;     // 闪光弹剩余致盲时间
    bool viewAngleKnown = false;    // 是否读到了面朝方向
    bool isFlashed = false;         // 是否正被致盲
    bool isSpotted = false;         // 是否已被我（队友）标记 spotted
    bool visibilityKnown = false;   // 是否知道可见性状态
    vec3 bonePositions[BoneIndex::BONE_COUNT]{};  // 28 块骨骼的世界坐标
    bool hasBones = false;          // 骨骼数据是否有效
};

// 本地玩家（自己）的缓存信息（每帧更新一次，瞄准/扳机共用）
struct LocalPlayerCache
{
    uintptr_t pawn = 0;             // 自己实体在内存里的地址
    vec3 position{};                // 自己的位置
    vec3 eyePosition{};             // 自己眼睛的位置（视角高度）
    vec2 viewAngle{};               // 当前视角（pitch, yaw）
    vec3 viewPunch{};               // 当前后座视角偏移（m_vecCsViewPunchAngle，弧度，压枪用）
    vec3 aimPunch{};                // 预测射击偏移（CCSPlayer_AimPunchServices::m_predictableBaseAngle，弧度，真实弹道用）
    float mouseSensitivity = 0.0f;  // 本地玩家当前灵敏度（m_flMouseSensitivity，压枪换算用，0=未知）
    int32_t weaponIndex = 0;        // 当前武器定义索引（m_iItemDefinitionIndex，录制/回放用，0=未知）
    int32_t weaponRecoilIndex = 0;  // 当前武器 m_iRecoilIndex（int，逐发递增，诊断用）
    float weaponRecoilIndexF = 0.0f; // 当前武器 m_flRecoilIndex（float，诊断用）
    float crosshairDistance = 0.0f; // 当前武器 m_flCrosshairDistance（动态准星离中心距离，诊断用）
    int32_t shotsFired = 0;         // 当前连发已开火次数（m_iShotsFired，判定是否在开枪）
    int32_t crosshairEntityIndex = -1; // 准星指向的实体编号（-1 = 没指着实体）
    uint8_t team = 0;               // 自己的队伍
    bool isValid = false;           // 本帧缓存是否有效
};

// 炸弹（C4）信息
struct BombInfo
{
    bool isPlanted = false;         // 是否已安放
    bool isDefusing = false;        // 是否正在拆除
    bool hasExploded = false;       // 是否已爆炸
    bool isDefused = false;         // 是否已被拆除
    float blowTime = 0.0f;          // 爆炸时刻（游戏内时间）
    float defuseCountDown = 0.0f;   // 拆除倒计时
    float curtime = 0.0f;           // 当前游戏时间
    int bombSite = 0;               // 安放点（0=A，1=B）
    uint64_t sampledAtMilliseconds = 0;  // 采样时刻（判断数据是否过期）
    vec3 position{};                // 炸弹位置
    bool positionKnown = false;     // 位置是否已知
};

// 世界实体信息（投掷物、掉落武器）
struct WorldEntityInfo
{
    vec3 position;                  // 位置
    int type;                       // 类型：0=烟 1=闪 2=HE 3=燃烧瓶 4=诱饵 5=武器
    std::string name;               // 名字
    float distance;                 // 距离
};

// 玩家缓存（快速路径用）：记住玩家控制器的关键字段，
// 减少每帧都要重新读的字段数
struct CachedPawn
{
    uintptr_t controllerAddress = 0;  // 控制器对象地址
    uintptr_t pawnAddress = 0;        // 实体（pawn）地址
    uint32_t pawnHandle = 0;          // 实体句柄
    uint32_t entityIndex = 0;         // 实体编号
    uint8_t team = 0;                 // 队伍
    uint64_t playerId = 0;            // 不透明玩家 ID
    uint64_t steamId = 0;             // Steam ID（可能未知）
    bool steamIdKnown = false;        // Steam ID 是否已知
    std::string playerName;           // 玩家名
    int competitiveColor = -1;        // 竞技队伍颜色
    int armor = 0;                    // 护甲
    int money = 0;                    // 金钱
    bool alive = false;               // 存活
    bool isLocal = false;             // 是不是自己
    bool hasHelmet = false;           // 有头盔
    bool hasDefuser = false;          // 有拆弹钳
    bool hasBomb = false;             // 背着 C4
    std::string weaponName;           // 武器名
    std::optional<game::WeaponSnapshot> activeWeapon;   // 当前武器
    std::vector<game::WeaponSnapshot> inventory;        // 背包
    float flashDuration = 0.0f;       // 致盲剩余
    bool isFlashed = false;           // 是否致盲
};

#include <mutex>    // 互斥锁

// 前向声明（让 esp 模块能引用 menu::RuntimeConfig，不必包含整个 menu.hpp）
// ----------------------------------------------------------------------------
// 🎓 概念卡 ㉓：前向声明（Forward Declaration，"先告诉编译器有这么个东西"）
// ----------------------------------------------------------------------------
// 【是什么】`struct RuntimeConfig;` 只声明"存在一个叫 RuntimeConfig 的结构体
// 类型"，但不给出它的定义（有哪些字段）。作用：
//   1. 让本文件能用"指向它的指针/引用"（比如函数参数 const RuntimeConfig&）；
//   2. **避免头文件互相包含**：如果 esp.hpp 去 #include 整个 menu.hpp，
//      而 menu.hpp 又 include 了 esp.hpp，两个头文件互相包含会编译失败
//      （循环依赖）。前向声明就是打破循环的办法。
// 【为什么在这里】esp 的函数需要接收 menu::RuntimeConfig，但又不能包含
// menu.hpp（会循环），所以只声明"有这么个结构体"。真正的定义在 menu.hpp，
// 调用方 include 后编译器自然知道全部细节。
// 【最小示例】
//   // a.hpp：不知道 B 的细节也能用它做参数
//   struct B;
//   void useB(const B& b);        // 合法：引用不需要完整定义
//   B makeB();                    // 合法：返回 B 也只需要声明
//   // b.hpp 里才真正定义 struct B { int x; };
// 【自己动手要点】
// 1) 前向声明只够"用指针/引用"，不够"访问成员"（b.x 需要完整定义）；
// 2) 头文件里能用前向声明就别 include，能明显加快编译速度；
// 3) 什么时候必须完整 include：需要 sizeof、访问成员、按值传参时。
// ----------------------------------------------------------------------------
namespace menu
{
    struct RuntimeConfig;
}

namespace esp
{
    // ------------------------------------------------------------------
    // 三种"共享只读快照"类型：
    // std::shared_ptr<const std::vector<EnemyInfo>> 表示
    // "指向一个不可修改的敌人列表的共享指针"。多个线程可以安全地同时读。
    // ------------------------------------------------------------------
    using EnemySnapshot =
        std::shared_ptr<const std::vector<EnemyInfo>>;
    using WorldEntitySnapshot =
        std::shared_ptr<const std::vector<WorldEntityInfo>>;
    using GameSnapshot = std::shared_ptr<const game::GameSnapshot>;

    // 全局快照（数据线程写、渲染线程读）
    inline EnemySnapshot enemies =
        std::make_shared<const std::vector<EnemyInfo>>();
    inline WorldEntitySnapshot worldEntities =
        std::make_shared<const std::vector<WorldEntityInfo>>();
    inline GameSnapshot gameSnapshot =
        std::make_shared<const game::GameSnapshot>();
    inline viewMatrix vm = {};                 // 当前视图矩阵
    inline vec3 player_position{};             // 自己位置
    inline float player_yaw = 0.0f;            // 自己朝向
    inline uintptr_t pID;                      // 游戏进程 ID
    inline uintptr_t modBase;                  // client.dll 基地址

    // 数据线程和渲染线程之间的互斥锁（保护下面几个缓存变量）
    inline std::mutex dataMutex;

    // 玩家缓存（周期性刷新）
    inline std::vector<CachedPawn> cachedPawns;

    // 本地玩家缓存（每帧更新一次）
    inline LocalPlayerCache localPlayer;

    // 炸弹状态
    inline BombInfo bombInfo;

    // 对外接口（声明；实现在 esp.cpp）
    bool init();                                 // 找到游戏进程和模块基地址
    void updateEntities(const menu::RuntimeConfig& config);  // 数据线程调用：读实体数据
    void refreshEntityCache(const menu::RuntimeConfig& config); // 刷新玩家缓存
    void render();                               // 渲染线程调用：画 ESP
    void renderBombTimer();                      // 画 C4 倒计时
    void clearRuntimeState();                    // 清空运行时状态（游戏断开时）
    EnemySnapshot getEnemySnapshot();            // 取敌人快照（渲染用）
    GameSnapshot getGameSnapshot();              // 取完整快照（雷达/中继用）
    bool w2s(const vec3& world, vec2& screen, float m[16]);  // 世界坐标→屏幕坐标（投影）
    double player_distance(const vec3& a, const vec3& b);    // 两点距离
    float normalizeAngle(float angle);           // 角度归一化到 [-180,180]
    float calculateYawToTarget(const vec3& from, const vec3& to); // 计算朝向目标的角度
    float calculateAngleToPlayer(float enemyYaw, const vec3& enemyPos, const vec3& playerPos); // 敌人朝向与"朝向我"的夹角
}
