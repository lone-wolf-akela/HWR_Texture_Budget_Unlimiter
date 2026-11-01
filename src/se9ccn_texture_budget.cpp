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
//   做一次 4 字节补丁：把 `CMP EDI, 0x77770000` 的立即数改成配置的预算（默认 3500MB）。
//   - 不修改任何游戏文件（磁盘上的 exe 原封不动）；
//   - 补丁前做字节签名校验，游戏版本不匹配时拒绝修改并写日志；
//   - 计数器语义不变（仍是"已用纹理字节"），只是上限变大。
//
// 安全/边界：
//   - 计数器高位检查仍存在，所以有效上限 < 4 GiB；我们把 cap 钳到 0xFFF00000。
//   - 预算应设为你的显存能从容容纳的大小（建议显存 70~80%），过大只会让驱动去用系统内存，
//     反而可能卡顿。
//
// 构建：见 README.md（CMake + 32 位 MSVC，静态 CRT）。

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char* kVersion = "1.0.0";

// 补丁位置：VA 0x7B4CF7（preferred base 0x400000）处的一条指令
//   81 FF 00 00 77 77   CMP EDI, 0x77770000
//   72 09               JB  +9            ; 小于上限 → 跳过"失败返回 0"
// 我们把 imm32（+2 处）改成新的预算值。
constexpr uint32_t kCmpSiteRva = 0x003B4CF7;
constexpr uint32_t kOriginalCap = 0x77770000u;
// 计数器高位检查：总量 ≥ 4GiB 时会走另一条失败路径，因此有效上限必须 < 4GiB。
constexpr uint32_t kHardCeiling = 0xFFF00000u;
constexpr uint32_t kDefaultBudgetMb = 3500;
constexpr uint32_t kMinBudgetMb = 64;

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

uint32_t read_budget_mb() {
    if (g_dir[0] == 0) {
        return kDefaultBudgetMb;
    }
    wchar_t ini[MAX_PATH] = {};
    lstrcpynW(ini, g_dir, MAX_PATH);
    lstrcatW(ini, L"se9ccn_texture_budget.ini");
    return static_cast<uint32_t>(
        GetPrivateProfileIntW(L"sehwr", L"budget_mb", static_cast<INT>(kDefaultBudgetMb), ini));
}

// 只在目标位置字节符合预期时才下补丁。函数内不做堆分配，便于在 __try 中安全使用。
bool patch_cmp_immediate(uintptr_t base, uint32_t cap) {
    static const uint8_t kExpected[8] = {0x81, 0xFF, 0x00, 0x00, 0x77, 0x77, 0x72, 0x09};
    auto* site = reinterpret_cast<uint8_t*>(base + kCmpSiteRva);
    __try {
        if (std::memcmp(site, kExpected, sizeof(kExpected)) != 0) {
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    DWORD old_protect = 0;
    auto* imm = site + 2;
    if (VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old_protect) == FALSE) {
        return false;
    }
    const uint8_t bytes[4] = {static_cast<uint8_t>(cap & 0xFF), static_cast<uint8_t>((cap >> 8) & 0xFF),
                              static_cast<uint8_t>((cap >> 16) & 0xFF),
                              static_cast<uint8_t>((cap >> 24) & 0xFF)};
    std::memcpy(imm, bytes, 4);
    DWORD restored = 0;
    VirtualProtect(imm, 4, old_protect, &restored);
    FlushInstructionCache(GetCurrentProcess(), imm, 4);
    return true;
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
    log_line("game exe base = 0x%08X", static_cast<unsigned>(base));

    const uint32_t mb = read_budget_mb();
    if (mb == 0) {
        log_line("disabled: se9ccn_texture_budget.ini sets budget_mb=0");
        return;
    }
    const uint32_t mb_clamped = (mb < kMinBudgetMb) ? kMinBudgetMb : mb;
    const uint64_t wanted = static_cast<uint64_t>(mb_clamped) * 1024ull * 1024ull;
    const uint32_t cap =
        (wanted >= kHardCeiling) ? kHardCeiling : static_cast<uint32_t>(wanted);

    if (!patch_cmp_immediate(base, cap)) {
        log_line("error: signature mismatch at RVA 0x%08X —— 游戏版本不匹配或已被其它补丁修改，"
                 "本工具不修改任何字节。",
                 static_cast<unsigned>(kCmpSiteRva));
        log_line("       expected bytes: 81 FF 00 00 77 77 72 09 (CMP EDI,0x77770000 / JB +9)");
        return;
    }
    log_line("patched: texture budget 0x%08X (%.0f MiB) -> 0x%08X (%.0f MiB)",
             static_cast<unsigned>(kOriginalCap), kOriginalCap / 1048576.0, static_cast<unsigned>(cap),
             cap / 1048576.0);
    log_line("done. If you see texture losses still, lower budget_mb or check VRAM.");
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
