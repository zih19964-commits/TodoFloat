#include "../src/model.hpp"
#include "../src/startup.hpp"
#include "../src/storage.hpp"
#include "../src/sync_port.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

void checkImpl(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "[FAIL] " << file << ":" << line << " -> " << expr << "\n";
        ++gFailures;
    }
}

#define CHECK(expr) checkImpl(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

void testMarkdownAndDisplayOrder() {
    using namespace todofloat;

    std::string raw = "\xEF\xBB\xBF# 2026-11-25\r\n"
                      "\r\n"
                      "- [x] 09:30 完成架构设计\r\n"
                      "- [ ] 核对接口参数\r\n"
                      "* [ ] 联调蓝牙特征同步\r\n";

    TodoDocument doc = TodoDocument::parse(raw, "2026-11-25");
    CHECK(doc.date() == "2026-11-25");
    CHECK(doc.totalCount() == 3);
    CHECK(doc.completedCount() == 1);
    CHECK(doc.pendingCount() == 2);

    // 验证未完成事项自动排在上方、已完成沉降在下方
    std::vector<std::size_t> order = doc.displayOrderIndices();
    CHECK(order.size() == 3);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
    CHECK(order[2] == 0);

    CHECK(!doc.addItem("   \r\n\t  "));
    CHECK(doc.addItem("  补充边界测试\n"));
    CHECK(doc.totalCount() == 4);

    CHECK(doc.toggleItem(1, "14:20"));
    CHECK(doc.items()[1].completed);
    CHECK(doc.items()[1].completedTime == "14:20");

    std::string serialized = doc.serialize();
    TodoDocument reparsed = TodoDocument::parse(serialized, "2026-11-25");
    CHECK(reparsed.items() == doc.items());

    CHECK(reparsed.ensureDate("2026-11-26"));
    CHECK(reparsed.date() == "2026-11-26");
    CHECK(reparsed.totalCount() == 0);
}

void testAutoHeightAndDualEdgeDocking() {
    using namespace todofloat;

    ScreenRect monitor{-1920, 0, 0, 1080};
    WindowStateMachine sm;
    sm.setDpi(144, monitor); // 150% DPI 缩放

    CHECK(sm.isTopmost());
    CHECK(!sm.isClickThrough());
    CHECK(sm.mode() == PanelMode::Expanded);

    // 验证单条待办时的紧致自适应高度（消除多余空白画布）
    sm.setContentHeightPx(96, monitor);
    sm.setPosition(-500, 100, monitor);
    ScreenRect compactBounds = sm.currentBounds(monitor);
    CHECK(compactBounds.height() == 96);
    CHECK(sm.preferredDockSide(monitor) == DockSide::Right);

    // 验证超出屏幕比例上限时自动封顶转内部滚动
    sm.setContentHeightPx(2000, monitor);
    CHECK(sm.currentBounds(monitor).height() <= (monitor.height() * 58) / 100);
    sm.setContentHeightPx(110, monitor);

    // 验证背景不透明度调节范围约束 [0.15, 0.95]
    CHECK(sm.adjustBgOpacity(-2.0f) >= 0.15f);
    CHECK(sm.adjustBgOpacity(2.0f) <= 0.95f);

    // 单行 Focus Bar 折叠与展开还原
    sm.toggleVerticalCollapse(monitor);
    CHECK(sm.mode() == PanelMode::CollapsedVert);
    CHECK(sm.currentBounds(monitor).height() == sm.metrics().focusBarHeight);
    sm.toggleVerticalCollapse(monitor);
    CHECK(sm.mode() == PanelMode::Expanded);

    // 1. 拖拽至左边缘释放 -> 左侧贴边隐藏，且唤醒后保持热区完整覆盖原左耳标与贴边间隙
    sm.setContentHeightPx(sm.metrics().minExpandedHeight, monitor);
    sm.onDragFinished(monitor.left + 4, 200, monitor);
    CHECK(sm.mode() == PanelMode::DockedHidden);
    CHECK(sm.dockSide() == DockSide::Left);
    ScreenRect leftStrip = sm.currentBounds(monitor);
    CHECK(leftStrip.left == monitor.left);
    CHECK(leftStrip.width() == sm.metrics().dockStripWidth);

    CHECK(sm.onMouseEnterStrip());
    ScreenRect leftPeek = sm.currentBounds(monitor);
    ScreenRect leftKeep = sm.peekRetentionBounds(monitor);
    CHECK(leftKeep.left <= leftStrip.left);
    CHECK(leftKeep.right >= leftPeek.right);
    CHECK(leftKeep.top <= leftStrip.top && leftKeep.top <= leftPeek.top);
    CHECK(leftKeep.bottom >= leftStrip.bottom && leftKeep.bottom >= leftPeek.bottom);
    CHECK(sm.onMouseLeaveTimeout(false));

    // 2. 拖拽至右边缘释放 -> 右侧贴边隐藏，且隐藏态即可直接查询侧边展开目标矩形供双向侧滑动画使用
    int nearRightX = monitor.right - sm.metrics().panelWidth - 5;
    sm.onDragFinished(nearRightX, 240, monitor);
    CHECK(sm.mode() == PanelMode::DockedHidden);
    CHECK(sm.dockSide() == DockSide::Right);
    ScreenRect rightStrip = sm.currentBounds(monitor);
    ScreenRect precalculatedRightPeek = sm.dockedPeekBounds(monitor);
    CHECK(rightStrip.right == monitor.right);
    CHECK(rightStrip.width() == sm.metrics().dockStripWidth);
    CHECK(precalculatedRightPeek.right == monitor.right - sm.metrics().dockPeekGap);

    // 触碰右侧耳标滑出面板，定位在右边界内侧，同时保持热区覆盖整个右耳标激活位
    CHECK(sm.onMouseEnterStrip());
    CHECK(sm.mode() == PanelMode::DockedPeek);
    ScreenRect rightPeek = sm.currentBounds(monitor);
    ScreenRect rightKeep = sm.peekRetentionBounds(monitor);
    CHECK(rightPeek == precalculatedRightPeek);
    CHECK(rightKeep.left == rightPeek.left);
    CHECK(rightKeep.right == monitor.right);
    CHECK(rightKeep.top <= rightStrip.top);
    CHECK(rightKeep.bottom >= rightStrip.bottom);

    // 输入框激活时保持弹出，失焦后自动滑回右侧耳标
    CHECK(!sm.onMouseLeaveTimeout(true));
    CHECK(sm.onMouseLeaveTimeout(false));
    CHECK(sm.mode() == PanelMode::DockedHidden);

    // 在右侧耳标隐藏态开启鼠标穿透时自动弹出清单
    CHECK(sm.toggleClickThrough(monitor));
    CHECK(sm.mode() == PanelMode::DockedPeek);
    CHECK(!sm.onMouseLeaveTimeout(false));
}

class MockHarmonySyncTransport final : public todofloat::ISyncTransport {
public:
    bool start(RemoteReceiveHandler onRemoteSnapshot) override {
        handler_ = std::move(onRemoteSnapshot);
        started_ = true;
        return true;
    }

    void stop() override { started_ = false; }

    void publishLocalSnapshot(const todofloat::SyncSnapshot& snapshot) override {
        lastPublished_ = snapshot;
        ++publishCount_;
    }

    void simulateIncomingFromHuawei(const todofloat::SyncSnapshot& snap) {
        if (started_ && handler_) {
            handler_(snap);
        }
    }

    bool started_ = false;
    int publishCount_ = 0;
    todofloat::SyncSnapshot lastPublished_{};
    RemoteReceiveHandler handler_;
};

void testStorageWatcherAndHarmonySyncPort() {
    using namespace todofloat;

    std::filesystem::path sandbox =
        std::filesystem::temp_directory_path() /
        ("todofloat_v2_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(sandbox);

    {
        TodoStorage storage(sandbox);
        MockHarmonySyncTransport mockSync;
        std::atomic<int> remoteAppliedCount{0};

        storage.setSyncTransport(&mockSync, [&remoteAppliedCount]() { remoteAppliedCount.fetch_add(1); });
        CHECK(mockSync.started_);

        std::string today = currentLocalDateString();
        TodoDocument doc = storage.loadOrInit(today);
        doc.addItem("PC 端创建的待办");
        CHECK(storage.save(doc));
        CHECK(mockSync.publishCount_ >= 1);
        CHECK(mockSync.lastPublished_.date == today);

        // 模拟华为 HarmonyOS 设备发来同日快照（同时包含勾选 PC 条目与华为端离线新增条目）
        SyncSnapshot huaweiSnap{};
        huaweiSnap.date = today;
        huaweiSnap.markdownPayload = "# " + today + "\n\n- [x] 18:45 PC 端创建的待办\n- [ ] 华为手机端新增的待办\n";
        huaweiSnap.updatedAtMs = mockSync.lastPublished_.updatedAtMs + 500;
        huaweiSnap.revision = 1;
        huaweiSnap.deviceId = "huawei-mate";

        mockSync.simulateIncomingFromHuawei(huaweiSnap);
        CHECK(remoteAppliedCount.load() == 1);

        std::ifstream todoIn(storage.todoFilePath(), std::ios::binary);
        std::string mergedDisk((std::istreambuf_iterator<char>(todoIn)), std::istreambuf_iterator<char>());
        TodoDocument verifyDoc = TodoDocument::parse(mergedDisk, today);
        CHECK(verifyDoc.totalCount() == 2);
        CHECK(verifyDoc.completedCount() == 1);
        CHECK(verifyDoc.items()[0].text == "PC 端创建的待办");
        CHECK(verifyDoc.items()[0].completed);
        CHECK(verifyDoc.items()[1].text == "华为手机端新增的待办");
        CHECK(!verifyDoc.items()[1].completed);

        // 验证内核文件监听
        std::atomic<int> notifyCount{0};
        CHECK(storage.startWatching([&notifyCount]() { notifyCount.fetch_add(1); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));

        {
            std::ofstream aiOut(storage.todoFilePath(), std::ios::binary | std::ios::trunc);
            aiOut << "# " + today + "\n\n- [ ] AI 追加的第三条待办\n";
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (notifyCount.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(notifyCount.load() >= 1);

        storage.setSyncTransport(nullptr);
        storage.stopWatching();
    }

    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
}

void testAutoStartFormattingAndRegistry() {
    using namespace todofloat;

    std::filesystem::path exeA(LR"(E:\便签软件\TodoFloat.exe)");
    std::filesystem::path exeB(LR"(D:\Tools\TodoFloat.exe)");

    CHECK(formatStartupCommand(exeA) == LR"("E:\便签软件\TodoFloat.exe")");
    CHECK(startupCommandMatches(LR"("e:/便签软件/todofloat.exe")", exeA));
    CHECK(startupCommandMatches(LR"(E:\便签软件\TodoFloat.exe)", exeA));
    CHECK(!startupCommandMatches(LR"("D:\Tools\TodoFloat.exe")", exeA));
    CHECK(!startupCommandMatches(L"", exeA));

    // 使用独立临时注册表子键验证写入、读取、路径变更识别与删除
    std::wstring testSubKey = L"Software\\TodoFloat_UnitTest_" + std::to_wstring(GetCurrentProcessId());
    constexpr wchar_t testVal[] = L"TodoFloatTest";

    CHECK(!isAutoStartEnabled(exeA, testSubKey.c_str(), testVal));
    CHECK(setAutoStartEnabled(true, exeA, testSubKey.c_str(), testVal));
    CHECK(isAutoStartEnabled(exeA, testSubKey.c_str(), testVal));
    CHECK(!isAutoStartEnabled(exeB, testSubKey.c_str(), testVal));

    CHECK(setAutoStartEnabled(false, exeA, testSubKey.c_str(), testVal));
    CHECK(!isAutoStartEnabled(exeA, testSubKey.c_str(), testVal));
    // 重复关闭应保持幂等成功
    CHECK(setAutoStartEnabled(false, exeA, testSubKey.c_str(), testVal));

    RegDeleteTreeW(HKEY_CURRENT_USER, testSubKey.c_str());
}

} // namespace

int main() {
    testMarkdownAndDisplayOrder();
    testAutoHeightAndDualEdgeDocking();
    testStorageWatcherAndHarmonySyncPort();
    testAutoStartFormattingAndRegistry();

    if (gFailures == 0) {
        std::cout << "[PASS] All core, HUD geometry, and cross-device sync port tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << "[FAIL] Total failures: " << gFailures << "\n";
    return EXIT_FAILURE;
}
