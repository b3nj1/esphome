#include "framed_rs485.h"

#include "esphome/core/application.h"

#ifdef USE_BINARY_SENSOR
#include "binary_sensor/framed_rs485_binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "button/framed_rs485_button.h"
#endif
#ifdef USE_NUMBER
#include "number/framed_rs485_number.h"
#endif
#ifdef USE_SENSOR
#include "sensor/framed_rs485_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "text_sensor/framed_rs485_text_sensor.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace esphome::framed_rs485 {

static const char *const TAG = "framed_rs485";

static const char *crc_type_str(CrcType t) {
  switch (t) {
    case CRC_TYPE_NONE:
      return "none";
    case CRC_TYPE_SUM8:
      return "sum8";
    case CRC_TYPE_SUM16:
      return "sum16";
    case CRC_TYPE_XOR8:
      return "xor8";
    case CRC_TYPE_CRC16_MODBUS:
      return "crc16_modbus";
    default:
      return "unknown";
  }
}

static const char *tx_gate_mode_str(TxGateMode m) {
  switch (m) {
    case TX_GATE_FRAME_TRIGGER:
      return "frame_trigger";
    case TX_GATE_IDLE_GAP:
      return "idle_gap";
    case TX_GATE_FIXED_DELAY:
      return "fixed_delay";
    default:
      return "unknown";
  }
}

static const char *queue_policy_str(QueuePolicy p) {
  switch (p) {
    case QUEUE_REPLACE_LATEST:
      return "replace_latest";
    case QUEUE_FIFO:
      return "fifo";
    default:
      return "unknown";
  }
}

bool FramedRS485Listener::matches(const std::vector<uint8_t> &payload) const {
  if (this->frame_type_.empty())
    return true;
  if (payload.size() < this->frame_type_.size())
    return false;
  return std::equal(this->frame_type_.begin(), this->frame_type_.end(), payload.begin());
}

void FramedRS485Hub::setup() {
  if (this->flow_control_pin_ != nullptr) {
    this->flow_control_pin_->setup();
    this->flow_control_pin_->digital_write(false);  // start in RX mode
  }
  if (this->tx_gate_mode_ == TX_GATE_FRAME_TRIGGER && this->tx_gate_frame_type_.empty() && !this->sniffer_only_) {
    ESP_LOGW(TAG, "TX gate mode is frame_trigger but gate.frame_type is empty — TX will never fire; "
                  "use idle_gap or fixed_delay instead");
  }
  // Pre-allocate scratch buffers to avoid per-frame heap churn on the main receive path.
  this->raw_frame_.reserve(this->max_frame_length_);
  this->rx_unescaped_.reserve(this->max_frame_length_);
  this->rx_payload_.reserve(this->max_frame_length_);
  this->tx_payload_buf_.reserve(16);
  this->tx_escaped_buf_.reserve(this->max_frame_length_);
  this->tx_frame_buf_.reserve(this->max_frame_length_ + 8);
  this->tx_queue_.reserve(this->max_queue_size_);
}

void FramedRS485Hub::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  this->read_uart_(now);

  // Reset receive state if a partial frame has been sitting on the bus longer than the
  // intra-frame timeout. Without this, a cable pull or noise burst mid-frame would
  // permanently stall reception until the next valid frame terminator arrived.
  if (this->in_frame_ && this->in_frame_timeout_ms_ > 0 && now - this->last_rx_time_ >= this->in_frame_timeout_ms_) {
    ESP_LOGW(TAG, "Intra-frame timeout — resetting receive state");
    this->in_frame_ = false;
    this->raw_frame_.clear();
  }

  this->maybe_tx_(now);
  // Unsigned subtraction wraps correctly so this comparison handles the 49-day millis rollover.
  if (this->tx_start_pending_ && now - this->tx_start_at_ < 0x80000000UL) {
    this->tx_start_pending_ = false;
    this->set_tx_mode_(true);
    this->write_array(this->pending_tx_frame_);
    this->flush();
    ESP_LOGD(TAG, "TX %s",
             format_hex(this->pending_tx_frame_).c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg): debug path,
                                                            // compiled out in non-debug builds; frame size is variable
    this->last_tx_time_ = now;
    this->commands_sent_++;
    this->tx_release_at_ = now + this->tx_guard_time_;
    this->tx_release_pending_ = true;
    if (this->tx_guard_time_ == 0)
      this->release_tx_();
  }
  if (this->tx_release_pending_ && now - this->tx_release_at_ < 0x80000000UL) {
    this->release_tx_();
  }
}

void FramedRS485Hub::dump_config() {
  ESP_LOGCONFIG(TAG, "Framed RS-485:");
  ESP_LOGCONFIG(TAG, "  Framing: DLE=0x%02x STX=0x%02x ETX=0x%02x ESC=0x%02x", this->dle_, this->stx_, this->etx_,
                this->escape_byte_);
  ESP_LOGCONFIG(TAG, "  CRC type: %s, accept header CRC: %s, accept payload CRC: %s", crc_type_str(this->crc_type_),
                YESNO(this->accept_header_crc_), YESNO(this->accept_payload_crc_));
  char gate_hex[format_hex_size(8)];  // gate frame type is at most a few bytes; 8 is generous
  format_hex_to(gate_hex, this->tx_gate_frame_type_.data(), this->tx_gate_frame_type_.size());
  ESP_LOGCONFIG(TAG, "  TX gate: %s, gate frame: %s, gate delay: %ums", tx_gate_mode_str(this->tx_gate_mode_), gate_hex,
                this->tx_gate_delay_);
  ESP_LOGCONFIG(TAG, "  TX idle gap: %ums, TX interval: %ums, guard: %ums", this->tx_idle_gap_,
                this->tx_fixed_interval_, this->tx_guard_time_);
  ESP_LOGCONFIG(TAG, "  Queue policy: %s, queue size: %u", queue_policy_str(this->queue_policy_),
                this->max_queue_size_);
  ESP_LOGCONFIG(TAG, "  Sniffer only: %s, dump frames: %s", YESNO(this->sniffer_only_), YESNO(this->dump_frames_));
  LOG_PIN("  Flow Control Pin: ", this->flow_control_pin_);
}

void FramedRS485Hub::set_framing(uint8_t dle, uint8_t stx, uint8_t etx, uint8_t escape_byte) {
  this->dle_ = dle;
  this->stx_ = stx;
  this->etx_ = etx;
  this->escape_byte_ = escape_byte;
}

bool FramedRS485Hub::queue_command_value(uint32_t command) {
  if (this->sniffer_only_) {
    ESP_LOGW(TAG, "Ignoring command because sniffer_only is enabled");
    this->command_drops_++;
    return false;
  }
  this->build_key_payload_(command, this->tx_payload_buf_);
  this->build_frame_(this->tx_payload_buf_, this->tx_frame_buf_);
  if (this->queue_policy_ == QUEUE_REPLACE_LATEST) {
    if (this->queue_size_() > 0 || this->tx_start_pending_) {
      ESP_LOGW(TAG, "Replacing pending framed_rs485 command before it was transmitted");
      this->command_drops_++;
    }
    if (this->tx_start_pending_) {
      this->pending_tx_frame_ = this->tx_frame_buf_;
      return true;
    }
    this->tx_queue_.clear();
    this->tx_queue_head_ = 0;
  } else {
    if (this->queue_size_() + (this->tx_start_pending_ ? 1 : 0) >= this->max_queue_size_) {
      ESP_LOGW(TAG, "framed_rs485 command queue full; dropping new command");
      this->command_drops_++;
      return false;
    }
  }
  this->tx_queue_.push_back(this->tx_frame_buf_);
  return true;
}

void FramedRS485Hub::read_uart_(uint32_t now) {
  uint8_t byte;
  while (this->available() && this->read_byte(&byte)) {
    this->last_rx_time_ = now;
    if (!this->in_frame_) {
      if (this->previous_byte_ == this->dle_ && byte == this->stx_) {
        this->in_frame_ = true;
        this->raw_frame_.clear();
        this->raw_frame_.push_back(this->dle_);
        this->raw_frame_.push_back(this->stx_);
      }
      this->previous_byte_ = byte;
      continue;
    }

    this->raw_frame_.push_back(byte);
    if (this->raw_frame_.size() > this->max_frame_length_) {
      ESP_LOGW(TAG, "Frame exceeded max_frame_length");
      this->in_frame_ = false;
      this->raw_frame_.clear();
      this->previous_byte_ = byte;
      continue;
    }

    const size_t size = this->raw_frame_.size();
    if (size >= 2 && this->raw_frame_[size - 2] == this->dle_ && byte == this->etx_) {
      this->process_raw_frame_(now);
      this->in_frame_ = false;
      this->raw_frame_.clear();
    }
    this->previous_byte_ = byte;
  }
}

void FramedRS485Hub::process_raw_frame_(uint32_t now) {
  if (!this->validate_frame_()) {
    this->crc_failures_++;
    return;
  }

  this->frames_received_++;
  this->update_last_frame_type_();
  if (this->dump_frames_) {
    ESP_LOGD(TAG, "RX %s",
             format_hex(this->rx_payload_).c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg): debug path gated by
                                                      // dump_frames; payload size is variable
  }

  if (this->frame_type_equals_(this->rx_payload_, this->tx_gate_frame_type_)) {
    if (this->last_ka_seen_)
      this->last_keepalive_ms_ = now - this->last_ka_time_;
    this->last_ka_seen_ = true;
    this->last_ka_time_ = now;
    if (this->tx_gate_mode_ == TX_GATE_FRAME_TRIGGER)
      this->send_next_(now);
  }

  for (auto *listener : this->listeners_) {
    if (listener->matches(this->rx_payload_))
      listener->handle_frame(this, this->rx_payload_, now);
  }
}

bool FramedRS485Hub::validate_frame_() {
  const auto &frame = this->raw_frame_;
  if (frame.size() < 6 || frame[0] != this->dle_ || frame[1] != this->stx_ || frame[frame.size() - 2] != this->dle_ ||
      frame[frame.size() - 1] != this->etx_)
    return false;

  this->rx_unescaped_.clear();
  for (size_t i = 2; i + 2 < frame.size(); i++) {
    uint8_t b = frame[i];
    if (b == this->dle_ && i + 1 < frame.size() - 2 && frame[i + 1] == this->escape_byte_) {
      this->rx_unescaped_.push_back(this->dle_);
      i++;
    } else {
      this->rx_unescaped_.push_back(b);
    }
  }
  size_t crc_len = this->crc_length_();
  if (this->rx_unescaped_.size() < 2 + crc_len)
    return false;

  this->rx_payload_.assign(this->rx_unescaped_.begin(), this->rx_unescaped_.end() - crc_len);
  if (crc_len == 0)
    return true;

  uint16_t received_crc = 0;
  if (crc_len == 1) {
    received_crc = this->rx_unescaped_.back();
  } else if (this->crc_type_ == CRC_TYPE_CRC16_MODBUS) {
    received_crc =
        this->rx_unescaped_[this->rx_unescaped_.size() - 2] | (static_cast<uint16_t>(this->rx_unescaped_.back()) << 8);
  } else {
    received_crc =
        (static_cast<uint16_t>(this->rx_unescaped_[this->rx_unescaped_.size() - 2]) << 8) | this->rx_unescaped_.back();
  }
  bool header_ok = this->accept_header_crc_ && this->calculate_crc_(this->rx_payload_, true) == received_crc;
  bool payload_ok = this->accept_payload_crc_ && this->calculate_crc_(this->rx_payload_, false) == received_crc;
  return header_ok || payload_ok;
}

uint16_t FramedRS485Hub::calculate_crc_(const std::vector<uint8_t> &payload, bool include_header) const {
  if (this->crc_type_ == CRC_TYPE_NONE)
    return 0;

  if (this->crc_type_ == CRC_TYPE_XOR8) {
    uint8_t crc = 0;
    if (include_header) {
      crc ^= this->dle_;
      crc ^= this->stx_;
    }
    for (auto b : payload)
      crc ^= b;
    return crc;
  }

  if (this->crc_type_ == CRC_TYPE_CRC16_MODBUS) {
    uint16_t crc = 0xFFFF;
    auto process_byte = [&crc](uint8_t b) {
      crc ^= b;
      for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x0001)
          crc = (crc >> 1) ^ 0xA001;
        else
          crc >>= 1;
      }
    };
    if (include_header) {
      process_byte(this->dle_);
      process_byte(this->stx_);
    }
    for (auto b : payload)
      process_byte(b);
    return crc;
  }

  uint32_t sum = 0;
  if (include_header) {
    sum += this->dle_;
    sum += this->stx_;
  }
  for (auto b : payload)
    sum += b;
  return this->crc_type_ == CRC_TYPE_SUM8 ? (sum & 0xFF) : (sum & 0xFFFF);
}

size_t FramedRS485Hub::crc_length_() const {
  switch (this->crc_type_) {
    case CRC_TYPE_NONE:
      return 0;
    case CRC_TYPE_SUM8:
    case CRC_TYPE_XOR8:
      return 1;
    case CRC_TYPE_SUM16:
    case CRC_TYPE_CRC16_MODBUS:
      return 2;
  }
  return 2;
}

void FramedRS485Hub::escape_dle_(const std::vector<uint8_t> &data, std::vector<uint8_t> &out) const {
  out.clear();
  out.reserve(data.size());
  for (auto b : data) {
    out.push_back(b);
    if (b == this->dle_)
      out.push_back(this->escape_byte_);
  }
}

void FramedRS485Hub::build_frame_(const std::vector<uint8_t> &payload, std::vector<uint8_t> &out) {
  out.clear();
  out.push_back(this->dle_);
  out.push_back(this->stx_);
  this->escape_dle_(payload, this->tx_escaped_buf_);
  out.insert(out.end(), this->tx_escaped_buf_.begin(), this->tx_escaped_buf_.end());

  size_t crc_len = this->crc_length_();
  if (crc_len > 0) {
    uint16_t crc = this->calculate_crc_(payload, this->tx_crc_variant_ == CRC_HEADER_INCLUSIVE);
    uint8_t crc_bytes[2];
    size_t ncrc = 0;
    if (crc_len == 1) {
      crc_bytes[ncrc++] = crc & 0xFF;
    } else if (this->crc_type_ == CRC_TYPE_CRC16_MODBUS) {
      crc_bytes[ncrc++] = crc & 0xFF;
      crc_bytes[ncrc++] = (crc >> 8) & 0xFF;
    } else {
      crc_bytes[ncrc++] = (crc >> 8) & 0xFF;
      crc_bytes[ncrc++] = crc & 0xFF;
    }
    for (size_t i = 0; i < ncrc; i++) {
      out.push_back(crc_bytes[i]);
      if (crc_bytes[i] == this->dle_)
        out.push_back(this->escape_byte_);
    }
  }
  out.push_back(this->dle_);
  out.push_back(this->etx_);
}

void FramedRS485Hub::build_key_payload_(uint32_t command, std::vector<uint8_t> &out) const {
  out.clear();
  if (this->key_format_ == KEY_FORMAT_WIRELESS_9BYTE) {
    // Hayward wireless remote frame type 0x0083: 3-byte header + 4-byte key × 2 + 1 pad byte.
    // Source: https://github.com/swilson/aqualogic (bus captures, wireless remote protocol).
    out.push_back(0x00);
    out.push_back(0x83);  // frame sub-type: wireless keypress
    out.push_back(0x01);  // sequence / channel byte, always 0x01 for single remote
    for (int repeat = 0; repeat < 2; repeat++) {
      out.push_back((command >> 24) & 0xFF);
      out.push_back((command >> 16) & 0xFF);
      out.push_back((command >> 8) & 0xFF);
      out.push_back(command & 0xFF);
    }
    out.push_back(0x00);  // trailing pad
    return;
  }

  if (this->key_format_ == KEY_FORMAT_JANDY_ALLBUTTON) {
    // Jandy AquaLink RS AllButton frame: 3-byte header + 1-byte button code.
    // Source: Jandy RS-485 protocol documentation.
    out.push_back(0x00);
    out.push_back(0x01);  // frame sub-type: AllButton keypress
    out.push_back(0x80);  // AllButton master address
    out.push_back(static_cast<uint8_t>(command & 0xFF));
    return;
  }

  // Hayward wired remote / wired local: 2-byte frame type + 2-byte key + 2 pad bytes.
  // Sub-type 0x03 = wired remote, 0x02 = wired local (local panel).
  out.push_back(0x00);
  out.push_back(this->key_format_ == KEY_FORMAT_WIRED_REMOTE ? 0x03 : 0x02);
  uint16_t key = (command >> 16) & 0xFFFF;
  out.push_back((key >> 8) & 0xFF);
  out.push_back(key & 0xFF);
  out.push_back(0x00);
  out.push_back(0x00);
}

void FramedRS485Hub::maybe_tx_(uint32_t now) {
  if (this->sniffer_only_ || this->queue_size_() == 0 || this->tx_start_pending_ || this->tx_release_pending_)
    return;
  if (this->tx_gate_mode_ == TX_GATE_IDLE_GAP && this->last_rx_time_ != 0 &&
      now - this->last_rx_time_ >= this->tx_idle_gap_)
    this->send_next_(now);
  if (this->tx_gate_mode_ == TX_GATE_FIXED_DELAY &&
      (this->last_tx_time_ == 0 || now - this->last_tx_time_ >= this->tx_fixed_interval_))
    this->send_next_(now);
}

void FramedRS485Hub::queue_pop_front_() {
  this->tx_queue_head_++;
  // Reclaim storage once all elements have been consumed to avoid unbounded growth.
  if (this->tx_queue_head_ >= this->tx_queue_.size()) {
    this->tx_queue_.clear();
    this->tx_queue_head_ = 0;
  }
}

void FramedRS485Hub::send_next_(uint32_t now) {
  if (this->sniffer_only_ || this->tx_start_pending_)
    return;
  if (this->queue_size_() == 0) {
    if (this->has_idle_command_ && !this->tx_release_pending_)
      this->send_next_idle_(now);
    return;
  }

  if (this->tx_gate_delay_ > 0) {
    this->pending_tx_frame_ = this->tx_queue_[this->tx_queue_head_];
    this->queue_pop_front_();
    this->tx_start_at_ = now + this->tx_gate_delay_;
    this->tx_start_pending_ = true;
    return;
  }

  {
    const auto &frame = this->tx_queue_[this->tx_queue_head_];
    this->set_tx_mode_(true);
    this->write_array(frame);
    this->flush();
    ESP_LOGD(TAG, "TX %s", format_hex(frame).c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg): debug path,
                                                        // compiled out in non-debug builds; frame size is variable
  }
  this->queue_pop_front_();
  this->last_tx_time_ = now;
  this->commands_sent_++;
  this->tx_release_at_ = now + this->tx_guard_time_;
  this->tx_release_pending_ = true;
  if (this->tx_guard_time_ == 0)
    this->release_tx_();
}

void FramedRS485Hub::send_next_idle_(uint32_t now) {
  this->build_key_payload_(this->idle_command_, this->tx_payload_buf_);
  this->build_frame_(this->tx_payload_buf_, this->tx_frame_buf_);
  if (this->tx_gate_delay_ > 0) {
    this->pending_tx_frame_ = this->tx_frame_buf_;
    this->tx_start_at_ = now + this->tx_gate_delay_;
    this->tx_start_pending_ = true;
    return;
  }
  this->set_tx_mode_(true);
  this->write_array(this->tx_frame_buf_);
  this->flush();
  this->last_tx_time_ = now;
  this->commands_sent_++;
  this->tx_release_at_ = now + this->tx_guard_time_;
  this->tx_release_pending_ = true;
  if (this->tx_guard_time_ == 0)
    this->release_tx_();
  ESP_LOGD(TAG, "TX idle %s",
           format_hex(this->tx_frame_buf_).c_str());  // NOLINT(cppcoreguidelines-pro-type-vararg): debug path, compiled
                                                      // out in non-debug builds; frame size is variable
}

bool FramedRS485Hub::queue_raw_frame(const std::vector<uint8_t> &payload) {
  if (this->sniffer_only_) {
    ESP_LOGW(TAG, "Ignoring raw frame because sniffer_only is enabled");
    this->command_drops_++;
    return false;
  }
  this->build_frame_(payload, this->tx_frame_buf_);
  if (this->queue_policy_ == QUEUE_REPLACE_LATEST) {
    if (this->queue_size_() > 0 || this->tx_start_pending_) {
      ESP_LOGW(TAG, "Replacing pending framed_rs485 frame before it was transmitted");
      this->command_drops_++;
    }
    if (this->tx_start_pending_) {
      this->pending_tx_frame_ = this->tx_frame_buf_;
      return true;
    }
    this->tx_queue_.clear();
    this->tx_queue_head_ = 0;
  } else {
    if (this->queue_size_() + (this->tx_start_pending_ ? 1 : 0) >= this->max_queue_size_) {
      ESP_LOGW(TAG, "framed_rs485 raw frame queue full; dropping frame");
      this->command_drops_++;
      return false;
    }
  }
  this->tx_queue_.push_back(this->tx_frame_buf_);
  return true;
}

void FramedRS485Hub::set_tx_mode_(bool tx) {
  if (this->flow_control_pin_ != nullptr)
    this->flow_control_pin_->digital_write(tx);
}

void FramedRS485Hub::release_tx_() {
  this->tx_release_pending_ = false;
  this->set_tx_mode_(false);
}

bool FramedRS485Hub::frame_type_equals_(const std::vector<uint8_t> &payload,
                                        const std::vector<uint8_t> &frame_type) const {
  if (frame_type.empty() || payload.size() < frame_type.size())
    return false;
  return std::equal(frame_type.begin(), frame_type.end(), payload.begin());
}

void FramedRS485Hub::update_last_frame_type_() {
  size_t len = std::min(this->rx_payload_.size(), size_t(2));
  format_hex_to(this->last_frame_type_, this->rx_payload_.data(), len);
}

uint32_t FramedRS485Hub::decode_led_mask(const std::vector<uint8_t> &payload) {
  if (payload.size() < 6)
    return 0;
  return static_cast<uint32_t>(payload[2]) | (static_cast<uint32_t>(payload[3]) << 8) |
         (static_cast<uint32_t>(payload[4]) << 16) | (static_cast<uint32_t>(payload[5]) << 24);
}

uint32_t FramedRS485Hub::decode_led_mask_blinking(const std::vector<uint8_t> &payload) {
  if (payload.size() < 10)
    return 0;
  return static_cast<uint32_t>(payload[6]) | (static_cast<uint32_t>(payload[7]) << 8) |
         (static_cast<uint32_t>(payload[8]) << 16) | (static_cast<uint32_t>(payload[9]) << 24);
}

void FramedRS485Hub::decode_display_text(const std::vector<uint8_t> &payload, std::string &out) {
  out.clear();
  // Display payload starts at byte 3; bytes 0-2 are frame header fields.
  if (payload.size() <= 3)
    return;
  for (size_t i = 3; i < payload.size(); i++) {
    uint8_t b = payload[i];
    if (b > 0x7F)
      b -= 0x80;  // strip blink bit — same glyph whether blinking or solid
    if (b == 0)
      continue;
    if (b == '_') {
      out += "\xC2\xB0";  // '_' encodes the degree symbol on AquaLogic displays
    } else {
      out.push_back(static_cast<char>(b));
    }
  }
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
    out.pop_back();
  size_t start = 0;
  while (start < out.size() && std::isspace(static_cast<unsigned char>(out[start])))
    start++;
  if (start > 0)
    out.erase(0, start);
}

void FramedRS485Hub::decode_display_blink_text(const std::vector<uint8_t> &payload, std::string &out) {
  out.clear();
  // Same layout as decode_display_text, but only emit characters that had bit 7 set
  // (i.e., were blinking on the physical display). Returns empty when nothing blinks.
  if (payload.size() <= 3)
    return;
  for (size_t i = 3; i < payload.size(); i++) {
    uint8_t b = payload[i];
    if (b <= 0x7F)
      continue;  // solid character — not blinking
    b -= 0x80;
    if (b == 0)
      continue;
    if (b == '_') {
      out += "\xC2\xB0";
    } else {
      out.push_back(static_cast<char>(b));
    }
  }
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
    out.pop_back();
  size_t start = 0;
  while (start < out.size() && std::isspace(static_cast<unsigned char>(out[start])))
    start++;
  if (start > 0)
    out.erase(0, start);
}

std::string FramedRS485Hub::normalize_display_ws(const std::string &s) {
  std::string out;
  // Start in_space=true so leading whitespace is skipped without special casing.
  bool in_space = true;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!in_space)
        out.push_back(' ');
      in_space = true;
    } else {
      out.push_back(c);
      in_space = false;
    }
  }
  if (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

#ifdef USE_BUTTON
void FramedRS485Button::press_action() { this->parent_->queue_command_value(this->command_value_); }
#endif  // USE_BUTTON

#ifdef USE_BINARY_SENSOR
void FramedRS485BinarySensor::add_match_on(const std::string &s) {
  this->match_on_.push_back(FramedRS485Hub::normalize_display_ws(s));
}
void FramedRS485BinarySensor::add_match_off(const std::string &s) {
  this->match_off_.push_back(FramedRS485Hub::normalize_display_ws(s));
}

// Returns true if all needles appear (case-insensitive substring) in haystack.
// Needles are expected to be pre-normalized (no extra whitespace).
static bool all_match(const std::string &haystack, const std::vector<std::string> &needles) {
  for (const auto &needle : needles) {
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
    if (it == haystack.end())
      return false;
  }
  return true;
}

void FramedRS485BinarySensor::handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) {
  if (this->lambda_ != nullptr) {
    auto value = this->lambda_(payload);
    if (value.has_value())
      this->publish_state(value.value());
    return;
  }

  if (this->decode_ == BINARY_DECODE_LED_BIT) {
    uint32_t mask = FramedRS485Hub::decode_led_mask(payload);
    this->publish_state((mask & (1UL << this->bit_)) != 0);
    return;
  }

  if (this->decode_ == BINARY_DECODE_DISPLAY_TEXT_MATCH) {
    std::string text;
    FramedRS485Hub::decode_display_text(payload, text);
    text = FramedRS485Hub::normalize_display_ws(text);

    // Proof positive: all match_on strings present — latch true.
    if (!this->match_on_.empty() && all_match(text, this->match_on_)) {
      this->last_proof_ms_ = now;
      this->timed_out_ = false;
      this->publish_state(true);
      return;
    }

    // Proof negative: all match_off strings present — latch false.
    if (!this->match_off_.empty() && all_match(text, this->match_off_)) {
      this->last_proof_ms_ = now;
      this->timed_out_ = false;
      this->publish_state(false);
      return;
    }

    // Neither proof seen on this frame — check timeout.
    // last_proof_ms_ == 0 means we have not seen any proof yet; don't expire before
    // the sensor has ever had a known state.
    if (this->timeout_ms_ > 0 && this->last_proof_ms_ != 0 && !this->timed_out_) {
      if (now - this->last_proof_ms_ >= this->timeout_ms_) {
        this->timed_out_ = true;
        this->publish_state(false);
      }
    }
  }
}
#endif  // USE_BINARY_SENSOR

#ifdef USE_SENSOR
optional<float> FramedRS485Sensor::decode_builtin_(FramedRS485Hub *hub, const std::vector<uint8_t> &payload) const {
  switch (this->decode_) {
    case SENSOR_DECODE_LED_MASK:
      return static_cast<float>(FramedRS485Hub::decode_led_mask(payload));
    case SENSOR_DECODE_LED_MASK_BLINKING:
      return static_cast<float>(FramedRS485Hub::decode_led_mask_blinking(payload));
    case SENSOR_DECODE_DISPLAY_TEMPERATURE: {
      std::string text;
      FramedRS485Hub::decode_display_text(payload, text);
      // Locate the label first so that multi-temperature displays ("Air 68F Pool 84F")
      // return the digit nearest the label rather than the first digit in the string.
      size_t scan_from = 0;
      if (!this->temperature_label_.empty()) {
        size_t pos = std::string::npos;
        for (size_t p = 0; p + this->temperature_label_.size() <= text.size(); p++) {
          bool match = true;
          for (size_t j = 0; j < this->temperature_label_.size() && match; j++) {
            if (std::tolower(static_cast<unsigned char>(text[p + j])) !=
                std::tolower(static_cast<unsigned char>(this->temperature_label_[j])))
              match = false;
          }
          if (match) {
            pos = p;
            break;
          }
        }
        if (pos == std::string::npos)
          return {};
        scan_from = pos;
      }
      for (size_t i = scan_from; i < text.size(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
          continue;
        size_t end = i;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
          end++;
        size_t limit = std::min(end + 3, text.size());
        bool has_unit = false;
        for (size_t j = end; j < limit && !has_unit; j++) {
          char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(text[j])));
          if (lc == 'f' || lc == 'c') {
            has_unit = true;
          } else if (j + 1 < limit && static_cast<unsigned char>(text[j]) == 0xC2 &&
                     static_cast<unsigned char>(text[j + 1]) == 0xB0) {
            has_unit = true;
          }
        }
        if (has_unit)
          return static_cast<float>(std::strtol(&text[i], nullptr, 10));
        i = end;
      }
      return {};
    }
    case SENSOR_DECODE_UINT8:
      if (payload.size() <= this->offset_)
        return {};
      return static_cast<float>(payload[this->offset_]);
    case SENSOR_DECODE_UINT16_BE:
      if (payload.size() <= this->offset_ + 1)
        return {};
      return static_cast<float>((static_cast<uint16_t>(payload[this->offset_]) << 8) | payload[this->offset_ + 1]);
    case SENSOR_DECODE_UINT16_LE:
      if (payload.size() <= this->offset_ + 1)
        return {};
      return static_cast<float>(payload[this->offset_] | (static_cast<uint16_t>(payload[this->offset_ + 1]) << 8));
    case SENSOR_DECODE_UINT32_BE:
      if (payload.size() <= this->offset_ + 3)
        return {};
      return static_cast<float>((static_cast<uint32_t>(payload[this->offset_]) << 24) |
                                (static_cast<uint32_t>(payload[this->offset_ + 1]) << 16) |
                                (static_cast<uint32_t>(payload[this->offset_ + 2]) << 8) |
                                static_cast<uint32_t>(payload[this->offset_ + 3]));
    case SENSOR_DECODE_UINT32_LE:
      if (payload.size() <= this->offset_ + 3)
        return {};
      return static_cast<float>(static_cast<uint32_t>(payload[this->offset_]) |
                                (static_cast<uint32_t>(payload[this->offset_ + 1]) << 8) |
                                (static_cast<uint32_t>(payload[this->offset_ + 2]) << 16) |
                                (static_cast<uint32_t>(payload[this->offset_ + 3]) << 24));
    case SENSOR_DECODE_BCD:
      if (payload.size() <= this->offset_)
        return {};
      return static_cast<float>(((payload[this->offset_] & 0xF0) >> 4) * 10 + (payload[this->offset_] & 0x0F));
    case SENSOR_DECODE_VSP_SPEED_REQUEST:
      if (payload.size() < 4)
        return {};
      return static_cast<float>((static_cast<uint16_t>(payload[2]) << 8) | payload[3]);
    case SENSOR_DECODE_VSP_POWER_BCD:
      if (payload.size() < 7)
        return {};
      return static_cast<float>(((payload[5] & 0xF0) >> 4) * 1000 + (payload[5] & 0x0F) * 100 +
                                ((payload[6] & 0xF0) >> 4) * 10 + (payload[6] & 0x0F));
    case SENSOR_DECODE_FRAMES_RECEIVED:
      return static_cast<float>(hub->get_frames_received());
    case SENSOR_DECODE_CRC_FAILURES:
      return static_cast<float>(hub->get_crc_failures());
    case SENSOR_DECODE_COMMANDS_SENT:
      return static_cast<float>(hub->get_commands_sent());
    case SENSOR_DECODE_COMMAND_DROPS:
      return static_cast<float>(hub->get_command_drops());
    case SENSOR_DECODE_LAST_KEEPALIVE_MS:
      return static_cast<float>(hub->get_last_keepalive_ms());
    case SENSOR_DECODE_QUEUE_DEPTH:
      return static_cast<float>(hub->get_queue_depth());
  }
  return {};
}

void FramedRS485Sensor::handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) {
  optional<float> value = this->lambda_ != nullptr ? this->lambda_(payload) : this->decode_builtin_(hub, payload);
  if (value.has_value())
    this->publish_state(value.value());
}
#endif  // USE_SENSOR

#ifdef USE_TEXT_SENSOR
void FramedRS485TextSensor::handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) {
  optional<std::string> value;
  if (this->lambda_ != nullptr) {
    value = this->lambda_(payload);
  } else if (this->decode_ == TEXT_DECODE_DISPLAY_TEXT) {
    std::string text;
    FramedRS485Hub::decode_display_text(payload, text);
    value = std::move(text);
  } else if (this->decode_ == TEXT_DECODE_DISPLAY_BLINK_TEXT) {
    // Non-empty when any character on the display is blinking. On AquaLogic this
    // typically indicates an alarm, a mode-change in progress, or a value being set.
    std::string text;
    FramedRS485Hub::decode_display_blink_text(payload, text);
    value = std::move(text);
  } else if (this->decode_ == TEXT_DECODE_LAST_FRAME_TYPE) {
    value = std::string(hub->get_last_frame_type());
  }
  if (value.has_value())
    this->publish_state(value.value());
}
#endif  // USE_TEXT_SENSOR

#ifdef USE_NUMBER
void FramedRS485Number::control(float value) {
  if (this->lambda_ == nullptr)
    return;
  auto payload = this->lambda_(value);
  if (!payload.has_value())
    return;
  if (this->parent_->queue_raw_frame(payload.value()))
    this->publish_state(value);
}
#endif  // USE_NUMBER

}  // namespace esphome::framed_rs485
