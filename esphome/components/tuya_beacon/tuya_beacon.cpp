#include "tuya_beacon.h"
#include <cstdio>

namespace esphome {
namespace tuya_beacon {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void TuyaBeacon::set_local_key(const std::string &key) {
  // Store the key as raw ASCII bytes — do NOT hex-decode it.
  // "F32108C9D53B45F7" is the literal 16-character string, not 8 bytes of hex.
  for (int i = 0; i < 16 && i < (int) key.size(); i++)
    key_[i] = (uint8_t) key[i];
}

void TuyaBeacon::set_controller_name(const std::string &name) {
  controller_name_ = name;
  // The local name must fit in one AD structure inside a 31-byte advert alongside
  // Flags (3 bytes) and the 18-byte UUID field — that caps the name at 8 chars.
  if (controller_name_.size() > 8)
    controller_name_.resize(8);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void TuyaBeacon::setup() {
  // Crypto self-test: encrypt then immediately decrypt a known block.
  // A key typo or wrong ASCII vs. hex interpretation silently produces bad
  // ciphertext that the bulb ignores forever — catching it here is cheaper.
  uint8_t plain[16], enc[16], back[16];
  for (int i = 0; i < 16; i++) plain[i] = (uint8_t) (i * 7 + 3);
  memcpy(enc, plain, 16);
  xxtea_encrypt_(enc, key_);
  xxtea_decrypt_(enc, key_, back);
  if (memcmp(plain, back, 16) != 0) {
    ESP_LOGE(TAG, "XXTEA self-test FAILED — marking component failed");
    this->mark_failed();
    return;
  }

  // Restore the counter from NVS so reboots don't require a fresh bootstrap.
  // The NVS key is derived from the bulb MAC so multiple bulbs each get their
  // own independent storage slot without namespace collisions.
  pref_ = global_preferences->make_preference<uint32_t>(
      (uint32_t) (mac_address_ & 0xFFFFFFFF) ^ 0x54424332u);
  uint32_t restored = 0;
  if (pref_.load(&restored) && restored != 0) {
    seq_ = restored;
    synced_ = true;
    ESP_LOGD(TAG, "Restored counter from NVS: 0x%06x", seq_);
  } else {
    seq_ = initial_seq_;
    synced_ = false;
    ESP_LOGD(TAG, "Cold start, seed=0x%06x (will bootstrap)", seq_);
  }

  // Advertising parameters for connectable ADV_IND.
  // ADV_TYPE_IND (connectable) is required — the bulb firmware silently
  // discards ADV_NONCONN_IND even with a perfect payload.
  memset(&adv_params_, 0, sizeof(adv_params_));
  adv_params_.adv_int_min = 0x20;
  adv_params_.adv_int_max = 0x40;
  adv_params_.adv_type = ADV_TYPE_IND;
  adv_params_.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
  adv_params_.channel_map = ADV_CHNL_ALL;
  adv_params_.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
}

void TuyaBeacon::dump_config() {
  ESP_LOGCONFIG(TAG, "Tuya Beacon:");
  ESP_LOGCONFIG(TAG, "  MAC:             %012llx", (unsigned long long) mac_address_);
  ESP_LOGCONFIG(TAG, "  Controller name: %s", controller_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Counter:         0x%06x (%s)", seq_,
                synced_ ? "synced" : "bootstrapping");
}

// ---------------------------------------------------------------------------
// ESPBTDeviceListener — status-beacon counter self-seed
//
// The bulb broadcasts a 0xD007 manufacturer-data beacon ~2/s.  Its wire layout:
//
//   [0]     0x04         fixed header byte
//   [1:7]   MAC[6]       bulb's own MAC address
//   [7:11]  sn[4 BE]     fast status sequence counter (increments every broadcast)
//   [11:27] enc[16]      XXTEA-encrypted status payload
//
// Inside enc (after decryption):
//   pt[4:8]   command counter (big-endian uint32)  — what we need
//   pt[8:11]  0xff 0xff 0xff marker — present only on counter-report frames
//
// The cleartext sn is a separate, faster counter unrelated to commands.
// ---------------------------------------------------------------------------

bool TuyaBeacon::parse_device(const esp32_ble_tracker::ESPBTDevice &device) {
  if (device.address_uint64() != mac_address_)
    return false;

  for (auto &md : device.get_manufacturer_datas()) {
    if (!(md.uuid == esp32_ble_tracker::ESPBTUUID::from_uint16(0xD007)))
      continue;

    const auto &d = md.data;
    // Minimum size: 1 (header) + 6 (MAC) + 4 (sn) + 16 (enc) = 27 bytes.
    if (d.size() < 27 || d[0] != 0x04)
      continue;

    uint8_t enc[16], pt[16];
    for (int i = 0; i < 16; i++) enc[i] = d[11 + i];  // enc starts at offset 11
    xxtea_decrypt_(enc, key_, pt);

    if (pt[8] == 0xff && pt[9] == 0xff && pt[10] == 0xff) {
      uint32_t c = ((uint32_t) pt[4] << 24) | ((uint32_t) pt[5] << 16) |
                   ((uint32_t) pt[6] << 8) | pt[7];
      if (!have_counter_ || c != bulb_counter_)
        ESP_LOGD(TAG, "Status beacon counter: 0x%06x", c);
      bulb_counter_ = c;
      have_counter_ = true;
    }
  }
  // Return false: we never "consume" the advertisement — other listeners
  // (e.g. bluetooth_proxy) should still see it.
  return false;
}

// ---------------------------------------------------------------------------
// High-level DP commands
//
// DP wire format (Tuya compact): dp_id | dp_type | dp_len[2 BE] | value[dp_len]
//
// The body arrays below are the raw DP bytes that go into the plaintext payload.
// See build_ct_() for how the full 16-byte plaintext is assembled.
// ---------------------------------------------------------------------------

void TuyaBeacon::send_on_off(bool on) {
  // DP1: boolean (type 0x01), length 1, value 0x00/0x01
  uint8_t body[4] = {0x01, 0x01, 0x01, (uint8_t) (on ? 1 : 0)};
  send_dp_(body, sizeof(body));
}

void TuyaBeacon::send_mode(uint8_t mode) {
  // DP2: enum (type 0x04), length 1 — must be sent before brightness or colour
  // so the bulb knows which channel to apply the subsequent values to.
  uint8_t body[4] = {0x02, 0x04, 0x01, mode};
  send_dp_(body, sizeof(body));
}

void TuyaBeacon::send_brightness(uint16_t level) {
  // DP3: value (type 0x02), length 4, big-endian uint32.
  // Range 10–1000; bulb ignores values below 10 (treat as 0 = off).
  ESP_LOGD(TAG, "DP3 brightness = %u", level);
  uint8_t body[7] = {0x03, 0x02, 0x04, 0, 0,
                     (uint8_t) (level >> 8), (uint8_t) (level & 0xFF)};
  send_dp_(body, sizeof(body));
}

void TuyaBeacon::send_color_temp(uint16_t temp) {
  // DP4: value (type 0x02), length 4, big-endian uint32.
  // Scale: 0 = warmest (high mireds), 1000 = coolest (low mireds).
  // This is intentionally inverted relative to the mired scale.
  ESP_LOGD(TAG, "DP4 color-temp = %u", temp);
  uint8_t body[7] = {0x04, 0x02, 0x04, 0, 0,
                     (uint8_t) (temp >> 8), (uint8_t) (temp & 0xFF)};
  send_dp_(body, sizeof(body));
}

void TuyaBeacon::send_color_hsv(uint16_t hue, uint8_t sat, uint8_t val) {
  // DP11 (0x0b): raw (type 0x00), length 4 — hue BE16 (0–360), sat (0–100), val (0–100).
  ESP_LOGD(TAG, "DP11 colour H=%u S=%u V=%u", hue, sat, val);
  uint8_t body[7] = {0x0b, 0x00, 0x04,
                     (uint8_t) (hue >> 8), (uint8_t) (hue & 0xFF), sat, val};
  send_dp_(body, sizeof(body));
}

// ---------------------------------------------------------------------------
// BLE advertising internals
// ---------------------------------------------------------------------------

// Broadcast the command for seq `s`, holding the advert for `ms` milliseconds.
//
// The stop→config→start cycle is required on every call because
// esp_ble_gap_config_adv_data_raw() does not update a live advertisement —
// the new payload only takes effect after a fresh start_advertising() call.
// The 15 ms delay after config gives the BLE stack time to apply the new data.
void TuyaBeacon::emit_(uint32_t s, const uint8_t *body, uint8_t blen, uint32_t ms) {
  uint8_t ct[16], adv[31];
  build_ct_(s & 0xFFFFFF, body, blen, ct);
  uint8_t n = build_adv_(ct, adv);
  esp_ble_gap_stop_advertising();
  esp_ble_gap_config_adv_data_raw(adv, n);
  delay(15);
  esp_ble_gap_start_advertising(&adv_params_);
  // Feed the WDT in chunks — a single delay(TRACK_MS) would trip the task WDT.
  for (uint32_t t = 0; t < ms; t += 120) {
    delay(ms - t > 120 ? 120 : ms - t);
    App.feed_wdt();
  }
  esp_ble_gap_stop_advertising();
}

// Decide between bootstrap and track, then call emit_().
void TuyaBeacon::send_dp_(const uint8_t *body, uint8_t blen) {
  if (!synced_) {
    // BOOTSTRAP: we don't know the bulb's exact counter yet.
    // If the status beacon has already been seen, seed from that value so the
    // sweep starts at the right place.  Otherwise use initial_seq_.
    // Then walk TB_CATCH steps at STEP_MS each — the bulb accepts the one
    // whose seq is exactly +1 above its stored counter and ignores the rest.
    if (have_counter_)
      seq_ = bulb_counter_;
    ESP_LOGI(TAG, "Bootstrap catch 0x%06x..0x%06x (have_ctr=%d)",
             seq_ + 1, seq_ + TB_CATCH, (int) have_counter_);
    for (uint32_t s = seq_ + 1; s <= seq_ + TB_CATCH; s++)
      emit_(s, body, blen, STEP_MS);
    seq_ = (seq_ + TB_CATCH) & 0xFFFFFF;
    synced_ = true;
    pref_.save(&seq_);
    ESP_LOGI(TAG, "Bootstrap done, counter now 0x%06x", seq_);
    return;
  }

  // TRACK: we are the sole controller and increment by exactly 1 each time.
  // If the status beacon shows the bulb is ahead (e.g. app was used), fast-
  // forward seq_ to match before adding 1 — otherwise the bulb rejects us.
  if (have_counter_ && bulb_counter_ > seq_) {
    ESP_LOGI(TAG, "App-drift resync: 0x%06x -> 0x%06x", seq_, bulb_counter_);
    seq_ = bulb_counter_;
  }
  uint32_t s = (seq_ + 1) & 0xFFFFFF;
  emit_(s, body, blen, TRACK_MS);
  seq_ = s;
  pref_.save(&seq_);
}

// Assemble the 16-byte DOWNLOAD_DP plaintext and encrypt it in-place.
//
// Plaintext layout:
//   [0]     0x07        opcode: DOWNLOAD_DP
//   [1]     0x24        fixed frame marker
//   [2:5]   seq[3 BE]   24-bit anti-replay counter, big-endian
//   [5:14]  body        DP payload (up to 9 bytes)
//   [14]    CRC-8       over bytes [1:14]
//   [15]    0x00        padding
void TuyaBeacon::build_ct_(uint32_t seq, const uint8_t *body, uint8_t blen,
                            uint8_t ct[16]) const {
  uint8_t pt[16];
  memset(pt, 0, 16);
  pt[0] = 0x07;  // DOWNLOAD_DP
  pt[1] = 0x24;  // frame marker
  pt[2] = (seq >> 16) & 0xFF;
  pt[3] = (seq >> 8) & 0xFF;
  pt[4] = seq & 0xFF;
  for (uint8_t i = 0; i < blen && i < 9; i++) pt[5 + i] = body[i];
  pt[14] = crc8_(pt + 1, 13);  // CRC over [1:14]; opcode byte [0] is excluded
  xxtea_encrypt_(pt, key_);
  memcpy(ct, pt, 16);
}

// Assemble the 31-byte raw legacy connectable advertisement.
//
// Three AD structures, packed in order:
//   02 01 06                  Flags: LE General Discoverable + no BR/EDR
//   <len> 09 <name bytes>     Complete Local Name
//   11 07 <ct[16]>            Complete 128-bit Service UUID (forward / on-air order)
//
// The ciphertext goes into the Service UUID field directly in forward byte order.
// (Android's ParcelUuid would require byte-reversal, but on ESP32 we write raw.)
uint8_t TuyaBeacon::build_adv_(const uint8_t ct[16], uint8_t adv[31]) const {
  uint8_t *p = adv;
  *p++ = 0x02;
  *p++ = 0x01;
  *p++ = 0x06;
  *p++ = (uint8_t) (controller_name_.size() + 1);
  *p++ = 0x09;
  memcpy(p, controller_name_.data(), controller_name_.size());
  p += controller_name_.size();
  *p++ = 0x11;
  *p++ = 0x07;
  memcpy(p, ct, 16);
  p += 16;
  return (uint8_t) (p - adv);
}

// ---------------------------------------------------------------------------
// XXTEA cipher — big-endian, 4 × uint32 block, 16-byte key, 19 rounds
//
// Standard XXTEA (Wheeler & Needham, 1998) with words packed big-endian to
// match the PHY6230 firmware's byte order.  The number of rounds (19) is the
// standard formula: 6 + 52/n where n=4, rounded to the nearest integer.
//
// Key: the 16 ASCII bytes of the localKey string, split into 4 × uint32 BE.
// Do not hex-decode the key — see set_local_key().
// ---------------------------------------------------------------------------

// Read a 4-byte big-endian word.
static inline uint32_t rd32(const uint8_t *p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

// Write a 4-byte big-endian word.
static inline void wr32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

void TuyaBeacon::xxtea_encrypt_(uint8_t block[16], const uint8_t key[16]) {
  uint32_t v[4], k[4];
  for (int i = 0; i < 4; i++) { v[i] = rd32(block + 4 * i); k[i] = rd32(key + 4 * i); }
  uint32_t sum = 0, z = v[3];
  for (int q = 0; q < 19; q++) {
    sum += XXTEA_DELTA;
    uint32_t e = (sum >> 2) & 3;
    for (int p = 0; p < 4; p++) {
      uint32_t y = v[(p + 1) & 3];
      uint32_t mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (k[(p & 3) ^ e] ^ z));
      v[p] += mx; z = v[p];
    }
  }
  for (int i = 0; i < 4; i++) wr32(block + 4 * i, v[i]);
}

void TuyaBeacon::xxtea_decrypt_(const uint8_t in[16], const uint8_t key[16], uint8_t out[16]) {
  uint32_t v[4], k[4];
  for (int i = 0; i < 4; i++) { v[i] = rd32(in + 4 * i); k[i] = rd32(key + 4 * i); }
  // Decryption starts with sum at the final value after all encrypt rounds.
  uint32_t sum = (uint32_t) (19 * XXTEA_DELTA), y = v[0];
  for (int q = 0; q < 19; q++) {
    uint32_t e = (sum >> 2) & 3;
    for (int p = 3; p > 0; p--) {
      uint32_t z = v[p - 1];
      y = v[p] -= (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (k[(p & 3) ^ e] ^ z));
    }
    uint32_t z = v[3];
    y = v[0] -= (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (k[e] ^ z));
    sum -= XXTEA_DELTA;
  }
  for (int i = 0; i < 4; i++) wr32(out + 4 * i, v[i]);
}

// CRC-8: poly 0x07, init 0x00, no input/output reflection.
// Applied over plaintext bytes [1:14] — the opcode byte [0] is excluded
// because it is fixed (0x07) and the bulb's firmware skips it in its own check.
uint8_t TuyaBeacon::crc8_(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t) ((crc << 1) ^ 0x07) : (uint8_t) (crc << 1);
  }
  return crc;
}

}  // namespace tuya_beacon
}  // namespace esphome
