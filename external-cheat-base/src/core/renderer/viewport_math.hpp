// ============================================================================
// viewport_math.hpp —— 覆盖层视口（游戏画面区域）的几何计算
//
// 【这个文件是干什么的】
// 覆盖层窗口和游戏窗口不一定完全重合（比如游戏用 4:3 拉伸模式、或者
// 窗口带边框）。"视口"就是"游戏画面实际占用的那块屏幕区域"。
// ESP 画框、鼠标拦截都需要把坐标算到正确的视口里，否则画的位置会偏。
// 本文件提供三种计算：
//   - forAspect：按宽高比裁出画面区域（处理 4:3 / 16:10 拉伸）；
//   - remap：游戏窗口大小变化时，按比例缩放视口；
//   - validProjectedBox：判断投影出的方框尺寸是否合理。
//
// 【给 C++ 初学者的概念】
// - struct：只装数据的结构体。这里 Viewport 装一个矩形（x,y,宽,高）。
// - std::lround：四舍五入成整数。
// - std::clamp(v, lo, hi)：把 v 限制在 [lo, hi] 范围内。
// ============================================================================

#pragma once

#include <algorithm>    // std::clamp
#include <cmath>        // std::isfinite、std::lround

namespace viewport_math
{
    // 视口：一个矩形区域（屏幕坐标）
    struct Viewport
    {
        int x = 0;          // 左上角 X
        int y = 0;          // 左上角 Y
        int width = 0;      // 宽
        int height = 0;     // 高
    };

    // 两个视口是否完全相同（判断游戏窗口有没有变化）
    inline bool same(const Viewport& lhs, const Viewport& rhs)
    {
        return lhs.x == rhs.x &&
            lhs.y == rhs.y &&
            lhs.width == rhs.width &&
            lhs.height == rhs.height;
    }

    // ------------------------------------------------------------------
    // forAspect：按目标宽高比裁出画面区域
    // ------------------------------------------------------------------
    // 例子：游戏实际画面是 16:9，但用户把窗口拉成了 4:3。
    // 游戏会用"信箱模式"（左右黑边）保持 16:9 画面。这个函数就算出
    // 画面实际占用的矩形（去掉黑边）。
    // 逻辑：窗口比目标比例"宽" → 宽度要缩（左右黑边）；"高" → 高度要缩（上下黑边）。
    inline Viewport forAspect(
        int clientWidth,        // 窗口客户区宽度
        int clientHeight,       // 窗口客户区高度
        float targetAspect)     // 目标宽高比（如 16.0/9.0）
    {
        Viewport result{ 0, 0, clientWidth, clientHeight };
        if (clientWidth <= 0 ||
            clientHeight <= 0 ||
            !std::isfinite(targetAspect) ||
            targetAspect <= 0.0f) {
            return result;      // 参数不合法：直接返回整个窗口
        }

        const float clientAspect =
            static_cast<float>(clientWidth) /
            static_cast<float>(clientHeight);
        if (clientAspect > targetAspect) {
            // 窗口比目标宽 → 压缩宽度，左右居中留黑边
            result.width = static_cast<int>(
                std::lround(
                    static_cast<float>(clientHeight) * targetAspect));
            result.x = (clientWidth - result.width) / 2;
        } else if (clientAspect < targetAspect) {
            // 窗口比目标高 → 压缩高度，上下居中留黑边
            result.height = static_cast<int>(
                std::lround(
                    static_cast<float>(clientWidth) / targetAspect));
            result.y = (clientHeight - result.height) / 2;
        }
        // 恰好相等 → 整个窗口就是视口
        return result;
    }

    // 按游戏"显示模式"直接算视口：模式 2 = 4:3 拉伸，模式 3 = 16:10 拉伸
    inline Viewport forMode(
        int mode,
        int clientWidth,
        int clientHeight)
    {
        switch (mode) {
            case 2:    // 4:3
                return forAspect(
                    clientWidth,
                    clientHeight,
                    4.0f / 3.0f);
            case 3:    // 16:10
                return forAspect(
                    clientWidth,
                    clientHeight,
                    16.0f / 10.0f);
            default:   // 其他（含 16:9）：用整个窗口
                return Viewport{
                    0,
                    0,
                    clientWidth,
                    clientHeight
                };
        }
    }

    // ------------------------------------------------------------------
    // remap：窗口大小变了之后，按比例缩放旧视口到新窗口
    // ------------------------------------------------------------------
    // 比如游戏窗口从 1920x1080 变成 1600x900，视口坐标要跟着等比缩放。
    // 最后再夹取一遍，保证视口不超出新窗口。
    inline Viewport remap(
        const Viewport& viewport,        // 旧视口
        int oldClientWidth,              // 旧窗口宽
        int oldClientHeight,             // 旧窗口高
        int newClientWidth,              // 新窗口宽
        int newClientHeight)             // 新窗口高
    {
        if (oldClientWidth <= 0 ||
            oldClientHeight <= 0 ||
            newClientWidth <= 0 ||
            newClientHeight <= 0 ||
            viewport.width <= 0 ||
            viewport.height <= 0) {
            return Viewport{             // 参数不合法：退回整个新窗口
                0,
                0,
                newClientWidth,
                newClientHeight
            };
        }

        // 局部小工具：把旧坐标按比例换算到新范围（四舍五入）
        const auto scaleCoordinate = [](
            int value,
            int oldExtent,
            int newExtent) {
            return static_cast<int>(std::lround(
                static_cast<double>(value) *
                static_cast<double>(newExtent) /
                static_cast<double>(oldExtent)));
        };

        // 四个值各自等比缩放
        Viewport result{
            scaleCoordinate(
                viewport.x,
                oldClientWidth,
                newClientWidth),
            scaleCoordinate(
                viewport.y,
                oldClientHeight,
                newClientHeight),
            scaleCoordinate(
                viewport.width,
                oldClientWidth,
                newClientWidth),
            scaleCoordinate(
                viewport.height,
                oldClientHeight,
                newClientHeight)
        };
        // 夹取：坐标不小于 0、不超窗口；宽高不超剩余空间
        result.x = std::clamp(
            result.x,
            0,
            newClientWidth);
        result.y = std::clamp(
            result.y,
            0,
            newClientHeight);
        result.width = std::clamp(
            result.width,
            0,
            newClientWidth - result.x);
        result.height = std::clamp(
            result.height,
            0,
            newClientHeight - result.y);
        return result;
    }

    // ------------------------------------------------------------------
    // validProjectedBox：判断投影出来的方框尺寸是否"合理"
    // ------------------------------------------------------------------
    // ESP 把敌人投影到屏幕上画方框。如果敌人太近，方框会巨大无比；
    // 投影异常时高度可能是 NaN 或负数。这里统一校验：
    // 高度必须 ≥ 1 像素，且不超过视口高度的 2 倍，才认为合理。
    inline bool validProjectedBox(
        float height,             // 投影出的方框高度（像素）
        float viewportHeight)     // 视口高度
    {
        return std::isfinite(height) &&
            std::isfinite(viewportHeight) &&
            height >= 1.0f &&
            viewportHeight > 0.0f &&
            height <= viewportHeight * 2.0f;
    }
}
