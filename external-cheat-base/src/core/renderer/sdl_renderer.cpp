// ============================================================================
// sdl_renderer.cpp —— 覆盖层渲染模块的实现（SDL2 + ImGui）
//
// 【这个文件是干什么的】
// 1. 创建一个"透明覆盖层窗口"叠在游戏窗口上（本文件最核心的工程点）；
// 2. 让鼠标只在菜单区域生效、其余区域"穿透"到游戏（点击穿透）；
// 3. 跟随游戏窗口移动/缩放/换显示器，并自动识别游戏画面区域（视口）；
// 4. 初始化 ImGui、处理键盘事件（F4 菜单 / F9 退出）、提供画线/画框函数。
//
// 【透明覆盖层的实现原理（重点）】
// 窗口用了 Windows 的"分层窗口 + 颜色键"技巧：
//   - 把窗口背景刷成一种特殊颜色（RGB(1,0,1)，几乎黑色但不是纯黑）；
//   - 调用 SetLayeredWindowAttributes 把这种颜色设为"透明"；
//   - 结果：窗口里除了这种颜色以外的地方都显示，这个颜色处完全透明。
//   这样 ESP 画的线/框能看到，窗口其他区域不遮游戏。选 RGB(1,0,1)
//   而不是纯黑，是为了把纯黑留给 ImGui 的背景/描边/文字使用。
//   代价是做不到"真半透明"（见 initImGui 里注释），所以菜单是纯不透明的。
//
// 【DPI 处理】
// 程序声明自己"每个显示器分别感知 DPI"（per-monitor DPI aware v2），
// 让 SDL 和 Win32 使用同一套物理像素坐标——否则副屏缩放比例不同时，
// ESP 画的位置会错位。
//
// 【给 C++ 初学者的概念】
// - HWND：Windows 窗口的句柄（身份证号）。
// - WNDPROC：窗口消息处理函数（Windows 把鼠标点击等事件"发消息"给它）。
// - std::atomic：多线程共享的变量。
// - 匿名命名空间 namespace { }：本文件私有。
// ============================================================================

#include "sdl_renderer.h"
#include <dwmapi.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "viewport_math.hpp"
#include "../diagnostics.hpp"
#include "../../features/menu.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")

namespace
{
    constexpr Uint8 TRANSPARENCY_KEY_R = 1;
    constexpr Uint8 TRANSPARENCY_KEY_G = 0;
    constexpr Uint8 TRANSPARENCY_KEY_B = 1;
    constexpr COLORREF TRANSPARENCY_COLOR_KEY =
        RGB(TRANSPARENCY_KEY_R, TRANSPARENCY_KEY_G, TRANSPARENCY_KEY_B);
    constexpr float BASE_FONT_SIZE = 18.0f;

    // ------------------------------------------------------------------
    // addCjkFallbackFont：把系统中文字体合并进 ImGui 字体图集
    // ------------------------------------------------------------------
    // ImGui 默认字体只含拉丁字形，没有中文字符——中文 UI 会全部渲染成问号。
    // 这里把系统中的中文字体（微软雅黑优先，依次回退黑体/宋体/等线）以
    // MergeMode 合入默认字体：拉丁字符仍走默认字形，CJK 区段走中文字体。
    // 找不到任何中文字体时静默跳过（此时 UI 保持英文/回退默认字形）。
    void addCjkFallbackFont(ImFontAtlas* atlas, const ImFontConfig& baseConfig)
    {
        // 需要合入的区段：CJK 表意文字、CJK 标点、全角形式、通用标点（… —）、
        // 数学符号（≠、−）、箭头（→）。拉丁字母与 °±× 由默认字体覆盖。
        static const ImWchar kCjkRanges[] = {
            0x2000, 0x206F,   // 通用标点
            0x2190, 0x21FF,   // 箭头
            0x2200, 0x22FF,   // 数学符号（≠、−）
            0x3000, 0x303F,   // CJK 标点
            0x4E00, 0x9FFF,   // CJK 统一表意文字
            0xFF00, 0xFFEF,   // 全角形式
            0
        };
        const char* const kFontCandidates[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",   // 微软雅黑
            "C:\\Windows\\Fonts\\msyhbd.ttc", // 微软雅黑 Bold
            "C:\\Windows\\Fonts\\simhei.ttf", // 黑体
            "C:\\Windows\\Fonts\\simsun.ttc", // 宋体
            "C:\\Windows\\Fonts\\Deng.ttf"    // 等线
        };
        for (const char* path : kFontCandidates) {
            ImFontConfig merged = baseConfig;
            merged.MergeMode = true;                 // 合入默认字体（不是新建独立字体）
            merged.GlyphOffset.y = 1.0f;             // 与默认字体基线微调对齐
            if (atlas->AddFontFromFileTTF(
                    path,
                    baseConfig.SizePixels,
                    &merged,
                    kCjkRanges)) {
                break;
            }
        }
    }

    struct GameDisplayGeometry
    {
        RECT gameClient{};
        RECT monitor{};
        HMONITOR monitorHandle = nullptr;
    };

    using ContentViewport = viewport_math::Viewport;

    struct WindowSearchContext
    {
        DWORD processId = 0;
        const wchar_t* expectedTitle = nullptr;
        HWND bestWindow = nullptr;
        uint64_t bestScore = 0;
    };

    GameDisplayGeometry currentGeometry{};
    ContentViewport currentViewport{};
    ContentViewport pendingViewport{};
    int pendingViewportSamples = 0;
    int appliedViewportMode = -1;
    ImGuiStyle baseImGuiStyle{};
    float currentDpiScale = 1.0f;
    uint32_t dpiRevision = 0;
    bool baseImGuiStyleReady = false;
    bool imguiInitialized = false;
    bool gameVisible = true;
    bool mixedMonitorBlocked = false;
    bool overlayPositionFailed = false;
    std::atomic<bool> gameForeground{ false };
    std::atomic<bool> inputAllowed{ false };
    std::atomic<bool> gameDisconnected{ false };
    std::atomic<bool> acceleratedRenderer{ false };
    std::atomic<bool> vsyncEnabled{ false };
    std::atomic<bool> dpiAwarenessReliable{ false };
    std::atomic<bool> gameOnSingleMonitor{ true };
    std::atomic<int> targetRefreshRate{ 144 };
    std::atomic<uint64_t> rendererRevision{ 0 };
    RECT interactiveRect{};
    WNDPROC originalWindowProc = nullptr;

    struct ViewportDetectionRequest
    {
        RECT rect{};
        uint64_t generation = 0;
    };

    struct ViewportDetectionResult
    {
        RECT rect{};
        ContentViewport viewport{};
        uint64_t generation = 0;
    };

    std::mutex viewportWorkerMutex;
    std::condition_variable viewportWorkerCv;
    std::thread viewportWorker;
    bool viewportWorkerStopping = false;
    bool viewportRequestReady = false;
    bool viewportResultReady = false;
    ViewportDetectionRequest viewportRequest{};
    ViewportDetectionResult viewportResult{};
    uint64_t viewportGeneration = 0;
    DWORD lastViewportQueueTime = 0;

    template <typename Function>
    Function loadFunction(
        HMODULE module,
        const char* functionName)
    {
        static_assert(sizeof(Function) == sizeof(FARPROC));
        return module
            ? std::bit_cast<Function>(
                GetProcAddress(module, functionName))
            : nullptr;
    }

    int rectWidth(const RECT& rect)
    {
        return rect.right - rect.left;
    }

    int rectHeight(const RECT& rect)
    {
        return rect.bottom - rect.top;
    }

    bool sameRect(const RECT& lhs, const RECT& rhs)
    {
        return lhs.left == rhs.left && lhs.top == rhs.top &&
            lhs.right == rhs.right && lhs.bottom == rhs.bottom;
    }

    bool sameViewport(const ContentViewport& lhs, const ContentViewport& rhs)
    {
        return viewport_math::same(lhs, rhs);
    }

    void publishViewport(const ContentViewport& viewport)
    {
        currentViewport = viewport;
        VIEWPORT_X = viewport.x;
        VIEWPORT_Y = viewport.y;
        VIEWPORT_W = static_cast<uint32_t>(std::max(0, viewport.width));
        VIEWPORT_H = static_cast<uint32_t>(std::max(0, viewport.height));
    }

    struct AsyncKeyTracker
    {
        int virtualKey = 0;
        bool wasDown = false;
    };

    bool consumeAsyncKeyPress(int virtualKey, AsyncKeyTracker& tracker)
    {
        if (virtualKey <= 0 || virtualKey > 0xFF) {
            tracker = {};
            return false;
        }

        if (tracker.virtualKey != virtualKey) {
            tracker.virtualKey = virtualKey;
            tracker.wasDown = false;
        }

        const SHORT state = GetAsyncKeyState(virtualKey);
        const bool isDown = (state & 0x8000) != 0;
        const bool pressedSinceLastPoll = (state & 0x0001) != 0;
        const bool pressed =
            pressedSinceLastPoll || (isDown && !tracker.wasDown);
        tracker.wasDown = isDown;
        return pressed;
    }

    // ------------------------------------------------------------------
    // findProcessWindow：EnumWindows 的回调——为每个窗口打分，挑出游戏主窗口
    // ------------------------------------------------------------------
    // EnumWindows 会遍历屏幕上所有顶层窗口，对每个窗口调用一次这个函数。
    // 打分规则（见函数内）：
    //   标题完全匹配 → 加超大权重（2^62）；可见 → 加较大权重（2^61）；
    //   面积大 → 加分。分数最高的窗口就是游戏主窗口。
    // 之所以这么复杂，是因为 cs2.exe 进程名下可能有好几个窗口（设置界面、
    // 控制台等），要挑出真正的主游戏窗口。
    BOOL CALLBACK findProcessWindow(HWND hwnd, LPARAM parameter)
    {
        auto* context = reinterpret_cast<WindowSearchContext*>(parameter);
        if (!context || GetWindow(hwnd, GW_OWNER)) {
            return TRUE;
        }

        DWORD windowProcessId = 0;
        GetWindowThreadProcessId(hwnd, &windowProcessId);
        if (context->processId != 0 && windowProcessId != context->processId) {
            return TRUE;
        }

        RECT client{};
        if (!GetClientRect(hwnd, &client)) {
            return TRUE;
        }

        const int width = rectWidth(client);
        const int height = rectHeight(client);
        if (width < 640 || height < 480) {
            return TRUE;
        }

        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        const bool exactTitle =
            context->expectedTitle &&
            _wcsicmp(title, context->expectedTitle) == 0;
        const bool visible = IsWindowVisible(hwnd) != FALSE;

        // Prefer the expected title, then visibility and area among top-level
        // windows owned by cs2.exe. Minimized games remain attachable.
        const uint64_t area =
            static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        const uint64_t score =
            area +
            (exactTitle ? (uint64_t{1} << 62) : 0) +
            (visible ? (uint64_t{1} << 61) : 0);
        if (score > context->bestScore) {
            context->bestScore = score;
            context->bestWindow = hwnd;
        }
        return TRUE;
    }

    HWND findGameWindow(DWORD processId, const wchar_t* expectedTitle)
    {
        WindowSearchContext context{};
        context.processId = processId;
        context.expectedTitle = expectedTitle;
        EnumWindows(findProcessWindow, reinterpret_cast<LPARAM>(&context));
        if (context.bestWindow) {
            return context.bestWindow;
        }

        // The title fallback is only used if no process id was supplied. A
        // process-scoped search must never silently attach to another process.
        return processId == 0 ? FindWindowW(nullptr, expectedTitle) : nullptr;
    }

    struct CapturedClientSample
    {
        int width = 0;
        int height = 0;
        std::vector<uint32_t> pixels;
    };

    bool captureClientSample(
        const RECT& clientScreenRect,
        CapturedClientSample& sample)
    {
        constexpr int MAX_SAMPLE_WIDTH = 320;
        constexpr int MAX_SAMPLE_HEIGHT = 180;

        const int sourceWidth = rectWidth(clientScreenRect);
        const int sourceHeight = rectHeight(clientScreenRect);
        if (sourceWidth <= 0 || sourceHeight <= 0) {
            return false;
        }

        const float scale = std::min(
            1.0f,
            std::min(
                static_cast<float>(MAX_SAMPLE_WIDTH) / sourceWidth,
                static_cast<float>(MAX_SAMPLE_HEIGHT) / sourceHeight));
        const int sampleWidth = std::max(
            1,
            static_cast<int>(std::lround(sourceWidth * scale)));
        const int sampleHeight = std::max(
            1,
            static_cast<int>(std::lround(sourceHeight * scale)));

        HDC desktop = GetDC(nullptr);
        if (!desktop) {
            return false;
        }

        HDC memory = CreateCompatibleDC(desktop);
        if (!memory) {
            ReleaseDC(nullptr, desktop);
            return false;
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = sampleWidth;
        bitmapInfo.bmiHeader.biHeight = -sampleHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* bitmapBits = nullptr;
        HBITMAP bitmap = CreateDIBSection(
            memory,
            &bitmapInfo,
            DIB_RGB_COLORS,
            &bitmapBits,
            nullptr,
            0);
        if (!bitmap || !bitmapBits) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            DeleteDC(memory);
            ReleaseDC(nullptr, desktop);
            return false;
        }

        HGDIOBJ previousBitmap = SelectObject(memory, bitmap);
        SetStretchBltMode(memory, COLORONCOLOR);
        const BOOL captured = StretchBlt(
            memory,
            0,
            0,
            sampleWidth,
            sampleHeight,
            desktop,
            clientScreenRect.left,
            clientScreenRect.top,
            sourceWidth,
            sourceHeight,
            SRCCOPY);

        if (previousBitmap) {
            SelectObject(memory, previousBitmap);
        }

        if (captured) {
            sample.width = sampleWidth;
            sample.height = sampleHeight;
            sample.pixels.resize(
                static_cast<size_t>(sampleWidth) *
                static_cast<size_t>(sampleHeight));
            std::copy_n(
                static_cast<const uint32_t*>(bitmapBits),
                sample.pixels.size(),
                sample.pixels.begin());
        }

        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, desktop);
        return captured != FALSE;
    }

    bool isNearBlack(uint32_t pixel)
    {
        // A top-down 32-bit BI_RGB DIB stores pixels as B, G, R, X.
        const uint8_t blue = static_cast<uint8_t>(pixel);
        const uint8_t green = static_cast<uint8_t>(pixel >> 8);
        const uint8_t red = static_cast<uint8_t>(pixel >> 16);
        return red <= 12 && green <= 12 && blue <= 12;
    }

    float blackRatioForColumn(
        const CapturedClientSample& sample,
        int x)
    {
        constexpr int SAMPLE_COUNT = 17;
        int black = 0;
        for (int i = 1; i <= SAMPLE_COUNT; ++i) {
            const int y =
                (sample.height * i) / (SAMPLE_COUNT + 1);
            const size_t index =
                static_cast<size_t>(y) * sample.width + x;
            black += isNearBlack(sample.pixels[index]) ? 1 : 0;
        }
        return static_cast<float>(black) /
            static_cast<float>(SAMPLE_COUNT);
    }

    float blackRatioForRow(
        const CapturedClientSample& sample,
        int y)
    {
        constexpr int SAMPLE_COUNT = 17;
        int black = 0;
        for (int i = 1; i <= SAMPLE_COUNT; ++i) {
            const int x =
                (sample.width * i) / (SAMPLE_COUNT + 1);
            const size_t index =
                static_cast<size_t>(y) * sample.width + x;
            black += isNearBlack(sample.pixels[index]) ? 1 : 0;
        }
        return static_cast<float>(black) /
            static_cast<float>(SAMPLE_COUNT);
    }

    // ------------------------------------------------------------------
    // 视口自动检测：截图采样判断"黑边"
    // ------------------------------------------------------------------
    // 有些玩家用 4:3 拉伸或非原生分辨率，游戏画面四周会有黑边。
    // ESP 如果按整个窗口画就会偏移。所以：
    //   1. captureClientSample：把游戏画面缩小截图（省内存，最多 320x180）；
    //   2. detectContentViewport：逐行/逐列统计"黑色像素比例"，
    //      连续很黑的边缘就是黑边，据此算出真正画面区域（视口）。
    // 由于截图较慢，这段逻辑放在一个后台线程跑（见 queueViewportDetection 等），
    // 不阻塞渲染线程。
    ContentViewport detectContentViewport(const RECT& clientScreenRect)
    {
        const int width = rectWidth(clientScreenRect);
        const int height = rectHeight(clientScreenRect);
        ContentViewport result{ 0, 0, width, height };
        if (width <= 0 || height <= 0) {
            return result;
        }

        CapturedClientSample sample{};
        if (!captureClientSample(clientScreenRect, sample)) {
            return result;
        }

        const int maxHorizontalBorder = sample.width * 3 / 10;
        const int maxVerticalBorder = sample.height * 3 / 10;
        int left = 0;
        int right = 0;
        int top = 0;
        int bottom = 0;

        while (left < maxHorizontalBorder &&
            blackRatioForColumn(
                sample,
                left) >= 0.88f) {
            ++left;
        }
        while (right < maxHorizontalBorder &&
            blackRatioForColumn(
                sample,
                sample.width - 1 - right) >= 0.88f) {
            ++right;
        }
        while (top < maxVerticalBorder &&
            blackRatioForRow(
                sample,
                top) >= 0.88f) {
            ++top;
        }
        while (bottom < maxVerticalBorder &&
            blackRatioForRow(
                sample,
                sample.height - 1 - bottom) >= 0.88f) {
            ++bottom;
        }

        const int horizontalTolerance =
            std::max(2, sample.width / 100);
        const int verticalTolerance =
            std::max(2, sample.height / 100);
        const int minimumHorizontalBorder =
            std::max(2, sample.width / 100);
        const int minimumVerticalBorder =
            std::max(2, sample.height / 100);
        const int horizontalProbe =
            std::min(
                sample.width - 1,
                left + std::max(2, sample.width / 200));
        const int verticalProbe =
            std::min(
                sample.height - 1,
                top + std::max(2, sample.height / 200));

        const bool hasSideBars =
            left >= minimumHorizontalBorder &&
            right >= minimumHorizontalBorder &&
            std::abs(left - right) <= horizontalTolerance &&
            blackRatioForColumn(
                sample,
                horizontalProbe) < 0.65f;
        const bool hasTopBars =
            top >= minimumVerticalBorder &&
            bottom >= minimumVerticalBorder &&
            std::abs(top - bottom) <= verticalTolerance &&
            blackRatioForRow(
                sample,
                verticalProbe) < 0.65f;

        // A real game viewport normally letterboxes on only one axis. If both
        // candidates match, retain the stronger normalized pair.
        if (hasSideBars &&
            (!hasTopBars ||
             static_cast<float>(left + right) / sample.width >=
                 static_cast<float>(top + bottom) / sample.height)) {
            const int symmetricSampleBorder = (left + right) / 2;
            const int symmetricBorder = static_cast<int>(std::lround(
                static_cast<float>(symmetricSampleBorder) * width /
                sample.width));
            result.x = symmetricBorder;
            result.width = width - symmetricBorder * 2;
        } else if (hasTopBars) {
            const int symmetricSampleBorder = (top + bottom) / 2;
            const int symmetricBorder = static_cast<int>(std::lround(
                static_cast<float>(symmetricSampleBorder) * height /
                sample.height));
            result.y = symmetricBorder;
            result.height = height - symmetricBorder * 2;
        }

        if (result.width < width / 2 || result.height < height / 2) {
            return ContentViewport{ 0, 0, width, height };
        }
        return result;
    }

    void queueViewportDetection(
        const RECT& rect,
        uint64_t generation)
    {
        {
            std::lock_guard<std::mutex> lock(viewportWorkerMutex);
            if (viewportWorkerStopping) {
                return;
            }
            viewportRequest = ViewportDetectionRequest{ rect, generation };
            viewportRequestReady = true;
        }
        viewportWorkerCv.notify_one();
    }

    void startViewportDetectionWorker()
    {
        std::lock_guard<std::mutex> lock(viewportWorkerMutex);
        if (viewportWorker.joinable()) {
            return;
        }
        viewportWorkerStopping = false;
        viewportRequestReady = false;
        viewportResultReady = false;
        viewportWorker = std::thread([]() {
            while (true) {
                ViewportDetectionRequest request{};
                {
                    std::unique_lock<std::mutex> lock(viewportWorkerMutex);
                    viewportWorkerCv.wait(lock, []() {
                        return viewportWorkerStopping ||
                            viewportRequestReady;
                    });
                    if (viewportWorkerStopping) {
                        return;
                    }
                    request = viewportRequest;
                    viewportRequestReady = false;
                }

                const ContentViewport detected =
                    detectContentViewport(request.rect);

                {
                    std::lock_guard<std::mutex> lock(viewportWorkerMutex);
                    if (!viewportWorkerStopping) {
                        viewportResult = ViewportDetectionResult{
                            request.rect,
                            detected,
                            request.generation
                        };
                        viewportResultReady = true;
                    }
                }
            }
        });
    }

    void stopViewportDetectionWorker()
    {
        {
            std::lock_guard<std::mutex> lock(viewportWorkerMutex);
            viewportWorkerStopping = true;
            viewportRequestReady = false;
        }
        viewportWorkerCv.notify_all();
        if (viewportWorker.joinable()) {
            viewportWorker.join();
        }

        std::lock_guard<std::mutex> lock(viewportWorkerMutex);
        viewportResultReady = false;
        viewportWorkerStopping = false;
    }

    bool consumeViewportDetection(ViewportDetectionResult& result)
    {
        std::lock_guard<std::mutex> lock(viewportWorkerMutex);
        if (!viewportResultReady) {
            return false;
        }
        result = viewportResult;
        viewportResultReady = false;
        return true;
    }

    void resetAutoViewportDetection(const RECT& rect)
    {
        ++viewportGeneration;
        pendingViewport = currentViewport;
        pendingViewportSamples = 1;
        lastViewportQueueTime = GetTickCount();
        queueViewportDetection(rect, viewportGeneration);
    }

    ContentViewport viewportForMode(
        int mode,
        int clientWidth,
        int clientHeight)
    {
        return viewport_math::forMode(mode, clientWidth, clientHeight);
    }

    // Keep SDL and Win32 in the same physical-pixel coordinate system. This is
    // especially important when the game is on a monitor whose scale differs
    // from the primary monitor.
    // ------------------------------------------------------------------
    // configureVideoHints：设置 DPI 感知模式（高分屏适配的关键）
    // ------------------------------------------------------------------
    // 声明"按显示器感知 DPI"（per-monitor v2），并强制 SDL 用同一套物理像素，
    // 让覆盖层坐标与游戏窗口坐标一致。函数在 user32.dll 里动态查找 API
    // （loadFunction），因为老系统可能没有这些函数，动态查找可以优雅降级。
    bool configureVideoHints()
    {
        using SetProcessDpiAwarenessContextFn =
            BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        static SetProcessDpiAwarenessContextFn setProcessDpiAwarenessContext =
            []() {
                return loadFunction<
                    SetProcessDpiAwarenessContextFn>(
                        GetModuleHandleW(L"user32.dll"),
                        "SetProcessDpiAwarenessContext");
            }();

        bool awarenessConfigured = false;
        if (setProcessDpiAwarenessContext) {
            // Failure is expected if the host has already selected an equal
            // or stronger awareness mode; SDL's hint below remains in place.
            awarenessConfigured =
                setProcessDpiAwarenessContext(
                    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        } else {
            using SetProcessDpiAwareFn = BOOL(WINAPI*)();
            const auto setProcessDpiAware =
                loadFunction<SetProcessDpiAwareFn>(
                    GetModuleHandleW(L"user32.dll"),
                    "SetProcessDPIAware");
            if (setProcessDpiAware) {
                awarenessConfigured = setProcessDpiAware() != FALSE;
            }
        }

        using GetThreadDpiAwarenessContextFn =
            DPI_AWARENESS_CONTEXT(WINAPI*)();
        using AreDpiAwarenessContextsEqualFn =
            BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        const auto getThreadDpiAwarenessContext =
            loadFunction<GetThreadDpiAwarenessContextFn>(
                user32,
                "GetThreadDpiAwarenessContext");
        const auto areDpiAwarenessContextsEqual =
            loadFunction<AreDpiAwarenessContextsEqualFn>(
                user32,
                "AreDpiAwarenessContextsEqual");
        if (getThreadDpiAwarenessContext &&
            areDpiAwarenessContextsEqual) {
            const DPI_AWARENESS_CONTEXT actual =
                getThreadDpiAwarenessContext();
            awarenessConfigured =
                areDpiAwarenessContextsEqual(
                    actual,
                    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ||
                areDpiAwarenessContextsEqual(
                    actual,
                    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        }

        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "0");
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d");
        dpiAwarenessReliable.store(
            awarenessConfigured,
            std::memory_order_relaxed);
        return awarenessConfigured;
    }

    float getWindowDpiScale(HWND targetWindow)
    {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static GetDpiForWindowFn getDpiForWindowFn = []() {
            return loadFunction<GetDpiForWindowFn>(
                GetModuleHandleW(L"user32.dll"),
                "GetDpiForWindow");
        }();

        UINT dpi = 96;
        bool hasWindowDpi = false;
        if (targetWindow && getDpiForWindowFn) {
            const UINT windowDpi = getDpiForWindowFn(targetWindow);
            if (windowDpi != 0) {
                dpi = windowDpi;
                hasWindowDpi = true;
            }
        }
        if (!hasWindowDpi && targetWindow) {
            using GetDpiForMonitorFn =
                HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            static GetDpiForMonitorFn getDpiForMonitorFn = []() {
                return loadFunction<GetDpiForMonitorFn>(
                    LoadLibraryW(L"shcore.dll"),
                    "GetDpiForMonitor");
            }();
            if (getDpiForMonitorFn) {
                const HMONITOR monitor = MonitorFromWindow(
                    targetWindow,
                    MONITOR_DEFAULTTONEAREST);
                UINT dpiX = 0;
                UINT dpiY = 0;
                if (monitor &&
                    SUCCEEDED(getDpiForMonitorFn(
                        monitor,
                        0,
                        &dpiX,
                        &dpiY)) &&
                    dpiX != 0) {
                    dpi = dpiX;
                    hasWindowDpi = true;
                }
            }
        }
        if (!hasWindowDpi) {
            using GetDpiForSystemFn = UINT(WINAPI*)();
            static GetDpiForSystemFn getDpiForSystemFn = []() {
                return loadFunction<GetDpiForSystemFn>(
                    GetModuleHandleW(L"user32.dll"),
                    "GetDpiForSystem");
            }();
            if (getDpiForSystemFn) {
                const UINT systemDpi = getDpiForSystemFn();
                if (systemDpi != 0) {
                    dpi = systemDpi;
                }
            } else {
                HDC desktop = GetDC(nullptr);
                if (desktop) {
                    const int logicalDpi = GetDeviceCaps(desktop, LOGPIXELSX);
                    ReleaseDC(nullptr, desktop);
                    if (logicalDpi > 0) {
                        dpi = static_cast<UINT>(logicalDpi);
                    }
                }
            }
        }

        float scale = static_cast<float>(dpi) / 96.0f;
        if (scale < 0.5f) scale = 0.5f;
        if (scale > 4.0f) scale = 4.0f;
        return scale;
    }

    void applyImGuiDpiScale(bool force = false)
    {
        if (!baseImGuiStyleReady || !ImGui::GetCurrentContext()) {
            return;
        }

        const float scale = getWindowDpiScale(sdl_renderer::overlayHwnd);
        if (!force &&
            scale > currentDpiScale - 0.01f &&
            scale < currentDpiScale + 0.01f) {
            return;
        }

        // Always scale from the unmodified reference style. ScaleAllSizes()
        // rounds values, so repeatedly scaling the current style would drift
        // after moving between monitors several times.
        ImGuiStyle scaledStyle = baseImGuiStyle;
        scaledStyle.FontScaleDpi = scale;
        scaledStyle.ScaleAllSizes(scale);
        ImGui::GetStyle() = scaledStyle;

        currentDpiScale = scale;
        ++dpiRevision;
    }

    bool getGameDisplayGeometry(HWND gameWindow, GameDisplayGeometry& geometry)
    {
        if (!gameWindow || !IsWindow(gameWindow)) {
            return false;
        }

        RECT clientRect{};
        if (!GetClientRect(gameWindow, &clientRect)) {
            return false;
        }

        POINT topLeft{ clientRect.left, clientRect.top };
        POINT bottomRight{ clientRect.right, clientRect.bottom };
        if (!ClientToScreen(gameWindow, &topLeft) ||
            !ClientToScreen(gameWindow, &bottomRight)) {
            return false;
        }

        RECT clientScreenRect{
            topLeft.x,
            topLeft.y,
            bottomRight.x,
            bottomRight.y
        };
        if (rectWidth(clientScreenRect) <= 0 || rectHeight(clientScreenRect) <= 0) {
            return false;
        }

        // MonitorFromWindow selects the monitor with the largest intersection
        // with the game window, so this also handles monitors left/above the
        // primary display (negative virtual-desktop coordinates).
        HMONITOR monitor = MonitorFromWindow(gameWindow, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) {
            return false;
        }

        geometry.gameClient = clientScreenRect;
        geometry.monitor = monitorInfo.rcMonitor;
        geometry.monitorHandle = monitor;
        return true;
    }

    bool clientFitsMonitor(const GameDisplayGeometry& geometry)
    {
        return geometry.gameClient.left >= geometry.monitor.left &&
            geometry.gameClient.top >= geometry.monitor.top &&
            geometry.gameClient.right <= geometry.monitor.right &&
            geometry.gameClient.bottom <= geometry.monitor.bottom;
    }

    int refreshRateForMonitor(HMONITOR monitor)
    {
        MONITORINFOEXW monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!monitor ||
            !GetMonitorInfoW(
                monitor,
                reinterpret_cast<MONITORINFO*>(&monitorInfo))) {
            return 144;
        }

        DEVMODEW displayMode{};
        displayMode.dmSize = sizeof(displayMode);
        if (!EnumDisplaySettingsW(
                monitorInfo.szDevice,
                ENUM_CURRENT_SETTINGS,
                &displayMode) ||
            displayMode.dmDisplayFrequency <= 1) {
            return 144;
        }
        return std::clamp(
            static_cast<int>(displayMode.dmDisplayFrequency),
            60,
            1000);
    }

    bool isGameClientForeground()
    {
        return sdl_renderer::gameHwnd &&
            GetForegroundWindow() == sdl_renderer::gameHwnd;
    }

    bool foregroundBelongsToGameUi()
    {
        const HWND foreground = GetForegroundWindow();
        if (!foreground) {
            return false;
        }
        if (foreground == sdl_renderer::gameHwnd ||
            foreground == sdl_renderer::overlayHwnd) {
            return true;
        }

        DWORD foregroundProcessId = 0;
        DWORD gameProcessId = 0;
        GetWindowThreadProcessId(foreground, &foregroundProcessId);
        GetWindowThreadProcessId(
            sdl_renderer::gameHwnd,
            &gameProcessId);
        return gameProcessId != 0 &&
            foregroundProcessId == gameProcessId;
    }

    void updateRenderDimensions()
    {
        if (!sdl_renderer::window) {
            return;
        }

        // ImGui draw coordinates use SDL window coordinates. With DPI scaling
        // disabled above these are physical pixels and match the game client.
        int renderWidth = 0;
        int renderHeight = 0;
        SDL_GetWindowSize(sdl_renderer::window, &renderWidth, &renderHeight);
        if (renderWidth <= 0 || renderHeight <= 0) {
            return;
        }

        const bool dimensionsChanged =
            WINDOW_W != static_cast<uint32_t>(renderWidth) ||
            WINDOW_H != static_cast<uint32_t>(renderHeight);
        WIDTH = static_cast<uint32_t>(renderWidth);
        HEIGHT = static_cast<uint32_t>(renderHeight);
        WINDOW_W = WIDTH;
        WINDOW_H = HEIGHT;
        if (dimensionsChanged ||
            currentViewport.width <= 0 ||
            currentViewport.height <= 0) {
            publishViewport(viewportForMode(
                menu::viewportMode,
                renderWidth,
                renderHeight));
            pendingViewport = {};
            pendingViewportSamples = 0;
        }
    }

    LRESULT CALLBACK overlayWindowProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam)
    {
        if (message == WM_NCHITTEST) {
            if (!sdl_renderer::menuVisible) {
                return HTTRANSPARENT;
            }

            POINT cursor{};
            if (GetCursorPos(&cursor) &&
                ScreenToClient(hwnd, &cursor) &&
                PtInRect(&interactiveRect, cursor)) {
                return HTCLIENT;
            }
            return HTTRANSPARENT;
        }

        return originalWindowProc
            ? CallWindowProcW(
                originalWindowProc,
                hwnd,
                message,
                wParam,
                lParam)
            : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // ------------------------------------------------------------------
    // configureOverlayWindow：把 SDL 窗口改造成"透明覆盖层窗口"
    // ------------------------------------------------------------------
    // 1. 加扩展样式：WS_EX_LAYERED（分层窗口）+ WS_EX_TOPMOST（总在最前）
    //    + WS_EX_TOOLWINDOW（不出现任务栏图标）；
    // 2. 挂自定义窗口消息处理函数 overlayWindowProc（用于点击穿透）；
    // 3. 用颜色键透明（把 RGB(1,0,1) 设为透明色）；
    // 4. DwmExtendFrameIntoClientArea 让窗口内容能延伸到客户区边缘。
    bool configureOverlayWindow(HWND hwnd)
    {
        if (!hwnd) {
            return false;
        }

        SetLastError(0);
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (exStyle == 0 && GetLastError() != 0) {
            return false;
        }

        SetLastError(0);
        const LONG_PTR previousStyle = SetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE,
            exStyle | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
        if (previousStyle == 0 && GetLastError() != 0) {
            return false;
        }

        SetLastError(0);
        const LONG_PTR previousWindowProc = SetWindowLongPtrW(
            hwnd,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(overlayWindowProc));
        if (previousWindowProc == 0 && GetLastError() != 0) {
            return false;
        }
        originalWindowProc =
            reinterpret_cast<WNDPROC>(previousWindowProc);

        if (!SetLayeredWindowAttributes(
                hwnd,
                TRANSPARENCY_COLOR_KEY,
                0,
                LWA_COLORKEY)) {
            return false;
        }

        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        return SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
    }

    bool setClickThrough(HWND hwnd, bool enabled)
    {
        if (!hwnd) {
            return false;
        }

        SetLastError(0);
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (exStyle == 0 && GetLastError() != 0) {
            return false;
        }

        const LONG_PTR newStyle = enabled
            ? exStyle | WS_EX_TRANSPARENT
            : exStyle & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        if (newStyle == exStyle) {
            return true;
        }

        SetLastError(0);
        const LONG_PTR previous =
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, newStyle);
        return previous != 0 || GetLastError() == 0;
    }

    void discardOverlayWindow()
    {
        if (sdl_renderer::window) {
            SDL_DestroyWindow(sdl_renderer::window);
            sdl_renderer::window = nullptr;
        }
        sdl_renderer::overlayHwnd = nullptr;
        originalWindowProc = nullptr;
    }

    SDL_Renderer* createRenderer()
    {
        SDL_Renderer* newRenderer = SDL_CreateRenderer(
            sdl_renderer::window,
            -1,
            SDL_RENDERER_ACCELERATED |
                SDL_RENDERER_PRESENTVSYNC);
        if (!newRenderer) {
            newRenderer = SDL_CreateRenderer(
                sdl_renderer::window,
                -1,
                SDL_RENDERER_ACCELERATED);
        }
        if (!newRenderer) {
            newRenderer = SDL_CreateRenderer(
                sdl_renderer::window,
                -1,
                SDL_RENDERER_SOFTWARE);
        }
        if (newRenderer &&
            SDL_SetRenderDrawBlendMode(newRenderer, SDL_BLENDMODE_BLEND) != 0) {
            SDL_DestroyRenderer(newRenderer);
            return nullptr;
        }
        SDL_RendererInfo rendererInfo{};
        const bool accelerated =
            newRenderer &&
            SDL_GetRendererInfo(newRenderer, &rendererInfo) == 0 &&
            (rendererInfo.flags & SDL_RENDERER_ACCELERATED) != 0;
        const bool synchronized =
            newRenderer &&
            (rendererInfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
        acceleratedRenderer.store(
            accelerated,
            std::memory_order_relaxed);
        vsyncEnabled.store(
            synchronized,
            std::memory_order_relaxed);
        if (newRenderer) {
            rendererRevision.fetch_add(1, std::memory_order_relaxed);
        }
        return newRenderer;
    }

    bool recoverRenderer()
    {
        if (!sdl_renderer::window) {
            return false;
        }

        const bool restoreImGui = imguiInitialized;
        if (restoreImGui) {
            ImGui_ImplSDLRenderer2_Shutdown();
            ImGui_ImplSDL2_Shutdown();
        }
        if (sdl_renderer::renderer) {
            SDL_DestroyRenderer(sdl_renderer::renderer);
        }

        sdl_renderer::renderer = createRenderer();
        if (!sdl_renderer::renderer) {
            imguiInitialized = false;
            return false;
        }

        if (restoreImGui) {
            const bool platformRecovered =
                ImGui_ImplSDL2_InitForSDLRenderer(
                    sdl_renderer::window,
                    sdl_renderer::renderer);
            const bool rendererRecovered =
                platformRecovered &&
                ImGui_ImplSDLRenderer2_Init(sdl_renderer::renderer);
            if (!rendererRecovered) {
                if (platformRecovered) {
                    ImGui_ImplSDL2_Shutdown();
                }
                SDL_DestroyRenderer(sdl_renderer::renderer);
                sdl_renderer::renderer = nullptr;
                imguiInitialized = false;
                return false;
            }
        }
        return true;
    }
}

// ----------------------------------------------------------------------------
// 🎓 概念卡 ㉒：SDL2 三件套（窗口 Window、渲染器 Renderer、事件 Event）
// ----------------------------------------------------------------------------
// 【是什么】SDL2 是一个跨平台的多媒体库，做窗口/绘图/输入非常省事。
// 用它的三个核心概念：
//   1. SDL_Window（窗口）：屏幕上的一块区域，可以设无边框、置顶、透明；
//   2. SDL_Renderer（渲染器）：往窗口里"画画"的工具（负责 GPU/CPU 绘制）；
//   3. SDL_Event（事件）：鼠标、键盘、窗口变化等系统消息，用 SDL_PollEvent
//      一帧一帧地取出来处理。
// 【为什么在这里】本项目的"覆盖层"就是：一个透明窗口（SDL_CreateWindow）+
// 一个渲染器（SDL_CreateRenderer）+ 每帧 pollEvents→beginFrame→画→endFrame
// 的事件循环。窗口能叠在游戏上并穿透鼠标，是后续所有画面的地基。
// 【最小示例（画一个白色矩形）】
//   SDL_Window* w = SDL_CreateWindow("t", 0,0, 640,480, 0);
//   SDL_Renderer* r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);
//   SDL_SetRenderDrawColor(r, 255,255,255,255);   // 白色
//   SDL_Rect rect{10,10,100,100};
//   SDL_RenderFillRect(r, &rect);                  // 填充矩形
//   SDL_RenderPresent(r);                          // 显示到屏幕
// 【自己动手要点】
// 1) 渲染器的"坐标原点"在窗口左上角，x 向右、y 向下（和数学课相反）；
// 2) 每帧都要：清屏（RenderClear）→ 画 → 显示（RenderPresent）；
// 3) 事件处理里记得把 SDL 事件转发给 ImGui（ImGui_ImplSDL2_ProcessEvent），
//    否则菜单点不动（本项目 pollEvents 里就是这么做的）。
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// initWaiting：初始化"等待界面"（还没找到游戏时显示的窗口）
// ----------------------------------------------------------------------------
// 在鼠标当前所在的显示器上创建一个全屏无边框窗口，显示等待动画。
// 先配置 DPI → 初始化 SDL → 找鼠标所在显示器 → 创建窗口 → 配透明样式。
// Initialize the waiting screen on the display the user is currently using.
bool sdl_renderer::initWaiting()
{
    if (!configureVideoHints()) {
        return false;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        return false;
    }

    int initialDisplayIndex = 0;
    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        const int displayCount = SDL_GetNumVideoDisplays();
        for (int displayIndex = 0;
             displayIndex < displayCount;
             ++displayIndex) {
            SDL_Rect bounds{};
            if (SDL_GetDisplayBounds(displayIndex, &bounds) == 0 &&
                cursor.x >= bounds.x &&
                cursor.x < bounds.x + bounds.w &&
                cursor.y >= bounds.y &&
                cursor.y < bounds.y + bounds.h) {
                initialDisplayIndex = displayIndex;
                break;
            }
        }
    }

    SDL_Rect initialDisplay{};
    if (SDL_GetDisplayBounds(initialDisplayIndex, &initialDisplay) != 0) {
        initialDisplay.x = 0;
        initialDisplay.y = 0;
        initialDisplay.w = GetSystemMetrics(SM_CXSCREEN);
        initialDisplay.h = GetSystemMetrics(SM_CYSCREEN);
    }

    WIDTH = static_cast<uint32_t>(initialDisplay.w);
    HEIGHT = static_cast<uint32_t>(initialDisplay.h);
    WINDOW_W = WIDTH;
    WINDOW_H = HEIGHT;
    publishViewport(ContentViewport{
        0,
        0,
        initialDisplay.w,
        initialDisplay.h
    });

    window = SDL_CreateWindow(
        "CS2 ESP - Waiting",
        initialDisplay.x,
        initialDisplay.y,
        WIDTH, HEIGHT,
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN
    );

    if (!window) {
        SDL_Quit();
        return false;
    }

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        overlayHwnd = wmInfo.info.win.window;
    }

    if (!configureOverlayWindow(overlayHwnd)) {
        discardOverlayWindow();
        SDL_Quit();
        return false;
    }
    if (!setClickThrough(overlayHwnd, !menuVisible)) {
        discardOverlayWindow();
        SDL_Quit();
        return false;
    }

    renderer = createRenderer();

    if (!renderer) {
        discardOverlayWindow();
        SDL_Quit();
        return false;
    }

    ShowWindow(overlayHwnd, SW_SHOWNOACTIVATE);
    return true;
}

// ----------------------------------------------------------------------------
// sdl_renderer::init：初始化覆盖层（游戏已找到）
// ----------------------------------------------------------------------------
// 流程：配 DPI → 找游戏主窗口 → 拿到它的位置/尺寸/所在显示器 →
// 在游戏窗口正上方创建一个同样大小、无边框、置顶的窗口 → 配置透明样式 →
// 启动"视口检测后台线程"→ 显示窗口。
bool sdl_renderer::init(
    const wchar_t* targetWindowName,
    DWORD targetProcessId)
{
    if (!configureVideoHints()) {
        return false;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        return false;
    }

    gameHwnd = findGameWindow(targetProcessId, targetWindowName);

    GameDisplayGeometry initialGeometry{};
    if (!getGameDisplayGeometry(gameHwnd, initialGeometry)) {
        SDL_Quit();
        return false;
    }

    currentGeometry = initialGeometry;
    gameDisconnected.store(false, std::memory_order_relaxed);
    gameOnSingleMonitor.store(
        clientFitsMonitor(initialGeometry),
        std::memory_order_relaxed);
    targetRefreshRate.store(
        refreshRateForMonitor(initialGeometry.monitorHandle),
        std::memory_order_relaxed);
    WIDTH = static_cast<uint32_t>(rectWidth(initialGeometry.gameClient));
    HEIGHT = static_cast<uint32_t>(rectHeight(initialGeometry.gameClient));
    WINDOW_W = WIDTH;
    WINDOW_H = HEIGHT;
    appliedViewportMode = menu::viewportMode;
    publishViewport(
        appliedViewportMode == 0
            ? detectContentViewport(initialGeometry.gameClient)
            : viewportForMode(
                appliedViewportMode,
                rectWidth(initialGeometry.gameClient),
                rectHeight(initialGeometry.gameClient)));
    ++viewportGeneration;
    pendingViewport = currentViewport;
    pendingViewportSamples = 1;
    lastViewportQueueTime = GetTickCount();

    window = SDL_CreateWindow(
        "Overlay",
        initialGeometry.gameClient.left,
        initialGeometry.gameClient.top,
        WIDTH, HEIGHT,
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN
    );

    if (!window) {
        SDL_Quit();
        return false;
    }

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        overlayHwnd = wmInfo.info.win.window;
    }

    if (!configureOverlayWindow(overlayHwnd)) {
        discardOverlayWindow();
        SDL_Quit();
        return false;
    }
    if (!setClickThrough(overlayHwnd, !menuVisible)) {
        discardOverlayWindow();
        SDL_Quit();
        return false;
    }

    renderer = createRenderer();

    if (!renderer) {
        discardOverlayWindow();
        SDL_Quit();
        return false;
    }

    startViewportDetectionWorker();
    ShowWindow(overlayHwnd, SW_SHOWNOACTIVATE);
    updateWindowPosition();
    updateRenderDimensions();

    return true;
}

// ----------------------------------------------------------------------------
// sdl_renderer::destroy：销毁覆盖层并复位所有状态（游戏断开或退出时）
// ----------------------------------------------------------------------------
void sdl_renderer::destroy()
{
    stopViewportDetectionWorker();
    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    overlayHwnd = nullptr;
    gameHwnd = nullptr;
    currentGeometry = {};
    currentViewport = {};
    pendingViewport = {};
    pendingViewportSamples = 0;
    appliedViewportMode = -1;
    lastViewportQueueTime = 0;
    ++viewportGeneration;
    VIEWPORT_X = 0;
    VIEWPORT_Y = 0;
    VIEWPORT_W = 0;
    VIEWPORT_H = 0;
    gameVisible = true;
    mixedMonitorBlocked = false;
    overlayPositionFailed = false;
    gameForeground.store(false, std::memory_order_relaxed);
    inputAllowed.store(false, std::memory_order_relaxed);
    acceleratedRenderer.store(false, std::memory_order_relaxed);
    vsyncEnabled.store(false, std::memory_order_relaxed);
    gameOnSingleMonitor.store(true, std::memory_order_relaxed);
    interactiveRect = {};
    originalWindowProc = nullptr;
    SDL_Quit();
}

// ----------------------------------------------------------------------------
// sdl_renderer::beginFrame：开始一帧
// ----------------------------------------------------------------------------
// 把整帧画面清成"透明键色"（RGB(1,0,1)），这样覆盖层窗口就会全部透明。
// 之后画上去的内容（ESP、菜单）会覆盖在这个颜色上显示出来。
// 如果渲染器失效（显卡重置等）会自动尝试恢复（recoverRenderer）。
bool sdl_renderer::beginFrame()
{
    if (!renderer && !recoverRenderer()) {
        running = false;
        return false;
    }

    // This reserved near-black color is the only transparent color. Pure black
    // remains available for ImGui backgrounds, outlines, text, and future UI.
    const auto clearFrame = []() {
        return SDL_SetRenderDrawColor(
                sdl_renderer::renderer,
                TRANSPARENCY_KEY_R,
                TRANSPARENCY_KEY_G,
                TRANSPARENCY_KEY_B,
                255) == 0 &&
            SDL_RenderClear(sdl_renderer::renderer) == 0;
    };

    if (!clearFrame() &&
        (!recoverRenderer() || !clearFrame())) {
        running = false;
        return false;
    }
    return true;
}

void sdl_renderer::endFrame()
{
    SDL_RenderPresent(renderer);
}

// ----------------------------------------------------------------------------
// sdl_renderer::pollEvents：处理窗口事件和全局热键
// ----------------------------------------------------------------------------
// 1. 把 SDL 事件喂给 ImGui（否则菜单点不了）；
// 2. F9 退出、F4 开关菜单（用 GetAsyncKeyState 检测，见 consumeAsyncKeyPress）；
// 3. 菜单显示时，覆盖层拦截鼠标；隐藏时设置"点击穿透"（setClickThrough），
//    鼠标直接点到游戏上。
// 设计细节：菜单隐藏后把焦点还给游戏窗口；绑定热键期间屏蔽全局热键。
void sdl_renderer::pollEvents()
{
    static AsyncKeyTracker exitKeyTracker{};
    static AsyncKeyTracker menuKeyTracker{};

    bool rendererResetRequested = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);

        if (event.type == SDL_QUIT) {
            running = false;
        } else if (
            event.type == SDL_RENDER_DEVICE_RESET ||
            event.type == SDL_RENDER_TARGETS_RESET) {
            rendererResetRequested = true;
        }
    }
    if (rendererResetRequested && !recoverRenderer()) {
        diagnostics::log(
            L"SDL render device recovery failed.");
        running = false;
        return;
    }

    // Skip global hotkeys while binding, and until every configured key has
    // been released after a bind. This prevents the newly assigned key from
    // immediately hiding the menu, exiting, aiming, or firing.
    if (menu::isBindingKey ||
        menu::suppressHotkeysUntilRelease) {
        exitKeyTracker = {};
        menuKeyTracker = {};
        if (!menu::isBindingKey &&
            menu::ConfiguredHotkeysReleased()) {
            menu::suppressHotkeysUntilRelease = false;
            menu::publishRuntimeConfig();
        }
        return;
    }

    const bool gameOrOverlayForeground =
        gameHwnd
            ? foregroundBelongsToGameUi()
            : GetForegroundWindow() == overlayHwnd;

    // Always sample the keys so GetAsyncKeyState's low-order latch cannot
    // replay a press made in another application when CS2 regains focus.
    const bool exitPressed =
        consumeAsyncKeyPress(menu::exitKey, exitKeyTracker);
    const bool menuPressed =
        consumeAsyncKeyPress(menu::menuToggleKey, menuKeyTracker);

    // Check exit key (configurable, default: F9).
    if (gameOrOverlayForeground && exitPressed) {
        running = false;
        return;
    }

    // The low-order GetAsyncKeyState bit records a short press that happened
    // between frames. This keeps F4 responsive even if a frame is delayed.
    if (gameOrOverlayForeground && gameHwnd && menuPressed) {
        menuVisible = !menuVisible;
        if (!setClickThrough(overlayHwnd, !menuVisible)) {
            menuVisible = !menuVisible;
            inputAllowed.store(false, std::memory_order_relaxed);
            diagnostics::log(
                L"Unable to change overlay click-through state; "
                L"the previous menu state was restored.");
            return;
        }

        // Clicking the menu can activate the overlay HWND. When hiding it,
        // return focus to CS2 when Windows permits that transition. Injection
        // stays blocked unless the game client itself is confirmed foreground.
        if (!menuVisible &&
            GetForegroundWindow() == overlayHwnd &&
            IsWindow(gameHwnd)) {
            SetForegroundWindow(gameHwnd);
        }
        inputAllowed.store(
            !menuVisible &&
                isGameClientForeground() &&
                !menu::isBindingKey &&
                !menu::suppressHotkeysUntilRelease,
            std::memory_order_relaxed);
    }
}

// ----------------------------------------------------------------------------
// sdl_renderer::updateWindowPosition：每一帧都调用，让覆盖层紧跟游戏窗口
// ----------------------------------------------------------------------------
// 职责：
//   1. 游戏窗口不见了/最小化/跨显示器 → 隐藏覆盖层并暂停；
//   2. 游戏窗口移动/缩放 → 把覆盖层 SetWindowPos 到同样的位置和大小；
//   3. 定期重新确认"置顶"状态（防止被其他窗口盖住）；
//   4. 视口模式为"自动"时，后台线程定期截图检测黑边并更新视口；
//   5. 按显示器刷新率更新目标帧率、按 DPI 缩放 ImGui。
// 注意：游戏窗口矩形才是覆盖层矩形（不用显示器分辨率），否则 ESP 会被
// 不均匀缩放变形（代码里有专门注释说明这点）。
void sdl_renderer::updateWindowPosition()
{
    if (!gameHwnd || !overlayHwnd) return;

    if (!IsWindow(gameHwnd)) {
        gameDisconnected.store(true, std::memory_order_relaxed);
        gameForeground.store(false, std::memory_order_relaxed);
        inputAllowed.store(false, std::memory_order_relaxed);
        ShowWindow(overlayHwnd, SW_HIDE);
        return;
    }

    // Poll often enough that dragging the game between monitors does not leave
    // the overlay visibly behind it.
    static DWORD lastUpdate = 0;
    DWORD now = GetTickCount();
    if (now - lastUpdate < 50) return;
    lastUpdate = now;

    const bool uiForeground = foregroundBelongsToGameUi();
    const bool gameClientForeground = isGameClientForeground();
    gameForeground.store(
        gameClientForeground,
        std::memory_order_relaxed);
    inputAllowed.store(
        gameClientForeground &&
            !menuVisible &&
            !menu::isBindingKey &&
            !menu::suppressHotkeysUntilRelease,
        std::memory_order_relaxed);

    if (IsIconic(gameHwnd) ||
        !IsWindowVisible(gameHwnd) ||
        !uiForeground) {
        gameVisible = false;
        ShowWindow(overlayHwnd, SW_HIDE);
        return;
    }
    GameDisplayGeometry geometry{};
    if (!getGameDisplayGeometry(gameHwnd, geometry)) {
        return;
    }

    const bool fitsOneMonitor = clientFitsMonitor(geometry);
    gameOnSingleMonitor.store(
        fitsOneMonitor,
        std::memory_order_relaxed);
    if (!fitsOneMonitor) {
        if (!mixedMonitorBlocked) {
            diagnostics::log(
                L"Overlay paused: the CS2 client spans multiple monitors. "
                L"Move it fully onto one monitor to preserve pixel mapping.");
            mixedMonitorBlocked = true;
        }
        gameVisible = false;
        inputAllowed.store(false, std::memory_order_relaxed);
        ShowWindow(overlayHwnd, SW_HIDE);
        return;
    }
    if (mixedMonitorBlocked) {
        diagnostics::log(
            L"Overlay resumed: the CS2 client is fully contained by one monitor.");
        mixedMonitorBlocked = false;
    }
    if (!gameVisible) {
        ShowWindow(overlayHwnd, SW_SHOWNOACTIVATE);
    }
    gameVisible = true;

    const int x = geometry.gameClient.left;
    const int y = geometry.gameClient.top;
    const int width = rectWidth(geometry.gameClient);
    const int height = rectHeight(geometry.gameClient);

    // The monitor rectangle is deliberately not used as the render rectangle.
    // The visible game client is the viewport. Forcing the overlay to the
    // monitor's native resolution when a windowed/borderless game uses another
    // resolution would non-uniformly scale and visibly deform the ESP.
    if (!sameRect(geometry.gameClient, currentGeometry.gameClient) ||
        !sameRect(geometry.monitor, currentGeometry.monitor) ||
        geometry.monitorHandle != currentGeometry.monitorHandle) {
        const int previousWidth =
            rectWidth(currentGeometry.gameClient);
        const int previousHeight =
            rectHeight(currentGeometry.gameClient);
        if (!SetWindowPos(
            overlayHwnd,
            HWND_TOPMOST,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW
        )) {
            if (!overlayPositionFailed) {
                diagnostics::log(
                    L"Overlay positioning failed; hiding it and retrying.");
            }
            overlayPositionFailed = true;
            gameVisible = false;
            inputAllowed.store(false, std::memory_order_relaxed);
            ShowWindow(overlayHwnd, SW_HIDE);
            return;
        }
        if (overlayPositionFailed) {
            diagnostics::log(
                L"Overlay positioning recovered.");
            overlayPositionFailed = false;
        }
        currentGeometry = geometry;
        targetRefreshRate.store(
            refreshRateForMonitor(geometry.monitorHandle),
            std::memory_order_relaxed);
        if (menu::viewportMode == 0) {
            publishViewport(viewport_math::remap(
                currentViewport,
                previousWidth,
                previousHeight,
                width,
                height));
        } else {
            publishViewport(viewportForMode(
                menu::viewportMode,
                width,
                height));
        }
        WIDTH = static_cast<uint32_t>(width);
        HEIGHT = static_cast<uint32_t>(height);
        WINDOW_W = WIDTH;
        WINDOW_H = HEIGHT;
        if (menu::viewportMode == 0) {
            resetAutoViewportDetection(geometry.gameClient);
        } else {
            pendingViewport = {};
            pendingViewportSamples = 0;
        }
    } else {
        // Reassert top-most state occasionally instead of doing it on every
        // geometry poll.
        static DWORD lastTopmostUpdate = 0;
        if (now - lastTopmostUpdate >= 1000) {
            lastTopmostUpdate = now;
            if (!SetWindowPos(
                    overlayHwnd,
                    HWND_TOPMOST,
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE | SWP_NOSIZE |
                        SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
                diagnostics::log(
                    L"Unable to reassert overlay top-most state; will retry.");
                return;
            }
        }
    }

    updateRenderDimensions();

    if (menu::viewportMode != appliedViewportMode) {
        appliedViewportMode = menu::viewportMode;
        if (appliedViewportMode == 0) {
            resetAutoViewportDetection(geometry.gameClient);
        } else {
            publishViewport(viewportForMode(
                appliedViewportMode,
                width,
                height));
            pendingViewport = {};
            pendingViewportSamples = 0;
        }
    }

    // A low-cost desktop sample runs on a worker. Periodic confirmation catches
    // loading-screen false negatives and internal resolution/aspect changes
    // that do not resize the game HWND, without blocking the render thread.
    if (menu::viewportMode == 0 &&
        !menuVisible &&
        now - lastViewportQueueTime >= 2000) {
        lastViewportQueueTime = now;
        queueViewportDetection(
            geometry.gameClient,
            viewportGeneration);
    }

    ViewportDetectionResult detection{};
    if (menu::viewportMode == 0 &&
        consumeViewportDetection(detection) &&
        detection.generation == viewportGeneration &&
        sameRect(detection.rect, geometry.gameClient)) {
        if (sameViewport(detection.viewport, pendingViewport)) {
            ++pendingViewportSamples;
        } else {
            pendingViewport = detection.viewport;
            pendingViewportSamples = 1;
            // Confirm a changed viewport immediately instead of leaving a
            // stretched/stale mapping visible until the normal 2-second probe.
            lastViewportQueueTime = now;
            queueViewportDetection(
                geometry.gameClient,
                viewportGeneration);
        }
        if (pendingViewportSamples >= 2) {
            if (!sameViewport(detection.viewport, currentViewport)) {
                publishViewport(detection.viewport);
            }
        }
    }

    applyImGuiDpiScale();
}

float sdl_renderer::getDpiScale()
{
    return currentDpiScale;
}

bool sdl_renderer::isGameVisible()
{
    return gameVisible;
}

bool sdl_renderer::isGameForeground()
{
    return gameForeground.load(std::memory_order_relaxed);
}

bool sdl_renderer::isInputAllowed()
{
    return inputAllowed.load(std::memory_order_relaxed) &&
        isGameClientForeground();
}

bool sdl_renderer::isGameDisconnected()
{
    return gameDisconnected.load(std::memory_order_relaxed);
}

int sdl_renderer::getTargetRefreshRate()
{
    const int refreshRate =
        targetRefreshRate.load(std::memory_order_relaxed);
    if (!acceleratedRenderer.load(std::memory_order_relaxed)) {
        return std::min(refreshRate, 60);
    }
    return refreshRate;
}

bool sdl_renderer::isAcceleratedRenderer()
{
    return acceleratedRenderer.load(std::memory_order_relaxed);
}

bool sdl_renderer::isVsyncEnabled()
{
    return vsyncEnabled.load(std::memory_order_relaxed);
}

bool sdl_renderer::isDpiAwarenessReliable()
{
    return dpiAwarenessReliable.load(std::memory_order_relaxed);
}

bool sdl_renderer::isGameOnSingleMonitor()
{
    return gameOnSingleMonitor.load(std::memory_order_relaxed);
}

void sdl_renderer::setInteractiveRect(
    float x,
    float y,
    float width,
    float height)
{
    interactiveRect.left = static_cast<LONG>(std::floor(x));
    interactiveRect.top = static_cast<LONG>(std::floor(y));
    interactiveRect.right =
        static_cast<LONG>(std::ceil(x + std::max(0.0f, width)));
    interactiveRect.bottom =
        static_cast<LONG>(std::ceil(y + std::max(0.0f, height)));
}

uint32_t sdl_renderer::getDpiRevision()
{
    return dpiRevision;
}

uint64_t sdl_renderer::getRendererRevision()
{
    return rendererRevision.load(std::memory_order_relaxed);
}

// 基础绘图函数：ESP 画框/画线用的几个 SDL 封装
void sdl_renderer::draw::line(int x1, int y1, int x2, int y2, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
}

void sdl_renderer::draw::box(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderDrawRect(renderer, &rect);

    SDL_Rect rect2 = { x - 1, y - 1, w + 2, h + 2 };
    SDL_RenderDrawRect(renderer, &rect2);
}

void sdl_renderer::draw::filledBox(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(renderer, &rect);
}

// ----------------------------------------------------------------------------
// sdl_renderer::initImGui：初始化 ImGui（界面库）
// ----------------------------------------------------------------------------
// 1. 创建 ImGui 上下文；
// 2. 设置深色主题配色（窗口/按钮/文字等颜色）；
// 3. 记录一份"基准样式"，之后按 DPI 缩放时始终从基准缩放（避免反复缩放漂移）；
// 4. 用 SDL 渲染器后端初始化 ImGui。
// 注意：因为覆盖层是"颜色键"透明（不是真 alpha 透明），菜单必须完全不透明
// （style.Alpha = 1.0f），否则会和透明键混色产生暗色光晕。
bool sdl_renderer::initImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImFontConfig fontConfig;
    fontConfig.SizePixels = BASE_FONT_SIZE;
    fontConfig.OversampleH = 1;
    fontConfig.OversampleV = 1;
    io.Fonts->AddFontDefaultVector(&fontConfig);
    // 合入中文字体，否则中文 UI 会显示成问号
    addCjkFallbackFont(io.Fonts, fontConfig);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.045f, 0.060f, 0.085f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.070f, 0.090f, 0.125f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.060f, 0.078f, 0.108f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.145f, 0.185f, 0.240f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.135f, 0.180f, 0.235f, 1.0f);
    style.Colors[ImGuiCol_Text] =
        ImVec4(0.920f, 0.945f, 0.980f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.500f, 0.555f, 0.640f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.095f, 0.125f, 0.170f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.125f, 0.175f, 0.235f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.145f, 0.215f, 0.300f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.100f, 0.310f, 0.510f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.120f, 0.410f, 0.670f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.090f, 0.260f, 0.440f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.090f, 0.255f, 0.410f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.110f, 0.350f, 0.565f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.100f, 0.300f, 0.490f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.250f, 0.790f, 1.000f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.250f, 0.730f, 0.980f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.390f, 0.840f, 1.000f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.075f, 0.115f, 0.165f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.110f, 0.350f, 0.565f, 1.0f);
    style.Colors[ImGuiCol_TabSelected] =
        ImVec4(0.100f, 0.300f, 0.490f, 1.0f);
    for (ImVec4& color : style.Colors) {
        color.w = 1.0f;
    }
    style.FontSizeBase = BASE_FONT_SIZE;
    style.FontScaleMain = 1.0f;
    style.FontScaleDpi = 1.0f;
    style.WindowRounding = 12.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 9.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 7.0f;
    style.TabRounding = 7.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    // Color-keyed layered windows cannot preserve true per-pixel translucency.
    // Keep the menu opaque so it does not blend against the reserved key and
    // leave a dark halo over the game.
    style.Alpha = 1.0f;
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.FramePadding = ImVec2(9.0f, 6.0f);
    style.ItemSpacing = ImVec2(9.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 6.0f);
    style.ScrollbarSize = 13.0f;
    style.TabBarBorderSize = 1.0f;

    baseImGuiStyle = style;
    baseImGuiStyleReady = true;
    applyImGuiDpiScale(true);

    const bool platformInitialized =
        ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    const bool rendererInitialized =
        platformInitialized && ImGui_ImplSDLRenderer2_Init(renderer);
    if (!rendererInitialized) {
        if (platformInitialized) {
            ImGui_ImplSDL2_Shutdown();
        }
        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
        baseImGuiStyleReady = false;
        return false;
    }
    imguiInitialized = true;
    return true;
}

void sdl_renderer::shutdownImGui()
{
    if (!imguiInitialized) {
        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
        baseImGuiStyleReady = false;
        return;
    }
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    imguiInitialized = false;
    baseImGuiStyleReady = false;
}

void sdl_renderer::newFrameImGui()
{
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void sdl_renderer::renderImGui()
{
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
}
