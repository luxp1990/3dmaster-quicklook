# 3dmaster-quicklook 🚀

> **面向 Windows QuickLook 的高性能 3D / CAD 模型空格极速预览插件（基于 OpenCASCADE 内核）**  
> **High-performance 3D / CAD preview plugin for Windows QuickLook, powered by OpenCASCADE (OCCT)**

[🇨🇳 中文介绍](#-为什么做这个项目) | [🇺🇸 English](#-english)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![QuickLook Plugin](https://img.shields.io/badge/QuickLook-Plugin-blue.svg)](https://github.com/QL-Win/QuickLook)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20(x64)-brightgreen.svg)]()
[![Language](https://img.shields.io/badge/UI-中文%20%7C%20English-informational.svg)]()

---

## 💡 为什么做这个项目？

我是一名**结构设计工程师**。在日常使用 **QuickLook** 搭配原有 3D 预览插件查看结构件和装配体时，经常遇到几个痛点：
- 预览稍大一点的 CAD 模型就**软件卡死**或**加载极慢**；
- 看不到内部构造（**没有截面剖切图**）；
- 装配件不能**隐藏单个零件**；
- 模型**丢失原始颜色**，一片白模看不清配合关系。

为了解决这些实际工程痛点，我基于 **OpenCASCADE (OCCT 8.0.0)** 几何内核开发了 **3dmaster-quicklook**，希望能帮到遇到同样问题的工程师和开发者。

- **即装即用**：直接按照 QuickLook 标准插件方式（双击 `.qlplugin` 或解压便携包）即可安装使用。
- **中英文支持**：界面支持 **中文 / English**，自动跟随 QuickLook / Windows 系统语言。
- **欢迎交流**：如果你在使用中有任何建议、Bug 反馈或功能想法，欢迎在 [Issues](https://github.com/luxp1990/3dmaster-quicklook/issues) 或 [Discussions](https://github.com/luxp1990/3dmaster-quicklook/discussions) 中一起讨论！

---

## 🌟 核心特性

- ⚡ **空格极速预览不卡死**：采用独立守护进程（`3dmaster-preview`）+ Win32 原生嵌入（`HwndHost`）+ 后台子线程异步解析，大装配体离散计算不阻塞 QuickLook 宿主进程。
- 🛠️ **全格式工业支持**：
  - **STEP / STP** (`.step`, `.stp`)：完整装配体结构树、材质固有色渲染与拓扑实体解析。
  - **IGES / IGS** (`.iges`, `.igs`)：工业曲线曲面高保真缝合与几何特征呈现。
  - **主流 3D 网格**：**glTF / GLB** (`.gltf`, `.glb`)、**3MF** (`.3mf`，支持多色与原型实例化)、**STL** (`.stl`)、**OBJ** (`.obj`)、**PLY** (`.ply`)、**OFF** (`.off`)。
  - **西门子 UG/NX PRT** (`.prt`)：自动嗅探本机已安装的 UG/NX 版本进行静默转码与缓存；未安装 UG 或遇到不兼容高版本时，0.05 秒内安全降级提示，QuickLook 宿主绝不崩溃。
- 📐 **结构工程级交互视口**：
  - **动态截面剖切 (Section View)**：支持 X / Y / Z 三向平滑拖动剖切，快速查看内部装配关系。
  - **装配体零件树显隐**：支持单个零件独立勾选显隐、右键隔离显示、全显/全隐/反选。
  - **CAD 特征棱线与色彩**：支持材质固有色、顶点色、白模、法线着色切换，清晰勾勒 CAD 特征棱线。
  - **操作手感预设**：支持 **西门子 UG/NX**、**SolidWorks**、**通用模式** 一键切换，附带六向标准视图与正交/透视切换。
- 📦 **100% 绿色自包含**：完整内置 VC++ 2015-2022、TBB、Qt6 与 OCCT 8.0 运行时，复制到纯净 Windows 10/11 电脑即可使用。

---

## 📥 安装指南

### 方式一：一键自动安装（推荐）
1. 从 [Release 页面](https://github.com/luxp1990/3dmaster-quicklook/releases) 下载 **`QuickLook.Plugin.ThreeDMaster.qlplugin`**。
2. 确保 QuickLook 正在运行，**双击**该 `.qlplugin` 文件（或在资源管理器中选中它按**空格键**）。
3. 在弹出的安装提示中点击确认，**重启 QuickLook** 即可生效。

### 方式二：便携免安装（离线 / 商店版手动目录）
1. 下载 **`QuickLook.Plugin.ThreeDMaster_Portable.zip`** 并解压。
2. 将解压后的文件夹放入 QuickLook 插件目录：
   - **微软商店版**：`%LOCALAPPDATA%\Packages\21090Pooi.QuickLook_*\LocalCache\Programs\QuickLook\QuickLook.Plugin.ThreeDMaster`
   - **便携版 / 经典安装版**：`QuickLook安装目录\QuickLook.Plugin.ThreeDMaster`
3. 重启 QuickLook 即可使用。

---

## ❓ 常见问题与排错

- **预览超复杂工业装配体时首次加载较久？**
  首次加载大型 STEP/IGES 时，OCCT 内核需要对 B-Rep 曲面进行三角化离散，插件全程在后台子线程异步计算（支持按 `Esc` 中止），QuickLook 主界面不会卡死。
- **提示缺少 `MSVCP140.dll` 或 `VCRUNTIME140.dll`？**
  发行版已内置运行库；如在精简版系统仍报错，可安装微软官方 [Visual C++ 2015-2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)。

---

## 💻 编译构建

- **环境依赖**：Windows 10 / 11 (x64)、Visual Studio 2022 (MSVC v143, C++17)、.NET 8.0 SDK、Qt 6.8.2 (MSVC 2022 64-bit)、OpenCASCADE Technology (OCCT) 8.0.0。
```powershell
# 1. 编译 3dmaster-preview 本地独立守护进程
cd 3dmaster-preview
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 2. 一键打包 .qlplugin 与便携 zip
cd ..\QuickLook.Plugin.3DMaster
.\pack-plugin.ps1
```

---

## 🤝 姐妹项目

- 🔍 **[3dmaster-seer](https://github.com/luxp1990/3dmaster-seer)**：面向 Windows **Seer** 用户的同核 3D/CAD 预览插件。

---

<a id="-english"></a>

## 🇺🇸 English

### Why I Built This

I am a **mechanical / structural design engineer**. When using **QuickLook** with existing 3D preview plugins to inspect CAD parts and assemblies in daily work, I kept running into frustrating issues:
- **Freezing / crashing** and **slow loading** on medium-to-large CAD models;
- **No section view** to inspect internal structures;
- **No way to hide/isolate individual parts** in an assembly;
- **Missing model colors** (everything rendered as plain white meshes).

That motivated me to build **3dmaster-quicklook**, powered by the industrial-grade **OpenCASCADE (OCCT 8.0.0)** geometry kernel, hoping it helps others facing the same problems.

- **Standard Plugin Installation**: Install it directly via QuickLook's standard `.qlplugin` installer or portable `.zip` — all runtime dependencies are self-contained.
- **Bilingual UI (English & Chinese)**: Automatically follows QuickLook's / Windows' UI language.
- **Feedback Welcome**: Feel free to share suggestions, bug reports, or ideas in [Issues](https://github.com/luxp1990/3dmaster-quicklook/issues) or [Discussions](https://github.com/luxp1990/3dmaster-quicklook/discussions)!

### Key Features

- ⚡ **Non-Blocking Native Preview**: Runs an isolated `3dmaster-preview` daemon embedded via Win32 `HwndHost` with asynchronous background parsing so QuickLook never freezes.
- 🛠️ **Wide Format Support**:
  - **STEP / STP** (`.step`, `.stp`): Full assembly tree, native material/face colors, and B-Rep solid parsing.
  - **IGES / IGS** (`.iges`, `.igs`): High-fidelity curve/surface sewing (`ShapeFix`) and geometry preview.
  - **3D Meshes**: **glTF / GLB** (`.gltf`, `.glb`), **3MF** (`.3mf`, with multi-color & instancing), **STL** (`.stl`), **OBJ** (`.obj`), **PLY** (`.ply`), **OFF** (`.off`).
  - **Siemens NX / UG PRT** (`.prt`): Automatically detects local NX installations for silent conversion; degrades gracefully in < 0.05s without crashing if NX is not installed.
- 📐 **Engineering Viewport Controls**:
  - **Dynamic Section View**: Smooth X / Y / Z clipping planes to inspect internal structures.
  - **Assembly Tree**: Toggle individual part visibility, right-click to isolate, show all, hide all, or invert selection.
  - **CAD Edges & Shading**: Material color, vertex color, pure white, and normals modes, plus crisp CAD feature edges.
  - **Navigation Presets**: Switch between **Siemens NX (UG)**, **SolidWorks**, and **Generic** mouse controls, plus 6 standard orthographic views and perspective/orthographic toggle.

### Installation

#### Option 1: One-Click Install (Recommended)
1. Download `QuickLook.Plugin.ThreeDMaster.qlplugin` from the [Releases](https://github.com/luxp1990/3dmaster-quicklook/releases) page.
2. With QuickLook running, **double-click** the `.qlplugin` file (or select it and press **Spacebar**).
3. Confirm the installation prompt and **restart QuickLook**.

#### Option 2: Portable / Manual Install
1. Download and extract `QuickLook.Plugin.ThreeDMaster_Portable.zip`.
2. Place the extracted folder into your QuickLook plugins directory:
   - **Microsoft Store**: `%LOCALAPPDATA%\Packages\21090Pooi.QuickLook_*\LocalCache\Programs\QuickLook\QuickLook.Plugin.ThreeDMaster`
   - **Portable / Classic**: `<QuickLook>\QuickLook.Plugin.ThreeDMaster`
3. Restart QuickLook.

### Sister Project

- 🔍 **[3dmaster-seer](https://github.com/luxp1990/3dmaster-seer)** — The companion 3D/CAD preview plugin for Windows **Seer**.

### License

Open-sourced under the [MIT License](LICENSE).
