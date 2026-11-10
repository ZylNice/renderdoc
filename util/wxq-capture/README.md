# PC 万象棋截帧

当前已验证 PC 正式客户端通过官方启动器启动后的 D3D11 截帧与回放。

## 使用方式

1. 退出已有的万象棋官方启动器，避免新实例把启动请求转交给未被捕获的旧实例。
2. 在 RenderTest 的启动配置中选择官方启动器 `王者万象棋.exe`，工作目录设为其所在目录。
3. 启用 **Capture Child Processes / 捕获子进程**，再从 RenderTest 启动官方启动器。
4. 在官方启动器中点击“启动游戏”。进入游戏后，连接子进程列表中的 `WXQ.exe`，然后触发截帧。

启动器路径示例为 `WXQLauncher/<启动器版本>/王者万象棋.exe`。
直接启动 `WXQLauncher/WXQ/WXQ.exe` 会触发客户端“需要从启动器启动游戏”的校验。
启动器本身、Electron 渲染进程和 Node 服务的连接不代表已经连接到游戏本体。

## 修复内容

- DXGI 工厂 IAT hook 使用对应入口的原函数 trampoline，并在返回前包装工厂。
  原逻辑仅凭全局 inline hook 安装标记跳过包装，可能向应用返回真实工厂。
  真实工厂接收到包装的 D3D11 设备后，可能在交换链创建期间崩溃。
- 为 glslang 的 `ShaderLang.cpp` 启用 `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`，
  同步设置 Visual Studio 和 MSVC CMake 构建。
  新版 MSVC 的 constexpr 互斥量初始化与启动器自带的旧版 `MSVCP140.dll` 不兼容，
  导致捕获 DLL 初始化失败，返回 `ERROR_DLL_INIT_FAILED (1114)`，中断 Node 服务到游戏的捕获链。

## 验证记录

2026-11-10，Release x64 构建验证通过：

- 独立进程使用 VC 14.36 运行库加载捕获 DLL：修复前返回 1114，修复后加载成功。
- 独立 D3D11 程序的显式工厂、设备父工厂、`CreateDXGIFactory` 和
  `CreateDXGIFactory1` 路径可创建交换链、呈现并生成 RDC。
- 官方启动器的 Node 服务和 `WXQ.exe` 均成功加载捕获 DLL。
- 从 PC 正式客户端大厅截取第 7817 帧，RDC 大小 68,807,423 字节。
  回放成功，包含 342 次绘制、173 张纹理和 4018 个 API 事件，并成功导出大厅画面。
