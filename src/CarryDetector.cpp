#include "CarryDetector.h"

#include <M5Unified.h>
#include <math.h>

bool CarryDetector::begin() {
  available_ = M5.Imu.isEnabled();
  Serial.printf("CarryDetector: imu=%s\n", available_ ? "ready" : "absent");
  return available_;
}

bool CarryDetector::poll() {
  if (!available_) return false;

  const uint32_t now = millis();
  const uint32_t since = now - last_sample_ms_;
  if (since < SAMPLE_INTERVAL_MS) return false;
  last_sample_ms_ = now;

  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
  if (!have_prev_) {
    prev_x_ = ax;
    prev_y_ = ay;
    prev_z_ = az;
    have_prev_ = true;
    return false;
  }

  // Compare against the previous sample rather than against gravity, so the
  // reading does not depend on which way up the robot is being held.
  const float dx = ax - prev_x_;
  const float dy = ay - prev_y_;
  const float dz = az - prev_z_;
  prev_x_ = ax;
  prev_y_ = ay;
  prev_z_ = az;

  if (sqrtf(dx * dx + dy * dy + dz * dz) > MOVE_THRESHOLD_G) {
    if (!carrying_) {
      carrying_ = true;
      moving_accum_ms_ = 0;
    }
    // Cap the contribution so a gap in polling cannot make a single bump look
    // like a long carry.
    moving_accum_ms_ += since < SAMPLE_INTERVAL_MS * 4 ? since : SAMPLE_INTERVAL_MS;
    last_motion_ms_ = now;
    return false;
  }

  if (!carrying_) return false;
  if (now - last_motion_ms_ < SETTLE_MS) return false;

  const uint32_t moved_ms = moving_accum_ms_;
  const bool carried = moved_ms >= MIN_MOVE_MS;
  carrying_ = false;
  moving_accum_ms_ = 0;
  if (carried) {
    Serial.printf("CarryDetector: settled after %lu ms of motion\n",
                  static_cast<unsigned long>(moved_ms));
  }
  return carried;
}
