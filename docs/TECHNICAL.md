# 技术说明：Homeworld Remastered 的纹理内存预算与巡航下调方案

> 本文记录 `se9ccn_texture_budget` 的依据：地址、实测数据、以及为什么用"巡航下调计数器"
> 而不是"改比较指令阈值"。证据来自对 `HomeworldRM.exe`（2.303 / build 3409635）的
> 反汇编、资产普查与运行期观测。

## 1. 引擎里的纹理内存记账

纹理上传的唯一入口：`FUN_007B4C80`（RVA `0x3B4C80`，`__thiscall`）。反编译关键片段：

```c
uVar3 = (bpp * (fmt + 3) * w * h) / 6;      // 本张纹理的估算字节数
*(uint*)(tex + 0x18) = uVar3;               // 单张纹理记账
delta = uVar3 - old;
DAT_009aed28 += delta;                      // 64 位全局计数器（低 dword @0x9AED28，高 @0x9AED2C）
if (DAT_009aed2c >= 0 && (DAT_009aed2c != 0 || 0x77770000 <= DAT_009aed28)) {
    return 0;                               // 超预算：纹理创建失败
}
... glTexImage2D ...
```

对应机器码（文件偏移 `0x3B40F3` = RVA `0x3B4CF3`）：

```
78 13                    js   short ok        ; 高 dword 为负 → 放行
7F 08                    jg   short fail      ; 高 dword 为正（总量 ≥ 4GiB）→ 失败
81 FF 00 00 77 77        cmp  edi, 077770000h ; 阈值 ≈1.867 GiB
72 09                    jb   short ok
                         ; fail: xor al,al; ret 4
```

- `0x77770000` = **2,002,796,544 字节 ≈ 1.867 GiB**；全镜像里只出现一次；
- 计数器 `DAT_009aed28/2C` 是 64 位的，所有更新都是"加减 delta"（每张纹理创建 +size、
  销毁 −size），因此**整体平移计数器不会破坏记账**。

## 2. 为什么 4 个种族就吃满、8 个种族必炸

- 资产普查（`.big` 解包 + HOD 内嵌贴图描述符反算）：4 个可选种族的船贴图合计
  **1903.9 MB = 该预算的 99.2%**；
- 8 个不同种族的运行期实测：纹理计数器峰值 **2,004,963,872 字节（100.03% 预算）**，
  同时 `Unable to retrieve UI` 出现 2,422 条；若先失败的恰好是必需 UI 贴图，
  `UIGraphic::parseLODTextures`（RVA `0x2B8FF5`）会直接 `-- FATAL EXIT --`，原文
  `Unable to load texture (data:UI\Pings\Meshes\Circle.mtga)`。

## 3. 为什么选"巡航下调计数器"

| 方案 | 结果 |
|---|---|
| **巡航下调计数器（本工具 v1.3）** | 每 250ms 读计数器，>1500 MiB 就 −512 MiB。引擎所有基于该计数器的判断都看到余量；只动一个内存变量，不碰代码与文件。**与之前实测可玩的配置一致** |
| 改比较阈值（v1.1/v1.2 尝试过，已放弃） | 只绕过闸门里的一处判断；**Steam 正式安装的 8 种族局仍 FATAL**（当时计数器 ≈1.6 GB）→ 说明还有别的基于该计数器的判断 |
| Hook `FUN_007B4C80` 重写闸门 | 该函数是 `__thiscall` + 自定义栈清理、热路径；替身 ABI/返回值一旦有误会污染整个绘制流程 |
| 降低贴图分辨率 / LAA（4GB 地址空间） | 前者是 mod 侧改动；后者只扩 VA，实测 8 种族时进程 commit 仅约 1GB，与这个预算无关 |

## 4. 边界与安全

- 只影响"纹理预算"这一类判断，不改变显存容量：贴图总量超过显存时驱动回退到系统内存，
  表现为变慢/掉帧，而不是丢贴图；
- **不修改磁盘文件**：只改进程内计数器，重启即恢复，Steam 校验无影响；
- 启动时对目标 exe 做 8 字节签名校验（只读），版本不匹配则**不启用任何功能**（失败安全）；
- 计数器写入是"先写高 dword、再写低 dword"，最坏中间态只会让引擎看到更大的占用（下一拍修正）；
- 线程随 DLL 常驻（DLL 已 `GetModuleHandleEx` PIN），开销为每 250ms 两次 32 位读 + 偶尔一次写。

## 5. 验证方法

1. 启动游戏，看 `se9ccn_texture_budget.log` 是否有 `mechanism:` 与 `done:`；
2. 开一局 8 个不同种族的 mod 对局，观察日志里的 `counter shave` / `engine texture counter ≈`；
3. 游戏内 UI 图标不应再出现红黑方块；`HwRM.log` 里不应再出现
   `Unable to retrieve UI (animated) texture` 或 `Unable to load texture`。

## 6. 致谢

- 资产体量与运行期测量的方法与本项目（HWRReverseEngineering）共享；
- 感谢 9CCN mod 制作组的测试与反馈。
