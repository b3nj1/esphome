#pragma once

#include "esphome/components/button/button.h"
#include "../rs485_frame.h"

#include <vector>

namespace esphome::rs485_frame {

/// Press-only button bound to an RS485FrameHub. Two construction modes:
///
///   1. `command:` form — the hub uses its configured key_format (wireless_12byte,
///      wired_remote, etc.) to encode `command_value` into the on-wire payload.
///      Restricted to profiles that supply a key_format (i.e. not generic).
///
///   2. Raw form — caller supplies the frame_type prefix and the full unframed payload.
///      The hub adds DLE-STX/ETX wrapping, byte-stuffing, and CRC, but the payload
///      bytes are otherwise emitted verbatim. This is the path that lets
///      generic_rs485_frame drive button entities without any built-in key_format.
///
/// The two modes are constructor-distinguished: there is one constructor per mode.
/// raw_mode_ is set from the constructor and is immutable for the lifetime of the
/// button, so press_action picks the right path with a single branch.
class RS485FrameButton : public button::Button {
 public:
  // Command-mode constructor. press_action() calls hub->queue_command_value(command_value).
  RS485FrameButton(RS485FrameHub *parent, uint32_t command_value)
      : parent_(parent), command_value_(command_value), raw_mode_(false) {}

  // Raw-mode constructor. press_action() concatenates frame_type + payload and calls
  // hub->queue_raw_frame(). The arguments are copied into member vectors so they remain
  // valid for the lifetime of the button.
  RS485FrameButton(RS485FrameHub *parent, const std::vector<uint8_t> &frame_type, const std::vector<uint8_t> &payload)
      : parent_(parent), raw_mode_(true) {
    this->raw_frame_.reserve(frame_type.size() + payload.size());
    this->raw_frame_.insert(this->raw_frame_.end(), frame_type.begin(), frame_type.end());
    this->raw_frame_.insert(this->raw_frame_.end(), payload.begin(), payload.end());
  }

 protected:
  void press_action() override;
  RS485FrameHub *parent_;
  uint32_t command_value_{0};
  // Pre-composed frame_type+payload buffer, only populated in raw mode. Held by value so
  // there is no allocation in press_action — only the queue copy inside queue_raw_frame.
  std::vector<uint8_t> raw_frame_;
  bool raw_mode_;
};

}  // namespace esphome::rs485_frame
