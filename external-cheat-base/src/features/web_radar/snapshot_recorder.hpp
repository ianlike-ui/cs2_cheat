// ============================================================================
// snapshot_recorder.hpp —— 脱敏雷达录制器（把快照录成 NDJSON 文件）
//
// 【这个文件是干什么的】
// 把脱敏后的雷达快照（JSON）一行一条写进文件（NDJSON 格式），
// 之后可以在网页雷达里加载回放。文件放在 %LOCALAPPDATA%\AegisCS2\recordings。
//
// 【NDJSON 是什么？】
// JSON Lines：每行是一个独立的完整 JSON 对象。好处是"边录边写、边读边解"，
// 不必等整个文件写完才能解析（CSV 的灵活版，JSON 的流式版）。
//
// 【线程模型】
// - 数据线程调用 publish()：只把最新帧放进"待写槽位"（锁 + 条件变量）；
// - 后台写线程（worker_）在 writeLoop 里真正写文件，写不动时旧帧被顶掉
//   （replacedFrames 计数），不会积压。
//
// 【给 C++ 初学者的概念】
// - std::condition_variable：条件变量。一个线程"等"，另一个线程"叫醒"它。
// - std::ofstream：写文件的输出流。
// - RAII：析构函数里自动 stop()（停线程、关文件）。
// ============================================================================

#pragma once

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace web_radar
{
    // 录制器状态（菜单里显示）
    struct SnapshotRecorderStatus
    {
        bool recording = false;         // 是否正在录制
        std::uint64_t framesWritten = 0;  // 已写入帧数
        std::uint64_t replacedFrames = 0; // 被顶掉的旧帧数
        std::uint64_t bytesWritten = 0;   // 已写字节数
        std::string path;               // 当前录制文件路径
        std::string lastError;          // 最近错误
    };

    // 录制器：单个后台线程负责写文件
    class SnapshotRecorder final
    {
    public:
        // 构造：指定录制文件存放目录
        explicit SnapshotRecorder(std::filesystem::path directory)
            : directory_(std::move(directory))
        {
        }

        // 析构：确保停止录制
        ~SnapshotRecorder()
        {
            stop();
        }

        // 禁止复制
        SnapshotRecorder(const SnapshotRecorder&) = delete;
        SnapshotRecorder& operator=(const SnapshotRecorder&) = delete;

        // 同步开关：界面开关变了就启动/停止
        void sync(const bool enabled)
        {
            if (enabled) {
                start();
            } else {
                stop();
            }
        }

        // 数据线程调用：发布一帧快照
        // 空帧忽略；如果上一帧还没写完，旧帧被替换并计数
        void publish(std::shared_ptr<const std::string> frame)
        {
            if (!frame || frame->empty()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_ || stopping_) {     // 没在录就不收
                    return;
                }
                if (pending_) {                   // 上一帧还没写 → 顶掉它
                    ++status_.replacedFrames;
                }
                pending_ = std::move(frame);      // 放进待写槽位
            }
            changed_.notify_one();                // 叫醒写线程
        }

        // 获取状态（加锁拷贝）
        [[nodiscard]] SnapshotRecorderStatus status() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return status_;
        }

        // 停止：等写线程把当前帧写完再退出
        void stop() noexcept
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_) {                  // 没在跑就直接返回
                    return;
                }
                stopping_ = true;                 // 标记"要停了"
            }
            changed_.notify_one();                // 叫醒写线程让它检查
            if (worker_.joinable()) {
                worker_.join();                   // 等写线程结束
            }
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            stopping_ = false;
            pending_.reset();
            status_.recording = false;
        }

    private:
        // 录制文件大小上限：256 MB（防止磁盘被录满）
        inline static constexpr std::uint64_t maximumBytes =
            UINT64_C(256) * 1024U * 1024U;

        // 启动录制：建目录 → 生成带时间戳的文件名 → 启动写线程
        void start()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_) {
                return;                           // 已经在录了
            }

            std::error_code error;
            std::filesystem::create_directories(directory_, error);
            if (error) {
                status_.lastError =
                    "Could not create recording directory: " +
                    error.message();
                return;
            }

            const std::filesystem::path path = directory_ / fileName();
            status_ = {};                         // 清空旧状态
            status_.path = path.string();
            status_.recording = true;
            running_ = true;
            stopping_ = false;
            worker_ = std::thread([this, path] { writeLoop(path); });  // 启动写线程
        }

        // 生成文件名：radar-YYYYMMDD-HHMMSS.ndjson
        [[nodiscard]] static std::string fileName()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &now);            // Windows 版线程安全取本地时间
#else
            localtime_r(&now, &local);
#endif
            std::ostringstream name;
            name << "radar-" << std::put_time(&local, "%Y%m%d-%H%M%S")
                 << ".ndjson";
            return name.str();
        }

        // 写线程主体：循环等帧 → 写文件 → 每 20 帧刷盘一次
        void writeLoop(const std::filesystem::path& path) noexcept
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) {
                fail("Could not open the recording file");
                return;
            }

            std::uint32_t framesSinceFlush = 0;
            for (;;) {
                std::shared_ptr<const std::string> frame;
                {
                    // 等"要停止"或"有待写帧"
                    std::unique_lock<std::mutex> lock(mutex_);
                    changed_.wait(lock, [this] {
                        return stopping_ || pending_ != nullptr;
                    });
                    if (stopping_ && !pending_) { // 停止且没有待写帧 → 退出
                        break;
                    }
                    frame.swap(pending_);         // 取走待写帧
                }

                // 检查 256MB 上限
                const std::uint64_t nextBytes =
                    static_cast<std::uint64_t>(frame->size()) + 1U;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (status_.bytesWritten + nextBytes > maximumBytes) {
                        status_.lastError =
                            "Recording stopped at the 256 MB safety limit";
                        stopping_ = true;
                        status_.recording = false;
                        pending_.reset();
                        break;
                    }
                }

                // 写入一行 JSON + 换行（NDJSON 格式）
                output.write(frame->data(),
                    static_cast<std::streamsize>(frame->size()));
                output.put('\n');
                if (!output) {
                    fail("Writing the recording file failed");
                    return;
                }

                // 更新统计
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++status_.framesWritten;
                    status_.bytesWritten += nextBytes;
                }
                // 每 20 帧把缓冲刷到磁盘一次（平衡性能与崩溃丢失量）
                if (++framesSinceFlush >= 20) {
                    output.flush();
                    framesSinceFlush = 0;
                }
            }
            output.flush();
        }

        // 出错统一处理：记录错误、停止录制
        void fail(std::string message) noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.lastError = std::move(message);
            status_.recording = false;
            stopping_ = true;
            pending_.reset();
        }

        // 成员变量
        std::filesystem::path directory_;         // 录制目录
        mutable std::mutex mutex_;                // 保护下面所有状态的锁
        std::condition_variable changed_;         // 通知写线程的条件变量
        std::shared_ptr<const std::string> pending_;  // 待写的最新帧
        SnapshotRecorderStatus status_;           // 状态
        bool running_ = false;                    // 是否在录
        bool stopping_ = false;                   // 是否正在停止
        std::thread worker_;                      // 写线程
    };
}
