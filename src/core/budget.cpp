#include "core/budget.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace fh {

    std::string BudgetCheck::reason() const {
        if (!entries_fit)
            return std::format("{} lines needed, {} available of the device's {}-entry limit ({} in other feeds, {:g}% kept free)",
                               lines, entry_available, entry_limit, other_entries, entry_headroom_percent);
        if (!memory_fits)
            return std::format("predicted memory use {:.1f}% exceeds the {:.1f}% ceiling ({:+.0f} MB for the change at {:.0f} bytes per line)",
                               memory_after_percent, memory_ceiling_percent, static_cast<double>(memory_delta) / 1e6, bytes_per_line);
        return {};
    }

    BudgetCheck check_budget(std::uint64_t lines, const DeviceFacts& device, const BudgetPolicy& policy) {
        BudgetCheck check;
        check.lines = lines;

        check.entry_limit_reported = device.entry_limit.has_value() && *device.entry_limit > 0;
        check.entry_limit = check.entry_limit_reported ? *device.entry_limit : policy.fallback_limit;
        auto headroom = static_cast<std::uint64_t>(std::ceil(static_cast<double>(check.entry_limit) * policy.entry_headroom_percent / 100.0));
        std::uint64_t taken = device.other_entries + headroom;
        check.entry_available = taken >= check.entry_limit ? 0 : check.entry_limit - taken;
        check.entries_fit = lines <= check.entry_available;
        check.other_entries = device.other_entries;
        check.entry_headroom_percent = policy.entry_headroom_percent;
        if (!check.entry_limit_reported)
            check.warnings.push_back(std::format("the device did not report its entry limit; assuming {}", check.entry_limit));

        check.bytes_per_line = policy.bytes_per_line;
        check.memory_known = device.memory_total && device.memory_used && *device.memory_total > 0;
        if (check.memory_known) {
            auto total = static_cast<double>(*device.memory_total), used = static_cast<double>(*device.memory_used);
            auto delta = (static_cast<double>(lines) - static_cast<double>(device.owned_entries)) * policy.bytes_per_line;
            check.memory_delta = static_cast<std::int64_t>(delta);
            check.memory_used_percent = 100.0 * used / total;
            check.memory_after_percent = 100.0 * std::max(0.0, used + delta) / total;
            check.memory_ceiling_percent = 100.0 - policy.memory_headroom_percent;
            // Conserve mode starts at the red threshold; stay a few points under it whatever the headroom says.
            if (device.memory_red_percent) check.memory_ceiling_percent = std::min(check.memory_ceiling_percent, *device.memory_red_percent - 3.0);
            // Shrinking or keeping the set is always allowed: it can only free memory.
            check.memory_fits = delta <= 0 || check.memory_after_percent <= check.memory_ceiling_percent;
            if (device.memory_peak_percent && device.memory_red_percent && delta > 0) {
                double peak_after = *device.memory_peak_percent + 100.0 * delta / total;
                if (peak_after >= *device.memory_red_percent)
                    check.warnings.push_back(std::format("at the last 24 hours' peak ({:.0f}%) the device would reach {:.1f}%, at or past its "
                                                         "conserve-mode threshold ({:.0f}%)",
                                                         *device.memory_peak_percent, peak_after, *device.memory_red_percent));
            }
        } else {
            check.warnings.push_back("the device did not report its memory; only the entry limit was checked");
        }
        return check;
    }

    std::optional<double> measure_bytes_per_line(std::uint64_t used_before, std::uint64_t used_after, std::uint64_t lines_before,
                                                 std::uint64_t lines_after) {
        // FortiOS memory moves by tens of megabytes on its own; below ~50,000 lines the signal drowns.
        if (lines_after <= lines_before || lines_after - lines_before < 50000) return std::nullopt;
        if (used_after <= used_before) return std::nullopt;
        return static_cast<double>(used_after - used_before) / static_cast<double>(lines_after - lines_before);
    }

    double learn_bytes_per_line(std::optional<double> stored, double measured) {
        double floor = default_bytes_per_line / 4;
        if (!stored) return std::max(measured, floor);
        if (measured >= *stored) return measured;
        return std::max(floor, *stored * 0.75 + measured * 0.25);
    }

}  // namespace fh
