# XInsight

面向 macOS 的 Source Insight 风格 C/C++ 代码阅读器 + 轻量编辑器，专注于大型嵌入式 / BSP / 音视频管线 / 裸机 STM32 代码库的**快速阅读、跳转与检索**，而非 IDE 式的编译调试。

完整需求见 [`XInsight_PRD_v4.md`](XInsight_PRD_v4.md)（中文），架构与开发约定见 [`CLAUDE.md`](CLAUDE.md)。

## 核心理念

- **三大支柱**：光标跟随的上下文侧栏、任意方向可嵌套的分屏、极速代码检索。
- **零配置双层代码智能**：tree-sitter 作为始终开启的默认层（容错解析，代码不能编译也能高亮/大纲/跳转），clangd 作为可选的精确层，检测到 `compile_commands.json` 时自动升级结果，永不阻塞、永不作为硬依赖。
- **单向依赖**：GUI（`xinsight-qt`）→ 适配层 → 核心（`xinsight-core`）。核心不依赖 Qt，也是本项目最重要的架构红线。

## 目录结构

```
XInsight/
├── xinsight-core/     # 纯 C++20 业务逻辑，零 Qt 依赖，可独立编译测试
│   ├── include/       # 公开头文件（单一出口 API）
│   ├── src/            # 实现：tree-sitter 引擎、搜索、导航、项目模型、编码、主题
│   ├── query/          # tree-sitter .scm 查询（高亮 / 大纲 / 折叠）
│   └── tests/          # doctest 单元测试
├── xinsight-qt/        # Qt 6 Widgets + QScintilla GUI，只负责渲染与输入
│   └── src/
├── CMakeLists.txt      # 顶层构建：core + qt 两个 target
├── CMakePresets.json   # default / core-only 两套预设
└── XInsight_PRD_v4.md  # 产品需求文档（单一事实来源）
```

## 架构速览

- `CodeIntelligence`、`SearchEngine`、`NavigationEngine`、`LspClient`、`IUiDispatcher` 是核心对外暴露的**单一出口**：GUI 不允许绕过它们直接拼装 LSP JSON、直接起 `rg`/`clangd` 子进程，或直接调用 tree-sitter/clangd。
- 符号索引位于 `ISymbolIndex` 接口之后，v1 只实现 `InMemorySymbolIndex`（SQLite 后端明确推迟）。
- 主题（背景/前景/当前行/选中/行号 + tree-sitter token 配色）通过 `xinsight::core::theme::ThemeManager` 数据驱动加载，内置 Dark / Light / Eye-care 三套，并支持从用户配置目录读取自定义 `*.json` 主题文件。
- 项目级状态存于项目根目录的 `.xinsight/`；用户级配置（当前主题等）存于 `~/Library/Application Support/XInsight/`。

## 依赖

**核心（`xinsight-core`）**，全部通过 CMake `FetchContent` 拉取并静态链接，构建自包含、可复现：

- [tree-sitter](https://github.com/tree-sitter/tree-sitter) + `tree-sitter-c` + `tree-sitter-cpp`
- [nlohmann/json](https://github.com/nlohmann/json)
- [reproc](https://github.com/DaanDeMeyer/reproc)（子进程管理，用于拉起 `rg`/`clangd`）
- [doctest](https://github.com/doctest/doctest)（仅测试）

**GUI（`xinsight-qt`）**：

- Qt 6（Widgets + PrintSupport + Svg）—— 需预先安装官方 Qt 6.8.3（或兼容版本），本仓库不 vendor Qt 本体
- [QScintilla](https://www.riverbankcomputing.com/software/qscintilla/)（vendor 源码 + 自写 CMake target，链接本机已装的 Qt）

**外部子进程工具**（运行时调用，不参与链接）：[ripgrep](https://github.com/BurntSushi/ripgrep)（`rg`，必需）、`clangd`（可选，P1+ 精确层）。

## 构建

```bash
# 完整构建（core + Qt GUI）——需要先把 CMakePresets.json 里的
# CMAKE_PREFIX_PATH 改成你本机的 Qt 6 安装路径
cmake --preset default
cmake --build build/default -j

# 仅构建核心，不链接 Qt（架构解耦验收）
cmake --preset core-only
cmake --build build/core-only -j
```

运行 GUI（目前是普通可执行文件，尚未打包成签名的 `.app`，见下方"当前进度"）：

```bash
./build/default/xinsight-qt/xinsight-qt
```

## 测试

```bash
./build/default/xinsight-core/tests/xinsight-core-tests
# 或用 ctest：
ctest --test-dir build/default
```

`xinsight-core-tests` 为 headless doctest 套件，覆盖 tree-sitter 符号提取、ripgrep JSON 解析、导航栈、编码往返、主题加载等，且**不链接 Qt**——这是验证核心/GUI 解耦是否被破坏的主要手段。

## 当前进度

- [x] **M1**：核心骨架、tree-sitter 高亮/折叠/大纲、ripgrep 文本搜索、项目文件树、基础编辑闭环（保存/新建/另存为/撤销/外部改动检测/多编码读写）、可加载主题结构
- [x] **M2**：符号索引（`InMemorySymbolIndex`）、跳转定义/查引用/工作区符号搜索
- [x] **M3**：`ContextEngine` 光标跟随上下文侧栏（含变量→类型解码、候选排序、下钻）
- [x] **M4**（大部分完成）：`LspClient` + `ClangdProvider` clangd 精确层、`CodeIntelligence` 按 §5.2 路由（自动探测 `compile_commands.json`，找到就升级为 precise、找不到零配置回退不回归）、`ClangdStatusView` 状态面板、F12/Shift+F12/Cmd+T 及 Cmd+点击跳转的 precise/fast 标记。**未完成**：会话恢复、面包屑
- [ ] 打包/签名/公证（PRD 明确推迟到 v1 收尾或 v2）

里程碑定义详见 PRD 第 7 节。

## v1 范围之外

不做自动补全、代码格式化、多光标/列编辑、宏录制、代码片段、符号重命名（clangd 落地前）、SQLite 符号索引后端、Git 集成（除外部改动检测 + 重新加载提示外）。
