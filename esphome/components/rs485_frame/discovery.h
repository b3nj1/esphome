#pragma once

// defines.h must come before the #ifdef gate so a translation unit that opens this header
// without first including a core component header (e.g., discovery.cpp itself) still sees
// USE_RS485_FRAME_DISCOVERY before the conditional is evaluated.
#include "esphome/core/defines.h"

#ifdef USE_RS485_FRAME_DISCOVERY

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::rs485_frame {

// Passive framing/CRC discovery for an unknown DLE-framed bus. This is a bring-up tool: with
// discovery: enabled the hub does no framing, validation, or transmission — it captures raw
// bytes, segments them into bursts by idle gap, and periodically logs candidate framing bytes,
// the escape scheme, and any CRC scheme that matches consistently across captured frames.
//
// It tests the DLE-framing hypothesis (opening DLE+STX, closing DLE+ETX, byte-stuffed DLEs).
// A bus that is length-prefixed, fixed-size, or Modbus-style produces no consistent candidate,
// which is itself useful information. Everything here is compiled out unless the YAML enables
// discovery (see USE_RS485_FRAME_DISCOVERY).
class RS485FrameDiscovery {
 public:
  RS485FrameDiscovery(uint32_t report_interval_ms, uint32_t idle_gap_ms, size_t max_burst,
                      uint8_t min_framing_confidence)
      : report_interval_ms_(report_interval_ms),
        idle_gap_ms_(idle_gap_ms),
        max_burst_(max_burst),
        min_framing_confidence_(min_framing_confidence) {}

  // Reserve the burst buffer once; no allocation after setup().
  void setup();
  // Append one received byte to the current burst (stamped with the loop time for gap timing).
  void feed_byte(uint8_t b, uint32_t now);
  // Close the current burst once the bus has been idle for idle_gap_ms_, and emit the periodic
  // report every report_interval_ms_.
  void tick(uint32_t now);

 protected:
  static constexpr size_t PAIR_TABLE_SIZE = 16;
  // CRC hypotheses scored against each candidate-valid frame: {sum8, xor8, sum16, crc16_modbus}
  // x {header_inclusive, payload_only} x {endianness for 2-byte}. 1-byte algos ignore endian.
  static constexpr size_t NUM_CRC_HYPS = 12;
  // Each hypothesis is scored twice: once over the unescaped frame content and once over the
  // raw on-wire content (the "CRC computed over escaped bytes" diagnostic — a scheme this
  // component cannot represent, but worth surfacing during discovery).
  static constexpr size_t NUM_ESCAPE_VIEWS = 2;

  struct BytePair {
    uint8_t a;
    uint8_t b;
    uint32_t count;
  };

  void close_burst_(uint32_t now);
  void analyze_burst_();
  // Process one extracted frame: interior bytes are b[inner_start, frame_end) (between the
  // opening STX and the closing DLE). Updates the escape histogram and CRC counters.
  void analyze_frame_(const std::vector<uint8_t> &b, size_t inner_start, size_t frame_end, uint8_t dle, uint8_t stx,
                      uint8_t etx);
  void report_();
  static void bump_pair_(BytePair *table, size_t &len, uint8_t a, uint8_t b);
  static const BytePair *top_pair_(const BytePair *table, size_t len);
  void reset_scoring_();
  void score_crc_(const std::vector<uint8_t> &content, bool unescaped_view);

  uint32_t report_interval_ms_;
  uint32_t idle_gap_ms_;
  size_t max_burst_;
  // The top start/end delimiter pair must account for at least this percentage of the bursts
  // that voted (length >= 4) before the framing is reported as confident and a ready-to-paste
  // config is suggested. 0 disables the gate. CRC scoring is unaffected: it always runs against
  // the current top candidate and self-corrects via reset_scoring_ when that candidate shifts.
  uint8_t min_framing_confidence_;

  std::vector<uint8_t> burst_;
  std::vector<uint8_t> unescaped_;  // scratch for unescaping a candidate frame's interior
  std::vector<uint8_t> raw_inner_;  // scratch for the raw (escaped) interior
  uint32_t last_byte_time_{0};
  bool burst_open_{false};
  bool burst_truncated_{false};

  uint32_t last_report_time_{0};
  bool report_primed_{false};

  uint32_t total_bursts_{0};
  uint32_t framing_bursts_{0};  // bursts long enough (>= 4 bytes) to vote for a delimiter pair
  uint32_t total_frames_{0};    // individual frames extracted from all bursts

  BytePair start_pairs_[PAIR_TABLE_SIZE];
  BytePair end_pairs_[PAIR_TABLE_SIZE];
  size_t start_pairs_len_{0};
  size_t end_pairs_len_{0};

  // Histogram of the byte following an interior DLE (the current DLE candidate), used to
  // classify the escape scheme: a peak at DLE means doubling, a peak at some other byte means
  // that byte is the escape marker.
  uint32_t dle_succ_hist_[256] = {};
  uint32_t dle_succ_total_{0};

  // Framing/escape candidate the CRC counters were scored against. When the candidate shifts
  // (early on, before it stabilizes) the counters and successor histogram are reset so they
  // reflect a single consistent hypothesis.
  bool scored_valid_{false};
  uint8_t scored_dle_{0};
  uint8_t scored_stx_{0};
  uint8_t scored_etx_{0};
  uint8_t scored_marker_{0};
  bool scored_has_marker_{false};

  uint32_t crc_samples_[NUM_ESCAPE_VIEWS][NUM_CRC_HYPS] = {};
  uint32_t crc_matches_[NUM_ESCAPE_VIEWS][NUM_CRC_HYPS] = {};
};

}  // namespace esphome::rs485_frame

#endif  // USE_RS485_FRAME_DISCOVERY
