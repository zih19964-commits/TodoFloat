#include "../src/model.hpp"
#include "../src/storage.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

int gFailures = 0;

void checkImpl(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "[FAIL] " << file << ":" << line << " -> " << expr << "\n";
        ++gFailures;
    }
}

#define CHECK(expr) checkImpl(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

void testMarkdownModel() {
    using namespace todofloat;

    // 带 UTF-8 BOM、不同勾选符号与完成时间戳的解析验证
    std::string raw = "\xEF\xBB\xBF# 2026-11-25\r\n"
                      "\r\n"
                      "- [ ] 核对接口参数\r\n"
                      "- [x] 09:30 完成架构设计\r\n"
                      "* [X] AI 直接勾选的任务\r\n";

    TodoDocument doc = TodoDocument::parse(raw, "2026-11-25");
    CHECK(doc.date() == "2026-11-25");
    CHECK(doc.totalCount() == 3);
    CHECK(doc.completedCount() == 2);
    CHECK(doc.pendingCount() == 1);

    CHECK(doc.items()[0].text == "核对接口参数");
    CHECK(!doc.items()[0].completed);
    CHECK(doc.items()[0].completedTime.empty());

    CHECK(doc.items()[1].text == "完成架构设计");
    CHECK(doc.items()[1].completed);
    CHECK(doc.items()[1].completedTime == "09:30");

    CHECK(doc.items()[2].text == "AI 直接勾选的任务");
    CHECK(doc.items()[2].completed);
    CHECK(doc.items()[2].completedTime.empty());

    // 新增、空白过滤、勾选切换与删除验证
    CHECK(!doc.addItem("   \r\n\t  "));
    CHECK(doc.addItem("  补充边界测试\n"));
    CHECK(doc.totalCount() == 4);
    CHECK(doc.items()[3].text == "补充边界测试");

    CHECK(doc.toggleItem(0, "14:20"));
    CHECK(doc.items()[0].completed);
    CHECK(doc.items()[0].completedTime == "14:20");

    CHECK(doc.toggleItem(0, "14:25"));
    CHECK(!doc.items()[0].completed);
    CHECK(doc.items()[0].completedTime.empty());

    CHECK(doc.removeItem(2));
    CHECK(doc.totalCount() == 3);
    CHECK(!doc.removeItem(99));

    // 序列化往返一致性验证
    std::string serialized = doc.serialize();
    TodoDocument reparsed = TodoDocument::parse(serialized, "2026-11-25");
    CHECK(reparsed.date() == doc.date());
    CHECK(reparsed.items() == doc.items());

    // 当日幂等与跨日清空验证
    CHECK(!reparsed.ensureDate("2026-11-25"));
    CHECK(reparsed.totalCount() == 3);
    CHECK(reparsed.ensureDate("2026-11-26"));
    CHECK(reparsed.date() == "2026-11-26");
    CHECK(reparsed.totalCount() == 0);
}

void testWindowStateMachine() {
    using namespace todofloat;

    // 模拟副屏位于主屏左侧（负坐标工作区）以验证多显示器贴边收敛性
    ScreenRect leftMonitor{-1920, 0, 0, 1080};
    WindowStateMachine sm;
    sm.setDpi(144, leftMonitor); // 150% DPI 缩放

    CHECK(sm.isTopmost());
    CHECK(!sm.isClickThrough());
    CHECK(sm.mode() == PanelMode::Expanded);
    CHECK(sm.metrics().panelWidth == 450);

    sm.setPosition(-600, 100, leftMonitor);
    ScreenRect expandedRect = sm.currentBounds(leftMonitor);
    CHECK(expandedRect.width() == sm.metrics().panelWidth);
    CHECK(expandedRect.height() == sm.metrics().expandedHeight);

    // 上下折叠与还原
    sm.toggleVerticalCollapse(leftMonitor);
    CHECK(sm.mode() == PanelMode::CollapsedVert);
    CHECK(sm.currentBounds(leftMonitor).height() == sm.metrics().titleBarHeight);

    sm.toggleVerticalCollapse(leftMonitor);
    CHECK(sm.mode() == PanelMode::Expanded);
    CHECK(sm.currentBounds(leftMonitor).height() == sm.metrics().expandedHeight);

    // 拖拽到屏幕左边缘释放，自动缩成紧贴左边界的窄条
    sm.onDragFinished(leftMonitor.left + 4, 220, leftMonitor);
    CHECK(sm.mode() == PanelMode::DockedHidden);
    ScreenRect stripRect = sm.currentBounds(leftMonitor);
    CHECK(stripRect.left == leftMonitor.left);
    CHECK(stripRect.width() == sm.metrics().dockStripWidth);

    // 鼠标触碰窄条弹出完整面板，输入框激活时移出保持弹出，失焦后移出自动收回
    CHECK(sm.onMouseEnterStrip());
    CHECK(sm.mode() == PanelMode::DockedPeek);
    CHECK(!sm.onMouseLeaveTimeout(true));
    CHECK(sm.mode() == PanelMode::DockedPeek);
    CHECK(sm.onMouseLeaveTimeout(false));
    CHECK(sm.mode() == PanelMode::DockedHidden);

    // 在左侧窄条隐藏态一键开启鼠标穿透：自动展开面板且不因鼠标离开收回
    CHECK(sm.toggleClickThrough(leftMonitor));
    CHECK(sm.isClickThrough());
    CHECK(sm.mode() == PanelMode::DockedPeek);
    CHECK(!sm.onMouseLeaveTimeout(false));

    // 再次一键切换恢复交互
    CHECK(!sm.toggleClickThrough(leftMonitor));
    CHECK(!sm.isClickThrough());
}

void testStorageAndRealtimeWatcher() {
    using namespace todofloat;

    std::filesystem::path sandbox =
        std::filesystem::temp_directory_path() / ("todofloat_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(sandbox);

    {
        TodoStorage storage(sandbox);

        // 1. 初始化昨日待办并验证同步写入 logs/2026-11-24.md
        TodoDocument day1 = storage.loadOrInit("2026-11-24");
        day1.addItem("昨日完成的任务");
        day1.addItem("昨日未完成的任务");
        day1.toggleItem(0, "18:00");
        CHECK(storage.save(day1));
        CHECK(std::filesystem::exists(storage.todoFilePath()));
        CHECK(std::filesystem::exists(storage.logFilePathFor("2026-11-24")));

        // 2. 跨入今日：验证昨日日志完整保留完成与未完成项，今日 todo.md 清空重置
        TodoDocument day2 = storage.loadOrInit("2026-11-25");
        CHECK(day2.date() == "2026-11-25");
        CHECK(day2.totalCount() == 0);

        std::ifstream logIn(storage.logFilePathFor("2026-11-24"), std::ios::binary);
        std::string day1Log((std::istreambuf_iterator<char>(logIn)), std::istreambuf_iterator<char>());
        TodoDocument archived = TodoDocument::parse(day1Log, "2026-11-24");
        CHECK(archived.totalCount() == 2);
        CHECK(archived.completedCount() == 1);
        CHECK(archived.pendingCount() == 1);

        // 3. 启动内核目录监听，模拟 AI 直接向 todo.md 写入新任务与完成状态
        std::atomic<int> notifyCount{0};
        CHECK(storage.startWatching([&notifyCount]() { notifyCount.fetch_add(1); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));

        {
            std::ofstream aiOut(storage.todoFilePath(), std::ios::binary | std::ios::trunc);
            aiOut << "# 2026-11-25\n\n- [ ] AI 自动创建的待办\n- [x] AI 自动完成的待办\n";
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (notifyCount.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(notifyCount.load() >= 1);

        TodoDocument reloaded;
        CHECK(storage.reloadIfExternallyModified("2026-11-25", reloaded));
        CHECK(reloaded.totalCount() == 2);
        CHECK(reloaded.completedCount() == 1);
        CHECK(reloaded.items()[0].text == "AI 自动创建的待办");

        // 内容未变时重复调用返回 false，过滤自身落盘回环
        TodoDocument dummy;
        CHECK(!storage.reloadIfExternallyModified("2026-11-25", dummy));

        storage.stopWatching();
    }

    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
}

} // namespace

int main() {
    testMarkdownModel();
    testWindowStateMachine();
    testStorageAndRealtimeWatcher();

    if (gFailures == 0) {
        std::cout << "[PASS] All core unit and integration tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << "[FAIL] Total failures: " << gFailures << "\n";
    return EXIT_FAILURE;
}
