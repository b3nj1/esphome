#pragma once

#include "rs485_frame.h"
#include "esphome/core/automation.h"

#include <vector>

namespace esphome::rs485_frame {

/// Action: queue a raw frame for transmission. The hub adds DLE-STX/ETX wrapping,
/// applies byte-stuffing, and computes the CRC according to the configured
/// crc.type and crc.tx_variant. The caller supplies the 2-byte (or longer)
/// frame_type and the unframed, unstuffed payload bytes; the action concatenates
/// them before handing to RS485FrameHub::queue_raw_frame.
///
/// Both frame_type and payload are templatable so they can be computed from
/// trigger arguments or other entity states at action time.
template<typename... Ts> class SendFrameAction : public Action<Ts...>, public Parented<RS485FrameHub> {
 public:
  TEMPLATABLE_VALUE(std::vector<uint8_t>, frame_type)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, payload)

  void play(Ts... x) override {
    auto frame_type = this->frame_type_.value(x...);
    auto payload = this->payload_.value(x...);
    std::vector<uint8_t> full;
    full.reserve(frame_type.size() + payload.size());
    full.insert(full.end(), frame_type.begin(), frame_type.end());
    full.insert(full.end(), payload.begin(), payload.end());
    this->parent_->queue_raw_frame(full);
  }
};

}  // namespace esphome::rs485_frame
