#pragma once

#include "esphome/components/button/button.h"
#include "../rs485_frame.h"

namespace esphome::rs485_frame {

class RS485FrameButton : public button::Button {
 public:
  RS485FrameButton(RS485FrameHub *parent, uint32_t command_value) : parent_(parent), command_value_(command_value) {}

 protected:
  void press_action() override;
  RS485FrameHub *parent_;
  uint32_t command_value_;
};

}  // namespace esphome::rs485_frame
