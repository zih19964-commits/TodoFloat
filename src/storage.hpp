#pragma once

#include "model.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace todofloat {

// 负责 todo.md 与 logs/YYYY-MM-DD.md 的原子持久化及内核级异步目录监听
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

    // 启动或跨日检查时加载当日待办；若检测到旧日期残留，先定格保存昨日日志再初始化今日空白清单
    TodoDocument loadOrInit(std::string_view todayDate);

    // 原子落盘 todo.md 并同步镜像 logs/YYYY-MM-DD.md
    bool save(const TodoDocument& doc);

    // 当收到外部文件变更通知时，读取最新 todo.md（若内容确有外部修改则返回 true 并输出新文档）
    bool reloadIfExternallyModified(std::string_view todayDate, TodoDocument& outDoc);

    // 启动基于 ReadDirectoryChangesW 的零轮询后台监听线程，外部改动防抖后触发 onExternalChange
    bool startWatching(std::function<void()> onExternalChange);
    void stopWatching();

private:
    static bool writeUtf8Atomic(const std::filesystem::path& targetPath, std::string_view content);
    static bool readUtf8File(const std::filesystem::path& targetPath, std::string& outContent);

    void watchThreadMain(std::function<void()> onExternalChange);

    std::filesystem::path baseDir_;
    std::filesystem::path todoPath_;
    std::filesystem::path logsDir_;

    mutable std::mutex mutex_;
    std::string lastSavedSerialized_;

    void* stopEvent_ = nullptr; // Win32 HANDLE
    std::thread watchThread_;
};

} // namespace todofloat
