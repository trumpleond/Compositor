#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

extern "C" {
#include "AdjustPixels.h"
#include "DitherPixels.h"
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"CompositorWindowsWindow";
constexpr wchar_t kWindowTitle[] = L"Compositor — Windows";
constexpr int kToolbarHeight = 54;

enum CommandId : UINT {
    CommandOpen = 1001,
    CommandSave,
    CommandSaveAs,
    CommandExit,
    CommandUndo,
    CommandRedo,
    CommandReset,
    CommandGrayscale,
    CommandInvert,
    CommandExposureUp,
    CommandExposureDown,
    CommandDither,
    CommandBrush,
    CommandEraser,
    CommandFit,
    CommandAbout
};

class CompositorWindow {
public:
    HRESULT initialize(HINSTANCE instance) {
        instance_ = instance;

        WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = &CompositorWindow::windowProc;
        windowClass.lpszClassName = kWindowClass;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        HRESULT result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf());
        if (FAILED(result)) return result;

        result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown **>(dwriteFactory_.GetAddressOf()));
        if (FAILED(result)) return result;
        result = dwriteFactory_->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"zh-CN", &textFormat_);
        if (FAILED(result)) return result;

        result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wicFactory_));
        if (FAILED(result)) {
            result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&wicFactory_));
        }
        if (FAILED(result)) return result;

        hwnd_ = CreateWindowExW(
            0, kWindowClass, kWindowTitle,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
            nullptr, nullptr, instance, this);
        if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());

        createMenu();
        DragAcceptFiles(hwnd_, TRUE);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        return S_OK;
    }

    int run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    enum class Tool { Brush, Eraser };

    void createMenu() {
        HMENU menu = CreateMenu();
        HMENU file = CreatePopupMenu();
        HMENU edit = CreatePopupMenu();
        HMENU image = CreatePopupMenu();
        HMENU view = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, CommandOpen, L"打开图片\tCtrl+O");
        AppendMenuW(file, MF_STRING, CommandSave, L"保存\tCtrl+S");
        AppendMenuW(file, MF_STRING, CommandSaveAs, L"另存为...");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, CommandExit, L"退出");
        AppendMenuW(edit, MF_STRING, CommandUndo, L"撤销\tCtrl+Z");
        AppendMenuW(edit, MF_STRING, CommandRedo, L"重做\tCtrl+Y");
        AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(edit, MF_STRING, CommandBrush, L"画笔\tB");
        AppendMenuW(edit, MF_STRING, CommandEraser, L"橡皮擦\tE");
        AppendMenuW(image, MF_STRING, CommandGrayscale, L"转为灰度");
        AppendMenuW(image, MF_STRING, CommandInvert, L"反相");
        AppendMenuW(image, MF_STRING, CommandExposureUp, L"提高曝光");
        AppendMenuW(image, MF_STRING, CommandExposureDown, L"降低曝光");
        AppendMenuW(image, MF_STRING, CommandDither, L"抖动");
        AppendMenuW(view, MF_STRING, CommandFit, L"适应窗口\tF");
        AppendMenuW(view, MF_STRING, CommandReset, L"重置视图");
        AppendMenuW(view, MF_STRING, CommandAbout, L"关于 Compositor");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"编辑");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(image), L"图像");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"视图");
        SetMenu(hwnd_, menu);
    }

    void handleCommand(UINT command) {
        switch (command) {
        case CommandOpen: openImage(); break;
        case CommandSave: saveCurrent(); break;
        case CommandSaveAs: saveImage(); break;
        case CommandExit: DestroyWindow(hwnd_); break;
        case CommandUndo: undo(); break;
        case CommandRedo: redo(); break;
        case CommandReset: resetView(); InvalidateRect(hwnd_, nullptr, FALSE); break;
        case CommandGrayscale: applyGrayscale(); break;
        case CommandInvert: applyInvert(); break;
        case CommandExposureUp: applyExposure(1.25f); break;
        case CommandExposureDown: applyExposure(0.8f); break;
        case CommandDither: applyDither(); break;
        case CommandBrush: tool_ = Tool::Brush; InvalidateRect(hwnd_, nullptr, FALSE); break;
        case CommandEraser: tool_ = Tool::Eraser; InvalidateRect(hwnd_, nullptr, FALSE); break;
        case CommandFit: resetView(); InvalidateRect(hwnd_, nullptr, FALSE); break;
        case CommandAbout:
            MessageBoxW(hwnd_, L"Compositor Windows\n原生 Win32 + Direct2D 版本", L"关于", MB_OK | MB_ICONINFORMATION);
            break;
        default: break;
        }
    }

    void resetHistory() {
        history_.clear();
        history_.push_back(pixels_);
        historyIndex_ = 0;
    }

    void beginEdit() {
        if (!imageLoaded_) return;
        if (historyIndex_ + 1 < history_.size()) history_.erase(history_.begin() + historyIndex_ + 1, history_.end());
    }

    void commitEdit() {
        if (!imageLoaded_ || history_.empty() || history_.back() == pixels_) return;
        history_.push_back(pixels_);
        historyIndex_ = history_.size() - 1;
        if (history_.size() > 32) {
            history_.erase(history_.begin());
            --historyIndex_;
        }
    }

    void undo() {
        if (historyIndex_ == 0 || history_.empty()) return;
        pixels_ = history_[--historyIndex_];
        updateBitmap();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void redo() {
        if (historyIndex_ + 1 >= history_.size()) return;
        pixels_ = history_[++historyIndex_];
        updateBitmap();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void applyGrayscale() {
        if (!imageLoaded_) return;
        beginEdit();
        for (size_t i = 0; i < pixels_.size(); i += 4) {
            const uint8_t gray = static_cast<uint8_t>((54u * pixels_[i + 2] + 183u * pixels_[i + 1] + 19u * pixels_[i] + 128u) / 256u);
            pixels_[i] = pixels_[i + 1] = pixels_[i + 2] = gray;
        }
        commitEdit();
        updateBitmap();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void applyInvert() {
        if (!imageLoaded_) return;
        beginEdit();
        for (size_t i = 0; i < pixels_.size(); i += 4)
            for (int c = 0; c < 3; ++c) pixels_[i + c] = pixels_[i + 3] - pixels_[i + c];
        commitEdit();
        updateBitmap();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void applyExposure(float factor) {
        if (!imageLoaded_) return;
        beginEdit();
        for (size_t i = 0; i < pixels_.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                const int value = static_cast<int>(std::lround(pixels_[i + c] * factor));
                pixels_[i + c] = static_cast<uint8_t>(std::clamp(value, 0, static_cast<int>(pixels_[i + 3])));
            }
        }
        commitEdit();
        updateBitmap();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void applyDither() {
        if (!imageLoaded_) return;
        beginEdit();
        DitherParams params{};
        params.style = DITHER_BAYER_8;
        params.levels = 4;
        params.contrast = 0.0f;
        params.cell = 6;
        params.originalColors = 1;
        params.dark[0] = params.dark[1] = params.dark[2] = 0;
        params.light[0] = params.light[1] = params.light[2] = 255;
        dither_apply(pixels_.data(), imageWidth_, imageHeight_, imageWidth_ * 4, &params);
        commitEdit();
        updateBitmap();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto *window = reinterpret_cast<CompositorWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
            window = static_cast<CompositorWindow *>(create->lpCreateParams);
            window->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        }
        return window ? window->handleMessage(message, wParam, lParam)
                      : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            createRenderTarget();
            return 0;
        case WM_SIZE:
            if (renderTarget_) {
                renderTarget_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            }
            if (imageLoaded_) resetView();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_KEYDOWN:
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wParam == 'O') {
                openImage();
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wParam == 'S') {
                saveCurrent();
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wParam == 'Z') {
                undo();
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wParam == 'Y') {
                redo();
                return 0;
            }
            if (wParam == 'F') {
                resetView();
                return 0;
            }
            if (wParam == 'B') {
                tool_ = Tool::Brush;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wParam == 'E') {
                tool_ = Tool::Eraser;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_OEM_4 || wParam == VK_OEM_6) {
                brushRadius_ = std::clamp(brushRadius_ + (wParam == VK_OEM_6 ? 2 : -2), 1, 200);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            return 0;
        case WM_COMMAND:
            handleCommand(LOWORD(wParam));
            return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(drop, 0, path, MAX_PATH)) loadImage(path);
            DragFinish(drop);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetCapture(hwnd_);
            lastMouse_ = pointFromLParam(lParam);
            if (imageLoaded_ && lastMouse_.y >= kToolbarHeight) {
                beginEdit();
                paintBrush(lastMouse_);
            }
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            commitEdit();
            return 0;
        case WM_MOUSEMOVE:
            if (wParam & MK_LBUTTON) {
                POINT current = pointFromLParam(lParam);
                if (imageLoaded_) {
                    paintBrush(current);
                }
                lastMouse_ = current;
            }
            return 0;
        case WM_MOUSEWHEEL: {
            POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &cursor);
            zoomAt(cursor, GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1.1f : 1.0f / 1.1f);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void createRenderTarget() {
        if (renderTarget_) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        const auto size = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(1, client.right - client.left)),
            static_cast<UINT32>(std::max<LONG>(1, client.bottom - client.top)));
        d2dFactory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            D2D1::HwndRenderTargetProperties(hwnd_, size), &renderTarget_);
        if (renderTarget_) {
            renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_);
            renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.13f, 0.15f), &toolbarBrush_);
            renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.45f, 0.85f), &accentBrush_);
            renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.62f, 0.65f, 0.70f), &mutedBrush_);
        }
    }

    void paint() {
        PAINTSTRUCT paintStruct{};
        BeginPaint(hwnd_, &paintStruct);
        if (!renderTarget_) {
            EndPaint(hwnd_, &paintStruct);
            return;
        }

        renderTarget_->BeginDraw();
        renderTarget_->Clear(D2D1::ColorF(0.08f, 0.08f, 0.09f, 1.0f));
        if (toolbarBrush_) {
            RECT client{};
            GetClientRect(hwnd_, &client);
            renderTarget_->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(client.right), kToolbarHeight), toolbarBrush_.Get());
            const std::wstring toolbar = imageLoaded_
                ? L"文件  编辑  图像  视图     工具: " + std::wstring(tool_ == Tool::Brush ? L"画笔" : L"橡皮擦")
                : L"文件  编辑  图像  视图     Ctrl+O 打开图片";
            renderTarget_->DrawTextW(toolbar.c_str(), static_cast<UINT32>(toolbar.size()), textFormat_.Get(),
                                     D2D1::RectF(20, 14, 900, 46), brush_.Get());
        }
        if (bitmap_) {
            const auto destination = D2D1::RectF(
                origin_.x, origin_.y,
                origin_.x + imageWidth_ * zoom_,
                origin_.y + imageHeight_ * zoom_);
            renderTarget_->DrawBitmap(bitmap_.Get(), destination, 1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            renderTarget_->DrawTextW(
                L"Ctrl+O 打开图片\n\n拖入图片也可以打开\n左键绘制 · 滚轮缩放 · F 适应窗口\nCtrl+S 保存 PNG",
                64, textFormat_.Get(), D2D1::RectF(48, kToolbarHeight + 32, 760, 280), brush_.Get());
        }
        const HRESULT result = renderTarget_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            bitmap_.Reset();
            renderTarget_.Reset();
            brush_.Reset();
            createRenderTarget();
        }
        EndPaint(hwnd_, &paintStruct);
    }

    void openImage() {
        wchar_t path[MAX_PATH]{};
        OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFilter = L"图片文件\0*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff\0所有文件\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = MAX_PATH;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&dialog)) loadImage(path);
    }

    void saveImage() {
        if (!imageLoaded_) return;
        wchar_t path[MAX_PATH] = L"Compositor-image.png";
        OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFilter = L"PNG 图片\0*.png\0所有文件\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrDefExt = L"png";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&dialog) && writePng(path)) currentPath_ = path;
    }

    void saveCurrent() {
        if (!imageLoaded_) return;
        if (currentPath_.empty()) {
            saveImage();
        } else {
            writePng(currentPath_.c_str());
        }
    }

    bool loadImage(const wchar_t *path) {
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory_->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnLoad, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) ||
            FAILED(wicFactory_->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom))) {
            showError(L"无法打开图片。", L"Compositor");
            return false;
        }

        UINT width = 0, height = 0;
        if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) return false;
        const UINT stride = width * 4;
        pixels_.assign(static_cast<size_t>(stride) * height, 0);
        if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels_.size()), pixels_.data()))) {
            pixels_.clear();
            return false;
        }

        imageWidth_ = width;
        imageHeight_ = height;
        currentPath_ = path;
        imageLoaded_ = true;
        resetHistory();
        resetView();
        updateBitmap();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    bool writePng(const wchar_t *path) {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        if (FAILED(wicFactory_->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
            FAILED(wicFactory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, &properties)) ||
            FAILED(frame->Initialize(properties.Get())) ||
            FAILED(frame->SetSize(imageWidth_, imageHeight_))) {
            showError(L"无法保存图片。", L"Compositor");
            return false;
        }

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
        if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppPBGRA ||
            FAILED(frame->WritePixels(imageHeight_, imageWidth_ * 4,
                                      static_cast<UINT>(pixels_.size()), pixels_.data())) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
            showError(L"无法写入 PNG。", L"Compositor");
            return false;
        }
        return true;
    }

    void updateBitmap() {
        bitmap_.Reset();
        if (!renderTarget_ || pixels_.empty()) return;
        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        renderTarget_->CreateBitmap(D2D1::SizeU(imageWidth_, imageHeight_), pixels_.data(),
                                    imageWidth_ * 4, properties, &bitmap_);
    }

    void resetView() {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const float clientWidth = static_cast<float>(client.right - client.left);
        const float clientHeight = std::max(1.0f, static_cast<float>(client.bottom - client.top - kToolbarHeight));
        const float fit = std::min(clientWidth / std::max(1u, imageWidth_),
                                   clientHeight / std::max(1u, imageHeight_));
        zoom_ = std::clamp(fit, 0.05f, 1.0f);
        origin_ = D2D1::Point2F(
            (clientWidth - imageWidth_ * zoom_) / 2.0f,
            kToolbarHeight + (clientHeight - imageHeight_ * zoom_) / 2.0f);
    }

    void zoomAt(POINT cursor, float factor) {
        if (!imageLoaded_) return;
        const float imageX = (static_cast<float>(cursor.x) - origin_.x) / zoom_;
        const float imageY = (static_cast<float>(cursor.y) - origin_.y) / zoom_;
        zoom_ = std::clamp(zoom_ * factor, 0.05f, 16.0f);
        origin_.x = static_cast<float>(cursor.x) - imageX * zoom_;
        origin_.y = static_cast<float>(cursor.y) - imageY * zoom_;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void paintBrush(POINT point) {
        if (!imageLoaded_ || point.x < origin_.x || point.y < origin_.y ||
            point.x >= origin_.x + imageWidth_ * zoom_ || point.y >= origin_.y + imageHeight_ * zoom_) return;
        const int centerX = static_cast<int>((point.x - origin_.x) / zoom_);
        const int centerY = static_cast<int>((point.y - origin_.y) / zoom_);
        const int radius = std::max(1, static_cast<int>(brushRadius_ / zoom_));
        for (int y = std::max(0, centerY - radius); y <= std::min<int>(imageHeight_ - 1, centerY + radius); ++y) {
            for (int x = std::max(0, centerX - radius); x <= std::min<int>(imageWidth_ - 1, centerX + radius); ++x) {
                const int dx = x - centerX;
                const int dy = y - centerY;
                if (dx * dx + dy * dy > radius * radius) continue;
                auto *pixel = pixels_.data() + static_cast<size_t>(y) * imageWidth_ * 4 + static_cast<size_t>(x) * 4;
                if (tool_ == Tool::Eraser) {
                    pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
                } else {
                    pixel[0] = pixel[1] = pixel[2] = 0;
                    pixel[3] = 255;
                }
            }
        }
        updateBitmap();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static POINT pointFromLParam(LPARAM lParam) {
        return POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    }

    static void showError(const wchar_t *message, const wchar_t *title) {
        MessageBoxW(nullptr, message, title, MB_OK | MB_ICONERROR);
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1SolidColorBrush> toolbarBrush_;
    ComPtr<ID2D1SolidColorBrush> accentBrush_;
    ComPtr<ID2D1SolidColorBrush> mutedBrush_;
    ComPtr<ID2D1Bitmap> bitmap_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IWICImagingFactory> wicFactory_;
    std::vector<uint8_t> pixels_;
    UINT imageWidth_ = 0;
    UINT imageHeight_ = 0;
    bool imageLoaded_ = false;
    std::wstring currentPath_;
    std::vector<std::vector<uint8_t>> history_;
    size_t historyIndex_ = 0;
    Tool tool_ = Tool::Brush;
    float brushRadius_ = 12.0f;
    float zoom_ = 1.0f;
    D2D1_POINT_2F origin_{0, 0};
    POINT lastMouse_{0, 0};
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) return 1;

    CompositorWindow app;
    const HRESULT result = app.initialize(instance);
    const int exitCode = SUCCEEDED(result) ? app.run() : 1;
    CoUninitialize();
    return exitCode;
}
