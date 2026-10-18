//
// Reports: what a compile produced and why, what a sync will do or did, what the device holds.
// Every report has a JSON form (stable keys, for automation: --json) and a text form.
//

#ifndef FORTI_HOLE_REPORT_HPP
#define FORTI_HOLE_REPORT_HPP

#include "device.hpp"
#include "pipeline.hpp"
#include "sources.hpp"
#include "sync.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>

namespace fh {

    nlohmann::json lists_json(const std::map<std::string, LoadedList, std::less<>>& lists);
    nlohmann::json budget_json(const BudgetCheck& budget);
    // With per-feed line lists only when `lines` is set (large).
    nlohmann::json compile_json(const CompileRun& run, bool lines = false);
    nlohmann::json plan_json(const SyncPlan& plan);
    nlohmann::json outcome_json(const SyncOutcome& outcome);
    nlohmann::json device_json(const Inventory& inventory);

    std::string lists_text(const std::map<std::string, LoadedList, std::less<>>& lists);
    std::string compile_text(const CompileRun& run);
    std::string plan_text(const SyncPlan& plan);
    std::string outcome_text(const SyncOutcome& outcome);
    std::string device_text(const Inventory& inventory);

    // 1234567 -> "1,234,567"
    std::string grouped(std::uint64_t value);

}  // namespace fh

#endif  // FORTI_HOLE_REPORT_HPP
