#pragma once

#include <Arduino.h>

// Non-blocking robot-side emotion/effect preset controller.
// Gemini should call high-level emotion names; this controller owns smooth LED effects.
class EmotionController {
 public:
  void begin();
  bool setEmotion(const String& emotion);
  const String& currentEmotion() const { return current_; }
  void loop();

 private:
  enum class Mode {
    Neutral,
    // Woken, but the Gemini session is not up yet, so speaking now would be
    // missed. Deliberately distinct from Thinking, which means Gemini has the
    // question and is working on it.
    Connecting,
    Listening,
    Speaking,
    Thinking,
    Looking,
    Happy,
    Angry,
    Found,
    Error,
    Sleep,
  };

  Mode mode_ = Mode::Neutral;
  String current_ = "neutral";
  uint32_t lastFrameMs_ = 0;
  uint32_t lastFaceMs_ = 0;
  uint32_t sleepStartedMs_ = 0;
  uint16_t frame_ = 0;
  bool sleepDisplayOff_ = false;
  uint8_t savedDisplayBrightness_ = 127;

  static Mode parseMode(const String& emotion, String& normalized);
  static void hsvToRgb(uint8_t h, uint8_t s, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b);
  static uint8_t wave8(uint16_t x, uint8_t minValue, uint8_t maxValue);
  static uint8_t scale8(uint8_t value, uint8_t scale);
  static bool externalPowerPresent();
  bool sleepVisualOffDue() const;
  void enterSleepDisplayOff(bool externalPower);
  void restoreSleepDisplay();
  void setLed(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
  void setAll(uint8_t r, uint8_t g, uint8_t b);
  void refresh();
  void drawLabel();
  void renderFace();
  void renderNeutral();
  void renderConnecting();
  void renderListening();
  void renderSpeaking();
  void renderThinking();
  void renderLooking();
  void renderHappy();
  void renderAngry();
  void renderFound();
  void renderError();
  void renderSleep();
};
