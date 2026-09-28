# 3dmaster-quicklook 🚀

> **高颜值、高性能的 Windows QuickLook 工业级 3D/CAD 模型空格预览插件**  
> 一键空格即可秒级预览 STEP、IGES、glTF、3MF、STL、OBJ、PLY 以及西门子 UG/NX 原生零件！

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![QuickLook Plugin](https://img.shields.io/badge/QuickLook-Plugin-blue.svg)](https://github.com/QL-Win/QuickLook)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20(x64)-brightgreen.svg)]()

---

## 🌟 核心特性 (Key Features)

- ⚡ **空格秒级极速预览**：基于 Win32 原生嵌入（HwndHost）与独立守护进程架构，与 QuickLook 宿主完美融合，秒开模型不卡顿。
- 📦 **100% 绿色自包含（Self-Contained）**：完整打包官方 VC++ 运行库闭包、TBB 剖分库、Direct3D/软件 OpenGL 回退驱动与 Qt 环境，**换到任何全新的纯净 Windows 电脑上解压即用**，绝不白屏。
- 🛠️ **全格式工业支持**：
  - **STEP / STP** (`.step`, `.stp`)：完整装配树结构、材质固有色渲染与拓扑实体解析。
  - **IGES / IGS** (`.iges`, `.igs`)：工业曲线曲面高保真缝合与几何特征呈现。
  - **现代 3D 网格**：**glTF / GLB** (`.gltf`, `.glb`)、**3MF** (`.3mf`，支持多色与原型实例化)、**STL** (`.stl`)、**OBJ** (`.obj`)、**PLY** (`.ply`)、**OFF** (`.off`)。
  - **UG/NX PRT** (`.prt`)：内置智能二进制嗅探。若本机装有 UG/NX，自动静默后台转码并启用 SHA-256 磁盘二级缓存（再次打开秒级加载）；若未安装则提供专业 CAD 导出导向卡片。
- 📐 **专业 CAD 交互视口**：
  - 黑色 CAD 特征棱线（CAD Edges）清晰勾勒。
  - 动态三向截面剖切（Dynamic Section View），支持滑块平滑控制内部构造。
  - 标准六向工程视图（前/后/左/右/俯/仰）一键切换与窗口自适应居中（Ctrl+F）。
  - 右侧多层级零件装配树交互，支持单个零件独立显隐控制。
  - 正交/透视投影无缝切换。

---

## 📥 安装指南 (Installation)

### 方式一：一键自动安装（推荐）
1. 下载 Release 页面发布的 **`QuickLook.Plugin.ThreeDMaster.qlplugin`**。
2. 确保 QuickLook 正在运行，**双击**下载的 `.qlplugin` 文件。
3. QuickLook 将弹出安装提示，点击确认后重启 QuickLook 即可生效。

### 方式二：便携免安装（适用于离线或局域网机器）
1. 下载 **`QuickLook.Plugin.ThreeDMaster_Portable.zip`**。
2. 将压缩包解压后的文件夹放入 QuickLook 插件目录：
   - 微软商店版路径：`%LOCALAPPDATA%\Packages\21090Pooi.QuickLook_*\LocalCache\Programs\QuickLook\QuickLook.Plugin.ThreeDMaster`
   - 经典便携版路径：`QuickLook安装目录\QuickLook.Plugin.ThreeDMaster`
3. 重启 QuickLook 即可直接享受 3D 模型空格极速预览！

---

## 💻 编译构建 (Build from Source)

### 环境依赖
- Windows 10 / 11 (x64)
- Visual Studio 2022 (MSVC v143, 支持 C++17)
- .NET 8.0 SDK (编译 C# 宿主插件)
- Qt 6.8.2 (MSVC 2022 64-bit)
- OpenCASCADE Technology (OCCT) 8.0.0

### 一键构建与打包
```powershell
# 1. 编译 3dmaster-preview 本地独立守护进程
cd 3dmaster-preview
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 2. 一键打包全闭包 .qlplugin 与便携 zip
cd ..\QuickLook.Plugin.3DMaster
.\pack-plugin.ps1
```
打包脚本将自动提取全套运行时闭包并生成 `QuickLook.Plugin.ThreeDMaster.qlplugin` 与 `QuickLook.Plugin.ThreeDMaster_Portable.zip`。

---

## 🤝 姐妹项目 (Sister Projects)

- 🔍 **[3dmaster-seer](https://github.com/luxp1990/3dmaster-seer)**：面向 Windows **Seer** 用户的同核高保真 3D/CAD 预览插件。

---

## 📄 开源许可证 (License)

本项目采用 [MIT License](LICENSE) 许可证开源。
