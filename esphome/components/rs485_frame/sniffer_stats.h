#pragma once

// defines.h must come before the #ifdef gate so a translation unit that opens this header
// without first including a core component header (e.g., sniffer_stats.cpp itself) still
// sees USE_RS485_FRAME_SNIFFER_STATS before the conditional is evaluated.
#include "esphome/core/defines.h"

#ifdef USE_RS485_FRAME_SNIFFER_STATS

#include "esphome/core/helpers.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace esphome::rs485_frame {

// Ring-buffer length of recent inter-arrival samples per frame_type. Median is computed
// from this window at dump time; min/max are tracked exactly across the whole period.
// Fixed at compile time because 16 is a good window for any sane bus and there's no
// reason a user would want to tune it.
static constexpr size_t SNIFFER_RECENT_DELAYS = 16;

// Upper bound for the user-configurable max_frame_types schema option. Each entry
// allocates max_unique_payloads * payload_capture_bytes of payload buffer plus overhead,
// so the real memory cap is the product of the three configurable knobs — keep this
// purely as a "don't typo a giant number" guard. The Python schema enforces it.
static constexpr size_t SNIFFER_MAX_FRAME_TYPES_UPPER = 64;

// Upper bound for the reference frame_type length. Matches MAX_FRAME_TYPE_LEN in
// rs485_frame.h but duplicated here so this header has no dependency on the hub header.
static constexpr size_t SNIFFER_REFERENCE_MAX_LEN = 8;
static constexpr size_t SNIFFER_HISTOGRAM_BUCKETS = 10;

struct SnifferHistogram {
  uint32_t buckets[SNIFFER_HISTOGRAM_BUCKETS]{};

  void add(size_t value);
};

struct SnifferTimingStats {
  uint64_t sum{0};
  uint32_t count{0};
  uint32_t min{UINT32_MAX};
  uint32_t max{0};

  void add(uint32_t value);
  uint32_t mean() const;
};

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

// One captured payload sample for the unique-payload table. The byte buffer is allocated
// once at entry-creation time (sized to SnifferStats::payload_capture_bytes_); record()
// then writes into it without further allocation. len = actual bytes captured for this
// payload; count = number of times this distinct payload was observed in the current
// dump period (reset to 0 at each dump, set to 1 on first sighting).
struct PayloadCapture {
  std::unique_ptr<uint8_t[]> bytes;
  uint8_t len{0};
  uint16_t count{0};

  // Allocate the bytes buffer. Called once per slot when the owning SnifferEntry is
  // initialized; later record() calls just memcpy into bytes.get().
  void init(size_t capacity);
};

// Per-frame-type accumulator. Frame types are keyed by the first 2 bytes of the payload
// (matching the existing last_frame_type_ diagnostic convention). A frame_type prefix
// longer than 2 bytes still matches on_frame: triggers correctly; the sniffer just buckets
// it together with any other prefix that happens to share its first 2 bytes.
struct SnifferEntry {
  uint8_t frame_type[2]{};
  uint32_t count{0};
  uint32_t last_seen_us{0};
  DelayStats d_ref;
  DelayStats d_same;
  // Heap-allocated array of PayloadCapture slots, sized to SnifferStats::max_unique_payloads_
  // at SnifferEntry::init(). Slot byte buffers are allocated up front too, so update_unique_payload_
  // never allocates from the hot path after the first sighting of each frame type.
  std::unique_ptr<PayloadCapture[]> payloads;
  uint8_t unique_count{0};
  uint16_t unique_overflow{0};

  // Allocate the payloads array and each slot's byte buffer. Called once per entry from
  // SnifferStats::find_or_create_ when a new frame_type is first observed.
  void init(size_t max_unique_payloads, size_t payload_capture_bytes);

  // Clears per-period counters: total frame count, delay stats, last_seen, AND the unique
  // payload list. Per request from the workflow side: a fresh dump period starts with an
  // empty payload list so the user can press a set of buttons, capture the table, then
  // press a different set in the next period without restarting the ESP. The allocated
  // payload byte buffers are kept; only the bookkeeping (unique_count, per-slot len/count,
  // unique_overflow) resets.
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
  // Called once during to_code wiring. max_entries is bounded by SNIFFER_MAX_FRAME_TYPES_UPPER.
  // max_unique_payloads + payload_capture_bytes are also user-configurable and decide how
  // much payload material the sniffer can distinguish per frame type. reference_frame_type
  // may be empty, in which case the d-ref column is always "-" (useful for protocols with
  // no obvious reference frame).
  void init(size_t max_entries, uint32_t interval_ms, uint8_t payload_dump_top, size_t max_unique_payloads,
            size_t payload_capture_bytes, const std::vector<uint8_t> &reference_frame_type, bool strip_high_bit);

  void loop_start(uint32_t loop_start_us, size_t uart_available);
  void loop_end(uint32_t loop_duration_us, uint32_t frames_seen, uint32_t rx_bytes_seen, uint32_t byte_time_us);
  void record_fifo_after_etx(size_t fifo_after, uint32_t correction_us);
  // Called when a deferred TX frame fires. lateness_us = micros() - tx_start_at_us_ at the
  // moment the frame is written; quantifies how much the cooperative loop delayed the send.
  void record_tx_lateness(uint32_t lateness_us);
  // Called after read_uart_() exhausts available() while in_frame_ is still true and the
  // accumulated raw bytes (after the DLE+STX header, so raw_after_stx[0..len-1]) match the
  // configured reference frame type prefix. Counts loops where the reference frame was split
  // across two or more loop() calls, requiring an extra scheduling round-trip before the gate
  // can fire. Only fires when the full reference prefix is visible (len >= prefix length).
  // raw_after_stx must not contain unescaped DLE bytes in the prefix region — assumed safe
  // because frame type values are protocol-defined identifiers that never equal 0x10 (DLE) in
  // any known bus implementation.
  void record_partial_ref_frame(const uint8_t *raw_after_stx, size_t len);

  // Hot path. Called once per validated RX frame with the payload-relative bytes (frame
  // type at payload[0..N-1], data after). Returns immediately if init() was never called.
  // loop_now_us  — micros() sampled at the top of this component loop.
  // frame_now_us — dead-reckoned ETX timestamp from micros() at ETX processing minus the
  //                UART bytes still buffered after the frame.
  void record(const std::vector<uint8_t> &payload, uint32_t loop_now_us, uint32_t frame_now_us);

  // Called from the hub's loop(). Emits the table if interval_ms has elapsed since the
  // last dump, then resets per-period counters.
  void tick(uint32_t now);

 protected:
  // Linear scan; entries_ has at most SNIFFER_MAX_FRAME_TYPES_UPPER (64) entries by
  // construction. Returns nullptr if the table is full and the frame type has not been
  // seen before — the caller bumps dropped_frame_types_ instead. Lazily allocates the
  // per-entry payload buffers on first sighting of a frame type.
  SnifferEntry *find_or_create_(const uint8_t *frame_type);
  bool matches_reference_(const std::vector<uint8_t> &payload) const;
  // Compares the (possibly truncated) payload against the entry's existing unique
  // payloads. Non-static because it needs max_unique_payloads_ and payload_capture_bytes_
  // for bounds and truncation; both are runtime-configurable.
  void update_unique_payload_(SnifferEntry &e, const std::vector<uint8_t> &payload);
  void dump_(uint32_t now);
  void dump_payloads_(size_t top_n, const uint8_t *order) const;
  void dump_processing_stats_(uint32_t now) const;

  FixedVector<SnifferEntry> entries_;
  StaticVector<uint8_t, SNIFFER_REFERENCE_MAX_LEN> reference_frame_type_;
  // Dead-reckoned micros() timestamp of the most recent reference frame.
  uint32_t last_ref_time_{0};
  // Raw loop-start micros() when the most recent reference frame was processed. Used for
  // cross-batch d_ref: subtracting a raw loop timestamp from a dead-reckoned frame_now
  // correctly measures the inter-loop gap minus the current frame's FIFO age, rather than
  // inflating d_ref by the reference frame's over-estimated FIFO age.
  uint32_t last_ref_loop_now_{0};
  bool ref_seen_in_period_{false};
  uint32_t interval_ms_{0};
  uint32_t last_dump_time_{0};
  uint32_t dropped_frame_types_{0};
  SnifferHistogram uart_available_start_;
  SnifferHistogram frames_per_loop_;
  SnifferHistogram fifo_after_etx_;
  // Reference-frame-specific variants of the above two histograms. Each sample is recorded
  // only for the loop/ETX event that produced the reference frame, so these show the UART
  // and FIFO state specifically when the gate frame arrives — not the bus-wide aggregate.
  // Only populated when reference_frame_type_ is non-empty.
  SnifferHistogram uart_available_start_ref_;
  SnifferHistogram fifo_after_etx_ref_;
  // Buffered values to correlate loop-start UART available and fifo_after with the frame
  // type (known only when record() is called, after record_fifo_after_etx).
  size_t last_loop_uart_available_{0};
  size_t last_fifo_after_{0};
  // Lifetime count of read_uart_() exits where the partial raw frame prefix matched the
  // reference frame type — each one is a loop() round-trip added to TX scheduling latency.
  uint32_t partial_ref_frames_{0};
  SnifferTimingStats loop_intercall_us_;
  SnifferTimingStats loop_duration_us_;
  SnifferTimingStats dead_reckon_correction_us_;
  SnifferTimingStats tx_lateness_us_;
  uint32_t last_loop_start_us_{0};
  uint32_t loop_count_{0};
  uint64_t rx_busy_us_{0};
  uint64_t rx_bytes_total_{0};
  uint32_t first_loop_ms_{0};
  uint8_t payload_dump_top_{0};
  bool strip_high_bit_{false};
  // Sized by the YAML schema; carried here so update_unique_payload_, find_or_create_,
  // and dump_payloads_ all reach the same numbers.
  size_t max_unique_payloads_{0};
  size_t payload_capture_bytes_{0};
  // Allocated once at init() to render hex+ASCII previews without per-dump heap traffic.
  // Sized to payload_capture_bytes_ * 3 + 1 and payload_capture_bytes_ + 1 respectively.
  std::unique_ptr<char[]> hex_buf_;
  std::unique_ptr<char[]> ascii_buf_;
  bool initialized_{false};
};

}  // namespace esphome::rs485_frame

#endif  // USE_RS485_FRAME_SNIFFER_STATS
