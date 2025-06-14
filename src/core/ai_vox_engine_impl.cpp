#include "ai_vox_engine_impl.h"

#ifdef ARDUINO
#include "espressif_button/button_gpio.h"
#include "espressif_button/iot_button.h"
#else
#include <button_gpio.h>
#include <esp_lvgl_port.h>
#include <iot_button.h>
#endif

#include <cJSON.h>
#include <driver/i2c_master.h>
#include <esp_crt_bundle.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mqtt_client.h>

#include "ai_vox_observer.h"
#include "audio_input_engine.h"
#include "audio_output_engine.h"
#include "fetch_config.h"

#ifndef CLOGGER_SEVERITY
#define CLOGGER_SEVERITY CLOGGER_SEVERITY_WARN
#endif

#include "clogger/clogger.h"

namespace ai_vox {

namespace {

enum WebScoketFrameType : uint8_t {
  kWebsocketTextFrame = 0x01,    // 文本帧
  kWebsocketBinaryFrame = 0x02,  // 二进制帧
  kWebsocketCloseFrame = 0x08,   // 关闭连接
  kWebsocketPingFrame = 0x09,    // Ping 帧
  kWebsocketPongFrame = 0x0A,    // Pong 帧
};

std::string GetMacAddress() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(mac_str);
}

std::string Uuid() {
  // UUID v4 需要 16 字节的随机数据
  uint8_t uuid[16];

  // 使用 ESP32 的硬件随机数生成器
  esp_fill_random(uuid, sizeof(uuid));

  // 设置版本 (版本 4) 和变体位
  uuid[6] = (uuid[6] & 0x0F) | 0x40;  // 版本 4
  uuid[8] = (uuid[8] & 0x3F) | 0x80;  // 变体 1

  // 将字节转换为标准的 UUID 字符串格式
  char uuid_str[37];
  snprintf(uuid_str,
           sizeof(uuid_str),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           uuid[0],
           uuid[1],
           uuid[2],
           uuid[3],
           uuid[4],
           uuid[5],
           uuid[6],
           uuid[7],
           uuid[8],
           uuid[9],
           uuid[10],
           uuid[11],
           uuid[12],
           uuid[13],
           uuid[14],
           uuid[15]);

  return std::string(uuid_str);
}
}  // namespace

EngineImpl &EngineImpl::GetInstance() {
  static std::once_flag s_once_flag;
  static EngineImpl *s_instance = nullptr;
  std::call_once(s_once_flag, []() { s_instance = new EngineImpl; });
  return *s_instance;
}

EngineImpl::EngineImpl()
    : uuid_(Uuid()),
      ota_url_("https://api.tenclass.net/xiaozhi/ota/"),
      websocket_url_("wss://api.tenclass.net/xiaozhi/v1/"),
      websocket_headers_{
          {"Authorization", "Bearer test-token"},
      } {
  CLOGD();
}

EngineImpl::~EngineImpl() {
  CLOGD();
  // TODO
}

void EngineImpl::SetObserver(std::shared_ptr<Observer> observer) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }

  observer_ = std::move(observer);
}

void EngineImpl::SetTrigger(const gpio_num_t gpio) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }

  trigger_pin_ = gpio;
}

void EngineImpl::SetOtaUrl(const std::string url) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }
  ota_url_ = std::move(url);
}

void EngineImpl::ConfigWebsocket(const std::string url, const std::map<std::string, std::string> headers) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }

  websocket_url_ = std::move(url);
  for (auto [key, value] : headers) {
    websocket_headers_.insert_or_assign(std::move(key), std::move(value));
  }
}

void EngineImpl::RegisterIotEntity(std::shared_ptr<iot::Entity> entity) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }
  iot_manager_.RegisterEntity(std::move(entity));
}

void EngineImpl::Start(std::shared_ptr<AudioInputDevice> audio_input_device, std::shared_ptr<AudioOutputDevice> audio_output_device) {
  CLOGD();
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }

  audio_input_device_ = std::move(audio_input_device);
  audio_output_device_ = std::move(audio_output_device);

  button_config_t btn_cfg = {
      .long_press_time = 1000,
      .short_press_time = 50,
  };

  button_gpio_config_t gpio_cfg = {
      .gpio_num = trigger_pin_,
      .active_level = 0,
      .enable_power_save = true,
      .disable_pull = false,
  };

  ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &button_handle_));
  ESP_ERROR_CHECK(iot_button_register_cb(button_handle_, BUTTON_SINGLE_CLICK, nullptr, OnButtonClick, this));

  ChangeState(State::kInited);
  LoadProtocol();

  auto ret = xTaskCreate(Loop, "AiVoxMain", 1024 * 4, this, tskIDLE_PRIORITY + 1, nullptr);
  assert(ret == pdPASS);
}

void EngineImpl::Loop(void *self) {
  reinterpret_cast<EngineImpl *>(self)->Loop();
  vTaskDelete(nullptr);
}

void EngineImpl::OnButtonClick(void *button_handle, void *self) {
  reinterpret_cast<EngineImpl *>(self)->OnButtonClick();
}

void EngineImpl::OnWebsocketEvent(void *self, esp_event_base_t base, int32_t event_id, void *event_data) {
  reinterpret_cast<EngineImpl *>(self)->OnWebsocketEvent(base, event_id, event_data);
}

void EngineImpl::Loop() {
loop_start:
  auto message = message_queue_.Recevie();
  if (!message.has_value()) {
    goto loop_start;
  }
  switch (message->type()) {
    case MessageType::kOnButtonClick: {
      CLOGD("kOnButtonClick");
      switch (state_) {
        case State::kInited: {
          LoadProtocol();
          break;
        }
        case State::kStandby: {
          ConnectWebSocket();
          break;
        }
        case State::kListening: {
          DisconnectWebSocket();
          break;
        }
        case State::kSpeaking: {
          AbortSpeaking();
          break;
        }
        default: {
          break;
        }
      }
      break;
    }

    case MessageType::kOnOutputDataComsumed: {
      CLOGD("kOnOutputDataComsumed");
      OnAudioOutputDataConsumed();
      break;
    }
    case MessageType::kOnWebsocketConnected: {
      CLOGD("kOnWebsocketConnected");
      OnWebSocketConnected();
      break;
    }
    case MessageType::kOnWebsocketDisconnected: {
      CLOGD("kOnWebsocketDisconnected");
      audio_input_engine_.reset();
      audio_output_engine_.reset();
      if (web_socket_client_ != nullptr) {
        esp_websocket_client_close(web_socket_client_, pdMS_TO_TICKS(5000));
        esp_websocket_client_destroy(web_socket_client_);
        web_socket_client_ = nullptr;
      }
      ChangeState(State::kInited);
      break;
    }
    case MessageType::kOnWebsocketEventData: {
      auto op_code = message->Read<uint8_t>();
      auto data = message->Read<std::shared_ptr<std::vector<uint8_t>>>();
      if (op_code && data) {
        OnWebSocketEventData(*op_code, std::move(*data));
      }
      break;
    }
    case MessageType::kOnWebsocketFinish: {
      CLOG("kOnWebsocketFinish");
      audio_input_engine_.reset();
      audio_output_engine_.reset();
      if (web_socket_client_ != nullptr) {
        esp_websocket_client_close(web_socket_client_, pdMS_TO_TICKS(5000));
        esp_websocket_client_destroy(web_socket_client_);
        web_socket_client_ = nullptr;
      }
      ChangeState(State::kStandby);
      break;
    }
    default: {
      break;
    }
  }
  goto loop_start;
}

void EngineImpl::OnButtonClick() {
  message_queue_.Send(MessageType::kOnButtonClick);
}

void EngineImpl::OnWebsocketEvent(esp_event_base_t base, int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  switch (event_id) {
    case WEBSOCKET_EVENT_BEGIN: {
      CLOGI("WEBSOCKET_EVENT_BEGIN");
      break;
    }
    case WEBSOCKET_EVENT_CONNECTED: {
      CLOGI("WEBSOCKET_EVENT_CONNECTED");
      message_queue_.Send(MessageType::kOnWebsocketConnected);
      break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED: {
      CLOGI("WEBSOCKET_EVENT_DISCONNECTED");
      message_queue_.Send(MessageType::kOnWebsocketDisconnected);
      break;
    }
    case WEBSOCKET_EVENT_DATA: {
      if (data->fin) {
        if (recving_websocket_data_.empty()) {
          Message message(MessageType::kOnWebsocketEventData);
          message.Write(data->op_code);
          message.Write(std::make_shared<std::vector<uint8_t>>(data->data_ptr, data->data_ptr + data->data_len));
          message_queue_.Send(std::move(message));
        } else {
          recving_websocket_data_.insert(recving_websocket_data_.end(), data->data_ptr, data->data_ptr + data->data_len);
          Message message(MessageType::kOnWebsocketEventData);
          message.Write(data->op_code);
          message.Write(std::make_shared<std::vector<uint8_t>>(std::move(recving_websocket_data_)));
          message_queue_.Send(std::move(message));
          recving_websocket_data_.clear();
        }
      } else {
        recving_websocket_data_.insert(recving_websocket_data_.end(), data->data_ptr, data->data_ptr + data->data_len);
      }
      break;
    }
    case WEBSOCKET_EVENT_ERROR: {
      CLOGI("WEBSOCKET_EVENT_ERROR");
      break;
    }
    case WEBSOCKET_EVENT_FINISH: {
      CLOGI("WEBSOCKET_EVENT_FINISH");
      message_queue_.Send(MessageType::kOnWebsocketFinish);
      break;
    }
    default: {
      break;
    }
  }
}

void EngineImpl::OnWebSocketEventData(const uint8_t op_code, std::shared_ptr<std::vector<uint8_t>> &&data) {
  switch (op_code) {
    case kWebsocketTextFrame: {
      CLOGI("%.*s", static_cast<int>(data->size()), data->data());
      OnJsonData(std::move(*data));
      break;
    }
    case kWebsocketBinaryFrame: {
      if (audio_output_engine_) {
        audio_output_engine_->Write(std::move(*data));
      }
      break;
    }
    default: {
      CLOGI("Unsupported WebSocket frame type: %u", op_code);
      break;
    }
  }
}

void EngineImpl::OnJsonData(std::vector<uint8_t> &&data) {
  const auto root_json = cJSON_ParseWithLength(reinterpret_cast<const char *>(data.data()), data.size());
  if (!cJSON_IsObject(root_json)) {
    CLOGE("Invalid JSON data");
    cJSON_Delete(root_json);
    return;
  }

  std::string type;
  auto *type_json = cJSON_GetObjectItem(root_json, "type");
  if (cJSON_IsString(type_json)) {
    type = type_json->valuestring;
  } else {
    CLOGE("Missing or invalid 'type' field in JSON data");
    cJSON_Delete(root_json);
    return;
  }
  CLOGI("Received JSON type: %s", type.c_str());

  if (type == "hello") {
    if (state_ != State::kWebsocketConnected) {
      CLOGE("Invalid state: %u", state_);
      cJSON_Delete(root_json);
      return;
    }

    auto session_id_json = cJSON_GetObjectItem(root_json, "session_id");
    if (cJSON_IsString(session_id_json)) {
      session_id_ = session_id_json->valuestring;
      CLOGI("Session ID: %s", session_id_.c_str());
    }

    SendIotDescriptions();
    SendIotUpdatedStates(true);
    StartListening();
  } else if (type == "goodbye") {
    auto session_id_json = cJSON_GetObjectItem(root_json, "session_id");
    std::string session_id;
    if (cJSON_IsString(session_id_json)) {
      if (session_id_ != session_id_json->valuestring) {
        cJSON_Delete(root_json);
        return;
      }
    }
  } else if (type == "tts") {
    auto *state_json = cJSON_GetObjectItem(root_json, "state");
    if (cJSON_IsString(state_json)) {
      if (strcmp("start", state_json->valuestring) == 0) {
        CLOG("tts start");
        if (state_ != State::kListening) {
          CLOGW("invalid state: %u", state_);
          cJSON_Delete(root_json);
          return;
        }
        audio_input_engine_.reset();
        audio_output_engine_ = std::make_shared<AudioOutputEngine>([this](AudioOutputEngine::Event event) {
          if (event == AudioOutputEngine::Event::kOnDataComsumed) {
            CLOGD("kOnDataComsumed");
            message_queue_.Send(MessageType::kOnOutputDataComsumed);
          }
        });
        audio_output_engine_->Open(audio_output_device_);
        ChangeState(State::kSpeaking);
      } else if (strcmp("stop", state_json->valuestring) == 0) {
        CLOG("tts stop");
        if (audio_output_engine_) {
          audio_output_engine_->NotifyDataEnd();
        }
      } else if (strcmp("sentence_start", state_json->valuestring) == 0) {
        auto text = cJSON_GetObjectItem(root_json, "text");
        if (text != nullptr) {
          CLOG("<< %s", text->valuestring);
          if (observer_) {
            observer_->PushEvent(Observer::ChatMessageEvent{ChatRole::kAssistant, text->valuestring});
          }
        }
      } else if (strcmp("sentence_end", state_json->valuestring) == 0) {
        // TODO:
      }
    }
  } else if (type == "stt") {
    auto text = cJSON_GetObjectItem(root_json, "text");
    if (text != nullptr) {
      CLOG(">> %s", text->valuestring);
      if (observer_) {
        observer_->PushEvent(Observer::ChatMessageEvent{ChatRole::kUser, text->valuestring});
      }
    }
  } else if (type == "llm") {
    auto emotion = cJSON_GetObjectItem(root_json, "emotion");
    if (cJSON_IsString(emotion)) {
      CLOG("emotion: %s", emotion->valuestring);
      if (observer_) {
        observer_->PushEvent(Observer::EmotionEvent{emotion->valuestring});
      }
    }
  } else if (type == "iot") {
    auto commands = cJSON_GetObjectItem(root_json, "commands");
    if (cJSON_IsArray(commands)) {
      auto count = cJSON_GetArraySize(commands);
      for (size_t i = 0; i < count; ++i) {
        auto *command = cJSON_GetArrayItem(commands, i);
        if (!cJSON_IsObject(command)) {
          continue;
        }

        auto *name_json = cJSON_GetObjectItem(command, "name");
        auto *method_json = cJSON_GetObjectItem(command, "method");
        auto *parameters_json = cJSON_GetObjectItem(command, "parameters");

        if (!cJSON_IsString(name_json) || !cJSON_IsString(method_json) || !cJSON_IsObject(parameters_json)) {
          continue;
        }

        std::string name = name_json->valuestring;
        std::string method = method_json->valuestring;
        std::map<std::string, iot::Value> parameters;
        auto *parameter = parameters_json->child;
        while (parameter) {
          auto *key = parameter->string;
          auto *value = parameter->valuestring;
          if (cJSON_IsString(parameter)) {
            parameters[key] = std::string(value);
          } else if (cJSON_IsNumber(parameter)) {
            parameters[key] = static_cast<int64_t>(parameter->valueint);
          } else if (cJSON_IsBool(parameter)) {
            parameters[key] = static_cast<bool>(parameter->valueint);
          }
          parameter = parameter->next;
        }
        auto iot_message = Observer::IotMessageEvent{name, method, parameters};
        if (observer_) {
          observer_->PushEvent(iot_message);
        }
      }
    }
  } else {
    CLOGE("Unknown JSON type: %s", type.c_str());
  }
  cJSON_Delete(root_json);
}

void EngineImpl::OnWebSocketConnected() {
  CLOGI();
  if (state_ != State::kWebsocketConnecting) {
    CLOG("invalid state: %u", state_);
    return;
  }
  ChangeState(State::kWebsocketConnected);

  auto const message = cJSON_CreateObject();
  cJSON_AddStringToObject(message, "type", "hello");
  cJSON_AddNumberToObject(message, "version", 1);
  cJSON_AddStringToObject(message, "transport", "websocket");

  auto const audio_params = cJSON_CreateObject();
  cJSON_AddStringToObject(audio_params, "format", "opus");
  cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
  cJSON_AddNumberToObject(audio_params, "channels", 1);
  cJSON_AddNumberToObject(audio_params, "frame_duration", 20);
  cJSON_AddItemToObject(message, "audio_params", audio_params);

  const auto text = cJSON_PrintUnformatted(message);
  const auto length = strlen(text);
  CLOGI("sending text: %.*s", static_cast<int>(length), text);
  esp_websocket_client_send_text(web_socket_client_, text, length, pdMS_TO_TICKS(5000));
  cJSON_free(text);
  cJSON_Delete(message);
}

void EngineImpl::OnAudioOutputDataConsumed() {
  CLOGI();
  if (state_ != State::kSpeaking) {
    CLOG("invalid state: %u", state_);
    return;
  }
  SendIotUpdatedStates(false);
  StartListening();
}

void EngineImpl::LoadProtocol() {
  CLOGI();
  if (state_ != State::kInited) {
    CLOG("invalid state: %u", state_);
    return;
  }

  ChangeState(State::kLoadingProtocol);

  auto config = GetConfigFromServer(ota_url_, uuid_);

  if (!config.has_value()) {
    CLOGE("GetConfigFromServer failed");
    ChangeState(State::kInited);
    return;
  }

  CLOG("mqtt endpoint: %s", config->mqtt.endpoint.c_str());
  CLOG("mqtt client_id: %s", config->mqtt.client_id.c_str());
  CLOG("mqtt username: %s", config->mqtt.username.c_str());
  CLOG("mqtt password: %s", config->mqtt.password.c_str());
  CLOG("mqtt publish_topic: %s", config->mqtt.publish_topic.c_str());
  CLOG("mqtt subscribe_topic: %s", config->mqtt.subscribe_topic.c_str());

  CLOG("activation code: %s", config->activation.code.c_str());
  CLOG("activation message: %s", config->activation.message.c_str());

  if (!config->activation.code.empty()) {
    if (observer_) {
      observer_->PushEvent(Observer::ActivationEvent{config->activation.code, config->activation.message});
    }
    ChangeState(State::kInited);
    return;
  }

  ChangeState(State::kStandby);
  return;
}

void EngineImpl::StartListening() {
  if (state_ != State::kWebsocketConnected && state_ != State::kSpeaking) {
    CLOG("invalid state: %u", state_);
    return;
  }

  auto root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
  cJSON_AddStringToObject(root, "type", "listen");
  cJSON_AddStringToObject(root, "state", "start");
  cJSON_AddStringToObject(root, "mode", "auto");
  const auto text = cJSON_PrintUnformatted(root);
  const auto length = strlen(text);
  CLOGI("sending text: %.*s", static_cast<int>(length), text);
  esp_websocket_client_send_text(web_socket_client_, text, length, pdMS_TO_TICKS(5000));
  cJSON_free(text);
  cJSON_Delete(root);

  audio_output_engine_.reset();
  audio_input_engine_ = std::make_shared<AudioInputEngine>(audio_input_device_, [this](std::vector<uint8_t> &&data) {
    esp_websocket_client_send_bin(web_socket_client_, reinterpret_cast<const char *>(data.data()), data.size(), portMAX_DELAY);
  });
  ChangeState(State::kListening);
}

void EngineImpl::AbortSpeaking() {
  if (state_ != State::kSpeaking) {
    CLOGE("invalid state: %d", state_);
    return;
  }

  auto const message = cJSON_CreateObject();
  cJSON_AddStringToObject(message, "session_id", session_id_.c_str());
  cJSON_AddStringToObject(message, "type", "abort");
  const auto text = cJSON_PrintUnformatted(message);
  const auto length = strlen(text);
  CLOGI("sending text: %.*s", static_cast<int>(length), text);
  esp_websocket_client_send_text(web_socket_client_, text, length, pdMS_TO_TICKS(5000));
  cJSON_free(text);
  cJSON_Delete(message);
  CLOG("OK");
}

bool EngineImpl::ConnectWebSocket() {
  if (state_ != State::kStandby) {
    CLOGE("invalid state: %u", state_);
    return false;
  }

  if (web_socket_client_ != nullptr) {
    esp_websocket_client_stop(web_socket_client_);
    esp_websocket_client_destroy(web_socket_client_);
  }

  esp_websocket_client_config_t websocket_cfg;
  memset(&websocket_cfg, 0, sizeof(websocket_cfg));
  websocket_cfg.uri = websocket_url_.c_str();
  websocket_cfg.task_prio = tskIDLE_PRIORITY;
  websocket_cfg.crt_bundle_attach = esp_crt_bundle_attach;

  CLOGI("url: %s", websocket_cfg.uri);
  web_socket_client_ = esp_websocket_client_init(&websocket_cfg);
  CLOGI("web_socket_client_:%p", web_socket_client_);
  if (web_socket_client_ == nullptr) {
    CLOGE("esp_websocket_client_init failed with %s", websocket_cfg.uri);
    return false;
  }

  for (const auto &[key, value] : websocket_headers_) {
    esp_websocket_client_append_header(web_socket_client_, key.c_str(), value.c_str());
  }
  esp_websocket_client_append_header(web_socket_client_, "Protocol-Version", "1");
  esp_websocket_client_append_header(web_socket_client_, "Device-Id", GetMacAddress().c_str());
  esp_websocket_client_append_header(web_socket_client_, "Client-Id", uuid_.c_str());
  esp_websocket_register_events(web_socket_client_, WEBSOCKET_EVENT_ANY, &EngineImpl::OnWebsocketEvent, this);
  CLOGI("esp_websocket_client_start");
  esp_websocket_client_start(web_socket_client_);
  ChangeState(State::kWebsocketConnecting);
  CLOGI("websocket client start");
  return true;
}

void EngineImpl::DisconnectWebSocket() {
  if (web_socket_client_ == nullptr) {
    abort();
  }

  audio_input_engine_.reset();
  audio_output_engine_.reset();

  esp_websocket_client_close(web_socket_client_, pdMS_TO_TICKS(5000));
}

void EngineImpl::SendIotDescriptions() {
  const auto descirptions = iot_manager_.DescriptionsJson();
  for (const auto &descirption : descirptions) {
    CLOGI("sending text: %.*s", static_cast<int>(descirption.size()), descirption.c_str());
    const auto ret = esp_websocket_client_send_text(web_socket_client_, descirption.c_str(), descirption.size(), pdMS_TO_TICKS(5000));
    if (ret != descirption.size()) {
      CLOGE("sending failed");
    } else {
      CLOGD("sending ok");
    }
  }
}

void EngineImpl::SendIotUpdatedStates(const bool force) {
  CLOGD("force: %d", force);
  const auto updated_states = iot_manager_.UpdatedJson(force);
  for (const auto &updated_state : updated_states) {
    CLOGI("sending text: %.*s", static_cast<int>(updated_state.size()), updated_state.c_str());
    const auto ret = esp_websocket_client_send_text(web_socket_client_, updated_state.c_str(), updated_state.size(), pdMS_TO_TICKS(5000));
    if (ret != updated_state.size()) {
      CLOGE("sending failed");
    } else {
      CLOGD("sending ok");
    }
  }
}

void EngineImpl::ChangeState(const State new_state) {
  auto convert_state = [](const State state) {
    switch (state) {
      case State::kIdle:
        return ChatState::kIdle;
      case State::kInited:
        return ChatState::kIniting;
      case State::kLoadingProtocol:
        return ChatState::kIniting;
      case State::kWebsocketConnecting:
        return ChatState::kConnecting;
      case State::kWebsocketConnected:
        return ChatState::kConnecting;
      case State::kStandby:
        return ChatState::kStandby;
      case State::kListening:
        return ChatState::kListening;
      case State::kSpeaking:
        return ChatState::kSpeaking;
      default:
        return ChatState::kIdle;
    }
  };

  if (observer_) {
    observer_->PushEvent(Observer::StateChangedEvent{convert_state(state_), convert_state(new_state)});
  }
  state_ = new_state;
}

}  // namespace ai_vox