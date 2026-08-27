# NTE / 国产游戏截帧支持说明

本目录记录让 RenderDoc 能够截帧 **《Neverness To Everness》(NTE)** 这类带
**ACE 反作弊 + 启动器接力链 + 捆绑旧版 CRT** 的国产游戏所做的改动与使用方法。

## 一、要解决的问题

### 1. 启动器接力链逃逸注入
NTE 的启动链不是单一进程，而是多跳接力：

```
任何方式启动 NTEGame.exe(壳)
   └─(CreateProcessW)→ NTELauncher.exe
        └─(NtCreateUserProcess)→ NTEUpdate.exe (更新/自检)
             └─(NtCreateUserProcess)→ NTEGame.exe (真正的启动器 UI)
                  └─(NtCreateUserProcess)→ HTGame.exe (游戏, UE4, D3D11加载/D3D12主渲染)
```

其中 `NTELauncher → NTEUpdate → 真UI` 这两跳**不走任何 `CreateProcess*` 变体**，
而是直接调 `ntdll!NtCreateUserProcess`，因此原有的 `CreateProcessA/W/AsUser/WithLogonW`
hook 全部抓不到，真 UI 和游戏都逃逸注入。

**改动**（`renderdoc/os/win32/sys_win32_hooks.cpp`）：
- 新增 `CreateProcessInternalW` / `CreateProcessInternalA` hook（shell32/CRT spawn 路径）。
- 新增 `CreateProcessWithTokenW` hook（补全 CreateProcess 家族）。
- 新增 **`NtCreateUserProcess` hook**——所有用户态进程创建的最终通道。hook 里强制
  `THREAD_CREATE_FLAGS_CREATE_SUSPENDED`，然后走与 `CreateProcess` hook 相同的
  “入口点 hijack 注入 → 恢复线程”流程。这是抓住接力链的关键。

### 2. 捆绑旧版 CRT 导致注入即崩（`ERROR_DLL_INIT_FAILED 1114`）
`rendertest.dll` 用 VS2022 编译（新 CRT），而启动器在 `runtime\` 目录捆绑了 2019 年的旧
CRT（`msvcp140.dll 14.24` 等）。注入时 Windows 按模块名把 DLL 绑定到已加载的旧 CRT，
新旧不兼容，初始化阶段构造 `std::mutex` 直接崩溃。

且启动器每次运行会**自愈**——文件校验把替换的新 CRT 还原回旧版。

**改动**（`renderdoc/os/win32/win32_process.cpp` + `renderdoc/os/os_specific.h`）：
- 新增 `Process::FixBundledCRTForTarget(appPath)`：检测目标 exe 同目录及其 `runtime\`
  子目录里的捆绑 CRT，版本低于 14.40 时用 `System32` 里的新版替换（旧文件备份到
  `crt_backup\`）。
- 在 `RunProcess`（RenderDoc launch 路径）调用。
- 在 `ShouldInject`（`sys_win32_hooks.cpp`，所有子进程 hook 的共同入口）里，决定注入
  某个子进程时**先调用它再创建进程**——这样接力链每一跳拉起子进程前都会重新替换，
  与启动器的自愈赛跑且必胜（我们抢在进程创建前一刻替换）。

### 3. 子进程 hook 传染导致的副作用
接力链开启 `hookIntoChildren` 后会传染给所有后代进程，其中两类会引发问题：
- **CEF/浏览器进程**（`NTEBrowser.exe`、`NTEWebBooster.exe`、`EpicWebHelper.exe`）：
  注入会让登录用的网页组件坏掉 → 游戏卡在登录/白屏。
- **NVIDIA NGX 更新工具**（`nvngx_update.exe`）：游戏启动时反复拉起它，每个都被注入
  导致 spawn 失败被无限重试 → 启动永远完不成（白屏）。

**改动**（`sys_win32_hooks.cpp` 的 `IsUtilityProcess` 黑名单）：
- 追加 `epicwebhelper.exe`、`ntebrowser.exe`、`ntewebbooster.exe`、`nvngx`——这些工具
  进程不注入。

### 4. 反作弊下启动极慢
ACE 在游戏登录流程下会疯狂调用 `GetProcAddress` 解析函数（上万次），每次都打在我们的
hook 上并写一条 Debug 日志，175 个线程在全局日志锁上争抢，启动从 20 秒变成 10+ 分钟
（表现为长时间白屏）。

**改动**（`renderdoc/common/globalconfig.h`）：
- `STRIP_DEBUG_LOGS` 置 `OPTION_ON`——关掉 Debug 级日志（GetProcAddress/IAT patch
  刷屏），保留 Comment/Warning/Error 级（所有关键诊断不受影响）。启动速度恢复正常。

### 5. D3D12 视口创建 `E_NOINTERFACE` 崩溃
游戏运行中触发 D3D12 视口创建时，向包装设备查询基础接口 `ID3D12Object`
（`{c4fec28f-...}`），包装层没有该分支返回 `E_NOINTERFACE`，被 UE4 当致命错误。

**改动**（`renderdoc/driver/d3d12/d3d12_device.cpp`）：
- `WrappedID3D12Device::QueryInterface` 增加 `ID3D12Object` 分支，转发给真实设备
  （它是 SetName/GetPrivateData 等纯元数据接口，与截帧无关）。

另在 `renderdoc/driver/d3d12/d3d12_device_wrap.cpp` 的 `CreateCommandQueue` 增加了
失败日志（仅排错用，不影响行为）。

## 二、HEAD 提交已包含的 inline hook 基础设施

`8b0d1498a "PC 鸣潮支持"` 已提交：
- `renderdoc/hooks/inline_hook.h`：x64 inline hook（断点+trampoline）基础设施。
- `renderdoc/driver/d3d12/d3d12_hooks.cpp`、`renderdoc/driver/dxgi/dxgi_hooks.cpp`：
  对 `d3d12.dll` / `d3d12core.dll`(Agility SDK) / `dxgi.dll` 的导出函数安装 **inline
  hook**，用于抓住反作弊驱动通过" stealth 路径"（绕过 IAT/GetProcAddress 拦截）发起的
  `D3D12CreateDevice` / `CreateDXGIFactory1/2` 调用，确保设备/工厂都被包装。

## 三、使用方法（纯 UI 流程，推荐）

1. 打开 `qrendertest`。
2. Launch Application → Executable 填启动器壳：
   `C:\GAME\Neverness To Everness\NTELauncher\NTEGame.exe`
3. **勾选 `Hook into Children`**（接力链每一跳靠它传染注入）。
4. Launch，等启动器界面出来。
5. 点"开始游戏"→ 游戏被自动注入 → 按 **F12** 截帧。

CRT 替换已由 DLL 自动完成，**无需手动操作**。

## 四、备用流程（对运行中的启动器布防）

`launch_and_arm.ps1`：先手动正常启动启动器，再运行此脚本完成 CRT 替换 + 注入
（同样带 `--opt-hook-children`）。改脚本顶部的两个路径即可。

## 五、已知注意事项

- **CRT 自愈**：启动器每次运行会把替换的新 CRT 还原。DLL 的 `FixBundledCRTForTarget`
  会在每次 launch / 子进程创建前重新替换，自动对抗。手动改文件则会被还原。
- **崩溃偶发**：D3D12 inline hook 通过 watcher 线程在 `d3d12.dll` 加载时安装，若游戏
  首次 `D3D12CreateDevice` 与 watcher 安装存在竞态，个别情况下 D3D12 视口创建仍可能
  偶发 `E_NOINTERFACE`。若频繁复现，可考虑把 inline hook 安装改为库加载回调里同步完成。
- **ACE 探测**：`IDXGIAdapterInternal2`、`ID3D12DeviceDriverDetails_RS5` 等接口查询在
  日志里会有 unsupported 警告，属正常探测，非致命。

## 六、已诊断问题：截帧时崩溃（DEVICE_HUNG，暂未修复）

**现象**：按 F12 截帧，快照进行到一半游戏崩溃，UE4 崩溃框报 **"GPU Crash dump Triggered"**。
资源越多（玩得越久/加载越多）越容易触发，即"截帧太多会崩"。

**诊断结论（2026-08-27，非内存不足）**：
- 崩溃发生在 D3D12 初始状态捕获阶段，日志关键序列：
  ```
  Starting capture
  Preparing up to 14914 potentially dirty resources   ← 约 1.5 万个 D3D12 资源要拍快照
  DXGI_ERROR_DEVICE_HUNG        (d3d12_device.cpp:4969)
  No DRED page fault information / 0 DRED nodes found  ← 无显存页面错误
  Couldn't create readback buffer: DXGI_ERROR_DEVICE_REMOVED（级联失败）
  ```
- 根因是 **GPU TDR 超时**：快照 ~1.5 万个资源让 GPU 连续忙超过 **Windows 默认 `TdrDelay=2s`**，
  系统判定显卡无响应并强制重置（`TdrLevel=3`）→ 设备 `DEVICE_REMOVED` → 游戏崩溃。
- **排除 OOM / 显存溢出**：错误是 `DEVICE_HUNG`（挂起）而非分配失败；DRED 无任何 page
  fault 记录 = 纯超时，非显存耗尽、非坏绘制。

**可选修复（当前未启用，用户决定维持现状）**：
- 调大 TDR 超时（注册表，需管理员 + 重启，非代码改动）：
  `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers`，DWORD `TdrDelay = 60`。
  让大快照跑完而不被系统重置。副作用：整机遇到真显卡死机时恢复也会变慢。
- 代价提示：即便调大 TDR，1.5 万资源快照也会让游戏卡数秒（能截下，但慢）。
- （代码层面备选，暂缓）：截帧时跳过辅助的 Intel/WARP D3D12 设备的初始状态快照，或缩小
  快照范围，可降低快照耗时。
