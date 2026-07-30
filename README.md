# 🖼️ PNG 不可见水印工具

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](https://github.com/GiftsfromtheRiver/PNG-Invisible-Watermark-Tool)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue)](https://isocpp.org/)

**PNG 不可见水印工具** — 基于 ±1 LSB Matching 和 3×3 块标记技术的图片版权保护工具，专为画师和创作者设计。

## ✨ 特性

- 🔒 **完全不可见** — 水印隐藏在像素 LSB 中，肉眼无法察觉
- 🎨 **RGB 三通道** — 避开 Alpha 通道，兼容 QQ/微信等社交平台
- 🛡️ **3×3 块标记** — 抗裁剪、抗缩放的鲁棒性设计
- 🔑 **密钥保护** — 不知道盐值（Salt）无法提取水印
- 📂 **批量处理** — 支持整个文件夹一键嵌入
- 🌐 **跨平台** — 支持 Windows / Linux / macOS
- 📦 **完全本地** — 无需联网，图片不上传任何服务器


## 🚀 快速开始

## 下载

方式一：直接下载 Release
从 [Releases](https://github.com/your-username/png-watermark/releases) 页面下载最新版的 `watermark.exe`。

方式二：从源码编译

克隆仓库
git clone https://github.com/GiftsfromtheRiver/pPNG-Invisible-Watermark-Tool.git
cd png-watermark

Windows (双击 build.bat 或在命令行运行)
build.bat

Linux / macOS
chmod +x build.sh
./build.sh
环境要求
Windows: Dev-C++ (TDM-GCC-64) 或 MinGW-w64

Linux/macOS: g++ 7.0+ 或 clang 5.0+

📖 使用说明
交互模式（推荐）
直接双击运行 watermark.exe，进入交互菜单：


    PNG 不可见水印工具 v1.2          
    ±1 LSB Matching + 3x3 Block      
    支持单文件 / 文件夹批量           

请选择功能:
  1. 嵌入水印 (Embed) [支持文件夹]
  2. 提取水印 (Extract)
  3. 查询容量 (Capacity)
  0. 退出

> 1
按照提示输入路径、密钥和水印文本即可。

命令行模式
适合脚本化批量处理：

嵌入水印
watermark.exe embed input.png output.png 114514 "© 2026 MyArtStudio"

提取水印
watermark.exe extract stego.png 114514

查询容量
watermark.exe capacity input.png
批量处理
在交互模式中，输入文件夹路径即可批量处理所有 PNG 文件：

输入PNG路径 (文件或文件夹): D:/MyArt/
找到 132 个 PNG 文件
种子密钥: 114514
输入水印文本: © 2026 MyArtStudio

--- 批量嵌入中 ---
  [1/132] D:/MyArt/pic1.png ... OK (128 bits)
  [2/132] D:/MyArt/pic2.png ... OK (256 bits)
  ...
### 🔧 技术原理
1. ±1 LSB Matching
传统 LSB 替换会留下直方图阶梯痕迹，容易被隐写分析检测。±1 LSB Matching 在 LSB 不匹配时随机 ±1，保持直方图平滑，使水印更难被检测。

原始值: 100 (0b01100100)
目标 bit: 1
LSB 匹配 → 不修改

原始值: 101 (0b01100101)
目标 bit: 0
LSB 不匹配 → 随机 ±1 → 100 或 102
2. 3×3 块标记
每个 3×3 像素块中：


 标记 标记 标记 
 标记 数据 标记   ← 中心像素：存储 1 bit 数据
 标记 标记 标记   ← 周边 8 像素：存储标记位（用于定位）

即使图片被裁剪或缩放，仍然可以通过标记位找到数据块的精确位置。

3. RGB 三通道策略
只使用 RGB 三通道，不使用 Alpha 通道。原因是：

QQ/微信等社交平台传输 PNG 时可能剥离 Alpha 通道，导致嵌入在 Alpha 中的数据丢失。

牺牲 25% 的容量，换取跨平台传输的鲁棒性。

📊 容量说明
图片尺寸	像素数	可用块数	最大水印字节
512×512	262,144	28,800	~3,200 字节
1024×1024	1,048,576	116,280	~13,000 字节
2048×2048	4,194,304	466,560	~52,000 字节
实际可用容量 = 总块数 - 4 字节（长度头）

📁 项目结构
text
png-watermark/
├── watermark.h              # 水印库头文件
├── watermark.cpp            # 水印库实现
├── watermark_tool.cpp       # CLI 工具主程序
├── lodepng.h                # PNG 编解码库 (zlib license)
├── lodepng.cpp              # PNG 编解码库
├── build.bat                # Windows 编译脚本
├── build.sh                 # Linux/macOS 编译脚本
├── README.md                # 项目说明
└── LICENSE                  # MIT License
🛠️ 编译指南
Windows：
方式一：双击运行
build.bat

方式二：命令行
g++ -std=c++17 -O2 -static -o watermark.exe watermark_tool.cpp watermark.cpp lodepng.cpp

Linux / macOS：
方式一：运行脚本
chmod +x build.sh
./build.sh

方式二：命令行
g++ -std=c++17 -O2 -o watermark watermark_tool.cpp watermark.cpp lodepng.cpp

🤝 贡献指南
欢迎提交 Issue 和 Pull Request！

Fork 本仓库

创建你的特性分支 (git checkout -b feature/amazing-feature)

提交你的改动 (git commit -m 'Add some amazing feature')

推送到分支 (git push origin feature/amazing-feature)

创建 Pull Request

📜 许可证
本项目采用 MIT License，你可以自由使用、修改、分发，甚至用于商业用途。

🙏 致谢
LodePNG — 轻量级 PNG 编解码库 (zlib license)

所有使用和反馈本工具的创作者们

📧 联系方式
GitHub: GiftsfromtheRiver

Email: 2696888172l@qq.com

⭐ Star History
如果这个项目对你有帮助，请给个 Star！⭐

https://api.star-history.com/svg?repos=your-username/png-watermark&type=Date

保护你的创作，从不可见水印开始！ 🎨🔒
