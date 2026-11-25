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

#include <cstddef>
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
    std::function<void()> remoteRefreshCallback() const;

private:
    enum class HeaderBtnId : int {
        None = -1,
        AddTask = 0,
        CopyAi = 1,
        Topmost = 2,
        ClickThrough = 3,
        DockSideBtn = 4,
        CollapseVert = 5,
    };

    struct RowVisual {
        std::size_t docIndex = 0;
        float topDip = 0.0f;
        float bottomDip = 0.0f;
        D2D1_RECT_F checkRectDip{};
        D2D1_RECT_F deleteRectDip{};
    };

    struct HeaderBtnVisual {
        HeaderBtnId id = HeaderBtnId::None;
        D2D1_RECT_F rectDip{};
    };

    struct MeasuredRow {
        std::size_t docIndex = 0;
        IDWriteTextLayout* layout = nullptr;
        float rowHeightDip = 30.0f;
        std::wstring displayStr;
    };

    static LRESULT CALLBACK wndProcSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK winEventProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime);

    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void onForegroundWindowChanged(HWND fgHwnd);

    // 图形工厂与逐像素 Alpha 分层缓冲管理
    bool initGraphicsFactories();
    bool rebuildTextFormats();
    bool ensureLayeredSurface(int widthPx, int heightPx);
    void releaseLayeredSurface() noexcept;

    // 排版测量、自适应高度同步与逐像素分层呈现
    std::vector<MeasuredRow> measureItems(float panelWidthDip, float& outTotalRowsDip);
    void freeMeasuredRows(std::vector<MeasuredRow>& rows) noexcept;
    void syncAutoHeightAndPresent(bool animateSlide = false, bool updateZOrder = false);
    void renderAndPresent();

    // 各形态绘制
    void drawDockedStrip(const D2D1_SIZE_F& sizeDip);
    void drawFocusBar(const D2D1_SIZE_F& sizeDip, float bgAlpha);
    void drawExpandedHud(const D2D1_SIZE_F& sizeDip, float bgAlpha, const std::vector<MeasuredRow>& rows, float totalRowsDip);
    void drawShadowedText(
        std::wstring_view text,
        IDWriteTextFormat* fmt,
        const D2D1_RECT_F& rect,
        D2D1_COLOR_F mainColor,
        ID2D1SolidColorBrush* brush);
    void drawHeaderIcon(HeaderBtnId id, const D2D1_RECT_F& r, ID2D1SolidColorBrush* brush);

    // 窗口交互与动作控制
    ScreenRect currentMonitorWorkArea() const noexcept;
    float dpiScale() const noexcept { return static_cast<float>(dpi_) / 96.0f; }
    D2D1_POINT_2F clientPxToDip(int xPx, int yPx) const noexcept;

    void applyExStyles(bool updateZOrder);
    void startSlideAnimation(int fromX, int toX, int fixedY);
    void stepSlideAnimation();

    void toggleTopmostAction();
    void toggleClickThroughAction();
    void toggleCollapseAction();
    void dockToSideAction(DockSide side);
    void summonOrCollapseAction();
    void quickAddHotkeyAction();
    void resetPositionAction();
    void showToast(std::wstring msg, UINT durationMs = 1300);

    void copyAiInstructionToClipboard();
    void openLogsFolder();
    void openTodayTodoFile();

    // 自绘原生输入框与中文输入法（IME）光标跟随
    void beginInputEditing(bool openedViaQuickHotkey = false);
    void commitInputText();
    void cancelInputEditing();
    void updateImeCompositionWindow();
    void handleInputKeyDown(WPARAM key);
    void handlePanelShortcutKeyDown(WPARAM key);
    void pasteClipboardIntoInput();

    void checkDayRollover();
    void onExternalFileNotification();
    void showContextMenu(POINT screenPt);

    // 系统托盘与热键
    void registerGlobalHotkeys();
    void unregisterGlobalHotkeys();
    void initTrayIcon();
    void updateTrayTooltip();
    void removeTrayIcon();
    HICON createProceduralAppIcon(bool clickThroughStyle);

    // 命中测试
    HeaderBtnId hitTestHeaderBtn(D2D1_POINT_2F ptDip) const noexcept;
    int hitTestRow(D2D1_POINT_2F ptDip, bool& outOnCheckbox, bool& outOnDelete) const noexcept;
    bool hitTestInputBox(D2D1_POINT_2F ptDip) const noexcept;
    bool hitTestLogsBadge(D2D1_POINT_2F ptDip) const noexcept;
    bool hitTestFocusCheckbox(D2D1_POINT_2F ptDip, std::size_t& outDocIndex) const noexcept;

    static FloatingTodoWindow* sActiveInstance_;

    TodoStorage& storage_;
    TodoDocument doc_;
    WindowStateMachine state_;

    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWINEVENTHOOK fgEventHook_ = nullptr;
    HICON iconNormal_ = nullptr;
    HICON iconClickThrough_ = nullptr;
    bool trayAdded_ = false;

    // 逐像素分层窗口后备 32bpp 预乘 Alpha 位图与 DC
    HDC screenDc_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP dibBitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    int surfaceW_ = 0;
    int surfaceH_ = 0;

    unsigned int dpi_ = 96;
    float scrollOffsetDip_ = 0.0f;
    float maxScrollDip_ = 0.0f;

    // 侧滑动画状态
    bool animatingSlide_ = false;
    int animStartX_ = 0;
    int animTargetX_ = 0;
    int animCurrentX_ = 0;
    int animFixedY_ = 0;
    int animStep_ = 0;

    // 鼠标、悬停与键盘选中状态
    bool mouseInside_ = false;
    bool trackingMouseLeave_ = false;
    bool draggingWindow_ = false;
    bool draggingStrip_ = false;
    bool dragMoved_ = false;
    POINT dragStartCursor_{};
    POINT dragStartWinOrigin_{};

    HeaderBtnId hoveredBtn_ = HeaderBtnId::None;
    int hoveredVisualRow_ = -1;
    bool hoveredRowDelete_ = false;
    bool hoveredLogsBadge_ = false;
    bool hoveredFocusCheck_ = false;
    int selectedVisualRow_ = -1;

    // 内联输入与中文输入法组合态
    bool inputActive_ = false;
    bool caretVisible_ = true;
    bool quickHotkeyReturnToHide_ = false;
    bool quickHotkeyRestoreClickThrough_ = false;
    std::wstring inputBuffer_;
    std::size_t inputCaret_ = 0;
    std::wstring imeCompString_;

    std::wstring toastMessage_;

    // 当前帧几何热区（DIP 坐标）
    std::vector<HeaderBtnVisual> headerBtns_;
    std::vector<RowVisual> rowVisuals_;
    D2D1_RECT_F listViewportDip_{};
    D2D1_RECT_F inputBoxRectDip_{};
    D2D1_RECT_F logsBadgeRectDip_{};
    D2D1_RECT_F focusCheckRectDip_{};
    std::size_t focusFirstPendingDocIdx_ = static_cast<std::size_t>(-1);

    // Direct2D / DirectWrite 接口指针
    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    ID2D1DCRenderTarget* dcRenderTarget_ = nullptr;
    IDWriteTextFormat* headerFormat_ = nullptr;
    IDWriteTextFormat* badgeFormat_ = nullptr;
    IDWriteTextFormat* itemPendingFormat_ = nullptr;
    IDWriteTextFormat* itemDoneFormat_ = nullptr;
    IDWriteTextFormat* focusFormat_ = nullptr;
    IDWriteTextFormat* inputFormat_ = nullptr;
};

} // namespace todofloat
