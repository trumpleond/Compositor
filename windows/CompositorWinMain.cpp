#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"CompositorWindowsWindow";
constexpr wchar_t kWindowTitle[] = L"Compositor — Windows";

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

        HRESULT result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
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
                saveImage();
                return 0;
            }
            if (wParam == 'F') {
                resetView();
                return 0;
            }
            return 0;
        case WM_LBUTTONDOWN:
            SetCapture(hwnd_);
            lastMouse_ = pointFromLParam(lParam);
            if (imageLoaded_) paintBrush(lastMouse_);
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
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
        if (bitmap_) {
            const auto destination = D2D1::RectF(
                origin_.x, origin_.y,
                origin_.x + imageWidth_ * zoom_,
                origin_.y + imageHeight_ * zoom_);
            renderTarget_->DrawBitmap(bitmap_.Get(), destination, 1.0f,
                                      D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else {
            renderTarget_->DrawTextW(
                L"Ctrl+O 打开图片\n\n左键绘制 · 滚轮缩放 · F 适应窗口\nCtrl+S 保存 PNG",
                36, textFormat_.Get(), D2D1::RectF(48, 48, 640, 220), brush_.Get());
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
        if (GetSaveFileNameW(&dialog)) writePng(path);
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
        imageLoaded_ = true;
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
        const float clientHeight = static_cast<float>(client.bottom - client.top);
        const float fit = std::min(clientWidth / std::max(1u, imageWidth_),
                                   clientHeight / std::max(1u, imageHeight_));
        zoom_ = std::clamp(fit, 0.05f, 1.0f);
        origin_ = D2D1::Point2F(
            (clientWidth - imageWidth_ * zoom_) / 2.0f,
            (clientHeight - imageHeight_ * zoom_) / 2.0f);
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
        if (!imageLoaded_ || point.x < origin_.x || point.y < origin_.y) return;
        const int centerX = static_cast<int>((point.x - origin_.x) / zoom_);
        const int centerY = static_cast<int>((point.y - origin_.y) / zoom_);
        const int radius = std::max(1, static_cast<int>(12.0f / zoom_));
        for (int y = std::max(0, centerY - radius); y <= std::min<int>(imageHeight_ - 1, centerY + radius); ++y) {
            for (int x = std::max(0, centerX - radius); x <= std::min<int>(imageWidth_ - 1, centerX + radius); ++x) {
                const int dx = x - centerX;
                const int dy = y - centerY;
                if (dx * dx + dy * dy > radius * radius) continue;
                auto *pixel = pixels_.data() + static_cast<size_t>(y) * imageWidth_ * 4 + static_cast<size_t>(x) * 4;
                pixel[0] = pixel[1] = pixel[2] = 0;
                pixel[3] = 255;
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
    ComPtr<ID2D1Bitmap> bitmap_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IWICImagingFactory> wicFactory_;
    std::vector<uint8_t> pixels_;
    UINT imageWidth_ = 0;
    UINT imageHeight_ = 0;
    bool imageLoaded_ = false;
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
