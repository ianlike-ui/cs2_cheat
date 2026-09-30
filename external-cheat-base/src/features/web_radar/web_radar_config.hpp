// ============================================================================
// web_radar_config.hpp —— 内嵌网页雷达服务的配置结构
//
// 【这个文件是干什么的】
// 定义 WebRadarService 启动时需要的配置项：监听地址、端口、网页文件目录、
// 访问令牌、连接数上限、线程数、超时等。真正的服务实现在
// web_radar_service.cpp。
//
// 【安全默认值（重要）】
// bindAddress 默认是 "127.0.0.1"（只允许本机访问）。想给局域网看，
// 调用方必须显式改成 "0.0.0.0"。这是刻意设计：默认安全，开放需显式操作。
// token 是 URL 安全字符组成的随机串（至少 128 位熵），没有它看不了雷达。
// ============================================================================

#pragma once

#include <cstddef>     // std::size_t
#include <cstdint>     // uint16_t / uint32_t
#include <string>      // std::string

namespace web_radar
{
    // 网页雷达服务的全部配置
    struct WebRadarConfig
    {
        // 默认只监听本机（安全）。
        // 调用方必须显式选择"0.0.0.0"这类监听地址才会开放局域网访问。
        std::string bindAddress = "127.0.0.1";
        std::uint16_t port = 22006;             // HTTP 端口
        std::string documentRoot = "web-radar/dist";  // 网页文件目录

        // URL 安全的访问令牌（A-Z, a-z, 0-9, '_' 和 '-'）。
        // 至少要有 128 位随机熵；程序生成的是 32 字符随机串。
        std::string token;

        std::size_t maxViewers = 16;            // 最多同时多少个网页观众
        std::uint32_t workerThreads = 24;       // Web 服务的线程数
        std::uint32_t requestTimeoutMilliseconds = 2000;   // 请求超时（毫秒）
        std::uint32_t websocketTimeoutMilliseconds = 15000; // WebSocket 超时（毫秒）
    };
}
