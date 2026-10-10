# CS2 External ESP

> [!IMPORTANT]
> ## Attribution
>
> **This repository is a locally modified copy of the original project — NOT original work.**
>
> - **Original author:** tiansongyu — <https://github.com/tiansongyu>
> - **Original repository:** **<https://github.com/tiansongyu/cs2_cheat>**
>
> This copy (<https://github.com/ianlike-ui/cs2_cheat>) only reuses the original author's code with local
> modifications (dual aimbot modes, magic bullet, visual hitbox picker, teleport, per-feature hotkeys, ...).
> **All original code, design, documentation and licensing belong to the original author.** For the latest
> version, releases and official support, please use the **original repository**:
> <https://github.com/tiansongyu/cs2_cheat>.

---

[![Build](https://github.com/ianlike-ui/cs2_cheat/actions/workflows/build.yaml/badge.svg?branch=main)](https://github.com/ianlike-ui/cs2_cheat/actions/workflows/build.yaml)
[![Offsets](https://github.com/ianlike-ui/cs2_cheat/actions/workflows/update-files.yml/badge.svg?branch=main)](https://github.com/ianlike-ui/cs2_cheat/actions/workflows/update-files.yml)
[![Release](https://img.shields.io/github/v/release/ianlike-ui/cs2_cheat?display_name=tag&sort=semver)](https://github.com/ianlike-ui/cs2_cheat/releases/latest)
[![License](https://img.shields.io/github/license/ianlike-ui/cs2_cheat)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D4)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599C)

An educational CS2 external ESP project built with SDL2, Dear ImGui, and C++20. It includes an in-game overlay, a fixed-map Radar, and an optional public sharing service.

[中文](README.md) | **English**

> [!WARNING]
> This project is intended only for learning reverse engineering and graphics rendering. Run it only in an offline environment launched with `-insecure`. Do not use it on VAC servers or in any online game. You are responsible for the consequences of misuse.

## 🚀 Beginner Quick Start (out of the box)

> **No compiler, no Visual Studio required.** Download → extract → add one launch option → double-click the exe → press `F4`. Working in five minutes.
>
> **[⬇️ Download the latest release](../../releases/latest)**

### Step 1 — Download

Get `cs2_cheat-vX.Y.Z-win64.zip` from **[Releases](https://github.com/ianlike-ui/cs2_cheat/releases/latest)**.
Latest: **[v1.0.14190](https://github.com/ianlike-ui/cs2_cheat/releases/tag/v1.0.14190)** (offsets for **CS2 build 14190**).

### Step 2 — Extract (do **not** drag out the exe alone)

Extract anywhere, e.g. `D:\cs2esp\`, keeping this layout:

```text
D:\cs2esp\
├─ external-cheat-base.exe     <- main executable
├─ SDL2.dll                    <- MUST sit next to the exe, otherwise it fails to start
├─ web-radar\dist\...          <- web radar assets, keep them
├─ CIVETWEB-LICENSE.md
├─ LICENSE
└─ 来源声明-ATTRIBUTION.txt
```

> ❗ Most common mistake: dragging only `external-cheat-base.exe` to the desktop → "SDL2.dll not found".

### Step 3 — Launch CS2 with `-insecure`

1. Open **Steam → Library**, right-click **Counter-Strike 2** → **Properties**
2. On the **General** tab, set **Launch Options** to: `-insecure`
3. Close the window and start CS2 normally
4. Join an **offline / bot match** (never a VAC server or official matchmaking)

Windowed Fullscreen is recommended: the overlay requires the game window to sit entirely on one monitor. Exclusive fullscreen is unsupported.

### Step 4 — Run the cheat

Double-click `external-cheat-base.exe`; running it **as administrator** is more reliable (reading another process needs the rights).
SmartScreen or your antivirus may warn on first launch — normal for an unsigned external memory reader.

### Step 5 — Open the menu

Press **`F4`** in game to show/hide the menu. **ESP is on by default**, so you should see boxes immediately. Press **`F9`** to exit.

### Default hotkeys

| Key | Action |
| --- | --- |
| `F4` | Show / hide the menu |
| `F9` | Exit |
| `Shift` | Aim assistance (hold, after enabling) |
| `F` | Triggerbot (hold, after enabling) |
| `F6` | Recoil recording |
| `F7` | Recoil control on / off |
| `F8` | Auto counter-strafe on / off |
| `F10` | Auto rotation (model spin, visible to others) on / off |
| `F11` | Teleport on / off |
| `F12` | Switch aim mode (mouse aim ↔ magic bullet) |
| `Insert` | On-fire trajectory offset on / off |
| `Home` | Cycle hitbox (head / neck / chest / stomach / arms / legs) |
| `End` | Cycle smart-aim priority |
| `PageUp` | Cycle viewport mode |

Every hotkey can be rebound in its own feature panel, and **duplicate bindings are allowed** (the menu shows a yellow warning when one key is bound to several features).

### Memory-writing features need an extra flag

Magic bullet, teleport, auto rotation and anti-flash **write to game memory and are off by default**:

```powershell
.\external-cheat-base.exe --allow-memory-writes
```

### Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| Nothing happens, or "SDL2.dll not found" | `SDL2.dll` is not next to the exe, or the antivirus quarantined it |
| `F4` does nothing / features inactive | Missing `-insecure`, not in an offline match, or insufficient rights (run as administrator) |
| Overlay invisible | Exclusive fullscreen, or the window spans monitors → use Windowed Fullscreen on one monitor |
| ESP boxes appear but positions/health are wrong | Offsets do not match your game build → see "Version mismatch" below |
| Antivirus alert | Unsigned external memory reader; decide yourself whether to allow it |

Runtime log: `%TEMP%\cs2-esp.log`.

### Version mismatch

The release ships offsets for **CS2 build 14190**. Every CS2 update changes the memory layout; if the game updates and things break, either grab a newer release (here or once [a2x/cs2-dumper](https://github.com/a2x/cs2-dumper) publishes the matching build) or dump the current build yourself and rebuild (see "Building" below).

---

## Learning Resources

| Topic | Video | Documentation |
| --- | --- | --- |
| Memory structures | [BV14szCBYErE](https://www.bilibili.com/video/BV14szCBYErE) | [CS2 memory structures](docs/CS2_Memory_Structure_Basics.md) |
| Source and CE walkthrough | [BV1Jm6gBaEEd](https://www.bilibili.com/video/BV1Jm6gBaEEd) | [Code walkthrough](docs/cheat_code.md) |
| Finding offsets | [BV1Lr6wBeEEF](https://www.bilibili.com/video/BV1Lr6wBeEEF) | [IDA reverse engineering](docs/IDA_Reverse_Engineering_Client_DLL.md) |
| Projection matrices | [BV1goFNzSEP3](https://www.bilibili.com/video/BV1goFNzSEP3) | [3D projection](docs/3d_projection_explained.md) |
| View matrices | [BV1AtF5zhE5J](https://www.bilibili.com/video/BV1AtF5zhE5J) | [View-matrix math](docs/cs2_view_matrix_math.md) |
| Signatures and IDA | [BV1yhcszwEJQ](https://www.bilibili.com/video/BV1yhcszwEJQ) | - |

## Features

| Component | Capabilities |
| --- | --- |
| ESP | Boxes, skeletons, health, weapons, distance, snaplines, spotted state, grenades, and dropped weapons |
| Assistance | Aim assistance with FOV and smoothing, triggerbot, bomb timer; Anti-Flash requires explicit memory-write permission |
| Radar | Fixed north-up maps, player position and direction, floor and confidence state, team panels, equipment, and C4 state |
| Sharing and replay | Local Web Radar, trusted-LAN access, public Relay, sanitized NDJSON recording, and browser playback |
| Performance and diagnostics | Adaptive 4/10/20 Hz Radar sampling, slow-client backpressure, timing, RPM, P95/P99, and deadline metrics |

![Application screenshot](img/hack.png)

## Requirements

- Windows 10 or Windows 11 (x64)
- Visual Studio 2022 with the Desktop development with C++ workload
- Node.js 22.12 or newer when building Web Radar
- CS2 launched with `-insecure` in Windowed Fullscreen mode

The overlay supports a game window located entirely on one monitor. Rendering pauses while the window spans monitors and resumes when it moves back. Exclusive fullscreen is unsupported. The application requests administrator privileges at startup and reconnects automatically after CS2 restarts.

## Quick Start

See **🚀 Beginner Quick Start** above for the full walkthrough. One-line version:

**Extract keeping the full layout → launch CS2 with `-insecure` in an offline match → run `external-cheat-base.exe` as administrator → `F4` for the menu, `F9` to exit.**

Web Radar and the memory-writing features (magic bullet, teleport, auto rotation, anti-flash) are disabled by default. Enable memory writes only for explicit offline testing:

```powershell
.\external-cheat-base.exe --allow-memory-writes
```

Runtime logs are written to `%TEMP%\cs2-esp.log`.

## Radar

### Local and browser modes

1. Press `F4` and open **World & Radar**.
2. Enable **Local map overlay** to display the fixed full map in the game overlay.
3. For a browser view, enable **Web Radar** and open the token-bearing URL shown in the menu. The default address is `127.0.0.1:22006`.
4. For access from a trusted LAN, enable LAN binding, replace `127.0.0.1` with the PC's private IPv4 address, and keep the full token in the URL.

Player names, Steam IDs, and team information can be filtered before transmission. Sampling runs at 4 Hz with no viewers, 10 Hz for background sharing, and 20 Hz with active viewers. When **Record sanitized Radar snapshots** is enabled, sanitized recordings are written to `%LOCALAPPDATA%\AegisCS2\recordings`; load a `.ndjson` file in Web Radar for playback and seeking.

The local service uses HTTP/WS, so its token does not encrypt transport. Do not port-forward it, expose it through a public tunnel, or share it over an untrusted network. See the [Web Radar design](docs/web-radar-design.md) for architecture and security details.

### Public sharing

The public Relay shares Radar data through an outbound `wss://` connection from the application, without exposing an inbound port on the gaming PC. It provides separate Producer and Viewer credentials, room isolation, rate limiting, and bounded latest-frame broadcasting.

Deployment and design references:

- [Public Relay deployment guide](deploy/public-relay/README.md)
- [Public Relay design](docs/public-radar-relay-design.md)
- [Public Radar architecture](docs/public-radar-relay-architecture.html)

## Building

### Visual Studio 2022

```powershell
git clone https://github.com/tiansongyu/cs2_cheat.git
cd cs2_cheat\web-radar
npm ci
npm test
npm run build
cd ..
```

Open `external-cheat-base.sln` in Visual Studio 2022, select `Release | x64`, and build. MSBuild copies an existing `web-radar/dist` directory into the binary output. Keep the complete `web-radar/dist` directory when distributing the application.

### Reproducible Docker build

```bash
docker build --target artifacts -t cs2-cheat-build .
```

This runs the frontend, C++, and Go tests before cross-compiling the Windows application. Build the production public Relay image separately:

```bash
docker build -f radar-relay/Dockerfile -t cs2-radar-relay:local .
```

To synchronize the pinned, SHA-256-verified map assets manually:

```bash
python3 scripts/sync_web_radar_maps.py
```

## Project Structure

| Path | Purpose |
| --- | --- |
| `external-cheat-base/` | Windows C++ application, overlay, and embedded Radar service |
| `web-radar/` | Browser Radar frontend and map assets |
| `radar-relay/` | Go public Relay service |
| `deploy/public-relay/` | Caddy, Docker Compose, and production deployment tools |
| `tests/` | C++ regression tests |
| `docs/` | Technical, architecture, and security documentation |

## CI and Offset Updates

The **Build** badge at the top is generated dynamically by GitHub Actions. Green means the latest `main` build passed; red means a build or test failed. CI runs the C++ regression suite, Web Radar tests and build, Go tests and vulnerability checks, Relay image builds, and the Windows `Release | x64` build.

The **Offsets** workflow checks `cs2-dumper` hourly. It fetches generated headers at an exact commit SHA, validates and commits them, and then triggers the complete build.

## License and Third-Party Assets

Project code is released under the [MIT License](LICENSE).

Radar images and overview coordinates are Valve property and remain subject to Valve's copyright and terms. Every distribution must retain `web-radar/dist/maps/NOTICE.txt` and `SOURCE.json`. Web Radar is a clean-room reimplementation and contains no GPL-3.0 source from `cs2_webradar`.

Primary dependencies and data sources: [cs2-dumper](https://github.com/a2x/cs2-dumper), [SDL](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), [CivetWeb](https://github.com/civetweb/civetweb), [Caddy](https://caddyserver.com/), [gorilla/websocket](https://github.com/gorilla/websocket), and [awpy-data](https://github.com/pnxenopoulos/awpy-data).
