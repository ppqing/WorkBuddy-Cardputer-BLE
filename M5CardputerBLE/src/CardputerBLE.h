/*
 * SPDX-FileCopyrightText: 2026 M5CardputerBLE contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * CardputerBLE.h
 * BLE UART (Nordic UART Service, NUS) 双向数据通道模块，用于 M5Cardputer / M5Cardputer-ADV。
 * 属于 M5CardputerBLE 库（基于 M5Stack M5Cardputer 驱动库派生，MIT 许可）。
 */
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

class BLE_Class {
public:
    // Nordic UART Service (NUS) 标准 UUID
    static const char* const SERVICE_UUID;
    static const char* const RX_CHAR_UUID;  // 手机 -> Cardputer (write / write-without-response)
    static const char* const TX_CHAR_UUID;  // Cardputer -> 手机 (notify)

    /// 收到 BLE 数据的回调（在 NimBLE 任务上下文中执行，请保持简短）
    typedef void (*RecvCallback_t)(const uint8_t* data, size_t len);
    /// 连接状态变化回调
    typedef void (*ConnectionCallback_t)(bool connected);

    BLE_Class() = default;
    ~BLE_Class();

    /// 初始化 BLE 并开始广播。deviceName 为空时使用默认名 "M5Cardputer"。
    void begin(const char* deviceName = "M5Cardputer", bool autoAdvertise = true);

    /// 停止广播并断开当前连接（之后可再次 begin() 恢复广播）
    void end();

    /// 当前是否有手机连接
    bool connected() const;

    /// BLE 是否已初始化
    bool initialized() const { return _initialized; }

    /// 注册数据接收回调（收到数据时调用；数据同时会写入内部接收缓冲，可用 read* 读取）
    void setRecvCallback(RecvCallback_t cb);

    /// 注册连接状态回调
    void setConnectionCallback(ConnectionCallback_t cb);

    // ==================== 发送 (Cardputer -> 手机) ====================
    size_t send(const uint8_t* data, size_t len);
    size_t send(const char* str);
    size_t send(const String& str);
    size_t send(char c);

    /// 发送一行文本（末尾自动追加 "\r\n"）
    size_t sendLine(const char* str);
    size_t sendLine(const String& str);

    // ==================== 接收 (手机 -> Cardputer) ====================
    /// 接收缓冲中当前可读的字节数
    size_t available() const;

    /// 读取一个字节，无数据时返回 -1
    int read();

    /// 批量读取，返回实际读取的字节数
    size_t readBytes(uint8_t* buf, size_t maxLen);

    /// 一次性读取当前缓冲中的全部数据（非阻塞）
    String readString();

    /// 清空接收缓冲
    void flushRx();

    // ---- 内部接口（供 NimBLE 回调类使用）----
    void _onConnected();
    void _onDisconnected();
    void _onWrite(const uint8_t* data, size_t len);

private:
    static const size_t RX_BUF_SIZE = 1024;

    uint8_t _rxBuf[RX_BUF_SIZE];
    volatile size_t _rxHead = 0;
    volatile size_t _rxTail = 0;
    SemaphoreHandle_t _rxMutex = nullptr;

    void _pushRx(const uint8_t* data, size_t len);
    size_t _popRx(uint8_t* dst, size_t maxLen);
    uint16_t _peerMTU() const;

    bool _stackInited = false;
    bool _initialized = false;
    bool _connected   = false;

    RecvCallback_t _recvCb        = nullptr;
    ConnectionCallback_t _connCb  = nullptr;

    NimBLEServer* _server   = nullptr;
    NimBLEService* _service = nullptr;
    NimBLECharacteristic* _rxChr = nullptr;
    NimBLECharacteristic* _txChr = nullptr;

    friend class BLE_ServerCallbacks;
    friend class BLE_RxCallbacks;
};
