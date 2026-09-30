// ============================================================================
// sdl_renderer.h —— 覆盖层渲染模块的头文件（声明 + 全局状态）
//
// 【这个文件是干什么的】
// 程序里所有"窗口和画面"相关的事都由这个模块管：
//   - 创建一个 SDL2 透明窗口，叠在游戏窗口上（这就是"覆盖层"）；
//   - 初始化 ImGui（画 ESP/菜单用的界面库）；
//   - 每帧开始/结束、处理鼠标键盘事件、跟随游戏窗口移动；
//   - 提供画线/画框/画椭圆的基础绘图函数。
//
// 【概念速览】extern 变量见概念卡 ②、inline 变量见概念卡 ②（或《搭建路线图与概念索引.md》）。
// HWND：Windows 的"窗口句柄"，代表一个窗口（详见概念卡 ⑮ 的句柄讲解）。
// ============================================================================

#pragma once

#ifndef NOMINMAX
#define NOMINMAX       // 防止 Windows.h 定义 min/max 宏，避免与标准库冲突
#endif

#include <SDL.h>           // SDL2 库（窗口、渲染、事件）
#include <SDL_syswm.h>     // SDL 与系统窗口交互（拿到 HWND）
#include <Windows.h>       // Windows API（HWND 类型等）
#include <atomic>          // 原子变量
#include <cstdint>         // 整数类型

// 全局窗口/屏幕尺寸（定义在 main.cpp，这里声明给所有文件用）
extern uint32_t WIDTH;      // 覆盖层窗口宽度
extern uint32_t HEIGHT;     // 覆盖层窗口高度
extern uint32_t WINDOW_W;   // 游戏窗口宽度
extern uint32_t WINDOW_H;   // 游戏窗口高度
extern int32_t VIEWPORT_X;  // 视口 X
extern int32_t VIEWPORT_Y;  // 视口 Y
extern uint32_t VIEWPORT_W; // 视口宽
extern uint32_t VIEWPORT_H; // 视口高

namespace sdl_renderer
{
    // ------------------------------------------------------------------
    // 全局状态（inline：头文件里定义的共享全局变量）
    // ------------------------------------------------------------------
    inline SDL_Window* window = nullptr;      // SDL 窗口对象（覆盖层）
    inline SDL_Renderer* renderer = nullptr;  // SDL 渲染器对象
    inline bool running = true;               // 程序是否在运行（F9 置 false 退出）
    inline HWND overlayHwnd = nullptr;        // 覆盖层窗口的 Windows 句柄
    inline HWND gameHwnd = nullptr;           // 游戏窗口的 Windows 句柄
    inline bool menuVisible = true;           // 菜单是否显示

    // 初始化/销毁
    bool init(const wchar_t* targetWindowName, DWORD targetProcessId = 0);  // 初始化覆盖层（叠在游戏窗口上）
    bool initWaiting();  // 初始化"等待界面"（还没有游戏窗口时用）
    void destroy();      // 销毁窗口和渲染器

    // 每帧流程
    bool beginFrame();   // 开始一帧（清屏、锁定绘制）
    void endFrame();     // 结束一帧（把画的内容显示出来）

    // 事件与窗口管理
    void pollEvents();                     // 处理鼠标/键盘/窗口事件
    void updateWindowPosition();           // 跟随游戏窗口移动/缩放
    bool isGameVisible();                  // 游戏窗口当前是否可见
    bool isGameForeground();               // 游戏窗口是否在前台（被激活）
    bool isInputAllowed();                 // 是否允许键盘/鼠标输入穿透到游戏
    bool isGameDisconnected();             // 游戏进程是否已断开
    int getTargetRefreshRate();            // 目标刷新率（显示器刷新率）
    bool isAcceleratedRenderer();          // 是否使用 GPU 加速渲染
    bool isVsyncEnabled();                 // 是否开启了垂直同步
    bool isDpiAwarenessReliable();         // DPI 感知是否可靠
    bool isGameOnSingleMonitor();          // 游戏是否只在单个显示器上
    void setInteractiveRect(float x, float y, float width, float height);  // 标记"鼠标应被拦截"的区域
    float getDpiScale();                   // DPI 缩放系数
    uint32_t getDpiRevision();             // DPI 版本号（变了说明分辨率改了）
    uint64_t getRendererRevision();        // 渲染器状态版本号

    // ImGui 初始化与帧流程
    bool initImGui();                      // 初始化 ImGui
    void shutdownImGui();                  // 关闭 ImGui
    void newFrameImGui();                  // ImGui 新一帧开始
    void renderImGui();                    // 把 ImGui 绘制内容渲染到屏幕

    // 基础绘图函数（画 ESP 用）
    namespace draw
    {
        void line(int x1, int y1, int x2, int y2, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);        // 画线
        void box(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);             // 画空心方框
        void filledBox(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);       // 画实心方框
        void filledEllipse(int cx, int cy, int rx, int ry, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255); // 画实心椭圆
    }
}
