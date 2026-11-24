#include "ui_window.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

// 兼容不同版本 Windows SDK 头文件的 DWM 圆角枚举值
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace todofloat {

namespace {

constexpr wchar_t kWindowClassName[] = L"TodoFloatNativeWindowClass";
constexpr UINT WM_APP_TODO_CHANGED = WM_APP + 10;
constexpr UINT WM_APP_TRAY_EVENT = WM_APP + 11;

constexpr UINT_PTR TIMER_PEEK_HIDE = 1;
constexpr UINT_PTR TIMER_TOAST_CLEAR = 2;
constexpr UINT_PTR TIMER_DAY_CHECK = 3;

constexpr int HOTKEY_ID_CLICKTHROUGH = 0x5446; // 'TF'

constexpr int CMD_TOGGLE_TOPMOST = 1001;
constexpr int CMD_TOGGLE_CLICKTHROUGH = 1002;
constexpr int CMD_RESET_POS = 1003;
constexpr int CMD_COPY_AI = 1004;
constexpr int CMD_OPEN_LOGS = 1005;
constexpr int CMD_OPEN_TODO = 1006;
constexpr int CMD_EXIT = 1007;

constexpr DWORD kDwmCornerRound = 2;      // DWMWCP_ROUND
constexpr DWORD kDwmCornerRoundSmall = 3; // DWMWCP_ROUNDSMALL

template <typename T>
void safeRelease(T*& ptr) noexcept {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

std::wstring utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
    return wide;
}

std::string wideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), len, nullptr, nullptr);
    return utf8;
}

D2D1_COLOR_F hexColor(std::uint32_t rgb, float alpha = 1.0f) noexcept {
    float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    float g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    float b = static_cast<float>(rgb & 0xFF) / 255.0f;
    return D2D1::ColorF(r, g, b, alpha);
}

bool pointInRect(D2D1_POINT_2F pt, const D2D1_RECT_F& r) noexcept {
    return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
}

unsigned int queryWindowDpi(HWND hwnd) noexcept {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d > 0) {
            return d;
        }
    }
    HDC hdc = GetDC(nullptr);
    unsigned int dpi = 96;
    if (hdc) {
        int logPixels = GetDeviceCaps(hdc, LOGPIXELSX);
        if (logPixels > 0) {
            dpi = static_cast<unsigned int>(logPixels);
        }
        ReleaseDC(nullptr, hdc);
    }
    return dpi;
}

} // namespace

FloatingTodoWindow::FloatingTodoWindow(TodoStorage& storage)
    : storage_(storage),
      doc_(storage.loadOrInit(currentLocalDateString())) {}

FloatingTodoWindow::~FloatingTodoWindow() {
    storage_.stopWatching();
    removeTrayIcon();
    if (hotkeyRegistered_ && hwnd_) {
        UnregisterHotKey(hwnd_, HOTKEY_ID_CLICKTHROUGH);
        hotkeyRegistered_ = false;
    }
    discardRenderTarget();
    safeRelease(titleFormat_);
    safeRelease(badgeFormat_);
    safeRelease(itemFormat_);
    safeRelease(hintFormat_);
    safeRelease(dwriteFactory_);
    safeRelease(d2dFactory_);

    if (editFont_) {
        DeleteObject(editFont_);
        editFont_ = nullptr;
    }
    if (editBgBrush_) {
        DeleteObject(editBgBrush_);
        editBgBrush_ = nullptr;
    }
    if (iconNormal_) {
        DestroyIcon(iconNormal_);
        iconNormal_ = nullptr;
    }
    if (iconClickThrough_) {
        DestroyIcon(iconClickThrough_);
        iconClickThrough_ = nullptr;
    }
}

bool FloatingTodoWindow::initGraphicsFactories() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
    if (FAILED(hr) || !d2dFactory_) {
        return false;
    }
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&dwriteFactory_));
    if (FAILED(hr) || !dwriteFactory_) {
        return false;
    }
    return rebuildTextFormats();
}

bool FloatingTodoWindow::rebuildTextFormats() {
    safeRelease(titleFormat_);
    safeRelease(badgeFormat_);
    safeRelease(itemFormat_);
    safeRelease(hintFormat_);

    if (!dwriteFactory_) {
        return false;
    }

    HRESULT hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.0f,
        L"zh-CN",
        &titleFormat_);
    if (FAILED(hr)) {
        return false;
    }
    titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI",
        nullptr,
        DWRITE_FONT_WEIGHT_MEDIUM,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        11.5f,
        L"zh-CN",
        &badgeFormat_);
    if (FAILED(hr)) {
        return false;
    }
    badgeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    badgeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    badgeFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.5f,
        L"zh-CN",
        &itemFormat_);
    if (FAILED(hr)) {
        return false;
    }
    itemFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.5f,
        L"zh-CN",
        &hintFormat_);
    if (FAILED(hr)) {
        return false;
    }
    hintFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hintFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    return true;
}

bool FloatingTodoWindow::create(HINSTANCE hInstance) {
    hInstance_ = hInstance;
    if (!initGraphicsFactories()) {
        return false;
    }

    editBgBrush_ = CreateSolidBrush(RGB(255, 255, 255));
    iconNormal_ = createProceduralAppIcon(false);
    iconClickThrough_ = createProceduralAppIcon(true);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &FloatingTodoWindow::wndProcSetup;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = iconNormal_;
    wc.hIconSm = iconNormal_;
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);

    // 初始计算主显示器右上偏右下的舒适默认位置
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    ScreenRect primaryWork{wa.left, wa.top, wa.right, wa.bottom};
    dpi_ = queryWindowDpi(nullptr);
    state_.setDpi(dpi_, primaryWork);
    int initX = std::max(primaryWork.left + 24, primaryWork.right - state_.metrics().panelWidth - 36);
    int initY = primaryWork.top + 64;
    state_.setPosition(initX, initY, primaryWork);
    ScreenRect bounds = state_.currentBounds(primaryWork);

    // WS_EX_TOOLWINDOW 将悬浮窗彻底排除在 Alt+Tab 窗口切换界面与任务栏之外
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    hwnd_ = CreateWindowExW(
        exStyle,
        kWindowClassName,
        L"今日待办",
        WS_POPUP | WS_CLIPCHILDREN,
        bounds.left,
        bounds.top,
        bounds.width(),
        bounds.height(),
        nullptr,
        nullptr,
        hInstance_,
        this);
    if (!hwnd_) {
        return false;
    }

    dpi_ = queryWindowDpi(hwnd_);
    state_.setDpi(dpi_, currentMonitorWorkArea());

    // 内嵌单行原生 EDIT 子控件，完美支持系统中文输入法选词框与剪贴板快捷键
    editHwnd_ = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | ES_AUTOHSCROLL,
        0,
        0,
        100,
        24,
        hwnd_,
        nullptr,
        hInstance_,
        nullptr);
    if (editHwnd_) {
        SetWindowLongPtrW(editHwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        origEditProc_ = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(editHwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FloatingTodoWindow::editSubclassProc)));
        updateEditFontAndBounds();
    }

    updateWindowCornerPreference();
    applyWindowStylesAndBounds(true);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);

    initTrayIcon();

    // 注册全局快捷键 Alt + T，确保穿透态下也能随时一键切回鼠标交互
    if (RegisterHotKey(hwnd_, HOTKEY_ID_CLICKTHROUGH, MOD_ALT | MOD_NOREPEAT, 'T')) {
        hotkeyRegistered_ = true;
    }

    // 每 30 秒做一次极轻量跨日检测（耗时微秒级，空闲 CPU 占用保持 0%）
    SetTimer(hwnd_, TIMER_DAY_CHECK, 30000, nullptr);

    HWND targetHwnd = hwnd_;
    storage_.startWatching([targetHwnd]() {
        PostMessageW(targetHwnd, WM_APP_TODO_CHANGED, 0, 0);
    });

    return true;
}

void FloatingTodoWindow::updateEditFontAndBounds() {
    if (!editHwnd_) {
        return;
    }
    if (editFont_) {
        DeleteObject(editFont_);
        editFont_ = nullptr;
    }
    editFont_ = CreateFontW(
        -static_cast<int>(std::lround(17.0 * static_cast<double>(dpi_) / 96.0)),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI");
    SendMessageW(editHwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);

    float scale = dpiScale();
    ScreenRect bounds = state_.currentBounds(currentMonitorWorkArea());
    float wDip = static_cast<float>(bounds.width()) / scale;
    float hDip = static_cast<float>(bounds.height()) / scale;

    inputBoxRectDip_ = D2D1::RectF(10.0f, hDip - 42.0f, wDip - 10.0f, hDip - 10.0f);
    int editLeft = static_cast<int>(std::lround((inputBoxRectDip_.left + 10.0f) * scale));
    int editRight = static_cast<int>(std::lround((inputBoxRectDip_.right - 10.0f) * scale));
    int editHeight = static_cast<int>(std::lround(20.0f * scale));
    int boxCenterY = static_cast<int>(std::lround(((inputBoxRectDip_.top + inputBoxRectDip_.bottom) * 0.5f) * scale));
    int editTop = boxCenterY - editHeight / 2;

    SetWindowPos(
        editHwnd_,
        nullptr,
        editLeft,
        editTop,
        std::max(40, editRight - editLeft),
        editHeight,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

ScreenRect FloatingTodoWindow::currentMonitorWorkArea() const noexcept {
    HMONITOR hMon = hwnd_ ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST)
                          : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (hMon && GetMonitorInfoW(hMon, &mi)) {
        return ScreenRect{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom};
    }
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    return ScreenRect{wa.left, wa.top, wa.right, wa.bottom};
}

D2D1_POINT_2F FloatingTodoWindow::clientPxToDip(int xPx, int yPx) const noexcept {
    float s = dpiScale();
    return D2D1::Point2F(static_cast<float>(xPx) / s, static_cast<float>(yPx) / s);
}

void FloatingTodoWindow::updateWindowCornerPreference() {
    if (!hwnd_) {
        return;
    }
    DWORD pref = (state_.mode() == PanelMode::DockedHidden) ? kDwmCornerRoundSmall : kDwmCornerRound;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}

void FloatingTodoWindow::applyWindowStylesAndBounds(bool updateZOrder) {
    if (!hwnd_) {
        return;
    }

    // 处于收缩态或穿透态时收起底部文本编辑子控件
    if (state_.isClickThrough() || state_.mode() == PanelMode::CollapsedVert || state_.mode() == PanelMode::DockedHidden) {
        if (inputActive_) {
            cancelInputEditing();
        }
    }

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW;
    exStyle &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);

    if (state_.isClickThrough()) {
        exStyle |= (WS_EX_LAYERED | WS_EX_TRANSPARENT);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);
        SetLayeredWindowAttributes(hwnd_, 0, 198, LWA_ALPHA); // 约 78% 不透明度，兼顾穿透感知与清晰可读
    } else {
        exStyle &= ~static_cast<LONG_PTR>(WS_EX_LAYERED | WS_EX_TRANSPARENT);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);
    }

    updateWindowCornerPreference();

    ScreenRect wa = currentMonitorWorkArea();
    ScreenRect b = state_.currentBounds(wa);
    HWND zOrder = state_.isTopmost() ? HWND_TOPMOST : HWND_NOTOPMOST;
    UINT flags = SWP_NOACTIVATE | SWP_FRAMECHANGED;
    if (!updateZOrder) {
        flags |= SWP_NOZORDER;
    }

    SetWindowPos(hwnd_, zOrder, b.left, b.top, b.width(), b.height(), flags);
    updateEditFontAndBounds();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool FloatingTodoWindow::ensureRenderTarget() {
    if (renderTarget_) {
        return true;
    }
    if (!d2dFactory_ || !hwnd_) {
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U pixelSize = D2D1::SizeU(
        static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
        static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top)));

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = static_cast<float>(dpi_);
    rtProps.dpiY = static_cast<float>(dpi_);

    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        rtProps,
        D2D1::HwndRenderTargetProperties(hwnd_, pixelSize),
        &renderTarget_);
    if (SUCCEEDED(hr) && renderTarget_) {
        renderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        return true;
    }
    return false;
}

void FloatingTodoWindow::discardRenderTarget() noexcept {
    safeRelease(renderTarget_);
}

void FloatingTodoWindow::onPaint() {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);

    if (ensureRenderTarget()) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        D2D1_SIZE_U pxSize = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
            static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top)));
        renderTarget_->Resize(pxSize);
        renderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));

        renderTarget_->BeginDraw();
        D2D1_SIZE_F sizeDip = renderTarget_->GetSize();
        if (state_.mode() == PanelMode::DockedHidden) {
            drawDockedStrip(sizeDip);
        } else {
            drawMainPanel(sizeDip);
        }
        HRESULT hr = renderTarget_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            discardRenderTarget();
        }
    }

    EndPaint(hwnd_, &ps);
}

void FloatingTodoWindow::drawDockedStrip(const D2D1_SIZE_F& sizeDip) {
    headerBtns_.clear();
    rowVisuals_.clear();

    ID2D1SolidColorBrush* brush = nullptr;
    // 有未完成待办时显示鲜明 Windows Fluent 蓝，全部完成时显示柔和翡翠绿
    std::uint32_t stripHex = (doc_.pendingCount() > 0 || doc_.totalCount() == 0) ? 0x0067C0 : 0x10B981;
    renderTarget_->Clear(hexColor(stripHex));
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(hexColor(0xFFFFFF, 0.35f), &brush))) {
        D2D1_ROUNDED_RECT innerLine = D2D1::RoundedRect(
            D2D1::RectF(1.5f, 18.0f, std::max(2.5f, sizeDip.width - 1.5f), std::max(24.0f, sizeDip.height - 18.0f)),
            1.0f,
            1.0f);
        renderTarget_->FillRoundedRectangle(innerLine, brush);
        brush->Release();
    }
}

void FloatingTodoWindow::drawHeaderIcon(HeaderBtnId id, const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;

    switch (id) {
        case HeaderBtnId::CopyAi: {
            if (badgeFormat_) {
                renderTarget_->DrawTextW(L"AI", 2, badgeFormat_, r, brush);
            }
            break;
        }
        case HeaderBtnId::Topmost: {
            // 图钉矢量图标
            renderTarget_->DrawLine(D2D1::Point2F(cx - 4.0f, cy - 5.0f), D2D1::Point2F(cx + 4.0f, cy - 5.0f), brush, 1.5f);
            renderTarget_->DrawRectangle(D2D1::RectF(cx - 2.8f, cy - 5.0f, cx + 2.8f, cy + 0.5f), brush, 1.4f);
            renderTarget_->DrawLine(D2D1::Point2F(cx - 5.0f, cy + 0.8f), D2D1::Point2F(cx + 5.0f, cy + 0.8f), brush, 1.5f);
            renderTarget_->DrawLine(D2D1::Point2F(cx, cy + 1.0f), D2D1::Point2F(cx, cy + 6.0f), brush, 1.5f);
            break;
        }
        case HeaderBtnId::ClickThrough: {
            // 鼠标穿透双层菱形/虚框光标图标
            D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(cx, cy), 5.5f, 5.5f);
            renderTarget_->DrawEllipse(outer, brush, 1.4f);
            renderTarget_->DrawLine(D2D1::Point2F(cx - 4.0f, cy + 4.0f), D2D1::Point2F(cx + 4.0f, cy - 4.0f), brush, 1.4f);
            break;
        }
        case HeaderBtnId::DockLeft: {
            // 贴左双折角图标 «
            renderTarget_->DrawLine(D2D1::Point2F(cx + 0.5f, cy - 4.5f), D2D1::Point2F(cx - 4.0f, cy), brush, 1.5f);
            renderTarget_->DrawLine(D2D1::Point2F(cx - 4.0f, cy), D2D1::Point2F(cx + 0.5f, cy + 4.5f), brush, 1.5f);
            renderTarget_->DrawLine(D2D1::Point2F(cx + 4.5f, cy - 4.5f), D2D1::Point2F(cx, cy), brush, 1.5f);
            renderTarget_->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + 4.5f, cy + 4.5f), brush, 1.5f);
            break;
        }
        case HeaderBtnId::CollapseVert: {
            if (state_.mode() == PanelMode::CollapsedVert) {
                // 向下展开箭头
                renderTarget_->DrawLine(D2D1::Point2F(cx - 4.5f, cy - 2.0f), D2D1::Point2F(cx, cy + 2.5f), brush, 1.6f);
                renderTarget_->DrawLine(D2D1::Point2F(cx, cy + 2.5f), D2D1::Point2F(cx + 4.5f, cy - 2.0f), brush, 1.6f);
            } else {
                // 上下收起横线
                renderTarget_->DrawLine(D2D1::Point2F(cx - 5.0f, cy), D2D1::Point2F(cx + 5.0f, cy), brush, 1.7f);
            }
            break;
        }
        case HeaderBtnId::None:
            break;
    }
}

void FloatingTodoWindow::drawMainPanel(const D2D1_SIZE_F& sizeDip) {
    renderTarget_->Clear(hexColor(0xF8FAFC));

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(renderTarget_->CreateSolidColorBrush(hexColor(0x0F172A), &brush))) {
        return;
    }

    const float headerH = 36.0f;
    const bool collapsed = (state_.mode() == PanelMode::CollapsedVert);

    // 标题栏底色
    brush->SetColor(hexColor(state_.isClickThrough() ? 0xEFF6FF : 0xF1F5F9));
    renderTarget_->FillRectangle(D2D1::RectF(0.0f, 0.0f, sizeDip.width, headerH), brush);

    if (!collapsed) {
        brush->SetColor(hexColor(0xE2E8F0));
        renderTarget_->DrawLine(D2D1::Point2F(0.0f, headerH), D2D1::Point2F(sizeDip.width, headerH), brush, 1.0f);
    }

    // 计算标题栏右侧按钮布局
    headerBtns_.clear();
    if (!state_.isClickThrough()) {
        const HeaderBtnId ids[5] = {
            HeaderBtnId::CopyAi,
            HeaderBtnId::Topmost,
            HeaderBtnId::ClickThrough,
            HeaderBtnId::DockLeft,
            HeaderBtnId::CollapseVert,
        };
        float rightEdge = sizeDip.width - 6.0f;
        for (int i = 4; i >= 0; --i) {
            float btnW = (ids[i] == HeaderBtnId::CopyAi) ? 26.0f : 24.0f;
            D2D1_RECT_F r = D2D1::RectF(rightEdge - btnW, 6.0f, rightEdge, 30.0f);
            headerBtns_.push_back(HeaderBtnVisual{ids[i], r});
            rightEdge -= (btnW + 2.0f);
        }
        std::reverse(headerBtns_.begin(), headerBtns_.end());
    }

    // 标题与今日完成进度 / 穿透提示
    float titleMaxRight = headerBtns_.empty() ? (sizeDip.width - 10.0f) : (headerBtns_.front().rectDip.left - 6.0f);
    std::wstring headerText;
    if (!toastMessage_.empty()) {
        headerText = toastMessage_;
    } else if (state_.isClickThrough()) {
        wchar_t buf[96]{};
        std::swprintf(
            buf,
            std::size(buf),
            L"今日待办 %zu/%zu · 穿透中 (Alt+T 恢复)",
            doc_.completedCount(),
            doc_.totalCount());
        headerText = buf;
    } else {
        headerText = L"今日待办";
    }

    brush->SetColor(hexColor(!toastMessage_.empty() ? 0x0067C0 : 0x0F172A));
    D2D1_RECT_F titleRect = D2D1::RectF(12.0f, 0.0f, std::min(74.0f, titleMaxRight), headerH);
    if (!toastMessage_.empty() || state_.isClickThrough()) {
        titleRect.right = titleMaxRight;
    }
    renderTarget_->DrawTextW(
        headerText.c_str(),
        static_cast<UINT32>(headerText.size()),
        titleFormat_,
        titleRect,
        brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP);

    // 进度与点击打开日志徽标（如 "1/3 日志"）
    logsBadgeRectDip_ = D2D1::RectF(0, 0, 0, 0);
    if (toastMessage_.empty() && !state_.isClickThrough() && titleMaxRight > 122.0f) {
        wchar_t badgeBuf[48]{};
        std::swprintf(badgeBuf, std::size(badgeBuf), L"%zu/%zu 日志", doc_.completedCount(), doc_.totalCount());
        std::wstring badgeStr = badgeBuf;
        float badgeW = std::min(68.0f, titleMaxRight - 72.0f);
        logsBadgeRectDip_ = D2D1::RectF(72.0f, 8.0f, 72.0f + badgeW, 28.0f);
        brush->SetColor(hexColor(hoveredLogsBadge_ ? 0xDBEAFE : 0xE2E8F0));
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(logsBadgeRectDip_, 5.0f, 5.0f), brush);
        brush->SetColor(hexColor(hoveredLogsBadge_ ? 0x0067C0 : 0x475569));
        renderTarget_->DrawTextW(
            badgeStr.c_str(),
            static_cast<UINT32>(badgeStr.size()),
            badgeFormat_,
            logsBadgeRectDip_,
            brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // 绘制标题栏右侧控制图标
    for (const HeaderBtnVisual& btn : headerBtns_) {
        bool activeState = (btn.id == HeaderBtnId::Topmost && state_.isTopmost());
        bool hovered = (hoveredBtn_ == btn.id);
        if (activeState || hovered) {
            brush->SetColor(hexColor(activeState ? 0xDBEAFE : 0xE2E8F0));
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(btn.rectDip, 5.0f, 5.0f), brush);
        }
        brush->SetColor(hexColor(activeState ? 0x0067C0 : (hovered ? 0x0F172A : 0x475569)));
        drawHeaderIcon(btn.id, btn.rectDip, brush);
    }

    // 若处于自由展开或贴边弹出态，绘制待办列表与底部输入区
    rowVisuals_.clear();
    if (!collapsed) {
        float bottomReserve = state_.isClickThrough() ? 10.0f : 52.0f;
        listViewportDip_ = D2D1::RectF(0.0f, headerH + 2.0f, sizeDip.width, std::max(headerH + 4.0f, sizeDip.height - bottomReserve));

        // 预先测量每一条待办的折行高度，支持多行长文本清晰展示
        struct LayoutEntry {
            IDWriteTextLayout* layout = nullptr;
            float rowHeight = 34.0f;
            std::wstring displayStr;
        };
        std::vector<LayoutEntry> layouts(doc_.items().size());
        float totalContentHeight = 8.0f;
        float textAvailWidth = std::max(80.0f, sizeDip.width - 66.0f);

        for (std::size_t i = 0; i < doc_.items().size(); ++i) {
            const TodoItem& item = doc_.items()[i];
            std::string fullUtf8 = item.text;
            if (item.completed && !item.completedTime.empty()) {
                fullUtf8 += "  (" + item.completedTime + ")";
            }
            layouts[i].displayStr = utf8ToWide(fullUtf8);

            if (dwriteFactory_ && itemFormat_ && !layouts[i].displayStr.empty()) {
                HRESULT hr = dwriteFactory_->CreateTextLayout(
                    layouts[i].displayStr.c_str(),
                    static_cast<UINT32>(layouts[i].displayStr.size()),
                    itemFormat_,
                    textAvailWidth,
                    400.0f,
                    &layouts[i].layout);
                if (SUCCEEDED(hr) && layouts[i].layout) {
                    if (item.completed) {
                        DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(item.text.empty() ? 0 : utf8ToWide(item.text).size())};
                        layouts[i].layout->SetStrikethrough(TRUE, range);
                    }
                    DWRITE_TEXT_METRICS tm{};
                    if (SUCCEEDED(layouts[i].layout->GetMetrics(&tm))) {
                        layouts[i].rowHeight = std::max(34.0f, std::ceil(tm.height) + 14.0f);
                    }
                }
            }
            totalContentHeight += layouts[i].rowHeight;
        }

        float viewH = listViewportDip_.bottom - listViewportDip_.top;
        maxScrollDip_ = std::max(0.0f, totalContentHeight - viewH);
        scrollOffsetDip_ = std::clamp(scrollOffsetDip_, 0.0f, maxScrollDip_);

        renderTarget_->PushAxisAlignedClip(listViewportDip_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (doc_.items().empty()) {
            std::wstring emptyHint = L"今日暂无待办 · 点击下方输入或由 AI 写入 todo.md";
            brush->SetColor(hexColor(0x94A3B8));
            D2D1_RECT_F emptyRect = D2D1::RectF(16.0f, listViewportDip_.top + 20.0f, sizeDip.width - 16.0f, listViewportDip_.top + 80.0f);
            itemFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            renderTarget_->DrawTextW(
                emptyHint.c_str(),
                static_cast<UINT32>(emptyHint.size()),
                itemFormat_,
                emptyRect,
                brush);
            itemFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        } else {
            float curY = listViewportDip_.top + 4.0f - scrollOffsetDip_;
            for (std::size_t i = 0; i < doc_.items().size(); ++i) {
                const TodoItem& item = doc_.items()[i];
                float rh = layouts[i].rowHeight;
                float rowBottom = curY + rh;

                D2D1_RECT_F checkR = D2D1::RectF(14.0f, curY + 8.0f, 32.0f, curY + 26.0f);
                D2D1_RECT_F delR = D2D1::RectF(sizeDip.width - 30.0f, curY + (rh - 22.0f) * 0.5f, sizeDip.width - 10.0f, curY + (rh + 22.0f) * 0.5f);
                rowVisuals_.push_back(RowVisual{curY, rowBottom, checkR, delR});

                if (rowBottom >= listViewportDip_.top && curY <= listViewportDip_.bottom) {
                    bool rowHov = (!state_.isClickThrough() && static_cast<int>(i) == hoveredRow_);
                    if (rowHov) {
                        brush->SetColor(hexColor(0xF1F5F9));
                        renderTarget_->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(6.0f, curY + 1.0f, sizeDip.width - 6.0f, rowBottom - 1.0f), 6.0f, 6.0f),
                            brush);
                    }

                    // 绘制圆角复选框与对勾
                    D2D1_ROUNDED_RECT cbRound = D2D1::RoundedRect(checkR, 4.5f, 4.5f);
                    if (item.completed) {
                        brush->SetColor(hexColor(0x0067C0));
                        renderTarget_->FillRoundedRectangle(cbRound, brush);
                        brush->SetColor(hexColor(0xFFFFFF));
                        renderTarget_->DrawLine(
                            D2D1::Point2F(checkR.left + 4.2f, checkR.top + 9.2f),
                            D2D1::Point2F(checkR.left + 7.6f, checkR.top + 12.6f),
                            brush,
                            1.8f);
                        renderTarget_->DrawLine(
                            D2D1::Point2F(checkR.left + 7.6f, checkR.top + 12.6f),
                            D2D1::Point2F(checkR.left + 13.8f, checkR.top + 5.6f),
                            brush,
                            1.8f);
                    } else {
                        brush->SetColor(hexColor(0xFFFFFF));
                        renderTarget_->FillRoundedRectangle(cbRound, brush);
                        brush->SetColor(hexColor(rowHov ? 0x0067C0 : 0x94A3B8));
                        renderTarget_->DrawRoundedRectangle(cbRound, brush, 1.4f);
                    }

                    // 绘制待办文本
                    brush->SetColor(hexColor(item.completed ? 0x94A3B8 : 0x0F172A));
                    if (layouts[i].layout) {
                        renderTarget_->DrawTextLayout(D2D1::Point2F(40.0f, curY + 7.0f), layouts[i].layout, brush);
                    }

                    // 悬停行右侧浮现删除图标 ×
                    if (rowHov) {
                        if (hoveredRowDelete_) {
                            brush->SetColor(hexColor(0xFEE2E2));
                            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(delR, 4.0f, 4.0f), brush);
                        }
                        brush->SetColor(hexColor(hoveredRowDelete_ ? 0xDC2626 : 0x94A3B8));
                        float dcx = (delR.left + delR.right) * 0.5f;
                        float dcy = (delR.top + delR.bottom) * 0.5f;
                        renderTarget_->DrawLine(D2D1::Point2F(dcx - 4.0f, dcy - 4.0f), D2D1::Point2F(dcx + 4.0f, dcy + 4.0f), brush, 1.5f);
                        renderTarget_->DrawLine(D2D1::Point2F(dcx + 4.0f, dcy - 4.0f), D2D1::Point2F(dcx - 4.0f, dcy + 4.0f), brush, 1.5f);
                    }
                }
                curY = rowBottom;
            }
        }

        // 轻量滚动指示细条
        if (maxScrollDip_ > 1.0f) {
            float ratio = viewH / totalContentHeight;
            float thumbH = std::max(20.0f, viewH * ratio);
            float thumbTop = listViewportDip_.top + (viewH - thumbH) * (scrollOffsetDip_ / maxScrollDip_);
            brush->SetColor(hexColor(0xCBD5E1));
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(sizeDip.width - 4.5f, thumbTop, sizeDip.width - 2.0f, thumbTop + thumbH), 1.2f, 1.2f),
                brush);
        }

        renderTarget_->PopAxisAlignedClip();

        for (LayoutEntry& le : layouts) {
            safeRelease(le.layout);
        }

        // 底部新建待办输入胶囊栏（穿透模式下隐藏以保持纯净）
        if (!state_.isClickThrough()) {
            inputBoxRectDip_ = D2D1::RectF(10.0f, sizeDip.height - 42.0f, sizeDip.width - 10.0f, sizeDip.height - 10.0f);
            D2D1_ROUNDED_RECT inputRound = D2D1::RoundedRect(inputBoxRectDip_, 6.0f, 6.0f);
            brush->SetColor(hexColor(0xFFFFFF));
            renderTarget_->FillRoundedRectangle(inputRound, brush);
            brush->SetColor(hexColor(inputActive_ ? 0x0067C0 : (hoveredInputBox_ ? 0x94A3B8 : 0xCBD5E1)));
            renderTarget_->DrawRoundedRectangle(inputRound, brush, inputActive_ ? 1.6f : 1.1f);

            if (!inputActive_) {
                std::wstring placeholder = L"+ 添加今日待办，回车创建...";
                brush->SetColor(hexColor(0x94A3B8));
                D2D1_RECT_F hintRect = D2D1::RectF(
                    inputBoxRectDip_.left + 10.0f,
                    inputBoxRectDip_.top,
                    inputBoxRectDip_.right - 10.0f,
                    inputBoxRectDip_.bottom);
                renderTarget_->DrawTextW(
                    placeholder.c_str(),
                    static_cast<UINT32>(placeholder.size()),
                    hintFormat_,
                    hintRect,
                    brush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    // 窗口外框细描边，强化 Windows 11 卡片悬浮边界质感
    brush->SetColor(hexColor(state_.isClickThrough() ? 0x93C5FD : 0xCBD5E1));
    renderTarget_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, sizeDip.width - 0.5f, sizeDip.height - 0.5f), 7.5f, 7.5f),
        brush,
        1.0f);

    brush->Release();
}

FloatingTodoWindow::HeaderBtnId FloatingTodoWindow::hitTestHeaderBtn(D2D1_POINT_2F ptDip) const noexcept {
    for (const HeaderBtnVisual& b : headerBtns_) {
        if (pointInRect(ptDip, b.rectDip)) {
            return b.id;
        }
    }
    return HeaderBtnId::None;
}

int FloatingTodoWindow::hitTestRow(D2D1_POINT_2F ptDip, bool& outOnCheckbox, bool& outOnDelete) const noexcept {
    outOnCheckbox = false;
    outOnDelete = false;
    if (!pointInRect(ptDip, listViewportDip_)) {
        return -1;
    }
    for (std::size_t i = 0; i < rowVisuals_.size(); ++i) {
        const RowVisual& rv = rowVisuals_[i];
        if (ptDip.y >= rv.topDip && ptDip.y < rv.bottomDip) {
            D2D1_RECT_F expandedCheck = D2D1::RectF(rv.checkRectDip.left - 4.0f, rv.topDip, rv.checkRectDip.right + 6.0f, rv.bottomDip);
            outOnCheckbox = pointInRect(ptDip, expandedCheck);
            outOnDelete = pointInRect(ptDip, rv.deleteRectDip);
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool FloatingTodoWindow::hitTestInputBox(D2D1_POINT_2F ptDip) const noexcept {
    if (state_.isClickThrough() || state_.mode() == PanelMode::CollapsedVert || state_.mode() == PanelMode::DockedHidden) {
        return false;
    }
    return pointInRect(ptDip, inputBoxRectDip_);
}

bool FloatingTodoWindow::hitTestLogsBadge(D2D1_POINT_2F ptDip) const noexcept {
    if (logsBadgeRectDip_.right <= logsBadgeRectDip_.left) {
        return false;
    }
    return pointInRect(ptDip, logsBadgeRectDip_);
}

void FloatingTodoWindow::beginInputEditing() {
    if (!editHwnd_ || state_.isClickThrough() || state_.mode() == PanelMode::CollapsedVert || state_.mode() == PanelMode::DockedHidden) {
        return;
    }
    inputActive_ = true;
    updateEditFontAndBounds();
    ShowWindow(editHwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(editHwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FloatingTodoWindow::commitInputText() {
    if (!editHwnd_) {
        return;
    }
    int len = GetWindowTextLengthW(editHwnd_);
    if (len <= 0) {
        cancelInputEditing();
        return;
    }
    std::wstring wbuf(static_cast<std::size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(editHwnd_, wbuf.data(), len + 1);
    wbuf.resize(static_cast<std::size_t>(std::max(0, copied)));

    checkDayRollover();
    if (doc_.addItem(wideToUtf8(wbuf))) {
        storage_.save(doc_);
        scrollOffsetDip_ = 1e6f; // 自动滚至最新添加的底部条目
    }
    SetWindowTextW(editHwnd_, L"");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FloatingTodoWindow::cancelInputEditing() {
    if (!editHwnd_) {
        return;
    }
    inputActive_ = false;
    ShowWindow(editHwnd_, SW_HIDE);
    if (GetFocus() == editHwnd_) {
        SetFocus(hwnd_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FloatingTodoWindow::toggleTopmostAction() {
    bool on = state_.toggleTopmost();
    applyWindowStylesAndBounds(true);
    toastMessage_ = on ? L"已开启窗口置顶" : L"已取消窗口置顶";
    SetTimer(hwnd_, TIMER_TOAST_CLEAR, 1400, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FloatingTodoWindow::toggleClickThroughAction() {
    state_.toggleClickThrough(currentMonitorWorkArea());
    hoveredBtn_ = HeaderBtnId::None;
    hoveredRow_ = -1;
    hoveredInputBox_ = false;
    toastMessage_.clear();
    applyWindowStylesAndBounds(true);
    updateTrayTooltip();
}

void FloatingTodoWindow::toggleCollapseAction() {
    state_.toggleVerticalCollapse(currentMonitorWorkArea());
    applyWindowStylesAndBounds(false);
}

void FloatingTodoWindow::dockLeftAction() {
    state_.dockToLeft(currentMonitorWorkArea());
    applyWindowStylesAndBounds(false);
}

void FloatingTodoWindow::resetPositionAction() {
    ScreenRect wa = currentMonitorWorkArea();
    if (state_.mode() != PanelMode::Expanded) {
        state_.onDragFinished(wa.right - state_.metrics().panelWidth - 36, wa.top + 64, wa);
    } else {
        state_.setPosition(wa.right - state_.metrics().panelWidth - 36, wa.top + 64, wa);
    }
    applyWindowStylesAndBounds(true);
}

void FloatingTodoWindow::copyAiInstructionToClipboard() {
    std::wstring pathStr = storage_.todoFilePath().wstring();
    std::wstring clipText = L"我的当日待办文件位于 " + pathStr +
                            L"（UTF-8 Markdown 格式，\"- [ ] 内容\" 表示未完成，\"- [x] HH:MM 内容\" 表示已完成）。请直接读取或修改该文件来查看、新增或勾选今日待办。";

    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        std::size_t bytes = (clipText.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            void* dst = GlobalLock(hMem);
            if (dst) {
                std::memcpy(dst, clipText.c_str(), bytes);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            } else {
                GlobalFree(hMem);
            }
        }
        CloseClipboard();

        toastMessage_ = L"已复制 AI 接入指令";
        SetTimer(hwnd_, TIMER_TOAST_CLEAR, 1600, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void FloatingTodoWindow::openLogsFolder() {
    std::error_code ec;
    std::filesystem::create_directories(storage_.logsDirPath(), ec);
    ShellExecuteW(nullptr, L"open", storage_.logsDirPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void FloatingTodoWindow::openTodayTodoFile() {
    ShellExecuteW(nullptr, L"open", storage_.todoFilePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void FloatingTodoWindow::checkDayRollover() {
    std::string today = currentLocalDateString();
    if (doc_.date() != today) {
        doc_ = storage_.loadOrInit(today);
        scrollOffsetDip_ = 0.0f;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void FloatingTodoWindow::onExternalFileNotification() {
    std::string today = currentLocalDateString();
    if (doc_.date() != today) {
        doc_ = storage_.loadOrInit(today);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    TodoDocument updated;
    if (storage_.reloadIfExternallyModified(today, updated)) {
        doc_ = std::move(updated);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void FloatingTodoWindow::showContextMenu(POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING | (state_.isTopmost() ? MF_CHECKED : 0), CMD_TOGGLE_TOPMOST, L"窗口置顶");
    AppendMenuW(
        menu,
        MF_STRING | (state_.isClickThrough() ? MF_CHECKED : 0),
        CMD_TOGGLE_CLICKTHROUGH,
        L"鼠标穿透 / 不与鼠标交互 (Alt+T)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_COPY_AI, L"复制 AI 接入说明");
    AppendMenuW(menu, MF_STRING, CMD_OPEN_LOGS, L"打开每日日志目录 (logs)");
    AppendMenuW(menu, MF_STRING, CMD_OPEN_TODO, L"打开今日 todo.md");
    AppendMenuW(menu, MF_STRING, CMD_RESET_POS, L"展开并归位窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"退出");

    SetForegroundWindow(hwnd_);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
        case CMD_TOGGLE_TOPMOST:
            toggleTopmostAction();
            break;
        case CMD_TOGGLE_CLICKTHROUGH:
            toggleClickThroughAction();
            break;
        case CMD_RESET_POS:
            resetPositionAction();
            break;
        case CMD_COPY_AI:
            copyAiInstructionToClipboard();
            break;
        case CMD_OPEN_LOGS:
            openLogsFolder();
            break;
        case CMD_OPEN_TODO:
            openTodayTodoFile();
            break;
        case CMD_EXIT:
            DestroyWindow(hwnd_);
            break;
        default:
            break;
    }
}

HICON FloatingTodoWindow::createProceduralAppIcon(bool clickThroughStyle) {
    constexpr int sz = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sz;
    bi.bV5Height = -sz; // 自顶向下
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hColor || !bits) {
        return LoadIconW(nullptr, IDI_APPLICATION);
    }

    auto* px = static_cast<std::uint32_t*>(bits);
    std::uint32_t bg = clickThroughStyle ? 0xCC64748Bu : 0xFF0067C0u;
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            bool inside = (x >= 2 && x < sz - 2 && y >= 2 && y < sz - 2);
            // 四个圆角裁剪
            if (inside) {
                int dx = (x < 7) ? (7 - x) : ((x > sz - 8) ? (x - (sz - 8)) : 0);
                int dy = (y < 7) ? (7 - y) : ((y > sz - 8) ? (y - (sz - 8)) : 0);
                if (dx * dx + dy * dy > 25) {
                    inside = false;
                }
            }
            px[y * sz + x] = inside ? bg : 0x00000000u;
        }
    }

    // 绘制白色对勾像素
    auto putWhite = [&](int x, int y) {
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                int nx = x + ox;
                int ny = y + oy;
                if (nx >= 4 && nx < sz - 4 && ny >= 4 && ny < sz - 4) {
                    px[ny * sz + nx] = 0xFFFFFFFFu;
                }
            }
        }
    };
    for (int i = 0; i <= 5; ++i) {
        putWhite(9 + i, 16 + i);
    }
    for (int i = 0; i <= 9; ++i) {
        putWhite(14 + i, 21 - i);
    }

    HBITMAP hMask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hColor;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hMask);
    DeleteObject(hColor);
    return hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
}

void FloatingTodoWindow::initTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY_EVENT;
    nid.hIcon = iconNormal_;
    std::wcsncpy(nid.szTip, L"今日待办 · 单击切换鼠标穿透 (Alt+T)", std::size(nid.szTip) - 1);
    trayAdded_ = (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
}

void FloatingTodoWindow::updateTrayTooltip() {
    if (!trayAdded_) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = state_.isClickThrough() ? iconClickThrough_ : iconNormal_;
    const wchar_t* tip = state_.isClickThrough()
                             ? L"今日待办 [鼠标穿透中] · 单击或按 Alt+T 恢复交互"
                             : L"今日待办 · 单击开启鼠标穿透 (Alt+T)";
    std::wcsncpy(nid.szTip, tip, std::size(nid.szTip) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void FloatingTodoWindow::removeTrayIcon() {
    if (!trayAdded_ || !hwnd_) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

LRESULT CALLBACK FloatingTodoWindow::editSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<FloatingTodoWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || !self->origEditProc_) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            self->commitInputText();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            SetWindowTextW(hwnd, L"");
            self->cancelInputEditing();
            return 0;
        }
    } else if (msg == WM_CHAR && (wParam == L'\r' || wParam == L'\n' || wParam == 0x1B)) {
        // 抑制单行 EDIT 按回车或 Esc 时的系统提示音
        return 0;
    } else if (msg == WM_KILLFOCUS) {
        if (GetWindowTextLengthW(hwnd) == 0) {
            self->inputActive_ = false;
            ShowWindow(hwnd, SW_HIDE);
            InvalidateRect(self->hwnd_, nullptr, FALSE);
        }
    }

    return CallWindowProcW(self->origEditProc_, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FloatingTodoWindow::wndProcSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* self = static_cast<FloatingTodoWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FloatingTodoWindow::wndProcThunk));
        return self->handleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FloatingTodoWindow::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<FloatingTodoWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->handleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT FloatingTodoWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            onPaint();
            return 0;

        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, RGB(15, 23, 42));
            SetBkColor(hdc, RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(editBgBrush_);
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            state_.setDpi(dpi_, currentMonitorWorkArea());
            applyWindowStylesAndBounds(false);
            return 0;
        }

        case WM_HOTKEY: {
            if (static_cast<int>(wParam) == HOTKEY_ID_CLICKTHROUGH) {
                toggleClickThroughAction();
                return 0;
            }
            break;
        }

        case WM_APP_TODO_CHANGED: {
            onExternalFileNotification();
            return 0;
        }

        case WM_APP_TRAY_EVENT: {
            if (lParam == WM_LBUTTONUP) {
                // 单击系统托盘图标直接一键切换“鼠标穿透 / 正常交互”
                toggleClickThroughAction();
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                POINT pt{};
                GetCursorPos(&pt);
                showContextMenu(pt);
            }
            return 0;
        }

        case WM_TIMER: {
            if (wParam == TIMER_PEEK_HIDE) {
                POINT cursor{};
                RECT winRc{};
                GetCursorPos(&cursor);
                GetWindowRect(hwnd_, &winRc);
                if (!PtInRect(&winRc, cursor)) {
                    KillTimer(hwnd_, TIMER_PEEK_HIDE);
                    if (state_.onMouseLeaveTimeout(isInputActive())) {
                        applyWindowStylesAndBounds(false);
                    }
                }
                return 0;
            }
            if (wParam == TIMER_TOAST_CLEAR) {
                KillTimer(hwnd_, TIMER_TOAST_CLEAR);
                toastMessage_.clear();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wParam == TIMER_DAY_CHECK) {
                checkDayRollover();
                return 0;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (!trackingMouseLeave_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd_;
                TrackMouseEvent(&tme);
                trackingMouseLeave_ = true;
            }

            if (state_.mode() == PanelMode::DockedHidden) {
                if (state_.onMouseEnterStrip()) {
                    KillTimer(hwnd_, TIMER_PEEK_HIDE);
                    applyWindowStylesAndBounds(false);
                }
                return 0;
            }

            if (state_.mode() == PanelMode::DockedPeek) {
                KillTimer(hwnd_, TIMER_PEEK_HIDE);
            }

            if (draggingWindow_) {
                POINT cur{};
                GetCursorPos(&cur);
                int dx = cur.x - dragStartCursor_.x;
                int dy = cur.y - dragStartCursor_.y;
                if (std::abs(dx) > 2 || std::abs(dy) > 2) {
                    dragMoved_ = true;
                }
                if (dragMoved_) {
                    SetWindowPos(
                        hwnd_,
                        nullptr,
                        dragStartWinOrigin_.x + dx,
                        dragStartWinOrigin_.y + dy,
                        0,
                        0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
                return 0;
            }

            D2D1_POINT_2F pt = clientPxToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            HeaderBtnId newBtn = hitTestHeaderBtn(pt);
            bool dummyCheck = false;
            bool newDel = false;
            int newRow = hitTestRow(pt, dummyCheck, newDel);
            bool newInput = hitTestInputBox(pt);
            bool newBadge = hitTestLogsBadge(pt);

            SetCursor(LoadCursorW(nullptr, newInput ? IDC_IBEAM : IDC_ARROW));

            if (newBtn != hoveredBtn_ || newRow != hoveredRow_ || newDel != hoveredRowDelete_ ||
                newInput != hoveredInputBox_ || newBadge != hoveredLogsBadge_) {
                hoveredBtn_ = newBtn;
                hoveredRow_ = newRow;
                hoveredRowDelete_ = newDel;
                hoveredInputBox_ = newInput;
                hoveredLogsBadge_ = newBadge;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            trackingMouseLeave_ = false;
            hoveredBtn_ = HeaderBtnId::None;
            hoveredRow_ = -1;
            hoveredRowDelete_ = false;
            hoveredInputBox_ = false;
            hoveredLogsBadge_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);

            if (state_.mode() == PanelMode::DockedPeek && !state_.isClickThrough()) {
                SetTimer(hwnd_, TIMER_PEEK_HIDE, 250, nullptr);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (state_.mode() == PanelMode::DockedHidden) {
                if (state_.onMouseEnterStrip()) {
                    applyWindowStylesAndBounds(false);
                }
                return 0;
            }

            D2D1_POINT_2F pt = clientPxToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

            // 点击输入区外自动收起空输入框
            if (inputActive_ && !hitTestInputBox(pt) && GetWindowTextLengthW(editHwnd_) == 0) {
                cancelInputEditing();
            }

            if (hitTestHeaderBtn(pt) != HeaderBtnId::None || hitTestLogsBadge(pt)) {
                return 0;
            }

            if (pt.y <= 36.0f) {
                draggingWindow_ = true;
                dragMoved_ = false;
                GetCursorPos(&dragStartCursor_);
                RECT wr{};
                GetWindowRect(hwnd_, &wr);
                dragStartWinOrigin_ = POINT{wr.left, wr.top};
                SetCapture(hwnd_);
                return 0;
            }

            if (hitTestInputBox(pt)) {
                beginInputEditing();
                return 0;
            }

            bool onCheck = false;
            bool onDel = false;
            int row = hitTestRow(pt, onCheck, onDel);
            if (row >= 0) {
                checkDayRollover();
                if (onDel) {
                    if (doc_.removeItem(static_cast<std::size_t>(row))) {
                        storage_.save(doc_);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                } else {
                    // 点击整行或复选框均顺手切换完成状态
                    if (doc_.toggleItem(static_cast<std::size_t>(row), currentLocalTimeHHMM())) {
                        storage_.save(doc_);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (draggingWindow_) {
                draggingWindow_ = false;
                ReleaseCapture();
                if (dragMoved_) {
                    RECT wr{};
                    GetWindowRect(hwnd_, &wr);
                    state_.onDragFinished(wr.left, wr.top, currentMonitorWorkArea());
                    applyWindowStylesAndBounds(false);
                }
                return 0;
            }

            D2D1_POINT_2F pt = clientPxToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hitTestLogsBadge(pt)) {
                openLogsFolder();
                return 0;
            }

            HeaderBtnId btn = hitTestHeaderBtn(pt);
            switch (btn) {
                case HeaderBtnId::CopyAi:
                    copyAiInstructionToClipboard();
                    return 0;
                case HeaderBtnId::Topmost:
                    toggleTopmostAction();
                    return 0;
                case HeaderBtnId::ClickThrough:
                    toggleClickThroughAction();
                    return 0;
                case HeaderBtnId::DockLeft:
                    dockLeftAction();
                    return 0;
                case HeaderBtnId::CollapseVert:
                    toggleCollapseAction();
                    return 0;
                case HeaderBtnId::None:
                    break;
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            D2D1_POINT_2F pt = clientPxToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (pt.y <= 36.0f && hitTestHeaderBtn(pt) == HeaderBtnId::None && !hitTestLogsBadge(pt)) {
                toggleCollapseAction();
                return 0;
            }
            break;
        }

        case WM_RBUTTONUP: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd_, &pt);
            showContextMenu(pt);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (state_.mode() == PanelMode::Expanded || state_.mode() == PanelMode::DockedPeek) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                scrollOffsetDip_ = std::clamp(scrollOffsetDip_ - static_cast<float>(delta) * 0.25f, 0.0f, maxScrollDip_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        }

        case WM_DESTROY: {
            storage_.stopWatching();
            removeTrayIcon();
            if (hotkeyRegistered_) {
                UnregisterHotKey(hwnd_, HOTKEY_ID_CLICKTHROUGH);
                hotkeyRegistered_ = false;
            }
            PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace todofloat
