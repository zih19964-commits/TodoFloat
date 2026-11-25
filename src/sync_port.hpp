#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace todofloat {

// 跨设备当日待办同步快照（负载小于 1KB，同时适配局域网 HTTP/WebSocket 帧与蓝牙 BLE GATT 特征分片）
struct SyncSnapshot {
    std::string date;              // 当日日期 YYYY-MM-DD
    std::string markdownPayload;   // 标准 UTF-8 todo.md 文本内容
    std::uint64_t updatedAtMs = 0; // 本地修改 UTC 毫秒时间戳
    std::uint64_t revision = 0;    // 本地单调递增修订序号
    std::string deviceId;          // 设备标识（如 "win-pc" 或 "huawei-harmony"）
};

// 远程同步传输层抽象端口：供后续局域网（UDP 发现 + HTTP/WS）或蓝牙（WinRT BLE GATT）模块无侵入挂载
class ISyncTransport {
public:
    using RemoteReceiveHandler = std::function<void(const SyncSnapshot&)>;

    virtual ~ISyncTransport() = default;

    // 启动传输监听并注册收到远端（如华为手机/平板）同步快照时的回调
    virtual bool start(RemoteReceiveHandler onRemoteSnapshot) = 0;

    // 停止传输监听
    virtual void stop() = 0;

    // 当本地 UI 或本地 AI 更新今日待办后，主动向已配对远端推送最新快照
    virtual void publishLocalSnapshot(const SyncSnapshot& snapshot) = 0;
};

} // namespace todofloat
