// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 9CCN
//
// se9ccn_texture_budget —— Homeworld Remastered 纹理内存预算解锁器（独立 DLL，GPLv3）
//
// 背景（详见 README.md / docs/TECHNICAL.md）：
//   HomeworldRM.exe 在纹理上传闸门 FUN_007B4C80 里维护一个 64 位"纹理字节"计数器，
//   并硬编码了 0x77770000（≈1.867 GiB）上限；超过后新纹理创建直接返回 0（静默失败）。
//   原版 4 个种族就把这个预算吃到 99% 以上，8 个不同种族的 mod 对局需要 ≈2.6GB，
//   于是出现 "Unable to retrieve UI texture"（红黑方块），甚至
//   "Unable to load texture (...Circle.mtga) -- FATAL EXIT --"（uigraphic.cpp:267）。
//
// 本 DLL 做什么：
//   游戏启动时会枚举 <exe目录>\se*.dll 并 LoadLibrary + GetProcAddress("InspectDLL")，
//   我们在被加载的这一瞬间（InspectDLL 回调里，已脱离 loader lock）对**内存中的代码**
//   做一次 8 字节补丁，彻底去掉"纹理预算"这道闸门：
//     a) `JG fail`（总量 ≥4GiB 必然失败）→ 两条 NOP；
//     b) `CMP EDI,0x77770000` 的阈值 → 0xFFFFFFFF。
//   只改 a) 不够：8 个不同种族的纹理总量（含逐级上传的 mip 链）约 5GB，会撞上 a) 的 4GiB 限制。
//   - 不修改任何游戏文件（磁盘上的 exe 原封不动）；
//   - 补丁前做字节签名校验，游戏版本不匹配时拒绝修改并写日志；
//   - 计数器语义不变（仍是"已用纹理字节"），只是不再拒绝创建纹理。
//
// 为什么直接拉满、不留配置：
//   贴图加载失败会让游戏直接不可玩（丢 UI、FATAL EXIT）；而显存不足最多只是帧率下降。
//   两者代价不对称，所以默认把这道闸门整体解除，不需要用户做选择。
//
// 边界：解除后唯一的限制是显存/驱动。贴图超出显存时驱动会回退到系统内存（变慢），
//   不会再出现"纹理创建返回 0"导致的丢贴图。
//
// 构建：见 README.md（CMake + 32 位 MSVC，静态 CRT）。

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kVersion = "1.2.0";

// 补丁位置（preferred base 0x400000，RVA 0x3B4CF5 起 8 字节）：
//   7F 08                JG  fail            ; 计数器高 dword > 0（总量 ≥4GiB）→ 失败
//   81 FF 00 00 77 77    CMP EDI,0x77770000  ; 已用纹理字节低 32 位 vs ≈1.867GiB
//   72 09                JB  ok              ; 小于阈值就继续
// 本工具做两处修改（合起来 = 彻底解除预算）：
//   1) 7F 08 → 90 90     去掉"总量 ≥4GiB 必然失败"的高位限制。
//      8 个不同种族的纹理总量（含逐级上传的 mip 链 ≈1.33×）约 5GB，只改阈值仍会卡在 4GiB；
//   2) 阈值 0x77770000 → 0xFFFFFFFF，把低 32 位比较也拉到几乎不可能触发。
constexpr uint32_t kGateRva = 0x003B4CF5;
constexpr uint8_t kGateExpected[8] = {0x7F, 0x08, 0x81, 0xFF, 0x00, 0x00, 0x77, 0x77};
constexpr uint8_t kGatePatched[8] = {0x90, 0x90, 0x81, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct PluginInfo {
    char name[32];
    char desc[256];
    char misc[256];
};

PluginInfo g_info = {
    "9CCN Texture Budget",
    "Raises the hard-coded texture memory budget of Homeworld Remastered "
    "(in-memory patch; no game files are modified).",
    "9CCN / GPLv3",
};

HMODULE g_self = nullptr;
HANDLE g_log = INVALID_HANDLE_VALUE;
wchar_t g_dir[MAX_PATH] = {};  // 带尾部反斜杠
volatile LONG g_init_done = 0;
uintptr_t g_base = 0;

// 引擎纹理字节计数器（64 位）：低 dword RVA 0x5AED28、高 dword RVA 0x5AED2C。
// 只读采样，用来在日志里记录真实需求峰值（证实 8 种族确实超过 4GiB）。
constexpr uint32_t kTexCounterLowRva = 0x005AED28;
constexpr uint32_t kTexCounterHighRva = 0x005AED2C;

void resolve_dir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return;
    }
    wchar_t* slash = nullptr;
    for (wchar_t* p = path; *p != 0; ++p) {
        if (*p == L'\\' || *p == L'/') {
            slash = p;
        }
    }
    if (slash == nullptr) {
        return;
    }
    *slash = 0;
    const int used = static_cast<int>(slash - path);
    lstrcpynW(g_dir, path, MAX_PATH);
    if (used + 2 < MAX_PATH) {
        g_dir[used] = L'\\';
        g_dir[used + 1] = 0;
    }
}

void open_log() {
    if (g_log != INVALID_HANDLE_VALUE || g_dir[0] == 0) {
        return;
    }
    wchar_t path[MAX_PATH] = {};
    lstrcpynW(path, g_dir, MAX_PATH);
    lstrcatW(path, L"se9ccn_texture_budget.log");
    g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void log_line(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) {
        return;
    }
    char buf[1024] = {};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = std::snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth,
                          st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (n < 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int m = std::vsnprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), fmt, ap);
    va_end(ap);
    if (m > 0) {
        n += m;
    }
    if (n > static_cast<int>(sizeof(buf)) - 3) {
        n = static_cast<int>(sizeof(buf)) - 3;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD written = 0;
    WriteFile(g_log, buf, static_cast<DWORD>(n), &written, nullptr);
}

// 只在目标位置字节完全符合预期时才下补丁。函数内不做堆分配，便于在 __try 中安全使用。
bool patch_budget_gate(uintptr_t base) {
    auto* site = reinterpret_cast<uint8_t*>(base + kGateRva);
    __try {
        if (std::memcmp(site, kGateExpected, sizeof(kGateExpected)) != 0) {
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    DWORD old_protect = 0;
    if (VirtualProtect(site, sizeof(kGatePatched), PAGE_EXECUTE_READWRITE, &old_protect) == FALSE) {
        return false;
    }
    std::memcpy(site, kGatePatched, sizeof(kGatePatched));
    DWORD restored = 0;
    VirtualProtect(site, sizeof(kGatePatched), old_protect, &restored);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(kGatePatched));
    return true;
}

uint64_t read_tex_counter() {
    if (g_base == 0) {
        return 0;
    }
    const auto* lo = reinterpret_cast<const volatile uint32_t*>(g_base + kTexCounterLowRva);
    const auto* hi = reinterpret_cast<const volatile uint32_t*>(g_base + kTexCounterHighRva);
    return (static_cast<uint64_t>(*hi) << 32) | *lo;
}

DWORD WINAPI counter_monitor(void* /*param*/) {
    uint32_t next_mb = 1024;  // 首次在 1GiB 记录一次，之后每 +512MiB 记录一次
    for (;;) {
        Sleep(1000);
        const uint64_t bytes = read_tex_counter();
        const uint64_t mb = bytes >> 20;
        if (mb >= next_mb && mb < 65536) {
            log_line("engine texture counter ≈ %llu MiB (this game is loading more than the old "
                     "1.867GiB budget)", static_cast<unsigned long long>(mb));
            next_mb = static_cast<uint32_t>(mb + 512);
        }
    }
}

void ensure_initialized() {
    if (InterlockedCompareExchange(&g_init_done, 1, 0) != 0) {
        return;
    }
    open_log();
    log_line("se9ccn_texture_budget v%s starting (%s %s)", kVersion, __DATE__, __TIME__);

    // 钉住自身：游戏在 InspectDLL 之后会 FreeLibrary，避免我们的代码被卸载。
    HMODULE pinned = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&g_info), &pinned) == 0) {
        log_line("warn: pin self failed (err=%lu); patch still applied", GetLastError());
    }

    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) {
        log_line("error: cannot determine main module base");
        return;
    }
    g_base = base;
    log_line("game exe base = 0x%08X", static_cast<unsigned>(base));

    if (!patch_budget_gate(base)) {
        log_line("error: signature mismatch at RVA 0x%08X —— 游戏版本不匹配或已被其它补丁修改，"
                 "本工具不修改任何字节。",
                 static_cast<unsigned>(kGateRva));
        log_line("       expected bytes: 7F 08 81 FF 00 00 77 77 "
                 "(JG fail / CMP EDI,0x77770000)");
        return;
    }
    log_line("patched: texture gate at RVA 0x%08X —— 4GiB 高位限制已移除，阈值改为 0xFFFFFFFF",
             static_cast<unsigned>(kGateRva));
    log_line("done: 纹理预算闸门已解除；剩下的唯一限制是显存。显存不足只会掉帧，不再丢贴图。");

    // 只读采样线程：把引擎自报的纹理占用峰值记到日志里（便于确认问题是否真的在预算上）。
    if (CreateThread(nullptr, 0, &counter_monitor, nullptr, 0, nullptr) == nullptr) {
        log_line("warn: counter monitor thread failed (err=%lu)", GetLastError());
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_self = hModule;
        resolve_dir();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log != INVALID_HANDLE_VALUE) {
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}

// Homeworld Remastered 的插件枚举协议：对 <exe目录>\se*.dll 依次
// LoadLibraryA → GetProcAddress("InspectDLL") → InspectDLL(&out0, &out1)。
// *out0 == 1 才登记为音频设备；我们返回 0（不是音频设备，只借用启动时机）。
extern "C" __declspec(dllexport) void* __cdecl InspectDLL(uint32_t* out0, uint32_t* out1) {
    __try {
        if (out0 != nullptr) {
            *out0 = 0;
        }
        if (out1 != nullptr) {
            *out1 = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 参数不可写：忽略。
    }
    ensure_initialized();
    return &g_info;
}

extern "C" __declspec(dllexport) void* __cdecl CreateDevice() {
    return nullptr;
}
