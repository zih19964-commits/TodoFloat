#pragma once

#include "model.hpp"
#include "sync_port.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace todofloat {

// 负责 todo.md 与 logs/YYYY-MM-DD.md 的原子持久化、内核目录监听以及跨设备同步端口编排
class TodoStorage {
public:
    explicit TodoStorage(std::filesystem::path baseDir);
    ~TodoStorage();

    TodoStorage(const TodoStorage&) = delete;
    TodoStorage& operator=(const TodoStorage&) = delete;

    const std::filesystem::path& baseDir() const noexcept { return baseDir_; }
    const std::filesystem::path& todoFilePath() const noexcept { return todoPath_; }
    const std::filesystem::path& logsDirPath() const noexcept { return logsDir_; }
    std::filesystem::path logFilePathFor(std::string_view dateStr) const;

    TodoDocument loadOrInit(std::string_view todayDate);

    // 原子落盘 todo.md、同步镜像今日日志，并向已挂载的跨设备同步端口广播最新快照
    bool save(const TodoDocument& doc);

    // 当收到外部文件（如 AI）修改通知时重载最新内容，同时广播给跨设备同步端口
    bool reloadIfExternallyModified(std::string_view todayDate, TodoDocument& outDoc);

    // 挂载可选的远程同步传输实现（如后续的华为局域网 / BLE GATT 模块）
    void setSyncTransport(ISyncTransport* transport, std::function<void()> onRemoteApplied = {});

    // 应用来自远端设备的同步快照，合并落盘至本地 todo.md 与当日日志
    bool applyRemoteSnapshot(const SyncSnapshot& snapshot, std::string_view todayDate, TodoDocument& outMerged);

    std::uint64_t currentRevision() const noexcept;

    bool startWatching(std::function<void()> onExternalChange);
    void stopWatching();

private:
    static bool writeUtf8Atomic(const std::filesystem::path& targetPath, std::string_view content);
    static bool readUtf8File(const std::filesystem::path& targetPath, std::string& outContent);
    static std::uint64_t currentEpochMs() noexcept;

    void notifySyncTransport(const TodoDocument& doc, const std::string& serialized);
    void watchThreadMain(std::function<void()> onExternalChange);

    std::filesystem::path baseDir_;
    std::filesystem::path todoPath_;
    std::filesystem::path logsDir_;

    mutable std::mutex mutex_;
    std::string lastSavedSerialized_;
    std::uint64_t lastUpdatedMs_ = 0;
    std::uint64_t revision_ = 0;
    ISyncTransport* syncTransport_ = nullptr;
    std::function<void()> onRemoteApplied_;

    void* stopEvent_ = nullptr;
    std::thread watchThread_;
};

} // namespace todofloat
