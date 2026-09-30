// ============================================================================
// diagnostics.hpp —— 日志与安装诊断
//
// 【这个文件是干什么的】
// 1. log()：把一条消息写到两处——Windows 调试输出（开发者用）和
//    临时目录下的日志文件 %TEMP%\cs2-esp.log（带时间戳）。
// 2. inspectInstallation()：程序启动时检查"运行环境是否完整"：
//    是否以管理员身份运行、SDL2.dll 是否在旁边、网页雷达文件是否齐全。
//    缺哪样就在菜单的"启动报告"里告诉用户，避免运行时才莫名其妙报错。
// ============================================================================

#pragma once

#include <Windows.h>      // Windows API（文件、调试输出等）
#include <shlobj.h>       // Shell API
#include <cwchar>         // 宽字符字符串函数
#include <filesystem>     // 文件系统路径
#include <string>         // std::string

namespace diagnostics
{
    // 启动检查报告：每一项是否满足 + 出错时的提示信息
    struct StartupReport
    {
        bool administrator = false;          // 是否以管理员运行
        bool sdlRuntimePresent = false;      // SDL2.dll 是否在旁边
        bool webRadarBundlePresent = false;  // 网页雷达打包文件（index.html）是否齐全
        bool mapMetadataPresent = false;     // 地图元数据（manifest.json / SOURCE.json）是否齐全
        std::string installationError;       // 如果没有完全就绪，这里放第一条错误信息

        // 全部满足才算"就绪"
        [[nodiscard]] bool ready() const noexcept
        {
            return administrator && sdlRuntimePresent &&
                webRadarBundlePresent && mapMetadataPresent;
        }
    };

    // 当前进程是否以管理员身份运行（IsUserAnAdmin 是 Windows 提供的 API）
    inline bool isAdministrator() noexcept
    {
        return IsUserAnAdmin() != FALSE;
    }

    // ------------------------------------------------------------------
    // inspectInstallation：检查安装/运行环境，返回报告
    // ------------------------------------------------------------------
    // 参数 documentRoot：网页雷达文件的目录（web-radar/dist）
    // 依次检查四个条件，缺一就把错误信息填进报告
    inline StartupReport inspectInstallation(
        const std::filesystem::path& documentRoot)
    {
        StartupReport report;
        report.administrator = isAdministrator();

        // error_code 版本的文件系统 API：失败时不会抛异常，而是把错误码放这里
        // （这是"不抛异常的写法"，避免启动流程被中断）
        std::error_code error;
        // 网页目录的上一级上一级 = exe 所在目录（web-radar/dist 的 parent 是 web-radar，
        // 再 parent 就是 exe 目录）。SDL2.dll 应该和 exe 放在一起。
        const std::filesystem::path executableDirectory =
            documentRoot.parent_path().parent_path();
        report.sdlRuntimePresent = std::filesystem::is_regular_file(
            executableDirectory / L"SDL2.dll",
            error);
        error.clear();
        // 网页打包是否完整（有 index.html 才算有）
        report.webRadarBundlePresent = std::filesystem::is_regular_file(
            documentRoot / L"index.html",
            error);
        error.clear();
        // 地图清单和素材来源声明都要在
        const bool manifestPresent = std::filesystem::is_regular_file(
            documentRoot / L"maps" / L"manifest.json",
            error);
        error.clear();
        const bool sourcePresent = std::filesystem::is_regular_file(
            documentRoot / L"maps" / L"SOURCE.json",
            error);
        report.mapMetadataPresent = manifestPresent && sourcePresent;

        // 按优先级挑出第一条错误信息（if/else if 链）
        if (!report.administrator) {
            report.installationError =
                "Process is not running with the required administrator token";
        } else if (!report.sdlRuntimePresent) {
            report.installationError = "SDL2.dll is missing beside the executable";
        } else if (!report.webRadarBundlePresent) {
            report.installationError = "web-radar/dist/index.html is missing";
        } else if (!report.mapMetadataPresent) {
            report.installationError =
                "Web Radar map manifest or SOURCE metadata is missing";
        }
        return report;
    }

    // ------------------------------------------------------------------
    // log：写一条日志（同时到调试输出和文件）
    // ------------------------------------------------------------------
    inline void log(const wchar_t* message)
    {
        if (!message) {          // 空消息直接忽略
            return;
        }

        // 写到 Windows 调试输出（配合 DebugView 等工具查看）
        OutputDebugStringW(message);
        OutputDebugStringW(L"\r\n");

        // 拿到系统临时目录路径（比如 C:\Users\xxx\AppData\Local\Temp）
        wchar_t tempPath[MAX_PATH]{};
        const DWORD pathLength =
            GetTempPathW(MAX_PATH, tempPath);
        if (pathLength == 0 || pathLength >= MAX_PATH) {
            return;
        }

        // 拼出日志文件完整路径：<临时目录>\cs2-esp.log
        wchar_t logPath[MAX_PATH]{};
        if (swprintf_s(
                logPath,
                L"%scs2-esp.log",
                tempPath) <= 0) {   // 拼接失败（返回 ≤0）
            return;
        }

        // 打开（或创建）日志文件，以"追加"模式写
        HANDLE file = CreateFileW(
            logPath,
            FILE_APPEND_DATA,                       // 追加写
            FILE_SHARE_READ | FILE_SHARE_WRITE,     // 允许其他程序同时读/写
            nullptr,                                // 无安全属性
            OPEN_ALWAYS,                            // 不存在就创建
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {         // 打开失败
            return;
        }

        // 拼时间戳 + 消息：形如 [2026-08-12 23:30:00] xxx
        SYSTEMTIME time{};
        GetLocalTime(&time);                        // 当前本地时间
        wchar_t line[1024]{};
        const int characters = swprintf_s(
            line,
            L"[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond,
            message);
        if (characters > 0) {                       // 格式化成功
            DWORD bytesWritten = 0;
            WriteFile(                              // 写入文件
                file,
                line,
                static_cast<DWORD>(
                    characters * sizeof(wchar_t)),  // 字节数 = 字符数 × 每字符字节数
                &bytesWritten,
                nullptr);
        }
        CloseHandle(file);                          // 写完关闭
    }
}
