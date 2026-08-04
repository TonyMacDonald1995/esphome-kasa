#pragma once

#include <memory>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/optional.h"
#include "esphome/components/async_tcp/async_tcp.h"
#include "esphome/components/switch/switch.h"

namespace esphome::kasa_plug {

/// TP-Link's "smarthome" protocol always speaks on this TCP port.
static const uint16_t KASA_DEFAULT_PORT = 9999;

/// A single TP-Link Kasa smart plug, exposed to ESPHome as a switch.
///
/// This is a port of the KasaSmartPlug Arduino library
/// (https://github.com/kj831ca/KasaSmartPlug) to the ESPHome component API. It
/// polls the plug for its relay state on the configured update interval and
/// sends set_relay_state commands when the switch is toggled from ESPHome/Home
/// Assistant.
///
/// All I/O runs through async_tcp's AsyncClient, so a single transaction
/// (connect, send one command, receive one reply) is fully event-driven: the
/// callbacks buffer what arrives and loop() drives the state machine. An
/// unreachable plug therefore costs nothing but a warning after the
/// transaction deadline.
class KasaPlugSwitch : public switch_::Switch, public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_host(const std::string &host) { this->host_ = host; }
  void set_port(uint16_t port) { this->port_ = port; }

 protected:
  /// What the single in-flight request is for.
  enum class Transaction : uint8_t { NONE, GET_STATE, SET_STATE };

  // switch_::Switch
  void write_state(bool state) override;

  /// Create a fresh AsyncClient and attach the receive callbacks.
  void create_client_();
  /// Begin a connect/send/receive round trip. Only valid when idle.
  void start_transaction_(Transaction type);
  /// Close the connection, update the warning status, and start the next
  /// transaction if a toggle arrived while this one was in flight.
  void finish_transaction_(bool success);
  /// Decrypt and act on a complete response frame (called from loop()).
  void process_response_(const std::string &raw);

  /// Frame a payload the way Kasa expects: a 4-byte big-endian length header
  /// followed by the XOR-autokey "encrypted" bytes (initial key 0xAB).
  static std::string encrypt_(const std::string &payload);
  /// Reverse encrypt_. Skips the 4-byte length header before decoding.
  static std::string decrypt_(const std::string &data);

  /// One client per transaction: created by start_transaction_(), destroyed by
  /// finish_transaction_(). Reusing one AsyncClient across connections is not
  /// safe on every platform: RPAsyncTCP's close() only *defers* the close, so
  /// a reused client still holds the previous pcb and refuses the next
  /// connect() (and closing again can touch a pcb lwIP already freed). The
  /// destructor is the one cleanup path all the AsyncTCP libraries implement
  /// as an immediate, callback-clearing close.
  std::unique_ptr<AsyncClient> client_;

  /// Guards rx_buf_ and the flags below: the AsyncTCP libraries invoke their
  /// callbacks outside the main loop on some platforms (a dedicated task on
  /// ESP32/LibreTiny). No-op where the platform is single-threaded.
  Mutex lock_;
  std::string rx_buf_;
  bool rx_error_{false};
  bool rx_disconnected_{false};

  /// The framed request for the current transaction. Only ever touched from
  /// the main loop: loop() sends it once the connection is established.
  std::string tx_frame_;
  /// How much of tx_frame_ has been handed to the client so far.
  size_t tx_offset_{0};
  std::string host_;
  uint32_t transaction_start_{0};
  /// Last time loop() emitted the in-flight progress heartbeat.
  uint32_t last_progress_log_{0};
  Transaction transaction_{Transaction::NONE};
  /// Latest state requested via write_state(); consumed when its transaction
  /// starts, so a toggle during an in-flight transaction is sent right after.
  optional<bool> pending_write_;
  bool target_state_{false};
  uint16_t port_{KASA_DEFAULT_PORT};
};

}  // namespace esphome::kasa_plug
