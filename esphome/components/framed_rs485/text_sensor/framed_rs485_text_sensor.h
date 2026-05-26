#pragma once

#include "esphome/components/text_sensor/text_sensor.h"
#include "../framed_rs485.h"

#include <functional>
#include <string>
#include <vector>

namespace esphome::framed_rs485 {

class FramedRS485TextSensor : public text_sensor::TextSensor, public FramedRS485Listener {
 public:
  using decode_lambda_t = std::function<optional<std::string>(const std::vector<uint8_t> &)>;
  /// When set, the lambda overrides the built-in `last_frame_type` decode.
  void set_template(decode_lambda_t lambda) { this->lambda_ = lambda; }
  void handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) override;

 protected:
  decode_lambda_t lambda_{nullptr};
};

}  // namespace esphome::framed_rs485
