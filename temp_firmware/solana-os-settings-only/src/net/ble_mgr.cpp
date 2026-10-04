#include "ble_mgr.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include <utility>  // std::swap

#include "../badge_log.h"
#include "ble_bridge.h"
#include "push_protocol.h"

namespace ble_mgr {
namespace {

constexpr char SERVICE_UUID[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char RX_UUID[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char TX_UUID[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

// Default ATT MTU is 23, leaving 20 bytes of payload per notification. We ask
// for more (185, below) but must not assume we got it: a central that never
// runs an MTU exchange leaves us at 23 forever.
constexpr size_t NOTIFY_CHUNK_MIN = 20;
// Upper bound on a single notification. 240 keeps one bridge frame (<=240 b64
// characters plus a short header and the newline) inside two notifies at worst,
// and stays clear of the NimBLE msys block sizes in sdkconfig.
constexpr size_t NOTIFY_CHUNK_MAX = 240;

// Pacing between notifications.
//
// The old code sent 20 bytes and slept 4 ms unconditionally (~5 KB/s), which is
// what made a bridged HTTP response unusable. Now the sleep is picked from the
// link: a negotiated MTU means a modern central that can drain notifications at
// a real rate, so one scheduler tick is enough of a yield; an un-negotiated
// 23-byte MTU keeps the original 4 ms exactly, so nothing about today's
// badge-push.py transfers over a dumb link changes.
//
// HARDWARE-TUNE: if a phone reports missing/garbled frames under load, raise
// NOTIFY_PACING_FAST_MS first - notify() on this core returns void and gives us
// no backpressure signal, so pacing is the only knob.
constexpr uint32_t NOTIFY_PACING_FAST_MS = 1;
constexpr uint32_t NOTIFY_PACING_LEGACY_MS = 4;

BLEServer *sServer = nullptr;
BLECharacteristic *sTx = nullptr;
bool sEnabled = false;
volatile bool sConnected = false;

LineHandler sHandler;

// The GATT write callback runs on the BLE stack's task. Lines are queued here
// and dispatched from update() on the main loop, for the same reason ESP-NOW
// packets are.
//
// Two rings, not one. A bridged HTTP response arrives as ~180 back-to-back
// %DATA frames; sharing one 8-slot ring with app traffic would drop most of
// them, and - worse - pumpBridge() would have to skip over app lines to find
// them, which a ring cannot do without reordering. Splitting the lanes at
// enqueue() time (a single character test) makes "drain only bridge frames,
// leave everything else queued in order" a property of the data structure.
constexpr size_t QUEUE_LEN = 12;
constexpr size_t BRIDGE_QUEUE_LEN = 24;
String sQueue[QUEUE_LEN];
volatile size_t sQueueHead = 0;
volatile size_t sQueueTail = 0;
String sBridgeQueue[BRIDGE_QUEUE_LEN];
volatile size_t sBridgeHead = 0;
volatile size_t sBridgeTail = 0;
portMUX_TYPE sQueueMux = portMUX_INITIALIZER_UNLOCKED;

// Partial line accumulated across writes.
String sPending;

void enqueue(const String &line) {
  const bool bridge = line.length() > 0 && line[0] == BRIDGE_PREFIX;

  // Copy (which mallocs) OUTSIDE the critical section. Taking the heap mutex
  // while interrupts are disabled by portENTER_CRITICAL can deadlock or abort,
  // so under the spinlock we only swap String buffers (a pointer exchange, no
  // allocation). `copy` then holds the slot's previous (empty) buffer and is
  // freed here, after the lock is released.
  String copy = line;
  bool room;
  portENTER_CRITICAL(&sQueueMux);
  if (bridge) {
    const size_t next = (sBridgeHead + 1) % BRIDGE_QUEUE_LEN;
    room = (next != sBridgeTail);
    if (room) {
      std::swap(sBridgeQueue[sBridgeHead], copy);
      sBridgeHead = next;
    }
  } else {
    const size_t next = (sQueueHead + 1) % QUEUE_LEN;
    room = (next != sQueueTail);
    if (room) {
      std::swap(sQueue[sQueueHead], copy);
      sQueueHead = next;
    }
  }
  portEXIT_CRITICAL(&sQueueMux);
  if (!room) {
    badge_log::tagf("ble", bridge ? "bridge rx queue full, frame dropped"
                                  : "rx queue full, line dropped");
  }
}

// Both pops follow the same rule as enqueue(): only a buffer swap happens under
// the spinlock, and the real free happens when `out`'s previous buffer is
// released by the caller after the lock is gone.
bool popLine(String &out) {
  bool got = false;
  portENTER_CRITICAL(&sQueueMux);
  if (sQueueTail != sQueueHead) {
    std::swap(out, sQueue[sQueueTail]);
    sQueueTail = (sQueueTail + 1) % QUEUE_LEN;
    got = true;
  }
  portEXIT_CRITICAL(&sQueueMux);
  return got;
}

bool popBridgeLine(String &out) {
  bool got = false;
  portENTER_CRITICAL(&sQueueMux);
  if (sBridgeTail != sBridgeHead) {
    std::swap(out, sBridgeQueue[sBridgeTail]);
    sBridgeTail = (sBridgeTail + 1) % BRIDGE_QUEUE_LEN;
    got = true;
  }
  portEXIT_CRITICAL(&sQueueMux);
  return got;
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    (void)server;
    sConnected = true;
    badge_log::tagf("ble", "central connected");
  }
  void onDisconnect(BLEServer *server) override {
    sConnected = false;
    sPending = "";
    // Drop the push-protocol session with the link. Without this the next
    // central to connect inherits the previous one's authorisation (and any
    // half-finished transfer / staged Wi-Fi password), because sAuthorised is a
    // sticky global that otherwise survives disconnect.
    push_protocol::reset();
    // Flag only - this runs on the BLE stack task, and the main loop may be
    // sitting inside a bridged request reading the very buffer a full teardown
    // would free. ble_bridge does the teardown itself, on the main loop.
    ble_bridge::linkLost();
    badge_log::tagf("ble", "central disconnected");
    // Without this the badge stops being discoverable after the first client.
    if (server) server->startAdvertising();
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    const uint8_t *data = characteristic->getData();
    const size_t length = characteristic->getLength();
    if (data == nullptr || length == 0) return;

    for (size_t i = 0; i < length; ++i) {
      const char c = (char)data[i];
      if (c == '\n') {
        enqueue(sPending);
        sPending = "";
      } else if (c != '\r') {
        // Bound the buffer: a peer that never sends a newline must not be able
        // to grow this without limit.
        if (sPending.length() < 4096) sPending += c;
      }
    }
  }
};

ServerCallbacks sServerCallbacks;
RxCallbacks sRxCallbacks;

}  // namespace

bool begin(const String &deviceName) {
  if (sEnabled) return true;

  BLEDevice::init(deviceName.c_str());
  BLEDevice::setMTU(185);  // request; the central decides what we actually get

  sServer = BLEDevice::createServer();
  if (sServer == nullptr) {
    badge_log::tagf("ble", "createServer failed");
    return false;
  }
  sServer->setCallbacks(&sServerCallbacks);

  BLEService *service = sServer->createService(SERVICE_UUID);

  // No explicit 2902 descriptor: this core builds BLE on NimBLE, which adds the
  // client-configuration descriptor itself for any characteristic that declares
  // NOTIFY. Adding one by hand is deprecated and will stop compiling.
  sTx = service->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);

  BLECharacteristic *rx = service->createCharacteristic(
      RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(&sRxCallbacks);

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);  // iOS connection-interval workaround
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  sEnabled = true;
  badge_log::tagf("ble", "advertising as '%s' (%s)", deviceName.c_str(),
                  BLEDevice::getAddress().toString().c_str());
  return true;
}

void end() {
  if (!sEnabled) return;
  BLEDevice::deinit(true);
  sServer = nullptr;
  sTx = nullptr;
  sEnabled = false;
  sConnected = false;
  sQueueHead = 0;
  sQueueTail = 0;
  sBridgeHead = 0;
  sBridgeTail = 0;
  sPending = "";
  ble_bridge::linkLost();
  badge_log::tagf("ble", "off");
}

bool enabled() { return sEnabled; }
bool connected() { return sEnabled && sConnected; }

uint16_t mtu() {
  if (!connected() || sServer == nullptr) return 23;
  // NimBLE answers this with ble_att_mtu(conn_handle), which is 0 for a handle
  // it does not know; Bluedroid answers 23 in the same situation. Normalise to
  // the ATT default so callers only ever see a legal MTU.
  const uint16_t negotiated = sServer->getPeerMTU(sServer->getConnId());
  return negotiated >= 23 ? negotiated : 23;
}

void pumpBridge() {
  if (!sEnabled) return;
  while (true) {
    String line;
    if (!popBridgeLine(line)) break;
    ble_bridge::onLine(line);
  }
}

void update() {
  if (!sEnabled) return;

  // Bridge frames first, and never through sHandler/push_protocol: an app that
  // claimed the link with ble.listen() must not see (or be able to spoof a
  // reply to) the phone's tunnel traffic.
  pumpBridge();

  while (true) {
    String line;
    // Moves the slot's buffer out by swapping (no alloc/free under the lock);
    // the slot is left empty and `line`'s old empty buffer goes into it. The
    // real free happens when `line` is destroyed at the end of the iteration,
    // outside the critical section.
    if (!popLine(line)) break;

    if (sHandler) {
      sHandler(line);
    } else {
      push_protocol::handleLine(line, [](const String &reply) { sendLine(reply); });
    }
  }
}

bool send(const String &text) {
  if (!connected() || sTx == nullptr) return false;

  const uint16_t negotiated = mtu();
  size_t chunkSize = NOTIFY_CHUNK_MIN;
  uint32_t pacingMs = NOTIFY_PACING_LEGACY_MS;
  if (negotiated > 23) {
    // 3 bytes of the ATT MTU are the notification header.
    chunkSize = (size_t)(negotiated - 3);
    if (chunkSize < NOTIFY_CHUNK_MIN) chunkSize = NOTIFY_CHUNK_MIN;
    if (chunkSize > NOTIFY_CHUNK_MAX) chunkSize = NOTIFY_CHUNK_MAX;
    pacingMs = NOTIFY_PACING_FAST_MS;
  }

  const size_t length = text.length();
  for (size_t offset = 0; offset < length; offset += chunkSize) {
    const size_t chunk = min(chunkSize, length - offset);
    sTx->setValue((uint8_t *)(text.c_str() + offset), chunk);
    sTx->notify();
    delay(pacingMs);  // give the stack time to drain; without it long replies are lost
  }
  return true;
}

bool sendLine(const String &text) {
  return send(text.endsWith("\n") ? text : text + "\n");
}

void onLine(LineHandler handler) { sHandler = std::move(handler); }
void clearLineHandler() { sHandler = nullptr; }
bool hasLineHandler() { return (bool)sHandler; }

String address() {
  if (!sEnabled) return String();
  return String(BLEDevice::getAddress().toString().c_str());
}

}  // namespace ble_mgr
