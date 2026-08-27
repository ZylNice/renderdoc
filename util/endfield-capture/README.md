# 终末地（Arknights Endfield）截帧支持说明

本目录记录让 RenderDoc（rendertest fork）能够截帧 **《明日方舟：终末地》** 这类带
**ACE 反作弊（AntiCheatExpert）+ CrashSight + Unity（Vulkan 主渲染）** 的国产游戏
所做的改动、踩过的坑与使用方法。

与 NTE 不同，终末地是**单进程游戏**（`Endfield.exe` 即游戏本体，无启动器接力链），
但会拉起若干子进程：`ACE-Setup64.exe`（反作弊安装器）、`UnityCrashHandler64.exe`、
`PlatformProcess.exe`、`nvidia-smi.exe`（硬件信息采集）。

## 一、要解决的问题

### 1. `CreateRemoteThread` 注入会被 CrashSight / ACE 检测
经典注入远程线程是反作弊与崩溃上报系统重点监控的特征。

**改动**（`renderdoc/os/win32/win32_process.cpp`）：
- 新增 **SetThreadContext 主线程劫持注入**，替代 `CreateRemoteThread`：
  - `GetSuspendedMainThread`：仅当目标是由 RenderDoc 全新启动、主线程仍挂起的进程
    （有且仅有一个线程且处于挂起态）才启用劫持。
  - `InjectDLLThreadContext`：从远程 PEB + PE 头读出 EXE 入口点 → 把入口点 patch 成
    2 字节自旋（`EB FE`）→ `ResumeThread` 让进程跑完初始化（**等静态导入的图形 API
    DLL 加载完**——若在初始化前劫持，这些导入会走 ntdll 内部 loader，绕过 LoadLibrary
    hook，任何图形 API 都探测不到）→ 到达入口点后恢复原始字节并把 Rip 归一到入口点
    → 再 `SetThreadContext` 把 Rip 指向远程 stub 执行 `LoadLibrary(rendertest.dll)`。
  - 劫持失败时 `RestoreHijackedThread` 还原现场，回退到经典 `CreateRemoteThread`
    （`InjectDLL`）。
- 日志识别（成功路径）：
  ```
  win32_process.cpp - Process N: main thread is suspended - hijack injection will be used
  win32_process.cpp - Hijack: process reached its entry point after XXXms
  win32_process.cpp - Process N: rendertest.dll loaded at 0x...
  ```
- 注意：只对"从 RenderDoc 启动"的进程生效；attach 已运行进程仍走
  `CreateRemoteThread`，仍可能被检测。

### 2. 官方 RenderDoc 全局 Vulkan 层"连坐" → 游戏启动 60 秒整被杀（核心坑点）
**现象**：注入、hook、截帧全部成功，但每个游戏实例都在启动后 **约 60 秒整**被杀
（目标控制连接 `WSAECONNRESET`，游戏侧无任何崩溃日志——外部 `TerminateProcess`
或 SDK 主动退出，非崩溃）。多个实例死亡时间 58~61s 高度一致 = ACE 服务首轮定时
扫描的处置节奏。

**根因链**：
1. 机器装过官方 RenderDoc → 安装器把官方 implicit layer 全局注册到
   `HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers`（`renderdoc.json`）。
2. fork 注入时往游戏环境写 `ENABLE_VULKAN_RENDERDOC_CAPTURE=1`，**与官方层的
   enable 变量同名** → 官方 `renderdoc.dll` 也被 Vulkan loader 拉进游戏。
3. 官方 build 编译了 Breakpad 崩溃处理器（`renderdoc/core/crash_handler.h` 要求
   `RDOC_RELEASE && RENDERDOC_OFFICIAL_BUILD`；我们的 Development 构建没有）→ 它在
   游戏进程内拉起子进程 `C:\Program Files\RenderDoc\renderdoccmd.exe`
   （日志特征：T+8s 左右 `NtCreateUserProcess entered, ... image=C:\Program
   Files\RenderDoc\renderdoccmd.exe`）。
4. ACE（T+4s 由游戏拉起 `ACE-Setup64.exe`）做进程树/模块扫描，一眼看到
   "RenderDoc" 字样的子进程和模块 → T+60s 杀游戏。

**修复**（注册表，需管理员；值 `0`=启用，`1`=禁用）：
```bat
reg add "HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers" /v "C:\Program Files\RenderDoc\renderdoc.json" /t REG_DWORD /d 1 /f
reg add "HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\ImplicitLayers" /v "C:\Program Files\RenderDoc\x86\renderdoc.json" /t REG_DWORD /d 1 /f
```
fork 自己的 `rendertest.json` 两条注册保持 `0` 不动。

**排查方法**（如何定位到官方层而非 fork 自身）：
- Development 构建不含崩溃处理器 → 不是 fork 拉起的 renderdoccmd；
- 搜遍 DLL 侧代码无任何 `LaunchProcess("renderdoccmd"...)` 调用点 → 代码里没有；
- 被拉起的路径是官方安装目录而非构建目录 → 只能是官方 DLL 的崩溃处理器所为；
- `reg query` 确认两条全局层注册同时存在，坐实官方层被同名变量误激活。

**注意**：重装/升级官方 RenderDoc 后安装器会重新启用这两条注册，若游戏重新开始
60 秒死亡，首先复查本节。

### 3. 勾选 `Capture Child Processes` 导致子进程注入失败误报 + 反作弊风险
**现象**：UI 报 `Failed to inject rendertest.dll into process`，看似"无法注入游戏"。

**实际**：游戏本体注入是成功的；失败的全部是**短命子进程**
（`PlatformProcess.exe`、`nvidia-smi.exe`）——劫持线程到达入口点后，
`HijackedExec` 在等待远程 `LoadLibrary` 完成期间进程已退出
（`win32_process.cpp(458) Process exited while waiting for hijacked call`），
回退 `CreateRemoteThread` 时进程已死，于是报"无法注入"。

另外勾选后还会给 `ACE-Setup64.exe`（反作弊安装器）注入 DLL，白白扩大检测面。

**对策**：终末地是单进程游戏，**不要勾 `Capture Child Processes`**（与 NTE 接力链
恰好相反——NTE 必须勾）。

**陷阱**：勾选状态在同一 qrendertest 进程的 CaptureDialog 内是持久的；点
"Load Last Capture" 也会从 `most_recent.cap` 恢复旧设置。从 NTE 流程切换目标到
终末地时极易忘记取消（12:11 会话即因此产生一串子进程注入失败报错，12:29 会话
未勾则完全正常）。

## 二、使用方法（纯 UI 流程，推荐）

1. 以**管理员**运行 `qrendertest`。
2. Launch Application → Executable 填：
   `C:\Application\Hypergryph Launcher\games\Arknights Endfield\Endfield.exe`
3. **确认不勾 `Capture Child Processes`**。
4. Launch，进游戏后按 **F12** 截帧。
5. 正常日志序列：
   ```
   hijack injection will be used → rendertest.dll loaded → 注册全部 API hooks
   → Vulkan layer activated → Used API: Vulkan (Presenting & supported)
   ```
   截帧参考：约 9000+ 待快照资源，捕获段 ~3000MB / ~5s，写盘压缩约 62%。

## 三、日志中的无害项（不要误判为故障）

- `dxgi_wrapped.cpp - Error - Creating swap chain with non-hooked device!`：
  游戏主渲染是 Vulkan，某组件用未包装的设备创建了一个 DXGI swapchain，被拒绝包装
  但不影响 Vulkan 截帧。
- `D3D12GetInterface ... E_NOINTERFACE` / `Unknown UUID` /
  `IDXGIAdapterInternal2` unsupported：ACE 的接口探测，正常。
- 游戏启动时会创建一个 D3D12 设备（被包装后随即移除 capturer）：正常，主渲染仍是
  Vulkan。
- `Socket unexpectedly disconnected` / `Didn't get proper handshake` 周期刷屏：
  adb 远程设备探测噪音，与游戏无关。
- `CreateToolhelp32Snapshot(pid) -> 0x00000018` 偶发：模块快照竞态，重试即成功。

## 四、已知注意事项

- 游戏必须经 qrendertest **Launch**（劫持注入依赖"新建且挂起"的进程）；先启动游戏
  再 attach 会退化为 `CreateRemoteThread`，可能被 ACE/CrashSight 检测。
- 当前仍存在但未被 ACE 触及的**残留指纹**：未签名的 `rendertest.dll` 模块本体、
  d3d12/dxgi/ntdll 上的 inline hook（`.text` 完整性校验可发现）、环境变量名
  `ENABLE_VULKAN_RENDERDOC_CAPTURE` 本身。ACE 版本升级后阈值可能收紧，届时可选的
  进一步手段：层名/enable 变量名去 RenderDoc 化（如 `VK_LAYER_RENDERTEST_Capture`
  / `ENABLE_VULKAN_RENDERTEST_CAPTURE`）、DLL 内 RenderDoc 字符串清理、inline hook
  隐藏（或改为库加载回调内同步安装，避免 watcher 竞态）。
- 验证"60 秒死亡"是否复发的方法：看新日志 T+8s 附近是否还有
  `image=C:\Program Files\RenderDoc\renderdoccmd.exe`，以及 T+60s 是否出现
  `WSAECONNRESET`。
