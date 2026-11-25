#include "ui_window.hpp"
#include "startup.hpp"

#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace todofloat {

FloatingTodoWindow* FloatingTodoWindow::sActiveInstance_ = nullptr;

namespace {

constexpr wchar_t kWindowClassName[] = L"TodoFloatNativeWindowClass";
constexpr UINT WM_APP_TODO_CHANGED = WM_APP + 10;
constexpr UINT WM_APP_TRAY_EVENT = WM_APP + 11;
constexpr UINT WM_APP_REMOTE_SYNCED = WM_APP + 12;

constexpr UINT_PTR TIMER_PEEK_HIDE = 1;
constexpr UINT_PTR TIMER_TOAST_CLEAR = 2;
constexpr UINT_PTR TIMER_DAY_CHECK = 3;
constexpr UINT_PTR TIMER_CARET_BLINK = 4;
constexpr UINT_PTR TIMER_SLIDE_ANIM = 5;
constexpr UINT_PTR TIMER_SHOWDESKTOP_GUARD = 6;

// 全局快捷键 ID
constexpr int HOTKEY_ID_CLICKTHROUGH = 0x5446; // Alt + T：切换鼠标穿透
constexpr int HOTKEY_ID_SUMMON = 0x5447;       // Alt + `：一键召唤 / 收回面板
constexpr int HOTKEY_ID_QUICK_ADD = 0x5448;    // Alt + Q：闪电新建待办

// 右键菜单命令 ID
constexpr int CMD_ADD_TASK = 1000;
constexpr int CMD_TOGGLE_TOPMOST = 1001;
constexpr int CMD_TOGGLE_CLICKTHROUGH = 1002;
constexpr int CMD_DOCK_LEFT = 1003;
constexpr int CMD_DOCK_RIGHT = 1004;
constexpr int CMD_TOGGLE_COLLAPSE = 1005;
constexpr int CMD_RESET_POS = 1006;
constexpr int CMD_COPY_AI = 1007;
constexpr int CMD_OPEN_LOGS = 1008;
constexpr int CMD_OPEN_TODO = 1009;
constexpr int CMD_OPACITY_UP = 1010;
constexpr int CMD_OPACITY_DOWN = 1011;
constexpr int CMD_TOGGLE_AUTOSTART = 1012;
constexpr int CMD_EXIT = 1013;

constexpr float kHeaderDip = 28.0f;
constexpr float kInputAreaDip = 38.0f;
constexpr float kBottomPadDip = 6.0f;

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
    return r.right > r.left && pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
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
      doc_(storage.loadOrInit(currentLocalDateString())) {
    sActiveInstance_ = this;
}

FloatingTodoWindow::~FloatingTodoWindow() {
    storage_.stopWatching();
    if (fgEventHook_) {
        UnhookWinEvent(fgEventHook_);
        fgEventHook_ = nullptr;
    }
    unregisterGlobalHotkeys();
    removeTrayIcon();
    releaseLayeredSurface();

    safeRelease(dcRenderTarget_);
    safeRelease(headerFormat_);
    safeRelease(badgeFormat_);
    safeRelease(itemPendingFormat_);
    safeRelease(itemDoneFormat_);
    safeRelease(focusFormat_);
    safeRelease(inputFormat_);
    safeRelease(dwriteFactory_);
    safeRelease(d2dFactory_);

    if (iconNormal_) {
        DestroyIcon(iconNormal_);
        iconNormal_ = nullptr;
    }
    if (iconClickThrough_) {
        DestroyIcon(iconClickThrough_);
        iconClickThrough_ = nullptr;
    }
    if (sActiveInstance_ == this) {
        sActiveInstance_ = nullptr;
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

    // 使用预乘 Alpha + 软件 SIMD 光栅化的 DCRenderTarget，配合 UpdateLayeredWindow 实现底透字清的逐像素真透明
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi_),
        static_cast<float>(dpi_));

    hr = d2dFactory_->CreateDCRenderTarget(&props, &dcRenderTarget_);
    if (FAILED(hr) || !dcRenderTarget_) {
        return false;
    }
    // 分层透明通道下使用灰度抗锯齿，杜绝半透明背景上的彩色边缘伪影
    dcRenderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    return rebuildTextFormats();
}

bool FloatingTodoWindow::rebuildTextFormats() {
    safeRelease(headerFormat_);
    safeRelease(badgeFormat_);
    safeRelease(itemPendingFormat_);
    safeRelease(itemDoneFormat_);
    safeRelease(focusFormat_);
    safeRelease(inputFormat_);

    if (!dwriteFactory_) {
        return false;
    }

    auto makeFmt = [&](float sizeDip, DWRITE_FONT_WEIGHT weight, DWRITE_WORD_WRAPPING wrap, IDWriteTextFormat** out) -> bool {
        HRESULT hr = dwriteFactory_->CreateTextFormat(
            L"Microsoft YaHei UI",
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            sizeDip,
            L"zh-CN",
            out);
        if (FAILED(hr) || !*out) {
            return false;
        }
        (*out)->SetWordWrapping(wrap);
        return true;
    };

    if (!makeFmt(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_WORD_WRAPPING_NO_WRAP, &headerFormat_)) return false;
    if (!makeFmt(11.0f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_WORD_WRAPPING_NO_WRAP, &badgeFormat_)) return false;
    if (!makeFmt(14.0f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_WORD_WRAPPING_WRAP, &itemPendingFormat_)) return false;
    if (!makeFmt(13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_WORD_WRAPPING_WRAP, &itemDoneFormat_)) return false;
    if (!makeFmt(13.0f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_WORD_WRAPPING_NO_WRAP, &focusFormat_)) return false;
    if (!makeFmt(13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_WORD_WRAPPING_NO_WRAP, &inputFormat_)) return false;

    headerFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    badgeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    badgeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    focusFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    focusFormat_->SetTrimming(&trimming, nullptr);
    inputFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    return true;
}

bool FloatingTodoWindow::ensureLayeredSurface(int widthPx, int heightPx) {
    widthPx = std::max(1, widthPx);
    heightPx = std::max(1, heightPx);
    if (memDc_ && dibBitmap_ && surfaceW_ == widthPx && surfaceH_ == heightPx) {
        return true;
    }

    releaseLayeredSurface();

    screenDc_ = GetDC(nullptr);
    if (!screenDc_) {
        return false;
    }
    memDc_ = CreateCompatibleDC(screenDc_);
    if (!memDc_) {
        ReleaseDC(nullptr, screenDc_);
        screenDc_ = nullptr;
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = widthPx;
    bmi.bmiHeader.biHeight = -heightPx; // 自顶向下 32bpp BGRA
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = nullptr;
    dibBitmap_ = CreateDIBSection(screenDc_, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
    if (!dibBitmap_) {
        DeleteDC(memDc_);
        memDc_ = nullptr;
        ReleaseDC(nullptr, screenDc_);
        screenDc_ = nullptr;
        return false;
    }

    oldBitmap_ = SelectObject(memDc_, dibBitmap_);
    surfaceW_ = widthPx;
    surfaceH_ = heightPx;
    return true;
}

void FloatingTodoWindow::releaseLayeredSurface() noexcept {
    if (memDc_ && oldBitmap_) {
        SelectObject(memDc_, oldBitmap_);
        oldBitmap_ = nullptr;
    }
    if (dibBitmap_) {
        DeleteObject(dibBitmap_);
        dibBitmap_ = nullptr;
    }
    if (memDc_) {
        DeleteDC(memDc_);
        memDc_ = nullptr;
    }
    if (screenDc_) {
        ReleaseDC(nullptr, screenDc_);
        screenDc_ = nullptr;
    }
    surfaceW_ = 0;
    surfaceH_ = 0;
}

bool FloatingTodoWindow::create(HINSTANCE hInstance) {
    hInstance_ = hInstance;
    dpi_ = queryWindowDpi(nullptr);
    if (!initGraphicsFactories()) {
        return false;
    }

    iconNormal_ = createProceduralAppIcon(false);
    iconClickThrough_ = createProceduralAppIcon(true);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &FloatingTodoWindow::wndProcSetup;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = iconNormal_;
    wc.hIconSm = iconNormal_;
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    ScreenRect primaryWork{wa.left, wa.top, wa.right, wa.bottom};
    state_.setDpi(dpi_, primaryWork);

    // 默认停放在屏幕右上侧边缘附近，避开中央主视觉区
    int initX = std::max(primaryWork.left + 16, primaryWork.right - state_.metrics().panelWidth - 20);
    int initY = primaryWork.top + 72;
    state_.setPosition(initX, initY, primaryWork);
    ScreenRect bounds = state_.currentBounds(primaryWork);

    // WS_EX_TOOLWINDOW 隐藏于 Alt+Tab 与任务栏；WS_EX_LAYERED 承载逐像素透明渲染
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
    hwnd_ = CreateWindowExW(
        exStyle,
        kWindowClassName,
        L"今日待办",
        WS_POPUP,
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
    dcRenderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));

    syncAutoHeightAndPresent(SlideTransition::Instant, true);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    initTrayIcon();
    registerGlobalHotkeys();

    // 监听系统前台窗口变化：当用户按下 Win + D 抬升桌面层 WorkerW / Progman 时，立即恢复悬浮窗与侧边耳标置顶可见
    fgEventHook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        &FloatingTodoWindow::winEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    SetTimer(hwnd_, TIMER_DAY_CHECK, 30000, nullptr);

    HWND targetHwnd = hwnd_;
    storage_.startWatching([targetHwnd]() {
        PostMessageW(targetHwnd, WM_APP_TODO_CHANGED, 0, 0);
    });

    return true;
}

std::function<void()> FloatingTodoWindow::remoteRefreshCallback() const {
    HWND target = hwnd_;
    return [target]() {
        if (target) {
            PostMessageW(target, WM_APP_REMOTE_SYNCED, 0, 0);
        }
    };
}

void CALLBACK FloatingTodoWindow::winEventProc(
    HWINEVENTHOOK,
    DWORD event,
    HWND fgHwnd,
    LONG,
    LONG,
    DWORD,
    DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && sActiveInstance_ && fgHwnd) {
        sActiveInstance_->onForegroundWindowChanged(fgHwnd);
    }
}

void FloatingTodoWindow::onForegroundWindowChanged(HWND fgHwnd) {
    if (!hwnd_ || fgHwnd == hwnd_) {
        return;
    }
    wchar_t clsName[64]{};
    if (GetClassNameW(fgHwnd, clsName, static_cast<int>(std::size(clsName))) > 0) {
        // Explorer 执行 Win + D 显示桌面时会将 WorkerW 或 Progman 提升到顶层遮盖普通 Topmost 窗口
        if ( std::wcscmp(clsName, L"WorkerW") == 0 || std::wcscmp(clsName, L"Progman") == 0) {
            SetTimer(hwnd_, TIMER_SHOWDESKTOP_GUARD, 25, nullptr);
        }
    }
}

void FloatingTodoWindow::registerGlobalHotkeys() {
    if (!hwnd_) {
        return;
    }
    RegisterHotKey(hwnd_, HOTKEY_ID_CLICKTHROUGH, MOD_ALT | MOD_NOREPEAT, 'T');
    RegisterHotKey(hwnd_, HOTKEY_ID_SUMMON, MOD_ALT | MOD_NOREPEAT, VK_OEM_3); // Alt + `
    RegisterHotKey(hwnd_, HOTKEY_ID_QUICK_ADD, MOD_ALT | MOD_NOREPEAT, 'Q');   // Alt + Q
}

void FloatingTodoWindow::unregisterGlobalHotkeys() {
    if (!hwnd_) {
        return;
    }
    UnregisterHotKey(hwnd_, HOTKEY_ID_CLICKTHROUGH);
    UnregisterHotKey(hwnd_, HOTKEY_ID_SUMMON);
    UnregisterHotKey(hwnd_, HOTKEY_ID_QUICK_ADD);
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

std::vector<FloatingTodoWindow::MeasuredRow> FloatingTodoWindow::measureItems(float panelWidthDip, float& outTotalRowsDip) {
    std::vector<std::size_t> order = doc_.displayOrderIndices();
    std::vector<MeasuredRow> rows;
    rows.reserve(order.size());

    outTotalRowsDip = 0.0f;
    if (order.empty()) {
        outTotalRowsDip = 32.0f; // 空状态仅占单行紧凑高度
        return rows;
    }

    float textAvailWidth = std::max(80.0f, panelWidthDip - 52.0f);
    bool seenDone = false;
    bool hasPending = doc_.pendingCount() > 0;

    for (std::size_t docIdx : order) {
        const TodoItem& item = doc_.items()[docIdx];
        if (item.completed && !seenDone && hasPending) {
            seenDone = true;
            outTotalRowsDip += 4.0f; // 未完成与已完成分组间的呼吸微距
        }

        MeasuredRow mr{};
        mr.docIndex = docIdx;
        std::string fullUtf8 = item.text;
        if (item.completed && !item.completedTime.empty()) {
            fullUtf8 += "  " + item.completedTime;
        }
        mr.displayStr = utf8ToWide(fullUtf8);

        IDWriteTextFormat* fmt = item.completed ? itemDoneFormat_ : itemPendingFormat_;
        if (dwriteFactory_ && fmt && !mr.displayStr.empty()) {
            HRESULT hr = dwriteFactory_->CreateTextLayout(
                mr.displayStr.c_str(),
                static_cast<UINT32>(mr.displayStr.size()),
                fmt,
                textAvailWidth,
                300.0f,
                &mr.layout);
            if (SUCCEEDED(hr) && mr.layout) {
                if (item.completed) {
                    DWRITE_TEXT_RANGE strikeRange{0, static_cast<UINT32>(utf8ToWide(item.text).size())};
                    mr.layout->SetStrikethrough(TRUE, strikeRange);
                }
                DWRITE_TEXT_METRICS tm{};
                if (SUCCEEDED(mr.layout->GetMetrics(&tm))) {
                    mr.rowHeightDip = std::max(29.0f, std::ceil(tm.height) + 10.0f);
                }
            }
        }
        outTotalRowsDip += mr.rowHeightDip;
        rows.push_back(mr);
    }

    return rows;
}

void FloatingTodoWindow::freeMeasuredRows(std::vector<MeasuredRow>& rows) noexcept {
    for (MeasuredRow& r : rows) {
        safeRelease(r.layout);
    }
    rows.clear();
}

void FloatingTodoWindow::applyExStyles(bool updateZOrder) {
    if (!hwnd_) {
        return;
    }
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    exStyle |= (WS_EX_TOOLWINDOW | WS_EX_LAYERED);
    exStyle &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    if (state_.isClickThrough()) {
        exStyle |= WS_EX_TRANSPARENT;
    } else {
        exStyle &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);

    if (updateZOrder) {
        // 侧边窄耳标与滑出预览态均保持置顶，防止贴边悬停唤醒时面板落入前台窗口后方触发遮挡重激活
        bool keepTop = state_.isTopmost() || (state_.mode() == PanelMode::DockedHidden) || (state_.mode() == PanelMode::DockedPeek);
        SetWindowPos(
            hwnd_,
            keepTop ? HWND_TOPMOST : HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void FloatingTodoWindow::syncAutoHeightAndPresent(SlideTransition transition, bool updateZOrder) {
    if (!hwnd_) {
        return;
    }
    RECT prevWinRc{};
    const bool hasPrevWin = (GetWindowRect(hwnd_, &prevWinRc) != FALSE) && (prevWinRc.right > prevWinRc.left);
    const bool wasSliding = (slideKind_ != SlideKind::None);
    const int prevAnimX = animCurrentX_;
    const float prevAlpha = animAlphaFactor_;

    if (state_.isClickThrough() || state_.mode() == PanelMode::CollapsedVert || state_.mode() == PanelMode::DockedHidden) {
        if (inputActive_) {
            inputActive_ = false;
            imeCompString_.clear();
            KillTimer(hwnd_, TIMER_CARET_BLINK);
        }
    }

    ScreenRect wa = currentMonitorWorkArea();
    float scale = dpiScale();
    float panelWidthDip = static_cast<float>(state_.metrics().panelWidth) / scale;

    float totalRowsDip = 0.0f;
    std::vector<MeasuredRow> rows = measureItems(panelWidthDip, totalRowsDip);
    freeMeasuredRows(rows);

    float desiredDip = kHeaderDip + totalRowsDip + (inputActive_ ? kInputAreaDip : 0.0f) + kBottomPadDip;
    int desiredPx = static_cast<int>(std::ceil(desiredDip * scale));
    state_.setContentHeightPx(desiredPx, wa);

    applyExStyles(updateZOrder);

    const ScreenRect peekB = state_.dockedPeekBounds(wa);
    const int fullW = peekB.width();
    const int stripW = state_.metrics().dockStripWidth;
    const int openX = peekB.left;
    const int closedX = (state_.dockSide() == DockSide::Left)
                            ? (wa.left - fullW + stripW)
                            : (wa.right - stripW);
    const int fullSpan = std::max(1, std::abs(closedX - openX));

    if (transition == SlideTransition::SlideOpen && state_.mode() == PanelMode::DockedPeek) {
        int startX = wasSliding ? prevAnimX : closedX;
        int span = std::abs(openX - startX);
        if (span > 2) {
            int dur = std::clamp((175 * span) / fullSpan, 85, 185);
            float startAlpha = wasSliding ? prevAlpha : 0.62f;
            startSlideAnimation(SlideKind::Opening, startX, openX, peekB.top, dur, startAlpha);
            return;
        }
    } else if ((transition == SlideTransition::SlideCloseAuto || transition == SlideTransition::SlideCloseManual) &&
               state_.mode() == PanelMode::DockedHidden) {
        int startX = openX;
        if (wasSliding) {
            startX = prevAnimX;
        } else if (hasPrevWin) {
            startX = (state_.dockSide() == DockSide::Left)
                         ? std::clamp<int>(prevWinRc.left, closedX, openX + fullW / 3)
                         : std::clamp<int>(prevWinRc.left, openX - fullW / 3, closedX);
        }
        int span = std::abs(closedX - startX);
        if (span > 2) {
            int dur = std::clamp((215 * span) / fullSpan, 95, 230);
            float startAlpha = wasSliding ? prevAlpha : 1.0f;
            SlideKind kind = (transition == SlideTransition::SlideCloseAuto)
                                 ? SlideKind::ClosingAutoHide
                                 : SlideKind::ClosingExplicit;
            startSlideAnimation(kind, startX, closedX, peekB.top, dur, startAlpha);
            return;
        }
    }

    slideKind_ = SlideKind::None;
    animAlphaFactor_ = 1.0f;
    KillTimer(hwnd_, TIMER_SLIDE_ANIM);
    renderAndPresent();
}

void FloatingTodoWindow::startSlideAnimation(
    SlideKind kind,
    int fromX,
    int toX,
    int fixedY,
    int durationMs,
    float startAlpha) {
    slideKind_ = kind;
    slideStartTime_ = std::chrono::steady_clock::now();
    slideDurationMs_ = std::max(1, durationMs);
    animStartX_ = fromX;
    animTargetX_ = toX;
    animCurrentX_ = fromX;
    animFixedY_ = fixedY;
    animStartAlpha_ = startAlpha;
    animAlphaFactor_ = startAlpha;
    renderAndPresent();
    SetTimer(hwnd_, TIMER_SLIDE_ANIM, 10, nullptr);
}

void FloatingTodoWindow::stepSlideAnimation() {
    if (slideKind_ == SlideKind::None) {
        KillTimer(hwnd_, TIMER_SLIDE_ANIM);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    float elapsedMs = std::chrono::duration<float, std::milli>(now - slideStartTime_).count();
    float t = std::clamp(elapsedMs / static_cast<float>(slideDurationMs_), 0.0f, 1.0f);

    if (t >= 1.0f) {
        SlideKind finished = slideKind_;
        slideKind_ = SlideKind::None;
        animAlphaFactor_ = 1.0f;
        KillTimer(hwnd_, TIMER_SLIDE_ANIM);
        if (finished == SlideKind::ClosingAutoHide || finished == SlideKind::ClosingExplicit) {
            trackingMouseLeave_ = false;
            mouseInside_ = false;
        }
        renderAndPresent();
        if (state_.mode() == PanelMode::DockedPeek && !state_.isClickThrough()) {
            POINT cursor{};
            RECT winRc{};
            if (GetCursorPos(&cursor) && GetWindowRect(hwnd_, &winRc) && !PtInRect(&winRc, cursor)) {
                SetTimer(hwnd_, TIMER_PEEK_HIDE, 220, nullptr);
            }
        }
        return;
    }

    float eased = t;
    float targetAlpha = 1.0f;
    if (slideKind_ == SlideKind::Opening) {
        float inv = 1.0f - t;
        eased = 1.0f - inv * inv * inv * inv; // EaseOutQuart：灵敏滑出，轻柔停稳
        targetAlpha = 1.0f;
    } else {
        // EaseInOutCubic：柔和起步加速，顺畅滑入屏幕边缘并阻尼贴合窄耳标
        if (t < 0.5f) {
            eased = 4.0f * t * t * t;
        } else {
            float u = -2.0f * t + 2.0f;
            eased = 1.0f - (u * u * u) * 0.5f;
        }
        targetAlpha = 0.48f;
    }

    animAlphaFactor_ = animStartAlpha_ + (targetAlpha - animStartAlpha_) * eased;
    animCurrentX_ = animStartX_ + static_cast<int>(std::lround(static_cast<float>(animTargetX_ - animStartX_) * eased));
    renderAndPresent();
}

void FloatingTodoWindow::renderAndPresent() {
    if (!hwnd_ || !dcRenderTarget_) {
        return;
    }

    ScreenRect wa = currentMonitorWorkArea();
    const bool sliding = (slideKind_ != SlideKind::None);
    ScreenRect b = sliding ? state_.dockedPeekBounds(wa) : state_.currentBounds(wa);
    int virtualX = b.left;
    int winY = b.top;
    int winW = b.width();
    int winH = b.height();

    if (draggingWindow_ || draggingStrip_) {
        RECT curRc{};
        if (GetWindowRect(hwnd_, &curRc)) {
            virtualX = curRc.left;
            winY = curRc.top;
        }
    } else if (sliding) {
        virtualX = animCurrentX_;
        winY = animFixedY_;
    }

    if (!ensureLayeredSurface(winW, winH)) {
        return;
    }

    RECT bindRc{0, 0, winW, winH};
    dcRenderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    if (FAILED(dcRenderTarget_->BindDC(memDc_, &bindRc))) {
        return;
    }

    float scale = dpiScale();
    D2D1_SIZE_F sizeDip = D2D1::SizeF(static_cast<float>(winW) / scale, static_cast<float>(winH) / scale);

    float panelWidthDip = static_cast<float>(state_.metrics().panelWidth) / scale;
    float totalRowsDip = 0.0f;
    std::vector<MeasuredRow> rows;
    if (sliding || state_.mode() == PanelMode::Expanded || state_.mode() == PanelMode::DockedPeek) {
        rows = measureItems(panelWidthDip, totalRowsDip);
    }

    // 计算当前情境下的背景不透明度：穿透态轻盈通透，静置态柔和半透，交互悬停态沉稳清晰
    float baseOp = state_.bgOpacity();
    float effectiveBgAlpha = baseOp;
    if (state_.isClickThrough()) {
        effectiveBgAlpha = std::clamp(baseOp * 0.50f, 0.10f, 0.55f);
    } else if (mouseInside_ || inputActive_) {
        effectiveBgAlpha = std::min(0.95f, baseOp + 0.20f);
    }

    dcRenderTarget_->BeginDraw();
    dcRenderTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)); // 窗口四角完全透明

    if (!sliding && state_.mode() == PanelMode::DockedHidden) {
        drawDockedStrip(sizeDip);
    } else if (!sliding && state_.mode() == PanelMode::CollapsedVert) {
        drawFocusBar(sizeDip, effectiveBgAlpha);
    } else {
        drawExpandedHud(sizeDip, effectiveBgAlpha, rows, totalRowsDip);
    }

    dcRenderTarget_->EndDraw();
    freeMeasuredRows(rows);

    POINT dstPos{virtualX, winY};
    SIZE winSize{winW, winH};
    POINT srcPos{0, 0};

    // 侧滑动画期间按当前显示器边缘逐像素裁剪源位图，使抽屉滑入滑出严丝合缝且多屏不穿帮
    if (sliding) {
        int visLeft = std::max(wa.left, virtualX);
        int visRight = std::min(wa.right, virtualX + winW);
        if (visRight > visLeft) {
            srcPos.x = visLeft - virtualX;
            dstPos.x = visLeft;
            winSize.cx = visRight - visLeft;
        } else {
            dstPos.x = (state_.dockSide() == DockSide::Left) ? wa.left : (wa.right - 1);
            srcPos.x = (state_.dockSide() == DockSide::Left) ? (winW - 1) : 0;
            winSize.cx = 1;
        }
    }

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = sliding
                                    ? static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(255.0f * animAlphaFactor_)), 1, 255))
                                    : 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, screenDc_, &dstPos, &winSize, memDc_, &srcPos, 0, &blend, ULW_ALPHA);
}

void FloatingTodoWindow::drawShadowedText(
    std::wstring_view text,
    IDWriteTextFormat* fmt,
    const D2D1_RECT_F& rect,
    D2D1_COLOR_F mainColor,
    ID2D1SolidColorBrush* brush) {
    if (text.empty() || !fmt || !brush) {
        return;
    }
    // 1.1dip 暗色柔影衬底，确保即使画布调至极高透明度，白色文字在浅色网页背景上依然锐利可读
    D2D1_RECT_F shadowRect = D2D1::RectF(rect.left + 0.6f, rect.top + 1.1f, rect.right + 0.6f, rect.bottom + 1.1f);
    brush->SetColor(hexColor(0x000000, 0.82f * mainColor.a));
    dcRenderTarget_->DrawTextW(
        text.data(),
        static_cast<UINT32>(text.size()),
        fmt,
        shadowRect,
        brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP);

    brush->SetColor(mainColor);
    dcRenderTarget_->DrawTextW(
        text.data(),
        static_cast<UINT32>(text.size()),
        fmt,
        rect,
        brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void FloatingTodoWindow::drawDockedStrip(const D2D1_SIZE_F& sizeDip) {
    headerBtns_.clear();
    rowVisuals_.clear();
    focusFirstPendingDocIdx_ = static_cast<std::size_t>(-1);

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dcRenderTarget_->CreateSolidColorBrush(hexColor(0x0F172A, 0.82f), &brush))) {
        return;
    }

    // 侧边耳标底板与状态色光条（支持左贴边与右贴边对称视觉）
    dcRenderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, sizeDip.width, sizeDip.height), 3.0f, 3.0f),
        brush);

    std::uint32_t accentHex = (doc_.pendingCount() > 0 || doc_.totalCount() == 0) ? 0x38BDF8 : 0x34D399;
    brush->SetColor(hexColor(accentHex, 0.95f));
    float barLeft = (state_.dockSide() == DockSide::Left) ? 1.5f : (sizeDip.width - 3.8f);
    dcRenderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(barLeft, 12.0f, barLeft + 2.3f, sizeDip.height - 12.0f), 1.1f, 1.1f),
        brush);

    brush->Release();
}

void FloatingTodoWindow::drawFocusBar(const D2D1_SIZE_F& sizeDip, float bgAlpha) {
    headerBtns_.clear();
    rowVisuals_.clear();
    logsBadgeRectDip_ = D2D1::RectF(0, 0, 0, 0);
    focusCheckRectDip_ = D2D1::RectF(0, 0, 0, 0);
    focusFirstPendingDocIdx_ = static_cast<std::size_t>(-1);

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dcRenderTarget_->CreateSolidColorBrush(hexColor(0x0F172A, bgAlpha), &brush))) {
        return;
    }

    D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, sizeDip.width - 0.5f, sizeDip.height - 0.5f), 8.0f, 8.0f);
    dcRenderTarget_->FillRoundedRectangle(pill, brush);

    brush->SetColor(hexColor(0xFFFFFF, mouseInside_ ? 0.20f : 0.10f));
    dcRenderTarget_->DrawRoundedRectangle(pill, brush, 1.0f);

    // 悬停且非穿透时在右侧展示展开按钮与贴边按钮
    float textRight = sizeDip.width - 10.0f;
    if (mouseInside_ && !state_.isClickThrough()) {
        D2D1_RECT_F expandR = D2D1::RectF(sizeDip.width - 26.0f, 4.0f, sizeDip.width - 4.0f, sizeDip.height - 4.0f);
        D2D1_RECT_F dockR = D2D1::RectF(sizeDip.width - 50.0f, 4.0f, sizeDip.width - 28.0f, sizeDip.height - 4.0f);
        headerBtns_.push_back(HeaderBtnVisual{HeaderBtnId::DockSideBtn, dockR});
        headerBtns_.push_back(HeaderBtnVisual{HeaderBtnId::CollapseVert, expandR});

        for (const HeaderBtnVisual& b : headerBtns_) {
            if (hoveredBtn_ == b.id) {
                brush->SetColor(hexColor(0xFFFFFF, 0.16f));
                dcRenderTarget_->FillRoundedRectangle(D2D1::RoundedRect(b.rectDip, 4.5f, 4.5f), brush);
            }
            brush->SetColor(hexColor(0xF8FAFC, hoveredBtn_ == b.id ? 1.0f : 0.80f));
            drawHeaderIcon(b.id, b.rectDip, brush);
        }
        textRight = dockR.left - 6.0f;
    }

    // 查找第一条未完成任务展示在单行 Focus Bar 中，一键勾选即跳下一条
    for (std::size_t i = 0; i < doc_.items().size(); ++i) {
        if (!doc_.items()[i].completed) {
            focusFirstPendingDocIdx_ = i;
            break;
        }
    }

    if (!toastMessage_.empty()) {
        drawShadowedText(toastMessage_, focusFormat_, D2D1::RectF(10.0f, 0.0f, textRight, sizeDip.height), hexColor(0x38BDF8), brush);
    } else if (focusFirstPendingDocIdx_ != static_cast<std::size_t>(-1)) {
        float cy = sizeDip.height * 0.5f;
        focusCheckRectDip_ = D2D1::RectF(10.0f, cy - 7.5f, 25.0f, cy + 7.5f);
        brush->SetColor(hexColor(hoveredFocusCheck_ ? 0x38BDF8 : 0xCBD5E1, 0.95f));
        dcRenderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(focusCheckRectDip_, 3.8f, 3.8f), brush, 1.5f);

        std::wstring line = utf8ToWide(doc_.items()[focusFirstPendingDocIdx_].text);
        if (doc_.pendingCount() > 1) {
            wchar_t tail[24]{};
            std::swprintf(tail, std::size(tail), L"  · 剩%zu", doc_.pendingCount());
            line += tail;
        }
        drawShadowedText(line, focusFormat_, D2D1::RectF(31.0f, 0.0f, textRight, sizeDip.height), hexColor(0xF8FAFC), brush);
    } else {
        std::wstring doneMsg = (doc_.totalCount() == 0) ? L"今日暂无待办 · 双击展开" : L"✓ 今日待办已全部完成";
        drawShadowedText(
            doneMsg,
            focusFormat_,
            D2D1::RectF(10.0f, 0.0f, textRight, sizeDip.height),
            hexColor(doc_.totalCount() == 0 ? 0xCBD5E1 : 0x34D399),
            brush);
    }

    brush->Release();
}

void FloatingTodoWindow::drawHeaderIcon(HeaderBtnId id, const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush) {
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;

    switch (id) {
        case HeaderBtnId::AddTask: {
            dcRenderTarget_->DrawLine(D2D1::Point2F(cx - 4.5f, cy), D2D1::Point2F(cx + 4.5f, cy), brush, 1.6f);
            dcRenderTarget_->DrawLine(D2D1::Point2F(cx, cy - 4.5f), D2D1::Point2F(cx, cy + 4.5f), brush, 1.6f);
            break;
        }
        case HeaderBtnId::CopyAi: {
            if (badgeFormat_) {
                dcRenderTarget_->DrawTextW(L"AI", 2, badgeFormat_, r, brush);
            }
            break;
        }
        case HeaderBtnId::Topmost: {
            dcRenderTarget_->DrawLine(D2D1::Point2F(cx - 3.5f, cy - 4.5f), D2D1::Point2F(cx + 3.5f, cy - 4.5f), brush, 1.4f);
            dcRenderTarget_->DrawRectangle(D2D1::RectF(cx - 2.4f, cy - 4.5f, cx + 2.4f, cy + 0.3f), brush, 1.3f);
            dcRenderTarget_->DrawLine(D2D1::Point2F(cx - 4.5f, cy + 0.6f), D2D1::Point2F(cx + 4.5f, cy + 0.6f), brush, 1.4f);
            dcRenderTarget_->DrawLine(D2D1::Point2F(cx, cy + 0.8f), D2D1::Point2F(cx, cy + 5.2f), brush, 1.4f);
            break;
        }
        case HeaderBtnId::ClickThrough: {
            D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(cx, cy), 4.8f, 4.8f);
            dcRenderTarget_->DrawEllipse(outer, brush, 1.3f);
            dcRenderTarget_->DrawLine(D2D1::Point2F(cx - 3.5f, cy + 3.5f), D2D1::Point2F(cx + 3.5f, cy - 3.5f), brush, 1.3f);
            break;
        }
        case HeaderBtnId::DockSideBtn: {
            // 根据当前靠近屏幕左侧或右侧，动态绘制向左 « 或向右 » 贴边箭头
            DockSide side = state_.preferredDockSide(currentMonitorWorkArea());
            if (side == DockSide::Left) {
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx + 0.2f, cy - 4.0f), D2D1::Point2F(cx - 3.8f, cy), brush, 1.4f);
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx - 3.8f, cy), D2D1::Point2F(cx + 0.2f, cy + 4.0f), brush, 1.4f);
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx + 3.8f, cy - 4.0f), D2D1::Point2F(cx - 0.2f, cy), brush, 1.4f);
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx - 0.2f, cy), D2D1::Point2F(cx + 3.8f, cy + 4.0f), brush, 1.4f);
            } else {
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx - 3.8f, cy - 4.0f), D2D1::Point2F(cx + 0.2f, cy), brush, 1.4f);
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx + 0.2f, cy), D2D1::Point2F(cx - 3.8f, cy + 4.0f), brush, 1.4f);
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx - 0.2f, cy - 4.0f), D2D1::Point2F(cx + 3.8f, cy), brush, 1.4f);
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx + 3.8f, cy), D2D1::Point2F(cx - 0.2f, cy + 4.0f), brush, 1.4f);
            }
            break;
        }
        case HeaderBtnId::CollapseVert: {
            if (state_.mode() == PanelMode::CollapsedVert) {
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx - 4.0f, cy - 1.8f), D2D1::Point2F(cx, cy + 2.2f), brush, 1.5f);
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx, cy + 2.2f), D2D1::Point2F(cx + 4.0f, cy - 1.8f), brush, 1.5f);
            } else {
                dcRenderTarget_->DrawLine(D2D1::Point2F(cx - 4.5f, cy), D2D1::Point2F(cx + 4.5f, cy), brush, 1.6f);
            }
            break;
        }
        case HeaderBtnId::None:
            break;
    }
}

void FloatingTodoWindow::drawExpandedHud(
    const D2D1_SIZE_F& sizeDip,
    float bgAlpha,
    const std::vector<MeasuredRow>& rows,
    float totalRowsDip) {
    headerBtns_.clear();
    rowVisuals_.clear();
    logsBadgeRectDip_ = D2D1::RectF(0, 0, 0, 0);
    inputBoxRectDip_ = D2D1::RectF(0, 0, 0, 0);
    focusCheckRectDip_ = D2D1::RectF(0, 0, 0, 0);
    focusFirstPendingDocIdx_ = static_cast<std::size_t>(-1);

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dcRenderTarget_->CreateSolidColorBrush(hexColor(0x0F172A, bgAlpha), &brush))) {
        return;
    }

    // 主半透暗色毛玻璃感圆角底板
    D2D1_ROUNDED_RECT mainCard = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, sizeDip.width - 0.5f, sizeDip.height - 0.5f),
        9.0f,
        9.0f);
    dcRenderTarget_->FillRoundedRectangle(mainCard, brush);

    // 悬停或激活输入时显现柔和微光边框；静置与穿透态仅保留极淡轮廓，消解画布压迫感
    float borderAlpha = state_.isClickThrough() ? 0.06f : ((mouseInside_ || inputActive_) ? 0.20f : 0.09f);
    brush->SetColor(hexColor(state_.isClickThrough() ? 0x7DD3FC : 0xFFFFFF, borderAlpha));
    dcRenderTarget_->DrawRoundedRectangle(mainCard, brush, 1.0f);

    const bool showChrome = (mouseInside_ || inputActive_) && !state_.isClickThrough();

    // 悬停交互态才浮现顶栏操作图标（Zero-Chrome 理念）
    if (showChrome) {
        const HeaderBtnId ids[6] = {
            HeaderBtnId::AddTask,
            HeaderBtnId::CopyAi,
            HeaderBtnId::Topmost,
            HeaderBtnId::ClickThrough,
            HeaderBtnId::DockSideBtn,
            HeaderBtnId::CollapseVert,
        };
        float rightEdge = sizeDip.width - 6.0f;
        for (int i = 5; i >= 0; --i) {
            float bw = (ids[i] == HeaderBtnId::CopyAi) ? 23.0f : 21.0f;
            D2D1_RECT_F r = D2D1::RectF(rightEdge - bw, 3.5f, rightEdge, kHeaderDip - 3.5f);
            headerBtns_.push_back(HeaderBtnVisual{ids[i], r});
            rightEdge -= (bw + 1.5f);
        }
        std::reverse(headerBtns_.begin(), headerBtns_.end());

        for (const HeaderBtnVisual& btn : headerBtns_) {
            bool active = (btn.id == HeaderBtnId::Topmost && state_.isTopmost()) ||
                          (btn.id == HeaderBtnId::AddTask && inputActive_);
            bool hov = (hoveredBtn_ == btn.id);
            if (active || hov) {
                brush->SetColor(hexColor(active ? 0x38BDF8 : 0xFFFFFF, active ? 0.25f : 0.15f));
                dcRenderTarget_->FillRoundedRectangle(D2D1::RoundedRect(btn.rectDip, 4.5f, 4.5f), brush);
            }
            brush->SetColor(hexColor(active ? 0x7DD3FC : 0xF8FAFC, (active || hov) ? 1.0f : 0.78f));
            drawHeaderIcon(btn.id, btn.rectDip, brush);
        }
    }

    // 顶栏左侧文字与点击打开日志的微标
    float headerRightLimit = headerBtns_.empty() ? (sizeDip.width - 10.0f) : (headerBtns_.front().rectDip.left - 4.0f);
    if (!toastMessage_.empty()) {
        drawShadowedText(
            toastMessage_,
            headerFormat_,
            D2D1::RectF(10.0f, 0.0f, headerRightLimit, kHeaderDip),
            hexColor(0x7DD3FC),
            brush);
    } else if (state_.isClickThrough()) {
        wchar_t ctBuf[64]{};
        std::swprintf(ctBuf, std::size(ctBuf), L"今日 %zu/%zu · 穿透 (Alt+T)", doc_.completedCount(), doc_.totalCount());
        drawShadowedText(
            ctBuf,
            headerFormat_,
            D2D1::RectF(10.0f, 0.0f, headerRightLimit, kHeaderDip),
            hexColor(0xBAE6FD, 0.88f),
            brush);
    } else {
        drawShadowedText(
            L"今日",
            headerFormat_,
            D2D1::RectF(10.0f, 0.0f, std::min(40.0f, headerRightLimit), kHeaderDip),
            hexColor(0xE2E8F0, showChrome ? 0.96f : 0.75f),
            brush);

        if (headerRightLimit > 86.0f) {
            wchar_t progBuf[40]{};
            if (showChrome) {
                std::swprintf(progBuf, std::size(progBuf), L"%zu/%zu 日志", doc_.completedCount(), doc_.totalCount());
            } else {
                std::swprintf(progBuf, std::size(progBuf), L"%zu/%zu", doc_.completedCount(), doc_.totalCount());
            }
            std::wstring badgeText = progBuf;
            float badgeW = showChrome ? std::min(58.0f, headerRightLimit - 40.0f) : 34.0f;
            logsBadgeRectDip_ = D2D1::RectF(39.0f, 5.0f, 39.0f + badgeW, kHeaderDip - 5.0f);
            if (showChrome) {
                brush->SetColor(hexColor(0xFFFFFF, hoveredLogsBadge_ ? 0.22f : 0.11f));
                dcRenderTarget_->FillRoundedRectangle(D2D1::RoundedRect(logsBadgeRectDip_, 4.0f, 4.0f), brush);
            }
            drawShadowedText(
                badgeText,
                badgeFormat_,
                logsBadgeRectDip_,
                hexColor(hoveredLogsBadge_ ? 0x7DD3FC : 0xCBD5E1, showChrome ? 0.95f : 0.70f),
                brush);
        }
    }

    // 待办列表视口区域
    float bottomUsed = (inputActive_ ? kInputAreaDip : 0.0f) + kBottomPadDip;
    listViewportDip_ = D2D1::RectF(0.0f, kHeaderDip, sizeDip.width, std::max(kHeaderDip + 2.0f, sizeDip.height - bottomUsed));
    float viewH = listViewportDip_.bottom - listViewportDip_.top;
    maxScrollDip_ = std::max(0.0f, totalRowsDip - viewH);
    scrollOffsetDip_ = std::clamp(scrollOffsetDip_, 0.0f, maxScrollDip_);

    dcRenderTarget_->PushAxisAlignedClip(listViewportDip_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (rows.empty()) {
        std::wstring emptyMsg = state_.isClickThrough() ? L"今日暂无待办" : L"暂无待办 · 按 Alt+Q 或点 + 添加";
        drawShadowedText(
            emptyMsg,
            headerFormat_,
            D2D1::RectF(12.0f, listViewportDip_.top, sizeDip.width - 12.0f, listViewportDip_.top + 30.0f),
            hexColor(0x94A3B8, 0.88f),
            brush);
    } else {
        float curY = listViewportDip_.top - scrollOffsetDip_;
        bool seenDone = false;
        bool hasPending = doc_.pendingCount() > 0;

        for (std::size_t vi = 0; vi < rows.size(); ++vi) {
            const MeasuredRow& mr = rows[vi];
            const TodoItem& item = doc_.items()[mr.docIndex];

            if (item.completed && !seenDone && hasPending) {
                seenDone = true;
                brush->SetColor(hexColor(0xFFFFFF, 0.08f));
                dcRenderTarget_->DrawLine(
                    D2D1::Point2F(12.0f, curY + 2.0f),
                    D2D1::Point2F(sizeDip.width - 12.0f, curY + 2.0f),
                    brush,
                    1.0f);
                curY += 4.0f;
            }

            float rh = mr.rowHeightDip;
            float rowBottom = curY + rh;
            D2D1_RECT_F checkR = D2D1::RectF(10.0f, curY + 7.0f, 25.0f, curY + 22.0f);
            D2D1_RECT_F delR = D2D1::RectF(sizeDip.width - 25.0f, curY + (rh - 18.0f) * 0.5f, sizeDip.width - 7.0f, curY + (rh + 18.0f) * 0.5f);
            rowVisuals_.push_back(RowVisual{mr.docIndex, curY, rowBottom, checkR, delR});

            if (rowBottom >= listViewportDip_.top && curY <= listViewportDip_.bottom) {
                bool rowHov = (!state_.isClickThrough() && static_cast<int>(vi) == hoveredVisualRow_);
                bool rowSel = (!state_.isClickThrough() && !inputActive_ && static_cast<int>(vi) == selectedVisualRow_);
                if (rowHov || rowSel) {
                    brush->SetColor(hexColor(rowSel ? 0x38BDF8 : 0xFFFFFF, rowSel ? 0.16f : 0.10f));
                    dcRenderTarget_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(5.0f, curY + 1.0f, sizeDip.width - 5.0f, rowBottom - 1.0f), 5.0f, 5.0f),
                        brush);
                }

                // 复选框
                D2D1_ROUNDED_RECT cbRound = D2D1::RoundedRect(checkR, 4.0f, 4.0f);
                if (item.completed) {
                    brush->SetColor(hexColor(0x38BDF8, 0.35f));
                    dcRenderTarget_->FillRoundedRectangle(cbRound, brush);
                    brush->SetColor(hexColor(0xE0F2FE, 0.92f));
                    dcRenderTarget_->DrawLine(
                        D2D1::Point2F(checkR.left + 3.5f, checkR.top + 7.6f),
                        D2D1::Point2F(checkR.left + 6.4f, checkR.top + 10.5f),
                        brush,
                        1.6f);
                    dcRenderTarget_->DrawLine(
                        D2D1::Point2F(checkR.left + 6.4f, checkR.top + 10.5f),
                        D2D1::Point2F(checkR.left + 11.6f, checkR.top + 4.5f),
                        brush,
                        1.6f);
                } else {
                    brush->SetColor(hexColor(0x000000, 0.35f));
                    dcRenderTarget_->FillRoundedRectangle(cbRound, brush);
                    brush->SetColor(hexColor((rowHov || rowSel) ? 0x7DD3FC : 0xE2E8F0, 0.92f));
                    dcRenderTarget_->DrawRoundedRectangle(cbRound, brush, 1.4f);
                }

                // 带微投影的高对比待办文本（底透字不透，始终清晰锐利）
                if (mr.layout) {
                    brush->SetColor(hexColor(0x000000, item.completed ? 0.65f : 0.88f));
                    dcRenderTarget_->DrawTextLayout(D2D1::Point2F(31.6f, curY + 5.1f), mr.layout, brush);

                    brush->SetColor(hexColor(item.completed ? 0x94A3B8 : 0xF8FAFC, item.completed ? 0.82f : 1.0f));
                    dcRenderTarget_->DrawTextLayout(D2D1::Point2F(31.0f, curY + 4.0f), mr.layout, brush);
                }

                // 悬停时右侧显现删除按钮 ×
                if (rowHov) {
                    if (hoveredRowDelete_) {
                        brush->SetColor(hexColor(0xEF4444, 0.32f));
                        dcRenderTarget_->FillRoundedRectangle(D2D1::RoundedRect(delR, 4.0f, 4.0f), brush);
                    }
                    brush->SetColor(hexColor(hoveredRowDelete_ ? 0xFCA5A5 : 0xCBD5E1, 0.95f));
                    float dcx = (delR.left + delR.right) * 0.5f;
                    float dcy = (delR.top + delR.bottom) * 0.5f;
                    dcRenderTarget_->DrawLine(D2D1::Point2F(dcx - 3.5f, dcy - 3.5f), D2D1::Point2F(dcx + 3.5f, dcy + 3.5f), brush, 1.4f);
                    dcRenderTarget_->DrawLine(D2D1::Point2F(dcx + 3.5f, dcy - 3.5f), D2D1::Point2F(dcx - 3.5f, dcy + 3.5f), brush, 1.4f);
                }
            }
            curY = rowBottom;
        }
    }

    if (maxScrollDip_ > 1.0f) {
        float ratio = viewH / totalRowsDip;
        float thumbH = std::max(16.0f, viewH * ratio);
        float thumbTop = listViewportDip_.top + (viewH - thumbH) * (scrollOffsetDip_ / maxScrollDip_);
        brush->SetColor(hexColor(0xFFFFFF, 0.28f));
        dcRenderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(sizeDip.width - 3.8f, thumbTop, sizeDip.width - 1.8f, thumbTop + thumbH), 1.0f, 1.0f),
            brush);
    }

    dcRenderTarget_->PopAxisAlignedClip();

    // 按需伸出的内联半透输入框（不录入时完全不占高度）
    if (inputActive_) {
        inputBoxRectDip_ = D2D1::RectF(8.0f, sizeDip.height - kBottomPadDip - 32.0f, sizeDip.width - 8.0f, sizeDip.height - kBottomPadDip);
        D2D1_ROUNDED_RECT inRound = D2D1::RoundedRect(inputBoxRectDip_, 6.0f, 6.0f);
        brush->SetColor(hexColor(0x020617, 0.68f));
        dcRenderTarget_->FillRoundedRectangle(inRound, brush);
        brush->SetColor(hexColor(0x38BDF8, 0.85f));
        dcRenderTarget_->DrawRoundedRectangle(inRound, brush, 1.3f);

        std::size_t caretPos = std::min(inputCaret_, inputBuffer_.size());
        std::wstring composed = inputBuffer_.substr(0, caretPos) + imeCompString_ + inputBuffer_.substr(caretPos);
        D2D1_RECT_F txtR = D2D1::RectF(inputBoxRectDip_.left + 8.0f, inputBoxRectDip_.top, inputBoxRectDip_.right - 8.0f, inputBoxRectDip_.bottom);

        if (composed.empty()) {
            drawShadowedText(L"输入待办，Enter 创建，Esc 收起", inputFormat_, txtR, hexColor(0x94A3B8, 0.85f), brush);
        } else {
            // 若文本超出输入框宽度，自动水平跟随光标尾部视窗
            std::size_t activeCaret = caretPos + imeCompString_.size();
            std::wstring visibleStr = composed;
            if (visibleStr.size() > 22 && activeCaret > 18) {
                std::size_t startCut = activeCaret - 18;
                visibleStr = visibleStr.substr(startCut);
                activeCaret -= startCut;
            }
            drawShadowedText(visibleStr, inputFormat_, txtR, hexColor(0xF8FAFC), brush);

            if (caretVisible_ && dwriteFactory_ && inputFormat_) {
                std::wstring prefix = visibleStr.substr(0, std::min(activeCaret, visibleStr.size()));
                IDWriteTextLayout* preLayout = nullptr;
                float caretX = txtR.left;
                if (!prefix.empty() &&
                    SUCCEEDED(dwriteFactory_->CreateTextLayout(
                        prefix.c_str(),
                        static_cast<UINT32>(prefix.size()),
                        inputFormat_,
                        1000.0f,
                        32.0f,
                        &preLayout)) &&
                    preLayout) {
                    DWRITE_TEXT_METRICS tm{};
                    if (SUCCEEDED(preLayout->GetMetrics(&tm))) {
                        caretX = std::min(txtR.right - 2.0f, txtR.left + tm.widthIncludingTrailingWhitespace + 1.0f);
                    }
                    preLayout->Release();
                }
                brush->SetColor(hexColor(0x38BDF8, 0.95f));
                dcRenderTarget_->DrawLine(
                    D2D1::Point2F(caretX, inputBoxRectDip_.top + 7.0f),
                    D2D1::Point2F(caretX, inputBoxRectDip_.bottom - 7.0f),
                    brush,
                    1.5f);
            }
        }
        if (composed.empty() && caretVisible_) {
            brush->SetColor(hexColor(0x38BDF8, 0.95f));
            dcRenderTarget_->DrawLine(
                D2D1::Point2F(txtR.left, inputBoxRectDip_.top + 7.0f),
                D2D1::Point2F(txtR.left, inputBoxRectDip_.bottom - 7.0f),
                brush,
                1.5f);
        }
    }

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
            D2D1_RECT_F expandedCheck = D2D1::RectF(0.0f, rv.topDip, rv.checkRectDip.right + 6.0f, rv.bottomDip);
            outOnCheckbox = pointInRect(ptDip, expandedCheck);
            outOnDelete = pointInRect(ptDip, rv.deleteRectDip);
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool FloatingTodoWindow::hitTestInputBox(D2D1_POINT_2F ptDip) const noexcept {
    return inputActive_ && pointInRect(ptDip, inputBoxRectDip_);
}

bool FloatingTodoWindow::hitTestLogsBadge(D2D1_POINT_2F ptDip) const noexcept {
    return pointInRect(ptDip, logsBadgeRectDip_);
}

bool FloatingTodoWindow::hitTestFocusCheckbox(D2D1_POINT_2F ptDip, std::size_t& outDocIndex) const noexcept {
    if (state_.mode() != PanelMode::CollapsedVert || focusFirstPendingDocIdx_ == static_cast<std::size_t>(-1)) {
        return false;
    }
    D2D1_RECT_F hitR = D2D1::RectF(
        focusCheckRectDip_.left - 4.0f,
        focusCheckRectDip_.top - 6.0f,
        focusCheckRectDip_.right + 6.0f,
        focusCheckRectDip_.bottom + 6.0f);
    if (pointInRect(ptDip, hitR)) {
        outDocIndex = focusFirstPendingDocIdx_;
        return true;
    }
    return false;
}

bool FloatingTodoWindow::isCursorInPeekRetentionZone() const noexcept {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return false;
    }
    ScreenRect keep = state_.peekRetentionBounds(currentMonitorWorkArea());
    RECT keepRc{keep.left, keep.top, keep.right, keep.bottom};
    return PtInRect(&keepRc, cursor) != FALSE;
}

void FloatingTodoWindow::updateImeCompositionWindow() {
    if (!hwnd_ || !inputActive_) {
        return;
    }
    HIMC himc = ImmGetContext(hwnd_);
    if (!himc) {
        return;
    }
    float scale = dpiScale();
    LONG pxX = static_cast<LONG>(std::lround((inputBoxRectDip_.left + 10.0f) * scale));
    LONG pxY = static_cast<LONG>(std::lround(inputBoxRectDip_.top * scale));

    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = POINT{pxX, pxY};
    ImmSetCompositionWindow(himc, &cf);

    CANDIDATEFORM cand{};
    cand.dwIndex = 0;
    cand.dwStyle = CFS_CANDIDATEPOS;
    cand.ptCurrentPos = POINT{pxX, static_cast<LONG>(std::lround(inputBoxRectDip_.bottom * scale))};
    ImmSetCandidateWindow(himc, &cand);

    ImmReleaseContext(hwnd_, himc);
}

void FloatingTodoWindow::beginInputEditing(bool openedViaQuickHotkey) {
    if (!hwnd_) {
        return;
    }
    if (state_.isClickThrough()) {
        state_.toggleClickThrough(currentMonitorWorkArea());
        updateTrayTooltip();
        quickHotkeyRestoreClickThrough_ = openedViaQuickHotkey;
    }
    bool openedFromStrip = false;
    if (state_.mode() == PanelMode::DockedHidden) {
        state_.onMouseEnterStrip();
        quickHotkeyReturnToHide_ = openedViaQuickHotkey;
        openedFromStrip = true;
    } else if (state_.mode() == PanelMode::CollapsedVert) {
        state_.toggleVerticalCollapse(currentMonitorWorkArea());
    }

    inputActive_ = true;
    caretVisible_ = true;
    inputCaret_ = inputBuffer_.size();
    SetTimer(hwnd_, TIMER_CARET_BLINK, 520, nullptr);
    syncAutoHeightAndPresent(openedFromStrip ? SlideTransition::SlideOpen : SlideTransition::Instant, true);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    updateImeCompositionWindow();
}

void FloatingTodoWindow::commitInputText() {
    if (inputBuffer_.empty()) {
        cancelInputEditing();
        return;
    }
    checkDayRollover();
    if (doc_.addItem(wideToUtf8(inputBuffer_))) {
        storage_.save(doc_);
        scrollOffsetDip_ = 0.0f; // 新增未完成项排在上方，自动对齐顶部视野
    }
    inputBuffer_.clear();
    inputCaret_ = 0;
    imeCompString_.clear();
    caretVisible_ = true;
    syncAutoHeightAndPresent(SlideTransition::Instant, false);
    updateImeCompositionWindow();
}

void FloatingTodoWindow::cancelInputEditing() {
    if (!inputActive_) {
        return;
    }
    inputActive_ = false;
    imeCompString_.clear();
    KillTimer(hwnd_, TIMER_CARET_BLINK);

    bool returningToHide = false;
    if (quickHotkeyReturnToHide_ && state_.mode() == PanelMode::DockedPeek) {
        quickHotkeyReturnToHide_ = false;
        returningToHide = state_.onMouseLeaveTimeout(false);
    }
    if (quickHotkeyRestoreClickThrough_ && !state_.isClickThrough()) {
        quickHotkeyRestoreClickThrough_ = false;
        state_.toggleClickThrough(currentMonitorWorkArea());
        updateTrayTooltip();
    }
    syncAutoHeightAndPresent(returningToHide ? SlideTransition::SlideCloseManual : SlideTransition::Instant, true);
    if (state_.mode() == PanelMode::DockedPeek && !state_.isClickThrough()) {
        POINT cursor{};
        RECT winRc{};
        if (GetCursorPos(&cursor) && GetWindowRect(hwnd_, &winRc) && !PtInRect(&winRc, cursor)) {
            SetTimer(hwnd_, TIMER_PEEK_HIDE, 220, nullptr);
        }
    }
}

void FloatingTodoWindow::pasteClipboardIntoInput() {
    if (!OpenClipboard(hwnd_)) {
        return;
    }
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const auto* wstr = static_cast<const wchar_t*>(GlobalLock(hData));
        if (wstr) {
            std::wstring clean;
            for (const wchar_t* p = wstr; *p != L'\0'; ++p) {
                if (*p == L'\r' || *p == L'\n' || *p == L'\t') {
                    clean.push_back(L' ');
                } else if (*p >= 0x20) {
                    clean.push_back(*p);
                }
            }
            GlobalUnlock(hData);
            if (!clean.empty()) {
                inputCaret_ = std::min(inputCaret_, inputBuffer_.size());
                inputBuffer_.insert(inputCaret_, clean);
                inputCaret_ += clean.size();
                caretVisible_ = true;
                renderAndPresent();
            }
        }
    }
    CloseClipboard();
}

void FloatingTodoWindow::handleInputKeyDown(WPARAM key) {
    bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    inputCaret_ = std::min(inputCaret_, inputBuffer_.size());

    if (ctrlDown && key == 'V') {
        pasteClipboardIntoInput();
        return;
    }
    if (ctrlDown && key == VK_BACK) {
        inputBuffer_.clear();
        inputCaret_ = 0;
        renderAndPresent();
        return;
    }

    switch (key) {
        case VK_RETURN:
            commitInputText();
            break;
        case VK_ESCAPE:
            inputBuffer_.clear();
            inputCaret_ = 0;
            cancelInputEditing();
            break;
        case VK_BACK:
            if (inputCaret_ > 0 && !inputBuffer_.empty()) {
                // 处理 UTF-16 代理对安全退格
                std::size_t eraseLen = 1;
                if (inputCaret_ >= 2) {
                    wchar_t low = inputBuffer_[inputCaret_ - 1];
                    wchar_t high = inputBuffer_[inputCaret_ - 2];
                    if (low >= 0xDC00 && low <= 0xDFFF && high >= 0xD800 && high <= 0xDBFF) {
                        eraseLen = 2;
                    }
                }
                inputBuffer_.erase(inputCaret_ - eraseLen, eraseLen);
                inputCaret_ -= eraseLen;
                caretVisible_ = true;
                renderAndPresent();
            }
            break;
        case VK_DELETE:
            if (inputCaret_ < inputBuffer_.size()) {
                inputBuffer_.erase(inputCaret_, 1);
                caretVisible_ = true;
                renderAndPresent();
            }
            break;
        case VK_LEFT:
            if (inputCaret_ > 0) {
                --inputCaret_;
                caretVisible_ = true;
                renderAndPresent();
            }
            break;
        case VK_RIGHT:
            if (inputCaret_ < inputBuffer_.size()) {
                ++inputCaret_;
                caretVisible_ = true;
                renderAndPresent();
            }
            break;
        case VK_HOME:
            inputCaret_ = 0;
            caretVisible_ = true;
            renderAndPresent();
            break;
        case VK_END:
            inputCaret_ = inputBuffer_.size();
            caretVisible_ = true;
            renderAndPresent();
            break;
        default:
            break;
    }
}

void FloatingTodoWindow::handlePanelShortcutKeyDown(WPARAM key) {
    bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int rowCount = static_cast<int>(rowVisuals_.size());

    if (ctrlDown && (key == VK_UP || key == VK_OEM_PLUS)) {
        int pct = static_cast<int>(std::lround(state_.adjustBgOpacity(0.08f) * 100.0f));
        wchar_t msg[32]{};
        std::swprintf(msg, std::size(msg), L"画布不透明度 %d%%", pct);
        showToast(msg, 900);
        return;
    }
    if (ctrlDown && (key == VK_DOWN || key == VK_OEM_MINUS)) {
        int pct = static_cast<int>(std::lround(state_.adjustBgOpacity(-0.08f) * 100.0f));
        wchar_t msg[32]{};
        std::swprintf(msg, std::size(msg), L"画布不透明度 %d%%", pct);
        showToast(msg, 900);
        return;
    }

    switch (key) {
        case VK_RETURN:
        case 'N':
            beginInputEditing(false);
            break;
        case VK_UP:
        case 'K':
            if (rowCount > 0) {
                selectedVisualRow_ = (selectedVisualRow_ <= 0) ? (rowCount - 1) : (selectedVisualRow_ - 1);
                renderAndPresent();
            }
            break;
        case VK_DOWN:
        case 'J':
            if (rowCount > 0) {
                selectedVisualRow_ = (selectedVisualRow_ < 0 || selectedVisualRow_ + 1 >= rowCount) ? 0 : (selectedVisualRow_ + 1);
                renderAndPresent();
            }
            break;
        case VK_SPACE:
            if (selectedVisualRow_ >= 0 && selectedVisualRow_ < rowCount) {
                checkDayRollover();
                std::size_t docIdx = rowVisuals_[static_cast<std::size_t>(selectedVisualRow_)].docIndex;
                if (doc_.toggleItem(docIdx, currentLocalTimeHHMM())) {
                    storage_.save(doc_);
                    syncAutoHeightAndPresent(SlideTransition::Instant, false);
                }
            }
            break;
        case VK_DELETE:
        case VK_BACK:
            if (selectedVisualRow_ >= 0 && selectedVisualRow_ < rowCount) {
                checkDayRollover();
                std::size_t docIdx = rowVisuals_[static_cast<std::size_t>(selectedVisualRow_)].docIndex;
                if (doc_.removeItem(docIdx)) {
                    storage_.save(doc_);
                    int remain = static_cast<int>(doc_.totalCount());
                    selectedVisualRow_ = (remain > 0) ? std::min(selectedVisualRow_, remain - 1) : -1;
                    syncAutoHeightAndPresent(SlideTransition::Instant, false);
                }
            }
            break;
        case VK_OEM_4: // '[' 贴向左侧边
            dockToSideAction(DockSide::Left);
            break;
        case VK_OEM_6: // ']' 贴向右侧边
            dockToSideAction(DockSide::Right);
            break;
        case VK_ESCAPE:
            if (state_.mode() == PanelMode::DockedPeek) {
                if (state_.onMouseLeaveTimeout(false)) {
                    mouseInside_ = false;
                    syncAutoHeightAndPresent(SlideTransition::SlideCloseManual, true);
                }
            } else if (selectedVisualRow_ >= 0) {
                selectedVisualRow_ = -1;
                renderAndPresent();
            }
            break;
        default:
            break;
    }
}

void FloatingTodoWindow::showToast(std::wstring msg, UINT durationMs) {
    toastMessage_ = std::move(msg);
    SetTimer(hwnd_, TIMER_TOAST_CLEAR, durationMs, nullptr);
    renderAndPresent();
}

void FloatingTodoWindow::toggleTopmostAction() {
    bool on = state_.toggleTopmost();
    applyExStyles(true);
    showToast(on ? L"已开启窗口置顶" : L"已取消置顶");
}

void FloatingTodoWindow::toggleClickThroughAction() {
    bool wasHidden = (state_.mode() == PanelMode::DockedHidden);
    state_.toggleClickThrough(currentMonitorWorkArea());
    hoveredBtn_ = HeaderBtnId::None;
    hoveredVisualRow_ = -1;
    hoveredLogsBadge_ = false;
    hoveredFocusCheck_ = false;
    selectedVisualRow_ = -1;
    mouseInside_ = false;
    toastMessage_.clear();
    SlideTransition trans = (wasHidden && state_.mode() == PanelMode::DockedPeek)
                                ? SlideTransition::SlideOpen
                                : SlideTransition::Instant;
    syncAutoHeightAndPresent(trans, true);
    updateTrayTooltip();
}

void FloatingTodoWindow::toggleAutoStartAction() {
    std::filesystem::path exePath = currentExecutablePath();
    bool targetEnabled = !isAutoStartEnabled(exePath);
    if (setAutoStartEnabled(targetEnabled, exePath)) {
        showToast(targetEnabled ? L"已开启开机自启" : L"已关闭开机自启");
    } else {
        showToast(L"修改开机自启失败");
    }
}

void FloatingTodoWindow::toggleCollapseAction() {
    state_.toggleVerticalCollapse(currentMonitorWorkArea());
    syncAutoHeightAndPresent(SlideTransition::Instant, false);
}

void FloatingTodoWindow::dockToSideAction(DockSide side) {
    state_.dockToSide(side, currentMonitorWorkArea());
    mouseInside_ = false;
    SlideTransition trans = (state_.mode() == PanelMode::DockedHidden)
                                ? SlideTransition::SlideCloseManual
                                : SlideTransition::Instant;
    syncAutoHeightAndPresent(trans, true);
}

void FloatingTodoWindow::summonOrCollapseAction() {
    ScreenRect wa = currentMonitorWorkArea();
    if (state_.mode() == PanelMode::DockedHidden) {
        state_.onMouseEnterStrip();
        syncAutoHeightAndPresent(SlideTransition::SlideOpen, true);
        if (!state_.isClickThrough()) {
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
        }
    } else if (state_.mode() == PanelMode::DockedPeek) {
        if (inputActive_) {
            cancelInputEditing();
        }
        if (state_.onMouseLeaveTimeout(false)) {
            mouseInside_ = false;
            syncAutoHeightAndPresent(SlideTransition::SlideCloseManual, true);
        }
    } else if (state_.mode() == PanelMode::CollapsedVert) {
        state_.toggleVerticalCollapse(wa);
        syncAutoHeightAndPresent(SlideTransition::Instant, true);
        if (!state_.isClickThrough()) {
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
        }
    } else {
        // 若当前已在展开态：若不在前台且非穿透则唤至前台聚焦，若已在前台则折叠成单行聚焦条
        if (!state_.isClickThrough() && GetForegroundWindow() != hwnd_) {
            applyExStyles(true);
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
            renderAndPresent();
        } else {
            state_.toggleVerticalCollapse(wa);
            syncAutoHeightAndPresent(SlideTransition::Instant, false);
        }
    }
}

void FloatingTodoWindow::quickAddHotkeyAction() {
    if (inputActive_ && GetForegroundWindow() == hwnd_) {
        cancelInputEditing();
        return;
    }
    beginInputEditing(true);
}

void FloatingTodoWindow::resetPositionAction() {
    ScreenRect wa = currentMonitorWorkArea();
    int targetX = std::max(wa.left + 20, wa.right - state_.metrics().panelWidth - 24);
    int targetY = wa.top + 72;
    state_.onDragFinished(targetX, targetY, wa);
    syncAutoHeightAndPresent(SlideTransition::Instant, true);
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
        showToast(L"已复制 AI 接入指令");
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
        syncAutoHeightAndPresent(SlideTransition::Instant, false);
    }
}

void FloatingTodoWindow::onExternalFileNotification() {
    std::string today = currentLocalDateString();
    if (doc_.date() != today) {
        doc_ = storage_.loadOrInit(today);
        syncAutoHeightAndPresent(SlideTransition::Instant, false);
        return;
    }
    TodoDocument updated;
    if (storage_.reloadIfExternallyModified(today, updated)) {
        doc_ = std::move(updated);
        syncAutoHeightAndPresent(SlideTransition::Instant, false);
    }
}

void FloatingTodoWindow::showContextMenu(POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, CMD_ADD_TASK, L"新建今日待办\tAlt+Q");
    AppendMenuW(menu, MF_STRING, CMD_TOGGLE_COLLAPSE, L"展开 / 单行聚焦切换\tAlt+`");
    AppendMenuW(
        menu,
        MF_STRING | (state_.isClickThrough() ? MF_CHECKED : 0),
        CMD_TOGGLE_CLICKTHROUGH,
        L"鼠标穿透 (不与鼠标交互)\tAlt+T");
    AppendMenuW(menu, MF_STRING | (state_.isTopmost() ? MF_CHECKED : 0), CMD_TOGGLE_TOPMOST, L"窗口置顶");
    bool autoStartOn = isAutoStartEnabled(currentExecutablePath());
    AppendMenuW(menu, MF_STRING | (autoStartOn ? MF_CHECKED : 0), CMD_TOGGLE_AUTOSTART, L"开机自动启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_DOCK_LEFT, L"贴靠屏幕左侧边\t[");
    AppendMenuW(menu, MF_STRING, CMD_DOCK_RIGHT, L"贴靠屏幕右侧边\t]");
    AppendMenuW(menu, MF_STRING, CMD_OPACITY_UP, L"增加背景不透明度\t顶栏滚轮↑");
    AppendMenuW(menu, MF_STRING, CMD_OPACITY_DOWN, L"降低背景不透明度\t顶栏滚轮↓");
    AppendMenuW(menu, MF_STRING, CMD_RESET_POS, L"展开并归位窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_COPY_AI, L"复制 AI 接入说明");
    AppendMenuW(menu, MF_STRING, CMD_OPEN_LOGS, L"打开每日日志目录 (logs)");
    AppendMenuW(menu, MF_STRING, CMD_OPEN_TODO, L"打开今日 todo.md");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"退出");

    contextMenuActive_ = true;
    SetForegroundWindow(hwnd_);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    contextMenuActive_ = false;

    switch (cmd) {
        case CMD_ADD_TASK:
            beginInputEditing(false);
            break;
        case CMD_TOGGLE_TOPMOST:
            toggleTopmostAction();
            break;
        case CMD_TOGGLE_CLICKTHROUGH:
            toggleClickThroughAction();
            break;
        case CMD_TOGGLE_AUTOSTART:
            toggleAutoStartAction();
            break;
        case CMD_DOCK_LEFT:
            dockToSideAction(DockSide::Left);
            break;
        case CMD_DOCK_RIGHT:
            dockToSideAction(DockSide::Right);
            break;
        case CMD_TOGGLE_COLLAPSE:
            toggleCollapseAction();
            break;
        case CMD_OPACITY_UP:
            state_.adjustBgOpacity(0.10f);
            renderAndPresent();
            break;
        case CMD_OPACITY_DOWN:
            state_.adjustBgOpacity(-0.10f);
            renderAndPresent();
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
    bi.bV5Height = -sz;
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
    std::uint32_t bg = clickThroughStyle ? 0xCC475569u : 0xFF0284C7u;
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            bool inside = (x >= 2 && x < sz - 2 && y >= 2 && y < sz - 2);
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
    for (int i = 0; i <= 5; ++i) putWhite(9 + i, 16 + i);
    for (int i = 0; i <= 9; ++i) putWhite(14 + i, 21 - i);

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
    std::wcsncpy(nid.szTip, L"今日待办 · Alt+Q 新建 | Alt+` 召唤 | Alt+T 穿透", std::size(nid.szTip) - 1);
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
                             ? L"今日待办 [穿透中] · 单击或按 Alt+T 恢复交互"
                             : L"今日待办 · Alt+Q 新建 | Alt+` 召唤 | Alt+T 穿透";
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

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            state_.setDpi(dpi_, currentMonitorWorkArea());
            syncAutoHeightAndPresent(SlideTransition::Instant, false);
            return 0;
        }

        case WM_HOTKEY: {
            int id = static_cast<int>(wParam);
            if (id == HOTKEY_ID_CLICKTHROUGH) {
                toggleClickThroughAction();
                return 0;
            }
            if (id == HOTKEY_ID_SUMMON) {
                summonOrCollapseAction();
                return 0;
            }
            if (id == HOTKEY_ID_QUICK_ADD) {
                quickAddHotkeyAction();
                return 0;
            }
            break;
        }

        case WM_APP_TODO_CHANGED: {
            onExternalFileNotification();
            return 0;
        }

        case WM_APP_REMOTE_SYNCED: {
            doc_ = storage_.loadOrInit(currentLocalDateString());
            syncAutoHeightAndPresent(SlideTransition::Instant, false);
            return 0;
        }

        case WM_APP_TRAY_EVENT: {
            if (lParam == WM_LBUTTONUP) {
                toggleClickThroughAction();
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                POINT pt{};
                GetCursorPos(&pt);
                showContextMenu(pt);
            }
            return 0;
        }

        case WM_TIMER: {
            if (wParam == TIMER_SLIDE_ANIM) {
                stepSlideAnimation();
                return 0;
            }
            if (wParam == TIMER_CARET_BLINK) {
                if (inputActive_) {
                    caretVisible_ = !caretVisible_;
                    renderAndPresent();
                }
                return 0;
            }
            if (wParam == TIMER_SHOWDESKTOP_GUARD) {
                KillTimer(hwnd_, TIMER_SHOWDESKTOP_GUARD);
                // Win + D 抬升桌面层后，重新将置顶悬浮窗或侧边耳标锚定至 Z 轴最顶层
                if (state_.isTopmost() || state_.mode() == PanelMode::DockedHidden || state_.mode() == PanelMode::DockedPeek) {
                    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
                return 0;
            }
            if (wParam == TIMER_PEEK_HIDE) {
                if (state_.mode() != PanelMode::DockedPeek || state_.isClickThrough()) {
                    KillTimer(hwnd_, TIMER_PEEK_HIDE);
                    return 0;
                }
                if (slideKind_ != SlideKind::None || draggingWindow_ || draggingStrip_ || inputActive_ || contextMenuActive_) {
                    return 0;
                }
                if (!isCursorInPeekRetentionZone()) {
                    KillTimer(hwnd_, TIMER_PEEK_HIDE);
                    if (state_.onMouseLeaveTimeout(inputActive_)) {
                        mouseInside_ = false;
                        hoveredBtn_ = HeaderBtnId::None;
                        hoveredVisualRow_ = -1;
                        hoveredRowDelete_ = false;
                        hoveredLogsBadge_ = false;
                        hoveredFocusCheck_ = false;
                        syncAutoHeightAndPresent(SlideTransition::SlideCloseAuto, true);
                    }
                }
                return 0;
            }
            if (wParam == TIMER_TOAST_CLEAR) {
                KillTimer(hwnd_, TIMER_TOAST_CLEAR);
                toastMessage_.clear();
                renderAndPresent();
                return 0;
            }
            if (wParam == TIMER_DAY_CHECK) {
                checkDayRollover();
                return 0;
            }
            break;
        }

        case WM_IME_STARTCOMPOSITION: {
            updateImeCompositionWindow();
            break;
        }

        case WM_IME_COMPOSITION: {
            if (inputActive_) {
                HIMC himc = ImmGetContext(hwnd_);
                if (himc) {
                    if (lParam & GCS_COMPSTR) {
                        LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
                        if (bytes > 0) {
                            imeCompString_.resize(static_cast<std::size_t>(bytes / sizeof(wchar_t)));
                            ImmGetCompositionStringW(himc, GCS_COMPSTR, imeCompString_.data(), static_cast<DWORD>(bytes));
                        } else {
                            imeCompString_.clear();
                        }
                        renderAndPresent();
                    }
                    if (lParam & GCS_RESULTSTR) {
                        imeCompString_.clear();
                    }
                    ImmReleaseContext(hwnd_, himc);
                }
            }
            break;
        }

        case WM_IME_ENDCOMPOSITION: {
            if (!imeCompString_.empty()) {
                imeCompString_.clear();
                renderAndPresent();
            }
            break;
        }

        case WM_KEYDOWN: {
            if (inputActive_) {
                handleInputKeyDown(wParam);
            } else {
                handlePanelShortcutKeyDown(wParam);
            }
            return 0;
        }

        case WM_CHAR: {
            if (inputActive_) {
                auto ch = static_cast<wchar_t>(wParam);
                if (ch >= 0x20 && ch != 0x7F) {
                    inputCaret_ = std::min(inputCaret_, inputBuffer_.size());
                    inputBuffer_.insert(inputCaret_, 1, ch);
                    ++inputCaret_;
                    caretVisible_ = true;
                    renderAndPresent();
                    updateImeCompositionWindow();
                }
                return 0;
            }
            break;
        }

        case WM_KILLFOCUS: {
            if (inputActive_ && inputBuffer_.empty() && imeCompString_.empty()) {
                cancelInputEditing();
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (slideKind_ == SlideKind::ClosingExplicit) {
                return 0;
            }

            if (!trackingMouseLeave_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd_;
                TrackMouseEvent(&tme);
                trackingMouseLeave_ = true;
            }

            if (draggingStrip_) {
                mouseInside_ = true;
                POINT cur{};
                GetCursorPos(&cur);
                int dy = cur.y - dragStartCursor_.y;
                if (std::abs(dy) > 2) {
                    dragMoved_ = true;
                }
                if (dragMoved_) {
                    ScreenRect wa = currentMonitorWorkArea();
                    int newTop = std::clamp<int>(
                        dragStartWinOrigin_.y + dy,
                        wa.top,
                        std::max(wa.top, wa.bottom - state_.metrics().dockStripHeight));
                    SetWindowPos(hwnd_, nullptr, dragStartWinOrigin_.x, newTop, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    state_.setDockY(newTop - 8, wa);
                }
                return 0;
            }

            if (state_.mode() == PanelMode::DockedHidden) {
                if (slideKind_ == SlideKind::ClosingAutoHide && !isCursorInPeekRetentionZone()) {
                    return 0;
                }
                mouseInside_ = true;
                if (state_.onMouseEnterStrip()) {
                    syncAutoHeightAndPresent(SlideTransition::SlideOpen, true);
                    if (!state_.isClickThrough()) {
                        SetTimer(hwnd_, TIMER_PEEK_HIDE, 220, nullptr);
                    }
                }
                return 0;
            }

            bool needRepaint = !mouseInside_;
            mouseInside_ = true;

            if (state_.mode() == PanelMode::DockedPeek && slideKind_ == SlideKind::None) {
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
            bool newBadge = hitTestLogsBadge(pt);
            std::size_t dummyFocusIdx = 0;
            bool newFocusCheck = hitTestFocusCheckbox(pt, dummyFocusIdx);

            SetCursor(LoadCursorW(nullptr, hitTestInputBox(pt) ? IDC_IBEAM : IDC_ARROW));

            if (needRepaint || newBtn != hoveredBtn_ || newRow != hoveredVisualRow_ || newDel != hoveredRowDelete_ ||
                newBadge != hoveredLogsBadge_ || newFocusCheck != hoveredFocusCheck_) {
                hoveredBtn_ = newBtn;
                hoveredVisualRow_ = newRow;
                hoveredRowDelete_ = newDel;
                hoveredLogsBadge_ = newBadge;
                hoveredFocusCheck_ = newFocusCheck;
                renderAndPresent();
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            trackingMouseLeave_ = false;
            if (slideKind_ == SlideKind::ClosingAutoHide || slideKind_ == SlideKind::ClosingExplicit) {
                mouseInside_ = false;
                return 0;
            }

            bool hadSubHover = (hoveredBtn_ != HeaderBtnId::None || hoveredVisualRow_ != -1 ||
                                hoveredRowDelete_ || hoveredLogsBadge_ || hoveredFocusCheck_);
            hoveredBtn_ = HeaderBtnId::None;
            hoveredVisualRow_ = -1;
            hoveredRowDelete_ = false;
            hoveredLogsBadge_ = false;
            hoveredFocusCheck_ = false;

            if (state_.mode() == PanelMode::DockedPeek && !state_.isClickThrough()) {
                SetTimer(hwnd_, TIMER_PEEK_HIDE, 220, nullptr);
                // 光标仍位于边缘唤醒区或贴边间隙时保持悬停视觉与展开状态，消除原地反复激活与闪烁
                if (slideKind_ == SlideKind::Opening || isCursorInPeekRetentionZone()) {
                    if (hadSubHover && slideKind_ == SlideKind::None) {
                        renderAndPresent();
                    }
                    return 0;
                }
            }

            mouseInside_ = false;
            renderAndPresent();
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);

            if (state_.mode() == PanelMode::DockedHidden) {
                if (slideKind_ != SlideKind::None) {
                    if (state_.onMouseEnterStrip()) {
                        mouseInside_ = true;
                        syncAutoHeightAndPresent(SlideTransition::SlideOpen, true);
                    }
                    return 0;
                }
                draggingStrip_ = true;
                dragMoved_ = false;
                GetCursorPos(&dragStartCursor_);
                RECT wr{};
                GetWindowRect(hwnd_, &wr);
                dragStartWinOrigin_ = POINT{wr.left, wr.top};
                SetCapture(hwnd_);
                return 0;
            }

            D2D1_POINT_2F pt = clientPxToDip(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

            if (inputActive_ && !hitTestInputBox(pt) && inputBuffer_.empty() && imeCompString_.empty()) {
                cancelInputEditing();
            }

            if (hitTestHeaderBtn(pt) != HeaderBtnId::None || hitTestLogsBadge(pt)) {
                return 0;
            }

            std::size_t focusDocIdx = 0;
            if (hitTestFocusCheckbox(pt, focusDocIdx)) {
                checkDayRollover();
                if (doc_.toggleItem(focusDocIdx, currentLocalTimeHHMM())) {
                    storage_.save(doc_);
                    renderAndPresent();
                }
                return 0;
            }

            if (state_.mode() == PanelMode::CollapsedVert || pt.y <= kHeaderDip) {
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
                inputCaret_ = inputBuffer_.size();
                caretVisible_ = true;
                renderAndPresent();
                updateImeCompositionWindow();
                return 0;
            }

            bool onCheck = false;
            bool onDel = false;
            int vRow = hitTestRow(pt, onCheck, onDel);
            if (vRow >= 0 && static_cast<std::size_t>(vRow) < rowVisuals_.size()) {
                checkDayRollover();
                std::size_t docIdx = rowVisuals_[static_cast<std::size_t>(vRow)].docIndex;
                if (onDel) {
                    if (doc_.removeItem(docIdx)) {
                        storage_.save(doc_);
                        syncAutoHeightAndPresent(SlideTransition::Instant, false);
                    }
                } else {
                    if (doc_.toggleItem(docIdx, currentLocalTimeHHMM())) {
                        storage_.save(doc_);
                        syncAutoHeightAndPresent(SlideTransition::Instant, false);
                    }
                }
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (draggingStrip_) {
                draggingStrip_ = false;
                ReleaseCapture();
                if (!dragMoved_ && state_.onMouseEnterStrip()) {
                    syncAutoHeightAndPresent(SlideTransition::SlideOpen, true);
                    if (!state_.isClickThrough()) {
                        SetTimer(hwnd_, TIMER_PEEK_HIDE, 220, nullptr);
                    }
                }
                return 0;
            }

            if (draggingWindow_) {
                draggingWindow_ = false;
                ReleaseCapture();
                if (dragMoved_) {
                    RECT wr{};
                    GetWindowRect(hwnd_, &wr);
                    state_.onDragFinished(wr.left, wr.top, currentMonitorWorkArea());
                    SlideTransition trans = SlideTransition::Instant;
                    if (state_.mode() == PanelMode::DockedHidden) {
                        mouseInside_ = false;
                        trans = SlideTransition::SlideCloseManual;
                    }
                    syncAutoHeightAndPresent(trans, true);
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
                case HeaderBtnId::AddTask:
                    if (inputActive_) {
                        cancelInputEditing();
                    } else {
                        beginInputEditing(false);
                    }
                    return 0;
                case HeaderBtnId::CopyAi:
                    copyAiInstructionToClipboard();
                    return 0;
                case HeaderBtnId::Topmost:
                    toggleTopmostAction();
                    return 0;
                case HeaderBtnId::ClickThrough:
                    toggleClickThroughAction();
                    return 0;
                case HeaderBtnId::DockSideBtn:
                    dockToSideAction(state_.preferredDockSide(currentMonitorWorkArea()));
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
            if (hitTestHeaderBtn(pt) == HeaderBtnId::None && !hitTestLogsBadge(pt)) {
                if (state_.mode() == PanelMode::CollapsedVert || pt.y <= kHeaderDip) {
                    toggleCollapseAction();
                    return 0;
                }
                // 双击列表空白处也可顺手唤起新建输入栏
                if (doc_.totalCount() == 0) {
                    beginInputEditing(false);
                    return 0;
                }
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
            POINT screenPt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT clientPt = screenPt;
            ScreenToClient(hwnd_, &clientPt);
            D2D1_POINT_2F ptDip = clientPxToDip(clientPt.x, clientPt.y);

            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            bool ctrlDown = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;

            // 在顶栏滚动滚轮或按住 Ctrl + 滚轮：实时无级调节画布背景不透明度（文字始终 100% 清晰）
            if (ctrlDown || ptDip.y <= kHeaderDip || state_.mode() == PanelMode::CollapsedVert) {
                float step = (delta > 0) ? 0.06f : -0.06f;
                int pct = static_cast<int>(std::lround(state_.adjustBgOpacity(step) * 100.0f));
                wchar_t msg[32]{};
                std::swprintf(msg, std::size(msg), L"画布不透明度 %d%%", pct);
                showToast(msg, 850);
                return 0;
            }

            if ((state_.mode() == PanelMode::Expanded || state_.mode() == PanelMode::DockedPeek) && maxScrollDip_ > 0.5f) {
                scrollOffsetDip_ = std::clamp(scrollOffsetDip_ - static_cast<float>(delta) * 0.25f, 0.0f, maxScrollDip_);
                renderAndPresent();
                return 0;
            }
            break;
        }

        case WM_DESTROY: {
            storage_.stopWatching();
            if (fgEventHook_) {
                UnhookWinEvent(fgEventHook_);
                fgEventHook_ = nullptr;
            }
            unregisterGlobalHotkeys();
            removeTrayIcon();
            PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace todofloat
