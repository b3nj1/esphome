#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "../framed_rs485.h"

#include <functional>
#include <string>
#include <vector>

namespace esphome::framed_rs485 {

/// Binary sensor for a DLE-framed RS-485 bus.
///
/// Two operating modes, selected by which optional keys are provided in YAML:
///
///   1. **Custom lambda** (`lambda:`): stateless decode — return `optional<bool>`,
///      or `{}` to publish nothing on this frame.
///
///   2. **Latching text match** (`match_on:` / `match_off:` / `timeout:`):
///      the sensor holds its last proven state until the opposite proof arrives
///      or `timeout` expires. `text_lambda:` controls how text is extracted from
///      the raw payload; the default strips non-printable bytes and yields ASCII.
///      Use `text_lambda:` when the device encodes display text with protocol-
///      specific flags (e.g. Hayward AquaLogic blink bits).
class FramedRS485BinarySensor : public binary_sensor::BinarySensor, public FramedRS485Listener {
 public:
  using decode_lambda_t = std::function<optional<bool>(const std::vector<uint8_t> &)>;
  using text_lambda_t = std::function<optional<std::string>(const std::vector<uint8_t> &)>;

  // Tokens are normalized by add_match_on/add_match_off at setup time to avoid per-frame allocation.
  void add_match_on(const std::string &s);
  void add_match_off(const std::string &s);
  /// Revert to false if no proof appears within this many milliseconds (0 = never expire).
  void set_timeout_ms(uint32_t ms) { this->timeout_ms_ = ms; }
  void set_template(decode_lambda_t lambda) { this->lambda_ = lambda; }
  /// Optional: custom text extractor for latching-match mode.
  /// Signature: optional<std::string>(const std::vector<uint8_t> &payload).
  /// Return {} to skip this frame entirely. When not set, the default extractor
  /// yields printable ASCII bytes from the full payload.
  void set_text_lambda(text_lambda_t lambda) { this->text_lambda_ = lambda; }

  void handle_frame(FramedRS485Hub *hub, const std::vector<uint8_t> &payload, uint32_t now) override;

 protected:
  decode_lambda_t lambda_{nullptr};
  text_lambda_t text_lambda_{nullptr};
  std::vector<std::string> match_on_;   ///< Pre-normalized proof-positive tokens.
  std::vector<std::string> match_off_;  ///< Pre-normalized proof-negative tokens.
  uint32_t timeout_ms_{0};
  uint32_t last_proof_ms_{0};  ///< 0 = no proof seen yet; do not expire before first proof.
  bool timed_out_{false};
};

}  // namespace esphome::framed_rs485
