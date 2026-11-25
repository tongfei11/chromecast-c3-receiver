#include <Arduino.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <esp_log.h>
#include <atomic>
#include <vector>
#include "ReceiverConfig.h"

namespace {
constexpr char kTarget[] = RECEIVER_TARGET_MAC;
constexpr char kBuild[] = "0006";
constexpr uint16_t kKeysHandle = 0x002b;
constexpr size_t kMaxNotification = 244;
constexpr size_t kMaxCommand = 32;
const BLEUUID kVoiceService("ab5e0001-5a21-4f05-bc7d-af01f617b664");
const BLEUUID kVoiceTx("ab5e0002-5a21-4f05-bc7d-af01f617b664");
const BLEUUID kVoiceRx("ab5e0003-5a21-4f05-bc7d-af01f617b664");
const BLEUUID kVoiceControl("ab5e0004-5a21-4f05-bc7d-af01f617b664");
constexpr uint32_t kPairingTimeout = 60000;
constexpr int kPending = -1;
constexpr int kIdle = -2;
BLEAddress targetAddress(std::string("00:00:00:00:00:00"));
BLEClient* client;
BLESecurity security;
std::atomic<int> authenticationResult{kPending};
std::atomic<int> bondRemovalResult{kIdle};
std::atomic<bool> linkConnected{false};
bool scanningEnabled = false;
bool ready = false;
uint16_t voiceTxHandle = 0;
esp_gatt_write_type_t voiceWriteType = ESP_GATT_WRITE_TYPE_RSP;

enum class EventKind : uint8_t { keys, audio, control, connected, disconnected, passkey, auth };
struct Event {
  EventKind kind;
  uint32_t value;
  uint16_t length;
  uint8_t data[kMaxNotification];
};

QueueHandle_t events;
std::atomic<unsigned> audioDrops{0};
std::atomic<unsigned> priorityDrops{0};

void queueEvent(EventKind kind, uint32_t value = 0, const uint8_t* data = nullptr, size_t length = 0) {
  // Reserve space for non-audio events and BLE lifecycle messages, keeping delivery in BLE order.
  const UBaseType_t reserve = kind == EventKind::audio ? 24 :
      (kind == EventKind::keys || kind == EventKind::control ? 4 : 0);
  if (length <= kMaxNotification && uxQueueSpacesAvailable(events) > reserve) {
    Event event{};
    event.kind = kind;
    event.value = value;
    event.length = length;
    if (length) memcpy(event.data, data, length);
    if (xQueueSend(events, &event, 0) == pdTRUE) return;
  }
  (kind == EventKind::audio ? audioDrops : priorityDrops).fetch_add(1);
}

size_t encodeAudioFrame(uint8_t* frame, const uint8_t* payload, size_t length) {
  frame[0] = 0xa5;
  frame[1] = 0x5a;
  frame[2] = 0x01;
  frame[3] = length & 0xff;
  frame[4] = length >> 8;
  memcpy(frame + 5, payload, length);
  uint16_t crc = 0xffff;
  for (size_t index = 2; index < length + 5; ++index) {
    crc ^= static_cast<uint16_t>(frame[index]) << 8;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
  }
  frame[length + 5] = crc & 0xff;
  frame[length + 6] = crc >> 8;
  return length + 7;
}

void drainEvents() {
  Event event;
  while (xQueuePeek(events, &event, 0) == pdTRUE) {
    char line[64 + 2 * kMaxNotification];
    size_t length = 0;
    switch (event.kind) {
      case EventKind::keys:
        length = snprintf(line, sizeof(line), "REPORT ms=%lu handle=%04x len=%u hex=",
                          static_cast<unsigned long>(event.value), kKeysHandle, event.length);
        break;
      case EventKind::audio:
        length = encodeAudioFrame(reinterpret_cast<uint8_t*>(line), event.data, event.length);
        break;
      case EventKind::control:
        length = snprintf(line, sizeof(line), "ATVV kind=control len=%u hex=", event.length);
        break;
      case EventKind::connected:
        length = snprintf(line, sizeof(line), "CONNECTED");
        break;
      case EventKind::disconnected:
        length = snprintf(line, sizeof(line), "DISCONNECTED");
        break;
      case EventKind::passkey:
        length = snprintf(line, sizeof(line), "PASSKEY %06lu", static_cast<unsigned long>(event.value));
        break;
      case EventKind::auth:
        length = snprintf(line, sizeof(line), "AUTH success=%u reason=%02x",
                          event.value >> 8, event.value & 0xff);
        break;
    }
    if (event.kind != EventKind::audio) {
      constexpr char digits[] = "0123456789abcdef";
      for (size_t index = 0; index < event.length; ++index) {
        line[length++] = digits[event.data[index] >> 4];
        line[length++] = digits[event.data[index] & 0xf];
      }
      line[length++] = '\n';
    }
    if (Serial.availableForWrite() < static_cast<int>(length + 32)) break;
    xQueueReceive(events, &event, 0);
    Serial.write(reinterpret_cast<const uint8_t*>(line), length);
  }
  if (Serial.availableForWrite() < 96) return;
  const unsigned audio = audioDrops.exchange(0);
  const unsigned priority = priorityDrops.exchange(0);
  if (audio || priority) Serial.printf("ATVV_DROPPED audio=%u priority=%u\n", audio, priority);
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

void sendVoiceCommand(const char* hex, size_t length) {
  if (length == 0 || length > 2 * kMaxCommand || length % 2 != 0) {
    Serial.println("TX_FAILED reason=invalid_length");
    return;
  }
  uint8_t bytes[kMaxCommand];
  for (size_t index = 0; index < length; index += 2) {
    const int high = hexDigit(hex[index]);
    const int low = hexDigit(hex[index + 1]);
    if (high < 0 || low < 0) {
      Serial.println("TX_FAILED reason=invalid_hex");
      return;
    }
    bytes[index / 2] = (high << 4) | low;
  }
  if (!ready || !linkConnected.load() || !voiceTxHandle) {
    Serial.println("TX_FAILED reason=voice_unavailable");
    return;
  }
  const esp_err_t result = esp_ble_gattc_write_char(client->getGattcIf(), client->getConnId(),
      voiceTxHandle, length / 2, bytes, voiceWriteType, ESP_GATT_AUTH_REQ_NONE);
  if (result == ESP_OK) Serial.printf("TX_ACCEPTED len=%u\n", static_cast<unsigned>(length / 2));
  else Serial.printf("TX_FAILED reason=%s\n", esp_err_to_name(result));
}
uint32_t nextAttempt = 0;
uint32_t pairingStartedAt = 0;
bool pairing = false;
bool pairingRequested = false;
bool stopRequested = false;

const char* targetConfigurationError() {
  if (kTarget[0] == '\0') return "target_not_configured";
  if (strlen(kTarget) != 17) return "target_invalid";
  for (size_t index = 0; index < 17; ++index) {
    const char c = kTarget[index];
    if (index % 3 == 2 ? c != ':' : !isxdigit(static_cast<unsigned char>(c))) {
      return "target_invalid";
    }
  }
  return nullptr;
}

bool authenticated() { return authenticationResult.load() == 0; }

void readCommands() {
  static char commandLine[3 + 2 * kMaxCommand];
  static size_t lineLength = 0;
  static bool isTxLine = false;
  static bool overflow = false;
  for (unsigned count = 0; count < 96 && Serial.available(); ++count) {
    const int command = Serial.read();
    if (isTxLine) {
      if (command == '\r' || command == '\n') {
        if (overflow) Serial.println("TX_FAILED reason=invalid_length");
        else if (lineLength < 3 || commandLine[1] != 'x' || commandLine[2] != ' ') {
          Serial.println("TX_FAILED reason=invalid_command");
        } else sendVoiceCommand(commandLine + 3, lineLength - 3);
        isTxLine = false;
        overflow = false;
        lineLength = 0;
      } else if (lineLength < sizeof(commandLine)) {
        commandLine[lineLength++] = command;
      } else {
        overflow = true;
      }
      continue;
    }
    if (command == 't') {
      isTxLine = true;
      commandLine[lineLength++] = command;
    } else if (command == 'p') {
      if (pairing || pairingRequested) Serial.println("PAIRING_BUSY");
      else {
        pairingRequested = true;
        stopRequested = false;
      }
    } else if (command == 'x') {
      stopRequested = true;
      pairingRequested = false;
    } else if (command == 's') {
      const bool connected = linkConnected.load();
      const bool secure = connected && authenticated();
      Serial.printf("STATUS build=%s connected=%u authenticated=%u ready=%u pairing=%u voice=%u\n",
                    kBuild, connected, secure, secure && ready, pairing || pairingRequested,
                    secure && ready && voiceTxHandle != 0);
    } else if (command != '\r' && command != '\n' && command != ' ' && command != '\t') {
      Serial.printf("COMMAND_ERROR byte=%02x\n", command);
    }
  }
}

void pairingFailed(const char* reason) {
  Serial.printf("PAIRING_FAILED reason=%s\n", reason);
  pairing = false;
  scanningEnabled = false;
}

bool targetMatches(const uint8_t* address) {
  return memcmp(address, targetAddress.getNative(), ESP_BD_ADDR_LEN) == 0;
}

void gapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* params) {
  if (event == ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT &&
      targetMatches(params->remove_bond_dev_cmpl.bd_addr)) {
    int pending = kPending;
    bondRemovalResult.compare_exchange_strong(pending, params->remove_bond_dev_cmpl.status);
  }
}

bool waitForDisconnect() {
  if (!linkConnected.load()) return true;
  client->disconnect();
  const uint32_t start = millis();
  while (linkConnected.load() && millis() - start < 4000) {
    readCommands();
    delay(20);
  }
  return !linkConnected.load();
}

const char* removeTargetBond() {
  // If a previous request timed out, wait for its completion before issuing another.
  if (bondRemovalResult.load() == kPending) return "bond_removal_pending";
  const int count = esp_ble_get_bond_device_num();
  if (count < 0) return "bond_list_failed";
  if (count == 0) return nullptr;
  std::vector<esp_ble_bond_dev_t> bonds(count);
  int available = count;
  if (esp_ble_get_bond_device_list(&available, bonds.data()) != ESP_OK) return "bond_list_failed";
  for (int index = 0; index < available; ++index) {
    if (!targetMatches(bonds[index].bd_addr)) continue;
    bondRemovalResult.store(kPending);
    if (esp_ble_remove_bond_device(bonds[index].bd_addr) != ESP_OK) {
      bondRemovalResult.store(kIdle);
      return "bond_remove_request_failed";
    }
    const uint32_t start = millis();
    while (bondRemovalResult.load() == kPending && millis() - start < 4000) {
      readCommands();
      delay(20);
    }
    const int status = bondRemovalResult.load();
    if (status == kPending) return "bond_remove_timeout";
    bondRemovalResult.store(kIdle);
    if (status != ESP_BT_STATUS_SUCCESS) return "bond_remove_failed";
    break;
  }
  return nullptr;
}

void beginPairing() {
  pairingRequested = false;
  if (const char* error = targetConfigurationError()) {
    pairingFailed(error);
    return;
  }
  if (!client) {
    pairingFailed("receiver_init_failed");
    return;
  }
  pairing = true;
  scanningEnabled = false;
  ready = false;
  Serial.printf("PAIRING_STARTED target=%s\n", kTarget);
  if (!waitForDisconnect()) {
    pairingFailed("disconnect_timeout");
    return;
  }
  if (stopRequested) return;
  if (const char* error = removeTargetBond()) {
    pairingFailed(error);
    return;
  }
  if (stopRequested) return;
  pairingStartedAt = millis();
  nextAttempt = pairingStartedAt;
  scanningEnabled = true;
}

void receivedKeys(BLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  queueEvent(EventKind::keys, millis(), data, length);
}

void receivedAudio(BLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  queueEvent(EventKind::audio, 0, data, length);
}

void receivedControl(BLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  queueEvent(EventKind::control, 0, data, length);
}

class SecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t pin) override {
    queueEvent(EventKind::passkey, pin);
  }
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    if (!targetMatches(result.bd_addr) || !linkConnected.load()) return;
    queueEvent(EventKind::auth, (static_cast<uint32_t>(result.success) << 8) | result.fail_reason);
    authenticationResult.store(result.success ? 0 : 1 + result.fail_reason);
  }
} securityCallbacks;

class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient*) override {
    queueEvent(EventKind::connected);
    linkConnected.store(true);
  }
  void onDisconnect(BLEClient*) override {
    authenticationResult.store(kPending);
    queueEvent(EventKind::disconnected);
    linkConnected.store(false);
  }
} clientCallbacks;

bool subscribe() {
  auto* services = client->getServices();
  BLERemoteCharacteristic* tx = nullptr;
  BLERemoteCharacteristic* rx = nullptr;
  BLERemoteCharacteristic* control = nullptr;
  bool keysSubscribed = false;
  voiceTxHandle = 0;
  for (const auto& serviceEntry : *services) {
    auto* service = serviceEntry.second;
    const bool voiceService = service->getUUID().equals(kVoiceService);
    // HID reports share a UUID, so enumerate by handle to retain every report.
    for (const auto& entry : *service->getCharacteristicsByHandle()) {
      auto* characteristic = entry.second;
      if (entry.first == kKeysHandle && characteristic->canNotify()) {
        characteristic->registerForNotify(receivedKeys);
        keysSubscribed = true;
      }
      if (!voiceService) continue;
      BLEUUID uuid = characteristic->getUUID();
      if (uuid.equals(kVoiceTx)) tx = characteristic;
      if (uuid.equals(kVoiceRx)) rx = characteristic;
      if (uuid.equals(kVoiceControl)) control = characteristic;
    }
  }
  if (tx && (tx->canWrite() || tx->canWriteNoResponse()) &&
      rx && (rx->canNotify() || rx->canIndicate()) &&
      control && (control->canNotify() || control->canIndicate()) && linkConnected.load()) {
    control->registerForNotify(receivedControl, control->canNotify());
    rx->registerForNotify(receivedAudio, rx->canNotify());
    if (linkConnected.load()) {
      voiceTxHandle = tx->getHandle();
      voiceWriteType = tx->canWrite() ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP;
    }
  }
  return keysSubscribed && linkConnected.load();
}

void connectRemote() {
  Serial.println("SCANNING target=" + String(kTarget));
  BLEScan* scan = BLEDevice::getScan();
  BLEScanResults results = scan->start(4, false);
  bool found = false;
  esp_ble_addr_type_t addressType = BLE_ADDR_TYPE_PUBLIC;
  for (int index = 0; index < results.getCount(); ++index) {
    BLEAdvertisedDevice device = results.getDevice(index);
    if (device.getAddress() == targetAddress) {
      found = true;
      addressType = device.getAddressType();
      Serial.printf("FOUND %s rssi=%d\n", device.toString().c_str(), device.getRSSI());
      break;
    }
  }
  scan->clearResults();
  readCommands();
  if (!found || stopRequested || pairingRequested) return;
  authenticationResult.store(kPending);
  if (!client->connect(targetAddress, addressType)) {
    Serial.println("CONNECT_FAILED");
    if (pairing) pairingFailed("connect_failed");
    return;
  }
  esp_bd_addr_t address;
  memcpy(address, client->getPeerAddress().getNative(), sizeof(address));
  esp_err_t result = esp_ble_set_encryption(address, ESP_BLE_SEC_ENCRYPT);
  Serial.printf("ENCRYPT_REQUEST %s\n", esp_err_to_name(result));
  const uint32_t start = millis();
  while (result == ESP_OK && linkConnected.load() && authenticationResult.load() == kPending &&
         !stopRequested && !pairingRequested && millis() - start < 15000) {
    readCommands();
    delay(20);
  }
  if (stopRequested || pairingRequested) return;
  const int auth = authenticationResult.load();
  if (result != ESP_OK || !linkConnected.load() || auth != 0) {
    Serial.println("AUTH_TIMEOUT_OR_FAILED");
    if (pairing) {
      if (result != ESP_OK) pairingFailed("encryption_request_failed");
      else if (!linkConnected.load()) pairingFailed("remote_disconnected");
      else pairingFailed(auth == kPending ? "authentication_timeout" : "authentication_failed");
    }
    if (linkConnected.load()) client->disconnect();
    return;
  }
  drainEvents();
  client->setMTU(kMaxNotification + 3);
  ready = subscribe() && linkConnected.load();
  if (ready) {
    Serial.printf("VOICE_AVAILABLE enabled=%u\n", voiceTxHandle != 0);
    Serial.printf("READY build=%s\n", kBuild);
    if (pairing) Serial.printf("PAIRING_SUCCEEDED target=%s\n", kTarget);
    pairing = false;
  } else {
    if (pairing) pairingFailed("subscription_failed");
    if (linkConnected.load()) client->disconnect();
  }
}
}  // namespace

void setup() {
  esp_log_level_set("*", ESP_LOG_NONE);
  Serial.setTxBufferSize(2048);
  Serial.begin(RECEIVER_SERIAL_BAUD);
  Serial.setDebugOutput(false);
  delay(1500);
  Serial.println("BOOT RemoteReceiver build=" + String(kBuild));
  Serial.println("Commands: p=re-pair target, s=status, x=disconnect and stop, tx HEX=ATVV command");
  if (const char* error = targetConfigurationError()) {
    Serial.printf("CONFIG_REQUIRED reason=%s setting=RECEIVER_TARGET_MAC file=ReceiverConfig.local.h\n", error);
    return;
  }
  events = xQueueCreate(64, sizeof(Event));
  if (!events) {
    Serial.println("INIT_FAILED reason=notification_queue_allocation");
    return;
  }
  targetAddress = BLEAddress(std::string(kTarget));
  BLEDevice::init(RECEIVER_DEVICE_NAME);
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(&securityCallbacks);
  BLEDevice::setCustomGapHandler(gapEvent);
  security.setAuthenticationMode(ESP_LE_AUTH_BOND);
  security.setCapability(ESP_IO_CAP_NONE);
  security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  client = BLEDevice::createClient();
  client->setClientCallbacks(&clientCallbacks);
  BLEDevice::getScan()->setActiveScan(true);
  BLEDevice::getScan()->setInterval(100);
  BLEDevice::getScan()->setWindow(80);
  scanningEnabled = true;
}

void loop() {
  if (events) drainEvents();
  readCommands();
  if (stopRequested) {
    stopRequested = false;
    if (pairing) pairingFailed("cancelled");
    scanningEnabled = false;
    ready = false;
    if (linkConnected.load()) client->disconnect();
    Serial.println("SCANNING_STOPPED");
  } else if (pairingRequested) {
    beginPairing();
  }

  if (!linkConnected.load()) {
    ready = false;
    voiceTxHandle = 0;
    if (pairing && scanningEnabled && millis() - pairingStartedAt >= kPairingTimeout) {
      pairingFailed("target_not_found");
    }
    if (!stopRequested && !pairingRequested && scanningEnabled &&
        static_cast<int32_t>(millis() - nextAttempt) >= 0) {
      connectRemote();
      nextAttempt = millis() + 2000;
    }
  }
  delay(2);
}
