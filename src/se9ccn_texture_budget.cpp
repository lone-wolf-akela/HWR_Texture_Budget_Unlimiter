// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 9CCN
//
// se9ccn_texture_budget —— Homeworld Remastered 纹理内存预算解锁器（独立 DLL，GPLv3）
//
// 背景（详见 README.md / docs/TECHNICAL.md）：
//   HomeworldRM.exe 在纹理上传闸门 FUN_007B4C80 里维护一个 64 位"纹理字节"计数器
//   （低 dword RVA 0x5AED28、高 dword RVA 0x5AED2C），并硬编码 ≈1.867 GiB（0x77770000）
//   的预算；超过后新纹理创建直接返回 0（静默失败）。原版 4 个种族就把预算吃到 99% 以上，
//   8 个不同种族的 mod 对局需求约 2.6GB+，于是丢 UI 贴图，甚至
//   "Unable to load texture (...Circle.mtga) -- FATAL EXIT --"（uigraphic.cpp:267）。
//
// 本 DLL 怎么做（**计数器巡航下调**，之前已验证可玩的方案）：
//   游戏启动时被当作 se*.dll 插件加载；我们在 InspectDLL 回调里起一个后台线程，
//   每 250ms 读一次那个 64 位纹理计数器：只要它超过 1500 MiB，就整体下调 512 MiB，
//   让引擎各种基于"已用纹理内存"的判断始终看到充裕余量。
//   - 计数器的更新全部是"加减 delta"，整体平移不会破坏记账；
//   - 不修改任何代码/磁盘文件：只动进程内一个计数器；
//   - 为什么不用"改比较指令阈值"：那只绕过闸门里的一处判断；实测 Steam 版 8 种族
//     在计数器 ≈1.6GB 时仍 FATAL，说明还有别的基于该计数器的判断，巡航下调一并覆盖。
//
// 边界：真正决定能否放下的是显存；贴图超出显存时驱动会回退到系统内存（变慢），
//   但不会再因为"纹理预算"而拒绝创建纹理。
//
// 构建：见 README.md（CMake + 32 位 MSVC，静态 CRT）。

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kVersion = "1.3.1";

// 版本身份校验：用纹理上传闸门里的那段字节确认这是预期构建（**只读**，不改代码）。
//   RVA 0x3B4CF7: 81 FF 00 00 77 77 (CMP EDI,0x77770000) 72 09 (JB ok)
constexpr uint32_t kBuildSigRva = 0x003B4CF7;
constexpr uint8_t kBuildSig[8] = {0x81, 0xFF, 0x00, 0x00, 0x77, 0x77, 0x72, 0x09};

// 引擎的 64 位纹理字节计数器（VA 0x9AED28 / 0x9AED2C）。
constexpr uint32_t kTexCounterLowRva = 0x005AED28;
constexpr uint32_t kTexCounterHighRva = 0x005AED2C;

// 巡航下调参数（与已验证可玩的配置一致）：超过 1500 MiB 就下调 512 MiB。
constexpr uint64_t kKeepBelowBytes = 1500ull << 20;
constexpr uint64_t kShaveStepBytes = 512ull << 20;
// 超过这个值就认为是"撕裂读"（64 位计数器分两次读，游戏线程可能正在更新）而不是真实占用。
constexpr uint64_t kSanityMaxBytes = 64ull << 30;  // 64 GiB

struct PluginInfo {
    char name[32];
    char desc[256];
    char misc[256];
};

PluginInfo g_info = {
    "9CCN Texture Budget",
    "Keeps the engine texture-memory counter below its internal budget thresholds "
    "(in-process only; no game files are modified).",
    "9CCN / GPLv3",
};

HMODULE g_self = nullptr;
HANDLE g_log = INVALID_HANDLE_VALUE;
wchar_t g_dir[MAX_PATH] = {};  // 带尾部反斜杠
volatile LONG g_init_done = 0;
uintptr_t g_base = 0;
volatile LONG g_shaves = 0;

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

// 只读校验：确认目标 exe 是我们分析过的构建（不修改任何字节）。
bool verify_build(uintptr_t base) {
    const auto* sig = reinterpret_cast<const uint8_t*>(base + kBuildSigRva);
    __try {
        return std::memcmp(sig, kBuildSig, sizeof(kBuildSig)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t read_counter() {
    if (g_base == 0) {
        return 0;
    }
    const auto* lo = reinterpret_cast<const volatile uint32_t*>(g_base + kTexCounterLowRva);
    const auto* hi = reinterpret_cast<const volatile uint32_t*>(g_base + kTexCounterHighRva);
    return (static_cast<uint64_t>(*hi) << 32) | *lo;
}

void write_counter(uint64_t value) {
    if (g_base == 0) {
        return;
    }
    auto* lo = reinterpret_cast<volatile uint32_t*>(g_base + kTexCounterLowRva);
    auto* hi = reinterpret_cast<volatile uint32_t*>(g_base + kTexCounterHighRva);
    // 先写高 dword：中间态只会让引擎"看起来用得更多"，下一拍就会修正。
    *hi = static_cast<uint32_t>(value >> 32);
    *lo = static_cast<uint32_t>(value & 0xFFFFFFFFu);
}

// 后台巡航：超过阈值就整体下调。线程随 DLL 常驻（DLL 已 PIN）。
// 注意两点（v1.3.1 修复）：
//   1) 引擎在纹理销毁时会做减法，被我们下调过的计数器可能变成**负数**——对闸门来说
//      等于"空间充足"（走 JS 分支），直接跳过即可；
//   2) 计数器是 64 位、分高低两 dword 读，游戏线程可能正好在中间更新，偶发"半新半旧"
//      的读值（接近 2^64）。这种值不处理，下一拍再读，避免把真实计数写坏。
//   下调步数用 O(1) 计算，绝不循环递减（v1.3.0 在遇到异常大值时会空转数分钟）。
DWORD WINAPI shaver_thread(void* /*param*/) {
    int64_t last_logged_mb = 0;
    int negative_logged = 0;
    for (;;) {
        Sleep(250);
        int64_t counter = static_cast<int64_t>(read_counter());
        if (counter == 0) {
            continue;
        }
        if (counter < 0) {
            if (negative_logged < 5) {
                ++negative_logged;
                log_line("counter is negative (%lld MiB, shave offset + frees): headroom is huge",
                         static_cast<long long>(counter >> 20));
            }
            continue;
        }
        if (counter > static_cast<int64_t>(kSanityMaxBytes)) {
            log_line("skip suspicious counter value %lld MiB (torn read), will retry",
                     static_cast<long long>(counter >> 20));
            continue;
        }
        if (counter >= static_cast<int64_t>(kKeepBelowBytes)) {
            const int64_t over = counter - static_cast<int64_t>(kKeepBelowBytes);
            const int64_t steps = over / static_cast<int64_t>(kShaveStepBytes) + 1;
            const int64_t target = counter - steps * static_cast<int64_t>(kShaveStepBytes);
            write_counter(static_cast<uint64_t>(target));
            const LONG n = InterlockedIncrement(&g_shaves);
            if (n <= 20 || (n % 50) == 0) {
                log_line("counter shave #%ld: %lld MiB -> %lld MiB", static_cast<long>(n),
                         static_cast<long long>(counter >> 20),
                         static_cast<long long>(target >> 20));
            }
            counter = target;
        }
        const int64_t mb = counter >> 20;
        if (mb >= 1024 && mb >= last_logged_mb + 512) {
            last_logged_mb = mb;
            log_line("engine texture counter ≈ %lld MiB (shaves so far: %ld)",
                     static_cast<long long>(mb), static_cast<long>(g_shaves));
        }
    }
    return 0;
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
        log_line("warn: pin self failed (err=%lu)", GetLastError());
    }

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (g_base == 0) {
        log_line("error: cannot determine main module base");
        return;
    }
    log_line("game exe base = 0x%08X", static_cast<unsigned>(g_base));

    if (!verify_build(g_base)) {
        log_line("error: build signature mismatch at RVA 0x%08X —— 这不是本工具分析过的游戏构建，"
                 "为避免误改内存，本次不启用任何功能。",
                 static_cast<unsigned>(kBuildSigRva));
        return;
    }

    log_line("mechanism: 每 250ms 采样纹理计数器，超过 %llu MiB 就下调 %llu MiB",
             static_cast<unsigned long long>(kKeepBelowBytes >> 20),
             static_cast<unsigned long long>(kShaveStepBytes >> 20));
    log_line("initial texture counter = %llu MiB",
             static_cast<unsigned long long>(read_counter() >> 20));

    if (CreateThread(nullptr, 0, &shaver_thread, nullptr, 0, nullptr) == nullptr) {
        log_line("error: CreateThread failed (err=%lu)", GetLastError());
        return;
    }
    log_line("done: 巡航下调线程已启动。");
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
