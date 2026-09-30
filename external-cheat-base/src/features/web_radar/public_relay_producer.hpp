// ============================================================================
// public_relay_producer.hpp —— 公网中继的"生产端"（客户端）
//
// 【这个文件是干什么的】
// PublicRelayProducer 负责把雷达快照通过 wss:// 出站连接推给 relay 服务器。
// 它只"往外连"（游戏电脑不开放任何入站端口），网络工作全部放在一个
// 后台线程里，游戏采样永远不用等网络。
//
// 【设计要点】
// - publish() 只替换"最新一帧"的槽位：发得比网络快时，只有最新的一帧
//   被保留，旧帧被丢弃（对雷达这种实时画面，丢旧帧比排队更好）；
// - requestStop() 和 stop() 分开：requestStop 可以从 UI 线程安全调用
//   （只是发信号）；stop 要等网络线程收尾，所以留给后台线程调用
//   （见 main.cpp 的 PublicRelayRuntime，避免阻塞 UI）。
// ============================================================================

#pragma once

#include "public_relay_config.hpp"   // 配置、状态、校验函数

#include <memory>      // std::unique_ptr
#include <string>

namespace web_radar
{
    // 托管 Relay 的出站 WSS 生产端。
    // 网络工作隔离在一个后台线程，publish() 只替换"最新状态"槽位，
    // 所以游戏采样永远不会等待互联网。
    class PublicRelayProducer final
    {
    public:
        explicit PublicRelayProducer(PublicRelayConfig config);
        ~PublicRelayProducer();

        // 禁止复制/移动
        PublicRelayProducer(const PublicRelayProducer&) = delete;
        PublicRelayProducer& operator=(const PublicRelayProducer&) = delete;
        PublicRelayProducer(PublicRelayProducer&&) = delete;
        PublicRelayProducer& operator=(PublicRelayProducer&&) = delete;

        // 启动后台网络线程并尝试连接
        [[nodiscard]] bool start() noexcept;

        // 通知工作线程"该走了"，但不等待同步的 WinHTTP 调用返回。
        // 可以安全地从 UI 线程调用；stop() 特意留给后台"收尸"线程，
        // 因为 WebSocket 发送没有文档化的硬性取消时限。
        void requestStop() noexcept;
        void stop() noexcept;

        // 发布一帧快照。对 DNS、TLS 和 socket I/O 来说不阻塞。
        // 空帧被忽略。当调用频率超过网络发送能力时，只保留最新一帧。
        void publish(std::shared_ptr<const std::string> absoluteState);

        // 获取当前状态（连接状态、已发送帧数、重连次数等）
        [[nodiscard]] PublicRelayStatus status() const;

    private:
        class Impl;                              // 实现细节类
        std::unique_ptr<Impl> impl_;             // PImpl 惯用法
    };
}
