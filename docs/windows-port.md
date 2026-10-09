# Windows 版

这个仓库的当前主线以 Windows 版本为目标。macOS 的 Xcode 工程不再是 Windows 构建入口。

## 当前功能

- 原生 Win32 窗口
- Direct2D GPU 绘制
- WIC 图片解码
- 打开 PNG/JPEG/BMP/TIFF
- 鼠标左键黑色画笔
- 滚轮缩放、F 键适应窗口
- Ctrl+O 打开，Ctrl+S 保存 PNG
- CMake 构建现有 C 图像处理核心

## Windows 构建

安装 Visual Studio 2022 的“使用 C++ 的桌面开发”工作负载后，在 Developer PowerShell 中执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

程序位于 `build/Release/Compositor.exe`。
