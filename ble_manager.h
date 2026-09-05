/**
 * @file ble_manager.h
 * @brief Bluetooth Low Energy (BLE) manager using Nordic UART Service (NUS).
 * 
 * Manages BLE advertising, connections, Device Information Service (DIS),
 * and bidirectional data streaming via BLEUart on Seeed XIAO nRF52840 Sense.
 */

#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <Arduino.h>
#include <bluefruit.h>
#include "config.h"

class BleManager {
public:
  BleManager();

  /**
   * @brief Initializes Bluefruit stack, DIS, and BLEUart service, then starts advertising.
   * @param deviceName Human-readable BLE name broadcasted in advertising packets.
   */
  void begin(const char* deviceName = BLE_DEVICE_NAME);

  /**
   * @brief Checks if a central device (phone/PC) is currently connected.
   */
  bool isConnected() const;

  /**
   * @brief Checks if the connected central has subscribed to BLE UART TX notifications.
   */
  bool isSubscribed() const;

  /**
   * @brief Returns reference to the underlying BLEUart stream.
   */
  BLEUart& getUart();

  /**
   * @brief Reads incoming byte from BLE UART buffer.
   */
  int read();

  /**
   * @brief Returns number of bytes available to read from BLE UART buffer.
   */
  int available();

  /**
   * @brief Writes a byte to BLE UART TX.
   */
  size_t write(uint8_t byte);

  /**
   * @brief Writes a buffer of bytes to BLE UART TX.
   */
  size_t write(const uint8_t* buffer, size_t size);

  /**
   * @brief Flushes pending BLE UART transmissions.
   */
  void flush();

private:
  BLEDis  _bledis;   // Device Information Service
  BLEUart _bleuart;  // Nordic UART Service (NUS)

  void startAdvertising();

  static void connectCallback(uint16_t conn_handle);
  static void disconnectCallback(uint16_t conn_handle, uint8_t reason);
};

#endif // BLE_MANAGER_H
