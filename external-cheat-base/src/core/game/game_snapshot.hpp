// ============================================================================
// game_snapshot.hpp —— 定义"游戏快照"的数据模型
//
// ----------------------------------------------------------------------------
// 🎓 概念卡 ⑰：struct 结构体、enum class 枚举、std::optional、std::vector
// ----------------------------------------------------------------------------
// 【struct 结构体：打包相关数据】
// 把几个相关的变量捆成一个"自定义类型"。PlayerSnapshot 就把一个玩家的
// 所有信息（名字、队伍、血量、位置…）打包成一坨，一个变量就代表一个玩家。
// struct 里每个成员叫"字段"，用 变量.字段 访问：player.health。
//
// 【enum class：给取值起名字，还防错】
// 队伍用数字 0/1/2/3 表示容易记错（2 是哪个队？）。enum class 给它们
// 起名字：Team::Terrorists 一目了然。且它"类型安全"——不能把 int 直接
// 塞给 Team 变量，编译器会拦你。
//
// 【std::optional<T>：可能有、也可能没有的盒子】
// 比如"玩家位置"：离得太远可能没读到。optional 表达"可能没有"：
//   - 有值：if (player.position) { ... player.position->x ... }
//   - 没有：就是一个"空盒子"，判断为 false
// 关键价值：把"没数据"和"数据是 0"区分开（坐标 0,0,0 和"没读到"是两回事）。
//
// 【std::vector<T>：可变长数组】
// 玩家数量每局不一样（5v5、死斗 32 人…）。vector 可以随时增删，
// 玩家列表就用 std::vector<PlayerSnapshot>。它的元素在堆上连续排列，
// 用 .push_back() 添加、.size() 查数量、for (auto& p : vec) 遍历。
//
// 【最小示例（四合一，可以跑）】
//   #include <optional>
//   #include <vector>
//   #include <string>
//   enum class Team { T, CT };              // 枚举
//   struct Player {                         // 结构体
//       std::string name;
//       Team team;
//       std::optional<int> health;          // 可能没读到血量
//   };
//   int main() {
//       std::vector<Player> players;        // 变长数组
//       players.push_back({"Alice", Team::T, 100});
//       players.push_back({"Bob", Team::CT, std::nullopt});
//       for (const auto& p : players) {
//           if (p.health) { /* 有血量 */ }
//       }
//   }
//
// 【自己动手要点】
// 1) struct 默认所有成员 public（可以直接访问），class 默认 private；
// 2) optional 访问值用 -> 或 *（跟指针类似）；std::nullopt 表示"空"；
// 3) vector 在头文件里要用 #include <vector>，字符串用 #include <string>。
// ----------------------------------------------------------------------------
//
// 【什么是"快照"（Snapshot）？】
// 数据线程每采一次样，就把游戏当前的一帧状态整理成一个 GameSnapshot 对象：
// 当前地图、所有玩家的信息、C4 炸弹状态……然后渲染线程、网页雷达、中继、
// 录制器都从这份快照里取数据。快照是各模块之间的"共同语言"。
//
// 【概念速览】struct / enum class / std::optional / std::vector / inline constexpr
// 都在上方 🎓 概念卡 ⑰ 详解（完整索引见《搭建路线图与概念索引.md》）。
// ============================================================================

#pragma once

#include <cstdint>      // uint32_t、uint64_t 等
#include <optional>     // std::optional
#include <string>       // std::string
#include <vector>       // std::vector

namespace game
{
    // 雷达协议的版本号。网页端会检查这个号，版本不匹配说明前后端不配套
    inline constexpr std::uint32_t kWebRadarProtocolVersion = 1;

    // ------------------------------------------------------------------
    // 玩家 ID 的"脱敏"设计
    // ------------------------------------------------------------------
    // 一个玩家控制器槽位（controller slot）在整局游戏里是稳定的，
    // 但它不暴露任何外部账号标识（比如 Steam ID）。
    // 这里把槽位值放到一个"高位命名空间"里（最高位置 1），
    // 防止它和普通的槽位数字混淆。
    // 简单说：网页上看到的"玩家 ID"是程序内部生成的临时编号，
    // 不是真实账号，别人看到了也找不到你是谁。
    [[nodiscard]] inline constexpr std::uint64_t makeOpaquePlayerId(
        const std::uint32_t controllerSlot) noexcept
    {
        return UINT64_C(0x8000000000000000) | controllerSlot;
    }

    // 世界坐标：一个点在世界里的位置（x/y/z，单位是游戏内的"米"）
    struct WorldPosition
    {
        float x{};
        float y{};
        float z{};
    };

    // 队伍
    enum class Team : std::uint8_t
    {
        Unknown = 0,               // 未知
        Spectator = 1,             // 观众/旁观
        Terrorists = 2,            // 恐怖分子（T）
        CounterTerrorists = 3      // 反恐精英（CT）
    };

    // 地图阶段（一局比赛的进行状态）
    enum class MapPhase : std::uint8_t
    {
        Unknown,        // 未知
        Loading,        // 加载中
        Warmup,         // 热身
        FreezeTime,     // 冻结时间（回合开始前的准备时间）
        Live,           // 回合进行中
        RoundOver,      // 回合结束
        MatchOver       // 比赛结束
    };

    // 武器类别（用于武器图标和分类显示）
    enum class WeaponCategory : std::uint8_t
    {
        Unknown,        // 未知
        Knife,          // 刀
        Pistol,         // 手枪
        Smg,            // 冲锋枪
        Rifle,          // 步枪
        Shotgun,        // 霰弹枪
        MachineGun,     // 机枪
        SniperRifle,    // 狙击枪
        Grenade,        // 手雷
        Equipment,      // 装备（甲、钳子等）
        Bomb            // C4 炸弹
    };

    // 一把武器的快照信息
    struct WeaponSnapshot
    {
        std::uint16_t definitionIndex{};   // 武器的定义编号（游戏内部 ID）
        std::string name;                  // 武器名（内部名）
        std::string displayName;           // 显示名（玩家看到的名字）
        WeaponCategory category{ WeaponCategory::Unknown }; // 武器类别
        int clipAmmo{};                    // 弹匣里的子弹数
        int reserveAmmo{};                 // 备用子弹数
    };

    // 一名玩家的快照信息
    struct PlayerSnapshot
    {
        // 采样器生成的不透明身份 ID，绝不是 Steam ID，也不是进程/实体地址
        std::uint64_t id{};
        // Steam ID 是可选的：菜单里可以关掉（隐私），关掉后这里为空
        std::optional<std::uint64_t> steamId;
        std::string name;                  // 玩家名

        Team team{ Team::Unknown };        // 队伍
        int competitiveColor{ -1 };        // 竞技模式里的队伍颜色
        bool alive{};                      // 是否存活
        bool dormant{};                    // 是否休眠（离得太远被游戏"冻结"）

        // 位置：可能没读到（比如距离太远），所以是 optional
        std::optional<WorldPosition> position;
        // 朝向（yaw，水平角）：同样可能没读到。
        // 注意：optional 为空和"真实朝向是 0 度/正东"是两码事，
        // 用 optional 区分开，渲染端就不会瞎画一个方向箭头。
        std::optional<float> yaw;

        int health{};                      // 血量
        int armor{};                       // 护甲
        int money{};                       // 金钱
        bool hasHelmet{};                  // 有没有头盔
        bool hasDefuser{};                 // 有没有拆弹钳
        bool hasBomb{};                    // 有没有背 C4

        std::optional<WeaponSnapshot> activeWeapon;  // 当前手持武器
        std::vector<WeaponSnapshot> inventory;       // 背包里所有武器
    };

    // C4 炸弹的状态
    enum class BombState : std::uint8_t
    {
        Unknown,      // 未知
        Carried,      // 被携带
        Dropped,      // 被丢在地上
        Planted,      // 已安放
        Defused,      // 已被拆除
        Exploded      // 已爆炸
    };

    // C4 的安放点
    enum class BombSite : std::uint8_t
    {
        Unknown,      // 未知
        A,            // A 点
        B             // B 点
    };

    // C4 炸弹的快照信息
    struct BombSnapshot
    {
        BombState state{ BombState::Unknown };       // 当前状态
        BombSite site{ BombSite::Unknown };          // 安放点
        std::optional<WorldPosition> position;       // 位置
        std::optional<std::uint64_t> carrierPlayerId; // 携带者的玩家 ID
        std::optional<float> explodeInSeconds;       // 距爆炸的秒数
        bool beingDefused{};                         // 是否正在被拆除
        std::optional<float> defuseInSeconds;        // 距拆除完成的秒数
        bool defuseWillSucceed{};                    // 拆除能否成功（有没有钳子等）
    };

    // 当前地图的状态
    struct MapState
    {
        std::string id;            // 地图 ID（如 de_dust2）
        std::string displayName;   // 地图显示名
        MapPhase phase{ MapPhase::Unknown };  // 比赛阶段
        std::uint32_t roundNumber{};          // 当前回合数
        bool connected{};                     // 是否已连接到游戏
    };

    // ====================================================================
    // GameSnapshot：一整帧游戏状态的完整快照（各模块之间的"共同语言"）
    // ====================================================================
    struct GameSnapshot
    {
        std::uint32_t protocolVersion{ kWebRadarProtocolVersion }; // 协议版本号
        std::uint64_t sequence{};            // 帧序号（每帧递增，用于判断新旧）
        std::uint64_t capturedAtMs{};        // 采集时间（毫秒）

        MapState map;                        // 地图状态
        std::optional<std::uint64_t> localPlayerId;    // 本地玩家（自己）的 ID
        // 本地玩家正在"看"的玩家（观战/死亡镜头跟随的对象）。
        // optional 是为了兼容"没在观战"和"老版本采样器"的情况。
        std::optional<std::uint64_t> observedPlayerId;
        Team localTeam{ Team::Unknown };     // 本地玩家的队伍
        std::vector<PlayerSnapshot> players; // 所有玩家
        BombSnapshot bomb;                   // C4 状态
    };
}
