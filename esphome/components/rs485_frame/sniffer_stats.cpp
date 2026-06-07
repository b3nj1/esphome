#include "sniffer_stats.h"

#ifdef USE_RS485_FRAME_SNIFFER_STATS

#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::rs485_frame {

static const char *const TAG = "rs485_frame.stats";

void SnifferHistogram::add(size_t value) {
  if (value == 0) {
    this->buckets[0]++;
    return;
  }
  uint8_t bucket = 1;
  size_t limit = 1;
  while (bucket + 1 < SNIFFER_HISTOGRAM_BUCKETS && value > limit) {
    limit <<= 1;
    bucket++;
  }
  this->buckets[bucket]++;
}

void SnifferTimingStats::add(uint32_t value) {
  if (this->count < UINT32_MAX)
    this->count++;
  this->sum += value;
  if (value < this->min)
    this->min = value;
  if (value > this->max)
    this->max = value;
}

uint32_t SnifferTimingStats::mean() const {
  return this->count == 0 ? 0 : static_cast<uint32_t>(this->sum / this->count);
}

void DelayStats::reset() {
  this->min = UINT32_MAX;
  this->max = 0;
  this->recent_idx = 0;
  this->recent_count = 0;
}

void DelayStats::add(uint32_t delay_ms) {
  if (delay_ms < this->min)
    this->min = delay_ms;
  if (delay_ms > this->max)
    this->max = delay_ms;
  // Saturate the ring-buffer sample at uint16_t max so very rare frames don't break the
  // median; the exact max is still tracked above.
  this->recent[this->recent_idx] = delay_ms > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(delay_ms);
  this->recent_idx = (this->recent_idx + 1) % SNIFFER_RECENT_DELAYS;
  if (this->recent_count < SNIFFER_RECENT_DELAYS)
    this->recent_count++;
}

uint32_t DelayStats::median() const {
  if (this->recent_count == 0)
    return 0;
  uint16_t copy[SNIFFER_RECENT_DELAYS];
  std::memcpy(copy, this->recent, sizeof(uint16_t) * this->recent_count);
  // Insertion sort: N ≤ 16, cheap and avoids dragging in std::sort template instantiation.
  for (size_t i = 1; i < this->recent_count; i++) {
    uint16_t v = copy[i];
    size_t j = i;
    while (j > 0 && copy[j - 1] > v) {
      copy[j] = copy[j - 1];
      j--;
    }
    copy[j] = v;
  }
  return copy[this->recent_count / 2];
}

void PayloadCapture::init(size_t capacity) { this->bytes = std::make_unique<uint8_t[]>(capacity); }

void SnifferEntry::init(size_t max_unique_payloads, size_t payload_capture_bytes) {
  this->payloads = std::make_unique<PayloadCapture[]>(max_unique_payloads);
  for (size_t i = 0; i < max_unique_payloads; i++) {
    this->payloads[i].init(payload_capture_bytes);
  }
}

void SnifferEntry::reset_period_stats() {
  this->count = 0;
  this->last_seen_us = 0;
  this->d_ref.reset();
  this->d_same.reset();
  // Wipe the unique-payload bookkeeping so the next period starts fresh. The payload
  // byte buffers stay allocated; per-slot len/count past unique_count are stale but
  // unread until a fresh payload overwrites them in update_unique_payload_.
  this->unique_count = 0;
  this->unique_overflow = 0;
}

void SnifferStats::init(size_t max_entries, uint32_t interval_ms, uint8_t payload_dump_top, size_t max_unique_payloads,
                        size_t payload_capture_bytes, const std::vector<uint8_t> &reference_frame_type,
                        bool strip_high_bit) {
  size_t capped = max_entries > SNIFFER_MAX_FRAME_TYPES_UPPER ? SNIFFER_MAX_FRAME_TYPES_UPPER : max_entries;
  this->entries_.init(capped);
  this->reference_frame_type_.assign(reference_frame_type.begin(), reference_frame_type.end());
  this->interval_ms_ = interval_ms;
  this->payload_dump_top_ = payload_dump_top;
  this->strip_high_bit_ = strip_high_bit;
  this->max_unique_payloads_ = max_unique_payloads;
  this->payload_capture_bytes_ = payload_capture_bytes;
  // Pre-allocate the hex/ASCII scratch buffers used by dump_payloads_ so dumps don't
  // allocate per-call. Sized to the worst-case captured payload.
  this->hex_buf_ = std::make_unique<char[]>(payload_capture_bytes * 3 + 1);
  this->ascii_buf_ = std::make_unique<char[]>(payload_capture_bytes + 1);
  this->initialized_ = true;
}

bool SnifferStats::matches_reference_(const std::vector<uint8_t> &payload) const {
  if (this->reference_frame_type_.empty() || payload.size() < this->reference_frame_type_.size())
    return false;
  return std::equal(this->reference_frame_type_.begin(), this->reference_frame_type_.end(), payload.begin());
}

SnifferEntry *SnifferStats::find_or_create_(const uint8_t *frame_type) {
  for (auto &entry : this->entries_) {
    if (entry.frame_type[0] == frame_type[0] && entry.frame_type[1] == frame_type[1])
      return &entry;
  }
  if (this->entries_.size() < this->entries_.capacity()) {
    SnifferEntry &e = this->entries_.emplace_back();
    e.frame_type[0] = frame_type[0];
    e.frame_type[1] = frame_type[1];
    // Lazy allocation: per-entry payload buffers are sized using the current SnifferStats
    // configuration. This only runs once per distinct frame_type, then never again — the
    // hot record() path after this allocation is pure memcpy/compare.
    e.init(this->max_unique_payloads_, this->payload_capture_bytes_);
    return &e;
  }
  return nullptr;
}

void SnifferStats::update_unique_payload_(SnifferEntry &e, const std::vector<uint8_t> &payload) {
  size_t len = payload.size() < this->payload_capture_bytes_ ? payload.size() : this->payload_capture_bytes_;
  for (uint8_t i = 0; i < e.unique_count; i++) {
    PayloadCapture &slot = e.payloads[i];
    if (slot.len == len && std::memcmp(slot.bytes.get(), payload.data(), len) == 0) {
      // Saturate the per-payload count at uint16_t max — a very chatty payload over a
      // long dump interval can otherwise wrap. The exact count past 65535 doesn't matter
      // for the discovery use case; "≥65535" is information enough.
      if (slot.count < UINT16_MAX)
        slot.count++;
      return;
    }
  }
  if (e.unique_count < this->max_unique_payloads_) {
    PayloadCapture &slot = e.payloads[e.unique_count];
    std::memcpy(slot.bytes.get(), payload.data(), len);
    slot.len = static_cast<uint8_t>(len);
    slot.count = 1;  // first sighting in this period
    e.unique_count++;
  } else if (e.unique_overflow < UINT16_MAX) {
    e.unique_overflow++;
  }
}

void SnifferStats::loop_start(uint32_t loop_start_us, size_t uart_available) {
  if (!this->initialized_)
    return;
  this->uart_available_start_.add(uart_available);
  // Buffer for the reference-frame histogram (consumed in record() if a ref frame is seen).
  this->last_loop_uart_available_ = uart_available;
  // If the previous loop exited mid-reference-frame, measure how long it took to get back.
  // The gap is in microseconds; convert to ms (rounded) for the histogram so the log-scale
  // buckets (0,1,2,4,8,16,...) give useful resolution over the 1–64 ms range of interest.
  if (this->partial_ref_pending_) {
    uint32_t gap_us = loop_start_us - this->last_partial_ref_loop_start_us_;
    this->partial_ref_loop_gap_ms_.add((gap_us + 500) / 1000);
    this->partial_ref_pending_ = false;
  }
  if (this->loop_count_ > 0)
    this->loop_intercall_us_.add(loop_start_us - this->last_loop_start_us_);
  this->last_loop_start_us_ = loop_start_us;
  if (this->loop_count_ < UINT32_MAX)
    this->loop_count_++;
}

void SnifferStats::loop_end(uint32_t loop_duration_us, uint32_t frames_seen, uint32_t rx_bytes_seen,
                            uint32_t byte_time_us) {
  if (!this->initialized_)
    return;
  this->loop_duration_us_.add(loop_duration_us);
  this->frames_per_loop_.add(frames_seen);
  this->rx_bytes_total_ += rx_bytes_seen;
  this->rx_busy_us_ += static_cast<uint64_t>(rx_bytes_seen) * byte_time_us;
}

void SnifferStats::record_fifo_after_etx(size_t fifo_after, uint32_t correction_us) {
  if (!this->initialized_)
    return;
  this->fifo_after_etx_.add(fifo_after);
  this->dead_reckon_correction_us_.add(correction_us);
  // Buffer for the reference-frame variant (consumed in record() if this is the ref frame).
  this->last_fifo_after_ = fifo_after;
}

void SnifferStats::record_tx_lateness(uint32_t lateness_us) {
  if (!this->initialized_)
    return;
  this->tx_lateness_us_.add(lateness_us);
}

void SnifferStats::record_partial_ref_frame(const uint8_t *raw_after_stx, size_t len) {
  if (!this->initialized_ || this->reference_frame_type_.empty())
    return;
  // Only count when we have enough bytes to confirm the full reference prefix is present.
  // Partial matches (e.g. only 1 of 2 prefix bytes arrived) are not counted — there is no
  // way to distinguish them from a different frame type that happens to share the first byte.
  if (len < this->reference_frame_type_.size())
    return;
  if (std::equal(this->reference_frame_type_.begin(), this->reference_frame_type_.end(), raw_after_stx)) {
    if (this->partial_ref_frames_ < UINT32_MAX)
      this->partial_ref_frames_++;
    // Arm the gap timer so the next loop_start() measures how long this split lasted.
    // last_loop_start_us_ is the loop-start timestamp for the loop currently in progress —
    // the gap will be (next_loop_start - last_loop_start_us_), matching the same anchor
    // used by loop_intercall_us_ so the two distributions are directly comparable.
    this->partial_ref_pending_ = true;
    this->last_partial_ref_loop_start_us_ = this->last_loop_start_us_;
  }
}

void SnifferStats::record(const std::vector<uint8_t> &payload, uint32_t loop_now_us, uint32_t frame_now_us) {
  if (!this->initialized_ || payload.size() < 2)
    return;

  // Update the reference-frame timestamps *before* computing since-ref for this frame so
  // that the reference frame itself shows up with no since-ref sample (its own d-ref row
  // is always "-"), and the next non-reference frame measures from this one.
  bool is_ref = this->matches_reference_(payload);
  if (is_ref) {
    this->last_ref_time_ = frame_now_us;
    this->last_ref_loop_now_ = loop_now_us;
    this->ref_seen_in_period_ = true;
    // Record UART-start and fifo-after specifically for the reference frame so these
    // histograms show gate-frame conditions rather than the bus-wide average.
    this->uart_available_start_ref_.add(this->last_loop_uart_available_);
    this->fifo_after_etx_ref_.add(this->last_fifo_after_);
  }

  SnifferEntry *e = this->find_or_create_(payload.data());
  if (e == nullptr) {
    if (this->dropped_frame_types_ < UINT32_MAX)
      this->dropped_frame_types_++;
    return;
  }

  // since-same-type: only after we've seen this frame type at least once in this period.
  if (e->count > 0) {
    e->d_same.add((frame_now_us - e->last_seen_us + 500) / 1000);
  }

  // since-ref: skip when this frame is the reference itself (the d-ref column would be
  // always zero and is uninformative for the reference row).
  if (this->ref_seen_in_period_ && !is_ref) {
    // Two cases depending on whether this frame and the reference frame were processed in
    // the same loop() call (same-batch) or different calls (cross-batch):
    //
    // Same-batch (loop_now == last_ref_loop_now_): both dead-reckoned timestamps are
    // anchored to the same loop_now, so their FIFO-age errors cancel in the subtraction.
    // Use frame_now - last_ref_time_ for an accurate intra-batch delta.
    //
    // Cross-batch (loop_now != last_ref_loop_now_): using last_ref_time_ (dead-reckoned,
    // pushed backward by the FIFO bytes that followed the reference frame) inflates d_ref
    // by that over-estimated age. Instead subtract last_ref_loop_now_ (the raw loop start
    // when the reference was seen), so d_ref = frame_now - last_ref_loop_now_ ≈ inter-loop
    // gap minus the current frame's own dead-reckoning correction — an accurate measure of
    // how long after the reference loop this frame arrived.
    uint32_t ref_base = (loop_now_us == this->last_ref_loop_now_) ? this->last_ref_time_ : this->last_ref_loop_now_;
    e->d_ref.add((frame_now_us - ref_base + 500) / 1000);
  }

  this->update_unique_payload_(*e, payload);

  e->count++;
  e->last_seen_us = frame_now_us;
}

void SnifferStats::tick(uint32_t now) {
  if (!this->initialized_ || this->interval_ms_ == 0)
    return;
  if (this->last_dump_time_ == 0) {
    // First tick after init — establish baseline so the first dump fires ~interval_ms
    // after sniffer start rather than immediately.
    this->last_dump_time_ = now;
    this->first_loop_ms_ = now;
    return;
  }
  if (now - this->last_dump_time_ < this->interval_ms_)
    return;
  this->dump_(now);
  this->last_dump_time_ = now;
}

void SnifferStats::dump_(uint32_t now) {
  // Indirection-sort by count descending. order[] holds entry indices; entry data is not
  // moved. Insertion sort keeps the cost bounded for our small N (≤ 64).
  uint8_t order[SNIFFER_MAX_FRAME_TYPES_UPPER];
  size_t n = this->entries_.size();
  for (size_t i = 0; i < n; i++)
    order[i] = static_cast<uint8_t>(i);
  for (size_t i = 1; i < n; i++) {
    uint8_t v = order[i];
    uint32_t key = this->entries_[v].count;
    size_t j = i;
    while (j > 0 && this->entries_[order[j - 1]].count < key) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = v;
  }

  ESP_LOGI(TAG, "RS485 sniffer stats over %" PRIu32 " ms (sorted by count):", now - this->last_dump_time_);
  // Header and data row widths are matched by hand: type(4) + sep(1) + cnt(5) + sep(3)
  // + d_ref triplet(17 = 3*5 + 2 separators) + sep(3) + d_same triplet(17) + sep(3) + payloads.
  // The label "(min/med/max)" is wider than the data triplet; the labels bleed past the
  // column right edge, which is harmless because nothing follows them on the header line.
  ESP_LOGI(TAG, "  type  cnt    d-ref(min/med/max)    d-same(min/med/max)   payloads");

  char d_ref_buf[24];
  char d_same_buf[24];
  char unique_buf[32];

  for (size_t i = 0; i < n; i++) {
    SnifferEntry &e = this->entries_[order[i]];

    if (e.d_ref.recent_count == 0) {
      std::snprintf(d_ref_buf, sizeof(d_ref_buf), "%5s %5s %5s", "-", "-", "-");
    } else {
      std::snprintf(d_ref_buf, sizeof(d_ref_buf), "%5" PRIu32 " %5" PRIu32 " %5" PRIu32, e.d_ref.min, e.d_ref.median(),
                    e.d_ref.max);
    }
    if (e.d_same.recent_count == 0) {
      std::snprintf(d_same_buf, sizeof(d_same_buf), "%5s %5s %5s", "-", "-", "-");
    } else {
      std::snprintf(d_same_buf, sizeof(d_same_buf), "%5" PRIu32 " %5" PRIu32 " %5" PRIu32, e.d_same.min,
                    e.d_same.median(), e.d_same.max);
    }
    if (e.unique_overflow > 0) {
      std::snprintf(unique_buf, sizeof(unique_buf), "%u unique +%u", e.unique_count, e.unique_overflow);
    } else {
      std::snprintf(unique_buf, sizeof(unique_buf), "%u unique", e.unique_count);
    }

    ESP_LOGI(TAG, "  %02X%02X %5" PRIu32 "    %s    %s   %s", e.frame_type[0], e.frame_type[1], e.count, d_ref_buf,
             d_same_buf, unique_buf);
  }

  if (this->payload_dump_top_ > 0 && n > 0) {
    size_t dump_n = this->payload_dump_top_ < n ? this->payload_dump_top_ : n;
    this->dump_payloads_(dump_n, order);
  }

  if (this->dropped_frame_types_ > 0) {
    ESP_LOGW(TAG, "  dropped %" PRIu32 " events for frame types past table capacity (%zu)", this->dropped_frame_types_,
             this->entries_.capacity());
  }
  this->dump_processing_stats_(now);

  // Per-period reset: total frame count, delay stats, AND unique payload list. The
  // payload list is intentionally cleared every period so the user can use successive
  // dumps as independent capture windows (press buttons A, dump, press buttons B, dump)
  // without having to reboot the ESP between sessions.
  for (size_t i = 0; i < n; i++)
    this->entries_[i].reset_period_stats();
  this->dropped_frame_types_ = 0;
  this->ref_seen_in_period_ = false;
}

void SnifferStats::dump_processing_stats_(uint32_t now) const {
  uint32_t elapsed_ms = now - this->first_loop_ms_;
  uint32_t rx_duty_per_mille =
      elapsed_ms == 0
          ? 0
          : static_cast<uint32_t>((this->rx_busy_us_ * 1000ULL) / (static_cast<uint64_t>(elapsed_ms) * 1000ULL));
  ESP_LOGI(TAG,
           "  processing lifetime: loops=%" PRIu32 " mean_loop_gap=%" PRIu32 "us mean_loop=%" PRIu32
           "us rx_bytes=%" PRIu64 " rx_duty=%" PRIu32 ".%01" PRIu32 "%%",
           this->loop_count_, this->loop_intercall_us_.mean(), this->loop_duration_us_.mean(), this->rx_bytes_total_,
           rx_duty_per_mille / 10, rx_duty_per_mille % 10);
  ESP_LOGI(TAG, "  loop gap us min/mean/max: %" PRIu32 " / %" PRIu32 " / %" PRIu32,
           this->loop_intercall_us_.count == 0 ? 0 : this->loop_intercall_us_.min, this->loop_intercall_us_.mean(),
           this->loop_intercall_us_.max);
  ESP_LOGI(TAG, "  loop duration us min/mean/max: %" PRIu32 " / %" PRIu32 " / %" PRIu32,
           this->loop_duration_us_.count == 0 ? 0 : this->loop_duration_us_.min, this->loop_duration_us_.mean(),
           this->loop_duration_us_.max);
  ESP_LOGI(TAG, "  dead-reckon correction us min/mean/max: %" PRIu32 " / %" PRIu32 " / %" PRIu32,
           this->dead_reckon_correction_us_.count == 0 ? 0 : this->dead_reckon_correction_us_.min,
           this->dead_reckon_correction_us_.mean(), this->dead_reckon_correction_us_.max);
  if (this->tx_lateness_us_.count > 0) {
    ESP_LOGI(TAG, "  tx_lateness_us min/mean/max: %" PRIu32 " / %" PRIu32 " / %" PRIu32 " (n=%" PRIu32 ")",
             this->tx_lateness_us_.min, this->tx_lateness_us_.mean(), this->tx_lateness_us_.max,
             this->tx_lateness_us_.count);
  }
  ESP_LOGI(TAG,
           "  hist uart_start bytes [0,1,2,4,8,16,32,64,128,>128]: %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32
           " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32,
           this->uart_available_start_.buckets[0], this->uart_available_start_.buckets[1],
           this->uart_available_start_.buckets[2], this->uart_available_start_.buckets[3],
           this->uart_available_start_.buckets[4], this->uart_available_start_.buckets[5],
           this->uart_available_start_.buckets[6], this->uart_available_start_.buckets[7],
           this->uart_available_start_.buckets[8], this->uart_available_start_.buckets[9]);
  ESP_LOGI(TAG,
           "  hist frames_per_loop [0,1,2,4,8,16,32,64,128,>128]: %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32
           " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32,
           this->frames_per_loop_.buckets[0], this->frames_per_loop_.buckets[1], this->frames_per_loop_.buckets[2],
           this->frames_per_loop_.buckets[3], this->frames_per_loop_.buckets[4], this->frames_per_loop_.buckets[5],
           this->frames_per_loop_.buckets[6], this->frames_per_loop_.buckets[7], this->frames_per_loop_.buckets[8],
           this->frames_per_loop_.buckets[9]);
  ESP_LOGI(TAG,
           "  hist fifo_after_etx bytes [0,1,2,4,8,16,32,64,128,>128]: %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32
           " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32,
           this->fifo_after_etx_.buckets[0], this->fifo_after_etx_.buckets[1], this->fifo_after_etx_.buckets[2],
           this->fifo_after_etx_.buckets[3], this->fifo_after_etx_.buckets[4], this->fifo_after_etx_.buckets[5],
           this->fifo_after_etx_.buckets[6], this->fifo_after_etx_.buckets[7], this->fifo_after_etx_.buckets[8],
           this->fifo_after_etx_.buckets[9]);
  // Reference-frame-specific histograms: only printed when a reference frame type is
  // configured. These isolate gate-frame conditions from the bus-wide aggregate, making
  // it easy to see how "clean" the UART state is specifically when the TX window opens.
  if (!this->reference_frame_type_.empty()) {
    ESP_LOGI(TAG,
             "  hist uart_start_ref bytes [0,1,2,4,8,16,32,64,128,>128]: %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32
             " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32,
             this->uart_available_start_ref_.buckets[0], this->uart_available_start_ref_.buckets[1],
             this->uart_available_start_ref_.buckets[2], this->uart_available_start_ref_.buckets[3],
             this->uart_available_start_ref_.buckets[4], this->uart_available_start_ref_.buckets[5],
             this->uart_available_start_ref_.buckets[6], this->uart_available_start_ref_.buckets[7],
             this->uart_available_start_ref_.buckets[8], this->uart_available_start_ref_.buckets[9]);
    ESP_LOGI(TAG,
             "  hist fifo_after_etx_ref bytes [0,1,2,4,8,16,32,64,128,>128]: %" PRIu32 " %" PRIu32 " %" PRIu32
             " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32,
             this->fifo_after_etx_ref_.buckets[0], this->fifo_after_etx_ref_.buckets[1],
             this->fifo_after_etx_ref_.buckets[2], this->fifo_after_etx_ref_.buckets[3],
             this->fifo_after_etx_ref_.buckets[4], this->fifo_after_etx_ref_.buckets[5],
             this->fifo_after_etx_ref_.buckets[6], this->fifo_after_etx_ref_.buckets[7],
             this->fifo_after_etx_ref_.buckets[8], this->fifo_after_etx_ref_.buckets[9]);
    if (this->partial_ref_frames_ > 0) {
      ESP_LOGI(TAG, "  ref partial loops: %" PRIu32 " (ref frame split — each is a loop-trip added to TX latency)",
               this->partial_ref_frames_);
      ESP_LOGI(TAG,
               "  hist partial_ref_loop_gap ms [0,1,2,4,8,16,32,64,128,>128]: %" PRIu32 " %" PRIu32 " %" PRIu32
               " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32,
               this->partial_ref_loop_gap_ms_.buckets[0], this->partial_ref_loop_gap_ms_.buckets[1],
               this->partial_ref_loop_gap_ms_.buckets[2], this->partial_ref_loop_gap_ms_.buckets[3],
               this->partial_ref_loop_gap_ms_.buckets[4], this->partial_ref_loop_gap_ms_.buckets[5],
               this->partial_ref_loop_gap_ms_.buckets[6], this->partial_ref_loop_gap_ms_.buckets[7],
               this->partial_ref_loop_gap_ms_.buckets[8], this->partial_ref_loop_gap_ms_.buckets[9]);
    }
  }
}

void SnifferStats::dump_payloads_(size_t top_n, const uint8_t *order) const {
  // Hex/ASCII view of every captured unique payload for the top-N frame types by count.
  // When ascii_strip_high_bit is set, bit 7 is masked before the printable-range gate so
  // displays that pack an attribute flag (blink/inverse) into the high bit render as their
  // underlying character. Off by default to avoid collapsing distinct 8-bit values on binary
  // buses. Non-printable bytes are rendered as '.'. Buffers are preallocated on SnifferStats
  // so the dump path has no heap traffic.
  for (size_t i = 0; i < top_n; i++) {
    const SnifferEntry &e = this->entries_[order[i]];
    if (e.unique_count == 0)
      continue;
    ESP_LOGI(TAG, "  %02X%02X payloads:", e.frame_type[0], e.frame_type[1]);
    for (uint8_t k = 0; k < e.unique_count; k++) {
      const PayloadCapture &p = e.payloads[k];
      for (uint8_t b = 0; b < p.len; b++)
        std::snprintf(this->hex_buf_.get() + b * 3, 4, "%02X ", p.bytes[b]);
      this->hex_buf_[p.len * 3] = '\0';
      for (uint8_t b = 0; b < p.len; b++) {
        uint8_t c = this->strip_high_bit_ ? (p.bytes[b] & 0x7F) : p.bytes[b];
        this->ascii_buf_[b] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
      }
      this->ascii_buf_[p.len] = '\0';
      ESP_LOGI(TAG, "    %5u @ %s |%s|", p.count, this->hex_buf_.get(), this->ascii_buf_.get());
    }
  }
}

}  // namespace esphome::rs485_frame

#endif  // USE_RS485_FRAME_SNIFFER_STATS
