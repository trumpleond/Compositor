# Windows port

The Windows build is intentionally kept in this repository. The existing Xcode target remains the macOS build; the Windows entry point is CMake.

## Current status

- `compositor_pixel_core` builds the image-processing C code without Apple frameworks.
- The C core no longer requires Apple Blocks or `dispatch/dispatch.h` in the dither implementation.
- OpenMP is used when the toolchain provides it. Without OpenMP, the same code uses a serial fallback.
- `compositor_pixel_core_smoke` covers the first cross-platform build boundary.

## Build on Windows

From a Developer PowerShell prompt:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This first target is deliberately separate from the GUI. SwiftUI, AppKit and Metal are macOS-only, so the Windows application layer will be added on top of this core rather than trying to make the Xcode target compile under Windows.
