#pragma once
#include "esphome/core/component.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "../tuya_beacon.h"
#include <cmath>

namespace esphome {
namespace tuya_beacon {

// TuyaBeaconLight bridges ESPHome's light abstraction to TuyaBeacon DP commands.
//
// Supported color modes:
//   RGB              → DP2 mode=1 + DP11 HSV colour
//   COLOR_TEMPERATURE → DP2 mode=0 + DP3 brightness + DP4 color-temp
//
// Each write_state() call delta-sends: only DPs whose values have actually
// changed are transmitted.  This matters because each DP command is a
// blocking BLE broadcast (~1.2 s) — sending redundant DPs on every ESPHome
// state update would make the light feel sluggish.

class TuyaBeaconLight : public Component, public light::LightOutput {
 public:
  // ----- configuration setters -----
  void set_beacon(TuyaBeacon *beacon) { beacon_ = beacon; }
  void set_cold_white_temperature(float t) { cold_mireds_ = t; }
  void set_warm_white_temperature(float t) { warm_mireds_ = t; }
  void set_resync_interval(uint32_t ms) { resync_interval_ = ms; }

  // ----- ESPHome lifecycle -----
  float get_setup_priority() const override { return setup_priority::DATA; }
  void setup() override;
  void dump_config() override;

  // ----- LightOutput interface -----
  light::LightTraits get_traits() override;
  void write_state(light::LightState *state) override;

 protected:
  static constexpr const char *const TAG = "tuya_beacon_light";

  // Re-broadcast the cached current state to the bulb. Because the bulb never
  // reports its real state, this is the only way to correct drift introduced
  // by external changes (Tuya app, wall switch, power-cycle): periodically
  // re-impose what Home Assistant believes the state is. Driven by a timer set
  // up in setup() at resync_interval_ (0 disables).
  void resync_();

  TuyaBeacon *beacon_{nullptr};

  // How often to re-assert state onto the bulb, in ms (0 = never).
  uint32_t resync_interval_{600000};

  // Color temperature endpoints in mireds, configurable via YAML.
  // Defaults match common RGBCW bulb specs (153 mireds ≈ 6500 K cool,
  // 500 mireds ≈ 2000 K warm).
  float cold_mireds_{153.0f};
  float warm_mireds_{500.0f};

  // Last-sent values for delta comparison.
  // have_last_ is false until the very first write_state() call so that the
  // first command always sends all relevant DPs regardless of their values.
  bool last_on_{false};
  bool have_last_{false};
  uint8_t last_mode_{0xFF};
  uint16_t last_bright_{0xFFFF};
  uint16_t last_temp_{0xFFFF};
  uint16_t last_hue_{0xFFFF};
  uint8_t last_sat_{0xFF};
  uint8_t last_val_{0xFF};

  // Extract hue (0–360) and saturation (0.0–1.0) from linear RGB (0.0–1.0).
  // Value (brightness) is handled separately via the LightState brightness field.
  static void rgb_to_hs_(float r, float g, float b, float *h, float *s);
};

}  // namespace tuya_beacon
}  // namespace esphome
