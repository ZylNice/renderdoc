# 鸣潮启动器：开启子进程捕获后管理器无法显示

2026-11-03 在本机鸣潮启动器 `3.0.1.0` 上完成修复和复测。开启 **Capture Child Processes** 后，管理器可以正常显示“进入游戏”，RenderTest 可以连接到 `launcher_main.exe`。本次没有进入游戏，也没有验证游戏内截帧。

## 现象与原因

当前安装需要通过启动器进入游戏。实际观察到的启动链为：

```text
launcher.exe
  └─ 3.0.1.0/launcher_main.exe
       └─ KRWebViewRuntime/msedgewebview2.exe
            └─ WebView2 浏览器、渲染和辅助进程
```

关闭子进程捕获后，启动器界面能显示，但捕获选项不会沿启动器的子进程链继续传递。开启后，旧逻辑会尝试向 WebView2 进程注入，日志出现入口点等待超时、注入失败和反复创建浏览器进程，导致管理器无法正常显示。仅修改 RenderDoc 的程序名和 DLL 名称不能解决这个进程创建兼容问题。

另外，扫描模块导入表时，旧实现用 `LoadLibraryW(modpath)` 增加引用。在这次启动器进程中，它可能返回另一个已加载映像，造成 `refcountModHandle == module` 断言反复失败。

## 源码修改

- [`sys_win32_hooks.cpp`](../../renderdoc/os/win32/sys_win32_hooks.cpp)：将 `msedgewebview2.exe` 加入已有的辅助进程排除表，并记录跳过日志。启动器本身仍按原有规则注入。
- 同一文件的 `Hooked_CreateProcess`：仅对需要注入的子进程添加 `CREATE_SUSPENDED`；被排除的进程保留调用方原有的创建标志。调用方本来要求挂起时也保留该状态。
- [`win32_hook.cpp`](../../renderdoc/os/win32/win32_hook.cpp)：使用 `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)` 引用已经加载的那个模块；处理完成后仍通过原有 `FreeLibrary` 释放引用。引用失败时跳过该次模块扫描。

参考了[异环说明](../nte-capture/README.md)中启动器转发和浏览器辅助进程排除的经验。[终末地说明](../endfield-capture/README.md)中直接启动游戏、关闭子进程捕获的设置适用于它记录的启动方式；当前鸣潮需要继续保留启动器的子进程传播。

WebView2 排除规则对使用这份 DLL 的其他程序也生效。模块引用方式修改适用于 Windows 导入表扫描，除当前启动器外尚未做更广泛的应用兼容性回归。

## 编译

修复基于 `v1.x` 分支提交 `1e65a8c0000ff70b458fd4c76b65466f17806e20`。

本次增量编译 `Development` 的 x64 和 Win32 DLL，复用已有依赖库。执行前需关闭占用这些 DLL 的 RenderTest 和测试启动器进程。以下命令在已完成依赖构建的仓库根目录执行，并按实际安装位置调整 MSBuild 路径：

```powershell
$repo = (Get-Location).Path
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
foreach ($platform in @('x64', 'Win32')) {
    & $msbuild "$repo\renderdoc\renderdoc.vcxproj" /t:Build `
        /p:Configuration=Development "/p:Platform=$platform" `
        "/p:SolutionDir=$repo\" /p:BuildProjectReferences=false `
        /m:4 /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $platform" }
}
```

输出为 `x64/Development/rendertest.dll` 和 `Win32/Development/rendertest.dll`。使用 `x64/Development/qrendertest.exe` 启动 GUI；本次没有修改 GUI 源码，它动态加载更新后的 DLL。

## 启动与复测

1. 以管理员身份打开本仓库 `x64/Development/qrendertest.exe`。启动器与调试器的权限需匹配。
2. 在 Launch Application 中选择鸣潮安装目录下的 `launcher.exe`，工作目录设为它所在的安装目录，命令行留空。
3. 保持 **Capture Child Processes** 勾选，然后 Launch。
4. 等待管理器显示完整界面及“进入游戏”按钮。检查日志中 `launcher_main.exe` 成功加载本仓库的 `rendertest.dll`，并出现 `skipping capture for utility child ... msedgewebview2.exe`。
5. WebView2 对应的创建调用应为 `inject=0`。本次日志为 `flags=0x00000400, resume=0, inject=0`，表示没有额外添加挂起标志。其他创建调用的原始标志可以不同。

可通过 Save Settings 将上述设置保存为 `.cap` 文件。勾选 Auto Start 后，用 RenderTest 打开该配置会直接启动管理器；仅需加载设置时保持 Auto Start 关闭。

本次还用另一个 GUI 连接了启动器的目标控制端口：

```powershell
& '.\x64\Development\qrendertest.exe' `
    --targetcontrol localhost:48921
```

`48921` 只是本次运行分配的端口，下次需使用对应进程实际的目标控制端口。连接日志应出现 `Got remote handshake: launcher_main [进程号]`。外层 `launcher.exe` 会转交给 `launcher_main.exe`，因此只观察外层进程不能判断管理器连接状态。

### 启动器没有 FPS/F12 叠加文字时如何判断

叠加文字由被捕获的图形 API 在呈现画面时绘制，DLL 加载本身不会自动在任意窗口上绘字。本次排除了负责绘制启动器界面的 WebView2，因此启动器界面没有叠加文字是预期行为。`launcher_main.exe` 的实时捕获面板可以显示 `Connection Status: Established`、`API: None`：前者说明控制连接可用，后者说明尚未报告捕获到的图形 API。

启动器阶段应核实 `launcher_main.exe` 加载本仓库 DLL、目标控制握手成功、子进程捕获保持开启。进入游戏后，还需核实游戏进程本身加载 DLL、图形 API 为 Active，并实际生成可回放的 RDC；启动器握手不能代替这些验证。若实际游戏由被排除的浏览器或其他未注入的进程创建，传播链也可能中断，应以届时的进程树与日志判断。

后续复查还发现一次权限不匹配：普通权限的 RenderTest 成功注入了外层 `launcher.exe`，但新的 `launcher_main.exe` 为管理员权限，该主进程没有注入记录或控制端口。通过 Windows 进程令牌查询确认权限后，以管理员权限重新运行，主进程已加载修复 DLL，并与控制 GUI 完成握手。

日常操作应右键 `qrendertest.exe`，选择“以管理员身份运行”，再启动鸣潮启动器。原有 GUI 的管理员标题判断使用注册表写权限探测，本机未可靠显示管理员标识；上述复查采用系统进程令牌判断。

## 验证记录与范围

两种架构编译成功；x64 管理器启动实测成功。二轮复测保存了以下日志记录计数：

| 日志记录 | 原失败会话 | 修复后复测会话 |
| --- | ---: | ---: |
| 子进程注入返回非零结果 | 828 | 0 |
| 模块句柄不一致断言 | 446 | 0 |
| 加载到 WebView2 的记录 | 20 | 0 |
| 明确排除 WebView2 的记录 | 0 | 1 |

这些数字是日志记录数，不是独立进程数量；两次会话持续时间不同，不能当作性能对比。

本次确认的范围是“开启子进程捕获时管理器正常显示，并且启动器的目标控制连接可用”。游戏本体注入、游戏内覆盖提示、F12 截帧及 RDC 回放仍需独立验证。
