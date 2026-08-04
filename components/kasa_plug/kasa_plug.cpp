#include "kasa_plug.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"

namespace esphome::kasa_plug {

static const char *const TAG = "kasa_plug";

// The autokey XOR cipher starts from this key (see the softScheck HS110
// reverse-engineering write-up referenced by the original library).
static const uint8_t KASA_KEY = 171;

// The plug commands. These are plain JSON; the framing/obfuscation is applied
// by encrypt_() before they go on the wire.
static const char *const CMD_GET_SYSINFO = "{\"system\":{\"get_sysinfo\":null}}";
static const char *const CMD_RELAY_ON = "{\"system\":{\"set_relay_state\":{\"state\":1}}}";
static const char *const CMD_RELAY_OFF = "{\"system\":{\"set_relay_state\":{\"state\":0}}}";

// One deadline covers the whole connect/send/receive round trip; an offline
// plug produces a single warning when it expires, never a blocked main loop.
static const uint32_t TRANSACTION_TIMEOUT_MS = 2000;
// Upper bound on a response frame (get_sysinfo is ~1 KB); anything larger
// means we lost sync with the framing.
static const size_t MAX_RESPONSE_SIZE = 8192;

void KasaPlugSwitch::setup() {
  this->client_ = std::make_unique<AsyncClient>();

  // The callbacks may run outside the main loop (see lock_), so they only
  // move bytes and set flags; all decisions (and all writes) happen in loop().
  this->client_->onData(
      [](void *arg, AsyncClient *client, void *data, size_t len) {
        auto *self = static_cast<KasaPlugSwitch *>(arg);
        LockGuard guard(self->lock_);
        if (self->rx_buf_.size() + len <= MAX_RESPONSE_SIZE + 4) {
          self->rx_buf_.append(static_cast<const char *>(data), len);
        } else {
          self->rx_error_ = true;
        }
      },
      this);
  this->client_->onError(
      [](void *arg, AsyncClient *client, int8_t error) {
        auto *self = static_cast<KasaPlugSwitch *>(arg);
        LockGuard guard(self->lock_);
        self->rx_error_ = true;
      },
      this);
  this->client_->onDisconnect(
      [](void *arg, AsyncClient *client) {
        auto *self = static_cast<KasaPlugSwitch *>(arg);
        LockGuard guard(self->lock_);
        self->rx_disconnected_ = true;
      },
      this);

  // Nothing to run until the first transaction starts.
  this->disable_loop();
}

void KasaPlugSwitch::dump_config() {
  LOG_SWITCH("", "Kasa Plug Switch", this);
  ESP_LOGCONFIG(TAG, "  Host: %s\n"
                     "  Port: %u",
                this->host_.c_str(), this->port_);
  LOG_UPDATE_INTERVAL(this);
}

void KasaPlugSwitch::update() {
  if (this->transaction_ != Transaction::NONE) {
    ESP_LOGV(TAG, "'%s': transaction still in flight, skipping poll", this->host_.c_str());
    return;
  }
  if (!network::is_connected()) {
    ESP_LOGV(TAG, "'%s': network not up, skipping poll", this->host_.c_str());
    return;
  }
  // A toggle that is still waiting takes priority over polling.
  this->start_transaction_(this->pending_write_.has_value() ? Transaction::SET_STATE : Transaction::GET_STATE);
}

void KasaPlugSwitch::write_state(bool state) {
  this->pending_write_ = state;
  if (this->transaction_ == Transaction::NONE) {
    this->start_transaction_(Transaction::SET_STATE);
  }
  // Otherwise finish_transaction_() picks the pending write up.
}

void KasaPlugSwitch::loop() {
#if !defined(USE_ESP32) && !defined(USE_ESP8266) && !defined(USE_RP2040) && !defined(USE_LIBRETINY)
  // The socket-based AsyncClient (host and other non-AsyncTCP platforms) is
  // driven from the main loop; the AsyncTCP libraries drive themselves.
  this->client_->loop();
#endif

  if (this->transaction_ == Transaction::NONE) {
    this->disable_loop();
    return;
  }

  bool error = false;
  bool disconnected = false;
  bool complete = false;
  std::string frame;
  {
    LockGuard guard(this->lock_);
    error = this->rx_error_;
    disconnected = this->rx_disconnected_;
    if (this->rx_buf_.size() >= 4) {
      uint32_t need = (static_cast<uint8_t>(this->rx_buf_[0]) << 24) |
                      (static_cast<uint8_t>(this->rx_buf_[1]) << 16) |
                      (static_cast<uint8_t>(this->rx_buf_[2]) << 8) | static_cast<uint8_t>(this->rx_buf_[3]);
      if (need > MAX_RESPONSE_SIZE) {
        error = true;
      } else if (this->rx_buf_.size() >= need + 4) {
        complete = true;
        frame = std::move(this->rx_buf_);
        this->rx_buf_.clear();
        // Anything past the declared frame is not part of this response.
        frame.resize(need + 4);
      }
    }
  }

  if (complete) {
    this->process_response_(frame);
    return;
  }
  if (error || disconnected) {
    ESP_LOGW(TAG, "'%s': connection %s before full response", this->host_.c_str(), error ? "error" : "closed");
    this->finish_transaction_(false);
    return;
  }
  if (millis() - this->transaction_start_ > TRANSACTION_TIMEOUT_MS) {
    ESP_LOGW(TAG, "'%s': timed out after %ums", this->host_.c_str(), (unsigned) TRANSACTION_TIMEOUT_MS);
    this->finish_transaction_(false);
    return;
  }

  // Still in progress: push the request out once the connection is up. Sending
  // from here (not from an onConnect callback) keeps tx_frame_ owned by the
  // main loop, so no cross-thread access to it can race a reassignment.
  if (this->tx_offset_ < this->tx_frame_.size() && this->client_->connected()) {
    this->tx_offset_ +=
        this->client_->write(this->tx_frame_.data() + this->tx_offset_, this->tx_frame_.size() - this->tx_offset_);
  }
}

void KasaPlugSwitch::start_transaction_(Transaction type) {
  {
    LockGuard guard(this->lock_);
    this->rx_buf_.clear();
    this->rx_error_ = false;
    this->rx_disconnected_ = false;
  }

  const char *payload;
  if (type == Transaction::SET_STATE) {
    this->target_state_ = *this->pending_write_;
    this->pending_write_.reset();
    payload = this->target_state_ ? CMD_RELAY_ON : CMD_RELAY_OFF;
    ESP_LOGD(TAG, "'%s': setting relay to %s", this->host_.c_str(), ONOFF(this->target_state_));
  } else {
    payload = CMD_GET_SYSINFO;
  }
  this->tx_frame_ = encrypt_(payload);
  this->tx_offset_ = 0;
  this->transaction_ = type;
  this->transaction_start_ = millis();
  this->enable_loop();

  if (!this->client_->connect(this->host_.c_str(), this->port_)) {
    ESP_LOGW(TAG, "'%s': starting connection failed", this->host_.c_str());
    this->finish_transaction_(false);
  }
}

void KasaPlugSwitch::finish_transaction_(bool success) {
  this->client_->close();
  this->transaction_ = Transaction::NONE;
  if (success) {
    this->status_clear_warning();
  } else {
    this->status_set_warning();
  }

  if (this->pending_write_.has_value()) {
    // A toggle arrived while the last transaction was in flight; send it now.
    this->start_transaction_(Transaction::SET_STATE);
    return;
  }
  this->disable_loop();
}

void KasaPlugSwitch::process_response_(const std::string &raw) {
  const std::string response = decrypt_(raw);
  bool ok;

  if (this->transaction_ == Transaction::SET_STATE) {
    ok = json::parse_json(response, [](JsonObject root) -> bool {
      JsonVariant err_code = root["system"]["set_relay_state"]["err_code"];
      return !err_code.isNull() && err_code.as<int>() == 0;
    });
    if (ok) {
      this->publish_state(this->target_state_);
    } else {
      ESP_LOGW(TAG, "'%s': plug rejected set_relay_state", this->host_.c_str());
    }
  } else {
    ok = json::parse_json(response, [this](JsonObject root) -> bool {
      JsonVariant relay_state = root["system"]["get_sysinfo"]["relay_state"];
      if (relay_state.isNull()) {
        return false;
      }
      bool state = relay_state.as<int>() != 0;
      ESP_LOGD(TAG, "'%s': relay_state=%d", this->host_.c_str(), state);
      this->publish_state(state);
      return true;
    });
    if (!ok) {
      ESP_LOGW(TAG, "'%s': could not parse get_sysinfo response", this->host_.c_str());
    }
  }

  this->finish_transaction_(ok);
}

std::string KasaPlugSwitch::encrypt_(const std::string &payload) {
  std::string out;
  out.reserve(payload.size() + 4);

  uint32_t len = payload.size();
  out.push_back(static_cast<char>((len >> 24) & 0xFF));
  out.push_back(static_cast<char>((len >> 16) & 0xFF));
  out.push_back(static_cast<char>((len >> 8) & 0xFF));
  out.push_back(static_cast<char>(len & 0xFF));

  uint8_t key = KASA_KEY;
  for (char c : payload) {
    key = static_cast<uint8_t>(c) ^ key;
    out.push_back(static_cast<char>(key));
  }
  return out;
}

std::string KasaPlugSwitch::decrypt_(const std::string &data) {
  std::string out;
  if (data.size() <= 4) {
    return out;
  }
  out.reserve(data.size() - 4);

  uint8_t key = KASA_KEY;
  for (size_t i = 4; i < data.size(); i++) {
    uint8_t cipher = static_cast<uint8_t>(data[i]);
    out.push_back(static_cast<char>(cipher ^ key));
    key = cipher;
  }
  return out;
}

}  // namespace esphome::kasa_plug
