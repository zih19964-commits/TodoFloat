#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace todofloat {

// 单条当日待办事项
struct TodoItem {
    std::string text;
    bool completed = false;
    std::string completedTime; // 格式 HH:MM，未完成或外部未记录时间时为空

    bool operator==(const TodoItem&) const = default;
};

// 当日待办文档模型：负责标准 Markdown 解析、序列化与日清重置
class TodoDocument {
public:
    TodoDocument() = default;
    explicit TodoDocument(std::string dateStr);

    const std::string& date() const noexcept { return date_; }
    const std::vector<TodoItem>& items() const noexcept { return items_; }

    std::size_t totalCount() const noexcept { return items_.size(); }
    std::size_t completedCount() const noexcept;
    std::size_t pendingCount() const noexcept;

    // 新增待办（自动裁剪首尾空白与控制字符，空文本返回 false）
    bool addItem(std::string_view rawText);

    // 切换指定索引条目的完成状态
    bool toggleItem(std::size_t index, std::string_view currentHHMM);

    // 删除指定索引条目
    bool removeItem(std::size_t index);

    // 校验日期：若跨入新日期则清空条目并切换为今日日期，返回是否发生了重置
    bool ensureDate(std::string_view todayDate);

    // 从 UTF-8 Markdown 文本解析
    static TodoDocument parse(std::string_view markdown, std::string_view fallbackDate);

    // 序列化为标准 UTF-8 Markdown 文本
    std::string serialize() const;

private:
    std::string date_;
    std::vector<TodoItem> items_;
};

// 屏幕矩形区域（物理像素）
struct ScreenRect {
    int left = 0;
    int top = 0;
    int right = 1920;
    int bottom = 1080;

    int width() const noexcept { return right - left; }
    int height() const noexcept { return bottom - top; }
    bool operator==(const ScreenRect&) const = default;
};

// 悬浮窗四种几何形态
enum class PanelMode : std::uint8_t {
    Expanded,      // 自由展开态
    CollapsedVert, // 上下收缩态（单行标题栏）
    DockedHidden,  // 左侧贴边隐藏态（窄指示条）
    DockedPeek     // 左侧贴边悬停弹出态
};

// 悬浮窗几何与交互状态机（纯逻辑，支持独立单元测试）
class WindowStateMachine {
public:
    struct Metrics {
        int panelWidth = 300;
        int expandedHeight = 390;
        int titleBarHeight = 36;
        int dockStripWidth = 6;
        int dockStripHeight = 108;
        int dockSnapThreshold = 12;
        int dockPeekOffset = 4;

        static Metrics fromDpi(unsigned int dpi) noexcept;
    };

    WindowStateMachine();

    PanelMode mode() const noexcept { return mode_; }
    bool isTopmost() const noexcept { return topmost_; }
    bool isClickThrough() const noexcept { return clickThrough_; }

    void setDpi(unsigned int dpi, const ScreenRect& workArea) noexcept;
    void setPosition(int x, int y, const ScreenRect& workArea) noexcept;

    // 切换窗口置顶
    bool toggleTopmost() noexcept;

    // 一键切换鼠标穿透（若正处于左侧窄条隐藏态，自动弹出面板以便查看清单）
    bool toggleClickThrough(const ScreenRect& workArea) noexcept;

    // 上下折叠 / 展开切换
    void toggleVerticalCollapse(const ScreenRect& workArea) noexcept;

    // 主动贴向当前屏幕左边缘
    void dockToLeft(const ScreenRect& workArea) noexcept;

    // 拖拽结束时判定是否触发左边缘自动吸附
    void onDragFinished(int currentX, int currentY, const ScreenRect& workArea) noexcept;

    // 鼠标移入左侧窄条时触发弹出
    bool onMouseEnterStrip() noexcept;

    // 鼠标离开面板且未处于输入焦点态满防抖时间后触发收回
    bool onMouseLeaveTimeout(bool isEditingInput) noexcept;

    // 计算当前状态对应的目标窗口物理矩形
    ScreenRect currentBounds(const ScreenRect& workArea) const noexcept;

    const Metrics& metrics() const noexcept { return metrics_; }

private:
    static int clampY(int y, int height, const ScreenRect& workArea) noexcept;

    PanelMode mode_ = PanelMode::Expanded;
    bool topmost_ = true;
    bool clickThrough_ = false;
    unsigned int dpi_ = 96;
    Metrics metrics_{};
    int normalX_ = 120;
    int normalY_ = 120;
    int dockY_ = 180;
    ScreenRect dockWorkArea_{};
};

// 获取本地当前日期 YYYY-MM-DD 与时间 HH:MM
std::string currentLocalDateString();
std::string currentLocalTimeHHMM();

} // namespace todofloat
