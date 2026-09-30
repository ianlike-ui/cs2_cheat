// ============================================================================
// main.cpp —— 整个程序的入口和"心脏"
//
// 【这个文件是干什么的】
// 程序启动后：
//   1. 先显示一个"等待连接"界面，每隔 3 秒检查一次 CS2 游戏是否在运行；
//   2. 找到游戏进程后，创建一个"透明覆盖层"窗口叠在游戏窗口上；
//   3. 同时启动两条线程：一条负责读游戏内存（数据线程），一条负责画画（渲染线程）；
//   4. 游戏退出后回到第 1 步，程序直到用户按 F9 才结束。
//
// 【给完全不懂 C/C++ 的读者的概念速览】
// 头文件、命名空间、对象（struct/class）等基础概念，见本文件里的 🎓 概念卡
// ①③④⑤⑥（或《搭建路线图与概念索引.md》的完整索引）。
// ============================================================================

// ============================================================================
// 🎓 概念卡 ①：头文件与编译单元（C++ 工程的"积木"机制）
// ----------------------------------------------------------------------------
// 【是什么】
// 一个 C++ 项目由很多"编译单元"组成：每个 .cpp 文件是一个编译单元，
// 会被编译器单独翻译成机器码，最后"链接器"把它们拼成一个 exe。
// .h/.hpp 头文件则是"图纸"：里面写"这里有什么"（声明），
// 具体怎么做写在 .cpp 里（实现）。其他文件想用某个函数，只需要
// #include 它的头文件，编译器就知道"有这么个东西"，链接时再对上号。
//
// 【为什么在这里】main.cpp 是整个程序的入口，几乎每个模块都从这里被引用，
// 所以它是理解"头文件机制"最自然的地方。
//
// 【最小示例（可以自己跑）】
//   // 文件 math_util.hpp
//   #pragma once                     // 防止同一个头文件被重复包含
//   int add(int a, int b);           // 只写声明
//
//   // 文件 math_util.cpp
//   #include "math_util.hpp"
//   int add(int a, int b) { return a + b; }   // 写实现
//
//   // 文件 main.cpp
//   #include "math_util.hpp"         // 引入声明
//   int main() { return add(1, 2); } // 放心调用
//
// 【自己动手要点】
// 1) 头文件里放"声明"，别放"实现"（除非是模板或 inline）；
// 2) 头文件第一行写 #pragma once（或 #ifndef 守卫），防止重复包含报错；
// 3) 尖括号 <xxx> 是"找编译器自带的库"，引号 "xxx" 是"找项目自己的文件"。
// ============================================================================

// 引入本项目的其他模块的头文件（声明了 esp::、aimbot::、menu:: 等函数/对象）
#include "features/esp.hpp"                     // ESP（在敌人身上画框/血条）模块
#include "features/aimbot.hpp"                  // 自动瞄准模块
#include "core/renderer/sdl_renderer.h"         // 覆盖层渲染（SDL2 库封装）
#include "features/menu.hpp"                    // 游戏内设置菜单（按 F4 打开）
#include "features/recoil_patterns.hpp"         // 枪械弹道录制/回放（加载已录模式）
#include "features/web_radar/public_relay_producer.hpp" // 公网中继的生产端（向服务器推数据）
#include "features/web_radar/web_radar_service.hpp"     // 本机网页雷达的 Web 服务
#include "features/web_radar/snapshot_recorder.hpp"     // 脱敏录制（保存为可回放的文件）
#include "features/local_radar/local_fixed_radar.hpp"   // 游戏内叠加的"固定全地图"雷达
#include "core/game/web_radar_json.hpp"         // 把游戏快照序列化成 JSON 字符串（给网页用）
#include "core/diagnostics.hpp"                 // 日志/诊断工具
#include "core/performance_metrics.hpp"         // 性能指标统计（采样耗时、P95/P99 等）
#include "core/runtime_timing.hpp"              // 采样频率的计算规则

// 引入 C++ 标准库
#include <algorithm>    // std::max/min 等算法
#include <array>        // 定长数组
#include <bcrypt.h>     // Windows 加密库（用来生成随机的访问令牌 token）
#include <thread>       // 多线程（std::thread）
#include <atomic>       // 原子变量（多线程安全地共享数值）
#include <filesystem>   // 文件路径操作（C++17 起加入标准库）
#include <iostream>     // 标准输入输出
#include <locale>       // 本地化
#include <codecvt>      // 编码转换
#include <chrono>       // 时间（std::chrono::steady_clock 用于计时）
#include <memory>       // 智能指针（std::shared_ptr 等）
#include <mutex>        // 互斥锁（防止多线程同时改同一份数据）
#include <new>          // placement new（在指定内存位置构造对象）
#include <optional>     // 可选值（可能"没有"的变量，如 std::optional<float>）
#include <stdexcept>    // 标准异常（std::runtime_error）
#include <string>       // 字符串 std::string
#include <string_view>  // 只读的字符串视图（更轻量）
#include <utility>      // std::move 等工具
#include <Windows.h>    // Windows 系统 API（进程、消息框、注册表等）
#include "imgui.h"      // ImGui 界面库（绘制菜单和图形）

// 取消下面这行注释可以显示一个控制台窗口（默认隐藏，因为程序是图形界面）
// #define SHOW_CONSOLE

// ----------------------------------------------------------------------------
// 全局变量：窗口和屏幕的尺寸。
// 这些变量由 sdl_renderer 在初始化时填入，供整个程序使用。
// 注意：它们不是 const，因为在运行期才知道游戏窗口多大、屏幕多大。
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// 全局变量与 extern（概念卡见下方）
// ----------------------------------------------------------------------------
// 🎓 概念卡 ②：全局变量与 extern（跨文件共享数据）
// ----------------------------------------------------------------------------
// 【是什么】
// 普通变量定义在函数里，是"局部"的。写在所有函数外面的变量是"全局变量"，
// 任何地方都能访问。extern 关键字的作用是"声明"：告诉编译器"这个变量在别的
// 文件里定义过，我这里只是借用"。定义只能有一处，extern 声明可以有很多处。
//
// 【为什么在这里】窗口尺寸（WIDTH/HEIGHT/视口）几乎每个模块都要用
// （画 ESP 要知道屏幕多大），所以设计成全局变量。它们不是 const，
// 因为尺寸要在运行期根据游戏窗口动态确定。
//
// 【最小示例】
//   // a.cpp
//   int shared = 10;        // 定义（唯一一处）
//   // b.cpp
//   extern int shared;      // 声明：借用 a.cpp 里的那个
//   void f() { shared = 20; }
//
// 【自己动手要点】
// 1) 全局变量在头文件里加 inline 也能共享（C++17），见 sdl_renderer.h 里的
//    inline SDL_Window* window —— 那是不用 extern 的现代写法；
// 2) 全局变量要慎用：多线程读写要加锁或用原子变量（后面有概念卡）。
uint32_t WIDTH;        // 覆盖层窗口的宽度（像素）
uint32_t HEIGHT;       // 覆盖层窗口的高度（像素）
uint32_t WINDOW_W;     // 游戏窗口宽度
uint32_t WINDOW_H;     // 游戏窗口高度
int32_t VIEWPORT_X;    // 视口（游戏画面）在屏幕上的左上角 X 坐标
int32_t VIEWPORT_Y;    // 视口左上角 Y 坐标
uint32_t VIEWPORT_W;   // 视口宽度
uint32_t VIEWPORT_H;   // 视口高度

// ----------------------------------------------------------------------------
// 🎓 概念卡 ③：const 与 constexpr（"不会变"与"编译期就算好"）
// ----------------------------------------------------------------------------
// 【是什么】
// - const：承诺"这个变量一旦赋值就不会再改"。给变量加 const，编译器会在
//   你误改时报错，也让读代码的人放心"它不会变"。写法：const 类型 名字。
// - constexpr：比 const 更强——值在**编译期**就能算出来（比如
//   constexpr int SIZE = 60 * 4; 编译时就等于 240，运行时零开销）。
// - const 引用（const T&）：只读地借用别人，不拷贝（省内存）、不改原值，
//   这是 C++ 里最常用的传参方式（见概念卡 ⑦）。
// 【为什么在这里】dpiScale（界面缩放系数）算出来后就不会变了，所以 const；
// 菜单、渲染处处用它，保证没人能悄悄改掉它。
// 【最小示例】
//   const int maxPlayers = 64;      // 编译期常量，且不可改
//   constexpr double pi = 3.14159;  // 编译期就算好的常量
//   int readOnly(const int& v) { return v; }  // 只读借用，不拷贝
// 【自己动手要点】
// 1) 能用 const 就加 const——"默认不可变"是 C++ 好代码的标记；
// 2) constexpr 适合"固定不变且要频繁用"的值，比 #define 宏更类型安全；
// 3) 指针的 const 有两种：const int* p（指向的值不可改）vs int* const p
//    （指针本身不可改），初学者先记住 const 修饰谁谁就不能改。
// ----------------------------------------------------------------------------
// renderWaitingScreen：绘制"等待连接"界面
// ----------------------------------------------------------------------------
// 🎓 概念卡 ④：ImGui 的使用模式（立即模式 GUI）
// ----------------------------------------------------------------------------
// 【是什么】ImGui（Dear ImGui）是"立即模式"界面库：没有"控件对象"，
// 你只是**每一帧**都调用画控件的函数，它根据返回值告诉你用户做了什么。
// 核心模式是固定三件套：
//   ImGui::Begin("窗口标题")  → 开始一个窗口
//   ...ImGui::Text/Checkbox/Slider...  → 在窗口里画控件
//   ImGui::End()              → 结束窗口
// 每帧重复执行，界面就"活"了（用户勾选 → 下一帧 Checkbox 显示勾选状态）。
//
// 【为什么在这里】整个项目的界面（等待界面、设置菜单、性能面板）都是
// ImGui 画的。看懂这个函数，菜单模块（menu.hpp）就全通了。
//
// 【最小示例】
//   ImGui::Begin("My Window");
//   static bool on = false;
//   ImGui::Checkbox("开关", &on);      // 用户点击会改 on 的值
//   if (on) ImGui::Text("ON");         // 立即反映
//   ImGui::End();
//
// 【自己动手要点】
// 1) ImGui 控件改的是你传入变量的值（注意传地址 &on），不是返回新值；
// 2) 颜色是 0~1 的浮点 RGBA（ImVec4(红,绿,蓝,不透明度)），不是 0~255；
// 3) 设置窗口位置/大小要在 Begin 之前用 SetNextWindowPos/Size（本项目
//    下面就是这么做的），并配合 ImGuiCond_Always 强制每帧生效。
// ----------------------------------------------------------------------------
// 参数 dotCount：动画用的点数（"Waiting for Counter-Strike 2." 后面的点会跳动）
void renderWaitingScreen(int dotCount)
{
    // DPI 缩放系数：高分屏（如 4K）上字体和窗口需要放大，否则太小看不清
    const float dpiScale = sdl_renderer::getDpiScale();

    // 窗口与屏幕边缘之间留出的边距（至少 8 像素，按 DPI 缩放）
    const float margin = std::max(8.0f, 16.0f * dpiScale);

    // 等待窗口的宽度：最多 480 像素（乘 DPI），但不超过屏幕宽度减去边距
    const float windowWidth = std::min(
        480.0f * dpiScale,
        std::max(1.0f, static_cast<float>(WIDTH) - margin * 2.0f));

    // 等待窗口的高度：最多 270 像素
    const float windowHeight = std::min(
        270.0f * dpiScale,
        std::max(1.0f, static_cast<float>(HEIGHT) - margin * 2.0f));

    // 下面几行是 ImGui 的 API：设置窗口位置（屏幕正中）和大小
    // ImVec2(x, y) 只是一个"包含两个 float"的小结构体，用来表示坐标
    ImGui::SetNextWindowPos(
        ImVec2(
            WIDTH / 2.0f - windowWidth / 2.0f,   // 屏幕宽/2 - 窗口宽/2 = 水平居中
            HEIGHT / 2.0f - windowHeight / 2.0f  // 垂直居中
        ),
        ImGuiCond_Always                        // 每次帧都强制这个位置
    );

    ImGui::SetNextWindowSize(
        ImVec2(windowWidth, windowHeight),
        ImGuiCond_Always
    );

    // 开始一个 ImGui 窗口。窗口标题是 "Aegis // Connection"
    // ImGuiWindowFlags_NoResize 等：禁止用户缩放/拖动/折叠这个窗口
    ImGui::Begin("Aegis // Connection", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    {
        // 记录这个窗口在屏幕上的位置，告诉 sdl_renderer：鼠标在这块区域
        // 应该被拦截（否则鼠标会直接穿过覆盖层点到下面的游戏）
        const ImVec2 position = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        sdl_renderer::setInteractiveRect(
            position.x,
            position.y,
            size.x,
            size.y);
    }

    // 生成跳动的点：dotCount 个点，比如 "..."
    char dots[4] = {0};
    for (int i = 0; i < dotCount; i++) dots[i] = '.';

    // 绘制文字（带颜色）。ImGui 的颜色是 RGBA，范围 0.0~1.0
    ImGui::TextColored(
        ImVec4(0.330f, 0.800f, 1.000f, 1.0f),
        "AEGIS");                                   // 蓝色的大标题 "AEGIS"
    ImGui::SameLine();                              // 让下一个元素紧跟在同一行
    ImGui::TextColored(
        ImVec4(0.500f, 0.570f, 0.670f, 1.0f),
        "CS2 OVERLAY");                             // 灰色副标题
    ImGui::Spacing();                               // 加一点垂直间距
    ImGui::Separator();                             // 画一条分隔线
    ImGui::Dummy(ImVec2(0.0f, 12.0f * dpiScale));   // 留白
    ImGui::TextColored(
        ImVec4(0.930f, 0.960f, 1.000f, 1.0f),
        "Waiting for Counter-Strike 2%s",           // %s 被后面的 dots 替换
        dots);
    ImGui::TextColored(
        ImVec4(0.500f, 0.570f, 0.670f, 1.0f),
        "The client and its active monitor will be detected automatically.");

    ImGui::Dummy(ImVec2(0.0f, 14.0f * dpiScale));
    // 绘制一个"子窗口"（带边框的背景框），用来放提示文字
    ImGui::PushStyleColor(
        ImGuiCol_ChildBg,
        ImVec4(0.070f, 0.090f, 0.125f, 1.0f));      // 背景色：深蓝黑
    ImGui::PushStyleColor(
        ImGuiCol_Border,
        ImVec4(0.145f, 0.185f, 0.240f, 1.0f));      // 边框色
    ImGui::BeginChild(
        "##WaitingHint",
        ImVec2(0.0f, 90.0f * dpiScale),
        true);                                       // true = 带边框
    ImGui::TextColored(
        ImVec4(0.930f, 0.650f, 0.260f, 1.0f),
        "DISPLAY MODE");                             // 橙色小标题
    ImGui::TextWrapped(
        "Use Fullscreen Windowed in CS2. The overlay will then map "
        "the game viewport to the correct monitor and aspect ratio.");
    ImGui::EndChild();
    ImGui::PopStyleColor(2);                         // 恢复刚才保存的 2 个颜色设置

    ImGui::Dummy(ImVec2(0.0f, 8.0f * dpiScale));
    ImGui::TextColored(
        ImVec4(0.500f, 0.570f, 0.670f, 1.0f),
        "Checking every 3 seconds  |  Press F9 to exit");

    ImGui::End();                                    // 结束这个 ImGui 窗口
}

// 匿名命名空间：这里的类/函数只在当前文件内可见（相当于其他语言的"私有"）
// ----------------------------------------------------------------------------
// 🎓 概念卡 ⑤：命名空间 namespace（给名字分组）
// ----------------------------------------------------------------------------
// 【是什么】
// 命名空间把名字放进"小组"里，避免冲突。比如两个库都有 init() 函数，
// 用 namespace 包起来后就是 sdl_renderer::init() 和 esp::init()，互不干扰。
//
// 【这里的"匿名命名空间"是什么】
// `namespace { ... }` 不给名字，里面的东西"只在本文件可见"——
// 相当于把一组辅助函数标记为"私有"，别的文件用不到。这样即使两个文件
// 都定义了同名的辅助函数，也不会打架。
//
// 【最小示例】
//   namespace math {
//       int add(int a, int b) { return a + b; }
//   }
//   namespace game {
//       int add(int a, int b) { return a + b + 1; }  // 不冲突
//   }
//   int main() { return math::add(1, 1); }  // 用 :: 指定是哪个组
//
// 【自己动手要点】
// 1) 访问命名空间里的东西用 名字::成员；
// 2) `using namespace xxx;` 可以免写前缀，但大项目里容易冲突，慎用；
// 3) 本项目的分层（menu::、esp::、web_radar::…）就是靠 namespace 组织起来的。
// ----------------------------------------------------------------------------
namespace
{
    // ------------------------------------------------------------------------
    // ScopedHandle：Windows 句柄的"自动关闭器"（RAII 习惯用法）
    // ------------------------------------------------------------------------
    // 🎓 概念卡 ⑥：类、构造函数、析构函数 与 RAII
    // ------------------------------------------------------------------------
    // 【类（class）是什么】
    // 类 = 数据 + 操作这些数据的函数，打包成一个"类型"。比如下面这个类
    // 打包了"一个句柄"和"关闭它的逻辑"。对象的创建叫"构造"（构造函数，
    // 名字 = 类名），销毁叫"析构"（析构函数，名字 = ~类名，无返回值）。
    //
    // 【什么是 RAII（读作"rai-i"）】
    // Resource Acquisition Is Initialization：资源（文件、句柄、锁…）
    // 在构造时拿到、在析构时自动释放。好处：不用记得手动释放，函数中途
    // return 或抛异常也不会漏——对象一出作用域，析构函数必然执行。
    //
    // 【为什么在这里】Windows 的句柄（HANDLE）用完必须 CloseHandle，
    // 忘关就泄漏。用 RAII 包装后，ScopedHandle 一离开作用域就自动关闭，
    // 永远不泄漏。
    //
    // 【最小示例】
    //   class File {
    //   public:
    //       File(const char* path) { handle_ = fopen(path, "r"); }  // 构造：拿资源
    //       ~File() { if (handle_) fclose(handle_); }               // 析构：释放
    //   private:
    //       FILE* handle_ = nullptr;
    //   };
    //   void demo() { File f("a.txt"); }  // 函数结束，f 自动关闭文件
    //
    // 【自己动手要点】
    // 1) 构造函数可以有参数（这里是 handle），析构函数不能有参数；
    // 2) 复制会导致"两个对象关同一个资源"→ 用 `= delete` 禁止复制
    //    （本项目里所有 RAII 类都这么干，注意看）；
    // 3) 写任何"需要手动清理"的类时，都应该用 RAII 而不是靠人记得清理。
    // ------------------------------------------------------------------------
    // 什么是句柄（HANDLE）？在 Windows 里，打开一个进程/文件会得到一个"句柄"，
    // 它像一个"通行证号码"，用完必须调用 CloseHandle 归还，否则就泄漏了。
    // ScopedHandle 的作用：把"归还"这个动作绑定到对象的生命周期——对象销毁时自动归还。
    // 这叫 RAII（资源获取即初始化）：资源跟着对象走，对象没了资源就释放。
    // 这样程序员就不用记得手动关闭，就算中途出错也不会漏。
    class ScopedHandle
    {
    public:
        // 构造函数：传入句柄（默认是空指针 nullptr，表示没有句柄）
        explicit ScopedHandle(HANDLE handle = nullptr)
            : handle_(handle)      // 把传入的 handle 存到成员变量 handle_ 里
        {
        }

        // 析构函数：对象销毁时自动执行。有句柄就关闭它。
        // （析构函数名字是 ~类名，构造函数没有返回值）
        ~ScopedHandle()
        {
            if (handle_) {
                CloseHandle(handle_);   // 关闭句柄（Windows API）
            }
        }

        // 删除拷贝构造函数和拷贝赋值：
        // 防止这个对象被"复制"（因为复制后两个对象会关同一个句柄，导致重复关闭）
        // 后面的 = delete 表示"禁止这个函数存在"
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;

        // 取值函数：把里面的句柄取出来用（getter）
        HANDLE get() const
        {
            return handle_;
        }

    private:
        HANDLE handle_ = nullptr;   // 成员变量：保存的句柄，默认空
    };

    // ------------------------------------------------------------------------
    // hasArgument：检查命令行参数里有没有某个参数
    // ------------------------------------------------------------------------
    // argc：参数的个数；argv：每个参数的字符串数组。
    // 例如程序被这样启动：external-cheat-base.exe --allow-memory-writes
    // 那么 argv[0]="external-cheat-base.exe"，argv[1]="--allow-memory-writes"
    bool hasArgument(
        int argc,
        char* argv[],
        const char* expected)
    {
        for (int index = 1; index < argc; ++index) {   // 从 1 开始，跳过程序名本身
            if (_stricmp(argv[index], expected) == 0) { // _stricmp 忽略大小写比较字符串
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------------
    // showFatalError：显示一个"致命错误"弹窗，同时写入日志
    // ------------------------------------------------------------------------
    // MessageBoxW 是 Windows 弹窗函数。L"..." 是宽字符字符串（每个字符 2 字节）。
    void showFatalError(const wchar_t* message)
    {
        diagnostics::log(message);                 // 写入日志文件
        MessageBoxW(
            nullptr,                               // 没有父窗口（独立弹窗）
            message,                               // 弹窗里显示的文字
            L"CS2 ESP",                            // 弹窗标题
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND); // 只有"确定"按钮 + 错误图标
    }

    // ------------------------------------------------------------------------
    // makeViewerToken：生成一个随机的 32 字符访问令牌（token）
    // ------------------------------------------------------------------------
    // 用途：网页雷达的 URL 里要带 token，没有正确 token 就看不了雷达。
    // 每次启用网页雷达都会重新生成，旧链接立刻失效，防止别人偷看。
    // 实现：用 Windows 的加密随机数生成器 BCryptGenRandom 生成 32 字节随机数，
    // 再把每个字节映射到 64 个"URL 安全字符"之一（0-9 A-Z a-z - _）。
    std::string makeViewerToken()
    {
        // 允许出现在 token 里的字符集合（URL 安全，不会被网址解析吃掉）
        static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789-_";
        // 编译期断言：这个字母表正好 64 个字符（后面要用 & 63 取余，64 的倍数才行）
        static_assert(sizeof(alphabet) - 1 == 64);

        // 32 字节的随机数缓冲。std::array<unsigned char, 32> = 定长数组
        std::array<unsigned char, 32> entropy{};
        // 调用 Windows 加密随机数生成器，把 32 字节真随机数填进 entropy
        const NTSTATUS result = BCryptGenRandom(
            nullptr,                                          // 使用默认算法提供者
            entropy.data(),                                   // 输出缓冲的起始地址
            static_cast<ULONG>(entropy.size()),               // 要生成的字节数
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);                 // 使用系统推荐的随机源
        if (result != 0) {                                    // 返回值非 0 表示失败
            throw std::runtime_error(                          // 抛异常给上层处理
                "Windows could not generate a Web Radar access token");
        }

        // 先把 32 个字符都填成 'A'（占位），下面逐个替换
        std::string token(32, 'A');
        for (std::size_t index = 0; index < token.size(); ++index) {
            // 取随机字节的低 6 位（0~63），作为字母表的下标
            token[index] = alphabet[entropy[index] & 63U];
        }
        return token;
    }

    // ------------------------------------------------------------------------
    // webRadarDocumentRoot：找到网页雷达前端文件的目录
    // ------------------------------------------------------------------------
    // 网页雷达是一个浏览器网页（React 打包后的 dist 文件夹）。
    // 它和 exe 放在一起：exe 所在目录 / web-radar / dist。
    // 这里用 Windows API 拿到当前 exe 的完整路径，再推出 dist 的路径。
    std::string webRadarDocumentRoot()
    {
        // 用来接收 exe 路径的宽字符缓冲区（32768 个字符，足够长）
        std::array<wchar_t, 32768> executablePath{};
        // GetModuleFileNameW：拿到当前程序的 exe 完整路径
        const DWORD length = GetModuleFileNameW(
            nullptr,                              // 传 nullptr = 获取"当前这个程序"的路径
            executablePath.data(),                // 输出缓冲
            static_cast<DWORD>(executablePath.size()));
        // 如果拿不到路径（length == 0）或缓冲区太小，就返回一个相对路径作为兜底
        if (length == 0 || length >= executablePath.size()) {
            return "web-radar/dist";
        }
        // 用 std::filesystem 取出 exe 所在目录，再拼上 "web-radar" / "dist"
        // parent_path() = 去掉文件名只留目录
        return (
            std::filesystem::path(
                std::wstring_view(executablePath.data(), length))
                .parent_path() /
            L"web-radar" /
            L"dist").string();
    }

    // ------------------------------------------------------------------------
    // teamViewPolicy：把菜单里的数字选项翻译成"看哪些队伍"的策略
    // ------------------------------------------------------------------------
    // 菜单里用户可选：全部队伍 / 只看本队 / 只看对方。这里做转换。
    game::web_radar_json::TeamViewPolicy teamViewPolicy(
        const int value) noexcept                    // noexcept：这个函数不会抛异常
    {
        switch (value) {
        case 1:
            return game::web_radar_json::TeamViewPolicy::localTeamOnly;   // 只看本队
        case 2:
            return game::web_radar_json::TeamViewPolicy::opponentsOnly;   // 只看对方
        default:
            return game::web_radar_json::TeamViewPolicy::all;             // 全部
        }
    }

    // ------------------------------------------------------------------------
    // serializeSnapshotCached：把游戏快照转成 JSON 字符串，并做"缓存"
    // ------------------------------------------------------------------------
    // 为什么缓存？同一份快照可能被多个出口用（本机雷达、公网中继、录制）各序列化一遍，
    // 很浪费 CPU。这里用一个只有 4 格的"小缓存"：如果这次要序列化的快照和上次一样，
    // 就直接返回上次的结果。
    // 参数 snapshot：游戏快照（smart pointer，可能为空）；options：序列化选项（是否带名字等）
    std::shared_ptr<const std::string> serializeSnapshotCached(
        const esp::GameSnapshot& snapshot,
        const game::web_radar_json::SerializationOptions& options)
    {
        // ------------------------------------------------------------------
        // 🎓 概念卡 ⑦：引用（References，& 符号）
        // ------------------------------------------------------------------
        // 【是什么】引用是"另一个变量的别名"：int& r = x; 之后 r 和 x 就是
        // 同一个变量。引用不拷贝数据，只"指向"原数据。最常见的用法是**传参**：
        //   void f(const Config& cfg);   // 不拷贝 Config（省内存），且只读
        // 对比：void f(Config cfg);      // 整个拷贝一份（慢）
        //       void f(Config& cfg);     // 可改原值（想改才这么写）
        // 【为什么在这里】这里是最早出现引用的地方：snapshot 和 options 都是
        // const 引用参数——快照很大，按引用传就不用每帧拷贝一份，加 const
        // 保证只读。全项目到处是 const T& 就是这个原因。
        // 【最小示例】
        //   struct Big { int data[1000]; };
        //   void read(const Big& b) { /* 只读，零拷贝 */ }
        //   void modify(Big& b) { b.data[0] = 1; }   // 改原对象
        //   int main() { Big b; read(b); modify(b); }
        // 【自己动手要点】
        // 1) 引用必须初始化且不能"换绑"（不像指针可以指向别的）；
        // 2) 区分 & 的三种含义：声明引用（int& r）、取地址（&x）、按位与（a & b）；
        // 3) 规则："函数参数用 const T&（只读）、T&（要改）、T（小对象值传递）"。
        // ------------------------------------------------------------------
        // 🎓 概念卡 ⑧：std::shared_ptr 智能指针（多线程共享的"自动释放"指针）
        // ------------------------------------------------------------------
        // 【是什么】普通指针 int* p 需要手动 delete。shared_ptr 是"自动释放"的
        // 指针：它内部记录"还有多少人在用我"（引用计数），最后一个使用的人
        // 离开时自动 delete。多个线程可以同时持有同一份数据的 shared_ptr，
        // 谁最后用完谁负责释放——完全不用人管。
        // 【为什么在这里】快照要被数据线程写、渲染线程读，还可能同时被网页
        // 服务、中继、录制器用。用 shared_ptr<const ...> 传递"只读共享快照"，
        // 大家放心引用，最后一个用完自动释放，不用手动管理生命周期。
        // 【最小示例】
        //   std::shared_ptr<int> a = std::make_shared<int>(42);
        //   std::shared_ptr<int> b = a;   // 两个指针指向同一份数据
        //   *a = 100;                     // b 看到的也是 100
        //   // 函数结束，a、b 都销毁，最后一个销毁时自动释放内存
        // 【自己动手要点】
        // 1) 永远用 std::make_shared 创建，别用 new（更安全更快）；
        // 2) shared_ptr<const T> 表示"只读共享"：拿到后不能改内容，适合快照；
        // 3) 只读共享 + "换新快照"是避免加锁的经典技巧：写线程不做修改，
        //    而是换一个新的 shared_ptr，读线程继续用旧的，天然安全。
        // ------------------------------------------------------------------
        // 快照为空（游戏还没读到数据）就直接返回空指针
        if (!snapshot) {
            return {};
        }
        // CacheEntry：一条缓存记录 = 快照 + 选项 + 上次的序列化结果
        struct CacheEntry
        {
            esp::GameSnapshot snapshot;
            game::web_radar_json::SerializationOptions options;
            std::shared_ptr<const std::string> payload;
        };
        // thread_local：每个线程各自一份缓存（多线程各用各的，不用加锁）
        // std::array<CacheEntry, 4>：4 格的环形缓存
        thread_local std::array<CacheEntry, 4> entries;
        thread_local std::size_t nextEntry = 0;      // 下一个要覆盖的位置（环形用）

        // 先翻一遍缓存：如果发现一模一样的快照+选项，直接返回缓存结果
        for (const CacheEntry& entry : entries) {
            if (entry.snapshot == snapshot && entry.options == options &&
                entry.payload) {                     // payload 非空才有效
                return entry.payload;
            }
        }

        // 没命中缓存，开始真正序列化，并记录耗时
        const auto serializationStarted =
            std::chrono::steady_clock::now();        // 记录开始时刻
        // 调用 web_radar_json 的序列化函数，结果用 shared_ptr 包装（可被多个线程共享读）
        auto payload = std::make_shared<const std::string>(
            game::web_radar_json::serializeSnapshotV1(*snapshot, options));
        // 把这次序列化耗时记入性能统计
        performance_metrics::serializationDuration.record(
            std::chrono::steady_clock::now() - serializationStarted);
        // 把结果放进缓存（覆盖最旧的一格，环形推进）
        entries[nextEntry] = CacheEntry{snapshot, options, payload};
        nextEntry = (nextEntry + 1) % entries.size();
        return payload;
    }

    // ------------------------------------------------------------------------
    // WebRadarRuntime：管理"本机网页雷达"Web 服务的生命周期
    // ------------------------------------------------------------------------
    // 职责：用户打开/关闭网页雷达开关时，启动或停止内嵌的 Web 服务；
    //       定时把最新快照推送给网页；把服务状态同步给菜单界面显示。
    class WebRadarRuntime final
    {
    public:
        // 构造函数：记录网页文件目录
        WebRadarRuntime()
            : documentRoot_(webRadarDocumentRoot())
        {
        }

        // 析构函数：程序退出时确保服务被停止（清理干净）
        ~WebRadarRuntime()
        {
            stop();
        }

        // --------------------------------------------------------------------
        // sync：把菜单里的配置同步到实际运行的服务上
        // 每次渲染循环都会调用它。它对比"想要的配置"和"当前的配置"，
        // 有变化才真正启停服务（避免反复重启）。
        void sync(const menu::RuntimeConfig& config)
        {
            // 从用户配置里取出三个我们关心的值
            const Settings desired{
                config.webRadarEnabled,      // 网页雷达开关
                config.webRadarLanAccess,    // 是否允许局域网内其他电脑访问
                config.webRadarPort          // 端口（默认 22006）
            };
            // 如果当前正在运行的服务配置和想要的完全一样，就只刷新一下状态显示，返回
            if (settings_ && *settings_ == desired) {
                publishUiStatus();
                return;
            }

            // rotateToken：从"关闭"变为"开启"时，要重新生成 token（防止旧链接还能用）
            const bool rotateToken =
                desired.enabled &&
                (!settings_ || !settings_->enabled);
            settings_ = desired;             // 记住新的期望配置
            detachAndStop();                 // 先停掉旧服务（如果有）
            lastError_.clear();              // 清空上次的错误信息

            // 用户把开关关掉了：不需要启动服务，刷新状态即可
            if (!desired.enabled) {
                publishUiStatus(true);
                return;
            }

            // 需要启动服务。用 try/catch 包住，出错不崩溃而是记录错误
            try {
                // 如果需要轮换 token 或还没有 token，就生成一个新 token
                if (rotateToken || token_.empty()) {
                    token_ = makeViewerToken();
                }
                // 组装服务配置
                web_radar::WebRadarConfig serviceConfig;
                serviceConfig.bindAddress = desired.lanAccess
                    ? "0.0.0.0"          // 监听所有网卡（局域网可访问）
                    : "127.0.0.1";       // 只监听本机（只有自己能访问）
                serviceConfig.port = desired.port;
                serviceConfig.documentRoot = documentRoot_;  // 网页文件目录
                serviceConfig.token = token_;                // 访问令牌

                // 创建服务对象。shared_ptr 智能指针：
                // 多个地方（主循环、这里）都能安全地引用同一个服务，没人用时自动销毁
                auto service = std::make_shared<
                    web_radar::WebRadarService>(
                        std::move(serviceConfig));
                // 启动服务。失败则记录错误并停止
                if (!service->start()) {
                    lastError_ = service->status().lastError;
                    service->stop();
                    publishUiStatus(true);
                    return;
                }

                // 加锁后把新服务保存到成员变量 service_（锁防止另一个线程同时读）
                // ------------------------------------------------------------------
                // 🎓 概念卡 ⑨：std::mutex 互斥锁 与 std::lock_guard
                // ------------------------------------------------------------------
                // 【是什么】多线程同时读写同一份数据会"数据竞争"（读到改了一半的值）。
                // mutex（互斥锁）保证"同一时刻只有一个线程能进入这段代码"。
                // lock_guard 是 RAII 版的锁：构造时自动加锁，离开作用域自动解锁，
                // 忘了解锁也不会死锁。
                // 【最小示例】
                //   std::mutex m;            // 一把锁
                //   std::vector<int> data;   // 共享数据
                //   void push(int v) {
                //       std::lock_guard<std::mutex> lock(m); // 加锁（自动）
                //       data.push_back(v);                   // 安全操作
                //   }                                        // 离开作用域自动解锁
                // 【自己动手要点】
                // 1) 锁的粒度要小：只在真正操作共享数据时加锁，别把无关代码包进去；
                // 2) 锁的顺序要一致，否则两个线程互相等对方的锁会"死锁"；
                // 3) 本项目多数地方用"快照"（shared_ptr 换指针）来少用锁，见概念卡 ⑩。
                // ------------------------------------------------------------------
                std::lock_guard<std::mutex> lock(serviceMutex_);
                service_ = std::move(service);
            } catch (const std::exception& error) {
                // 捕获所有异常，把错误信息记下来（不崩溃）
                lastError_ = error.what();
            }
            publishUiStatus(true);
        }

        // --------------------------------------------------------------------
        // publish：把一帧游戏快照推送给所有网页观众
        // --------------------------------------------------------------------
        // includePlayerNames / includeSteamIds / teamPolicy：
        // 隐私相关的三个开关，决定发给网页的数据里带不带玩家名、Steam ID、哪些队伍
        void publish(
            const esp::GameSnapshot& snapshot,
            const bool includePlayerNames,
            const bool includeSteamIds,
            const int teamPolicy)
        {
            if (!snapshot) {                 // 快照为空就没什么可发的
                return;
            }

            // 取当前服务（加锁拷贝出 shared_ptr，避免持有锁时间过长）
            std::shared_ptr<web_radar::WebRadarService> service;
            {
                std::lock_guard<std::mutex> lock(serviceMutex_);
                service = service_;
            }
            if (!service || !service->isRunning()) {  // 服务没在跑就跳过
                return;
            }

            // 组装序列化选项（隐私过滤），然后序列化并推给服务
            game::web_radar_json::SerializationOptions options;
            options.includePlayerNames = includePlayerNames;
            options.includeSteamIds = includeSteamIds;
            options.teamViewPolicy = teamViewPolicy(teamPolicy);
            service->publish(serializeSnapshotCached(snapshot, options));
        }

        // 停止服务并清空状态
        void stop()
        {
            settings_.reset();
            detachAndStop();
            publishUiStatus(true);
        }

        // 当前有多少个网页观众正在看（给数据线程决定采样频率用）
        [[nodiscard]] std::size_t viewerCount() const noexcept
        {
            return viewerCount_.load(std::memory_order_relaxed);
        }

    private:
        // 服务配置（内部用的小结构体）
        struct Settings
        {
            bool enabled = false;      // 是否启用
            bool lanAccess = false;    // 是否允许局域网访问
            uint16_t port = 22006;     // 端口号
            // 让两个 Settings 可以直接用 == 比较（C++20 特性）
            bool operator==(const Settings&) const = default;
        };

        // 停止并放弃当前服务（把 service_ 换出来，让最后持有的人去销毁）
        void detachAndStop()
        {
            std::shared_ptr<web_radar::WebRadarService> service;
            {
                std::lock_guard<std::mutex> lock(serviceMutex_);
                service.swap(service_);      // service 和 service_ 交换
            }
            if (service) {                   // 有旧服务才需要停
                service->stop();
            }
        }

        // 把服务运行状态（运行中/观众数/错误等）同步到菜单界面。
        // force=true 强制刷新；否则 250ms 最多刷新一次（避免刷屏）
        void publishUiStatus(bool force = false)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!force && now < nextUiStatusPublish_) {
                return;
            }
            nextUiStatusPublish_ = now + std::chrono::milliseconds(250);

            menu::WebRadarUiStatus ui;       // 菜单要显示的状态结构体
            ui.bindAddress = settings_ && settings_->lanAccess
                ? "0.0.0.0"
                : "127.0.0.1";
            ui.error = lastError_;

            std::shared_ptr<web_radar::WebRadarService> service;
            {
                std::lock_guard<std::mutex> lock(serviceMutex_);
                service = service_;
            }
            if (service) {
                // 从服务里取出各种统计数字，填进 UI 状态
                const web_radar::WebRadarStatus status = service->status();
                ui.running = status.isRunning();
                ui.viewers = status.viewerCount;
                ui.publishedFrames = status.publishedFrames;    // 已发布的帧数
                ui.sentFrames = status.sentFrames;              // 已发送给观众的帧数
                ui.replacedFrames = status.replacedFrames;      // 被新帧顶掉的旧帧数
                ui.publishedBytes = status.publishedBytes;      // 发布的字节数
                ui.maximumSendLatencyMilliseconds =
                    status.maximumSendLatencyMilliseconds;      // 最大发送延迟
                ui.bindAddress = status.bindAddress;
                if (!status.lastError.empty()) {
                    ui.error = status.lastError;
                }
                // 拼出观众访问的 URL：http://127.0.0.1:端口/?token=令牌
                if (ui.running) {
                    ui.viewerUrl =
                        "http://127.0.0.1:" +
                        std::to_string(status.port) +
                        "/?token=" + token_;
                }
            }
            // 把观众数存进原子变量（数据线程会读它来决定采样频率）
            viewerCount_.store(ui.viewers, std::memory_order_relaxed);
            // 交给菜单模块显示
            menu::setWebRadarStatus(std::move(ui));
        }

        // 成员变量（类的"内部数据"）
        std::mutex serviceMutex_;                        // 保护 service_ 的锁
        std::shared_ptr<web_radar::WebRadarService> service_;  // 当前服务
        std::optional<Settings> settings_;               // 当前的期望配置（可能还没有）
        std::string token_;                              // 当前访问令牌
        std::string documentRoot_;                       // 网页文件目录
        std::string lastError_;                          // 最近一次错误信息
        std::chrono::steady_clock::time_point nextUiStatusPublish_{}; // 下次允许刷新状态的时间
        // ------------------------------------------------------------------
        // 🎓 概念卡 ⑩：std::atomic 原子变量（无锁的安全计数器/开关）
        // ------------------------------------------------------------------
        // 【是什么】普通 int 在多线程里 a++ 可能"丢失更新"（两个线程同时读到 0，
        // 各自 +1 后都写回 1，实际应该 +2）。atomic 保证这些操作"不可分割"：
        // 读、写、加减都是原子的，多线程用完全安全，而且比加锁更快。
        // 【为什么在这里】viewerCount_（观众数）被数据线程读（决定采样频率）、
        // 渲染线程写（刷新状态）。用 atomic 就不用为这一个数字加锁。
        // 【最小示例】
        //   std::atomic<int> count{0};
        //   count.fetch_add(1);          // 线程安全地 +1
        //   int v = count.load();        // 线程安全地读
        // 【自己动手要点】
        // 1) memory_order_relaxed 是最宽松的内存序（只保证原子性，不保证顺序），
        //    适合计数器；需要"同步可见性"时用 acquire/release 或直接默认值；
        // 2) atomic 只对"单个变量"安全，多变量组合操作还是得用锁。
        // ------------------------------------------------------------------
        std::atomic<std::size_t> viewerCount_{0};        // 观众数（多线程安全的计数器）
    };

    // ------------------------------------------------------------------------
    // 下面是"公网中继"的异常处理辅助机制（代码较底层，初学者先了解用途即可）
    // ------------------------------------------------------------------------
    // 公网中继连接在停止时可能需要等网络线程收尾（最多几秒）。
    // 正常情况下由专门线程去"收尸"（retire），但如果系统连线程都创建不出来，
    // 就把 producer 保存在下面这块"永久内存"里，等进程退出时由 Windows 统一回收。
    // 这样 UI 线程永远不会因为等待网络而卡死。

    // AbandonedPublicRelayProducerSlot：一个"保管位"，装着一个互斥锁和一个 producer
    struct AbandonedPublicRelayProducerSlot final
    {
        std::mutex mutex;
        std::shared_ptr<web_radar::PublicRelayProducer> producer;
    };

    // 下面是在静态内存里"放置构造"一个保管位。
    // 普通静态对象销毁顺序不可控，这里故意不用静态对象，而是手动在内存块里构造，
    // 让它在整个进程生命周期内都有效（永远不会被提前销毁）。
    // alignas(...)：按结构体要求的对齐方式分配内存。
    alignas(AbandonedPublicRelayProducerSlot)
        unsigned char abandonedPublicRelayProducerStorage[
            sizeof(AbandonedPublicRelayProducerSlot)]{};
    // 用 placement new 在这块内存上真正构造对象（::new (地址) 类型）
    AbandonedPublicRelayProducerSlot* const
        abandonedPublicRelayProducerSlot = ::new (
            static_cast<void*>(abandonedPublicRelayProducerStorage))
                AbandonedPublicRelayProducerSlot{};

    // 把一个 producer 放进保管位（防止它被销毁时阻塞）
    void retainAbandonedPublicRelayProducer(
        std::shared_ptr<web_radar::PublicRelayProducer> producer) noexcept
    {
        std::lock_guard<std::mutex> lock(
            abandonedPublicRelayProducerSlot->mutex);
        abandonedPublicRelayProducerSlot->producer = std::move(producer);
    }

    // ------------------------------------------------------------------------
    // PublicRelayRuntime：管理"公网中继"连接的生命周期
    // ------------------------------------------------------------------------
    // 职责：用户开启公网共享时，建立一个到 relay 服务器的出站 wss 连接（Producer），
    // 把快照推过去；关闭时安全地断开（让旧连接自己收尾，不阻塞界面）。
    class PublicRelayRuntime final
    {
    public:
        ~PublicRelayRuntime()          // 析构：退出时清理
        {
            stop();
        }

        // sync：对比期望配置和当前配置，需要时创建/重启 producer 连接
        void sync(const menu::RuntimeConfig& config)
        {
            // 期望的配置：开关 + 连接参数（服务器地址、房间、令牌）
            const Settings desired{
                config.publicRelayEnabled,
                config.publicRelayEnabled
                    ? config.publicRelayConnection
                    : std::shared_ptr<
                        const menu::PublicRelayConnectionSettings>{}
            };
            const bool settingsChanged =
                !settings_ || *settings_ != desired;
            if (settingsChanged) {
                settings_ = desired;
                // 只要配置变了，就先"退役"旧连接（requestStop + 后台线程收尾）
                if (!retirementFailed_) {
                    lastError_.clear();
                    beginRetirement();
                }
            }

            // 看看后台收尾线程是否已完成；如果正在收尾或已经失败，就只刷新状态
            consumeCompletedRetirement();
            if (retirementFailed_ || retirement_) {
                publishUiStatus(settingsChanged);
                return;
            }
            if (!settings_ || !settings_->enabled) {
                publishUiStatus(settingsChanged);
                return;
            }

            // 已经有一个 producer 在跑，直接刷新状态
            {
                std::lock_guard<std::mutex> lock(producerMutex_);
                if (producer_) {
                    publishUiStatusLockedProducer(
                        producer_,
                        settingsChanged);
                    return;
                }
            }

            // 需要新建连接
            try {
                // 把用户配置（URL、房间名、token）搬进 relay 需要的结构
                web_radar::PublicRelayConfig relayConfig;
                if (settings_->connection) {
                    const std::string_view endpointUrl =
                        settings_->connection->endpointUrl();
                    relayConfig.endpointUrl.assign(
                        endpointUrl.data(),
                        endpointUrl.size());
                    const std::string_view room =
                        settings_->connection->room();
                    relayConfig.room.assign(
                        room.data(),
                        room.size());
                    const std::string_view token =
                        settings_->connection->token();
                    relayConfig.token.assign(
                        token.data(),
                        token.size());
                }
                // 创建并启动 producer（底层用 WinHTTP 建立 wss 出站连接）
                auto producer = std::make_shared<
                    web_radar::PublicRelayProducer>(
                        std::move(relayConfig));
                const bool started = producer->start();
                if (!started) {
                    lastError_ = producer->status().lastError;
                }
                std::lock_guard<std::mutex> lock(producerMutex_);
                producer_ = std::move(producer);
            } catch (const std::exception& error) {
                lastError_ = error.what();
            }
            publishUiStatus(true);
        }

        // 把一帧快照推给公网中继（和 WebRadarRuntime::publish 类似，只是目标是 relay）
        void publish(
            const esp::GameSnapshot& snapshot,
            const bool includePlayerNames,
            const bool includeSteamIds,
            const int teamPolicy)
        {
            if (!snapshot) {
                return;
            }

            std::shared_ptr<web_radar::PublicRelayProducer> producer;
            {
                std::lock_guard<std::mutex> lock(producerMutex_);
                producer = producer_;
            }
            if (!producer) {
                return;
            }
            // 只在连接处于"运行"状态时才发；禁用/收尾/失败都跳过
            const web_radar::PublicRelayState state =
                producer->status().state;
            if (state == web_radar::PublicRelayState::disabled ||
                state == web_radar::PublicRelayState::retiring ||
                state == web_radar::PublicRelayState::failed) {
                return;
            }

            game::web_radar_json::SerializationOptions options;
            options.includePlayerNames = includePlayerNames;
            options.includeSteamIds = includeSteamIds;
            options.teamViewPolicy = teamViewPolicy(teamPolicy);
            producer->publish(serializeSnapshotCached(snapshot, options));
        }

        void stop()
        {
            settings_.reset();
            consumeCompletedRetirement();
            if (!retirementFailed_) {
                beginRetirement();
            }
            publishUiStatus(true);
        }

    private:
        // 期望配置
        struct Settings
        {
            bool enabled = false;
            std::shared_ptr<const menu::PublicRelayConnectionSettings>
                connection;                      // 连接参数（可能为空）

            bool operator==(const Settings&) const = default;
        };

        // "退役票"：后台收尾线程完成后把 complete 置为 true，用来告诉主线程
        struct RetirementTicket final
        {
            std::atomic<bool> complete{false};
        };

        // 开始退役：requestStop 让 producer 停止接收新数据，
        // 然后另起一个后台线程去真正关闭连接（因为关闭可能要等网络超时，不能阻塞 UI）
        void beginRetirement()
        {
            std::shared_ptr<web_radar::PublicRelayProducer> producer;
            {
                std::lock_guard<std::mutex> lock(producerMutex_);
                producer.swap(producer_);
            }
            if (!producer) {
                return;
            }

            producer->requestStop();
            std::unique_ptr<std::thread> reaper;
            try {
                const auto ticket =
                    std::make_shared<RetirementTicket>();
                // 创建后台线程：在 lambda（匿名函数）里执行停止逻辑
                reaper = std::make_unique<std::thread>(
                    [producer, ticket]() mutable noexcept {
                        producer->stop();
                        producer.reset();            // 释放 producer
                        ticket->complete.store(       // 标记完成
                            true,
                            std::memory_order_release);
                    });
                try {
                    reaper->detach();    // 让后台线程脱离管理（它自己跑完自己结束）
                } catch (...) {
                    // 极端情况：如果 detach 失败，只泄漏线程包装对象，
                    // 既不让析构触发进程终止，也不让 UI 线程去 join 阻塞
                    static_cast<void>(reaper.release());
                    throw;
                }
                reaper.reset();
                retirement_ = ticket;    // 记住这张票，之后轮询它是否完成
            } catch (...) {
                // 连线程都创建不出来：把 producer 放进永久保管位，进入"永久失败"状态
                if (reaper && reaper->joinable()) {
                    static_cast<void>(reaper.release());
                }
                retainAbandonedPublicRelayProducer(
                    std::move(producer));
                retirement_.reset();
                retirementFailed_ = true;
                lastError_ =
                    "Unable to retire the previous Relay connection safely. "
                    "Restart the program before enabling Public Relay again.";
            }
        }

        // 如果退役票显示完成，就清理掉，恢复正常状态
        void consumeCompletedRetirement()
        {
            if (!retirement_ ||
                !retirement_->complete.load(
                    std::memory_order_acquire)) {
                return;
            }
            retirement_.reset();
            lastError_.clear();
        }

        // 把 producer 的状态同步到菜单（锁已持有时的版本）
        void publishUiStatusLockedProducer(
            const std::shared_ptr<
                web_radar::PublicRelayProducer>& producer,
            bool force)
        {
            if (!beginUiStatusPublish(force)) {
                return;
            }
            menu::PublicRelayUiStatus ui;
            const web_radar::PublicRelayStatus status =
                producer->status();
            ui.state = status.state;                 // 连接状态（运行/重连/失败…）
            ui.framesSent = status.framesSent;       // 已发送帧数
            ui.replacedFrames = status.replacedFrames;
            ui.droppedFrames = status.droppedFrames; // 被丢弃的帧数
            ui.reconnects = status.reconnects;       // 重连次数
            ui.error = status.lastError.empty()
                ? lastError_
                : status.lastError;
            menu::setPublicRelayStatus(std::move(ui));
        }

        // 状态刷新节流：250ms 最多刷一次
        bool beginUiStatusPublish(bool force)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!force && now < nextUiStatusPublish_) {
                return false;
            }
            nextUiStatusPublish_ = now + std::chrono::milliseconds(250);
            return true;
        }

        // 把状态同步到菜单（未持锁的版本）
        void publishUiStatus(bool force = false)
        {
            if (!beginUiStatusPublish(force)) {
                return;
            }
            menu::PublicRelayUiStatus ui;
            ui.error = lastError_;
            if (retirementFailed_) {
                ui.state = web_radar::PublicRelayState::failed;
                menu::setPublicRelayStatus(std::move(ui));
                return;
            }
            if (retirement_) {
                ui.state = web_radar::PublicRelayState::retiring;
                menu::setPublicRelayStatus(std::move(ui));
                return;
            }
            std::shared_ptr<web_radar::PublicRelayProducer> producer;
            {
                std::lock_guard<std::mutex> lock(producerMutex_);
                producer = producer_;
            }
            if (producer) {
                const web_radar::PublicRelayStatus status =
                    producer->status();
                ui.state = status.state;
                ui.framesSent = status.framesSent;
                ui.replacedFrames = status.replacedFrames;
                ui.droppedFrames = status.droppedFrames;
                ui.reconnects = status.reconnects;
                if (!status.lastError.empty()) {
                    ui.error = status.lastError;
                }
            }
            menu::setPublicRelayStatus(std::move(ui));
        }

        // 成员变量
        std::mutex producerMutex_;                                   // 保护 producer_ 的锁
        std::shared_ptr<web_radar::PublicRelayProducer> producer_;   // 当前 producer
        std::shared_ptr<RetirementTicket> retirement_;               // 退役票（收尾中则为非空）
        std::optional<Settings> settings_;                           // 期望配置
        std::string lastError_;
        bool retirementFailed_ = false;                              // 是否进入永久失败
        std::chrono::steady_clock::time_point nextUiStatusPublish_{};
    };
}

// ============================================================================
// main 函数：程序的入口。操作系统启动程序时，第一个调用它。
// argc：命令行参数的个数；argv：命令行参数数组（argv[0] 是程序自身路径）
// ----------------------------------------------------------------------------
// 🎓 概念卡 ⑪：main() 与命令行参数
// ----------------------------------------------------------------------------
// 【是什么】
// 每个 C++ 程序的起点都是 main()。操作系统把程序加载进内存后，
// 做的第一件事就是调用它。它返回 int：0 表示"正常退出"，
// 非 0 表示"出错了"（可以被脚本/批处理判断）。
// argc/argv 让你能接收命令行参数，比如：
//   external-cheat-base.exe --allow-memory-writes
// 这时 argc=2，argv[0]="external-cheat-base.exe"，argv[1]="--allow-memory-writes"。
//
// 【为什么在这里】程序需要 --allow-memory-writes 这样的开关（决定是否开放
// 写内存权限），所以 main 第一件事就是解析 argc/argv。
//
// 【最小示例】
//   #include <cstdio>
//   int main(int argc, char* argv[]) {
//       for (int i = 0; i < argc; ++i)
//           std::printf("参数 %d: %s\n", i, argv[i]);
//       return 0;
//   }
//
// 【自己动手要点】
// 1) argc/argv 是 C 风格：argv 是"指针数组"，argv[i] 是第 i 个字符串；
// 2) 在 Windows 上还有 wmain/WinMain 变体（宽字符/图形界面），
//    本项目用的普通 main + FreeConsole 隐藏控制台；
// 3) 现代做法是用库解析（本项目菜单模块自己写了个 hasArgument 手写解析）。
// ============================================================================
int main(int argc, char* argv[])
{
    // ------------------------------------------------------------------------
    // 单实例保护：用一个命名互斥量防止程序被启动两次
    // ------------------------------------------------------------------------
    // CreateMutexW 创建一个"互斥量"。如果已经存在同名互斥量，会返回 ERROR_ALREADY_EXISTS，
    // 说明已经有一个实例在运行了，这时直接提示并退出。
    SetLastError(ERROR_SUCCESS);          // 先清空错误码，好判断下面是否"已存在"
    ScopedHandle instanceMutex(CreateMutexW(
        nullptr,
        FALSE,
        L"Local\\CS2ExternalOverlay-76F5E6C2-235A-4AA0"));
    const DWORD mutexError = GetLastError();
    if (!instanceMutex.get()) {           // 创建失败（非"已存在"）
        showFatalError(
            L"Unable to create the single-instance guard.");
        return -1;
    }
    if (mutexError == ERROR_ALREADY_EXISTS) {  // 已经有一个实例在跑
        MessageBoxW(
            nullptr,
            L"CS2 ESP is already running.",
            L"CS2 ESP",
            MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }

    // 写内存默认开启（魔法子弹/防闪光等功能需要）。想保持纯只读，
    // 用 --no-memory-writes 关闭。--allow-memory-writes 作为历史参数保留（现为无操作）。
    memory::SetWritesAllowed(
        !hasArgument(
            argc,
            argv,
            "--no-memory-writes"));
    // 提高进程优先级（ABOVE_NORMAL）：后台/其它程序占用 CPU 时，叠加层与瞄准线程
    // 仍能优先拿到调度，降低"电脑过载时自瞄延迟变高"。ABOVE_NORMAL 无需管理员权限。
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    // 加载上次保存的设置（记忆用户上次的选择，比如 ESP 开关状态）
    menu::loadPersistentSettings();
    // 加载已录制的枪械弹道模式（录制一次，之后按记录回放）
    recoil_patterns::loadFromDisk();

#ifndef SHOW_CONSOLE
    // 隐藏控制台窗口（图形界面程序一般不需要黑色控制台窗口）
    FreeConsole();
#endif

    // 创建三个"运行时管理器"对象（它们的构造函数会准备状态）
    WebRadarRuntime webRadar;                    // 本机网页雷达
    PublicRelayRuntime publicRelay;              // 公网中继
    web_radar::SnapshotRecorder snapshotRecorder(  // 脱敏录制器
        menu::persistentSettingsPath().parent_path() / L"recordings");
    // 把启动诊断信息（web-radar/dist 是否存在等）交给菜单显示
    menu::setStartupReport(diagnostics::inspectInstallation(
        std::filesystem::path(webRadarDocumentRoot())));

    // ========================================================================
    // 最外层主循环：等游戏 → 附着游戏 → 跑两条线程 → 游戏退出 → 回到等待
    // ========================================================================
    while (sdl_renderer::running)              // running 为 false 表示用户按了 F9 退出
    {
        // 菜单先默认显示
        sdl_renderer::menuVisible = true;

        // ---- 阶段 1：初始化覆盖层窗口与 ImGui ----
        if (!sdl_renderer::initWaiting()) {
            showFatalError(
                L"Unable to initialize the overlay. Windows 10/11 with "
                L"per-monitor DPI awareness and a working SDL2.dll are required.");
            memory::Close();
            return -1;
        }
        if (!sdl_renderer::initImGui()) {
            showFatalError(L"Unable to initialize ImGui.");
            local_fixed_radar::reset();
            sdl_renderer::destroy();
            memory::Close();
            return -1;
        }

        // ---- 阶段 2：等待游戏出现（每 3 秒查一次） ----
        int dotCount = 0;                      // 等待界面动画的跳动点数
        DWORD lastCheckTime = 0;               // 上次检查的时间（毫秒）
        bool gameFound = false;                // 是否找到了游戏
        while (sdl_renderer::running && !gameFound)
        {
            sdl_renderer::pollEvents();        // 处理窗口事件（鼠标、键盘、关闭窗口等）

            // 每 3 秒检查一次游戏是否启动（GetTickCount 返回开机以来的毫秒数）
            const DWORD currentTime = GetTickCount();
            if (currentTime - lastCheckTime >= 3000 ||
                lastCheckTime == 0) {
                lastCheckTime = currentTime;
                dotCount = (dotCount % 3) + 1;  // 让点 1→2→3→1 循环跳动
                if (esp::init()) {              // 尝试找到并打开 cs2.exe 进程
                    gameFound = true;           // 找到了，跳出等待循环
                    break;
                }
            }

            // 渲染一帧等待画面（beginFrame → newFrameImGui → render → endFrame）
            if (!sdl_renderer::beginFrame()) {
                break;
            }
            sdl_renderer::newFrameImGui();
            renderWaitingScreen(dotCount);
            sdl_renderer::renderImGui();
            sdl_renderer::endFrame();
            Sleep(16);                         // 约 60 FPS（16ms 一帧）
        }

        // 关闭等待阶段创建的窗口资源
        sdl_renderer::shutdownImGui();
        local_fixed_radar::reset();
        sdl_renderer::destroy();

        // 用户按了 F9 退出
        if (!sdl_renderer::running) {
            break;
        }

        // ---- 阶段 3：附着游戏，初始化渲染窗口和自动瞄准 ----
        // init(...) 会创建一个透明的覆盖层窗口，叠到游戏窗口上
        if (!gameFound ||
            !sdl_renderer::init(
                L"Counter-Strike 2",
                static_cast<DWORD>(esp::pID)) ||   // esp::pID 是找到的游戏进程 ID
            !sdl_renderer::initImGui() ||
            !aimbot::init()) {
            // 附着失败（比如游戏刚好退出）：清理后稍等重试
            diagnostics::log(
                L"Game attach failed or raced with shutdown; retrying.");
            sdl_renderer::shutdownImGui();
            local_fixed_radar::reset();
            sdl_renderer::destroy();
            esp::clearRuntimeState();
            memory::Close();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500));
            continue;
        }

        // 附着成功：启动独立压枪线程（~500Hz 高频读后坐 + 平滑注入）
        aimbot::startRcsWorker();

        // ====================================================================
        // 启动【数据线程】——负责读游戏内存、跑瞄准/扳机、给雷达出口发数据
        // ====================================================================
        // ------------------------------------------------------------------
        // 🎓 概念卡 ⑫：std::thread 线程 与 lambda 匿名函数
        // ------------------------------------------------------------------
        // 【是什么】
        // std::thread 让一段代码"并行"执行：主线程继续跑，新线程同时跑。
        // lambda（[捕获](参数){ 函数体 }）是"就地定义的匿名函数"，
        // [&dataRunning, &webRadar, ...] 表示"把外面这些变量按引用抓进来用"。
        // 【为什么在这里】程序被拆成两条线程：
        //   - 数据线程：每秒最多 240 次读内存 + 跑瞄准（要快、要准）；
        //   - 主线程（渲染）：每秒 60~240 次画画面（要跟显示器刷新同步）。
        //   如果不拆线程，画画面就会阻塞读内存，瞄准就会卡顿。
        // 【最小示例】
        //   #include <thread>
        //   int main() {
        //       int x = 0;
        //       std::thread t([&x] {      // lambda：抓 x 的引用
        //           for (int i = 0; i < 1000000; ++i) ++x;
        //       });
        //       t.join();                 // 等 t 跑完（重要！忘了会崩溃）
        //       return x;
        //   }
        // 【自己动手要点】
        // 1) 线程用完必须 join()（等它结束）或 detach()（放手），否则程序退出时崩溃；
        // 2) lambda 按引用捕获 [&x] 时，x 的生命周期必须比线程长（本项目用
        //    atomic<bool> dataRunning 控制线程退出，就是这个原因）；
        // 3) 多线程共享数据要配合概念卡 ⑧⑨⑩（shared_ptr 快照 / mutex 锁 / atomic 原子）。
        // ------------------------------------------------------------------
        // 采样频率规则（详见 runtime_timing.hpp）：
        //   - 前台 + 开了瞄准/扳机：240 Hz（对延迟敏感，需要最新数据）
        //   - 前台 + 开了 ESP：跟随显示器刷新率（60~240 Hz，避免画面撕裂）
        //   - 否则：20 Hz 空闲频率
        // 视觉渲染（画框）在渲染线程做，采样只负责"读数据"，两者解耦。
        std::atomic<bool> dataRunning{true};   // 用原子变量通知数据线程何时退出
        performance_metrics::resetSession();   // 重置本轮的性能统计
        memory::ResetReadMetrics();            // 重置内存读取统计（RPM 计数等）
        std::thread dataThread(
            [&dataRunning, &webRadar, &publicRelay, &snapshotRecorder]() {
            // ---- 数据线程主体（lambda 匿名函数） ----
            auto nextDataTime = std::chrono::steady_clock::now();  // 下次采样的计划时间
            auto nextRadarTime = nextDataTime;                     // 下次雷达发布计划时间
            int appliedSamplingRate = 0;          // 当前实际使用的采样频率
            int appliedThreadPriority = 0x7FFFFFFF; // 当前线程优先级（初始为无效值）
            bool stateClearedWhileInactive = false;  // 失焦时是否已清空过状态

            while (dataRunning.load(std::memory_order_relaxed)) {
                // 读一次最新菜单配置（ESP 开没开、瞄准开没开……）
                const menu::RuntimeConfig config =
                    menu::getRuntimeConfig();
                // 游戏是否在前台（覆盖层是否叠在游戏上）
                const bool gameForeground =
                    sdl_renderer::isGameForeground();
                // "延迟敏感" = 游戏在前台、没按暂停键、且瞄准/扳机/魔法子弹/压枪/连跳开着。
                // 魔法子弹需要在"服务器每个 tick 生成 usercmd 前"把视角写到正确角度，
                // 采样频率必须高于 tick 率（240Hz ≈ 4ms 一次），否则很多发子弹赶不上修正。
                const bool latencySensitive =
                    gameForeground &&
                    !config.inputSuppressed &&
                    (config.aimbotEnabled ||
                     config.triggerbotEnabled ||
                     (config.aimbotEnabled && config.aimbotMode == 1) ||
                     config.rcsEnabled ||
                     config.bhopEnabled ||
                     config.autoRotationEnabled);

                // 动态调整线程优先级：
                // 延迟敏感 → 正常优先级；不敏感 → 低于正常（省 CPU）
                const int desiredThreadPriority = latencySensitive
                    ? THREAD_PRIORITY_NORMAL
                    : THREAD_PRIORITY_BELOW_NORMAL;
                if (desiredThreadPriority != appliedThreadPriority &&
                    SetThreadPriority(
                        GetCurrentThread(),
                        desiredThreadPriority)) {
                    appliedThreadPriority = desiredThreadPriority;
                }

                // 雷达采样频率：根据"谁在看雷达"自适应（详见 runtime_timing.hpp）
                // 有人看 → 20Hz；后台共享 → 10Hz；只有服务没人看 → 4Hz；没人 → 0（不采）
                const bool radarSamplingEnabled =
                    config.radarSnapshotEnabled();
                const int radarRate = radarSamplingEnabled
                    ? runtime_timing::radarSamplingRate(
                        runtime_timing::RadarDemand{
                            config.localRadarEnabled,
                            config.webRadarEnabled,
                            webRadar.viewerCount() > 0,
                            config.publicRelayEnabled,
                            config.radarRecordingEnabled,
                            gameForeground
                        })
                    : 0;
                performance_metrics::radarRateHz.store(
                    radarRate,
                    std::memory_order_relaxed);

                // 后台是否继续采样雷达（即使游戏不在前台，也允许网页雷达继续更新）
                const bool backgroundRadarSampling =
                    (config.webRadarEnabled ||
                     config.publicRelayEnabled) &&
                    !config.webRadarPauseWhenUnfocused;

                // ---- 游戏不在前台时的处理 ----
                if (!gameForeground && !backgroundRadarSampling) {
                    // 只清空一次状态（避免每帧都做）
                    if (!stateClearedWhileInactive) {
                        esp::clearRuntimeState();          // 清掉旧的实体数据
                        stateClearedWhileInactive = true;
                        // 通知网页/中继：现在没有有效数据了
                        if (config.webRadarEnabled) {
                            webRadar.publish(
                                esp::getGameSnapshot(),
                                config.webRadarIncludePlayerNames,
                                config.webRadarIncludeSteamIds,
                                config.webRadarTeamViewPolicy);
                        }
                        if (config.publicRelayEnabled) {
                            publicRelay.publish(
                                esp::getGameSnapshot(),
                                config.publicRelayIncludePlayerNames,
                                config.publicRelayIncludeSteamIds,
                                config.publicRelayTeamViewPolicy);
                        }
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(25));   // 睡 25ms 再循环
                    nextDataTime = std::chrono::steady_clock::now();
                    nextRadarTime = nextDataTime;
                    continue;
                }
                stateClearedWhileInactive = false;

                // ---- 计算这一轮该用多少采样频率 ----
                const runtime_timing::SamplingDemand samplingDemand{
                    latencySensitive,                       // 瞄准/扳机开着 → 240Hz
                    gameForeground &&
                        (config.espEnabled ||
                         config.grenadeESP ||
                         config.droppedWeaponESP),          // 画 ESP → 跟随刷新率
                    gameForeground &&
                        (config.antiFlash || config.bombTimer), // 周期性功能 → 60Hz
                    radarRate > 0 ? radarRate : 20          // 兜底 20Hz
                };
                const int samplingRate =
                    runtime_timing::dataSamplingRate(
                        sdl_renderer::getTargetRefreshRate(), // 显示器刷新率
                        samplingDemand);
                // 频率变了才重新计算计划时间（避免每帧都重置定时器）
                if (samplingRate != appliedSamplingRate) {
                    appliedSamplingRate = samplingRate;
                    performance_metrics::samplingRateHz.store(
                        samplingRate,
                        std::memory_order_relaxed);
                    nextDataTime = std::chrono::steady_clock::now();
                }
                const auto dataInterval =
                    runtime_timing::intervalForRate(samplingRate); // 频率→间隔毫秒

                // 采样用的配置：雷达频率写进去；游戏不在前台时强制关掉
                // 瞄准/扳机/ESP（防止在桌面误触发）
                menu::RuntimeConfig samplingConfig = config;
                samplingConfig.radarRefreshRateHz = std::max(1, radarRate);
                if (!gameForeground) {
                    samplingConfig.antiFlash = false;
                    samplingConfig.aimbotEnabled = false;
                    samplingConfig.triggerbotEnabled = false;
                    samplingConfig.espEnabled = false;
                    samplingConfig.rcsEnabled = false;
                    samplingConfig.aimbotMode = 0;      // 顺带禁用魔法子弹模式
                    samplingConfig.autoRotationEnabled = false;
                    samplingConfig.bhopEnabled = false;
                }

                // ---- 真正干活：读取实体数据 + 瞄准/扳机 ----
                const auto samplingStarted =
                    std::chrono::steady_clock::now();   // 记录开始时刻
                esp::updateEntities(samplingConfig);    // 遍历实体列表读数据
                if (gameForeground) {                   // 前台才允许瞄准
                    aimbot::update(config);             // 自动瞄准
                    aimbot::updateTriggerbot(config);   // 自动扳机
                    aimbot::updateMagicBullet(config);  // 魔法子弹（静默写）
                    aimbot::updateTeleport(config);     // 角色瞬移（写本地位置）
                    aimbot::updateBhop(config);         // 连跳
                }

                // 记录这次采样耗时（用于性能统计）
                const auto sampleComplete =
                    std::chrono::steady_clock::now();
                performance_metrics::samplingDuration.record(
                    sampleComplete - samplingStarted);

                // ---- 到点就把快照发布给雷达出口 ----
                if (radarSamplingEnabled &&
                    sampleComplete >= nextRadarTime) {
                    const esp::GameSnapshot snapshot =
                        esp::getGameSnapshot();
                    if (config.webRadarEnabled) {        // 本机网页雷达
                        webRadar.publish(
                            snapshot,
                            config.webRadarIncludePlayerNames,
                            config.webRadarIncludeSteamIds,
                            config.webRadarTeamViewPolicy);
                    }
                    if (config.publicRelayEnabled) {     // 公网中继
                        publicRelay.publish(
                            snapshot,
                            config.publicRelayIncludePlayerNames,
                            config.publicRelayIncludeSteamIds,
                            config.publicRelayTeamViewPolicy);
                    }
                    if (config.radarRecordingEnabled && snapshot) { // 脱敏录制
                        game::web_radar_json::SerializationOptions options;
                        options.includePlayerNames = false;  // 录制时一律不带玩家名（脱敏）
                        snapshotRecorder.publish(
                            serializeSnapshotCached(snapshot, options));
                    }
                    nextRadarTime =
                        sampleComplete +
                        runtime_timing::intervalForRate(
                            std::max(1, radarRate));
                } else if (!radarSamplingEnabled) {
                    nextRadarTime = sampleComplete;
                }

                // ---- 按计划节奏休眠，控制采样频率 ----
                nextDataTime += dataInterval;
                const auto now = std::chrono::steady_clock::now();
                if (nextDataTime > now) {
                    std::this_thread::sleep_until(nextDataTime); // 睡到计划时间
                } else {
                    // 采样耗时超过了预算（比如某帧数据特别大）：
                    // 睡 1ms 退避而不是空转烧 CPU，并记录一次"错过截止时间"
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                    performance_metrics::missedSamplingDeadlines.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    nextDataTime = now + dataInterval;
                }
            }
        });

        // ====================================================================
        // 主线程 = 渲染线程：负责画 ESP、雷达、菜单，并同步 Web 服务配置
        // ====================================================================
        auto nextRenderTime = std::chrono::steady_clock::now();
        while (sdl_renderer::running &&
               !sdl_renderer::isGameDisconnected())   // 游戏断开就退出这个循环
        {
            const auto renderStarted = std::chrono::steady_clock::now();
            sdl_renderer::pollEvents();               // 处理鼠标/键盘事件
            sdl_renderer::updateWindowPosition();     // 跟随游戏窗口移动/缩放

            // 游戏窗口不可见（最小化/被遮挡）时暂停渲染，省 CPU
            if (!sdl_renderer::isGameVisible()) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(25));
                nextRenderTime = std::chrono::steady_clock::now();
                continue;
            }

            if (!sdl_renderer::beginFrame()) {        // 开始一帧
                break;
            }
            sdl_renderer::newFrameImGui();            // 开始一帧 ImGui

            // ---- 绘制 ESP（敌人框/血条/骨骼） ----
            if (menu::espEnabled ||
                menu::grenadeESP ||
                menu::droppedWeaponESP ||
                (menu::aimbotEnabled &&
                 menu::aimbotShowFOV)) {              // 开了瞄准且显示 FOV 圈也要画
                esp::render();
            }

            // ---- 绘制本地固定地图雷达（叠加在游戏里） ----
            const menu::RuntimeConfig renderConfig =
                menu::getRuntimeConfig();
            if (renderConfig.localRadarEnabled) {
                const esp::GameSnapshot snapshot =
                    esp::getGameSnapshot();
                if (snapshot) {
                    local_fixed_radar::render(
                        *snapshot,
                        local_fixed_radar::RenderConfig{
                            renderConfig.localRadarAnchorX,   // 雷达在屏幕上的位置
                            renderConfig.localRadarAnchorY,
                            renderConfig.localRadarSize,      // 大小
                            renderConfig.localRadarMarkerSize,// 玩家标记大小
                            renderConfig.localRadarShowNames  // 是否显示名字
                        });
                }
            } else {
                local_fixed_radar::reset();           // 关掉就清空状态
            }

            esp::renderBombTimer();                   // C4 炸弹倒计时
            menu::render();                           // 设置菜单（F4 开关）
            menu::autosavePersistentSettings();       // 设置改动后最多 5 秒落盘一次

            // ---- 同步 Web 服务配置（把菜单改动应用到服务） ----
            {
                const menu::RuntimeConfig config =
                    menu::getRuntimeConfig();
                webRadar.sync(config);                // 本机网页雷达
                publicRelay.sync(config);             // 公网中继
                snapshotRecorder.sync(config.radarRecordingEnabled); // 录制
                menu::setRecorderStatus(snapshotRecorder.status());
            }

            sdl_renderer::renderImGui();              // 把 ImGui 画到屏幕上
            sdl_renderer::endFrame();                 // 结束一帧
            performance_metrics::renderCpuDuration.record(
                std::chrono::steady_clock::now() - renderStarted);

            // ---- 控制渲染帧率：跟随显示器刷新率（至少 60 FPS） ----
            const int refreshRate = std::max(
                60,
                sdl_renderer::getTargetRefreshRate());
            const auto renderInterval =
                std::chrono::nanoseconds(
                    1000000000LL / refreshRate);      // 1 秒 / 帧率 = 每帧间隔
            nextRenderTime += renderInterval;
            const auto now = std::chrono::steady_clock::now();
            if (nextRenderTime > now) {
                std::this_thread::sleep_until(nextRenderTime); // 睡到该画下一帧
            } else {
                // 渲染超时（一帧画太久）：记录错过，立即进入下一帧
                performance_metrics::missedRenderDeadlines.fetch_add(
                    1,
                    std::memory_order_relaxed);
                nextRenderTime = now;
            }
        }

        // ---- 游戏断开：停止数据线程与压枪线程，清理，回到等待界面 ----
        dataRunning.store(false, std::memory_order_relaxed); // 通知数据线程退出
        dataThread.join();                                  // 等它真正结束
        aimbot::stopRcsWorker();                            // 停压枪线程
        sdl_renderer::shutdownImGui();
        local_fixed_radar::reset();
        sdl_renderer::destroy();
        esp::clearRuntimeState();

        // 断开前把最后一帧（空快照）推给出口，让网页显示"已断开"
        {
            const menu::RuntimeConfig config =
                menu::getRuntimeConfig();
            if (config.webRadarEnabled) {
                webRadar.publish(
                    esp::getGameSnapshot(),
                    config.webRadarIncludePlayerNames,
                    config.webRadarIncludeSteamIds,
                    config.webRadarTeamViewPolicy);
            }
            if (config.publicRelayEnabled) {
                publicRelay.publish(
                    esp::getGameSnapshot(),
                    config.publicRelayIncludePlayerNames,
                    config.publicRelayIncludeSteamIds,
                    config.publicRelayTeamViewPolicy);
            }
        }
        memory::Close();               // 关闭游戏进程句柄

        // 如果用户没退出（游戏只是重启了），等 500ms 回到外层循环重新等待
        if (sdl_renderer::running) {
            diagnostics::log(
                L"CS2 disconnected; returning to the waiting screen.");
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500));
        }
    }

    // ---- 程序退出前的统一清理 ----
    webRadar.stop();                   // 停止网页雷达服务
    publicRelay.stop();                // 停止公网中继
    snapshotRecorder.stop();           // 停止录制
    menu::setRecorderStatus(snapshotRecorder.status());
    menu::savePersistentSettings();    // 保存用户设置（下次启动时恢复）
    memory::Close();                   // 关闭进程句柄
    return 0;                          // 返回 0 表示正常退出
}
