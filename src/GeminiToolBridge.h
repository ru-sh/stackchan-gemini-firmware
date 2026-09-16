#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "ToolGatewayClient.h"
#include "EmotionController.h"
#include "ServoGestureController.h"
#include "CameraCapture.h"

class GeminiLiveProbe;
class MemoryStore;

// Bridges Gemini function calls to local robot handlers or the LAN Tool/MCP Gateway.
// This is intentionally small: the gateway does MCP discovery/auth/schema filtering.
class GeminiToolBridge {
 public:
  enum class LocalStatus {
    NotHandled,
    Handled,
    Error,
  };

  GeminiToolBridge(ToolGatewayClient& gateway, EmotionController& emotion,
                   ServoGestureController& servoGestures, CameraCapture& camera)
      : gateway_(gateway), emotion_(emotion), servoGestures_(servoGestures), camera_(camera) {}

  void setGemini(GeminiLiveProbe* gemini) { gemini_ = gemini; }
  void setMemoryStore(MemoryStore* memory) { memory_ = memory; }

  String functionDeclarationsJson();
  // Scheduling hint the Live API requires on responses to tools declared
  // NON_BLOCKING; nullptr for ordinary blocking tools.
  static const char* nonBlockingScheduling(const String& name);
  String handleFunctionCall(const String& name, const JsonVariantConst& args,
                            const String& sessionId = "");

 private:
  ToolGatewayClient& gateway_;
  EmotionController& emotion_;
  ServoGestureController& servoGestures_;
  CameraCapture& camera_;
  GeminiLiveProbe* gemini_ = nullptr;
  MemoryStore* memory_ = nullptr;

  LocalStatus handleLocal(const String& name, const JsonVariantConst& args, JsonDocument& response);
  String errorJson(const String& code, const String& message);
};
