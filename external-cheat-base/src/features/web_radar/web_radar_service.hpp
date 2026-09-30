// ============================================================================
// web_radar_service.hpp —— 内嵌网页雷达服务（基于 CivetWeb）
//
// 【这个文件是干什么的】
// 定义 WebRadarService：一个内嵌在程序里的 HTTP + WebSocket 服务器。
// 浏览器打开它提供的网页（web-radar/dist），并通过 WebSocket 实时接收
// 雷达快照。真正的实现在 web_radar_service.cpp。
//
// 【设计要点】
// - RAII：对象销毁自动停服（析构函数调 stop()）；
// - publish() 不阻塞网络 I/O：每个观众只有"一个待发送的最新快照"槽位，
//   慢观众直接跳过旧帧，而不是攒一个无限队列拖慢所有人；
// - start()/stop() 都是幂等的（重复调用安全）。
// ============================================================================

#pragma once

#include "web_radar_config.hpp"    // 服务配置

#include <cstddef>
#include <cstdint>
#include <memory>      // std::unique_ptr
#include <string>

namespace web_radar
{
    // 服务状态机：停止 → 启动中 → 运行中 → 停止中 → 失败
    enum class WebRadarServiceState : std::uint8_t
    {
        stopped,
        starting,
        running,
        stopping,
        failed
    };

    // 服务运行状态（菜单里显示）
    struct WebRadarStatus
    {
        WebRadarServiceState state = WebRadarServiceState::stopped;
        std::string bindAddress = "127.0.0.1";  // 监听地址
        std::uint16_t port = 22006;             // 端口
        std::size_t viewerCount = 0;            // 观众数
        std::uint64_t publishedFrames = 0;      // 已发布帧数
        std::uint64_t sentFrames = 0;           // 已发送帧数
        std::uint64_t replacedFrames = 0;       // 被新帧顶掉的旧帧数
        std::uint64_t publishedBytes = 0;       // 发布字节数
        double maximumSendLatencyMilliseconds = 0.0;  // 最大发送延迟
        std::string lastError;                  // 最近错误

        [[nodiscard]] bool isRunning() const noexcept
        {
            return state == WebRadarServiceState::running;
        }
    };

    // 内嵌 CivetWeb HTTP/WebSocket 服务器的 RAII 持有者。
    //
    // publish() 对网络 I/O 来说是不阻塞的。每个观众只有一个可替换的
    // 待发送快照，所以慢观众会跳过过期的状态，而不是攒无界队列或拖慢
    // 其他观众。
    class WebRadarService final
    {
    public:
        explicit WebRadarService(WebRadarConfig config);  // 构造（只保存配置）
        ~WebRadarService();                               // 析构：自动停服

        // 禁止复制/移动（服务器资源不能复制）
        WebRadarService(const WebRadarService&) = delete;
        WebRadarService& operator=(const WebRadarService&) = delete;
        WebRadarService(WebRadarService&&) = delete;
        WebRadarService& operator=(WebRadarService&&) = delete;

        // 启动服务。幂等。成功返回 true；失败时 status().lastError 里有
        // 给用户看的错误原因。
        [[nodiscard]] bool start() noexcept;

        // 停止服务。幂等。会等待有界时间的写线程和 CivetWeb 服务器线程结束。
        void stop() noexcept;

        // 发布一帧完整的、绝对的 JSON 状态。空快照被忽略。
        // 调用方可以在本调用返回后继续复用这份不可变的共享负载。
        void publish(std::shared_ptr<const std::string> absoluteState);

        [[nodiscard]] bool isRunning() const noexcept;
        [[nodiscard]] std::size_t viewerCount() const noexcept;
        [[nodiscard]] WebRadarStatus status() const;

        // 纯校验函数（单元测试和配置界面共用）。
        // 返回空串 = 配置合法。
        [[nodiscard]] static std::string validateConfig(
            const WebRadarConfig& config);

    private:
        class Impl;                              // 实现细节类（隐藏复杂度）
        std::unique_ptr<Impl> impl_;             // 指向实现（PImpl 惯用法）
    };
}
