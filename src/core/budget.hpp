//
// Will a compiled set fit the device? Two independent limits, both computed from the deduplicated
// lines forti-hole would publish (CompileStats::lines), never from raw list sizes:
//
//   entries  FortiOS 7.4.4+ caps domain-feed entries device-wide (accepted_limit: 1,000,000 on
//            entry-level models such as the 60F) and silently drops what does not fit. Available =
//            limit - entries of feeds forti-hole does not own - entry headroom.
//   memory   the lines must also fit in RAM. Predicted used memory = used now + (lines after - lines
//            on the device now) x bytes per line. It must leave memory headroom free (default 15%)
//            and stay below the conserve-mode red threshold (FortiOS stops accepting new sessions
//            there). Bytes per line is measured on the device (learn_bytes_per_line) and starts from a
//            conservative default.
//
// Nothing here talks to the device: DeviceFacts is read by the caller (device.hpp).
//

#ifndef FORTI_HOLE_CORE_BUDGET_HPP
#define FORTI_HOLE_CORE_BUDGET_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fh {

    // Device RAM per feed line before forti-hole has measured the device itself: about twice what
    // was measured on a FortiGate-60F running FortiOS 7.4.8 (docs/guides/capacity.md).
    inline constexpr double default_bytes_per_line = 400;

    struct DeviceFacts {
        std::optional<std::uint64_t> entry_limit;   // accepted_limit
        std::uint64_t other_entries = 0;            // valid entries in domain feeds forti-hole does not own
        std::uint64_t owned_entries = 0;            // valid entries in forti-hole's feeds now
        std::optional<std::uint64_t> memory_total, memory_used;   // bytes (monitor/system/performance/status)
        std::optional<double> memory_red_percent;   // system global memory-use-threshold-red
        std::optional<double> memory_peak_percent;  // 24-hour peak (monitor/system/resource/usage), for warnings
    };

    struct BudgetPolicy {
        std::uint64_t fallback_limit = 1000000;
        double entry_headroom_percent = 10;
        double memory_headroom_percent = 15;
        double bytes_per_line = default_bytes_per_line;
    };

    struct BudgetCheck {
        std::uint64_t lines = 0;                    // what the compiled set needs

        std::uint64_t entry_limit = 0;
        bool entry_limit_reported = false;          // false: fallback_limit
        std::uint64_t entry_available = 0;          // for forti-hole, after other feeds and headroom
        std::uint64_t other_entries = 0;
        double entry_headroom_percent = 0;
        bool entries_fit = true;

        bool memory_known = false;                  // the device reported its memory
        double bytes_per_line = 0;
        std::int64_t memory_delta = 0;              // predicted change in used bytes
        double memory_used_percent = 0, memory_after_percent = 0, memory_ceiling_percent = 0;
        bool memory_fits = true;

        std::vector<std::string> warnings;
        [[nodiscard]] bool fits() const { return entries_fit && memory_fits; }
        // Why it does not fit, in one sentence (empty when it fits).
        [[nodiscard]] std::string reason() const;
    };

    [[nodiscard]] BudgetCheck check_budget(std::uint64_t lines, const DeviceFacts& device, const BudgetPolicy& policy);

    // A measurement from one push: memory used before and after, lines before and after. nullopt when
    // the change was too small to measure against the device's normal fluctuation.
    [[nodiscard]] std::optional<double> measure_bytes_per_line(std::uint64_t used_before, std::uint64_t used_after,
                                                               std::uint64_t lines_before, std::uint64_t lines_after);

    // The estimate to keep: the larger of the stored estimate and a new measurement pulls it up at
    // once (safety first); a smaller measurement lowers it only gradually, and never below a quarter
    // of the default.
    [[nodiscard]] double learn_bytes_per_line(std::optional<double> stored, double measured);

}  // namespace fh

#endif  // FORTI_HOLE_CORE_BUDGET_HPP
