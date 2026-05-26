#pragma once

#include "esphome/components/button/button.h"
#include "../framed_rs485.h"

namespace esphome::framed_rs485 {

class FramedRS485Button : public button::Button {
 public:
  FramedRS485Button(FramedRS485Hub *parent, uint32_t command_value) : parent_(parent), command_value_(command_value) {}

 protected:
  void press_action() override;
  FramedRS485Hub *parent_;
  uint32_t command_value_;
};

}  // namespace esphome::framed_rs485
