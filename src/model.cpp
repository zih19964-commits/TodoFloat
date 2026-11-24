#include "model.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace todofloat {

namespace {

std::string_view trimWhitespace(std::string_view sv) noexcept {
    while (!sv.empty() && static_cast<unsigned char>(sv.front()) <= 0x20) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && static_cast<unsigned char>(sv.back()) <= 0x20) {
        sv.remove_suffix(1);
    }
    return sv;
}

// 清理单行输入中的换行与控制字符，保留正常 UTF-8 字节与空格
std::string sanitizeSingleLine(std::string_view raw) {
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (unsigned char ch : raw) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            cleaned.push_back(' ');
        } else if (ch >= 0x20) {
            cleaned.push_back(static_cast<char>(ch));
        }
    }
    std::string_view trimmed = trimWhitespace(cleaned);
    return std::string(trimmed);
}

bool isValidDateToken(std::string_view s) noexcept {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

bool isLeadingHHMM(std::string_view s) noexcept {
    if (s.size() < 6 || s[2] != ':' || s[5] != ' ') {
        return false;
    }
    if (s[0] < '0' || s[0] > '2' || s[1] < '0' || s[1] > '9') {
        return false;
    }
    if (s[0] == '2' && s[1] > '3') {
        return false;
    }
    if (s[3] < '0' || s[3] > '5' || s[4] < '0' || s[4] > '9') {
        return false;
    }
    return true;
}

} // namespace

TodoDocument::TodoDocument(std::string dateStr) : date_(std::move(dateStr)) {}

std::size_t TodoDocument::completedCount() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(items_.begin(), items_.end(), [](const TodoItem& item) { return item.completed; }));
}

std::size_t TodoDocument::pendingCount() const noexcept {
    return items_.size() - completedCount();
}

bool TodoDocument::addItem(std::string_view rawText) {
    std::string clean = sanitizeSingleLine(rawText);
    if (clean.empty()) {
        return false;
    }
    items_.push_back(TodoItem{.text = std::move(clean), .completed = false, .completedTime = {}});
    return true;
}

bool TodoDocument::toggleItem(std::size_t index, std::string_view currentHHMM) {
    if (index >= items_.size()) {
        return false;
    }
    TodoItem& item = items_[index];
    item.completed = !item.completed;
    if (item.completed) {
        item.completedTime = std::string(trimWhitespace(currentHHMM));
    } else {
        item.completedTime.clear();
    }
    return true;
}

bool TodoDocument::removeItem(std::size_t index) {
    if (index >= items_.size()) {
        return false;
    }
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool TodoDocument::ensureDate(std::string_view todayDate) {
    if (date_ == todayDate) {
        return false;
    }
    date_ = std::string(todayDate);
    items_.clear();
    return true;
}

TodoDocument TodoDocument::parse(std::string_view markdown, std::string_view fallbackDate) {
    // 剥离可能由记事本写入的 UTF-8 BOM
    if (markdown.size() >= 3 &&
        static_cast<unsigned char>(markdown[0]) == 0xEF &&
        static_cast<unsigned char>(markdown[1]) == 0xBB &&
        static_cast<unsigned char>(markdown[2]) == 0xBF) {
        markdown.remove_prefix(3);
    }

    TodoDocument doc{std::string(fallbackDate)};
    bool headerParsed = false;

    std::size_t pos = 0;
    while (pos <= markdown.size()) {
        std::size_t lineEnd = markdown.find('\n', pos);
        std::string_view line = (lineEnd == std::string_view::npos)
                                    ? markdown.substr(pos)
                                    : markdown.substr(pos, lineEnd - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        std::string_view trimmed = trimWhitespace(line);
        if (!trimmed.empty()) {
            // 解析首行日期标题，例如 "# 2026-11-25"
            if (!headerParsed && trimmed.front() == '#') {
                std::string_view rest = trimWhitespace(trimmed.substr(1));
                while (!rest.empty() && rest.front() == '#') {
                    rest.remove_prefix(1);
                }
                rest = trimWhitespace(rest);
                if (isValidDateToken(rest.substr(0, std::min<std::size_t>(rest.size(), 10)))) {
                    doc.date_ = std::string(rest.substr(0, 10));
                    headerParsed = true;
                }
            } else if ((trimmed.front() == '-' || trimmed.front() == '*') && trimmed.size() >= 5) {
                // 解析标准 Markdown 任务项 "- [ ] 内容" 或 "- [x] 10:15 内容"
                std::string_view afterBullet = trimWhitespace(trimmed.substr(1));
                if (afterBullet.size() >= 3 && afterBullet[0] == '[' && afterBullet[2] == ']') {
                    char mark = afterBullet[1];
                    if (mark == ' ' || mark == 'x' || mark == 'X') {
                        bool done = (mark == 'x' || mark == 'X');
                        std::string_view body = trimWhitespace(afterBullet.substr(3));
                        std::string timeStr;
                        if (done && isLeadingHHMM(body)) {
                            timeStr = std::string(body.substr(0, 5));
                            body = trimWhitespace(body.substr(6));
                        }
                        std::string cleanText = sanitizeSingleLine(body);
                        if (!cleanText.empty()) {
                            doc.items_.push_back(TodoItem{
                                .text = std::move(cleanText),
                                .completed = done,
                                .completedTime = std::move(timeStr),
                            });
                        }
                    }
                }
            }
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        pos = lineEnd + 1;
    }

    return doc;
}

std::string TodoDocument::serialize() const {
    std::ostringstream oss;
    oss << "# " << date_ << "\n";
    if (!items_.empty()) {
        oss << "\n";
        for (const TodoItem& item : items_) {
            oss << "- [" << (item.completed ? 'x' : ' ') << "] ";
            if (item.completed && !item.completedTime.empty()) {
                oss << item.completedTime << ' ';
            }
            oss << item.text << "\n";
        }
    }
    return oss.str();
}

WindowStateMachine::Metrics WindowStateMachine::Metrics::fromDpi(unsigned int dpi) noexcept {
    double scale = (dpi == 0) ? 1.0 : (static_cast<double>(dpi) / 96.0);
    auto px = [scale](int logical) -> int {
        return std::max(1, static_cast<int>(std::lround(logical * scale)));
    };
    return Metrics{
        .panelWidth = px(300),
        .expandedHeight = px(390),
        .titleBarHeight = px(36),
        .dockStripWidth = std::max(5, px(6)),
        .dockStripHeight = px(108),
        .dockSnapThreshold = px(12),
        .dockPeekOffset = px(4),
    };
}

WindowStateMachine::WindowStateMachine() : metrics_(Metrics::fromDpi(96)) {}

int WindowStateMachine::clampY(int y, int height, const ScreenRect& workArea) noexcept {
    int maxTop = std::max(workArea.top, workArea.bottom - height);
    return std::clamp(y, workArea.top, maxTop);
}

void WindowStateMachine::setDpi(unsigned int dpi, const ScreenRect& workArea) noexcept {
    if (dpi == 0) {
        dpi = 96;
    }
    dpi_ = dpi;
    metrics_ = Metrics::fromDpi(dpi_);
    if (mode_ == PanelMode::DockedHidden || mode_ == PanelMode::DockedPeek) {
        dockWorkArea_ = workArea;
        dockY_ = clampY(dockY_, metrics_.expandedHeight, dockWorkArea_);
    }
}

void WindowStateMachine::setPosition(int x, int y, const ScreenRect& workArea) noexcept {
    normalX_ = std::clamp(x, workArea.left, std::max(workArea.left, workArea.right - metrics_.panelWidth));
    normalY_ = clampY(y, metrics_.titleBarHeight, workArea);
}

bool WindowStateMachine::toggleTopmost() noexcept {
    topmost_ = !topmost_;
    return topmost_;
}

bool WindowStateMachine::toggleClickThrough(const ScreenRect& workArea) noexcept {
    clickThrough_ = !clickThrough_;
    // 开启鼠标穿透时若处于左侧窄条隐藏态，自动弹出完整面板以便直接阅览当日待办
    if (clickThrough_ && mode_ == PanelMode::DockedHidden) {
        dockWorkArea_ = workArea;
        mode_ = PanelMode::DockedPeek;
    }
    return clickThrough_;
}

void WindowStateMachine::toggleVerticalCollapse(const ScreenRect& workArea) noexcept {
    if (mode_ == PanelMode::CollapsedVert) {
        mode_ = PanelMode::Expanded;
        normalY_ = clampY(normalY_, metrics_.expandedHeight, workArea);
    } else if (mode_ == PanelMode::Expanded) {
        mode_ = PanelMode::CollapsedVert;
    } else if (mode_ == PanelMode::DockedPeek) {
        // 从贴边弹出态折叠时转为自由折叠条，方便停靠在桌面
        normalX_ = dockWorkArea_.left + metrics_.dockPeekOffset;
        normalY_ = dockY_;
        mode_ = PanelMode::CollapsedVert;
    }
}

void WindowStateMachine::dockToLeft(const ScreenRect& workArea) noexcept {
    dockWorkArea_ = workArea;
    dockY_ = clampY(normalY_, metrics_.expandedHeight, dockWorkArea_);
    // 穿透模式下保持面板展开可见，交互模式下收拢为左侧窄条
    mode_ = clickThrough_ ? PanelMode::DockedPeek : PanelMode::DockedHidden;
}

void WindowStateMachine::onDragFinished(int currentX, int currentY, const ScreenRect& workArea) noexcept {
    if (currentX <= workArea.left + metrics_.dockSnapThreshold) {
        dockWorkArea_ = workArea;
        dockY_ = clampY(currentY, metrics_.expandedHeight, dockWorkArea_);
        mode_ = clickThrough_ ? PanelMode::DockedPeek : PanelMode::DockedHidden;
        return;
    }

    normalX_ = std::clamp(currentX, workArea.left, std::max(workArea.left, workArea.right - metrics_.panelWidth));
    int refHeight = (mode_ == PanelMode::CollapsedVert) ? metrics_.titleBarHeight : metrics_.expandedHeight;
    normalY_ = clampY(currentY, refHeight, workArea);
    if (mode_ == PanelMode::DockedHidden || mode_ == PanelMode::DockedPeek) {
        mode_ = PanelMode::Expanded;
    }
}

bool WindowStateMachine::onMouseEnterStrip() noexcept {
    if (mode_ == PanelMode::DockedHidden) {
        mode_ = PanelMode::DockedPeek;
        return true;
    }
    return false;
}

bool WindowStateMachine::onMouseLeaveTimeout(bool isEditingInput) noexcept {
    // 正在输入框打字或处于穿透阅览态时保持弹出，避免打断输入或遮蔽清单
    if (mode_ == PanelMode::DockedPeek && !isEditingInput && !clickThrough_) {
        mode_ = PanelMode::DockedHidden;
        return true;
    }
    return false;
}

ScreenRect WindowStateMachine::currentBounds(const ScreenRect& workArea) const noexcept {
    switch (mode_) {
        case PanelMode::Expanded: {
            int y = clampY(normalY_, metrics_.expandedHeight, workArea);
            return ScreenRect{
                .left = normalX_,
                .top = y,
                .right = normalX_ + metrics_.panelWidth,
                .bottom = y + metrics_.expandedHeight,
            };
        }
        case PanelMode::CollapsedVert: {
            int y = clampY(normalY_, metrics_.titleBarHeight, workArea);
            return ScreenRect{
                .left = normalX_,
                .top = y,
                .right = normalX_ + metrics_.panelWidth,
                .bottom = y + metrics_.titleBarHeight,
            };
        }
        case PanelMode::DockedHidden: {
            const ScreenRect& wa = (dockWorkArea_.width() > 0) ? dockWorkArea_ : workArea;
            // 窄条垂直对齐在面板顶部偏下处，视觉自然且易定位
            int stripTop = clampY(dockY_ + metrics_.titleBarHeight, metrics_.dockStripHeight, wa);
            return ScreenRect{
                .left = wa.left,
                .top = stripTop,
                .right = wa.left + metrics_.dockStripWidth,
                .bottom = stripTop + metrics_.dockStripHeight,
            };
        }
        case PanelMode::DockedPeek: {
            const ScreenRect& wa = (dockWorkArea_.width() > 0) ? dockWorkArea_ : workArea;
            int top = clampY(dockY_, metrics_.expandedHeight, wa);
            int left = wa.left + metrics_.dockPeekOffset;
            return ScreenRect{
                .left = left,
                .top = top,
                .right = left + metrics_.panelWidth,
                .bottom = top + metrics_.expandedHeight,
            };
        }
    }
    return ScreenRect{};
}

std::string currentLocalDateString() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &tt);
#else
    localtime_r(&tt, &localTm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y-%m-%d");
    return oss.str();
}

std::string currentLocalTimeHHMM() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &tt);
#else
    localtime_r(&tt, &localTm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&localTm, "%H:%M");
    return oss.str();
}

} // namespace todofloat
