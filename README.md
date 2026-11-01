# se9ccn_texture_budget

Homeworld Remastered 的**纹理内存预算解锁器**（独立 DLL，GPLv3）。
由 9CCN mod 制作组发布。

> **English** summary at the bottom. 中文说明在前。

---

## 这是什么 / 解决什么问题

Homeworld Remastered（`HomeworldRM.exe`，32 位）的渲染代码里维护了一个
**纹理字节计数器**，并围绕它做了硬编码的**预算判断（≈1.867 GiB，`0x77770000`）**：
超过预算后，新纹理创建会**静默返回 0**。

- 原版的 4 个可选种族就已经把这个预算吃到 **99.2%**；
- 大型 mod 的 **8 个不同种族**对局，实测贴图需求约 **2.6 GB 以上**；
- 结果就是大家熟悉的现象：
  - UI 图标变红黑方块（`Unable to retrieve UI (animated) texture (...)`）；
  - 或者加载到必需 UI 贴图时直接弹 **`-- FATAL EXIT --`**
    （`Unable to load texture (data:UI\Pings\Meshes\Circle.mtga)`，`uigraphic.cpp:267`）。

## 原理：启动时把计数器一次性下移 32 GiB（不改任何代码/文件）

游戏启动时会枚举 `<exe目录>\se*.dll` 并 `LoadLibrary` + `GetProcAddress("InspectDLL")`。
本 DLL 借用这个加载时机（`InspectDLL` 回调里，已脱离 loader lock）做两件事：

1. **只读校验**目标 exe 是分析过的构建（比对纹理闸门处的 8 字节签名；不匹配就什么都不做）；
2. 把引擎那个 64 位纹理计数器（RVA `0x5AED28/0x5AED2C`）**整体减去 32 GiB**。

为什么这样就够了：闸门对计数器的判断是"**负数 → 无条件放行**"（`T < 0` 走 `JS` 分支，
详见 `docs/TECHNICAL.md`）。一次下移 32 GiB 后，计数器要净增 ≈34 GiB 纹理才会重新进入
"拒绝区"，而 8 个不同种族一场也就 2.6~5 GB —— 所以整场都不会再拒绝纹理。写入发生在任何
纹理创建之前（无竞争），不需要反复改写。

一个 **2 秒一次的低频看门狗**只在"计数器被重置/爬回来"这类异常时补一次偏移，正常对局不会触发。

为什么这样做是安全的：引擎对这个计数器的所有更新都是"加减 delta"（每张纹理创建时 +size、
销毁时 −size），**整体平移不会破坏记账**，只是让引擎的各种"快满了"判断始终看到余量。

为什么不用"直接改比较阈值"：我们试过——把闸门里 `CMP EDI,0x77770000` 的阈值改大甚至去掉，
Steam 正式安装的 8 种族局**仍然**在同一张 `Circle.mtga` 上 `FATAL EXIT`（当时计数器才 ≈1.6 GB）。
说明除了闸门本身，还有别的基于这个计数器的判断；整体下移把这一类判断一并覆盖。

**不修改磁盘文件**：所有操作只发生在进程内存里，游戏重启即恢复原状（再加载本 DLL 再执行一次），
Steam「验证文件完整性」也不会受影响。

## 安装

1. 把 `se9ccn_texture_budget.dll` 复制到游戏目录：
   `...\Homeworld Remastered Collection\HomeworldRM\bin\Release\`
   （与 `HomeworldRM.exe` 同一目录；游戏会自动加载所有 `se*.dll`）。
2. 启动游戏。同目录会出现日志 `se9ccn_texture_budget.log`，例如：

   ```
   [2026-11-01 23:25:01] se9ccn_texture_budget v1.4.0 starting (Nov  1 2026 23:24:58)
   [2026-11-01 23:25:01] game exe base = 0x006E0000
   [2026-11-01 23:25:01] mechanism: 启动时把纹理计数器整体下移 32768 MiB（一次写入），之后每 2000 ms 由看门狗兜底
   [2026-11-01 23:25:01] counter offset applied (startup): 0 MiB -> -32768 MiB
   [2026-11-01 23:25:01] done: 看门狗线程已启动（正常对局不会触发）。
   ```
   进入对局后正常不会有额外日志（看门狗只在异常时补偏移）。

   **日志每次都重写**（不会随游玩次数增长），并且单次运行最多 256 KB。

## 配置：不需要

偏移量（32 GiB）与看门狗阈值（-16 GiB）是固定常量；
要调整的话改 `src/se9ccn_texture_budget.cpp` 顶部的 `kOffsetBytes` / `kReapplyThreshold` 重新编译即可。

## 卸载

删除 `se9ccn_texture_budget.dll`（以及可选的日志）即可；游戏文件从未被修改。

## 从源码构建

需要 Windows + Visual Studio（"使用 C++ 的桌面开发"）+ CMake。

```powershell
pwsh -File build.ps1            # 输出 se9ccn_texture_budget.dll（32 位 Release）
pwsh -File build.ps1 -Clean     # 清理后重建
```

或者手动：

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物在 `build/bin/se9ccn_texture_budget.dll`。必须是 **x86（32 位）** 构建。

仓库自带 GitHub Actions（`.github/workflows/build.yml`）：
- push / PR → 自动构建 x86 DLL 并校验 PE machine，产物作为 Artifact 上传；
- 推送 `v*` tag → 自动创建 GitHub Release 并把 DLL 作为附件上传。

## 兼容性与已知限制

- 实测版本：Homeworld Remastered **2.303 / build 3409635**（Steam 正式安装）；
- 其它版本：启动时做 8 字节签名校验，不匹配则**不启用任何功能**并在日志里报错（失败安全）；
- 只解决"引擎纹理预算"这一道软件闸门，不解决显存不足、贴图分辨率过高、或 mod 自身 bug；
- 贴图总量超过显存时，驱动会回退到系统内存：表现为变慢/掉帧，而不是丢贴图；
- 与其它 `se*.dll` 插件共存；如果同时有别的工具也在改纹理预算，请只保留一个；
- 单人 / 合作 / 局域网 mod 游戏适用；请不要用于任何形式的对抗性场景。

## 许可

**GNU General Public License v3.0 or later**（GPL-3.0-or-later）。
完整文本见 [`LICENSE`](LICENSE)。

技术细节（地址、实测数据、方案取舍）见 [`docs/TECHNICAL.md`](docs/TECHNICAL.md)。

---

## English

**se9ccn_texture_budget** works around the hard-coded texture-memory budget of
*Homeworld Remastered* (≈1.867 GiB) that makes large mods lose UI textures or die
with `-- FATAL EXIT --` (`Unable to load texture ... Circle.mtga`, `uigraphic.cpp:267`).

The DLL is loaded by the game's own `se*.dll` plugin scan at startup. It verifies
the game build with an 8-byte read-only signature check, then subtracts 32 GiB
from the engine's 64-bit texture-memory counter exactly once. The gate treats any
negative counter as "plenty of space", and 32 GiB is far more than a large mod
match consumes (≈2.6–5 GB), so no texture is ever rejected. A 2-second watchdog
only re-applies the offset if the counter is ever reset. The counter is updated
by deltas only, so shifting it does not break bookkeeping.

**No game file is modified** (the effect is in-process only; a restart resets it).
Log: `se9ccn_texture_budget.log` next to the DLL.

Install: copy `se9ccn_texture_budget.dll` into `HomeworldRM\bin\Release\`.
Build: `pwsh -File build.ps1` (32-bit MSVC + CMake). License: GPL-3.0-or-later.
