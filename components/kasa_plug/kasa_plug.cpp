#include "kasa_plug.h"

#include <cerrno>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"

namespace esphome {
namespace kasa_plug {

static const char *const TAG = "kasa_plug";

// The autokey XOR cipher starts from this key (see the softScheck HS110
// reverse-engineering write-up referenced by the original library).
static const uint8_t KASA_KEY = 171;

// The plug commands. These are plain JSON; the framing/obfuscation is applied
// by encrypt_() before they go on the wire.
static const char *const CMD_GET_SYSINFO = "{\"system\":{\"get_sysinfo\":null}}";
static const char *const CMD_RELAY_ON = "{\"system\":{\"set_relay_state\":{\"state\":1}}}";
static const char *const CMD_RELAY_OFF = "{\"system\":{\"set_relay_state\":{\"state\":0}}}";

// Bound every network operation so an offline plug can never stall the main
// loop long enough to trip the watchdog.
static const uint32_t CONNECT_TIMEOUT_MS = 1000;
static const uint32_t READ_TIMEOUT_MS = 1000;

void KasaPlugSwitch::setup() {
  // Nothing to do here: WiFi generally isn't connected yet, so we leave the
  // initial state query to the first PollingComponent::update() tick.
  ESP_LOGCONFIG(TAG, "Setting up Kasa plug '%s' (%s:%u)...", this->get_name().c_str(), this->host_.c_str(),
                this->port_);
}

void KasaPlugSwitch::dump_config() {
  LOG_SWITCH("", "Kasa Plug Switch", this);
  ESP_LOGCONFIG(TAG, "  Host: %s", this->host_.c_str());
  ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
  LOG_UPDATE_INTERVAL(this);
}

void KasaPlugSwitch::update() {
  std::string response;
  if (!this->request_(CMD_GET_SYSINFO, &response)) {
    ESP_LOGW(TAG, "'%s': querying state failed", this->host_.c_str());
    return;
  }

  bool ok = json::parse_json(response, [this](JsonObject root) -> bool {
    JsonObject sysinfo = root["system"]["get_sysinfo"];
    if (sysinfo.isNull() || sysinfo["relay_state"].isNull()) {
      return false;
    }
    bool state = sysinfo["relay_state"].as<int>() != 0;
    ESP_LOGD(TAG, "'%s': relay_state=%d", this->host_.c_str(), state);
    this->publish_state(state);
    return true;
  });

  if (!ok) {
    ESP_LOGW(TAG, "'%s': could not parse get_sysinfo response", this->host_.c_str());
  }
}

void KasaPlugSwitch::write_state(bool state) {
  ESP_LOGD(TAG, "'%s': setting relay to %s", this->host_.c_str(), ONOFF(state));
  if (!this->request_(state ? CMD_RELAY_ON : CMD_RELAY_OFF, nullptr)) {
    ESP_LOGW(TAG, "'%s': setting relay state failed", this->host_.c_str());
    return;
  }
  // The command succeeded; report the new state immediately. The next update()
  // will reconcile if the plug ended up somewhere else.
  this->publish_state(state);
}

bool KasaPlugSwitch::request_(const std::string &payload, std::string *response) {
  std::unique_ptr<socket::Socket> sock = socket::socket_ip(SOCK_STREAM, IPPROTO_TCP);
  if (sock == nullptr) {
    ESP_LOGW(TAG, "Failed to create socket");
    return false;
  }
  sock->setblocking(false);

  struct sockaddr_storage addr {};
  socklen_t addr_len = socket::set_sockaddr((struct sockaddr *) &addr, sizeof(addr), this->host_, this->port_);
  if (addr_len == 0) {
    ESP_LOGW(TAG, "Invalid host '%s'", this->host_.c_str());
    return false;
  }

  int err = sock->connect((struct sockaddr *) &addr, addr_len);
  if (err != 0 && errno != EINPROGRESS) {
    ESP_LOGW(TAG, "Connecting to %s:%u failed: errno %d", this->host_.c_str(), this->port_, errno);
    sock->close();
    return false;
  }

  // Send the framed request. A non-blocking write before the connection has
  // completed returns EAGAIN/ENOTCONN, so we spin (bounded) until it goes out.
  const std::string frame = encrypt_(payload);
  uint32_t start = millis();
  size_t sent = 0;
  while (sent < frame.size()) {
    ssize_t written = sock->write(frame.data() + sent, frame.size() - sent);
    if (written > 0) {
      sent += written;
    } else if (written < 0 &&
               (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN || errno == EINPROGRESS)) {
      if (millis() - start > CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Timed out sending to %s:%u", this->host_.c_str(), this->port_);
        sock->close();
        return false;
      }
      delay(2);
    } else {
      ESP_LOGW(TAG, "Sending to %s:%u failed: errno %d", this->host_.c_str(), this->port_, errno);
      sock->close();
      return false;
    }
  }

  // A command that we don't need the reply for (set_relay_state) is done.
  if (response == nullptr) {
    sock->close();
    return true;
  }

  // Read the reply: a 4-byte big-endian length header followed by that many
  // encrypted payload bytes. Keep reading until we have the whole frame, the
  // peer closes, or we time out.
  std::string raw;
  uint32_t need = 0;
  bool have_header = false;
  start = millis();
  char buf[512];
  while (true) {
    ssize_t rd = sock->read(buf, sizeof(buf));
    if (rd > 0) {
      raw.append(buf, rd);
      if (!have_header && raw.size() >= 4) {
        need = (static_cast<uint8_t>(raw[0]) << 24) | (static_cast<uint8_t>(raw[1]) << 16) |
               (static_cast<uint8_t>(raw[2]) << 8) | static_cast<uint8_t>(raw[3]);
        have_header = true;
      }
      if (have_header && raw.size() >= need + 4) {
        break;
      }
    } else if (rd == 0) {
      // Peer closed the connection.
      break;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (millis() - start > READ_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Timed out reading from %s:%u", this->host_.c_str(), this->port_);
        break;
      }
      delay(2);
    } else {
      ESP_LOGW(TAG, "Reading from %s:%u failed: errno %d", this->host_.c_str(), this->port_, errno);
      sock->close();
      return false;
    }
  }
  sock->close();

  if (raw.size() <= 4) {
    ESP_LOGW(TAG, "Empty response from %s:%u", this->host_.c_str(), this->port_);
    return false;
  }

  *response = decrypt_(raw);
  return true;
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

}  // namespace kasa_plug
}  // namespace esphome
