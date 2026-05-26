#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"

#include <vector>

namespace esphome::framed_rs485 {

/// Built-in decode modes for the sensor platform.
enum SensorDecode {
  SENSOR_DECODE_UINT8,              ///< Unsigned byte at the configured offset.
  SENSOR_DECODE_UINT16_BE,          ///< Unsigned 16-bit big-endian at the configured offset.
  SENSOR_DECODE_UINT16_LE,          ///< Unsigned 16-bit little-endian at the configured offset.
  SENSOR_DECODE_UINT32_BE,          ///< Unsigned 32-bit big-endian at the configured offset.
  SENSOR_DECODE_UINT32_LE,          ///< Unsigned 32-bit little-endian at the configured offset.
  SENSOR_DECODE_BCD,                ///< Packed BCD byte at the configured offset.
  SENSOR_DECODE_FRAMES_RECEIVED,    ///< Diagnostic: running count of validated RX frames.
  SENSOR_DECODE_CRC_FAILURES,       ///< Diagnostic: running count of CRC-failed frames.
  SENSOR_DECODE_COMMANDS_SENT,      ///< Diagnostic: running count of transmitted frames.
  SENSOR_DECODE_COMMAND_DROPS,      ///< Diagnostic: commands dropped (queue full or sniffer mode).
  SENSOR_DECODE_LAST_KEEPALIVE_MS,  ///< Diagnostic: interval (ms) between the last two gate frames.
  SENSOR_DECODE_QUEUE_DEPTH,        ///< Diagnostic: current TX queue depth.
};

/// Key-frame format used when encoding button commands for TX.
enum KeyFormat {
  KEY_FORMAT_WIRELESS_9BYTE,   ///< Hayward AquaLogic wireless remote (frame type 0x0083, 9-byte payload).
  KEY_FORMAT_WIRED_REMOTE,     ///< Hayward AquaLogic wired remote.
  KEY_FORMAT_WIRED_LOCAL,      ///< Hayward AquaLogic local wired controller.
  KEY_FORMAT_JANDY_ALLBUTTON,  ///< Jandy AquaLink RS AllButton frame.
};

/// Which bytes are included in the CRC calculation.
enum CrcVariant {
  CRC_HEADER_INCLUSIVE,  ///< DLE+STX preamble bytes are included in the CRC sum (Hayward wireless).
  CRC_PAYLOAD_ONLY,      ///< CRC covers only the unescaped payload bytes (Hayward wired remotes).
};

/// CRC algorithm applied to each frame.
enum CrcType {
  CRC_TYPE_NONE,          ///< No CRC — every structurally valid frame is accepted.
  CRC_TYPE_SUM8,          ///< 8-bit arithmetic sum.
  CRC_TYPE_SUM16,         ///< 16-bit arithmetic sum.
  CRC_TYPE_XOR8,          ///< 8-bit XOR.
  CRC_TYPE_CRC16_MODBUS,  ///< CRC-16/MODBUS (poly 0xA001, init 0xFFFF, little-endian output).
};

/// TX queue overflow strategy.
enum QueuePolicy {
  QUEUE_REPLACE_LATEST,  ///< New command replaces the pending command (max_queue_size must be 1).
  QUEUE_FIFO,            ///< Commands are transmitted in arrival order.
};

/// TX gate trigger mode — controls when queued commands are transmitted.
enum TxGateMode {
  TX_GATE_FRAME_TRIGGER,  ///< Transmit after receiving a specific gate frame type.
  TX_GATE_IDLE_GAP,       ///< Transmit after the bus has been silent for min_silence.
  TX_GATE_FIXED_DELAY,    ///< Transmit on a fixed periodic interval.
};

class FramedRS485Hub;

class FramedRS485Listener {
 public:
  void set_frame_type(const std::vector<uint8_t> &frame_type) { this->frame_type_ = frame_type; }
  bool matches(const std::vector<uint8_t> &payload) const;
  virtual void handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) = 0;

 protected:
  std::vector<uint8_t> frame_type_;
};

/// Automation trigger that fires when a framed RS-485 frame matching the configured
/// frame_type is received. The full decoded payload is passed as the automation argument
/// `payload`: bytes 0-1 are the two-byte frame type, bytes 2+ are the frame data.
///
/// Registered with the hub via register_listener() — it is itself a listener.
class FramedRS485FrameTrigger : public Trigger<std::vector<uint8_t>>, public FramedRS485Listener {
 public:
  void handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) override {
    this->trigger(payload);
  }
};

class FramedRS485Hub : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_framing(uint8_t dle, uint8_t stx, uint8_t etx, uint8_t escape_byte);
  void set_accept_header_crc(bool accept) { this->accept_header_crc_ = accept; }
  void set_accept_payload_crc(bool accept) { this->accept_payload_crc_ = accept; }
  void set_crc_type(CrcType type) { this->crc_type_ = type; }
  void set_tx_crc_variant(CrcVariant variant) { this->tx_crc_variant_ = variant; }
  void set_tx_gate_mode(TxGateMode mode) { this->tx_gate_mode_ = mode; }
  void set_tx_gate_frame_type(const std::vector<uint8_t> &frame_type) { this->tx_gate_frame_type_ = frame_type; }
  void set_tx_gate_delay(uint32_t delay) { this->tx_gate_delay_ = delay; }
  void set_tx_idle_gap(uint32_t idle_gap) { this->tx_idle_gap_ = idle_gap; }
  void set_tx_fixed_interval(uint32_t interval) { this->tx_fixed_interval_ = interval; }
  void set_queue_policy(QueuePolicy policy) { this->queue_policy_ = policy; }
  void set_max_queue_size(uint32_t size) { this->max_queue_size_ = size; }
  void set_key_format(KeyFormat format) { this->key_format_ = format; }
  void set_idle_command(uint32_t cmd) {
    this->idle_command_ = cmd;
    this->has_idle_command_ = true;
  }
  void set_dump_frames(bool dump_frames) { this->dump_frames_ = dump_frames; }
  void set_sniffer_only(bool sniffer_only) { this->sniffer_only_ = sniffer_only; }
  void set_max_frame_length(uint32_t length) { this->max_frame_length_ = length; }
  void set_in_frame_timeout(uint32_t ms) { this->in_frame_timeout_ms_ = ms; }

  bool queue_command_value(uint32_t command);
  bool queue_raw_frame(const std::vector<uint8_t> &payload);
  void register_listener(FramedRS485Listener *listener) { this->listeners_.push_back(listener); }

  uint32_t get_frames_received() const { return this->frames_received_; }
  uint32_t get_crc_failures() const { return this->crc_failures_; }
  uint32_t get_commands_sent() const { return this->commands_sent_; }
  uint32_t get_command_drops() const { return this->command_drops_; }
  uint32_t get_last_keepalive_ms() const { return this->last_keepalive_ms_; }
  uint32_t get_queue_depth() const {
    return (this->tx_queue_.size() - this->tx_queue_head_) + (this->tx_start_pending_ ? 1 : 0);
  }
  const char *get_last_frame_type() const { return this->last_frame_type_; }

 protected:
  void read_uart_(uint32_t now);
  void process_raw_frame_(uint32_t now);
  bool validate_frame_();
  uint16_t calculate_crc_(const std::vector<uint8_t> &payload, bool include_header) const;
  size_t crc_length_() const;
  void escape_dle_(const std::vector<uint8_t> &data, std::vector<uint8_t> &out) const;
  void build_frame_(const std::vector<uint8_t> &payload, std::vector<uint8_t> &out);
  void build_key_payload_(uint32_t command, std::vector<uint8_t> &out) const;
  bool enqueue_frame_();
  void maybe_tx_(uint32_t now);
  void send_next_(uint32_t now);
  void send_next_idle_(uint32_t now);
  bool frame_type_equals_(const std::vector<uint8_t> &payload, const std::vector<uint8_t> &frame_type) const;
  void update_last_frame_type_();
  size_t queue_size_() const { return this->tx_queue_.size() - this->tx_queue_head_; }
  void queue_pop_front_();

  uint8_t dle_{0x10};
  uint8_t stx_{0x02};
  uint8_t etx_{0x03};
  uint8_t escape_byte_{0x00};
  bool accept_header_crc_{true};
  bool accept_payload_crc_{true};
  CrcType crc_type_{CRC_TYPE_SUM16};
  CrcVariant tx_crc_variant_{CRC_HEADER_INCLUSIVE};
  TxGateMode tx_gate_mode_{TX_GATE_FRAME_TRIGGER};
  std::vector<uint8_t> tx_gate_frame_type_{0x01, 0x01};
  uint32_t tx_gate_delay_{0};
  uint32_t tx_idle_gap_{4};
  uint32_t tx_fixed_interval_{100};
  QueuePolicy queue_policy_{QUEUE_REPLACE_LATEST};
  uint32_t max_queue_size_{1};
  KeyFormat key_format_{KEY_FORMAT_WIRELESS_9BYTE};
  uint32_t idle_command_{0};
  bool has_idle_command_{false};
  bool dump_frames_{false};
  bool sniffer_only_{false};
  uint32_t max_frame_length_{128};
  uint32_t in_frame_timeout_ms_{50};

  std::vector<FramedRS485Listener *> listeners_;
  std::vector<std::vector<uint8_t>> tx_queue_;
  size_t tx_queue_head_{0};

  bool in_frame_{false};
  uint8_t previous_byte_{0};
  std::vector<uint8_t> raw_frame_;
  uint32_t last_rx_time_{0};
  bool last_ka_seen_{false};
  uint32_t last_ka_time_{0};
  uint32_t last_keepalive_ms_{0};
  uint32_t last_tx_time_{0};
  bool tx_start_pending_{false};
  uint32_t tx_start_at_{0};
  std::vector<uint8_t> pending_tx_frame_;

  // Pre-allocated scratch buffers reused each loop to avoid heap churn.
  std::vector<uint8_t> rx_unescaped_;
  std::vector<uint8_t> rx_payload_;
  std::vector<uint8_t> tx_payload_buf_;
  std::vector<uint8_t> tx_escaped_buf_;
  std::vector<uint8_t> tx_frame_buf_;

  uint32_t frames_received_{0};
  uint32_t crc_failures_{0};
  uint32_t commands_sent_{0};
  uint32_t command_drops_{0};
  // Fixed buffer: 4 hex chars for a 2-byte frame type + null terminator.
  char last_frame_type_[5]{};
};

}  // namespace esphome::framed_rs485
