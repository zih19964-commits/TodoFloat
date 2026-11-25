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

    // 返回按“未完成在前、已完成在后”排列的原始条目索引序列，提升清单扫视可读性
    std::vector<std::size_t> displayOrderIndices() const;

    // 新增待办（自动裁剪首尾空白与控制字符，空文本返回 false）
    bool addItem(std::string_view rawText);

    // 切换指定索引条目的完成状态
    bool toggleItem(std::size_t index, std::string_view currentHHMM);

    // 删除指定索引条目
    bool removeItem(std::size_t index);

    // 校验日期：若跨入新日期则清空条目并切换为今日日期，返回是否发生了重置
    bool ensureDate(std::string_view todayDate);

    // 与远端（如华为端）同日待办快照无损合并：保留双端各自新增条目，同文本条目按 preferRemoteStatus 对齐状态
    void mergeWithRemote(const TodoDocument& remote, bool preferRemoteStatus);

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

// 贴边方向：支持左边缘与右边缘双侧贴靠
enum class DockSide : std::uint8_t {
    Left,
    Right
};

// 悬浮窗四种几何形态
enum class PanelMode : std::uint8_t {
    Expanded,      // 自由展开态（高度随内容动态伸缩）
    CollapsedVert, // 单行聚焦胶囊态（Focus Bar，展示首要未完成事项）
    DockedHidden,  // 侧边隐藏窄耳标态（支持左侧或右侧）
    DockedPeek     // 侧边悬停滑出态（支持左侧或右侧）
};

// 悬浮窗几何、双侧贴边、自适应高度与透明度状态机（纯逻辑，支持独立单元测试）
class WindowStateMachine {
public:
    struct Metrics {
        int panelWidth = 260;
        int minExpandedHeight = 62;
        int maxExpandedHeight = 420;
        int focusBarHeight = 32;
        int dockStripWidth = 6;
        int dockStripHeight = 68;
        int dockSnapThreshold = 16;
        int dockPeekGap = 4;

        static Metrics fromDpi(unsigned int dpi) noexcept;
    };

    WindowStateMachine();

    PanelMode mode() const noexcept { return mode_; }
    DockSide dockSide() const noexcept { return dockSide_; }
    bool isTopmost() const noexcept { return topmost_; }
    bool isClickThrough() const noexcept { return clickThrough_; }
    float bgOpacity() const noexcept { return bgOpacity_; }

    void setDpi(unsigned int dpi, const ScreenRect& workArea) noexcept;
    void setPosition(int x, int y, const ScreenRect& workArea) noexcept;

    // 根据实际待办排版内容高度（物理像素）动态更新展开高度，消除多余空白画布
    void setContentHeightPx(int contentHeightPx, const ScreenRect& workArea) noexcept;
    int expandedHeightPx(const ScreenRect& workArea) const noexcept;

    // 调节画布背景不透明度（范围 0.15 ~ 0.95，文字始终保持 1.0 满清晰度）
    float adjustBgOpacity(float delta) noexcept;

    // 判断当前窗口中心更靠近屏幕左侧还是右侧，供快捷贴边按钮使用
    DockSide preferredDockSide(const ScreenRect& workArea) const noexcept;

    // 切换窗口置顶
    bool toggleTopmost() noexcept;

    // 一键切换鼠标穿透（若正处于侧边窄耳标隐藏态，自动弹出完整面板以便直接阅览）
    bool toggleClickThrough(const ScreenRect& workArea) noexcept;

    // 单行聚焦胶囊折叠 / 展开切换
    void toggleVerticalCollapse(const ScreenRect& workArea) noexcept;

    // 主动贴向屏幕左侧或右侧边缘
    void dockToSide(DockSide side, const ScreenRect& workArea) noexcept;

    // 拖拽结束时判定是否触发左边缘或右边缘自动贴靠吸附
    void onDragFinished(int currentX, int currentY, const ScreenRect& workArea) noexcept;

    // 侧边窄耳标上下拖动调整停靠纵向位置
    void setDockY(int y, const ScreenRect& workArea) noexcept;

    // 鼠标移入侧边窄耳标时触发滑出
    bool onMouseEnterStrip() noexcept;

    // 鼠标离开面板且未处于输入焦点态满防抖时间后触发滑回
    bool onMouseLeaveTimeout(bool isEditingInput) noexcept;

    // 计算当前状态对应的目标窗口物理矩形
    ScreenRect currentBounds(const ScreenRect& workArea) const noexcept;

    const Metrics& metrics() const noexcept { return metrics_; }

private:
    static int clampY(int y, int height, const ScreenRect& workArea) noexcept;

    PanelMode mode_ = PanelMode::Expanded;
    DockSide dockSide_ = DockSide::Right;
    bool topmost_ = true;
    bool clickThrough_ = false;
    float bgOpacity_ = 0.64f;
    unsigned int dpi_ = 96;
    Metrics metrics_{};
    int contentHeightPx_ = 64;
    int normalX_ = 120;
    int normalY_ = 96;
    int dockY_ = 160;
    ScreenRect dockWorkArea_{};
};

// 获取本地当前日期 YYYY-MM-DD 与时间 HH:MM
std::string currentLocalDateString();
std::string currentLocalTimeHHMM();

} // namespace todofloat
