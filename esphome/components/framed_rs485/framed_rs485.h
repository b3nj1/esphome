#pragma once

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#include "esphome/components/uart/uart.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace framed_rs485 {

enum SensorDecode {
  SENSOR_DECODE_LED_MASK,
  SENSOR_DECODE_LED_MASK_BLINKING,
  SENSOR_DECODE_DISPLAY_TEMPERATURE,
  SENSOR_DECODE_UINT8,
  SENSOR_DECODE_UINT16_BE,
  SENSOR_DECODE_UINT16_LE,
  SENSOR_DECODE_UINT32_BE,
  SENSOR_DECODE_UINT32_LE,
  SENSOR_DECODE_BCD,
  SENSOR_DECODE_VSP_SPEED_REQUEST,
  SENSOR_DECODE_VSP_POWER_BCD,
  SENSOR_DECODE_FRAMES_RECEIVED,
  SENSOR_DECODE_CRC_FAILURES,
  SENSOR_DECODE_COMMANDS_SENT,
  SENSOR_DECODE_COMMAND_DROPS,
  SENSOR_DECODE_LAST_KEEPALIVE_MS,
  SENSOR_DECODE_QUEUE_DEPTH,
};

enum BinaryDecode {
  BINARY_DECODE_LED_BIT,
  // True when the decoded display text (high-bit-stripped, trimmed) contains
  // the configured match string (case-insensitive substring).
  BINARY_DECODE_DISPLAY_TEXT_MATCH,
};

enum TextDecode {
  TEXT_DECODE_DISPLAY_TEXT,
  TEXT_DECODE_LAST_FRAME_TYPE,
  // Returns only the characters that have their high bit set in the raw frame
  // (blinking characters on an AquaLogic display). Empty string when nothing blinks.
  TEXT_DECODE_DISPLAY_BLINK_TEXT,
};

enum KeyFormat {
  KEY_FORMAT_WIRELESS_9BYTE,
  KEY_FORMAT_WIRED_REMOTE,
  KEY_FORMAT_WIRED_LOCAL,
  KEY_FORMAT_JANDY_ALLBUTTON,
};

enum CrcVariant {
  CRC_HEADER_INCLUSIVE,
  CRC_PAYLOAD_ONLY,
};

enum CrcType {
  CRC_TYPE_NONE,
  CRC_TYPE_SUM8,
  CRC_TYPE_SUM16,
  CRC_TYPE_XOR8,
  CRC_TYPE_CRC16_MODBUS,
};

enum QueuePolicy {
  QUEUE_REPLACE_LATEST,
  QUEUE_FIFO,
};

enum TxGateMode {
  TX_GATE_FRAME_TRIGGER,
  TX_GATE_IDLE_GAP,
  TX_GATE_FIXED_DELAY,
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
  void set_tx_guard_time(uint32_t guard_time) { this->tx_guard_time_ = guard_time; }
  void set_key_format(KeyFormat format) { this->key_format_ = format; }
  void set_idle_command(uint32_t cmd) {
    this->idle_command_ = cmd;
    this->has_idle_command_ = true;
  }
  void set_dump_frames(bool dump_frames) { this->dump_frames_ = dump_frames; }
  void set_sniffer_only(bool sniffer_only) { this->sniffer_only_ = sniffer_only; }
  void set_max_frame_length(uint32_t length) { this->max_frame_length_ = length; }
  void set_flow_control_pin(GPIOPin *pin) { this->flow_control_pin_ = pin; }

  bool queue_command_value(uint32_t command);
  bool queue_raw_frame(const std::vector<uint8_t> &payload);
  void register_listener(FramedRS485Listener *listener) { this->listeners_.push_back(listener); }

  uint32_t get_frames_received() const { return this->frames_received_; }
  uint32_t get_crc_failures() const { return this->crc_failures_; }
  uint32_t get_commands_sent() const { return this->commands_sent_; }
  uint32_t get_command_drops() const { return this->command_drops_; }
  uint32_t get_last_keepalive_ms() const { return this->last_keepalive_ms_; }
  uint32_t get_queue_depth() const { return this->tx_queue_.size() + (this->tx_start_pending_ ? 1 : 0); }
  const std::string &get_last_frame_type() const { return this->last_frame_type_; }

  static uint32_t decode_led_mask(const std::vector<uint8_t> &payload);
  static uint32_t decode_led_mask_blinking(const std::vector<uint8_t> &payload);
  // Strips the high bit from each byte, substitutes '_' with degree sign, trims whitespace.
  static void decode_display_text(const std::vector<uint8_t> &payload, std::string &out);
  // Returns only the characters whose high bit was set in the raw frame (blinking chars).
  // Produces an empty string when nothing is blinking.
  static void decode_display_blink_text(const std::vector<uint8_t> &payload, std::string &out);
  // Collapses runs of whitespace to a single space and trims ends. Used before
  // display_text_match comparisons to absorb display centering padding.
  static std::string normalize_display_ws(const std::string &s);

 protected:
  void read_uart_(uint32_t now);
  void process_raw_frame_(uint32_t now);
  bool validate_frame_();
  uint16_t calculate_crc_(const std::vector<uint8_t> &payload, bool include_header) const;
  size_t crc_length_() const;
  void escape_dle_(const std::vector<uint8_t> &data, std::vector<uint8_t> &out) const;
  void build_frame_(const std::vector<uint8_t> &payload, std::vector<uint8_t> &out);
  void build_key_payload_(uint32_t command, std::vector<uint8_t> &out) const;
  void maybe_tx_(uint32_t now);
  void send_next_(uint32_t now);
  void send_next_idle_(uint32_t now);
  void set_tx_mode_(bool tx);
  void release_tx_();
  bool frame_type_equals_(const std::vector<uint8_t> &payload, const std::vector<uint8_t> &frame_type) const;
  void update_last_frame_type_();

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
  uint32_t tx_guard_time_{5};
  KeyFormat key_format_{KEY_FORMAT_WIRELESS_9BYTE};
  uint32_t idle_command_{0};
  bool has_idle_command_{false};
  bool dump_frames_{false};
  bool sniffer_only_{false};
  uint32_t max_frame_length_{128};

  GPIOPin *flow_control_pin_{nullptr};
  std::vector<FramedRS485Listener *> listeners_;
  std::vector<std::vector<uint8_t>> tx_queue_;

  bool in_frame_{false};
  uint8_t previous_byte_{0};
  std::vector<uint8_t> raw_frame_;
  uint32_t last_rx_time_{0};
  uint32_t last_ka_time_{0};
  uint32_t last_keepalive_ms_{0};
  uint32_t last_tx_time_{0};
  bool tx_release_pending_{false};
  uint32_t tx_release_at_{0};
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
  std::string last_frame_type_;
};

#ifdef USE_BUTTON
class FramedRS485Button : public button::Button {
 public:
  void set_parent(FramedRS485Hub *parent) { this->parent_ = parent; }
  void set_command_value(uint32_t command) { this->command_value_ = command; }

 protected:
  void press_action() override;
  FramedRS485Hub *parent_{nullptr};
  uint32_t command_value_{0};
};
#endif  // USE_BUTTON

#ifdef USE_BINARY_SENSOR
class FramedRS485BinarySensor : public binary_sensor::BinarySensor, public FramedRS485Listener {
 public:
  using decode_lambda_t = std::function<optional<bool>(const std::vector<uint8_t> &)>;
  void set_decode(BinaryDecode decode) { this->decode_ = decode; }
  void set_bit(uint8_t bit) { this->bit_ = bit; }
  // display_text_match: all strings in match_on must appear (AND) to latch true;
  // all strings in match_off must appear (AND) to latch false.
  void add_match_on(const std::string &s) { this->match_on_.push_back(s); }
  void add_match_off(const std::string &s) { this->match_off_.push_back(s); }
  // Revert to false if no proof is seen within this many milliseconds (0 = never).
  void set_timeout_ms(uint32_t ms) { this->timeout_ms_ = ms; }
  void set_template(decode_lambda_t lambda) { this->lambda_ = lambda; }
  void handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) override;

 protected:
  BinaryDecode decode_{BINARY_DECODE_LED_BIT};
  decode_lambda_t lambda_{nullptr};
  uint8_t bit_{0};
  std::vector<std::string> match_on_;
  std::vector<std::string> match_off_;
  uint32_t timeout_ms_{0};
  uint32_t last_proof_ms_{0};  // 0 = no proof seen yet
  bool timed_out_{false};
};
#endif  // USE_BINARY_SENSOR

#ifdef USE_SENSOR
class FramedRS485Sensor : public sensor::Sensor, public FramedRS485Listener {
 public:
  using decode_lambda_t = std::function<optional<float>(const std::vector<uint8_t> &)>;
  void set_decode(SensorDecode decode) { this->decode_ = decode; }
  void set_offset(uint32_t offset) { this->offset_ = offset; }
  void set_temperature_label(const std::string &label) { this->temperature_label_ = label; }
  void set_template(decode_lambda_t lambda) { this->lambda_ = lambda; }
  void handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) override;

 protected:
  optional<float> decode_builtin_(FramedRS485Hub *hub, const std::vector<uint8_t> &payload) const;
  SensorDecode decode_{SENSOR_DECODE_UINT8};
  decode_lambda_t lambda_{nullptr};
  uint32_t offset_{0};
  std::string temperature_label_;
};
#endif  // USE_SENSOR

#ifdef USE_TEXT_SENSOR
class FramedRS485TextSensor : public text_sensor::TextSensor, public FramedRS485Listener {
 public:
  using decode_lambda_t = std::function<optional<std::string>(const std::vector<uint8_t> &)>;
  void set_decode(TextDecode decode) { this->decode_ = decode; }
  void set_template(decode_lambda_t lambda) { this->lambda_ = lambda; }
  void handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) override;

 protected:
  TextDecode decode_{TEXT_DECODE_DISPLAY_TEXT};
  decode_lambda_t lambda_{nullptr};
};
#endif  // USE_TEXT_SENSOR

#ifdef USE_NUMBER
class FramedRS485Number : public number::Number {
 public:
  using encode_lambda_t = std::function<optional<std::vector<uint8_t>>(float)>;
  void set_parent(FramedRS485Hub *parent) { this->parent_ = parent; }
  void set_template(encode_lambda_t lambda) { this->lambda_ = lambda; }

 protected:
  void control(float value) override;
  FramedRS485Hub *parent_{nullptr};
  encode_lambda_t lambda_{nullptr};
};
#endif  // USE_NUMBER

}  // namespace framed_rs485
}  // namespace esphome
