// ============================================================================
// weapon_names.hpp —— 武器编号 → 名字的查询表
//
// 【这个文件是干什么的】
// 游戏内存里武器只用"编号"（Item Definition Index）表示，比如 7 = AK-47。
// 要显示武器名就得查表。本文件就是这张"编号 → 名字"的静态对照表，
// 数据来源是 CS2 游戏文件里的 items_game.txt（官方数据，非逆向推导）。
// ============================================================================

#pragma once

#include <cstdint>             // uint16_t
#include <string>              // std::string
#include <unordered_map>       // 哈希表（按键快速查找）

namespace weapon_names
{
    // CS2 武器条目定义编号 → 名字
    // 数据来源：CS2 游戏文件 items_game.txt
    // 参考：https://github.com/SteamDatabase/GameTracking-CS2
    inline const std::unordered_map<uint16_t, std::string> weaponMap = {
        // ===== 手枪 =====
        {1, "Desert Eagle"},
        {2, "Dual Berettas"},
        {3, "Five-SeveN"},
        {4, "Glock-18"},
        {30, "Tec-9"},
        {32, "P2000"},
        {36, "P250"},
        {61, "USP-S"},
        {63, "CZ75-Auto"},
        {64, "R8 Revolver"},

        // ===== 步枪 =====
        {7, "AK-47"},
        {8, "AUG"},
        {10, "FAMAS"},
        {13, "Galil AR"},
        {16, "M4A4"},
        {39, "SG 553"},
        {60, "M4A1-S"},

        // ===== 狙击枪 =====
        {9, "AWP"},
        {11, "G3SG1"},
        {38, "SCAR-20"},
        {40, "SSG 08"},

        // ===== 冲锋枪 =====
        {17, "MAC-10"},
        {19, "P90"},
        {23, "MP5-SD"},
        {24, "UMP-45"},
        {26, "PP-Bizon"},
        {33, "MP7"},
        {34, "MP9"},

        // ===== 重型武器 =====
        {14, "M249"},
        {28, "Negev"},
        {25, "XM1014"},
        {27, "MAG-7"},
        {29, "Sawed-Off"},
        {35, "Nova"},

        // ===== 投掷物 =====
        {43, "Flashbang"},      // 闪光弹
        {44, "HE Grenade"},     // 高爆手雷
        {45, "Smoke Grenade"},  // 烟雾弹
        {46, "Molotov"},        // 燃烧瓶（T）
        {47, "Decoy Grenade"},  // 诱饵弹
        {48, "Incendiary"},     // 燃烧弹（CT）
        {68, "TA Grenade"},
        {81, "Frag Grenade"},
        {82, "Snowball"},       // 雪球
        {84, "Breach Charge"},  // 破片炸药

        // ===== 装备 =====
        {31, "Zeus x27"},       // 电击枪
        {42, "Knife (CT)"},     // 刀（CT）
        {49, "C4 Explosive"},   // C4 炸弹
        {50, "Kevlar Vest"},    // 防弹衣
        {51, "Kevlar + Helmet"},// 防弹衣+头盔
        {52, "Defuse Kit"},     // 拆弹钳
        {54, "Rescue Kit"},     // 救援包
        {55, "Medi-Shot"},      // 医疗针
        {57, "Healthshot"},     // 医疗针（另一版本）
        {59, "Knife (T)"},      // 刀（T）
        {80, "Shield"},         // 盾牌

        // ===== 刀（皮肤版）=====
        {500, "Bayonet"},       // 刺刀
        {503, "Karambit"},      // 爪刀
        {505, "Flip Knife"},    // 折叠刀
        {506, "Gut Knife"},     // 穿肠刀
        {507, "M9 Bayonet"},    // M9 刺刀
        {508, "Huntsman Knife"},// 猎杀者匕首
        {509, "Falchion Knife"},// 弯刀
        {512, "Bowie Knife"},   // 鲍伊猎刀
        {514, "Butterfly Knife"},// 蝴蝶刀
        {515, "Shadow Daggers"},// 暗影双匕
        {516, "Paracord Knife"},// 伞绳刀
        {517, "Survival Knife"},// 求生匕首
        {518, "Ursus Knife"},   // 熊刀
        {519, "Navaja Knife"},  // 折刀
        {520, "Nomad Knife"},   // 流浪者匕首
        {521, "Stiletto Knife"},// 刺刀
        {522, "Talon Knife"},   // 猎鹰之爪
        {523, "Classic Knife"}, // 经典小刀
        {525, "Skeleton Knife"},// 骷髅匕首

        // ===== 危险区物品 =====
        {85, "Tablet"},         // 平板
        {86, "Axe"},            // 斧头
        {87, "Hammer"},         // 锤子
        {88, "Wrench"},         // 扳手
        {89, "Spanner"},        // 活动扳手

        // ===== 手套 =====
        {5027, "Hand Wraps"},       // 手缠绷带
        {5028, "Sport Gloves"},     // 运动手套
        {5029, "Driver Gloves"},    // 驾驶手套
        {5030, "Specialist Gloves"},// 专业手套
        {5031, "Moto Gloves"},      // 摩托手套
        {5032, "Bloodhound Gloves"},// 猎犬手套
        {5033, "Hydra Gloves"},     // 九头蛇手套
        {5034, "Broken Fang Gloves"},// 破伤风手套

        // ===== 探员（角色模型）=====
        {4619, "Agent | FBI"},
        {4680, "Agent | GIGN"},
        {4711, "Agent | SAS"},
        {4712, "Agent | GSG-9"},
        {4713, "Agent | Phoenix"},
        {4714, "Agent | Elite Crew"},
        {4715, "Agent | Professionals"},
        {4716, "Agent | Sabre"},

        // ===== 音乐盒 =====
        {1314, "Music Kit"},

        // ===== 印花与徽章 =====
        {1209, "Sticker"},       // 印花
        {1348, "Patch"},         // 徽章
        {1349, "Graffiti"},      // 涂鸦

        // ===== 箱子与钥匙 =====
        {1210, "Case Key"},      // 钥匙

        // ===== 工具 =====
        {1200, "Name Tag"},          // 命名标签
        {1201, "Stattrak Swap Tool"},// 计数刻印交换工具
    };

    // 查表：编号 → 名字；查不到就返回 "Unknown"
    inline std::string getWeaponName(uint16_t itemDefIndex)
    {
        auto it = weaponMap.find(itemDefIndex);   // 哈希表查找（很快）
        if (it != weaponMap.end()) {
            return it->second;                    // 找到：返回名字
        }
        return "Unknown";                         // 没找到
    }
}

