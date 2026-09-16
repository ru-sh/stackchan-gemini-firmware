#include "GeminiToolBridge.h"
#include "GeminiLiveProbe.h"
#include "MemoryStore.h"
#include <M5Unified.h>
#include <M5StackChan.h>

const char* GeminiToolBridge::nonBlockingScheduling(const String& name) {
  // INTERRUPT: the picture is what the user just asked about, so it should cut
  // in. WHEN_IDLE: a gateway answer can wait for a natural pause.
  if (name == "look_with_camera") return "INTERRUPT";
  if (name == "ask_hermes") return "WHEN_IDLE";
  return nullptr;
}

String GeminiToolBridge::functionDeclarationsJson() {
  // Keep tool declarations deterministic: do not block Gemini setup on a LAN
  // /tools fetch. Local robot tools are handled here; gateway tools still go
  // through gateway_.callTool().
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  auto noArgTool = [&](const char* name, const char* description) {
    auto tool = arr.add<JsonObject>();
    tool["name"] = name;
    tool["description"] = description;
    auto params = tool["parameters"].to<JsonObject>();
    params["type"] = "object";
    params["properties"].to<JsonObject>();
  };

  noArgTool("get_robot_status", "Get StackChan robot status: battery, network, state, and local memory health.");
  noArgTool("ping_gateway", "Check that the home-server StackChan Tool Gateway is reachable.");
  noArgTool("get_server_time", "Get current Unix time from the home server running the gateway.");
  noArgTool("ha_get_light_states", "Get Home Assistant states for StackChan-allowlisted lights.");

  auto haControl = arr.add<JsonObject>();
  haControl["name"] = "ha_control_light";
  haControl["description"] = "Control one allowlisted Home Assistant light. Use only when the user explicitly asks to change a light.";
  auto hcParams = haControl["parameters"].to<JsonObject>();
  hcParams["type"] = "object";
  auto hcProps = hcParams["properties"].to<JsonObject>();
  hcProps["entity_id"]["type"] = "string";
  hcProps["entity_id"]["description"] = "Allowlisted HA light entity_id.";
  hcProps["action"]["type"] = "string";
  hcProps["action"]["description"] = "on, off, or toggle.";
  hcProps["brightness_pct"]["type"] = "integer";
  hcProps["brightness_pct"]["minimum"] = 1;
  hcProps["brightness_pct"]["maximum"] = 100;
  hcParams["required"].add("entity_id");
  hcParams["required"].add("action");

  auto searchMemory = arr.add<JsonObject>();
  searchMemory["name"] = "search_memory";
  searchMemory["description"] = "Time-aware local memory search over active context, safe facts, and semantic summaries. Use for ordinary recall questions. It groups by observed time/session, prefers recent results by default, never exposes raw archived dialogues, and never exposes private secrets/PINs/passwords.";
  auto smParams = searchMemory["parameters"].to<JsonObject>();
  smParams["type"] = "object";
  auto smProps = smParams["properties"].to<JsonObject>();
  smProps["query"]["type"] = "string";
  smProps["query"]["description"] = "Memory search query.";
  smParams["required"].add("query");

  auto rememberPrivate = arr.add<JsonObject>();
  rememberPrivate["name"] = "remember_private_memory";
  rememberPrivate["description"] = "Store a private local memory such as a PIN, code, password, token, address, or other user-explicit secret. Use only when the user explicitly asks you to remember it. This stays on the robot SD card and must never be sent to Hermes/gateway or ordinary search_memory.";
  auto rpmParams = rememberPrivate["parameters"].to<JsonObject>();
  rpmParams["type"] = "object";
  auto rpmProps = rpmParams["properties"].to<JsonObject>();
  rpmProps["kind"]["type"] = "string";
  rpmProps["kind"]["description"] = "Type, e.g. private_code, private_pin, private_password, private_address, private_note.";
  rpmProps["label"]["type"] = "string";
  rpmProps["label"]["description"] = "Short non-secret label describing what this value is for.";
  rpmProps["value"]["type"] = "string";
  rpmProps["value"]["description"] = "The exact private value the user explicitly asked to remember.";
  rpmParams["required"].add("value");

  auto recallPrivate = arr.add<JsonObject>();
  recallPrivate["name"] = "recall_private_memory";
  recallPrivate["description"] = "Recall a private local memory only when the user explicitly asks for that saved private value. Never call this for ordinary conversation, never forward its result to external tools, and do not expose it unless the user clearly requested recall.";
  auto rclParams = recallPrivate["parameters"].to<JsonObject>();
  rclParams["type"] = "object";
  auto rclProps = rclParams["properties"].to<JsonObject>();
  rclProps["query"]["type"] = "string";
  rclProps["query"]["description"] = "Short label/query, e.g. 'PIN', 'door code', 'wifi password'. Empty means latest private memory.";

  auto askHermes = arr.add<JsonObject>();
  askHermes["name"] = "ask_hermes";
  askHermes["description"] = "Create a safe structured request for the configured external assistant gateway only for explicit external help. Never use for 'remember this' or private/sensitive content such as PINs/passwords/tokens; those stay local.";
  auto ahParams = askHermes["parameters"].to<JsonObject>();
  ahParams["type"] = "object";
  auto ahProps = ahParams["properties"].to<JsonObject>();
  ahProps["question"]["type"] = "string";
  ahProps["question"]["description"] = "Question or task for Hermes. Do not include passwords, tokens, or secrets.";
  ahProps["urgency"]["type"] = "string";
  ahProps["urgency"]["description"] = "low, normal, or high.";
  ahParams["required"].add("question");
  // Gateway round-trips are slow; let the model keep talking while it waits and
  // deliver the answer when it lands rather than stalling the turn.
  askHermes["behavior"] = "NON_BLOCKING";

  auto setEmotion = arr.add<JsonObject>();
  setEmotion["name"] = "set_emotion";
  setEmotion["description"] = "Set StackChan face/emotion state.";
  auto seParams = setEmotion["parameters"].to<JsonObject>();
  seParams["type"] = "object";
  auto seProps = seParams["properties"].to<JsonObject>();
  seProps["emotion"]["type"] = "string";
  seProps["emotion"]["description"] = "One of neutral,happy,thinking,looking,speaking,found,error,sleep.";
  seParams["required"].add("emotion");

  auto servo = arr.add<JsonObject>();
  servo["name"] = "servo_gesture";
  servo["description"] = "Queue a safe non-blocking StackChan head gesture. Returns immediately; the robot moves asynchronously while conversation continues.";
  auto sgParams = servo["parameters"].to<JsonObject>();
  sgParams["type"] = "object";
  auto sgProps = sgParams["properties"].to<JsonObject>();
  sgProps["gesture"]["type"] = "string";
  sgProps["gesture"]["description"] = "One of center_head,look_at_user,look_left_small,look_right_small,look_left,look_right,look_up,look_down,look_top_left,look_top_right,search_left_wide,search_left,search_center,search_right,search_right_wide,nod_yes,shake_no,curious_tilt,speaking_micro_motion,stop_motion. Use search_* only for deliberate visual search sectors; they keep pitch slightly raised for camera coverage. Use look_left/look_right for explicit turn requests; use *_small and speaking_micro_motion for subtle emotions. For phrases like 'a little left' or custom smooth motion, prefer head_motion.";
  sgParams["required"].add("gesture");

  auto motion = arr.add<JsonObject>();
  motion["name"] = "head_motion";
  motion["description"] = "Create a safe smooth StackChan head motion on the fly. Use this when the user asks for a custom amount such as 'a little left', 'slightly up', or to keep looking toward someone after a camera search. Coordinates are deltas from the robot's current look anchor: x_deg negative means robot's left, positive means robot's right; y_deg positive means up, negative means down. Firmware clamps to safe ranges, executes asynchronously, and the final keyframe becomes the new look anchor.";
  auto hmParams = motion["parameters"].to<JsonObject>();
  hmParams["type"] = "object";
  auto hmProps = hmParams["properties"].to<JsonObject>();
  hmProps["motion_name"]["type"] = "string";
  hmProps["motion_name"]["description"] = "Short descriptive name, e.g. slight_left, curious_scan.";
  auto steps = hmProps["steps"].to<JsonObject>();
  steps["type"] = "array";
  steps["description"] = "1 to 6 keyframes, each relative to the current look anchor. For a small left turn use one step like {x_deg:-12,y_deg:0,speed:500,hold_ms:500}. For a smooth expressive sequence, include multiple keyframes and usually end at {x_deg:0,y_deg:0} to preserve the current anchor.";
  steps["minItems"] = 1;
  steps["maxItems"] = 6;
  auto item = steps["items"].to<JsonObject>();
  item["type"] = "object";
  auto ip = item["properties"].to<JsonObject>();
  ip["x_deg"]["type"] = "number";
  ip["x_deg"]["description"] = "Yaw delta from the current look anchor. Safe useful range -65..65. Negative=left, positive=right.";
  ip["y_deg"]["type"] = "number";
  ip["y_deg"]["description"] = "Pitch delta from the current look anchor. Safe useful range -6..40. Positive=up, negative=down.";
  ip["speed"]["type"] = "integer";
  ip["speed"]["description"] = "Servo speed 100..700 for normal motions; lower is softer.";
  ip["hold_ms"]["type"] = "integer";
  ip["hold_ms"]["description"] = "How long to wait before next keyframe, 100..1500 ms.";
  hmParams["required"].add("steps");

  auto lookCamera = arr.add<JsonObject>();
  lookCamera["name"] = "look_with_camera";
  lookCamera["description"] = "Take exactly one still snapshot with StackChan's camera and send that image into the current Gemini Live conversation for visual analysis. Use when the user asks what you see, to look around, read the scene, identify visible objects, or check whether a searched target is visible in the current sector. While this tool runs, stay silent and do not move the head. For search tasks, take only one photo per sector; if the target is not visible, move to the next search_* sector before calling this tool again.";
  auto lcParams = lookCamera["parameters"].to<JsonObject>();
  lcParams["type"] = "object";
  auto lcProps = lcParams["properties"].to<JsonObject>();
  lcProps["question"]["type"] = "string";
  lcProps["question"]["description"] = "Short visual question, e.g. 'what is in front of me?' or 'what is on the table?'";
  // Capture plus upload takes seconds; NON_BLOCKING keeps the session alive
  // instead of leaving the model waiting on a blocking call.
  lookCamera["behavior"] = "NON_BLOCKING";

  auto endSession = arr.add<JsonObject>();
  endSession["name"] = "end_session";
  endSession["description"] = "End the current continuous StackChan conversation when the user says goodbye, good night, stop listening, or otherwise clearly wants the session to finish. The robot stops listening after its final response and enters sleep/idle mode.";
  auto esParams = endSession["parameters"].to<JsonObject>();
  esParams["type"] = "object";
  auto esProps = esParams["properties"].to<JsonObject>();
  esProps["reason"]["type"] = "string";
  esProps["reason"]["description"] = "Brief reason, e.g. user_said_goodbye, bedtime, stop_listening.";

  String out;
  serializeJson(arr, out);
  return out;
}

String GeminiToolBridge::handleFunctionCall(const String& name, const JsonVariantConst& args,
                                            const String& sessionId) {
  JsonDocument localResponse;
  LocalStatus local = handleLocal(name, args, localResponse);
  if (local == LocalStatus::Handled || local == LocalStatus::Error) {
    String out;
    serializeJson(localResponse, out);
    return out;
  }

  if (name == "ask_hermes") {
    String question = String((const char*)(args["question"] | ""));
    String q = question;
    q.toLowerCase();
    if (q.indexOf("pin") >= 0 || q.indexOf("\xD0\xBF\xD0\xB8\xD0\xBD") >= 0 ||
        q.indexOf("password") >= 0 || q.indexOf("\xD0\xBF\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB") >= 0 ||
        q.indexOf("token") >= 0 || q.indexOf("secret") >= 0 || q.indexOf("\xD1\x81\xD0\xB5\xD0\xBA\xD1\x80\xD0\xB5\xD1\x82") >= 0 ||
        q.indexOf("code") >= 0 || q.indexOf("\xD0\xBA\xD0\xBE\xD0\xB4") >= 0 ||
        q.indexOf("remember") >= 0 || q.indexOf("\xD0\xB7\xD0\xB0\xD0\xBF\xD0\xBE\xD0\xBC") >= 0) {
      JsonDocument blocked;
      blocked["ok"] = false;
      blocked["blocked"] = true;
      blocked["error"] = "private_memory_request_not_forwarded";
      blocked["message"] = "Private remember/secrets/PIN content is not forwarded to Hermes. Acknowledge locally; local transcript memory records the conversation automatically.";
      String out;
      serializeJson(blocked, out);
      return out;
    }
  }

  auto gatewayResult = gateway_.callTool(name, args, sessionId);
  if (gatewayResult.ok) return gatewayResult.body;
  return errorJson(gatewayResult.error.length() ? gatewayResult.error : "gateway_call_failed",
                   gatewayResult.body.length() ? gatewayResult.body : "Tool gateway call failed");
}

GeminiToolBridge::LocalStatus GeminiToolBridge::handleLocal(
    const String& name, const JsonVariantConst& args, JsonDocument& response) {
  if (name == "get_robot_status") {
    response["ok"] = true;
    response["battery_mv"] = M5StackChan.getBatteryVoltage();
    response["battery_ma"] = M5StackChan.getBatteryCurrent();
    response["free_heap"] = ESP.getFreeHeap();
    response["free_psram"] = ESP.getFreePsram();
    response["gateway_enabled"] = gateway_.isEnabled();
    return LocalStatus::Handled;
  }

  if (name == "set_emotion") {
    const char* emotion = args["emotion"] | "neutral";
    emotion_.setEmotion(emotion);
    response["ok"] = true;
    response["emotion"] = emotion_.currentEmotion();
    return LocalStatus::Handled;
  }

  if (name == "search_memory") {
    const char* queryRaw = args["query"] | "";
    String query = String(queryRaw);
    String q = query;
    q.toLowerCase();
    response["ok"] = true;
    response["source"] = "local_active_dialogue_context_only";
    response["query"] = query;

    if (q.indexOf("pin") >= 0 || q.indexOf("\xD0\xBF\xD0\xB8\xD0\xBD") >= 0 ||
        q.indexOf("password") >= 0 || q.indexOf("\xD0\xBF\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB") >= 0 ||
        q.indexOf("token") >= 0 || q.indexOf("secret") >= 0 || q.indexOf("\xD1\x81\xD0\xB5\xD0\xBA\xD1\x80\xD0\xB5\xD1\x82") >= 0 ||
        q.indexOf("code") >= 0 || q.indexOf("\xD0\xBA\xD0\xBE\xD0\xB4") >= 0) {
      response["private_query"] = true;
      response["active_context"] = "";
      response["instruction"] = "search_memory never exposes private values. If and only if the user explicitly asks to recall a saved private code/PIN/password, call recall_private_memory locally instead.";
      return LocalStatus::Handled;
    }

    if (memory_) {
      String hits = memory_->searchMemory(query, 8, 9000);
      response["retrieval_json"] = hits;
      response["raw_dialogues_exposed"] = false;
      response["retrieval_layers"] = "active_context,facts,semantic_summaries";
      response["time_aware"] = true;
    } else {
      response["retrieval_json"] = "";
      response["warning"] = "memory_store_not_attached";
    }
    response["instruction"] = "Use the time-aware local retrieval_json. Prefer active/fresh results; if several dates/sessions are present, answer as separate episodes and do not merge them. Do not request raw archived dialogues or expose private values.";
    return LocalStatus::Handled;
  }

  if (name == "remember_private_memory") {
    if (!memory_) {
      response["ok"] = false;
      response["error"] = "memory_store_not_attached";
      return LocalStatus::Error;
    }
    String kind = String((const char*)(args["kind"] | "private_note"));
    String label = String((const char*)(args["label"] | "private memory"));
    String value = String((const char*)(args["value"] | ""));
    bool ok = memory_->rememberPrivateMemory(kind, label, value);
    response["ok"] = ok;
    response["stored"] = ok;
    response["kind"] = kind;
    response["label"] = label;
    response["recall_policy"] = "explicit_user_request_only_local";
    response["value_echoed"] = false;
    response["message"] = ok ? "Private memory stored locally on SD. Do not repeat the value unless the user explicitly asks to recall it." : "Private memory was not stored.";
    return ok ? LocalStatus::Handled : LocalStatus::Error;
  }

  if (name == "recall_private_memory") {
    if (!memory_) {
      response["ok"] = false;
      response["error"] = "memory_store_not_attached";
      return LocalStatus::Error;
    }
    String query = String((const char*)(args["query"] | ""));
    String kind, label, value, ts;
    bool found = memory_->recallPrivateMemory(query, kind, label, value, ts);
    response["ok"] = found;
    response["private_recall"] = true;
    response["query"] = query;
    if (found) {
      response["kind"] = kind;
      response["label"] = label;
      response["value"] = value;
      response["ts"] = ts;
      response["recall_policy"] = "explicit_user_request_only_local";
      response["external_forwarding_allowed"] = false;
    } else {
      response["error"] = "private_memory_not_found";
      response["message"] = "No matching local private memory was found.";
    }
    return found ? LocalStatus::Handled : LocalStatus::Error;
  }

  if (name == "servo_gesture") {
    const char* gesture = args["gesture"] | "";
    bool queued = servoGestures_.queueGesture(String(gesture));
    response["ok"] = queued;
    if (queued) {
      response["queued"] = gesture;
      response["active"] = servoGestures_.currentGesture();
      response["async"] = true;
      response["anchor_x"] = servoGestures_.anchorX();
      response["anchor_y"] = servoGestures_.anchorY();
    } else {
      response["error"] = "unknown_gesture";
      response["allowed"] = "center_head,look_at_user,look_left_small,look_right_small,look_left,look_right,look_up,look_down,look_top_left,look_top_right,search_left_wide,search_left,search_center,search_right,search_right_wide,nod_yes,shake_no,curious_tilt,speaking_micro_motion,stop_motion";
      return LocalStatus::Error;
    }
    return LocalStatus::Handled;
  }

  if (name == "head_motion") {
    auto clampInt = [](int v, int lo, int hi) -> int {
      if (v < lo) return lo;
      if (v > hi) return hi;
      return v;
    };
    ServoGestureController::Step steps[6]{};
    uint8_t count = 0;
    JsonArrayConst arr = args["steps"].as<JsonArrayConst>();
    for (JsonObjectConst s : arr) {
      if (count >= 6) break;
      float xDeg = s["x_deg"] | 0.0f;
      float yDeg = s["y_deg"] | 0.0f;
      int speed = s["speed"] | 500;
      int holdMs = s["hold_ms"] | 500;
      xDeg = constrain(xDeg, -65.0f, 65.0f);
      yDeg = constrain(yDeg, -6.0f, 40.0f);
      steps[count].x = clampInt(static_cast<int>(xDeg * 10.0f), -650, 650);
      steps[count].y = clampInt(static_cast<int>(yDeg * 10.0f), -60, 400);
      steps[count].speed = clampInt(speed, 100, 700);
      steps[count].holdMs = static_cast<uint16_t>(clampInt(holdMs, 100, 1500));
      steps[count].relative = true;
      count++;
    }
    if (count == 0) {
      response["ok"] = false;
      response["error"] = "missing_steps";
      response["message"] = "head_motion requires 1..6 steps with x_deg/y_deg/speed/hold_ms";
      return LocalStatus::Error;
    }
    String motionName = String((const char*)(args["motion_name"] | "custom_motion"));
    bool queued = servoGestures_.queueSteps(motionName, steps, count);
    response["ok"] = queued;
    response["queued"] = motionName;
    response["async"] = true;
    response["step_count"] = count;
    response["clamped"] = true;
    response["anchor_x"] = servoGestures_.anchorX();
    response["anchor_y"] = servoGestures_.anchorY();
    response["coordinate_note"] = "x_deg negative=robot left, positive=robot right; y_deg positive=up, negative=down; values are deltas from the current look anchor; final keyframe becomes the new anchor";
    return queued ? LocalStatus::Handled : LocalStatus::Error;
  }

  if (name == "end_session") {
    const char* reason = args["reason"] | "user_requested";
    // Sleep/rest semantics: explicitly return the physical head home and reset
    // the persistent look anchor only when the session is ending.
    servoGestures_.queueGesture("center_head");
    response["ok"] = true;
    response["ending"] = true;
    response["reason"] = reason;
    response["message"] = "Conversation will end after the final response.";
    return LocalStatus::Handled;
  }

  if (name == "look_with_camera") {
    const char* question = args["question"] | "What is visible in this snapshot?";
    emotion_.setEmotion("looking");
    if (!gemini_ || !gemini_->isReady()) {
      response["ok"] = false;
      response["error"] = "gemini_not_ready";
      response["message"] = "Cannot send camera image because Gemini Live is not ready.";
      return LocalStatus::Error;
    }
    String imageBase64;
    if (!camera_.captureJpegBase64(imageBase64)) {
      response["ok"] = false;
      response["error"] = camera_.lastError();
      response["message"] = "Camera capture failed.";
      response["camera_enabled"] = camera_.enabled();
      return LocalStatus::Error;
    }
    String prompt = "This is one frame from my StackChan camera. Answer concisely and practically in the user's language. User question: ";
    prompt += question;
    bool sent = gemini_->sendImageTurn(imageBase64, prompt);
    // The bright white looking LEDs are useful only while the camera is actively
    // capturing/sending. Once the still image is handed to Gemini Live, switch
    // to thinking so the white assist light cannot stick through the later
    // audio response or overload the small robot power/speaker path.
    emotion_.setEmotion(sent ? "thinking" : "error");
    response["ok"] = sent;
    response["image_sent_to_current_gemini_session"] = sent;
    response["jpeg_bytes"] = camera_.lastJpegBytes();
    response["base64_bytes"] = camera_.lastBase64Bytes();
    response["message"] = sent ? "Snapshot was sent to the current Gemini Live conversation for visual analysis." : "Failed to send snapshot into Gemini Live.";
    return sent ? LocalStatus::Handled : LocalStatus::Error;
  }

  return LocalStatus::NotHandled;
}

String GeminiToolBridge::errorJson(const String& code, const String& message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = code;
  doc["message"] = message;
  String out;
  serializeJson(doc, out);
  return out;
}
