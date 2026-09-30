// ============================================================================
// memory.hpp —— 内存读取模块的头文件（声明 + 模板函数 + 全局状态）
//
// 【这个文件包含三部分】
// 1. 全局状态：进程句柄、进程 ID、是否允许写内存、读取统计计数器；
// 2. 模板读函数：TryRead/Read/ReadRaw，封装 ReadProcessMemory；
// 3. 普通函数声明：真正的实现在 memory.cpp 里。
//
// ----------------------------------------------------------------------------
// 🎓 概念卡 ⑬：指针与内存地址（整个项目的地基，务必读懂）
// ----------------------------------------------------------------------------
// 【内存是什么】内存就是一长排"编号的格子"，每个格子 1 字节，编号就是"地址"。
// 程序里每个变量都住在某个地址：int 占 4 格（4 字节），double 占 8 格。
// 用 & 取一个变量的地址，用 * 根据地址取里面的值。
//
// 【什么是指针】指针就是"存着地址的变量"。uintptr_t 是"能装下一个地址的
// 整数类型"（64 位系统上就是 64 位无符号整数）。
//
// 【为什么本项目的核心就是"算地址"】
// 读游戏内存的本质是：
//   目标地址 = 模块基地址 + 偏移量
// 模块基地址：client.dll 被加载到内存的起始位置（每次启动可能不同）；
// 偏移量：cs2-dumper 挖出来的"相对模块开头差多少字节"。
// 有了目标地址，ReadProcessMemory 就能把游戏内存里的数据拷到自己这边。
//
// 【最小示例（读自己进程的内存，可以跑）】
//   #include <cstdio>
//   int main() {
//       int value = 42;
//       int* p = &value;              // p 存着 value 的地址
//       std::printf("%d %d\n", value, *p);  // 42 42（*p = 通过地址取值）
//       std::printf("%p\n", (void*)p);      // 打印地址
//       return 0;
//   }
//
// 【自己动手要点】
// 1) 指针本身也是变量：int* p 里 p 是地址，*p 才是值；
// 2) 地址 ± 数字 = 向前/后跳几个字节：`基地址 + 偏移` 就是本项目最常用的运算；
// 3) 读到非法地址会崩溃/返回失败——所以所有读函数都要检查返回值，
//    本项目 TryRead 失败时会把值清零并返回 false（这就是"健壮性"）。
// ----------------------------------------------------------------------------
//
// 【概念速览】
// 头文件、模板、原子变量等概念见 🎓 概念卡 ①⑭⑩（或《搭建路线图与概念索引.md》）。
// inline 变量（C++17）：头文件里的变量加 inline 就不会重复定义报错（本文件大量使用）。
// thread_local：每个线程独立的变量（多线程各存一份）。
// ============================================================================

#pragma once

// NOMINMAX：让 Windows.h 不要定义 min/max 宏（否则会跟标准库的 std::min/max 冲突）
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>      // Windows API（ReadProcessMemory 等）
#include <TlHelp32.h>     // 进程/模块快照（Toolhelp32）
#include <atomic>         // 原子变量
#include <cstdint>        // uint64_t 等
#include <cstring>        // std::memset
#include <type_traits>    // std::is_trivially_copyable_v

namespace memory
{
	// ------------------------------------------------------------------
	// 全局状态
	// ------------------------------------------------------------------
	inline HANDLE gHandle = nullptr;          // "读内存"用的进程句柄
	inline HANDLE gWriteHandle = nullptr;     // "写内存"用的进程句柄

	inline uintptr_t pID = 0;                 // 目标进程（cs2.exe）的进程 ID
	// 是否允许写内存：默认允许（魔法子弹等写视角功能需要）。
	// 想只读就加 --no-memory-writes 启动。
	inline bool gWritesAllowed = true;

	// 读取统计信息（给菜单里的性能面板用）
	struct ReadMetrics
	{
		std::uint64_t calls{};          // 读内存的总次数
		std::uint64_t failures{};       // 失败的次数
		std::uint64_t bytesRequested{}; // 请求读取的总字节数
	};

	// 用原子变量保存统计计数（数据线程和渲染线程同时读写，必须原子）
	inline std::atomic<std::uint64_t> gReadCalls{0};
	inline std::atomic<std::uint64_t> gReadFailures{0};
	inline std::atomic<std::uint64_t> gReadBytesRequested{0};
	// "纪元"号：每次重置统计时 +1。用来让每个线程丢弃自己过期的本地累加数据
	inline std::atomic<std::uint64_t> gReadMetricsEpoch{1};

	// ------------------------------------------------------------------
	// RecordRead：记录一次内存读取（用于性能统计）
	// ------------------------------------------------------------------
	// 设计：不是每次都更新全局原子变量（那样太慢），而是先在"每个线程本地"
	// 累加，攒够 256 次再一次性合并到全局。这是经典的"批量合并"优化。
	inline void RecordRead(size_t size, bool succeeded) noexcept
	{
		// 线程本地的小累加器
		struct LocalAccumulator
		{
			std::uint64_t epoch{};   // 记录的纪元号
			std::uint64_t calls{};   // 累计次数
			std::uint64_t failures{};
			std::uint64_t bytes{};   // 累计字节数
		};
		thread_local LocalAccumulator local;   // 每个线程自己的累加器
		const std::uint64_t epoch = gReadMetricsEpoch.load(
			std::memory_order_relaxed);
		// 如果纪元变了（统计被重置过），先丢弃旧数据
		if (local.epoch != epoch) {
			local = LocalAccumulator{epoch};
		}
		++local.calls;                          // 次数 +1
		local.bytes += static_cast<std::uint64_t>(size);  // 字节数累加
		if (!succeeded) {
			++local.failures;                   // 失败数 +1
		}
		if (local.calls >= 256) {               // 攒够了，合并到全局
			gReadCalls.fetch_add(local.calls, std::memory_order_relaxed);
			gReadFailures.fetch_add(
				local.failures,
				std::memory_order_relaxed);
			gReadBytesRequested.fetch_add(
				local.bytes,
				std::memory_order_relaxed);
			local = LocalAccumulator{epoch};    // 清零本地，重新开始
		}
	}

	// 读取当前的全局统计
	inline ReadMetrics GetReadMetrics() noexcept
	{
		return ReadMetrics{
			gReadCalls.load(std::memory_order_relaxed),
			gReadFailures.load(std::memory_order_relaxed),
			gReadBytesRequested.load(std::memory_order_relaxed)
		};
	}

	// 清零统计，并把纪元号 +1（让所有线程丢弃本地旧数据）
	inline void ResetReadMetrics() noexcept
	{
		gReadCalls.store(0, std::memory_order_relaxed);
		gReadFailures.store(0, std::memory_order_relaxed);
		gReadBytesRequested.store(0, std::memory_order_relaxed);
		gReadMetricsEpoch.fetch_add(1, std::memory_order_relaxed);
	}

	// 实现在 memory.cpp 里的普通函数（声明）
	uintptr_t GetProcID(const wchar_t* process);            // 按进程名找进程 ID
	uintptr_t GetModuleBaseAddress(uintptr_t procID, const wchar_t* module); // 找模块基地址
	void Close();                                           // 关闭句柄
	void SetWritesAllowed(bool allowed);                    // 设置是否允许写
	bool WritesAllowed();                                   // 查询是否允许写
	bool TryWriteRaw(uintptr_t address, const void* buffer, size_t size); // 写原始字节

    // ------------------------------------------------------------------
    // 模板读函数：从目标进程读取一个"T 类型的值"
    // ------------------------------------------------------------------
    // 🎓 概念卡 ⑭：template 模板（一份代码支持任意类型）
    // ------------------------------------------------------------------
    // 【是什么】"类型"也变成了参数：写一次逻辑，调用时指定类型。
    // TryRead<int32_t> 读 4 字节整数，TryRead<float> 读 4 字节浮点，
    // TryRead<uintptr_t> 读 8 字节地址——函数体完全一样，编译器帮你各生成一份。
    //
    // 【为什么在这里】读内存的场景太多（读血量、读坐标、读名字指针…），
    // 每种类型的读取逻辑一模一样：目标地址 + 读 sizeof(T) 字节。用模板
    // 写一次，处处可用。static_assert 是"编译期检查"：T 不适合直接读内存
    // （比如 std::string 内部是堆指针）时直接编译报错，把错误挡在编译期。
    //
    // 【最小示例】
    //   template <typename T>
    //   T twice(T value) { return value * 2; }
    //   int a = twice(3);          // T = int
    //   double b = twice(3.5);     // T = double（自动推断）
    //
    // 【自己动手要点】
    // 1) 模板的定义要放在头文件里（编译器需要"看到"完整实现才能实例化）；
    // 2) <typename T> 只是参数名，可以叫任何名字；
    // 3) std::is_trivially_copyable_v<T> 检查的是"T 是不是一堆能整块拷贝的字节"。
    // ------------------------------------------------------------------
	// 用法示例：
	//   int32_t health;
	//   memory::TryRead<int32_t>(entity + offset, health);  // 读一个 int
	// 这里的 T 由调用时推断（这里是 int32_t），sizeof(T) 自动得到字节数。
	template <typename T> bool TryRead(uintptr_t address, T& value)
	{
		// 编译期检查：T 必须是"可平凡复制"的类型（就是纯粹的一堆字节）。
		// 像 std::string 这种内部带指针的类不能直接读内存（会读到一堆垃圾指针），
		// 所以编译器直接拒绝，防止低级错误。
		static_assert(std::is_trivially_copyable_v<T>,
			"Remote memory reads require a trivially copyable type");

		value = T{};                              // 先清零
		if (!gHandle || address == 0) {           // 没句柄或地址为空 → 失败
			RecordRead(sizeof(T), false);
			return false;
		}

		// ReadProcessMemory：Windows API，从目标进程读内存
		SIZE_T bytesRead = 0;
		if (!ReadProcessMemory(
				gHandle,
				reinterpret_cast<LPCVOID>(address),   // 目标地址
				&value,                               // 读到哪里（本地）
				sizeof(T),                            // 读多少字节
				&bytesRead) ||                        // 实际读了多少
			bytesRead != sizeof(T)) {                 // 没读满 → 失败
			value = T{};                              // 失败时清零，防止用脏数据
			RecordRead(sizeof(T), false);
			return false;
		}

		RecordRead(sizeof(T), true);
		return true;
	}

	// TryRead 的便捷版本：失败时返回全零值（调用方不用管是否成功）
	template <typename T> T Read(uintptr_t address)
	{
		T value{};
		TryRead(address, value);
		return value;
	}

	// 写一个 T 类型的值到目标进程（仅供防闪光功能使用）
	template <typename T> bool Write(uintptr_t address, T value)
	{
		static_assert(std::is_trivially_copyable_v<T>,
			"Remote memory writes require a trivially copyable type");
		return TryWriteRaw(address, &value, sizeof(T));
	}

	// 读取一段原始字节（不关心类型）。失败时会把 buffer 清成 0
	inline bool ReadRaw(uintptr_t address, void* buffer, size_t size)
	{
		if (!buffer || size == 0) {               // 参数不合法
			return false;
		}

		std::memset(buffer, 0, size);             // 先清零（防止读到一半失败留脏数据）
		if (!gHandle || address == 0) {
			RecordRead(size, false);
			return false;
		}

		SIZE_T bytesRead = 0;
		if (!ReadProcessMemory(
				gHandle,
				reinterpret_cast<LPCVOID>(address),
				buffer,
				size,
				&bytesRead) ||
			bytesRead != size) {                  // 没读满
			std::memset(buffer, 0, size);         // 失败：再次清零
			RecordRead(size, false);
			return false;
		}

		RecordRead(size, true);
		return true;
	}


}
