// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/stats.h"

#include <inttypes.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <cstdint>
#include <limits>

#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/macros.h"
#include "absl/debugging/internal/vdso_support.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/bits.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"

namespace tcmalloc {

static double BytesToMiB(size_t bytes) {
  const double MiB = 1048576.0;
  return bytes / MiB;
}

static double PagesToMiB(uint64_t pages) {
  return BytesToMiB(pages * kPageSize);
}

// For example, PrintRightAdjustedWithPrefix(out, ">=", 42, 6) prints "  >=42".
static void PrintRightAdjustedWithPrefix(TCMalloc_Printer *out,
                                         const char *prefix, int num,
                                         int width) {
  width -= strlen(prefix);
  int num_tmp = num;
  for (int i = 0; i < width - 1; i++) {
    num_tmp /= 10;
    if (num_tmp == 0) {
      out->printf(" ");
    }
  }
  out->printf("%s%d", prefix, num);
}

void PrintStats(const char *label, TCMalloc_Printer *out,
                const BackingStats &backing, const SmallSpanStats &small,
                const LargeSpanStats &large, bool everything) {
  size_t nonempty_sizes = 0;
  for (int i = 0; i < kMaxPages; ++i) {
    const size_t norm = small.normal_length[i];
    const size_t ret = small.returned_length[i];
    if (norm + ret > 0) nonempty_sizes++;
  }

  out->printf("------------------------------------------------\n");
  out->printf("%s: %zu sizes; %6.1f MiB free; %6.1f MiB unmapped\n", label,
              nonempty_sizes, BytesToMiB(backing.free_bytes),
              BytesToMiB(backing.unmapped_bytes));
  out->printf("------------------------------------------------\n");

  size_t cum_normal_pages = 0, cum_returned_pages = 0, cum_total_pages = 0;
  if (!everything) return;

  for (size_t i = 0; i < kMaxPages; ++i) {
    const size_t norm = small.normal_length[i];
    const size_t ret = small.returned_length[i];
    const size_t total = norm + ret;
    if (total == 0) continue;
    const size_t norm_pages = norm * i;
    const size_t ret_pages = ret * i;
    const size_t total_pages = norm_pages + ret_pages;
    cum_normal_pages += norm_pages;
    cum_returned_pages += ret_pages;
    cum_total_pages += total_pages;
    out->printf(
        "%6zu pages * %6zu spans ~ %6.1f MiB; %6.1f MiB cum"
        "; unmapped: %6.1f MiB; %6.1f MiB cum\n",
        i, total, PagesToMiB(total_pages), PagesToMiB(cum_total_pages),
        PagesToMiB(ret_pages), PagesToMiB(cum_returned_pages));
  }

  cum_normal_pages += large.normal_pages;
  cum_returned_pages += large.returned_pages;
  const size_t large_total_pages = large.normal_pages + large.returned_pages;
  cum_total_pages += large_total_pages;
  PrintRightAdjustedWithPrefix(out, ">=", kMaxPages, 6);
  out->printf(
      " large * %6zu spans ~ %6.1f MiB; %6.1f MiB cum"
      "; unmapped: %6.1f MiB; %6.1f MiB cum\n",
      static_cast<size_t>(large.spans), PagesToMiB(large_total_pages),
      PagesToMiB(cum_total_pages), PagesToMiB(large.returned_pages),
      PagesToMiB(cum_returned_pages));
}

struct HistBucket {
  uint64_t min_sec;
  const char *label;
};

static const HistBucket kSpanAgeHistBuckets[] = {
    // clang-format off
    {0, "<1s"},
    {1, "1s"},
    {30, "30s"},
    {1 * 60, "1m"},
    {30 * 60, "30m"},
    {1 * 60 * 60, "1h"},
    {8 * 60 * 60, "8+h"},
    // clang-format on
};

struct PageHeapEntry {
  int64_t span_size;  // bytes
  int64_t present;    // bytes
  int64_t released;   // bytes
  int64_t num_spans;
  double avg_live_age_secs;
  double avg_released_age_secs;
  int64_t live_age_hist_bytes[PageAgeHistograms::kNumBuckets] = {0, 0, 0, 0,
                                                               0, 0, 0};
  int64_t released_age_hist_bytes[PageAgeHistograms::kNumBuckets] = {0, 0, 0, 0,
                                                                   0, 0, 0};

  void PrintInPbtxt(PbtxtRegion *parent,
                    absl::string_view sub_region_name) const;
};

void PageHeapEntry::PrintInPbtxt(PbtxtRegion *parent,
                                 absl::string_view sub_region_name) const {
  auto page_heap = parent->CreateSubRegion(sub_region_name);
  page_heap.PrintI64("span_size", span_size);
  page_heap.PrintI64("present", present);
  page_heap.PrintI64("released", released);
  page_heap.PrintI64("num_spans", num_spans);
  page_heap.PrintDouble("avg_live_age_secs", avg_live_age_secs);
  page_heap.PrintDouble("avg_released_age_secs", avg_released_age_secs);

  for (int j = 0; j < PageAgeHistograms::kNumBuckets; j++) {
    uint64_t min_age_secs = kSpanAgeHistBuckets[j].min_sec;
    uint64_t max_age_secs = j != PageAgeHistograms::kNumBuckets - 1
                              ? kSpanAgeHistBuckets[j + 1].min_sec
                              : INT_MAX;
    if (live_age_hist_bytes[j] != 0) {
      auto live_age_hist = page_heap.CreateSubRegion("live_age_hist");
      live_age_hist.PrintI64("bytes", live_age_hist_bytes[j]);
      live_age_hist.PrintI64("min_age_secs", min_age_secs);
      live_age_hist.PrintI64("max_age_secs", max_age_secs);
    }
    if (released_age_hist_bytes[j] != 0) {
      auto released_age_hist = page_heap.CreateSubRegion("released_age_hist");
      released_age_hist.PrintI64("bytes", released_age_hist_bytes[j]);
      released_age_hist.PrintI64("min_age_secs", min_age_secs);
      released_age_hist.PrintI64("max_age_secs", max_age_secs);
    }
  }
}

void PrintStatsInPbtxt(PbtxtRegion *region, const SmallSpanStats &small,
                       const LargeSpanStats &large,
                       const PageAgeHistograms &ages) {
  // Print for small pages.
  for (size_t i = 0; i < kMaxPages; ++i) {
    const size_t norm = small.normal_length[i];
    const size_t ret = small.returned_length[i];
    const size_t total = norm + ret;
    if (total == 0) continue;
    const size_t norm_pages = norm * i;
    const size_t ret_pages = ret * i;
    PageHeapEntry entry;
    entry.span_size = i * kPageSize;
    entry.present = norm_pages * kPageSize;
    entry.released = ret_pages * kPageSize;
    entry.num_spans = total;

    // Histogram is only collected for pages < ages.kNumSize.
    if (i < PageAgeHistograms::kNumSizes) {
      entry.avg_live_age_secs =
          ages.GetSmallHistogram(/*released=*/false, i)->avg_age();
      entry.avg_released_age_secs =
          ages.GetSmallHistogram(/*released=*/true, i)->avg_age();
      for (int j = 0; j < ages.kNumBuckets; j++) {
        entry.live_age_hist_bytes[j] =
            ages.GetSmallHistogram(/*released=*/false, i)->pages_in_bucket(j) *
            kPageSize;
        entry.released_age_hist_bytes[j] =
            ages.GetSmallHistogram(/*released=*/true, i)->pages_in_bucket(j) *
            kPageSize;
      }
    }
    entry.PrintInPbtxt(region, "page_heap");
  }

  // Print for large page.
  {
    PageHeapEntry entry;
    entry.span_size = -1;
    entry.num_spans = large.spans;
    entry.present = large.normal_pages * kPageSize;
    entry.released = large.returned_pages * kPageSize;
    entry.avg_live_age_secs =
        ages.GetLargeHistogram(/*released=*/false)->avg_age();
    entry.avg_released_age_secs =
        ages.GetLargeHistogram(/*released=*/true)->avg_age();
    for (int j = 0; j < ages.kNumBuckets; j++) {
      entry.live_age_hist_bytes[j] =
          ages.GetLargeHistogram(/*released=*/false)->pages_in_bucket(j) *
          kPageSize;
      entry.released_age_hist_bytes[j] =
          ages.GetLargeHistogram(/*released=*/true)->pages_in_bucket(j) *
          kPageSize;
    }
    entry.PrintInPbtxt(region, "page_heap");
  }

  region->PrintI64("min_large_span_size", kMaxPages);
}

static int HistBucketIndex(double age_exact) {
  uint64_t age_secs = age_exact;  // truncate to seconds
  for (int i = 0; i < ABSL_ARRAYSIZE(kSpanAgeHistBuckets) - 1; i++) {
    if (age_secs < kSpanAgeHistBuckets[i + 1].min_sec) {
      return i;
    }
  }
  return ABSL_ARRAYSIZE(kSpanAgeHistBuckets) - 1;
}

PageAgeHistograms::PageAgeHistograms(int64_t now)
    : now_(now), freq_(absl::base_internal::CycleClock::Frequency()) {
  static_assert(
      PageAgeHistograms::kNumBuckets == ABSL_ARRAYSIZE(kSpanAgeHistBuckets),
      "buckets don't match constant in header");

  memset(&live_, 0, sizeof(live_));
  memset(&returned_, 0, sizeof(returned_));
}

void PageAgeHistograms::RecordRange(Length pages, bool released, int64_t when) {
  double age = std::max(0.0, (now_ - when) / freq_);
  (released ? returned_ : live_).Record(pages, age);
}

void PageAgeHistograms::PerSizeHistograms::Record(Length pages, double age) {
  (pages < kLargeSize ? GetSmall(pages) : GetLarge())->Record(pages, age);
  total.Record(pages, age);
}

static uint32_t SaturatingAdd(uint32_t x, uint32_t y) {
  uint32_t z = x + y;
  if (z < x) z = std::numeric_limits<uint32_t>::max();
  return z;
}

void PageAgeHistograms::Histogram::Record(Length pages, double age) {
  size_t bucket = HistBucketIndex(age);
  buckets_[bucket] = SaturatingAdd(buckets_[bucket], pages);
  total_pages_ += pages;
  total_age_ += pages * age;
}

void PageAgeHistograms::Print(const char *label, TCMalloc_Printer *out) const {
  out->printf("------------------------------------------------\n");
  out->printf(
      "%s cache entry age (count of pages in spans of "
      "a given size that have been idle for up to the given period of time)\n",
      label);
  out->printf("------------------------------------------------\n");
  out->printf("                             ");
  // Print out the table header.  All columns have width 8 chars.
  out->printf("    mean");
  for (int b = 0; b < kNumBuckets; b++) {
    out->printf("%8s", kSpanAgeHistBuckets[b].label);
  }
  out->printf("\n");

  live_.Print("Live span", out);
  out->printf("\n");
  returned_.Print("Unmapped span", out);
}

static void PrintLineHeader(TCMalloc_Printer *out, const char *kind,
                            const char *prefix, int num) {
  // Print the beginning of the line, e.g. "Live span,   >=128 pages: ".  The
  // span size ("128" in the example) is padded such that it plus the span
  // prefix ("Live") plus the span size prefix (">=") is kHeaderExtraChars wide.
  const int kHeaderExtraChars = 19;
  const int span_size_width =
      std::max<int>(0, kHeaderExtraChars - strlen(kind));
  out->printf("%s, ", kind);
  PrintRightAdjustedWithPrefix(out, prefix, num, span_size_width);
  out->printf(" pages: ");
}

void PageAgeHistograms::PerSizeHistograms::Print(const char *kind,
                                                 TCMalloc_Printer *out) const {
  out->printf("%-15s TOTAL PAGES: ", kind);
  total.Print(out);

  for (Length l = 1; l < kNumSizes; ++l) {
    const Histogram *here = &small[l - 1];
    if (here->empty()) continue;
    PrintLineHeader(out, kind, "", l);
    here->Print(out);
  }

  if (!large.empty()) {
    PrintLineHeader(out, kind, ">=", kNumSizes);
    large.Print(out);
  }
}

void PageAgeHistograms::Histogram::Print(TCMalloc_Printer *out) const {
  const double mean = avg_age();
  out->printf(" %7.1f", mean);
  for (int b = 0; b < kNumBuckets; ++b) {
    out->printf(" %7" PRIu32, buckets_[b]);
  }

  out->printf("\n");
}

void PageAllocInfo::Print(TCMalloc_Printer *out) const {
  int64_t ns = TimeNanos();
  double hz = (1000.0 * 1000 * 1000) / ns;
  out->printf("%s: stats on allocation sizes\n", label_);
  out->printf("%s: %zu pages live small allocation\n", label_, total_small_);
  out->printf("%s: %zu pages of slack on large allocations\n", label_,
              total_slack_);
  out->printf("%s: largest seen allocation %zu pages\n", label_, largest_seen_);
  out->printf("%s: per-size information:\n", label_);

  auto print_counts = [this, hz, out](const Counts &c, Length nmin,
                                      Length nmax) {
    const size_t a = c.nalloc;
    const size_t f = c.nfree;
    const size_t a_pages = c.alloc_size;
    const size_t f_pages = c.free_size;
    if (a == 0) return;
    const size_t live = a - f;
    const double live_mib = BytesToMiB((a_pages - f_pages) * kPageSize);
    const double rate_hz = a * hz;
    const double mib_hz = BytesToMiB(a_pages * kPageSize) * hz;
    if (nmin == nmax) {
      out->printf("%s: %21zu page info: ", label_, nmin);
    } else {
      out->printf("%s: [ %7zu , %7zu ] page info: ", label_, nmin, nmax);
    }
    out->printf(
        "%10zu / %10zu a/f, %8zu (%6.1f MiB) live, "
        "%8.3g allocs/s (%6.1f MiB/s)\n",
        a, f, live, live_mib, rate_hz, mib_hz);
  };

  for (int i = 0; i < kMaxPages; ++i) {
    const Length n = i + 1;
    print_counts(small_[i], n, n);
  }

  for (int i = 0; i < kAddressBits - kPageShift; ++i) {
    const Length nmax = static_cast<Length>(1) << i;
    const Length nmin = nmax / 2 + 1;
    print_counts(large_[i], nmin, nmax);
  }
}

void PageAllocInfo::PrintInPbtxt(PbtxtRegion *region,
                                 absl::string_view stat_name) const {
  int64_t ns = TimeNanos();
  double hz = (1000.0 * 1000 * 1000) / ns;
  region->PrintI64("num_small_allocation_pages", total_small_);
  region->PrintI64("num_slack_pages", total_slack_);
  region->PrintI64("largest_allocation_pages", largest_seen_);

  auto print_counts = [hz, region, &stat_name](const Counts &c, Length nmin,
                                               Length nmax) {
    const size_t a = c.nalloc;
    const size_t f = c.nfree;
    const size_t a_pages = c.alloc_size;
    const size_t f_pages = c.free_size;
    if (a == 0) return;
    const int64_t live_bytes = (a_pages - f_pages) * kPageSize;
    const double rate_hz = a * hz;
    const double bytes_hz = static_cast<double>(a_pages * kPageSize) * hz;
    auto stat = region->CreateSubRegion(stat_name);
    stat.PrintI64("min_span_pages", nmin);
    stat.PrintI64("max_span_pages", nmax);
    stat.PrintI64("num_spans_allocated", a);
    stat.PrintI64("num_spans_freed", f);
    stat.PrintI64("live_bytes", live_bytes);
    stat.PrintDouble("spans_allocated_per_second", rate_hz);
    stat.PrintI64("bytes_allocated_per_second", static_cast<int64_t>(bytes_hz));
  };

  for (int i = 0; i < kMaxPages; ++i) {
    const Length n = i + 1;
    print_counts(small_[i], n, n);
  }

  for (int i = 0; i < kAddressBits - kPageShift; ++i) {
    const Length nmax = static_cast<Length>(1) << i;
    const Length nmin = nmax / 2 + 1;
    print_counts(large_[i], nmin, nmax);
  }
}

static size_t RoundUp(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

void PageAllocInfo::RecordAlloc(PageID p, Length n) {
  if (ABSL_PREDICT_FALSE(log_on())) {
    int64_t t = TimeNanos();
    LogAlloc(t, p, n);
  }

  static_assert(kMaxPages * kPageSize == 1024 * 1024, "threshold changed?");
  static_assert(kMaxPages < kPagesPerHugePage, "there should be slack");
  largest_seen_ = std::max(largest_seen_, n);
  if (n <= kMaxPages) {
    total_small_ += n;
    small_[n - 1].Alloc(n);
  } else {
    Length slack = RoundUp(n, kPagesPerHugePage) - n;
    total_slack_ += slack;
    size_t i = tcmalloc_internal::Bits::Log2Ceiling(n);
    large_[i].Alloc(n);
  }
}

void PageAllocInfo::RecordFree(PageID p, Length n) {
  if (ABSL_PREDICT_FALSE(log_on())) {
    int64_t t = TimeNanos();
    LogFree(t, p, n);
  }

  if (n <= kMaxPages) {
    total_small_ -= n;
    small_[n - 1].Free(n);
  } else {
    Length slack = RoundUp(n, kPagesPerHugePage) - n;
    total_slack_ -= slack;
    size_t i = tcmalloc_internal::Bits::Log2Ceiling(n);
    large_[i].Free(n);
  }
}

void PageAllocInfo::RecordRelease(Length n, Length got) {
  if (ABSL_PREDICT_FALSE(log_on())) {
    int64_t t = TimeNanos();
    LogRelease(t, n);
  }
}

const PageAllocInfo::Counts &PageAllocInfo::counts_for(Length n) const {
  if (n <= kMaxPages) {
    return small_[n - 1];
  }
  size_t i = tcmalloc_internal::Bits::Log2Ceiling(n);
  return large_[i];
}

// Our current format is really simple. We have an eight-byte version
// number as a header (currently = 1). We then follow up with a sequence
// of fixed-size events, each 16 bytes:
// - 8 byte "id" (really returned page)
// - 4 byte size (in kib, for compatibility)
//   (this gets us to 4 TiB; anything larger is reported truncated)
// - 4 bytes for when (ms since last event) + what
// We shift up the when by 8 bits, and store what the event is in
// low 8 bits. (Currently just 0=alloc, 1=free, 2=Release.)
// This truncates time deltas to 2^24 ms ~= 4 hours.
// This could be compressed further.  (As is, it compresses well
// with gzip.)
// All values are host-order.

struct Entry {
  uint64_t id;
  uint32_t kib;
  uint32_t whenwhat;
};

using tcmalloc::tcmalloc_internal::signal_safe_write;

void PageAllocInfo::Write(uint64_t when, uint8_t what, PageID p, Length n) {
  static_assert(sizeof(Entry) == 16, "bad sizing");
  Entry e;
  // Round the time to ms *before* computing deltas, because this produces more
  // accurate results in the long run.

  // Consider events that occur at absolute time 0.7ms and 50ms.  If
  // we take deltas first, we say the first event occurred at +0.7 =
  // 0ms and the second event occurred at +49.3ms = 49ms.
  // Rounding first produces 0 and 50.
  const uint64_t ms = when / 1000 / 1000;
  uint64_t delta_ms = ms - last_ms_;
  last_ms_ = ms;
  // clamping
  if (delta_ms >= 1 << 24) {
    delta_ms = (1 << 24) - 1;
  }
  e.whenwhat = delta_ms << 8 | what;
  e.id = p;
  size_t bytes = (n << kPageShift);
  static const size_t KiB = 1024;
  static const size_t kMaxRep = std::numeric_limits<uint32_t>::max() * KiB;
  if (bytes > kMaxRep) {
    bytes = kMaxRep;
  }
  e.kib = bytes / KiB;
  const char *ptr = reinterpret_cast<const char *>(&e);
  const size_t len = sizeof(Entry);
  CHECK_CONDITION(len == signal_safe_write(fd_, ptr, len, nullptr));
}

PageAllocInfo::PageAllocInfo(const char *label, int log_fd)
    : label_(label), fd_(log_fd) {
  if (ABSL_PREDICT_FALSE(log_on())) {
    // version 1 of the format, in case we change things up
    uint64_t header = 1;
    const char *ptr = reinterpret_cast<const char *>(&header);
    const size_t len = sizeof(header);
    CHECK_CONDITION(len == signal_safe_write(fd_, ptr, len, nullptr));
  }
}

int64_t PageAllocInfo::TimeNanos() const {
  return GetCurrentTimeNanos() - baseline_ns_;
}

// Why does this exist?  Why not just use absl::GetCurrentTimeNanos?
// Failing that, why not just use clock_gettime?  See b/65384231, but
// essentially because we can't work around people LD_PRELOADing a
// broken and unsafe clock_gettime. Since the real implementation is
// actually a VDSO function, we just go straight to there, which LD_PRELOAD
// can't interfere with.
//
// Now, of course, we can't guarantee this VDSO approach will work--we
// may be on some strange system without one, or one with a newer
// version of the symbols and no interpolating shim. But we can
// gracefully fail back to the "real" clock_gettime.  Will it work if
// someone is doing something weird? Who knows, but it's no worse than
// any other option.
typedef int (*ClockGettimePointer)(clockid_t clk_id, struct timespec *tp);

const ClockGettimePointer GetRealClock() {
#if ABSL_HAVE_ELF_MEM_IMAGE
  absl::debugging_internal::VDSOSupport vdso;
  absl::debugging_internal::VDSOSupport::SymbolInfo info;
  // The VDSO contents aren't very consistent, so we make our best
  // guesses.  Each of these named and versioned symbols should be
  // equivalent to just calling clock_gettime if they exist.

  // Expected on x86_64
  if (vdso.LookupSymbol("__vdso_clock_gettime", "LINUX_2.6",
                        absl::debugging_internal::VDSOSupport::kVDSOSymbolType,
                        &info)) {
    return reinterpret_cast<const ClockGettimePointer>(
        const_cast<void *>(info.address));
  }

  // Expected on Power
  if (vdso.LookupSymbol("__kernel_clock_gettime", "LINUX_2.6.15",
                        absl::debugging_internal::VDSOSupport::kVDSOSymbolType,
                        &info)) {
    return reinterpret_cast<const ClockGettimePointer>(
        const_cast<void *>(info.address));
  }
  // Expected on arm64
  if (vdso.LookupSymbol("__kernel_clock_gettime", "LINUX_2.6.39",
                        absl::debugging_internal::VDSOSupport::kVDSOSymbolType,
                        &info)) {
    return reinterpret_cast<const ClockGettimePointer>(
        const_cast<void *>(info.address));
  }
#endif

  // Hopefully this is good enough.
  return &clock_gettime;
}

int64_t GetCurrentTimeNanos() {
  static const ClockGettimePointer p = GetRealClock();
  struct timespec ts;
  int ret = p(CLOCK_MONOTONIC, &ts);
  CHECK_CONDITION(ret == 0);

  // If we are here rather than failing from the CHECK_CONDITION, gettime (via
  // p) succeeded.  Since we used an unusual calling technique (directly into
  // the VDSO), sanitizers cannot see that this memory has been initialized.
  ANNOTATE_MEMORY_IS_INITIALIZED(&ts.tv_sec, sizeof(ts.tv_sec));
  ANNOTATE_MEMORY_IS_INITIALIZED(&ts.tv_nsec, sizeof(ts.tv_nsec));

  int64_t s = ts.tv_sec;
  int64_t ns = ts.tv_nsec;
  ns += s * 1000 * 1000 * 1000;

  return ns;
}

}  // namespace tcmalloc