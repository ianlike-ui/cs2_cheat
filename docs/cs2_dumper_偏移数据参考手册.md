# cs2-dumper 数据参考手册

> 生成版本：CS2 build **14190**（本地 `cs2-dumper` 输出，对齐你实际运行的游戏）
> 用途：把 dumper 能拿到的每一类数据说清楚——代表什么、怎么访问、能用来干什么。
> 原则：dumper 给的是**偏移**（"地图"），不是运行时数值（"宝物"）。下面每个偏移 = 模块基地址 + 偏移量，运行期用 `ReadProcessMemory` 取值。

---

## 0. 一句话总览

cs2-dumper 在游戏运行时读一次内存，扒出五类静态数据：

| 类别 | 文件 | 内容 |
|---|---|---|
| 全局偏移 | `offsets.hpp` | 关键对象的固定地址（本地玩家、实体表、视图矩阵、灵敏度…） |
| Schema 类字段 | `client_dll.hpp` / `server_dll.hpp` / `engine2_dll.hpp` …（20 个模块各一份） | 每个类每个字段的偏移 + 类型 |
| 接口地址 | `interfaces.hpp` | 各模块导出接口的名字 → 地址 |
| 按钮键位 | `buttons.hpp` | `IN_*` 按键对应的内存地址 |
| 枚举 | 各 `.hpp` 内联 | 字段用到的枚举（队伍、武器类型…） |

---

## 1. 全局偏移（`offsets.hpp`）——逐条说明

所有值都是 `client.dll`（除非注明模块）里的**模块相对偏移**：`地址 = 模块基址 + 偏移`。

### 玩家 / 实体
| 偏移 | 值 | 含义 |
|---|---|---|
| `dwLocalPlayerPawn` | 0x255C3C8 | **本地玩家实体**（pawn）。血、位置、视角、后座、骨骼全部从它往下读。 |
| `dwLocalPlayerController` | 0x25338A8 | 本地玩家控制器（名字、SteamID、金钱、装备）。 |
| `dwEntityList` | 0x2711598 | **实体表**。遍历所有玩家/实体的根（按索引取实体地址）。 |
| `dwGameEntitySystem` | 0x2711598 | 游戏实体系统（与 EntityList 同址）。 |
| `dwGameEntitySystem_highestEntityIndex` | +0x2120 | 实体表最高索引（遍历边界）。 |

### 渲染 / 视角
| 偏移 | 值 | 含义 |
|---|---|---|
| `dwViewMatrix` | 0x2561CD0 | **视图矩阵**（16 float）。世界坐标 → 屏幕坐标投影（ESP 画框用）。 |
| `dwViewRender` | 0x2562698 | 视图渲染器。 |
| `dwViewAngles` | 0x2572AE8 | **引擎视角角度**（引擎当前看向哪）。写它可能直接影响发给服务器的 usercmd 视角。 |
| `dwGlowManager` | 0x2558BA0 | 发光（glow）管理器。 |

### 输入 / 灵敏度 / 预测（和压枪、瞄准强相关）
| 偏移 | 值 | 含义 |
|---|---|---|
| `dwSensitivity` | 0x25596C8 | 游戏内灵敏度对象。 |
| `dwSensitivity_sensitivity` | +0x58 | **灵敏度数值**（float）。压枪"像素/角度"换算可直接从这读真实值。 |
| `dwCSGOInput` | 0x2572460 | 输入系统（usercmd 队列所在）。 |
| `dwInputSystem` | 0x46BC0 | 底层输入系统。 |
| `dwPrediction` | 0x255C2D0 | 预测系统（客户端预测用）。 |

### 全局 / 网络 / 其他
| 偏移 | 值 | 含义 |
|---|---|---|
| `dwGlobalVars` | 0x2228090 | 全局变量（当前 tick、时间、帧率）。 |
| `dwGameRules` | 0x255BE80 | 游戏规则（回合状态等）。 |
| `dwPlantedC4` | 0x24C2248 | 已安放的 C4。 |
| `dwWeaponC4` | 0x24C0800 | C4 武器实体。 |
| `dwBuildNumber` | 0x61CFE8 | 游戏版本号。 |
| `dwNetworkGameClient` | 0x91AFC0 | 网络客户端；子偏移：`_clientTickCount`(0x398)、`_serverTickCount`/`_deltaTick`(0x24C)、`_signOnState`(0x230)、`_maxClients`(0x240)、`_localPlayer`(0xF8)、`_isBackgroundMap` |
| `dwWindowHeight/Width` | 0x91F334 / 0x91F330 | 窗口尺寸（视口映射）。 |
| `dwSoundSystem` / `dwGameTypes` | 0x535350 / 0x1B0FD0 | 声音系统 / 游戏类型。 |

---

## 2. 玩家实体类字段（按功能域分组）

访问方式：`玩家地址 = dwLocalPlayerPawn`（本地）或 `dwEntityList` 取到的实体地址（他人）。

### 2.1 位置（世界坐标，Vector = 3 float）
| 字段 | 所属类 | 偏移 | 含义 |
|---|---|---|---|
| `m_vOldOrigin` | C_BasePlayerPawn | 0x14A4 | **实体原点**（脚底，网络同步的位置，最常用）。 |
| `m_vecAbsOrigin` | CGameSceneNode | 0xC8 | 绝对原点（场景节点，另一份位置源）。 |
| `m_vecOrigin` | CGameSceneNode | 0x80 | 本地原点（相对父节点）。 |
| `m_vecViewOffset` | C_BaseModelEntity | 0xF60 | 眼睛高度偏移。**眼位 = 原点 + 此偏移**（瞄准用）。 |
| `m_vecAbsVelocity` | C_BaseEntity | 0x3F8 | 绝对速度（移动判定/预判用）。 |

### 2.2 视角 / 后座（角度相关，QAngle = 3 float，本套代码按"角度"用）
| 字段 | 所属类 | 偏移 | 含义 |
|---|---|---|---|
| `m_angEyeAngles` | C_CSPlayerPawn | 0x3600 | **眼睛视角（pitch/yaw/roll）**。写它 = 改瞄准方向（魔法子弹写的就是它）。 |
| `v_angle` | C_BasePlayerPawn | 0x13A8 | 视角（服务器复制的字段）。外部写会触发预测回滚，**不要写**。 |
| `m_angAbsRotation` / `m_angRotation` | CGameSceneNode | 0xD4 / 0xB8 | 实体朝向（旋转）。 |
| `m_vecCsViewPunchAngle` | CPlayer_CameraServices | 0x48 | **相机视觉后座**（枪口飘的视觉来源，RCS 压枪读它）。 |
| `m_predictableBaseAngle` | CCSPlayer_AimPunchServices | 0x50 | **预测射击偏移**（子弹真正落点偏移，魔法子弹修正读它）。 |
| `m_unpredictableBaseAngle` | CCSPlayer_AimPunchServices | 0xA4 | 不可预测部分的后座角。 |
| `m_flMouseSensitivity` | C_BasePlayerPawn | 0x14A0 | 该玩家灵敏度（float）——压枪换算可读。 |
| `m_flFOVSensitivityAdjust` | C_BasePlayerPawn | 0x149C | 开镜 FOV 灵敏度调整。 |

### 2.3 生命 / 队伍 / 状态
| 字段 | 所属类 | 偏移 | 含义 |
|---|---|---|---|
| `m_iHealth` / `m_iMaxHealth` | C_BaseEntity | 0x34C / 0x348 | 当前/最大生命。 |
| `m_lifeState` | C_BaseEntity | 0x354 | 生死状态（0=存活）。 |
| `m_iTeamNum` | C_BaseEntity | 0x3E7 | 队伍（2=T 3=CT）。 |
| `m_fFlags` | C_BaseEntity | 0x3F4 | 状态位（`FL_ONGROUND=1` 连跳判定）。 |
| `m_entitySpottedState` | C_CSPlayerPawn | 0x1E88 | **可见性标记**（spotted，ESP/瞄准的"是否被看到"）。 |
| `m_iIDEntIndex` | C_CSPlayerPawn | 0x36DC | 准星指向的实体编号（自动扳机用）。 |
| `m_iShotsFired` | C_CSPlayerPawn | 0x1EB4 | **本连发已开枪数**（开火判定）。 |
| `m_flFlashDuration` | C_CSPlayerPawnBase | 0x1510 | 闪光剩余致盲时间（防闪光写 0）。 |
| `m_bIsScoped` | C_CSPlayerPawn | 0x1EA0 | 是否开镜。 |
| `m_flLastFiredWeaponTime` | C_CSPlayerPawn | 0x15AC | 上次开火时间。 |

### 2.4 服务指针（往下再读一层）
| 字段 | 所属类 | 偏移 | 含义 |
|---|---|---|---|
| `m_pCameraServices` | C_BasePlayerPawn | 0x1328 | 相机服务（读后座在这下面）。 |
| `m_pWeaponServices` | C_BasePlayerPawn | 0x12F0 | 武器服务（拿当前武器）。 |
| `m_pAimPunchServices` | C_CSPlayerPawn | 0x1598 | 后座服务（读预测射击偏移）。 |
| `m_pBulletServices` | C_CSPlayerPawn | 0x1570 | 子弹服务（`m_totalHitsOnServer` 服务器命中共 0x48）。 |
| `m_hController` | C_BasePlayerPawn | 0x14BC | 关联的控制器句柄。 |

---

## 3. 武器类字段

访问：`m_pWeaponServices` → `m_hActiveWeapon`（句柄，再解引用出武器实体地址）。

### 3.1 武器实例
| 字段 | 所属类 | 偏移 | 含义 |
|---|---|---|---|
| `m_iClip1` | C_BasePlayerWeapon | 0x1928 | 弹匣余弹。 |
| `m_pReserveAmmo` | C_BasePlayerWeapon | 0x1930 | 备用弹。 |
| `m_nNextPrimaryAttackTick` | C_BasePlayerWeapon | 0x1918 | 下次能开火的 tick（射速节流）。 |
| `m_weaponMode` | C_CSWeaponBase | 0x1A00 | 全自动/半自动/三连发模式。 |
| ~~`m_flCrosshairDistance`~~ | C_CSWeaponBase | **14186 已移除** | 旧版为 0x17B0（动态准星离中心距离）；该字段已被游戏删除，代码不再读取。 |
| `m_flTurningInaccuracy` | C_CSWeaponBase | 0x1A14 | 转身精度惩罚。 |
| `m_fAccuracyPenalty` | C_CSWeaponBase | 0x1A18 | 精度惩罚累计（压低可减少散布——实验性）。 |
| `m_iRecoilIndex` / `m_flRecoilIndex` | C_CSWeaponBase | 0x1A24 / 0x1A28 | **后坐指数**（CS2 新后坐模型的核心，整数/小数两部分）。 |
| `m_bInReload` | C_CSWeaponBase | 0x1A3C | 是否换弹中。 |

### 3.2 武器静态数据（CCSWeaponBaseVData，服务器判定散布/后坐用这份，但服务器用自己那份）
| 字段 | 偏移 | 含义 |
|---|---|---|
| `m_WeaponType` / `m_WeaponCategory` | 0x520 / 0x524 | 武器类型 / 分类。 |
| `m_szName` | 0x720 | 武器名。 |
| `m_nNumBullets` | 0x730 | 每次开火弹丸数（霰弹）。 |
| `m_bIsFullAuto` | 0x72D | 是否全自动。 |
| `m_flSpread` | 0x750 | 基础散布。 |
| `m_flInaccuracyStand/Crouch/Jump/Land/Fire/Move` | 0x760/0x758/0x768/0x770/0x780/0x788 | 各姿态下的散布惩罚。 |
| `m_flRecoilAngle` / `m_flRecoilMagnitude` | 0x790 / 0x7A0 | 后坐角度 / 幅度（弹道模式）。 |
| `m_nRecoilSeed` / `m_nSpreadSeed` | 0x7D4 / 0x7D8 | 后坐 / 散布随机种子。 |
| `m_nDamage` / `m_flRange` / `m_flPenetration` | 0x820 / 0x830 / 0x82C | 伤害 / 有效射程 / 穿甲。 |

> ⚠️ 这些 vdata 是静态表。**改客户端副本不影响服务器判定**——服务器按自己那份算散布/后坐。

---

## 4. 按钮键位（`buttons.hpp`，`client.dll` 内）

每个按钮名对应一个内存地址，用于**读/写按键状态**（构造 usercmd 按钮位、模拟输入）：

`attack`(开火)、`attack2`(副开火/开镜)、`jump`(跳)、`duck`(蹲)、`forward/back/left/right`(移动)、`reload`(换弹)、`use`(使用)、`sprint`(冲刺)、`turnleft/turnright`(转身)、`zoom`(开镜)、`lookatweapon`、`showscores`。

---

## 5. 接口（`interfaces.hpp`）——分组示例

| 模块 | 接口示例 | 用途 |
|---|---|---|
| client.dll | `Source2Client002`、`Source2ClientPrediction001`、`GameClientExports001` | 客户端主接口、预测、导出。 |
| engine2.dll | `EngineServiceMgr001`、`NetworkClientService_001`、`GameEventSystemClientV001` | 引擎服务、网络客户端、事件。 |
| 其他 | `InputService_001`、`HostStateMgr001`、`RenderService_001`、`SoundService_001` | 输入、主机状态、渲染、声音。 |

接口 = "拿到对象指针再调它的虚函数表"的入口；纯外部项目通常用不上，是走注入路线的起点。

---

## 6. 能做什么 / 不能做什么（结合本项目的功能）

### ✅ 这些数据足以支撑
- **ESP**：`dwViewMatrix`（投影）+ 位置/骨骼/血量/队伍/名称。
- **自动瞄准（鼠标模拟）**：位置 → 角度。
- **自动扳机**：`m_iIDEntIndex`（准星指向实体）。
- **连跳**：`m_fFlags`（FL_ONGROUND）。
- **压枪（RCS）**：`m_vecCsViewPunchAngle` + `dwSensitivity_sensitivity`/`m_flMouseSensitivity`（换算系数）+ 每帧实时后座。
- **魔法子弹（视角写入）**：`m_angEyeAngles` + `m_predictableBaseAngle`（真实弹道偏移）。
- **防闪光**：`m_flFlashDuration`。

### ❌ 这些数据给不了（CS2 服务端权威模型的边界）
- 服务器命中判定、服务器散布——在 `server.dll` 运行内存，外部读不到。
- 可 hook 的函数/签名——dumper 不输出函数地址。
- "让服务器以为你拿的是狙击枪"——需要伪造武器网络状态（强一致校验，等于必检测）。
