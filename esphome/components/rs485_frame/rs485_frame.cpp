#include "rs485_frame.h"

#include "esphome/core/application.h"

#ifdef USE_BUTTON
#include "button/rs485_frame_button.h"
#endif
#ifdef USE_NUMBER
#include "number/rs485_frame_number.h"
#endif

#include <algorithm>
#include <cinttypes>

namespace esphome::rs485_frame {

static const char *const TAG = "rs485_frame";

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

bool RS485FrameTrigger::matches(const std::vector<uint8_t> &payload) const {
  // Empty prefix list = match every frame (documented `frame_type: []` behavior). Any
  // prefix that is itself empty also matches — kept for backward compatibility with
  // older codegen paths, though the current to_code skips zero-length prefixes.
  if (this->frame_types_.empty())
    return true;
  for (const auto &prefix : this->frame_types_) {
    if (prefix.empty())
      return true;
    if (payload.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), payload.begin()))
      return true;
  }
  return false;
}

void RS485FrameHub::setup() {
  // Pre-allocate scratch buffers to avoid per-frame heap churn on the main receive path.
  const size_t tx_slot_capacity = this->max_frame_length_ * 2 + FRAME_OVERHEAD_BYTES;
  this->raw_frame_.reserve(this->max_frame_length_);
  this->rx_unescaped_.reserve(this->max_frame_length_);
  this->rx_payload_.reserve(this->max_frame_length_);
  this->tx_payload_buf_.reserve(MAX_KEY_PAYLOAD_LEN);
  // tx_escaped_buf_ worst case: every payload byte is DLE and requires an escape byte.
  this->tx_escaped_buf_.reserve(this->max_frame_length_ * 2);
  this->tx_frame_buf_.reserve(tx_slot_capacity);
  this->pending_tx_frame_.reserve(tx_slot_capacity);
  // Ring buffer: pre-size and pre-reserve each slot so enqueue only swaps, never allocates.
  this->tx_queue_.resize(this->max_queue_size_);
  for (auto &slot : this->tx_queue_)
    slot.reserve(tx_slot_capacity);
  // Hex-text log buffer sized to the worst-case TX frame (2 hex chars per byte + NUL).
  this->hex_log_buf_size_ = tx_slot_capacity * 2 + 1;
  this->hex_log_buf_ = std::make_unique<char[]>(this->hex_log_buf_size_);
}

void RS485FrameHub::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  this->read_uart_(now);

  // Reset receive state if a partial frame has been sitting on the bus longer than the
  // intra-frame timeout. Without this, a cable pull or noise burst mid-frame would
  // permanently stall reception until the next valid frame terminator arrived.
  if (this->in_frame_ && this->in_frame_timeout_ms_ > 0 && now - this->last_rx_time_ >= this->in_frame_timeout_ms_) {
    ESP_LOGW(TAG, "Intra-frame timeout — resetting receive state");
    this->in_frame_ = false;
    this->raw_frame_.clear();
    // Reset previous_byte_ so a stale DLE before the timeout can't combine with the next STX
    // to spuriously start a new frame on the first byte received after recovery.
    this->previous_byte_ = 0;
  }

  this->maybe_tx_(now);
  // Unsigned subtraction wraps correctly so this comparison handles the 49-day millis rollover.
  if (this->tx_start_pending_ && now - this->tx_start_at_ < 0x80000000UL) {
    this->tx_start_pending_ = false;
    this->write_frame_(this->pending_tx_frame_);
    this->last_tx_time_ = now;
    if (!this->pending_is_idle_)
      this->commands_sent_++;
    this->pending_is_idle_ = false;
  }

#ifdef USE_RS485_FRAME_SNIFFER_STATS
  if (this->sniffer_stats_ != nullptr)
    this->sniffer_stats_->tick(now);
#endif
}

void RS485FrameHub::dump_config() {
  // StaticVector caps the size at MAX_FRAME_TYPE_LEN, so no run-time bound needed.
  char gate_hex[format_hex_size(MAX_FRAME_TYPE_LEN)];
  format_hex_to(gate_hex, this->tx_gate_frame_type_.data(), this->tx_gate_frame_type_.size());
  // Consolidated multi-line ESP_LOGCONFIG (matches modbus_server style) to save flash.
  ESP_LOGCONFIG(TAG,
                "RS485 Frame:\n"
                "  Framing: DLE=0x%02x STX=0x%02x ETX=0x%02x ESC=0x%02x\n"
                "  CRC type: %s, accept header CRC: %s, accept payload CRC: %s\n"
                "  TX gate: %s, gate frame: %s, gate delay: %" PRIu32 "ms\n"
                "  TX idle gap: %" PRIu32 "ms, TX interval: %" PRIu32 "ms\n"
                "  Queue policy: %s, queue size: %" PRIu32 "\n"
                "  Max frame length: %" PRIu32 ", frame timeout: %" PRIu32 "ms\n"
                "  Sniffer only: %s, dump frames: %s",
                this->dle_, this->stx_, this->etx_, this->escape_byte_, crc_type_str(this->crc_type_),
                YESNO(this->accept_header_crc_), YESNO(this->accept_payload_crc_),
                tx_gate_mode_str(this->tx_gate_mode_), gate_hex, this->tx_gate_delay_, this->tx_idle_gap_,
                this->tx_fixed_interval_, queue_policy_str(this->queue_policy_), this->max_queue_size_,
                this->max_frame_length_, this->in_frame_timeout_ms_, YESNO(this->sniffer_only_),
                YESNO(this->dump_frames_));
}

void RS485FrameHub::set_framing(uint8_t dle, uint8_t stx, uint8_t etx, uint8_t escape_byte) {
  this->dle_ = dle;
  this->stx_ = stx;
  this->etx_ = etx;
  this->escape_byte_ = escape_byte;
}

bool RS485FrameHub::queue_command_value(uint32_t command) {
  if (this->sniffer_only_) {
    ESP_LOGW(TAG, "Ignoring command because sniffer_only is enabled");
    this->command_drops_++;
    return false;
  }
  this->build_key_payload_(command, this->tx_payload_buf_);
  this->build_frame_(this->tx_payload_buf_, this->tx_frame_buf_);
  return this->enqueue_frame_();
}

bool RS485FrameHub::enqueue_frame_() {
  if (this->queue_policy_ == QUEUE_REPLACE_LATEST) {
    if (this->queue_size_() > 0 || this->tx_start_pending_) {
      ESP_LOGW(TAG, "Replacing pending rs485_frame frame before it was transmitted");
      this->command_drops_++;
    }
    if (this->tx_start_pending_) {
      // Swap the new frame into pending_tx_frame_ — both are pre-reserved, no allocation.
      std::swap(this->pending_tx_frame_, this->tx_frame_buf_);
      this->pending_is_idle_ = false;
      return true;
    }
    // Reset ring buffer logical state; slot capacity is preserved.
    this->tx_queue_head_ = 0;
    this->tx_queue_tail_ = 0;
    this->tx_queue_count_ = 0;
  } else {
    if (this->tx_queue_count_ + (this->tx_start_pending_ ? 1 : 0) >= this->max_queue_size_) {
      ESP_LOGW(TAG, "rs485_frame frame queue full; dropping frame");
      this->command_drops_++;
      return false;
    }
  }
  // Swap tx_frame_buf_ into the pre-reserved tail slot — no heap allocation.
  // After the swap, tx_frame_buf_ holds the slot's old content (empty, capacity intact).
  std::swap(this->tx_queue_[this->tx_queue_tail_], this->tx_frame_buf_);
  this->tx_queue_tail_ = (this->tx_queue_tail_ + 1) % this->max_queue_size_;
  this->tx_queue_count_++;
  return true;
}

void RS485FrameHub::read_uart_(uint32_t now) {
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

    if (this->raw_frame_.size() >= this->max_frame_length_) {
      ESP_LOGW(TAG, "Frame exceeded max_frame_length");
      this->in_frame_ = false;
      this->raw_frame_.clear();
      this->previous_byte_ = byte;
      continue;
    }
    this->raw_frame_.push_back(byte);

    const size_t size = this->raw_frame_.size();
    if (size >= 2 && this->raw_frame_[size - 2] == this->dle_ && byte == this->etx_) {
      this->process_raw_frame_(now);
      this->in_frame_ = false;
      this->raw_frame_.clear();
    }
    // Maintain previous_byte_ for every in-frame byte too. Without this, a normal frame
    // terminator (DLE ETX) leaves previous_byte_ stale at the value seen just before the
    // frame began (typically DLE), so a bare STX immediately after the terminator would
    // spuriously start a new frame.
    this->previous_byte_ = byte;
  }
}

void RS485FrameHub::process_raw_frame_(uint32_t now) {
  if (!this->validate_frame_()) {
    this->crc_failures_++;
    return;
  }

  this->frames_received_++;
  this->update_last_frame_type_();
  if (this->dump_frames_) {
    // Reuse the setup-time allocated hex_log_buf_ to avoid per-frame heap allocation
    // that the std::vector-returning hex formatter would incur. Buffer is sized for the
    // worst-case TX frame so any RX payload fits.
    format_hex_to(this->hex_log_buf_.get(), this->hex_log_buf_size_, this->rx_payload_.data(),
                  this->rx_payload_.size());
    ESP_LOGD(TAG, "RX %s", this->hex_log_buf_.get());
  }

  if (this->frame_type_equals_(this->rx_payload_, this->tx_gate_frame_type_)) {
    if (this->last_ka_seen_)
      this->last_keepalive_ms_ = now - this->last_ka_time_;
    this->last_ka_seen_ = true;
    this->last_ka_time_ = now;
    if (this->tx_gate_mode_ == TX_GATE_FRAME_TRIGGER)
      this->send_next_(now);
  }

#ifdef USE_RS485_FRAME_SNIFFER_STATS
  if (this->sniffer_stats_ != nullptr)
    this->sniffer_stats_->record(this->rx_payload_, now);
#endif

  for (auto *trigger : this->triggers_) {
    if (trigger->matches(this->rx_payload_))
      trigger->trigger(this->rx_payload_);
  }
}

bool RS485FrameHub::validate_frame_() {
  const auto &frame = this->raw_frame_;
  // Minimum valid frame: DLE(1)+STX(1) + frame_type(2) + CRC_1byte_min(1) + DLE(1)+ETX(1) = 7
  // but sum16 (2-byte CRC) gives minimum 8. The constant 6 is the no-CRC minimum and is the
  // tightest pre-check before crc_length_() is called below.
  if (frame.size() < 6 || frame[0] != this->dle_ || frame[1] != this->stx_ || frame[frame.size() - 2] != this->dle_ ||
      frame[frame.size() - 1] != this->etx_)
    return false;

  this->rx_unescaped_.clear();
  // The frame[] iteration covers bytes between the opening STX (index 1) and the closing
  // DLE+ETX (last 2 bytes). A DLE inside that range must be followed by escape_byte_;
  // any other DLE successor is a protocol violation and the frame is rejected.
  for (size_t i = 2; i + 2 < frame.size(); i++) {
    uint8_t b = frame[i];
    if (b == this->dle_) {
      // i+1 < frame.size()-2 means "DLE has a successor that is not part of the closing
      // DLE+ETX terminator". A DLE that is the first byte of DLE+ETX is handled by the
      // framer (loop terminates before reaching it), so reaching here means a DLE is
      // followed by something that must be the escape byte. Anything else is invalid.
      if (i + 1 < frame.size() - 2 && frame[i + 1] == this->escape_byte_) {
        this->rx_unescaped_.push_back(this->dle_);
        i++;
        continue;
      }
      return false;
    }
    this->rx_unescaped_.push_back(b);
  }
  size_t crc_len = this->crc_length_();
  if (this->rx_unescaped_.size() < 2 + crc_len)
    return false;

  // rx_payload_ is what on_frame: triggers and the frame_type_equals_ gate see. It begins
  // with the frame_type bytes (payload-relative: payload[0..N-1] = frame_type, data starts
  // at payload[N]) and ends just before the CRC. DLE+STX/DLE+ETX framing is excluded;
  // escape bytes are unwrapped. This convention is documented for users in the
  // rs485_frame docs' "Offset convention" section.
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

uint16_t RS485FrameHub::calculate_crc_(const std::vector<uint8_t> &payload, bool include_header) const {
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

size_t RS485FrameHub::crc_length_() const {
  switch (this->crc_type_) {
    case CRC_TYPE_NONE:
      return 0;
    case CRC_TYPE_SUM8:
    case CRC_TYPE_XOR8:
      return 1;
    case CRC_TYPE_SUM16:
    case CRC_TYPE_CRC16_MODBUS:
      return 2;
    default:
      // All CrcType values are handled above. A new CrcType must add a case here
      // AND update calculate_crc_() to keep them in sync.
      return 2;
  }
}

void RS485FrameHub::escape_dle_(const std::vector<uint8_t> &data, std::vector<uint8_t> &out) const {
  out.clear();
  // Worst case: every byte equals DLE and requires an escape byte → 2× input size.
  out.reserve(data.size() * 2);
  for (auto b : data) {
    out.push_back(b);
    if (b == this->dle_)
      out.push_back(this->escape_byte_);
  }
}

void RS485FrameHub::build_frame_(const std::vector<uint8_t> &payload, std::vector<uint8_t> &out) {
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

void RS485FrameHub::build_key_payload_(uint32_t command, std::vector<uint8_t> &out) const {
  out.clear();
  if (this->key_format_ == KEY_FORMAT_WIRELESS_12BYTE) {
    // Hayward wireless remote frame type 0x0083: 3-byte header + 4-byte key × 2 + 1 pad byte = 12 bytes.
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
    // Jandy AquaLink RS AllButton response. Community-contributed; the third byte (0x80
    // here) varies between reverse-engineered descriptions and has not been verified on
    // physical hardware. If your master rejects responses, capture a known-good response
    // and adjust this builder.
    //
    // Protocol envelope reference (DLE STX <data> <checksum> DLE ETX):
    //   https://wiki.jmehan.com/display/KNOW/Jandy+Pool+Heater
    // AllButton ACK byte sequence variants (community reverse-engineering):
    //   https://github.com/earlephilhower/aquaweb/blob/master/protocol.md
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(0x80);
    out.push_back(command & 0xFF);
    return;
  }

  // Hayward wired remote / wired local: 2-byte frame type + 4-byte command (big-endian) ×
  // 2 = 10 bytes payload before CRC. Same layout as the wireless 0x0083 form minus the
  // 1-byte sequence prefix and 1-byte trailing pad — the wired-side controller does not
  // need either, and the local panel observed on a live bus emits exactly this shape.
  //
  // Sub-type 0x02 = wired local (the main keypad on an AquaLogic / ProLogic panel).
  //   Verified against a live AquaLogic bus 2026-05: pressed Menu / Right / AUX 1 / AUX 2 /
  //   Heater / Valve 4 on the panel and observed `00 02 [cmd:4 BE] [cmd:4 BE] [sum16:2 BE]`
  //   frames with the cmd field matching the wireless-remote command map (e.g. 0x00040000
  //   for AUX 2, 0x00000400 for Heater). The second 4-byte block is the press payload
  //   repeated; the panel emits the same block zeroed when reporting key release, but
  //   sending the press-only form is sufficient for the panel to act on the command.
  //
  // Sub-type 0x03 = wired remote (the OEM spa-side wired remote, a separate physical
  //   product on the same bus). Format is extrapolated from wired_local and is not yet
  //   verified on real hardware; the spa-side remote uses the same command map, so the
  //   payload structure is expected to be identical with only the frame sub-type byte
  //   differing. Open an issue if your wired remote does not respond to this format.
  out.push_back(0x00);
  // Sub-type byte: explicit YAML override wins; otherwise pick the key_format default
  // (0x03 for wired_remote = "additional registered keypad" — safer, no collision with
  // main-panel traffic; 0x02 for wired_local = "main panel" — convenient but collides
  // when the user also presses the physical panel).
  uint8_t sub_type = this->has_wired_sub_type_override_ ? this->wired_sub_type_override_
                                                        : (this->key_format_ == KEY_FORMAT_WIRED_REMOTE ? 0x03 : 0x02);
  out.push_back(sub_type);
  for (int repeat = 0; repeat < 2; repeat++) {
    out.push_back((command >> 24) & 0xFF);
    out.push_back((command >> 16) & 0xFF);
    out.push_back((command >> 8) & 0xFF);
    out.push_back(command & 0xFF);
  }
}

void RS485FrameHub::maybe_tx_(uint32_t now) {
  if (this->sniffer_only_ || this->tx_start_pending_)
    return;
  // For idle_gap and fixed_delay modes, fire the gate regardless of queue depth so that
  // idle_command keepalives can be emitted; send_next_() falls through to send_next_idle_()
  // when the queue is empty and an idle command is configured.
  const bool queue_or_idle = this->queue_size_() > 0 || this->has_idle_command_;
  if (!queue_or_idle)
    return;
  if (this->tx_gate_mode_ == TX_GATE_IDLE_GAP && this->last_rx_time_ != 0 &&
      now - this->last_rx_time_ >= this->tx_idle_gap_)
    this->send_next_(now);
  if (this->tx_gate_mode_ == TX_GATE_FIXED_DELAY &&
      (this->last_tx_time_ == 0 || now - this->last_tx_time_ >= this->tx_fixed_interval_))
    this->send_next_(now);
}

void RS485FrameHub::queue_pop_front_() {
  // Clear the slot content but keep its reserved capacity for the next enqueue swap.
  this->tx_queue_[this->tx_queue_head_].clear();
  this->tx_queue_head_ = (this->tx_queue_head_ + 1) % this->max_queue_size_;
  this->tx_queue_count_--;
}

void RS485FrameHub::write_frame_(const std::vector<uint8_t> &frame) {
  this->write_array(frame);
#ifdef USE_ARDUINO
  // On Arduino paths the uart component does not drive flow_control_pin, so users must
  // run on auto-DE transceivers (DE follows the TX line state). flush() prevents the
  // function from returning before bytes are physically on the wire so the transceiver
  // does not flip back to RX mid-frame. On ESP-IDF the hardware RS485 half-duplex mode
  // drives DE/RE from the shift-register-done signal; flush() there is pure busy-wait
  // and is omitted to keep loop() responsive.
  this->flush();
#endif
  if (this->dump_frames_) {
    // Reuse the setup-time allocated hex_log_buf_ — no heap allocation per TX.
    format_hex_to(this->hex_log_buf_.get(), this->hex_log_buf_size_, frame.data(), frame.size());
    ESP_LOGD(TAG, "TX %s", this->hex_log_buf_.get());
  }
}

void RS485FrameHub::send_next_(uint32_t now) {
  if (this->sniffer_only_ || this->tx_start_pending_)
    return;
  if (this->queue_size_() == 0) {
    if (this->has_idle_command_)
      this->send_next_idle_(now);
    return;
  }

  if (this->tx_gate_delay_ > 0) {
    // Swap the frame into pending_tx_frame_ — both are pre-reserved, no allocation.
    std::swap(this->pending_tx_frame_, this->tx_queue_[this->tx_queue_head_]);
    this->queue_pop_front_();
    this->tx_start_at_ = now + this->tx_gate_delay_;
    this->tx_start_pending_ = true;
    this->pending_is_idle_ = false;
    return;
  }

  this->write_frame_(this->tx_queue_[this->tx_queue_head_]);
  this->queue_pop_front_();
  this->last_tx_time_ = now;
  this->commands_sent_++;
}

void RS485FrameHub::send_next_idle_(uint32_t now) {
  this->build_key_payload_(this->idle_command_, this->tx_payload_buf_);
  this->build_frame_(this->tx_payload_buf_, this->tx_frame_buf_);
  if (this->tx_gate_delay_ > 0) {
    std::swap(this->pending_tx_frame_, this->tx_frame_buf_);
    this->tx_start_at_ = now + this->tx_gate_delay_;
    this->tx_start_pending_ = true;
    this->pending_is_idle_ = true;
    return;
  }
  this->write_frame_(this->tx_frame_buf_);
  this->last_tx_time_ = now;
  // Idle keepalives are not counted in commands_sent_ — that counter tracks only real HA commands.
}

bool RS485FrameHub::queue_raw_frame(const std::vector<uint8_t> &payload) {
  if (this->sniffer_only_) {
    ESP_LOGW(TAG, "Ignoring raw frame because sniffer_only is enabled");
    this->command_drops_++;
    return false;
  }
  this->build_frame_(payload, this->tx_frame_buf_);
  return this->enqueue_frame_();
}

bool RS485FrameHub::frame_type_equals_(const std::vector<uint8_t> &payload,
                                       const StaticVector<uint8_t, MAX_FRAME_TYPE_LEN> &frame_type) const {
  if (frame_type.empty() || payload.size() < frame_type.size())
    return false;
  return std::equal(frame_type.begin(), frame_type.end(), payload.begin());
}

void RS485FrameHub::update_last_frame_type_() {
  size_t len = std::min(this->rx_payload_.size(), size_t(2));
  format_hex_to(this->last_frame_type_, this->rx_payload_.data(), len);
}

#ifdef USE_BUTTON
void RS485FrameButton::press_action() {
  if (this->raw_mode_) {
    this->parent_->queue_raw_frame(this->raw_frame_);
  } else {
    this->parent_->queue_command_value(this->command_value_);
  }
}
#endif  // USE_BUTTON

#ifdef USE_NUMBER
void RS485FrameNumber::control(float value) {
  if (this->lambda_ == nullptr)
    return;
  auto payload = this->lambda_(value);
  if (!payload.has_value())
    return;
  if (this->parent_->queue_raw_frame(payload.value()))
    this->publish_state(value);
}
#endif  // USE_NUMBER

}  // namespace esphome::rs485_frame
