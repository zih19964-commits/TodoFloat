#pragma once

#include "model.hpp"
#include "storage.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace todofloat {

class FloatingTodoWindow {
public:
    explicit FloatingTodoWindow(TodoStorage& storage);
    ~FloatingTodoWindow();

    FloatingTodoWindow(const FloatingTodoWindow&) = delete;
    FloatingTodoWindow& operator=(const FloatingTodoWindow&) = delete;

    bool create(HINSTANCE hInstance);
    HWND hwnd() const noexcept { return hwnd_; }

private:
    enum class HeaderBtnId : int {
        None = -1,
        CopyAi = 0,
        Topmost = 1,
        ClickThrough = 2,
        DockLeft = 3,
        CollapseVert = 4,
    };

    struct RowVisual {
        float topDip = 0.0f;
        float bottomDip = 0.0f;
        D2D1_RECT_F checkRectDip{};
        D2D1_RECT_F deleteRectDip{};
    };

    struct HeaderBtnVisual {
        HeaderBtnId id = HeaderBtnId::None;
        D2D1_RECT_F rectDip{};
    };

    static LRESULT CALLBACK wndProcSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK editSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // D2D / DWrite 设备与字体资源管理
    bool initGraphicsFactories();
    bool ensureRenderTarget();
    void discardRenderTarget() noexcept;
    bool rebuildTextFormats();
    void updateEditFontAndBounds();

    // 界面绘制
    void onPaint();
    void drawDockedStrip(const D2D1_SIZE_F& sizeDip);
    void drawMainPanel(const D2D1_SIZE_F& sizeDip);
    void drawHeaderIcon(HeaderBtnId id, const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush);

    // 交互与窗口控制
    ScreenRect currentMonitorWorkArea() const noexcept;
    float dpiScale() const noexcept { return static_cast<float>(dpi_) / 96.0f; }
    D2D1_POINT_2F clientPxToDip(int xPx, int yPx) const noexcept;

    void applyWindowStylesAndBounds(bool updateZOrder = false);
    void updateWindowCornerPreference();
    void toggleTopmostAction();
    void toggleClickThroughAction();
    void toggleCollapseAction();
    void dockLeftAction();
    void resetPositionAction();
    void copyAiInstructionToClipboard();
    void openLogsFolder();
    void openTodayTodoFile();

    void beginInputEditing();
    void commitInputText();
    void cancelInputEditing();
    bool isInputActive() const noexcept { return inputActive_; }

    void checkDayRollover();
    void onExternalFileNotification();
    void showContextMenu(POINT screenPt);

    // 系统托盘
    void initTrayIcon();
    void updateTrayTooltip();
    void removeTrayIcon();
    HICON createProceduralAppIcon(bool clickThroughStyle);

    // 命中测试辅助
    HeaderBtnId hitTestHeaderBtn(D2D1_POINT_2F ptDip) const noexcept;
    int hitTestRow(D2D1_POINT_2F ptDip, bool& outOnCheckbox, bool& outOnDelete) const noexcept;
    bool hitTestInputBox(D2D1_POINT_2F ptDip) const noexcept;
    bool hitTestLogsBadge(D2D1_POINT_2F ptDip) const noexcept;

    TodoStorage& storage_;
    TodoDocument doc_;
    WindowStateMachine state_;

    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND editHwnd_ = nullptr;
    WNDPROC origEditProc_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBgBrush_ = nullptr;
    HICON iconNormal_ = nullptr;
    HICON iconClickThrough_ = nullptr;
    bool trayAdded_ = false;
    bool hotkeyRegistered_ = false;

    unsigned int dpi_ = 96;
    float scrollOffsetDip_ = 0.0f;
    float maxScrollDip_ = 0.0f;

    // 鼠标拖拽与悬停状态
    bool draggingWindow_ = false;
    bool dragMoved_ = false;
    POINT dragStartCursor_{};
    POINT dragStartWinOrigin_{};
    bool trackingMouseLeave_ = false;
    HeaderBtnId hoveredBtn_ = HeaderBtnId::None;
    int hoveredRow_ = -1;
    bool hoveredRowDelete_ = false;
    bool hoveredInputBox_ = false;
    bool hoveredLogsBadge_ = false;
    bool inputActive_ = false;

    std::wstring toastMessage_;

    // 缓存的当前帧几何热区（DIP 坐标）
    std::vector<HeaderBtnVisual> headerBtns_;
    std::vector<RowVisual> rowVisuals_;
    D2D1_RECT_F listViewportDip_{};
    D2D1_RECT_F inputBoxRectDip_{};
    D2D1_RECT_F logsBadgeRectDip_{};

    // Direct2D / DirectWrite 接口指针
    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    ID2D1HwndRenderTarget* renderTarget_ = nullptr;
    IDWriteTextFormat* titleFormat_ = nullptr;
    IDWriteTextFormat* badgeFormat_ = nullptr;
    IDWriteTextFormat* itemFormat_ = nullptr;
    IDWriteTextFormat* hintFormat_ = nullptr;
};

} // namespace todofloat
