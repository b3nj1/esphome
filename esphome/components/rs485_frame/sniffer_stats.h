#pragma once

// defines.h must come before the #ifdef gate so a translation unit that opens this header
// without first including a core component header (e.g., sniffer_stats.cpp itself) still
// sees USE_RS485_FRAME_SNIFFER_STATS before the conditional is evaluated.
#include "esphome/core/defines.h"

#ifdef USE_RS485_FRAME_SNIFFER_STATS

#include "esphome/core/helpers.h"

#include <cstdint>
#include <vector>

namespace esphome::rs485_frame {

// Bytes of payload captured per unique payload sample. Long Hayward display frames are 30+
// bytes; capturing the full payload would balloon per-entry size. dump_frames: true already
// logs every full payload — sniffer_stats only needs enough to distinguish "is this payload
// the same as one we've seen before" and to give a hex/ASCII preview.
static constexpr size_t SNIFFER_PAYLOAD_CAPTURE_BYTES = 16;

// Number of distinct payloads remembered per frame_type. Additional unique payloads beyond
// this cap are counted in unique_overflow without being stored.
static constexpr size_t SNIFFER_MAX_UNIQUE_PAYLOADS = 8;

// Ring-buffer length of recent inter-arrival samples per frame_type. Median is computed
// from this window at dump time; min/max are tracked exactly across the whole period.
static constexpr size_t SNIFFER_RECENT_DELAYS = 16;

// Upper bound for the user-configurable max_frame_types schema option. Each entry costs
// ~140 bytes; 64 caps RAM growth at ~9 KB plus the FixedVector header.
static constexpr size_t SNIFFER_MAX_FRAME_TYPES_UPPER = 64;

// Upper bound for the reference frame_type length. Matches MAX_FRAME_TYPE_LEN in
// rs485_frame.h but duplicated here so this header has no dependency on the hub header.
static constexpr size_t SNIFFER_REFERENCE_MAX_LEN = 8;

// Per-frame-type sliding-window stats for the inter-arrival delay between bus frames.
// Tracks exact min/max across the dump period plus a small ring buffer of recent samples
// for median estimation. The ring buffer trades exact median for a fixed memory footprint;
// a 16-sample window is enough to reflect the cadence of the most recent few seconds at
// typical bus frame rates (10–100 Hz).
struct DelayStats {
  uint32_t min{UINT32_MAX};
  uint32_t max{0};
  // Samples saturate at uint16_t max (~65 s) to halve memory; min/max remain exact via
  // uint32_t, so the saturation only affects the median estimate for very rare frames.
  uint16_t recent[SNIFFER_RECENT_DELAYS]{};
  uint8_t recent_idx{0};
  uint8_t recent_count{0};

  void reset();
  void add(uint32_t delay_ms);
  // Returns 0 when no samples are present; caller is expected to check recent_count first
  // if it needs to distinguish "no samples" from "median is exactly 0 ms".
  uint32_t median() const;
};

// One short captured payload prefix for the unique-payload table. Bytes are copied from
// the RX payload at record() time; len is min(payload.size(), SNIFFER_PAYLOAD_CAPTURE_BYTES).
struct PayloadCapture {
  uint8_t bytes[SNIFFER_PAYLOAD_CAPTURE_BYTES]{};
  uint8_t len{0};
  uint8_t count{0};
};

// Per-frame-type accumulator. Frame types are keyed by the first 2 bytes of the payload
// (matching the existing last_frame_type_ diagnostic convention). A frame_type prefix
// longer than 2 bytes still matches on_frame: triggers correctly; the sniffer just buckets
// it together with any other prefix that happens to share its first 2 bytes.
struct SnifferEntry {
  uint8_t frame_type[2]{};
  uint32_t count{0};
  uint32_t last_seen_ms{0};
  DelayStats d_ref;
  DelayStats d_same;
  PayloadCapture payloads[SNIFFER_MAX_UNIQUE_PAYLOADS]{};
  uint8_t unique_count{0};
  uint16_t unique_overflow{0};

  // Clears per-period counters (count, delay stats, last_seen) but preserves the
  // unique-payload list — payload uniqueness is a long-term observation that compounds
  // across dump periods. unique_overflow is also preserved.
  void reset_period_stats();
};

// Optional diagnostic that records per-frame-type cadence and unique-payload histograms
// while the hub is in sniffer mode. Compiled out unless USE_RS485_FRAME_SNIFFER_STATS is
// defined; the hub holds a unique_ptr that is nullptr in production, so the hot path is
// a single null-check.
//
// One instance is owned by the hub and fed by a single call from process_raw_frame_().
// Output is periodic: tick() is called from loop() and emits the table when interval_ms
// has elapsed, then resets per-period counters.
class SnifferStats {
 public:
  // Called once during to_code wiring. max_entries is bounded by
  // SNIFFER_MAX_FRAME_TYPES_UPPER. reference_frame_type may be empty, in which case the
  // d-ref column is always "-" (useful for protocols with no obvious reference frame).
  void init(size_t max_entries, uint32_t interval_ms, uint8_t payload_dump_top,
            const std::vector<uint8_t> &reference_frame_type);

  // Hot path. Called once per validated RX frame with the payload-relative bytes (frame
  // type at payload[0..N-1], data after). Returns immediately if init() was never called.
  void record(const std::vector<uint8_t> &payload, uint32_t now);

  // Called from the hub's loop(). Emits the table if interval_ms has elapsed since the
  // last dump, then resets per-period counters.
  void tick(uint32_t now);

 protected:
  // Linear scan; entries_ has at most SNIFFER_MAX_FRAME_TYPES_UPPER (64) entries by
  // construction. Returns nullptr if the table is full and the frame type has not been
  // seen before — the caller bumps dropped_frame_types_ instead.
  SnifferEntry *find_or_create_(const uint8_t *frame_type);
  bool matches_reference_(const std::vector<uint8_t> &payload) const;
  // Compares the (possibly truncated) payload against the entry's existing unique
  // payloads, appending it if absent and there is room, or bumping unique_overflow.
  static void update_unique_payload_(SnifferEntry &e, const std::vector<uint8_t> &payload);
  void dump_(uint32_t now);
  void dump_payloads_(size_t top_n, const uint8_t *order) const;

  FixedVector<SnifferEntry> entries_;
  StaticVector<uint8_t, SNIFFER_REFERENCE_MAX_LEN> reference_frame_type_;
  uint32_t last_ref_time_{0};
  bool ref_seen_in_period_{false};
  uint32_t interval_ms_{0};
  uint32_t last_dump_time_{0};
  uint32_t dropped_frame_types_{0};
  uint8_t payload_dump_top_{0};
  bool initialized_{false};
};

}  // namespace esphome::rs485_frame

#endif  // USE_RS485_FRAME_SNIFFER_STATS
