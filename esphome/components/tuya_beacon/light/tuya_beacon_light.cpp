#include "tuya_beacon_light.h"
#include "esphome/core/log.h"

namespace esphome {
namespace tuya_beacon {

void TuyaBeaconLight::setup() {
  // Schedule the periodic state re-assert. The bulb does not broadcast its real
  // state, so this self-heals any external drift by re-imposing HA's state.
  if (resync_interval_ > 0)
    this->set_interval("resync", resync_interval_, [this]() { this->resync_(); });
}

void TuyaBeaconLight::dump_config() {
  ESP_LOGCONFIG(TAG, "Tuya Beacon Light:");
  ESP_LOGCONFIG(TAG, "  Cold white: %.0f mireds", cold_mireds_);
  ESP_LOGCONFIG(TAG, "  Warm white: %.0f mireds", warm_mireds_);
  if (resync_interval_ > 0)
    ESP_LOGCONFIG(TAG, "  Resync interval: %u ms", resync_interval_);
  else
    ESP_LOGCONFIG(TAG, "  Resync interval: disabled");
}

// Re-broadcast the cached current state. Uses the same delta-tracked values
// that write_state() maintains, so it mirrors exactly what HA last applied.
// Sends are unconditional here — the whole point is to re-impose state even
// when nothing changed, so it does not consult the last_* delta trackers.
void TuyaBeaconLight::resync_() {
  if (!have_last_)
    return;  // nothing applied yet — nothing to re-assert

  ESP_LOGD(TAG, "Periodic resync: re-asserting state to bulb");

  if (!last_on_) {
    beacon_->send_on_off(false);
    return;
  }

  beacon_->send_on_off(true);
  beacon_->send_mode(last_mode_);
  if (last_mode_ == 1) {
    beacon_->send_color_hsv(last_hue_, last_sat_, last_val_);
  } else {
    beacon_->send_brightness(last_bright_);
    beacon_->send_color_temp(last_temp_);
  }
}

light::LightTraits TuyaBeaconLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes(
      {light::ColorMode::RGB, light::ColorMode::COLOR_TEMPERATURE});
  traits.set_min_mireds(cold_mireds_);
  traits.set_max_mireds(warm_mireds_);
  return traits;
}

void TuyaBeaconLight::write_state(light::LightState *state) {
  auto v = state->current_values;
  bool on = v.get_state() > 0.0f && v.is_on();

  // --- off ---
  if (!on) {
    // Only send on→off once; skip if we already sent off last time.
    if (!have_last_ || last_on_) {
      beacon_->send_on_off(false);
      last_on_ = false;
      have_last_ = true;
    }
    return;
  }

  // --- on ---
  if (!have_last_ || !last_on_) {
    beacon_->send_on_off(true);
    last_on_ = true;
  }

  if (v.get_color_mode() == light::ColorMode::RGB) {
    float r = v.get_red(), g = v.get_green(), b = v.get_blue();
    float h, s;
    rgb_to_hs_(r, g, b, &h, &s);
    uint16_t H = (uint16_t) lroundf(h);
    uint8_t S = (uint8_t) lroundf(s * 100.0f);
    // ESPHome keeps brightness in its own field; DP11 val is 0–100.
    uint8_t V = (uint8_t) lroundf(v.get_brightness() * 100.0f);

    // Mode must be sent before colour so the bulb switches its internal channel
    // to colour mode before applying the HSV values.
    if (!have_last_ || last_mode_ != 1) {
      beacon_->send_mode(1);
      last_mode_ = 1;
    }
    if (!have_last_ || H != last_hue_ || S != last_sat_ || V != last_val_) {
      beacon_->send_color_hsv(H, S, V);
      last_hue_ = H;
      last_sat_ = S;
      last_val_ = V;
    }
  } else {
    // ColorMode::COLOR_TEMPERATURE
    //
    // DP3 brightness: ESPHome 0.0–1.0 → bulb 10–1000.
    // The bulb ignores brightness values below 10 (treats them as off).
    uint16_t bright = (uint16_t) (10 + lroundf(v.get_brightness() * 990.0f));

    // DP4 color-temp: bulb 0 = warmest, 1000 = coolest — inverse of the mired
    // scale.  frac=0 means warm (high mireds), frac=1 means cool (low mireds).
    float mired = v.get_color_temperature();
    float frac = (warm_mireds_ - mired) / (warm_mireds_ - cold_mireds_);
    frac = frac < 0.0f ? 0.0f : frac > 1.0f ? 1.0f : frac;
    uint16_t temp = (uint16_t) lroundf(frac * 1000.0f);

    // Mode before brightness/temp for the same reason as in RGB mode.
    if (!have_last_ || last_mode_ != 0) {
      beacon_->send_mode(0);
      last_mode_ = 0;
    }
    if (!have_last_ || bright != last_bright_) {
      beacon_->send_brightness(bright);
      last_bright_ = bright;
    }
    if (!have_last_ || temp != last_temp_) {
      beacon_->send_color_temp(temp);
      last_temp_ = temp;
    }
  }

  have_last_ = true;
}

// Standard HSV hue and saturation extraction from linear RGB.
// Value is not computed here — it comes from LightState::get_brightness().
void TuyaBeaconLight::rgb_to_hs_(float r, float g, float b, float *h, float *s) {
  float mx = std::fmax(r, std::fmax(g, b));
  float mn = std::fmin(r, std::fmin(g, b));
  float d = mx - mn;
  *s = (mx <= 0.0f) ? 0.0f : d / mx;
  float hh = 0.0f;
  if (d > 0.0f) {
    if (mx == r) hh = 60.0f * fmodf((g - b) / d, 6.0f);
    else if (mx == g) hh = 60.0f * (((b - r) / d) + 2.0f);
    else hh = 60.0f * (((r - g) / d) + 4.0f);
    if (hh < 0.0f) hh += 360.0f;
  }
  *h = hh;
}

}  // namespace tuya_beacon
}  // namespace esphome
