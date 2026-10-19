#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr std::size_t kFuture = 4096;
constexpr std::size_t kWindowStorage = kFuture + 1;  // seed plus NEXT 4096 requests
constexpr std::size_t kTemporalBins = 100;

class FastInput {
 public:
  explicit FastInput(const std::string& path)
      : file_(std::fopen(path.c_str(), "rb")), buffer_(8 * 1024 * 1024) {
    if (!file_) throw std::runtime_error("cannot open " + path);
  }
  ~FastInput() { std::fclose(file_); }

  bool Next(std::uint64_t& value) {
    int ch;
    do {
      ch = Get();
      if (ch == EOF) return false;
    } while (ch < '0' || ch > '9');
    value = 0;
    do {
      value = value * 10 + static_cast<unsigned>(ch - '0');
      ch = Get();
    } while (ch >= '0' && ch <= '9');
    return true;
  }

 private:
  int Get() {
    if (pos_ == size_) {
      size_ = std::fread(buffer_.data(), 1, buffer_.size(), file_);
      pos_ = 0;
      if (size_ == 0) return EOF;
    }
    return static_cast<unsigned char>(buffer_[pos_++]);
  }
  FILE* file_;
  std::vector<char> buffer_;
  std::size_t pos_ = 0;
  std::size_t size_ = 0;
};

struct Event {
  std::uint64_t page = 0;
  std::uint8_t cl = 0;
  std::uint64_t ordinal = 0;
};

struct WindowPage {
  std::array<std::uint16_t, 64> counts{};
  std::uint64_t mask = 0;
};

struct Bin {
  std::uint64_t observations = 0;
  long double sum_a = 0;
  long double sum_s = 0;
  long double sum_q = 0;
  long double sum_moran = 0;
  std::uint64_t moran_valid = 0;
  std::uint64_t small = 0;
  std::uint64_t medium = 0;
  std::uint64_t large = 0;
};

std::uint64_t Quantile(const std::array<std::uint64_t, 65>& hist, double q) {
  std::uint64_t total = 0;
  for (std::size_t i = 1; i < hist.size(); ++i) total += hist[i];
  const auto target = static_cast<std::uint64_t>(std::ceil(q * static_cast<long double>(total)));
  std::uint64_t cumulative = 0;
  for (std::size_t i = 1; i < hist.size(); ++i) {
    cumulative += hist[i];
    if (cumulative >= std::max<std::uint64_t>(1, target)) return i;
  }
  return 0;
}

double Moran(std::uint64_t mask, unsigned active) {
  if (active == 0 || active == 64) return std::numeric_limits<double>::quiet_NaN();
  const double mean = active / 64.0;
  const unsigned both_active = __builtin_popcountll(mask & (mask >> 1));
  const unsigned different = __builtin_popcountll((mask ^ (mask >> 1)) & ((1ULL << 63) - 1));
  const unsigned both_inactive = 63 - both_active - different;
  const double adjacent_sum =
      both_active * (1.0 - mean) * (1.0 - mean) +
      both_inactive * mean * mean - different * mean * (1.0 - mean);
  const double denominator = active * (1.0 - mean);
  return (64.0 / 63.0) * adjacent_sum / denominator;
}

std::string F(double value) {
  if (!std::isfinite(value)) return "nan";
  std::ostringstream out;
  out << std::setprecision(12) << value;
  return out.str();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: feature_analyzer TRACE_NAME TRACE_PATH EXPECTED_ACCESSES SUMMARY_CSV TEMPORAL_CSV\n";
    return 2;
  }
  const std::string name = argv[1];
  const std::string path = argv[2];
  const std::uint64_t expected = std::stoull(argv[3]);
  const std::string summary_path = argv[4];
  const std::string temporal_path = argv[5];

  FastInput input(path);
  std::vector<Event> ring(kWindowStorage);
  std::size_t head = 0;
  std::size_t window_size = 0;
  std::unordered_map<std::uint64_t, WindowPage> window_pages;
  std::unordered_map<std::uint64_t, std::uint64_t> lifetime_masks;
  window_pages.reserve(kWindowStorage * 2);
  lifetime_masks.reserve(1 << 20);

  std::array<std::uint64_t, 65> active_hist{};
  std::array<std::uint64_t, 65> span_hist{};
  std::array<Bin, kTemporalBins> bins{};
  std::uint64_t observations = 0;
  std::uint64_t small = 0, medium = 0, large = 0;
  std::uint64_t span_small = 0, span_medium = 0, span_large = 0;
  std::uint64_t q_ge_050 = 0, q_ge_075 = 0;
  std::uint64_t segment_like = 0, broad_contiguous = 0, fragmented = 0;
  long double sum_a = 0, sum_s = 0, sum_q = 0;
  long double sum_moran = 0, sum_moran_full_as_one = 0;
  std::uint64_t moran_valid = 0, full_active = 0;

  auto add = [&](const Event& event) {
    auto& page = window_pages[event.page];
    if (page.counts[event.cl]++ == 0) page.mask |= (1ULL << event.cl);
  };
  auto remove = [&](const Event& event) {
    auto it = window_pages.find(event.page);
    auto& page = it->second;
    if (--page.counts[event.cl] == 0) page.mask &= ~(1ULL << event.cl);
    if (page.mask == 0) window_pages.erase(it);
  };
  auto evaluate_front = [&]() {
    const Event& seed = ring[head];
    const std::uint64_t mask = window_pages.at(seed.page).mask;
    const unsigned a = __builtin_popcountll(mask);
    int left = seed.cl;
    int right = seed.cl;
    while (left > 0 && (mask & (1ULL << (left - 1)))) --left;
    while (right < 63 && (mask & (1ULL << (right + 1)))) ++right;
    const unsigned s = static_cast<unsigned>(right - left + 1);
    const double q = static_cast<double>(s) / a;
    const double moran = Moran(mask, a);

    ++active_hist[a];
    ++span_hist[s];
    ++observations;
    sum_a += a;
    sum_s += s;
    sum_q += q;
    if (a <= 4) ++small;
    else if (a <= 16) ++medium;
    else ++large;
    if (s <= 4) ++span_small;
    else if (s <= 16) ++span_medium;
    else ++span_large;
    if (q >= 0.50) ++q_ge_050;
    if (q >= 0.75) ++q_ge_075;
    if (a >= 5 && a <= 16 && q >= 0.75) ++segment_like;
    if (a > 16 && q >= 0.75) ++broad_contiguous;
    if (a >= 5 && q <= 0.25) ++fragmented;
    if (std::isfinite(moran)) {
      sum_moran += moran;
      sum_moran_full_as_one += moran;
      ++moran_valid;
    } else if (a == 64) {
      sum_moran_full_as_one += 1.0;
      ++full_active;
    }

    const std::size_t bin_index = std::min<std::size_t>(
        kTemporalBins - 1, expected ? (seed.ordinal * kTemporalBins / expected) : 0);
    auto& bin = bins[bin_index];
    ++bin.observations;
    bin.sum_a += a;
    bin.sum_s += s;
    bin.sum_q += q;
    if (a <= 4) ++bin.small;
    else if (a <= 16) ++bin.medium;
    else ++bin.large;
    if (std::isfinite(moran)) {
      bin.sum_moran += moran;
      ++bin.moran_valid;
    }

    remove(seed);
    head = (head + 1) % kWindowStorage;
    --window_size;
  };

  std::uint64_t timestamp = 0, cpu = 0, address = 0, size = 0, rw = 0;
  std::uint64_t requests = 0, reads = 0, writes = 0;
  std::uint64_t first_ts = 0, last_ts = 0;
  while (input.Next(timestamp)) {
    if (!input.Next(cpu) || !input.Next(address) || !input.Next(size) || !input.Next(rw)) {
      std::cerr << "truncated record at event " << requests << "\n";
      return 3;
    }
    if (size != 64 || rw > 1 || (requests && timestamp < last_ts)) {
      std::cerr << "invalid size/rw/timestamp at event " << requests << '\n'; return 3;
    }
    if (requests == 0) first_ts = timestamp;
    last_ts = timestamp;
    const std::uint64_t page = address >> 12;
    const std::uint8_t cl = static_cast<std::uint8_t>((address >> 6) & 63);
    lifetime_masks[page] |= (1ULL << cl);
    if (rw == 1) ++reads;
    else ++writes;

    const std::size_t tail = (head + window_size) % kWindowStorage;
    ring[tail] = Event{page, cl, requests};
    add(ring[tail]);
    ++window_size;
    ++requests;
    if (window_size == kWindowStorage) evaluate_front();
    if (requests % 10000000ULL == 0) {
      std::cerr << name << ": parsed " << requests << " accesses\n";
    }
  }
  if (!requests) { std::cerr << "empty trace\n"; return 3; }
  while (window_size) evaluate_front();

  std::uint64_t unique_cls = 0;
  for (const auto& [page, mask] : lifetime_masks) unique_cls += __builtin_popcountll(mask);
  const double active_cacheline_ws_mib = unique_cls * 64.0 / (1024.0 * 1024.0);
  const double footprint_mib = lifetime_masks.size() * 4096.0 / (1024.0 * 1024.0);
  const double trace_gib = std::filesystem::file_size(path) / static_cast<double>(1ULL << 30);
  const double mean_interval = requests > 1 && last_ts >= first_ts
                                   ? static_cast<double>(last_ts - first_ts) / (requests - 1)
                                   : std::numeric_limits<double>::quiet_NaN();
  const auto frac = [&](std::uint64_t count) {
    return observations ? static_cast<double>(count) / observations : 0.0;
  };

  std::ofstream summary(summary_path);
  summary << "trace,path,accesses,expected_accesses,count_match,trace_size_gib,unique_pages,footprint_mib,active_cacheline_ws_mib,read_count,write_count,write_ratio,mean_request_interval_raw,window,mean_active_cl,median_active_cl,p90_active_cl,p_active_small_le4,p_active_medium_5_16,p_active_large_gt16,mean_seed_span,median_seed_span,p90_seed_span,p_span_small_le4,p_span_medium_5_16,p_span_large_gt16,mean_q,p_q_ge_050,p_q_ge_075,moran_i_mean_standard,moran_i_mean_full_as_one,p_full_active,segment_like_fraction,broad_contiguous_fraction,fragmented_fraction\n";
  summary << name << ',' << path << ',' << requests << ',' << expected << ','
          << (requests == expected ? "yes" : "no") << ',' << F(trace_gib) << ','
          << lifetime_masks.size() << ',' << F(footprint_mib) << ',' << F(active_cacheline_ws_mib) << ',' << reads << ',' << writes << ','
          << F(requests ? static_cast<double>(writes) / requests : 0.0) << ',' << F(mean_interval) << ','
          << kFuture << ',' << F(static_cast<double>(sum_a / observations)) << ','
          << Quantile(active_hist, 0.5) << ',' << Quantile(active_hist, 0.9) << ','
          << F(frac(small)) << ',' << F(frac(medium)) << ',' << F(frac(large)) << ','
          << F(static_cast<double>(sum_s / observations)) << ',' << Quantile(span_hist, 0.5) << ','
          << Quantile(span_hist, 0.9) << ',' << F(frac(span_small)) << ',' << F(frac(span_medium)) << ','
          << F(frac(span_large)) << ',' << F(static_cast<double>(sum_q / observations)) << ','
          << F(frac(q_ge_050)) << ',' << F(frac(q_ge_075)) << ','
          << F(moran_valid ? static_cast<double>(sum_moran / moran_valid) : std::numeric_limits<double>::quiet_NaN()) << ','
          << F((moran_valid + full_active) ? static_cast<double>(sum_moran_full_as_one / (moran_valid + full_active)) : std::numeric_limits<double>::quiet_NaN()) << ','
          << F(frac(full_active)) << ',' << F(frac(segment_like)) << ',' << F(frac(broad_contiguous)) << ','
          << F(frac(fragmented)) << '\n';

  std::ofstream temporal(temporal_path);
  temporal << "trace,bin,observations,mean_active_cl,mean_seed_span,mean_q,p_small_le4,p_medium_5_16,p_large_gt16,moran_i_mean_standard\n";
  for (std::size_t i = 0; i < bins.size(); ++i) {
    const auto& bin = bins[i];
    if (!bin.observations) continue;
    temporal << name << ',' << i << ',' << bin.observations << ','
             << F(static_cast<double>(bin.sum_a / bin.observations)) << ','
             << F(static_cast<double>(bin.sum_s / bin.observations)) << ','
             << F(static_cast<double>(bin.sum_q / bin.observations)) << ','
             << F(static_cast<double>(bin.small) / bin.observations) << ','
             << F(static_cast<double>(bin.medium) / bin.observations) << ','
             << F(static_cast<double>(bin.large) / bin.observations) << ','
             << F(bin.moran_valid ? static_cast<double>(bin.sum_moran / bin.moran_valid) : std::numeric_limits<double>::quiet_NaN()) << '\n';
  }
  std::cerr << name << ": done, accesses=" << requests << " footprint_mib=" << footprint_mib << '\n';
  return requests == expected ? 0 : 4;
}

