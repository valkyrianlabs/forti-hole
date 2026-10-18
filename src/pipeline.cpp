#include "pipeline.hpp"

#include <chrono>
#include <format>

namespace fh {

    BudgetPolicy budget_policy(const Config& config, const State& state) {
        BudgetPolicy policy;
        policy.fallback_limit = config.budget.fallback_limit;
        policy.entry_headroom_percent = config.budget.entry_headroom_percent;
        policy.memory_headroom_percent = config.budget.memory_headroom_percent;
        policy.bytes_per_line = config.budget.bytes_per_line ? *config.budget.bytes_per_line
                                : state.bytes_per_line       ? *state.bytes_per_line
                                                             : default_bytes_per_line;
        return policy;
    }

    namespace {

        const LoadedList& required(const std::map<std::string, LoadedList, std::less<>>& lists, const std::string& key) {
            auto it = lists.find(key);
            if (it == lists.end() || !it->second.usable())
                throw PipelineError(std::format("list {} is unavailable{}: nothing was compiled, so nothing will be written", key,
                                                it == lists.end() ? "" : ": " + it->second.note));
            return it->second;
        }

        std::unique_ptr<Compiler> compiler_with(const Config& config, const std::map<std::string, LoadedList, std::less<>>& lists, const State& state,
                                                const std::map<std::string, std::string, std::less<>>& variants) {
            CompileOptions options = compile_options(config);
            for (const auto& [label, group]: state.groups) options.partitions[label] = group.partitions;
            auto compiler = std::make_unique<Compiler>(options);
            for (const auto& source: config.sources) {
                if (!source.enabled) continue;
                std::string variant = source.has_variants() ? variants.at(source.id) : "";
                const auto& list = required(lists, list_key(source.id, variant));
                compiler->add_source(list_key(source.id, variant), source.tier, list.text, source.format);
            }
            for (const auto& block: config.block) compiler->add_block(block.name, block.scope, block.tier);
            for (const auto& allow: config.allow) compiler->add_allow(allow.name, allow.scope, allow.profiles);
            for (const auto& regex: config.allow_regex) compiler->add_allow_regex(regex.pattern, regex.profiles);
            if (config.encoding == Encoding::compact) compiler->set_reference(required(lists, "reference").text, config.reference.format);
            return compiler;
        }

    }  // namespace

    CompileRun compile_lists(const Config& config, const std::map<std::string, LoadedList, std::less<>>& lists, const State& state,
                             const DeviceFacts& device) {
        auto started = std::chrono::steady_clock::now();
        CompileRun run;
        BudgetPolicy policy = budget_policy(config, state);

        // Pinned variants first; at most one source chooses automatically (config validation).
        std::map<std::string, std::string, std::less<>> variants;
        const Source* automatic = nullptr;
        for (const auto& source: config.sources) {
            if (!source.enabled || !source.has_variants()) continue;
            if (source.variant == "auto" && !automatic) automatic = &source;
            else variants[source.id] = source.variant == "auto" ? source.auto_order.front() : source.variant;
        }

        if (!automatic) {
            run.result = compiler_with(config, lists, state, variants)->compile();
            run.budget = check_budget(run.result.stats.lines, device, policy);
        } else {
            bool done = false;
            for (const auto& candidate: automatic->candidates()) {
                VariantAttempt attempt{automatic->id, candidate, 0, 0, {}, false, {}};
                auto it = lists.find(list_key(automatic->id, candidate));
                if (it == lists.end() || !it->second.usable()) {
                    attempt.skipped = it == lists.end() ? "not loaded" : it->second.note;
                    run.attempts.push_back(std::move(attempt));
                    continue;
                }
                variants[automatic->id] = candidate;
                // Count first; build the feed lines only for the candidate that is kept.
                auto compiler = compiler_with(config, lists, state, variants);
                CompileResult counted = compiler->compile(false);
                attempt.lines = counted.stats.lines;
                attempt.names = counted.stats.names;
                attempt.budget = check_budget(counted.stats.lines, device, policy);
                bool last = candidate == automatic->candidates().back();
                if (attempt.budget.fits() || last) {
                    attempt.chosen = true;
                    run.result = compiler->compile();
                    run.budget = attempt.budget;
                    run.attempts.push_back(std::move(attempt));
                    done = true;
                    break;
                }
                run.attempts.push_back(std::move(attempt));
            }
            if (!done) {
                // Every candidate list was unavailable.
                std::string why;
                for (const auto& a: run.attempts) why += std::format(" {}: {};", a.variant, a.skipped);
                throw PipelineError(std::format("no variant of {} is available:{} nothing was compiled", automatic->id, why));
            }
            for (std::size_t i = 0; i + 1 < run.attempts.size(); ++i) {
                const auto& a = run.attempts[i];
                if (a.skipped.empty())
                    run.warnings.push_back(std::format("{} {} does not fit: {}; using {}", a.source_id, a.variant, a.budget.reason(),
                                                       variants[automatic->id]));
                else
                    run.warnings.push_back(std::format("{} {} skipped: {}", a.source_id, a.variant, a.skipped));
            }
        }
        run.selected = variants;
        for (const auto& w: run.budget.warnings) run.warnings.push_back(w);
        for (const auto& [key, list]: lists)
            if (list.origin == LoadedList::Origin::cached && !list.note.empty())
                run.warnings.push_back(std::format("{}: using the last good copy ({})", key, list.note));
        run.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return run;
    }

}  // namespace fh
