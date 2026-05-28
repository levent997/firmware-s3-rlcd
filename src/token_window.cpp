#include "token_window.h"

namespace tokenwin {

void TokenWindow::update(uint32_t tokens_now, uint32_t now, uint32_t &tokens_boot) {
  if (last_tokens_seen_ == UINT32_MAX) {
    // First heartbeat after boot — anchor without backfill.
    last_tokens_seen_ = tokens_now;
  } else if (tokens_now < last_tokens_seen_) {
    // Desktop restarted — re-anchor, don't count the pre-restart value as
    // a fresh delta.
    last_tokens_seen_ = tokens_now;
  } else {
    uint32_t delta = tokens_now - last_tokens_seen_;
    if (delta > 0) tokens_boot += delta;
    last_tokens_seen_ = tokens_now;
  }

  // Sample at most once per minute (the t=0 edge intentionally re-samples,
  // matching the original behaviour).
  if (last_sample_ms_ != 0 && now - last_sample_ms_ < 60000) {
    // keep existing samples; just recompute the windows below
  } else {
    last_sample_ms_ = now;
    hist_[head_] = {now, tokens_boot};
    head_ = (head_ + 1) % HISTORY;
    if (count_ < HISTORY) count_++;
  }

  // Window = tokens_boot now minus the cumulative at/just-before the cutoff.
  auto cumAgo = [&](uint32_t window_ms) -> uint32_t {
    uint32_t cutoff = (now > window_ms) ? (now - window_ms) : 0;
    uint32_t cum_at_cutoff = tokens_boot;  // no older data => 0 diff
    for (int i = 0; i < count_; i++) {
      int idx = (head_ - 1 - i + HISTORY) % HISTORY;
      if (hist_[idx].t_ms <= cutoff) {
        cum_at_cutoff = hist_[idx].cum;
        break;
      }
      cum_at_cutoff = hist_[idx].cum;
    }
    return tokens_boot - cum_at_cutoff;
  };
  tokens_1h = cumAgo(60UL * 60UL * 1000UL);
  tokens_5h = cumAgo(5UL * 60UL * 60UL * 1000UL);
}

}  // namespace tokenwin
