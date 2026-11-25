#include "storage.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cwctype>
#include <vector>

namespace todofloat {

namespace {

bool equalsIgnoreCaseWide(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

TodoStorage::TodoStorage(std::filesystem::path baseDir)
    : baseDir_(std::move(baseDir)),
      todoPath_(baseDir_ / L"todo.md"),
      logsDir_(baseDir_ / L"logs") {}

TodoStorage::~TodoStorage() {
    if (syncTransport_) {
        syncTransport_->stop();
        syncTransport_ = nullptr;
    }
    stopWatching();
}

std::filesystem::path TodoStorage::logFilePathFor(std::string_view dateStr) const {
    std::string fileName(dateStr);
    fileName += ".md";
    return logsDir_ / std::filesystem::path(fileName);
}

std::uint64_t TodoStorage::currentEpochMs() noexcept {
    auto now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

std::uint64_t TodoStorage::currentRevision() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

bool TodoStorage::writeUtf8Atomic(const std::filesystem::path& targetPath, std::string_view content) {
    std::error_code ec;
    if (targetPath.has_parent_path()) {
        std::filesystem::create_directories(targetPath.parent_path(), ec);
    }

    std::filesystem::path tmpPath = targetPath;
    tmpPath += L".tmp";

    HANDLE hFile = CreateFileW(
        tmpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    const char* ptr = content.data();
    std::size_t remaining = content.size();
    bool writeOk = true;
    while (remaining > 0) {
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(hFile, ptr, chunk, &written, nullptr) || written != chunk) {
            writeOk = false;
            break;
        }
        ptr += written;
        remaining -= written;
    }

    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    if (!writeOk) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    if (!MoveFileExW(tmpPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

bool TodoStorage::readUtf8File(const std::filesystem::path& targetPath, std::string& outContent) {
    HANDLE hFile = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        hFile = CreateFileW(
            targetPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            break;
        }
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        Sleep(15);
    }

    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 0 || fileSize.QuadPart > (4LL << 20)) {
        CloseHandle(hFile);
        return false;
    }

    outContent.resize(static_cast<std::size_t>(fileSize.QuadPart));
    DWORD totalRead = 0;
    bool ok = true;
    if (!outContent.empty()) {
        ok = ReadFile(hFile, outContent.data(), static_cast<DWORD>(outContent.size()), &totalRead, nullptr) != FALSE;
        outContent.resize(totalRead);
    }
    CloseHandle(hFile);
    return ok;
}

TodoDocument TodoStorage::loadOrInit(std::string_view todayDate) {
    std::error_code ec;
    std::filesystem::create_directories(logsDir_, ec);

    std::string raw;
    if (readUtf8File(todoPath_, raw)) {
        TodoDocument doc = TodoDocument::parse(raw, todayDate);
        if (doc.date() != todayDate) {
            writeUtf8Atomic(logFilePathFor(doc.date()), doc.serialize());
            doc.ensureDate(todayDate);
            save(doc);
            return doc;
        }
        save(doc);
        return doc;
    }

    std::filesystem::path todayLog = logFilePathFor(todayDate);
    if (readUtf8File(todayLog, raw)) {
        TodoDocument doc = TodoDocument::parse(raw, todayDate);
        doc.ensureDate(todayDate);
        save(doc);
        return doc;
    }

    TodoDocument fresh{std::string(todayDate)};
    save(fresh);
    return fresh;
}

void TodoStorage::notifySyncTransport(const TodoDocument& doc, const std::string& serialized) {
    ISyncTransport* transport = nullptr;
    SyncSnapshot snap{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = syncTransport_;
        if (!transport) {
            return;
        }
        snap.date = doc.date();
        snap.markdownPayload = serialized;
        snap.updatedAtMs = lastUpdatedMs_;
        snap.revision = revision_;
        snap.deviceId = "windows-pc";
    }
    transport->publishLocalSnapshot(snap);
}

bool TodoStorage::save(const TodoDocument& doc) {
    std::string serialized = doc.serialize();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastSavedSerialized_ = serialized;
        lastUpdatedMs_ = currentEpochMs();
        ++revision_;
    }
    bool okTodo = writeUtf8Atomic(todoPath_, serialized);
    bool okLog = writeUtf8Atomic(logFilePathFor(doc.date()), serialized);
    if (okTodo && okLog) {
        notifySyncTransport(doc, serialized);
    }
    return okTodo && okLog;
}

bool TodoStorage::reloadIfExternallyModified(std::string_view todayDate, TodoDocument& outDoc) {
    std::string raw;
    if (!readUtf8File(todoPath_, raw)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (raw == lastSavedSerialized_) {
            return false;
        }
    }

    TodoDocument parsed = TodoDocument::parse(raw, todayDate);
    if (parsed.date() != todayDate) {
        std::string_view rawView = raw;
        std::size_t firstNewline = rawView.find('\n');
        if (firstNewline != std::string_view::npos) {
            parsed = TodoDocument::parse(rawView.substr(firstNewline + 1), todayDate);
        } else {
            parsed.ensureDate(todayDate);
        }
    }

    std::string canonical = parsed.serialize();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastSavedSerialized_ = canonical;
        lastUpdatedMs_ = currentEpochMs();
        ++revision_;
    }
    writeUtf8Atomic(logFilePathFor(parsed.date()), canonical);
    if (raw != canonical) {
        writeUtf8Atomic(todoPath_, canonical);
    }
    notifySyncTransport(parsed, canonical);

    outDoc = std::move(parsed);
    return true;
}

void TodoStorage::setSyncTransport(ISyncTransport* transport, std::function<void()> onRemoteApplied) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (syncTransport_ && syncTransport_ != transport) {
            syncTransport_->stop();
        }
        syncTransport_ = transport;
        onRemoteApplied_ = std::move(onRemoteApplied);
    }
    if (transport) {
        transport->start([this](const SyncSnapshot& snap) {
            TodoDocument merged;
            std::function<void()> cb;
            if (applyRemoteSnapshot(snap, currentLocalDateString(), merged)) {
                std::lock_guard<std::mutex> lock(mutex_);
                cb = onRemoteApplied_;
            }
            if (cb) {
                cb();
            }
        });
    }
}

bool TodoStorage::applyRemoteSnapshot(
    const SyncSnapshot& snapshot,
    std::string_view todayDate,
    TodoDocument& outMerged) {
    if (snapshot.date != todayDate) {
        return false;
    }

    std::string localRaw;
    TodoDocument localDoc = readUtf8File(todoPath_, localRaw)
                                ? TodoDocument::parse(localRaw, todayDate)
                                : TodoDocument(std::string(todayDate));
    localDoc.ensureDate(todayDate);

    TodoDocument remoteDoc = TodoDocument::parse(snapshot.markdownPayload, todayDate);
    bool preferRemote = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        preferRemote = (snapshot.updatedAtMs >= lastUpdatedMs_);
    }

    localDoc.mergeWithRemote(remoteDoc, preferRemote);
    std::string mergedStr = localDoc.serialize();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mergedStr == lastSavedSerialized_) {
            outMerged = std::move(localDoc);
            return false;
        }
        lastSavedSerialized_ = mergedStr;
        lastUpdatedMs_ = std::max(currentEpochMs(), snapshot.updatedAtMs);
        ++revision_;
    }

    bool okTodo = writeUtf8Atomic(todoPath_, mergedStr);
    bool okLog = writeUtf8Atomic(logFilePathFor(todayDate), mergedStr);
    outMerged = std::move(localDoc);
    return okTodo && okLog;
}

bool TodoStorage::startWatching(std::function<void()> onExternalChange) {
    stopWatching();

    HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hStop) {
        return false;
    }
    stopEvent_ = hStop;
    watchThread_ = std::thread(&TodoStorage::watchThreadMain, this, std::move(onExternalChange));
    return true;
}

void TodoStorage::stopWatching() {
    if (stopEvent_) {
        SetEvent(static_cast<HANDLE>(stopEvent_));
    }
    if (watchThread_.joinable()) {
        watchThread_.join();
    }
    if (stopEvent_) {
        CloseHandle(static_cast<HANDLE>(stopEvent_));
        stopEvent_ = nullptr;
    }
}

void TodoStorage::watchThreadMain(std::function<void()> onExternalChange) {
    HANDLE hDir = CreateFileW(
        baseDir_.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (hDir == INVALID_HANDLE_VALUE) {
        return;
    }

    HANDLE hIoEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hIoEvent) {
        CloseHandle(hDir);
        return;
    }

    std::vector<BYTE> buffer(16 * 1024);
    HANDLE stopEvt = static_cast<HANDLE>(stopEvent_);
    const std::wstring targetName = todoPath_.filename().wstring();

    while (WaitForSingleObject(stopEvt, 0) != WAIT_OBJECT_0) {
        ResetEvent(hIoEvent);
        OVERLAPPED ov{};
        ov.hEvent = hIoEvent;

        BOOL issued = ReadDirectoryChangesW(
            hDir,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            nullptr,
            &ov,
            nullptr);
        if (!issued) {
            break;
        }

        HANDLE waits[2] = {stopEvt, hIoEvent};
        DWORD waitRes = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (waitRes == WAIT_OBJECT_0) {
            CancelIoEx(hDir, &ov);
            DWORD transferred = 0;
            GetOverlappedResult(hDir, &ov, &transferred, TRUE);
            break;
        }
        if (waitRes != WAIT_OBJECT_0 + 1) {
            CancelIoEx(hDir, &ov);
            break;
        }

        DWORD bytesReturned = 0;
        if (!GetOverlappedResult(hDir, &ov, &bytesReturned, FALSE) || bytesReturned == 0) {
            continue;
        }

        bool touchedTodo = false;
        const BYTE* cursor = buffer.data();
        for (;;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor);
            std::wstring_view changedName(info->FileName, info->FileNameLength / sizeof(WCHAR));
            if (equalsIgnoreCaseWide(changedName, targetName)) {
                touchedTodo = true;
                break;
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            cursor += info->NextEntryOffset;
        }

        if (touchedTodo) {
            if (WaitForSingleObject(stopEvt, 50) == WAIT_OBJECT_0) {
                break;
            }
            if (onExternalChange) {
                onExternalChange();
            }
        }
    }

    CloseHandle(hIoEvent);
    CloseHandle(hDir);
}

} // namespace todofloat
