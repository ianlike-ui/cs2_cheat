// ============================================================================
// memory.cpp —— 与 Windows 系统交互、读写"别的进程"内存的底层模块
//
// 【背景知识：为什么能读游戏内存？】
// Windows 里每个程序（进程）都活在自己的"虚拟地址空间"里，互相隔离。
// 但 Windows 提供了一套官方 API，允许一个程序在获得句柄后，
// 用 ReadProcessMemory / WriteProcessMemory 读取/写入另一个进程的内存。
// 本模块就是这套 API 的封装：
//   - 找到游戏进程（按进程名 "cs2.exe" 查找，拿到进程 ID）
//   - 找到游戏某个模块（dll）在内存里的基地址
//   - 提供读/写内存的函数（读用得多，写默认关闭）
//
// 【概念速览】指针与地址、HANDLE 句柄见 🎓 概念卡 ⑬⑮（或《搭建路线图与概念索引.md》）。
// DWORD：Windows 定义的 32 位无符号整数（就是 unsigned int）。
// ============================================================================

#include "memory.hpp"       // 本模块的头文件（声明了这些函数）

// 匿名命名空间：这些辅助函数只在本文件内部用，外部看不到
namespace
{
	// 关闭一个句柄并把它置空（防止重复关闭）
	void closeHandle(HANDLE& handle)
	{
		if (handle) {              // 句柄非空才需要关
			CloseHandle(handle);   // Windows API：关闭句柄
			handle = nullptr;      // 置空，表示已经关了
		}
	}
}

// ----------------------------------------------------------------------------
// memory::Close：关闭所有句柄，复位进程 ID
// ----------------------------------------------------------------------------
void memory::Close()
{
	closeHandle(gWriteHandle);   // 关闭"写内存"用的句柄
	closeHandle(gHandle);        // 关闭"读内存"用的句柄
	pID = 0;                     // 进程 ID 清零（表示当前没有目标进程）
}

// ----------------------------------------------------------------------------
// memory::SetWritesAllowed：设置是否允许写游戏内存
// ----------------------------------------------------------------------------
// 默认不允许写。用户必须以 --allow-memory-writes 启动程序才会允许，
// 这是刻意的安全设计：写内存能力关掉后，程序就"只读"，改不了游戏数据。
void memory::SetWritesAllowed(bool allowed)
{
	gWritesAllowed = allowed;
	if (!allowed) {                // 设为不允许时，把写句柄也关掉
		closeHandle(gWriteHandle);
	}
}

// 查询当前是否允许写内存
bool memory::WritesAllowed()
{
	return gWritesAllowed;
}

// ----------------------------------------------------------------------------
// memory::TryWriteRaw：向目标进程的指定地址写入一段原始字节
// ----------------------------------------------------------------------------
// 返回 true 表示写入成功。
// 参数 address：目标地址；buffer：要写入的数据；size：数据字节数。
// 这个函数被"防闪光"功能使用（把闪光状态改掉），其他功能不写内存。
bool memory::TryWriteRaw(
	uintptr_t address,
	const void* buffer,
	size_t size)
{
	// 安全检查：不允许写 / 没有目标进程 / 地址为空 / 数据为空 → 直接失败
	if (!gWritesAllowed ||
		pID == 0 ||
		address == 0 ||
		!buffer ||
		size == 0) {
		return false;
	}

	// 第一次写时才去打开"写句柄"（懒加载）
	if (!gWriteHandle) {
		// OpenProcess 打开目标进程，申请写内存需要的权限
		gWriteHandle = OpenProcess(
			PROCESS_QUERY_LIMITED_INFORMATION |   // 查询进程信息
			PROCESS_VM_WRITE |                    // 写虚拟内存
			PROCESS_VM_OPERATION,                 // 内存操作（如改保护属性）
			FALSE,                                // 不继承句柄
			static_cast<DWORD>(pID));             // 目标进程 ID
		if (!gWriteHandle) {                      // 打开失败
			return false;
		}
	}

	// WriteProcessMemory：真正写入。bytesWritten 返回实际写入了多少字节
	SIZE_T bytesWritten = 0;
	return WriteProcessMemory(
			gWriteHandle,
			reinterpret_cast<LPVOID>(address),   // 目标地址
			buffer,                              // 数据
			size,                                // 大小
			&bytesWritten) &&                    // 实际写入量
		bytesWritten == size;                    // 必须全部写入才算成功
}

// ----------------------------------------------------------------------------
// memory::GetProcID：按进程名查找进程，返回进程 ID
// ----------------------------------------------------------------------------
// 🎓 概念卡 ⑮：HANDLE 句柄 与 Windows API（系统能力怎么借）
// ----------------------------------------------------------------------------
// 【是什么】
// 你的程序跑在"沙盒"里，想碰别的进程/文件/窗口，只能通过 Windows 提供的
// 系统 API（函数名以 Open/Create/Get 开头的那一堆）。这些 API 大多给你一个
// "句柄"（HANDLE）——一张"通行证"，之后的操作都靠它，用完必须还
// （CloseHandle）。HANDLE 本质就是一个指针大小的数字，NULL/INVALID 表示没拿到。
//
// 【为什么在这里】本模块全部是 Windows API 的封装：
//   CreateToolhelp32Snapshot —— 给系统"拍快照"，列出所有进程/模块；
//   OpenProcess —— 打开目标进程，拿到读内存的通行证；
//   ReadProcessMemory —— 凭通行证读目标进程的内存。
// 这是"外部工具"能读游戏内存的唯一官方途径。
//
// 【最小示例】
//   // 打开计算器进程并读它的一块内存（示意）
//   HANDLE h = OpenProcess(PROCESS_VM_READ, FALSE, pid);
//   if (!h) return;                 // 失败（权限不够/进程不存在）
//   int buf; SIZE_T got = 0;
//   ReadProcessMemory(h, (LPCVOID)addr, &buf, sizeof(buf), &got);
//   CloseHandle(h);                 // 用完必须还
//
// 【自己动手要点】
// 1) 所有 Windows API 调用都要检查返回值，失败时用 GetLastError() 查原因；
// 2) 句柄用 RAII 包装（本项目 ScopedHandle）自动关闭，防泄漏；
// 3) 打开进程需要权限：读内存要 PROCESS_VM_READ，管理员权限通常必须
//    （这也是为什么程序要请求管理员权限运行）。
// ----------------------------------------------------------------------------
// 做法：给系统拍一张"所有进程的快照"（Toolhelp32 快照），
// 然后逐个进程比较名字，找到 "cs2.exe" 就返回它的 ID。
// 找到后顺便打开"读句柄"（gHandle），后续读内存就靠它。
uintptr_t memory::GetProcID(const wchar_t* process)
{
	// 创建进程快照（遍历所有进程的"名单"）
	HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (handle == INVALID_HANDLE_VALUE) {       // 快照失败
		return 0;
	}

	// PROCESSENTRY32 是"一条进程记录"的结构体
	PROCESSENTRY32 proc{};
	proc.dwSize = sizeof(PROCESSENTRY32);       // 必须填大小（Windows 老规矩）
	// Process32First 取第一条记录；如果名单是空的就失败
	if (!Process32First(handle, &proc)) {
		CloseHandle(handle);
		return 0;
	}

	// 循环遍历名单里的每个进程
	do
	{
		// _wcsicmp：忽略大小写比较宽字符串。找到了！
		if (!_wcsicmp(process, proc.szExeFile))
			{
				CloseHandle(handle);             // 快照用完就关
				Close();                         // 关掉旧的句柄（如果有）
				pID = proc.th32ProcessID;        // 记下进程 ID（全局变量）

				// 立刻打开"读句柄"（只申请读内存的权限）
				gHandle = OpenProcess(
					PROCESS_QUERY_LIMITED_INFORMATION |  // 查询信息
					PROCESS_VM_READ,                     // 读虚拟内存
					FALSE,                               // 不继承
					static_cast<DWORD>(pID));
			if (!gHandle) {                  // 打开失败就复位并返回 0
				pID = 0;
				return 0;
			}
			return proc.th32ProcessID;       // 返回进程 ID
		}
	} while (Process32Next(handle, &proc));      // 取下一条记录，循环继续

	// 遍历完了都没找到：清理并返回 0（表示没找到）
	CloseHandle(handle);
	Close();
	return 0;
}

// ----------------------------------------------------------------------------
// memory::GetModuleBaseAddress：获取某个进程里指定模块（dll）的基地址
// ----------------------------------------------------------------------------
// 为什么需要它？游戏代码被拆成很多 dll（client.dll、engine2.dll…），
// 每个 dll 被加载到内存的某个"基地址"。我们之前从 cs2-dumper 拿到的
// 偏移量都是"相对 dll 开头的偏移"，所以真正读内存时要：基地址 + 偏移。
uintptr_t memory::GetModuleBaseAddress(uintptr_t procID, const wchar_t* module)
{
	// 创建"模块快照"（列出目标进程加载了哪些 dll）
	HANDLE handle = CreateToolhelp32Snapshot(
		TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,   // 32 位和 64 位模块都要
		static_cast<DWORD>(procID));
	if (handle == INVALID_HANDLE_VALUE) {
		return 0;
	}

	MODULEENTRY32 mod{};                    // 一条模块记录
	mod.dwSize = sizeof(MODULEENTRY32);
	if (!Module32First(handle, &mod)) {     // 没有模块
		CloseHandle(handle);
		return 0;
	}

	do
	{
		// 找到名字匹配的模块
		if (!_wcsicmp(module, mod.szModule))
		{
			CloseHandle(handle);
			return (uintptr_t)mod.modBaseAddr;  // 返回它的基地址
		}
	} while (Module32Next(handle, &mod));   // 下一个

	CloseHandle(handle);
	return 0;                               // 没找到，返回 0
}
