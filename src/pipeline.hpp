//
// One compile, start to finish: lists -> compiler -> budget, including the choice of a source's
// variant ("auto": hagezi TIF medium if the compiled, deduplicated set fits the device, else mini).
//
// The choice is made on the compiled set, never on a list's advertised size: each candidate is
// compiled together with every other source, and the first whose total line count fits both
// budgets (entries and memory, budget.hpp) wins. When none fits, the result says so and why, and
// a sync refuses it (budget.on_overrun "refuse"); nothing is trimmed to make it fit.
//

#ifndef FORTI_HOLE_PIPELINE_HPP
#define FORTI_HOLE_PIPELINE_HPP

#include "config.hpp"
#include "core/budget.hpp"
#include "core/compile.hpp"
#include "sources.hpp"
#include "state.hpp"

#include <map>
#include <string>
#include <vector>

namespace fh {

    struct VariantAttempt {
        std::string source_id, variant;
        std::uint64_t lines = 0, names = 0;
        BudgetCheck budget;
        bool chosen = false;
        std::string skipped;   // why this candidate was not compiled (its list unavailable)
    };

    struct CompileRun {
        CompileResult result;
        BudgetCheck budget;
        std::vector<VariantAttempt> attempts;
        std::map<std::string, std::string, std::less<>> selected;   // source id -> variant compiled
        std::vector<std::string> warnings;
        double seconds = 0;
        [[nodiscard]] bool fits() const { return budget.fits(); }
    };

    class PipelineError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // Throws PipelineError when a required list is unavailable (no download, no last good copy).
    CompileRun compile_lists(const Config& config, const std::map<std::string, LoadedList, std::less<>>& lists, const State& state,
                             const DeviceFacts& device);

    BudgetPolicy budget_policy(const Config& config, const State& state);

}  // namespace fh

#endif  // FORTI_HOLE_PIPELINE_HPP
