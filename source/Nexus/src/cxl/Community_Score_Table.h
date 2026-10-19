#ifndef MQSIM_COMMUNITY_SCORE_TABLE_H
#define MQSIM_COMMUNITY_SCORE_TABLE_H

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

// Host-side acceleration of the existing N=63 likelihood calculation.
// No approximation: preserve division, log, addition order and strict ties.
// This table is simulator implementation memory, not modeled cache metadata.
class CommunityScoreTable64 {
public:
    static const double* for_total(uint64_t total)
    {
        static const CommunityScoreTable64 table;
        return table.scores_.data() + total * 4096;
    }

private:
    static double term(uint64_t count, double probability)
    {
        return count == 0 ? 0.0 : static_cast<double>(count) * std::log(probability);
    }

    CommunityScoreTable64()
    {
        scores_.fill(-std::numeric_limits<double>::infinity());
        constexpr uint64_t N = 63;
        for (uint64_t K = 1; K <= N; ++K) {
            double p = static_cast<double>(K) / static_cast<double>(N);
            double l0 = term(K, p) + term(N - K, 1.0 - p);
            for (uint64_t n = 1; n < N; ++n) {
                for (uint64_t k = 1; k <= n && k <= K; ++k) {
                    uint64_t outside_n = N - n;
                    uint64_t outside_k = K - k;
                    if (outside_k > outside_n) continue;
                    double p_in = static_cast<double>(k) / static_cast<double>(n);
                    double p_out = static_cast<double>(outside_k) / static_cast<double>(outside_n);
                    if (!(p_in > p_out)) continue;
                    double l1 = term(k, p_in) + term(n - k, 1.0 - p_in)
                        + term(outside_k, p_out) + term(outside_n - outside_k, 1.0 - p_out);
                    scores_[K * 4096 + n * 64 + k] = 2.0 * (l1 - l0) - std::log(static_cast<double>(N));
                }
            }
        }
    }

    std::array<double, 64 * 64 * 64> scores_;
};

#endif
