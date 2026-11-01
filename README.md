# se9ccn_texture_budget

Homeworld Remastered 的**纹理内存预算解锁器**（独立 DLL，GPLv3）。
由 9CCN mod 制作组发布。

> **English** summary at the bottom. 中文说明在前。

---

## 这是什么 / 解决什么问题

Homeworld Remastered（`HomeworldRM.exe`，32 位）的渲染代码里硬编码了一个
**纹理字节预算 ≈ 1.867 GiB**（`0x77770000`）。所有纹理上传都要经过同一道闸门
（`FUN_007B4C80`）：累计字节超过该值后，**新纹理创建直接返回 0（静默失败）**。

- 原版的 4 个可选种族就已经把这个预算吃到 **99.2%**；
- 而大型 mod 的 **8 个不同种族**对局，实测贴图需求约 **2.6 GB**；
- 结果就是大家熟悉的现象：
  - UI 图标变红黑方块（`Unable to retrieve UI (animated) texture (...)`）；
  - 或者加载到必需 UI 贴图时直接弹 **`-- FATAL EXIT --`**
    （`Unable to load texture (data:UI\Pings\Meshes\Circle.mtga)`，`uigraphic.cpp:267`）。

本 DLL 在**游戏启动时**（`se*.dll` 被枚举加载的那一刻）对内存中的那一条比较指令
做一次 **4 字节补丁**，把上限改成**引擎允许的最大值**（`0xFFF00000` ≈ **4095 MiB**）：

```asm
; 补丁前
81 FF 00 00 77 77   cmp edi, 077770000h   ; ≈1.867 GiB
72 09               jb  ok                 ; 小于上限就继续
; 补丁后（imm32 被改为 0FFF00000h）
81 FF 00 00 F0 FF   cmp edi, 0FFF00000h  ; ≈4095 MiB（引擎上限）
72 09               jb  ok
```

- **不修改任何游戏文件**（磁盘上的 exe 原封不动，Steam「验证完整性」不受影响）；
- 补丁前做**字节签名校验**，游戏版本不匹配时拒绝修改并写日志；
- 不注册为音频设备（遵守游戏的 `se*.dll` 插件协议，只借用启动时机）。

## 安装

1. 把 `se9ccn_texture_budget.dll` 复制到游戏目录：
   `...\Homeworld Remastered Collection\HomeworldRM\bin\Release\`
   （与 `HomeworldRM.exe` 同一目录；游戏会自动加载所有 `se*.dll`）。
2. 启动游戏。同目录会出现日志 `se9ccn_texture_budget.log`，内容示例：

   ```
   [2026-11-01 22:40:01] se9ccn_texture_budget v1.0.0 starting (Nov  1 2026 22:38:12)
   [2026-11-01 22:40:01] game exe base = 0x007A0000
   [2026-11-01 22:40:01] patched: texture budget 0x77770000 (1867 MiB) -> 0xDE800000 (3560 MiB)
   ```
   看到 `patched:` 就说明生效了。

## 配置：不需要

预算**固定为引擎允许的最大值**（`0xFFF00000` ≈ 4095 MiB；再大就会撞上引擎 4 GiB 的高位检查）。
本工具不读任何配置文件，装上就生效。

为什么不留参数：贴图加载失败会让游戏**直接不可玩**（丢 UI、甚至 `FATAL EXIT`），
而显存不足最多只是帧率下降。两者代价不对称，所以默认直接给到上限。
2026 年了，4GB 显存已经是底线配置，再让用户为这个开关做选择没有意义。

如果显存确实不够，表现为卡顿/掉帧（而不是丢贴图）——这时应该降低 mod 的贴图分辨率，
而不是把预算调小。

## 卸载

删除 `se9ccn_texture_budget.dll`（以及可选的日志 `se9ccn_texture_budget.log`）即可；游戏文件从未被修改。

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

## 兼容性与已知限制

- 实测版本：Homeworld Remastered **2.303 / build 3409635**；
- 其它版本：补丁点做 8 字节签名校验，不匹配则**拒绝修改**并在日志里报错；
- 只解决"引擎纹理预算"这一道闸门，不解决显存不足、贴图分辨率过高、或 mod 自身 bug；
- 与其它 `se*.dll` 插件共存，但如果同时有别的工具也在改纹理预算，请只保留一个；
- 单人 / 合作 / 局域网 mod 游戏适用；请不要用于任何形式的对抗性场景。

## 许可

**GNU General Public License v3.0 or later**（GPL-3.0-or-later）。
完整文本见 [`LICENSE`](LICENSE)。

技术细节（地址、字节、实测数据、为什么这样改）见 [`docs/TECHNICAL.md`](docs/TECHNICAL.md)。

---

## English

**se9ccn_texture_budget** raises the hard-coded texture memory budget of *Homeworld
Remastered* (≈1.867 GiB) that makes large mods lose UI textures or die with
`-- FATAL EXIT --` (`Unable to load texture ... Circle.mtga`, `uigraphic.cpp:267`).

The DLL is loaded by the game's own `se*.dll` plugin scan at startup and patches a
single 4-byte immediate in memory (`cmp edi, 0x77770000` → `cmp edi, <budget>`).
**No game file is modified**; the patch site is byte-signature-verified and the
patch is skipped on unknown builds. The budget is fixed to the engine maximum
(0xFFF00000 ≈ 4095 MiB); there is no configuration file. Log:
`se9ccn_texture_budget.log` next to the DLL.

Install: copy `se9ccn_texture_budget.dll` into `HomeworldRM\bin\Release\`.
Build: `pwsh -File build.ps1` (32-bit MSVC + CMake). License: GPL-3.0-or-later.
