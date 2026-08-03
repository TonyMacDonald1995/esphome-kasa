#pragma once

#include <string>

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"

namespace esphome {
namespace kasa_plug {

/// TP-Link's "smarthome" protocol always speaks on this TCP port.
static const uint16_t KASA_DEFAULT_PORT = 9999;

/// A single TP-Link Kasa smart plug, exposed to ESPHome as a switch.
///
/// This is a port of the KasaSmartPlug Arduino library
/// (https://github.com/kj831ca/KasaSmartPlug) to the ESPHome component API. It
/// polls the plug for its relay state on the configured update interval and
/// sends set_relay_state commands when the switch is toggled from ESPHome/Home
/// Assistant.
class KasaPlugSwitch : public switch_::Switch, public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_host(const std::string &host) { this->host_ = host; }
  void set_port(uint16_t port) { this->port_ = port; }

 protected:
  // switch_::Switch
  void write_state(bool state) override;

  /// Send an already-formed JSON command to the plug and, optionally, collect
  /// the decrypted JSON reply. Returns true if the round-trip succeeded.
  bool request_(const std::string &payload, std::string *response);

  /// Frame a payload the way Kasa expects: a 4-byte big-endian length header
  /// followed by the XOR-autokey "encrypted" bytes (initial key 0xAB).
  static std::string encrypt_(const std::string &payload);
  /// Reverse encrypt_. Skips the 4-byte length header before decoding.
  static std::string decrypt_(const std::string &data);

  std::string host_;
  uint16_t port_{KASA_DEFAULT_PORT};
};

}  // namespace kasa_plug
}  // namespace esphome
