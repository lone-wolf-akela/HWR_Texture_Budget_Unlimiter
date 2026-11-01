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
// 本 DLL 怎么做（**一次性把计数器下移 32 GiB**）：
//   游戏启动时被当作 se*.dll 插件加载；我们在 InspectDLL 回调里把那个 64 位纹理计数器
//   整体减去 32 GiB。闸门对"负值"一律放行（T<0 → JS 分支，见 docs/TECHNICAL.md），
//   而 -32 GiB 的偏移远大于一场 8 种族对局的纹理净增量（≈2.6~5GB），整场都不会再拒绝纹理。
//   - 计数器的更新全部是"加减 delta"，整体平移不会破坏记账；
//   - 不修改任何代码/磁盘文件：只动进程内一个计数器；
//   - 写入发生在任何纹理创建之前（无竞争）；每 2 秒的看门狗只在"计数器被重置"这类
//     异常情况下补一次偏移，正常对局里它永远不会动手。

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

constexpr const char* kVersion = "1.4.1";

// 版本身份校验：用纹理上传闸门里的那段字节确认这是预期构建（**只读**，不改代码）。
//   RVA 0x3B4CF7: 81 FF 00 00 77 77 (CMP EDI,0x77770000) 72 09 (JB ok)
constexpr uint32_t kBuildSigRva = 0x003B4CF7;
constexpr uint8_t kBuildSig[8] = {0x81, 0xFF, 0x00, 0x00, 0x77, 0x77, 0x72, 0x09};

// 引擎的 64 位纹理字节计数器（VA 0x9AED28 / 0x9AED2C）。
constexpr uint32_t kTexCounterLowRva = 0x005AED28;
constexpr uint32_t kTexCounterHighRva = 0x005AED2C;

// 启动时把计数器整体下移这么多。闸门对"负值"一律放行（见 docs/TECHNICAL.md），
// 所以只要偏移量远大于一场对局的纹理净增量（8 种族约 2.6~5 GB），整场都不会再触发。
constexpr uint64_t kOffsetBytes = 32ull << 30;  // 32 GiB

// 低频看门狗：计数器比 -16 GiB 还高（说明被某段代码重置过、或已经爬回来一半）就再下移一次。
constexpr int64_t kReapplyThreshold = -(16ll << 30);
constexpr DWORD kWatchdogIntervalMs = 2000;

// 超过这个值就认为是"撕裂读"（64 位计数器分两次读，游戏线程可能正在更新）而不是真实占用。
constexpr uint64_t kSanityMaxBytes = 64ull << 30;  // 64 GiB

// 单次运行的日志上限：正常情况下几十行就够，这里只做兜底，避免极端长局把日志写爆。
constexpr size_t kLogCapBytes = 256 * 1024;

struct PluginInfo {
    char name[32];
    char desc[256];
    char misc[256];
};

PluginInfo g_info = {
    "9CCN Texture Budget",
    "Shifts the engine texture-memory counter far below its internal budget thresholds "
    "(in-process only; no game files are modified).",
    "9CCN / GPLv3",
};

HMODULE g_self = nullptr;
HANDLE g_log = INVALID_HANDLE_VALUE;
wchar_t g_dir[MAX_PATH] = {};  // 带尾部反斜杠
volatile LONG g_init_done = 0;
uintptr_t g_base = 0;
volatile LONG g_reapplies = 0;
size_t g_log_bytes = 0;  // 本次运行已写入的字节数（用于 kLogCapBytes 兜底）

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
    // CREATE_ALWAYS：每次启动重写日志，避免多次游戏后日志无限增长。
    g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_log_bytes = 0;
}

void log_line(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE || g_log_bytes >= kLogCapBytes) {
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
    g_log_bytes += static_cast<size_t>(written);
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

// 启动时一次性把计数器整体下移（默认 32 GiB）。闸门对"负值"一律放行（T<0 → JS 分支），
// 只要偏移量大于一场对局的纹理净增量，整场都不会再触发；不需要周期性改写。
bool apply_offset(const char* why, long long* before_mb, long long* after_mb) {
    if (g_base == 0) {
        return false;
    }
    const int64_t before = static_cast<int64_t>(read_counter());
    int64_t target = 0;
    if (before < INT64_MIN + static_cast<int64_t>(kOffsetBytes)) {
        target = INT64_MIN + static_cast<int64_t>(kOffsetBytes);  // 防 64 位下溢
    } else {
        target = before - static_cast<int64_t>(kOffsetBytes);
    }
    write_counter(static_cast<uint64_t>(target));
    *before_mb = static_cast<long long>(before >> 20);
    *after_mb = static_cast<long long>(target >> 20);
    log_line("counter offset applied (%s): %lld MiB -> %lld MiB", why, *before_mb, *after_mb);
    return true;
}

// 低频看门狗（每 2 秒）：正常情况下计数器从一个很大的负数慢慢往上爬，正常对局到不了
// -16 GiB；只有"计数器被某段代码重置"这类异常才会触发它，届时再下移一次。
DWORD WINAPI watchdog_thread(void* /*param*/) {
    for (;;) {
        Sleep(kWatchdogIntervalMs);
        const int64_t counter = static_cast<int64_t>(read_counter());
        if (counter > static_cast<int64_t>(kSanityMaxBytes)) {
            log_line("skip suspicious counter value (torn read), will retry");
            continue;
        }
        if (counter > kReapplyThreshold) {
            long long before = 0;
            long long after = 0;
            if (apply_offset("watchdog", &before, &after)) {
                const LONG n = InterlockedIncrement(&g_reapplies);
                log_line("watchdog: counter was %lld MiB, re-applied (reapplies=%ld)", before,
                         static_cast<long>(n));
            }
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

    log_line("mechanism: 启动时把纹理计数器整体下移 %lld MiB（一次写入），之后每 %lu ms 由看门狗兜底",
             static_cast<long long>(kOffsetBytes >> 20),
             static_cast<unsigned long>(kWatchdogIntervalMs));

    long long before_mb = 0;
    long long after_mb = 0;
    if (!apply_offset("startup", &before_mb, &after_mb)) {
        log_line("error: failed to apply counter offset");
        return;
    }

    if (CreateThread(nullptr, 0, &watchdog_thread, nullptr, 0, nullptr) == nullptr) {
        log_line("error: CreateThread failed (err=%lu)", GetLastError());
        return;
    }
    log_line("done: 看门狗线程已启动（正常对局不会触发）。");
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
