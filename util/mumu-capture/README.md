# MuMu 模拟器截帧支持说明

本目录记录让 RenderDoc（rendertest fork）能够截帧 **MuMu 模拟器** 这类
"渲染设备与上屏设备分离"的模拟器所做的改动、踩过的坑与使用方法。

## 一、背景：为什么原版截不到帧

雷电、MuMu 等模拟器里，渲染和呈现通常是分开的：

- **渲染设备（Device A）**：在后台用 Vulkan 把游戏画到离屏 Buffer。
- **上屏设备（Device B）**：用 GLES 或另一个 Vulkan 实例，把画好的离屏 Buffer
  刷到屏幕。

原版 RenderDoc 默认只盯负责上屏的那个设备。点"截帧"只抓到上屏设备那几条
Presentation 命令（~30MB，没有真正的 Draw Call），而后台真正渲染的设备被无视。

## 二、已实现：多设备 Vulkan 桥接级联捕获

思路：既然后台有多个 Vulkan 设备实例，就写一个桥接把它们全绑在一起——只要
有一个设备触发了捕获，其它所有 Vulkan 设备都跟着一起
`StartFrameCapture` / `EndFrameCapture`。

引入自提交 `8361dcee9 "MuMu模拟器截帧完整支持（终末地）"`，**无条件编译、无
开关**，不用改代码即生效。

### 实现要点

- **触发点**：`renderdoc/driver/vulkan/vk_core.cpp` 的 `WrappedVulkan::Present()`
  里把原版的单设备 `Start/EndFrameCapture` 换成桥接版
  `RenderDoc::StartVulkanBridgeFrameCapture` /
  `EndVulkanBridgeFrameCapture`。
- **桥接本体**：`renderdoc/core/core.cpp` 的三个函数
  `StartVulkanBridgeFrameCapture` / `EndVulkanBridgeFrameCapture` /
  `DiscardVulkanBridgeFrameCapture`。状态在
  `renderdoc/core/core.h` 的 `m_VulkanBridgeCaptureOwner` /
  `m_VulkanBridgeCapturesActive`。
- **不另建注册表**：复用 RenderDoc 原有的 `m_DeviceFrameCapturers`，触发时按
  驱动类型过滤（Vulkan + OpenGL，见下方"GL follower 级联"），因此不用改任何
  设备实例的构造函数。
- **多 capture 落盘不互相覆盖**：非触发设备用 `DeviceOwnedWindow(NULL, NULL)`，
  `WrappedVulkan::EndFrameCapture` 对 NULL window 兼容；多份 capture 由
  `RenderDoc::CreateRDC` 的后缀去重（`_2`、`_3`…）写成独立 `.rdc`。
- **生命周期**：`RemoveDeviceFrameCapturer` 时从活动列表摘除并清 owner，带
  owner 去重，避免重复触发 / 漏收尾。

### 怎么验证级联生效

截帧后 grep 目标进程的应用日志（`%TEMP%\RenderTest\RenderDoc_<时间>.log`）：

```
Cascade bridge diagnostics: starting N capturers
```

（旧构建的日志前缀为 `Vulkan bridge diagnostics`，语义相同。）

- `N >= 2`：级联生效，多个设备一起被抓。
- `N == 1`：只抓到一个设备（触发进程里那一刻活的可级联设备只有 1 个）。

触发点已同时挂在 Vulkan 和 OpenGL 的 Present（`WrappedVulkan::Present` 与
`WrappedOpenGL::Present`，后者见 `gl_driver.cpp`）——上屏设备走 GLES 时也能
发起级联，owner 不再限制驱动类型（`StartVulkanBridgeFrameCapture` 中放开）。
follower 收 Vulkan + OpenGL 设备（见下）；D3D11/D3D12 触发点未挂。

### GL follower 级联（2026-09-28）

MuMu GL 渲染模式下，游戏画面由**上屏同进程**的多个 GL sharegroup（离屏渲染
上下文）绘制，owner（上屏 GL 上下文）触发时它们必须作为 follower 一起抓，
否则只抓到合成 blit。为此 follower 过滤器从"仅 Vulkan"放开为
"Vulkan + OpenGL"（`core.cpp` `StartVulkanBridgeFrameCapture`）。

配套保护：GL follower 的 `Start/EndFrameCapture` 由 owner 的 Present 线程
跨实例调用，`MakeValidContextCurrent` 用 sharegroup 的 backdoor 上下文兜底；
若该 sharegroup 从未 activate 过任何上下文（`m_LastContexts` 为空），
`WrappedOpenGL::StartFrameCapture` 会跳过本次捕获（`RDCWARN` + 状态回滚），
不再走到空上下文崩溃。`glLock` 为全局可重入锁（CRITICAL_SECTION），同线程
重入安全，跨线程只是普通竞争。

### 已知限制

1. **同进程内级联**：`RenderDoc::Inst()` 与 `m_DeviceFrameCapturers` 都是进程
   内状态，**跨进程的 Device A / Device B 桥接不到**。若渲染进程和上屏进程
   不是同一个，需另加跨进程通道（共享内存注册表或走 target-control）。
2. **~~触发点只挂在 Vulkan 的 Present~~**（已修）：OpenGL 的 Present 同样会
   触发桥接；**~~follower 只收 Vulkan~~**（已修 2026-09-28）：GL 渲染设备
   也参与级联。D3D11/D3D12 仍未挂触发点、也不收为 follower。
3. **离屏设备 capture 质量待修（未修）**：非触发设备从不 present，桥接对它们
   用 `EndFrameCapture(DeviceOwnedWindow(NULL, NULL))`，
   `WrappedVulkan::EndFrameCapture` 在 `windowHandle == NULL` 时拿不到 swapchain
   边界，抓出的帧没有正常 present 收尾，action 树不完整。这类 `capture_N.rdc`
   可加载但事件树残缺（见下一节）。

### 坑：官方 RenderDoc 的 Vulkan layer 抢注（2026-09-28 定位）

**现象**：GL 模式截帧一直 N=1，且目标进程里同时存在 `rendertest.dll` 和
`C:\Program Files\RenderDoc\renderdoc.dll` 两份注入。

**根因**：MuMuNxDevice.exe 运行中会创建 Vulkan 实例（渲染探测/游戏渲染），
Vulkan loader 会加载所有启用的隐式 layer。rendertest 与官方 RenderDoc 的
manifest 同名（`VK_LAYER_RENDERDOC_Capture`）且都注册在
`HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers`（含 WOW6432Node），loader 对同名
layer 去重只留一个——官方版胜出时，Vulkan 设备就注册进官方 build 的进程内表，
rendertest 的级联表里永远看不到 → N=1。官方 qrenderdoc 每次启动会重新注册自己
的 layer，所以"谁赢"会随时间变化（这也解释了为何 Vulkan 模式曾成功）。

**处置**（2026-09-28 已自动化）：qrendertest 每次启动时自动修复注册——

- `vk_win32.cpp` `ProcessImplicitLayersKey`：fork 改名 `rendertest` 后，原
  "其它 RenderDoc 安装"检测（按 `rendertest.json` 文件名匹配）漏掉官方
  `renderdoc.json`，已补上——同名 layer 的其它 manifest 一律视为冲突项。
- `qrenderdoc.cpp` main：启动时 `RENDERDOC_NeedVulkanLayerRegistration` 检测
  到冲突即调用 `RENDERDOC_UpdateVulkanLayerRegistration(true)`，删除冲突注册
  并确保 `rendertest.json` 注册启用。**需要 HKLM 写权限（管理员运行
  qrendertest）**；非管理员启动时静默失败，CaptureDialog 警告条仍会提示。

注意：**每次运行官方 RenderDoc/qrenderdoc 后它可能重新注册自己的 layer**，
下次启动 qrendertest 会再次自动清理。验证方式：截帧前确认目标进程模块列表里
没有 `C:\Program Files\RenderDoc\renderdoc.dll`；且 rendertest 日志中 Vulkan
设备注册（`Adding Vulkan device frame capturer`）出现在 `%TEMP%\RenderTest\`
的日志里而非 `%TEMP%\RenderDoc\`。

另注：GL 侧 `WrappedOpenGL` 是每进程单例（WGL/EGL hook 各一），同进程所有 GL
上下文同属一个 capturer，因此 GL 进程的级联 `N=1` 为预期行为，单次捕获即覆盖
进程内全部 GL 上下文。

## 三、已修复：EventBrowser 过滤器空指针崩溃

### 现象

加载级联产出的 `capture_N.rdc`（离屏设备那份）或某些 `frameNNNN.rdc` 时，
qrendertest 必崩，且反复发生。`%LOCALAPPDATA%\CrashDumps` 下有多个
`qrendertest.exe.*.dmp`。

硬证据（Windows 事件日志 + PDB 离线符号化）：

```
出错模块: qrendertest.exe
异常代码: 0xC0000005 (访问违例)
出错偏移: 0x6E250F
→ std::_Func_impl_no_alloc<…, bool, ICaptureContext*, rdcstr const&, rdcstr const&,
                           uint32_t, SDChunk const*, ActionDescription const*,
                           rdcstr const&>::_Do_call+15
```

该签名即 `IEventBrowser::EventFilterCallback`
（`qrenderdoc/Code/Interface/QRDInterface.h`）。

### 根因

调用链：

1. `qrenderdoc/Windows/EventBrowser.cpp` 的 `EventFilterModel::filterAcceptsSingleRow`
   从模型行读 `ROLE_GROUPED_ACTION`：
   ```cpp
   const ActionDescription *action =
       (const ActionDescription *)(sourceModel()->data(source_idx, ROLE_GROUPED_ACTION).toULongLong());
   ```
   某些行该 role 为 0 → `action == NULL`。
2. 传入 `EvaluateFilterSet(... action ...)`（同文件），后者调用
   `filter.callback(&ctx, ..., chunk, action, name)`。
3. 各内建 flag 过滤器 lambda（如
   `return action->eventId == eventId && (action->flags & flags);`）**不检查
   NULL**，直接 `action->flags` → 访问违例。

触发条件缺一不可：
- EventBrowser 里挂着一个过滤器（否则 `EvaluateFilterSet` 在
  `filters.empty()` 早退，不调 callback）；
- 加载的截帧里某些行 `ROLE_GROUPED_ACTION == 0`。多设备桥接的离屏设备
  `capture_N.rdc` 因 action 树不完整正好命中。

### 修复

`EvaluateFilterSet` 入口对 `action == NULL` 短路，按"无过滤器适用 → 可见"
处理（与 root / capture-start 行永远可见的语义一致）：

```cpp
// qrenderdoc/Windows/EventBrowser.cpp, EvaluateFilterSet 开头
if(filters.empty())
  return true;

if(!action)
  return true;   // ← 本次修复
```

这是一个**通用**防护：任何 `ROLE_GROUPED_ACTION == 0` 的行都不再让过滤器
崩溃，与具体游戏 / 模拟器无关。它**只堵崩溃**，不修第 2 节限制 3 里的"离屏
capture action 树残缺"问题——那个是 capture 质量问题，需另行处理
（按提交 / 队列信号量界定帧边界，而非依赖 present）。

### 临时绕法（不想重新构建时）

加载 `capture_N.rdc` 前先清空 EventBrowser 的过滤器：搜索框清空、
Drawcall / Dispatch / Clear / Copy / Present / Marker 等勾选全部取消。
