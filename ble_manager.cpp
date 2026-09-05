/**
 * @file ble_manager.cpp
 * @brief Implementation of Bluetooth Low Energy manager with Nordic UART Service.
 */

#include "ble_manager.h"

BleManager::BleManager() 
  : _bledis(), _bleuart() {
}

void BleManager::begin(const char* deviceName) {
  // Disable automatic connection LED to preserve onboard RGB GPIO mapping
  Bluefruit.autoConnLed(false);

  // Configure maximum bandwidth: enables maximum MTU (247 bytes) and DLE
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);

  // Initialize Bluefruit stack
  Bluefruit.begin();
  Bluefruit.setTxPower(BLE_TX_POWER_DBM);
  Bluefruit.setName(deviceName);

  // Register peripheral connection event callbacks
  Bluefruit.Periph.setConnectCallback(BleManager::connectCallback);
  Bluefruit.Periph.setDisconnectCallback(BleManager::disconnectCallback);

  // 1. Initialize Device Information Service (DIS)
  _bledis.setManufacturer(BLE_MANUFACTURER_STR);
  _bledis.setModel(BLE_MODEL_STR);
  _bledis.begin();

  // 2. Initialize Nordic UART Service (NUS)
  _bleuart.begin();

  // 3. Setup and start BLE advertising
  startAdvertising();
}

void BleManager::startAdvertising() {
  // Primary advertising packet
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(_bleuart);

  // Secondary Scan Response packet contains the full advertised device name
  Bluefruit.ScanResponse.addName();

  // Automatically restart advertising when central disconnects
  Bluefruit.Advertising.restartOnDisconnect(true);

  // Fast advertising initially, transitioning to slow power-saving advertising
  Bluefruit.Advertising.setInterval(BLE_ADV_FAST_INTERVAL, BLE_ADV_SLOW_INTERVAL);
  Bluefruit.Advertising.setFastTimeout(BLE_ADV_FAST_TIMEOUT_S);

  // 0 = advertise indefinitely until a central connects
  Bluefruit.Advertising.start(0);
}

bool BleManager::isConnected() const {
  return Bluefruit.connected() > 0;
}

bool BleManager::isSubscribed() const {
  return const_cast<BLEUart&>(_bleuart).notifyEnabled();
}

BLEUart& BleManager::getUart() {
  return _bleuart;
}

int BleManager::read() {
  return _bleuart.read();
}

int BleManager::available() {
  return _bleuart.available();
}

size_t BleManager::write(uint8_t byte) {
  return _bleuart.write(byte);
}

size_t BleManager::write(const uint8_t* buffer, size_t size) {
  return _bleuart.write(buffer, size);
}

void BleManager::flush() {
  _bleuart.flush();
}

void BleManager::connectCallback(uint16_t conn_handle) {
  BLEConnection* connection = Bluefruit.Connection(conn_handle);
  char central_name[32] = { 0 };
  if (connection != nullptr) {
    connection->getPeerName(central_name, sizeof(central_name));
  }

  Serial.println(F("\n=================================================="));
  Serial.print(F("[BLE] Central Connected: "));
  Serial.println(central_name[0] != '\0' ? central_name : "Unknown Central");
  Serial.println(F("=================================================="));
}

void BleManager::disconnectCallback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;

  Serial.println(F("\n=================================================="));
  Serial.print(F("[BLE] Central Disconnected (reason: 0x"));
  Serial.print(reason, HEX);
  Serial.println(F(")"));
  Serial.println(F("=================================================="));
}
