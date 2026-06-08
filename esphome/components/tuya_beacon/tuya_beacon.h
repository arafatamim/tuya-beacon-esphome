#pragma once
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include <cstring>
#include <cstdint>
#include <string>

extern "C" {
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
}

// ESPHome logging macros must come after IDF headers to win any macro conflicts.
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace tuya_beacon {

// TuyaBeacon controls a Tuya Beacon-protocol RGBCW bulb over BLE advertising.
//
// Control is entirely one-way: the bulb never accepts a GATT connection — it
// only scans advertisements.  To send a command we broadcast a single
// connectable ADV_IND carrying the XXTEA-encrypted command payload in the
// 128-bit Service UUID field.  No acknowledgement is ever received.
//
// Anti-replay counter
// -------------------
// The bulb only accepts a command whose seq is just above its stored counter.
// The counter is persisted to NVS after every successful command so reboots
// are instant.  The bulb continuously broadcasts a 0xD007 status beacon whose
// encrypted payload carries its live counter; parse_device() decrypts each
// beacon and keeps bulb_counter_ current so any drift (e.g. app use) is
// detected and corrected before the next command is sent.
//
// Two sending modes
// -----------------
//  Bootstrap (synced_ == false): on the first command after a cold start we
//    sweep a range of counters (seq+1 .. seq+TB_CATCH) at STEP_MS dwell each.
//    This guarantees we cross the bulb's counter regardless of minor seed
//    inaccuracy, then locks in the new counter.
//
//  Track (synced_ == true): every subsequent command is a single emit at
//    counter+1 held for TRACK_MS.  If the status beacon shows the bulb is
//    ahead (app use), seq_ is updated first.

class TuyaBeacon : public Component, public esp32_ble_tracker::ESPBTDeviceListener {
 public:
  // ----- configuration setters (called by generated code) -----
  void set_local_key(const std::string &key);
  void set_controller_name(const std::string &name);
  void set_mac_address(uint64_t mac) { mac_address_ = mac; }
  void set_initial_seq(uint32_t seq) { initial_seq_ = seq; }

  // ----- ESPHome lifecycle -----
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ----- ESPBTDeviceListener: status-beacon counter self-seed -----
  bool parse_device(const esp32_ble_tracker::ESPBTDevice &device) override;

  // ----- high-level DP commands (called by platform components) -----
  // Each call is blocking (delay + WDT feed) — safe on Arduino loop task.
  void send_on_off(bool on);
  void send_mode(uint8_t mode);           // 0 = white, 1 = colour
  void send_brightness(uint16_t level);   // 10–1000
  void send_color_temp(uint16_t temp);    // 0 (warm) – 1000 (cool)
  void send_color_hsv(uint16_t hue, uint8_t sat, uint8_t val);

 protected:
  static constexpr const char *const TAG = "tuya_beacon";

  // XXTEA block cipher, big-endian word packing, 16-byte key and block.
  static void xxtea_encrypt_(uint8_t block[16], const uint8_t key[16]);
  static void xxtea_decrypt_(const uint8_t in[16], const uint8_t key[16], uint8_t out[16]);

  // CRC-8: poly 0x07, init 0x00, no reflection — covers plaintext bytes [1:14].
  static uint8_t crc8_(const uint8_t *data, size_t len);

  // Build the 16-byte DOWNLOAD_DP ciphertext for a given seq + DP body.
  void build_ct_(uint32_t seq, const uint8_t *body, uint8_t blen, uint8_t ct[16]) const;

  // Assemble the 31-byte raw legacy connectable advertisement and return its length.
  uint8_t build_adv_(const uint8_t ct[16], uint8_t adv[31]) const;

  // Broadcast one seq for `ms` milliseconds, feeding the WDT every 120 ms.
  void emit_(uint32_t s, const uint8_t *body, uint8_t blen, uint32_t ms);

  // Bootstrap-or-track wrapper: resolves counter sync, then calls emit_.
  void send_dp_(const uint8_t *body, uint8_t blen);

  // ----- configuration -----
  uint8_t key_[16]{};        // localKey as 16 raw ASCII bytes
  std::string controller_name_;
  uint64_t mac_address_{0};
  uint32_t initial_seq_{1};  // used only on cold start (empty NVS)

  // ----- runtime state -----
  esp_ble_adv_params_t adv_params_{};
  ESPPreferenceObject pref_;   // NVS slot for seq_ persistence
  uint32_t seq_{1};            // last seq sent (or the bootstrap seed)
  bool synced_{false};         // false = bootstrap mode, true = track mode
  uint32_t bulb_counter_{0};   // most recent counter read from status beacon
  bool have_counter_{false};   // true once we have seen at least one status beacon

  // Standard XXTEA constant: floor(2^32 / golden ratio).
  static constexpr uint32_t XXTEA_DELTA = 0x9E3779B9UL;

  // Bootstrap sweep width: how many counter values to try on cold start.
  // 0x40 (64) covers typical seed inaccuracy while keeping sweep time short.
  static constexpr uint32_t TB_CATCH = 0x40;

  // Dwell time per seq during the bootstrap sweep.  250 ms gives the bulb
  // enough air time to receive and validate each individual advertisement.
  static constexpr uint32_t STEP_MS = 250;

  // Dwell time for a single tracked command.  Held long enough that the bulb
  // reliably hears the advert even with BLE scanner duty competing for the radio.
  static constexpr uint32_t TRACK_MS = 1200;
};

}  // namespace tuya_beacon
}  // namespace esphome
