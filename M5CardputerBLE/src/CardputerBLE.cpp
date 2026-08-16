/*
 * SPDX-FileCopyrightText: 2026 M5CardputerBLE contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * CardputerBLE.cpp
 * BLE UART (Nordic UART Service, NUS) 双向数据通道模块实现。
 *
 * 依赖：NimBLE-Arduino (https://github.com/h2zero/NimBLE-Arduino)
 *   - arduino-esp32 v2.x  -> 安装 NimBLE-Arduino 1.4.x（旧版回调签名）
 *   - arduino-esp32 v3.x  -> 安装 NimBLE-Arduino 2.x  （新版回调签名）
 */
#include "CardputerBLE.h"

// ---------------------------------------------------------------------------
// NimBLE-Arduino API 版本适配
//   arduino-esp32 v2.x  -> NimBLE-Arduino 1.x：onWrite(chr) / onConnect(server)
//   arduino-esp32 v3.x  -> NimBLE-Arduino 2.x：onWrite(chr, connInfo) /
//                          onConnect(server, connInfo) / onDisconnect(server, connInfo, reason)
// ---------------------------------------------------------------------------
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define BLE_NIMBLE_API_V2 1
#else
#define BLE_NIMBLE_API_V2 0
#endif

const char* const BLE_Class::SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const char* const BLE_Class::RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
const char* const BLE_Class::TX_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

static BLE_Class* _ble_instance = nullptr;

// ---------------------------------------------------------------------------
// Server 回调：连接 / 断开
// ---------------------------------------------------------------------------
class BLE_ServerCallbacks : public NimBLEServerCallbacks {
public:
#if BLE_NIMBLE_API_V2
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override
    {
        (void)server;
        (void)connInfo;
        if (_ble_instance) _ble_instance->_onConnected();
    }
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override
    {
        (void)server;
        (void)connInfo;
        (void)reason;
        if (_ble_instance) _ble_instance->_onDisconnected();
    }
#else
    void onConnect(NimBLEServer* server) override
    {
        (void)server;
        if (_ble_instance) _ble_instance->_onConnected();
    }
    void onDisconnect(NimBLEServer* server) override
    {
        (void)server;
        if (_ble_instance) _ble_instance->_onDisconnected();
    }
#endif
};

// ---------------------------------------------------------------------------
// RX 特征回调：手机写入数据
// ---------------------------------------------------------------------------
class BLE_RxCallbacks : public NimBLECharacteristicCallbacks {
public:
#if BLE_NIMBLE_API_V2
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override
    {
        (void)connInfo;
        _handle(chr);
    }
#else
    void onWrite(NimBLECharacteristic* chr) override
    {
        _handle(chr);
    }
#endif

private:
    static void _handle(NimBLECharacteristic* chr)
    {
        if (!_ble_instance || !chr) return;
        std::string value = chr->getValue();
        if (value.empty()) return;
        _ble_instance->_onWrite(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
};

// ---------------------------------------------------------------------------
// BLE_Class 实现
// ---------------------------------------------------------------------------
BLE_Class::~BLE_Class()
{
    end();
}

void BLE_Class::begin(const char* deviceName, bool autoAdvertise)
{
    if (_stackInited) {
        if (autoAdvertise) NimBLEDevice::startAdvertising();
        return;
    }

    const char* name = (deviceName && deviceName[0]) ? deviceName : "M5Cardputer";

    NimBLEDevice::init(name);

    // 请求最大 MTU，让音频帧等信息能在一个通知内发送，避免分包导致的
    // 数据错乱（默认 MTU=23 导致 ~400 字节的音频帧被切成 20+ 个分包）。
    NimBLEDevice::setMTU(512);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new BLE_ServerCallbacks());

    _service = _server->createService(SERVICE_UUID);

// NIMBLE_PROPERTY::* 宏在 NimBLE-Arduino 1.4.x 与 2.x 中均可用
    const uint32_t rx_props = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR;
    const uint32_t tx_props = NIMBLE_PROPERTY::NOTIFY;

    _rxChr = _service->createCharacteristic(RX_CHAR_UUID, rx_props);
    _rxChr->setCallbacks(new BLE_RxCallbacks());

    _txChr = _service->createCharacteristic(TX_CHAR_UUID, tx_props);

    _service->start();

    if (!_rxMutex) {
        _rxMutex = xSemaphoreCreateMutex();
    }

    // 广播中包含 NUS 服务 UUID，方便手机端识别
    _server->getAdvertising()->addServiceUUID(_service->getUUID());

    _stackInited = true;
    _initialized = true;
    _ble_instance = this;

    if (autoAdvertise) {
        NimBLEDevice::startAdvertising();
    }
}

void BLE_Class::end()
{
    if (!_stackInited) return;

    NimBLEDevice::stopAdvertising();

    if (_server) {
        auto peers = _server->getPeerDevices();
        for (auto conn : peers) {
            _server->disconnect(conn);
        }
    }

    _connected   = false;
    _initialized = false;
    _rxHead      = 0;
    _rxTail      = 0;
}

bool BLE_Class::connected() const
{
    return _connected;
}

void BLE_Class::setRecvCallback(RecvCallback_t cb)
{
    _recvCb = cb;
}

void BLE_Class::setConnectionCallback(ConnectionCallback_t cb)
{
    _connCb = cb;
}

// ---------------------------------------------------------------------------
// 内部事件（在 NimBLE 任务上下文中被调用）
// ---------------------------------------------------------------------------
void BLE_Class::_onConnected()
{
    _connected = true;
    if (_connCb) _connCb(true);
}

void BLE_Class::_onDisconnected()
{
    _connected = false;
    if (_connCb) _connCb(false);
    // 断开后自动重新广播，方便手机再次连接
    NimBLEDevice::startAdvertising();
}

void BLE_Class::_onWrite(const uint8_t* data, size_t len)
{
    _pushRx(data, len);
    if (_recvCb) _recvCb(data, len);
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------
uint16_t BLE_Class::_peerMTU() const
{
    if (!_server) return 23;
    auto peers = _server->getPeerDevices();
    if (peers.empty()) return 23;
    uint16_t mtu = _server->getPeerMTU(peers[0]);
    return (mtu > 0) ? mtu : 23;
}

size_t BLE_Class::send(const uint8_t* data, size_t len)
{
    if (!_stackInited || !_connected || !_txChr || !data || len == 0) return 0;

    // 按协商后的 MTU 分块发送（BLE 单包最大载荷 = MTU - 3）
    // 上限 509 字节：NimBLE 默认属性最大长度 BLE_ATT_ATTR_MAX_LEN(512) - 3
    uint16_t chunk = _peerMTU() - 3;
    if (chunk < 20) chunk = 20;
    if (chunk > 509) chunk = 509;

    size_t sent = 0;
    while (sent < len) {
        size_t n = (len - sent > chunk) ? chunk : (len - sent);
        _txChr->setValue(data + sent, n);
        _txChr->notify();
        sent += n;
    }
    return sent;
}

size_t BLE_Class::send(const char* str)
{
    return (str == nullptr) ? 0 : send(reinterpret_cast<const uint8_t*>(str), strlen(str));
}

size_t BLE_Class::send(const String& str)
{
    return (str.length() == 0) ? 0 : send(reinterpret_cast<const uint8_t*>(str.c_str()), str.length());
}

size_t BLE_Class::send(char c)
{
    return send(reinterpret_cast<const uint8_t*>(&c), 1);
}

size_t BLE_Class::sendLine(const char* str)
{
    return send(str) + send("\r\n");
}

size_t BLE_Class::sendLine(const String& str)
{
    return send(str) + send("\r\n");
}

// ---------------------------------------------------------------------------
// 接收环形缓冲
// ---------------------------------------------------------------------------
void BLE_Class::_pushRx(const uint8_t* data, size_t len)
{
    if (!_rxMutex || !data || len == 0) return;
    if (xSemaphoreTake(_rxMutex, portMAX_DELAY) != pdTRUE) return;

    for (size_t i = 0; i < len; ++i) {
        size_t next = (_rxHead + 1) % RX_BUF_SIZE;
        if (next == _rxTail) break;  // 缓冲区满，丢弃新数据
        _rxBuf[_rxHead] = data[i];
        _rxHead         = next;
    }
    xSemaphoreGive(_rxMutex);
}

size_t BLE_Class::_popRx(uint8_t* dst, size_t maxLen)
{
    if (!_rxMutex || !dst || maxLen == 0) return 0;

    size_t got = 0;
    if (xSemaphoreTake(_rxMutex, portMAX_DELAY) != pdTRUE) return 0;

    while (got < maxLen && _rxTail != _rxHead) {
        dst[got++] = _rxBuf[_rxTail];
        _rxTail    = (_rxTail + 1) % RX_BUF_SIZE;
    }
    xSemaphoreGive(_rxMutex);
    return got;
}

size_t BLE_Class::available() const
{
    size_t head = _rxHead;
    size_t tail = _rxTail;
    return (head >= tail) ? (head - tail) : (RX_BUF_SIZE - tail + head);
}

int BLE_Class::read()
{
    uint8_t b = 0;
    return _popRx(&b, 1) ? static_cast<int>(b) : -1;
}

size_t BLE_Class::readBytes(uint8_t* buf, size_t maxLen)
{
    return _popRx(buf, maxLen);
}

String BLE_Class::readString()
{
    String s;
    s.reserve(available());

    uint8_t tmp[64];
    size_t n = 0;
    while ((n = _popRx(tmp, sizeof(tmp))) > 0) {
        s.concat(reinterpret_cast<const char*>(tmp), n);
    }
    return s;
}

void BLE_Class::flushRx()
{
    if (!_rxMutex) return;
    if (xSemaphoreTake(_rxMutex, portMAX_DELAY) != pdTRUE) return;
    _rxHead = _rxTail;
    xSemaphoreGive(_rxMutex);
}
