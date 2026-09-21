#pragma once

#include <Arduino.h>

// Detects the robot being picked up and carried somewhere else.
//
// The robot is only ever moved while no conversation is running, which is what
// makes a plain accelerometer threshold enough here: there is no need to model
// the head servos, because they only run during a conversation. The caller
// still skips polling while a gesture is active, so the one idle gesture
// (centring the head on the way to sleep) cannot be mistaken for a carry.
//
// The useful event is the arrival, not the movement, so the detector reports
// once the robot has been still again for a moment: that is when a Wi-Fi scan
// will see the new room rather than the corridor. A knock against the desk is
// ignored, because a real carry accumulates motion over several seconds.
class CarryDetector {
 public:
  // Safe to call even where there is no IMU; the detector then stays silent
  // and the Wi-Fi backstop rescan is what keeps the network choice current.
  bool begin();
  bool available() const { return available_; }

  // Call every loop while idle. Returns true exactly once per completed carry.
  bool poll();

 private:
  // Fast enough to catch a carry, slow enough to stay out of the audio path.
  static constexpr uint32_t SAMPLE_INTERVAL_MS = 50;
  // Change in the acceleration vector between samples, in g. Resting noise on
  // this part sits an order of magnitude below it.
  static constexpr float MOVE_THRESHOLD_G = 0.12f;
  // Total motion a carry has to accumulate before arriving counts as arriving.
  static constexpr uint32_t MIN_MOVE_MS = 1200;
  // Stillness that ends a carry. Also lets the robot be set down and settle
  // before anything measures the radio.
  static constexpr uint32_t SETTLE_MS = 3000;

  bool available_ = false;
  bool have_prev_ = false;
  float prev_x_ = 0.0f;
  float prev_y_ = 0.0f;
  float prev_z_ = 0.0f;
  uint32_t last_sample_ms_ = 0;
  uint32_t last_motion_ms_ = 0;
  uint32_t moving_accum_ms_ = 0;
  bool carrying_ = false;
};
