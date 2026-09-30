// ============================================================================
// public_relay_config.hpp —— 公网中继（Public Relay）的配置与校验
//
// 【这个文件是干什么的】
// 1. 定义中继连接需要的配置（服务器 URL、房间名、producer token、超时…）；
// 2. 提供严格的 URL 解析（只允许 wss://、拒绝用户信息/查询串/片段）；
// 3. 提供配置校验函数（房间名、token、超时范围、快照大小上限…）；
// 4. 提供连接失败后的指数退避算法（1s、2s、4s…30s 封顶，带抖动）。
//
// 【为什么校验这么严？】
// - 只允许 wss://（加密的 WebSocket）：明文 ws:// 会把雷达数据裸奔在网络上；
// - 拒绝 URL 里带用户名/密码（userinfo）：防止凭据被意外拼进 URL 或日志；
// - 拒绝查询串/片段：防止 token 出现在访问日志里。
// 这些校验同时用在界面测试和 Windows 传输层，保证两边行为一致。
// ============================================================================

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace web_radar
{
    // 解析后的中继端点（host + 端口 + 路径）
    struct PublicRelayEndpoint
    {
        std::string host;           // 服务器主机名
        std::uint16_t port = 443;   // 端口（默认 443 = HTTPS/WSS）
        std::string path = "/";     // 路径
    };

    // 中继配置（连接参数 + 超时 + 大小上限）
    struct PublicRelayConfig
    {
        // producer 的 WebSocket 端点，通常是
        // wss://radar.example.com/api/v1/publish。
        // 明文 ws:// 永远不会被接受，且必须通过 WinHTTP 的证书校验。
        std::string endpointUrl;
        std::string room;           // 房间名（把不同的比赛隔离开）
        std::string token;          // producer token（推流专用凭证）

        std::uint32_t resolveTimeoutMilliseconds = 5000;  // DNS 解析超时
        std::uint32_t connectTimeoutMilliseconds = 5000;  // 建连超时
        std::uint32_t sendTimeoutMilliseconds = 5000;     // 发送超时
        std::uint32_t receiveTimeoutMilliseconds = 5000;  // 接收超时

        // 这些默认值和生产环境 Relay 一致。把两个上限都写清楚，
        // 防止一个过期或异常大的快照把弱网变成"反复重连"。
        // 部署方如果故意调过 Relay 的参数，可以同步调整 producer。
        std::uint32_t maxSnapshotBytes = 512U * 1024U;            // 单帧快照上限 512KB
        std::uint32_t maximumQueuedSnapshotAgeMilliseconds = 5000; // 排队快照最大年龄
    };

    // 中继连接的状态（菜单里显示用）
    enum class PublicRelayState : std::uint8_t
    {
        disabled,     // 未启用
        connecting,   // 连接中
        connected,    // 已连接
        backoff,      // 退避等待重连
        retiring,     // 正在收尾断开
        failed        // 已失败
    };

    // 中继的运行状态统计（菜单显示）
    struct PublicRelayStatus
    {
        PublicRelayState state = PublicRelayState::disabled;
        std::uint64_t framesSent = 0;      // 已发送帧数
        std::uint64_t replacedFrames = 0;  // 被新帧顶掉的旧帧数
        std::uint64_t droppedFrames = 0;   // 被丢弃的帧数
        std::uint64_t reconnects = 0;      // 重连次数
        std::string lastError;             // 最近错误
    };

    namespace detail
    {
        [[nodiscard]] inline bool relayAsciiAlphaNumeric(
            const unsigned char character) noexcept
        {
            return (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9');
        }

        [[nodiscard]] inline bool validRelayDnsHost(
            const std::string_view host) noexcept
        {
            if (host.empty() || host.size() > 253) {
                return false;
            }
            std::size_t labelStart = 0;
            while (labelStart < host.size()) {
                const std::size_t separator = host.find('.', labelStart);
                const std::size_t labelEnd =
                    separator == std::string_view::npos
                        ? host.size()
                        : separator;
                const std::size_t labelSize = labelEnd - labelStart;
                if (labelSize == 0 || labelSize > 63 ||
                    !relayAsciiAlphaNumeric(
                        static_cast<unsigned char>(host[labelStart])) ||
                    !relayAsciiAlphaNumeric(
                        static_cast<unsigned char>(host[labelEnd - 1]))) {
                    return false;
                }
                for (std::size_t index = labelStart;
                     index < labelEnd;
                     ++index) {
                    const auto character =
                        static_cast<unsigned char>(host[index]);
                    if (!relayAsciiAlphaNumeric(character) &&
                        character != '-') {
                        return false;
                    }
                }
                if (separator == std::string_view::npos) {
                    return true;
                }
                labelStart = separator + 1;
            }
            return false;
        }

        [[nodiscard]] inline bool relayStartsWithWss(
            const std::string_view value) noexcept
        {
            if (value.size() < 6) {
                return false;
            }
            constexpr std::string_view expected = "wss://";
            for (std::size_t index = 0; index < expected.size(); ++index) {
                const auto character = static_cast<unsigned char>(value[index]);
                if (static_cast<char>(std::tolower(character)) != expected[index]) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] inline bool validRelayPort(
            const std::string_view text,
            std::uint16_t& parsed) noexcept
        {
            if (text.empty() || text.size() > 5) {
                return false;
            }
            std::uint32_t value = 0;
            for (const char rawCharacter : text) {
                const auto character = static_cast<unsigned char>(rawCharacter);
                if (character < '0' || character > '9') {
                    return false;
                }
                value = value * 10U + static_cast<std::uint32_t>(character - '0');
            }
            if (value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
                return false;
            }
            parsed = static_cast<std::uint16_t>(value);
            return true;
        }
    }

    // ------------------------------------------------------------------
    // parsePublicRelayEndpoint：严格解析中继 URL（不分配额外内存）
    // ------------------------------------------------------------------
    // 支持普通域名（example.com:443）和 [IPv6] 地址。
    // 规则：必须以 wss:// 开头；拒绝 userinfo、查询串、片段；
    // 路径字符必须是可见 ASCII。解析失败时返回空并（可选）给出错误信息。
    // 严格的、分配轻量的 URL 解析，由 UI 测试和 Windows 传输层共用。
    // 用户信息、查询串和片段都会被拒绝，这样凭据不会意外迁移进 URL 或访问日志。
    [[nodiscard]] inline std::optional<PublicRelayEndpoint>
    parsePublicRelayEndpoint(
        const std::string_view url,
        std::string* error = nullptr)
    {
        const auto reject = [error](const char* message)
            -> std::optional<PublicRelayEndpoint> {
            if (error != nullptr) {
                *error = message;
            }
            return std::nullopt;
        };

        if (url.empty() || url.size() > 2048) {
            return reject("Relay URL must contain 1 to 2048 characters.");
        }
        if (!detail::relayStartsWithWss(url)) {
            return reject("Relay URL must use wss://.");
        }

        const std::string_view remainder = url.substr(6);
        const std::size_t pathOffset = remainder.find('/');
        const std::string_view authority = remainder.substr(0, pathOffset);
        const std::string_view path = pathOffset == std::string_view::npos
            ? std::string_view{"/"}
            : remainder.substr(pathOffset);
        if (authority.empty()) {
            return reject("Relay URL is missing a host.");
        }
        if (authority.find('@') != std::string_view::npos) {
            return reject("Relay URL must not contain user information.");
        }
        if (path.find('?') != std::string_view::npos ||
            path.find('#') != std::string_view::npos) {
            return reject("Relay URL must not contain a query or fragment.");
        }
        for (const char rawCharacter : path) {
            const auto character = static_cast<unsigned char>(rawCharacter);
            if (character <= 0x20U || character >= 0x7fU || character == '\\') {
                return reject("Relay URL path contains an unsupported character.");
            }
        }

        PublicRelayEndpoint endpoint;
        endpoint.path.assign(path);
        std::string_view host;
        std::string_view portText;
        bool explicitPort = false;
        if (authority.front() == '[') {
            const std::size_t closingBracket = authority.find(']');
            if (closingBracket == std::string_view::npos || closingBracket == 1) {
                return reject("Relay URL contains an invalid IPv6 host.");
            }
            host = authority.substr(1, closingBracket - 1);
            const std::string_view suffix = authority.substr(closingBracket + 1);
            if (!suffix.empty()) {
                if (suffix.front() != ':') {
                    return reject("Relay URL contains an invalid host suffix.");
                }
                explicitPort = true;
                portText = suffix.substr(1);
            }
            if (host.size() > 45 ||
                host.find(':') == std::string_view::npos) {
                return reject("Relay URL contains an invalid IPv6 host.");
            }
            for (const char rawCharacter : host) {
                const auto character = static_cast<unsigned char>(rawCharacter);
                const bool hexadecimal =
                    (character >= 'a' && character <= 'f') ||
                    (character >= 'A' && character <= 'F') ||
                    (character >= '0' && character <= '9');
                if (!hexadecimal && character != ':' && character != '.') {
                    return reject("Relay URL contains an invalid IPv6 host.");
                }
            }
        } else {
            const std::size_t colon = authority.rfind(':');
            if (colon != std::string_view::npos) {
                if (authority.find(':') != colon) {
                    return reject("IPv6 hosts in Relay URLs must use brackets.");
                }
                host = authority.substr(0, colon);
                explicitPort = true;
                portText = authority.substr(colon + 1);
            } else {
                host = authority;
            }
            if (host.empty()) {
                return reject("Relay URL is missing a host.");
            }
            if (!detail::validRelayDnsHost(host)) {
                return reject("Relay URL contains an invalid host.");
            }
        }

        endpoint.host.assign(host);
        if (explicitPort &&
            !detail::validRelayPort(portText, endpoint.port)) {
            return reject("Relay URL contains an invalid port.");
        }
        if (error != nullptr) {
            error->clear();
        }
        return endpoint;
    }

    // ------------------------------------------------------------------
    // validatePublicRelayConfig：校验整份配置是否合法
    // ------------------------------------------------------------------
    // 依次检查：URL 可解析、房间名长度/字符集、token 长度/可见字符、
    // 各超时范围、快照大小上限。任一不合法就返回错误字符串（空串 = 合法）。
    [[nodiscard]] inline std::string validatePublicRelayConfig(
        const PublicRelayConfig& config)
    {
        std::string error;
        if (!parsePublicRelayEndpoint(config.endpointUrl, &error)) {
            return error;
        }
        if (config.room.size() < 3 || config.room.size() > 64) {
            return "Relay room must contain 3 to 64 characters.";
        }
        for (const char rawCharacter : config.room) {
            const auto character = static_cast<unsigned char>(rawCharacter);
            if (!detail::relayAsciiAlphaNumeric(character) &&
                character != '-' && character != '_') {
                return "Relay room may contain only letters, digits, '-' and '_'.";
            }
        }
        if (config.token.size() < 24 || config.token.size() > 512) {
            return "Relay producer token must contain 24 to 512 characters.";
        }
        for (const char rawCharacter : config.token) {
            const auto character = static_cast<unsigned char>(rawCharacter);
            if (character < 0x21U || character > 0x7eU) {
                return "Relay producer token must contain visible ASCII characters only.";
            }
        }
        const auto validTimeout = [](const std::uint32_t value) {
            return value >= 100 && value <= 30000;
        };
        if (!validTimeout(config.resolveTimeoutMilliseconds) ||
            !validTimeout(config.connectTimeoutMilliseconds) ||
            !validTimeout(config.sendTimeoutMilliseconds) ||
            !validTimeout(config.receiveTimeoutMilliseconds)) {
            return "Relay network timeouts must be between 100 and 30000 ms.";
        }
        if (config.maxSnapshotBytes < 4096U ||
            config.maxSnapshotBytes > 4U * 1024U * 1024U) {
            return "Relay snapshot limit must be between 4096 and 4194304 bytes.";
        }
        if (config.maximumQueuedSnapshotAgeMilliseconds < 250U ||
            config.maximumQueuedSnapshotAgeMilliseconds > 30000U) {
            return "Relay queued snapshot age must be between 250 and 30000 ms.";
        }
        return {};
    }

    [[nodiscard]] inline bool publicRelaySnapshotFits(
        const std::size_t snapshotBytes,
        const std::uint32_t maximumBytes) noexcept
    {
        return snapshotBytes != 0 && snapshotBytes <= maximumBytes;
    }

    [[nodiscard]] inline bool publicRelayQueuedSnapshotExpired(
        const std::chrono::milliseconds queuedFor,
        const std::uint32_t maximumAgeMilliseconds) noexcept
    {
        return queuedFor >
            std::chrono::milliseconds(maximumAgeMilliseconds);
    }

    // Authentication/configuration errors require user action. Capacity and
    // transient request statuses stay retryable so a producer can recover
    // without exposing transport-specific policy to the WinHTTP worker.
    [[nodiscard]] inline bool publicRelayHttpFailureIsPermanent(
        const std::uint32_t statusCode) noexcept
    {
        return statusCode >= 400U && statusCode < 500U &&
            statusCode != 408U && statusCode != 409U &&
            statusCode != 425U && statusCode != 429U;
    }

    inline constexpr auto publicRelayHealthyConnectionDuration =
        std::chrono::seconds(10);

    [[nodiscard]] inline bool publicRelayConnectionWasHealthy(
        const std::chrono::milliseconds connectedFor) noexcept
    {
        return connectedFor >= publicRelayHealthyConnectionDuration;
    }

    // 指数退避重连：1、2、4、8、16、30 秒封顶，外加确定性的 ±20% 抖动。
    // 抖动用传入的 entropy 计算，让纯函数容易测试；生产环境每次失败
    // 都传一个新随机值。作用是：多台机器同时断线重连时不会"齐步走"
    // 撞在一起（惊群效应）。
    [[nodiscard]] inline std::chrono::milliseconds publicRelayBackoffDelay(
        const std::uint32_t failureCount,
        const std::uint32_t entropy) noexcept
    {
        constexpr std::uint64_t initialMilliseconds = 1000;
        constexpr std::uint64_t maximumMilliseconds = 30000;
        const std::uint32_t shift = std::min(failureCount, 5U);
        const std::uint64_t exponential = std::min(
            initialMilliseconds << shift,
            maximumMilliseconds);
        const std::uint64_t jitterPercent = 80U + entropy % 41U;
        return std::chrono::milliseconds(std::min(
            exponential * jitterPercent / 100U,
            maximumMilliseconds));
    }
}
