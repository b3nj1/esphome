#pragma once

#include "esphome/components/sensor/sensor.h"
#include "../framed_rs485.h"

#include <functional>
#include <string>
#include <vector>

namespace esphome::framed_rs485 {

class FramedRS485Sensor : public sensor::Sensor, public FramedRS485Listener {
 public:
  using decode_lambda_t = std::function<optional<float>(const std::vector<uint8_t> &)>;
  void set_decode(SensorDecode decode) { this->decode_ = decode; }
  void set_offset(uint32_t offset) { this->offset_ = offset; }
  void set_template(decode_lambda_t lambda) { this->lambda_ = lambda; }
  void handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) override;

 protected:
  optional<float> decode_builtin_(FramedRS485Hub *hub, const std::vector<uint8_t> &payload) const;
  SensorDecode decode_{SENSOR_DECODE_UINT8};
  decode_lambda_t lambda_{nullptr};
  uint32_t offset_{0};
};

}  // namespace esphome::framed_rs485
