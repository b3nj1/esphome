#pragma once

#include "esphome/components/number/number.h"
#include "../framed_rs485.h"

#include <functional>
#include <vector>

namespace esphome::framed_rs485 {

class FramedRS485Number : public number::Number {
 public:
  using encode_lambda_t = std::function<optional<std::vector<uint8_t>>(float)>;
  explicit FramedRS485Number(FramedRS485Hub *parent) : parent_(parent) {}
  void set_template(encode_lambda_t &&lambda) { this->lambda_ = std::move(lambda); }

 protected:
  void control(float value) override;
  FramedRS485Hub *parent_;
  encode_lambda_t lambda_{nullptr};
};

}  // namespace esphome::framed_rs485
