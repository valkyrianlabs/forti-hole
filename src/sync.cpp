#include "sync.hpp"

#include "core/budget.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace fh {

    using json = nlohmann::json;

    std::string_view to_string(FeedAction::Kind kind) {
        switch (kind) {
            case FeedAction::Kind::unchanged: return "unchanged";
            case FeedAction::Kind::create: return "create";
            case FeedAction::Kind::snapshot: return "snapshot";
            case FeedAction::Kind::delta: return "delta";
            case FeedAction::Kind::remove: return "remove";
        }
        return "unchanged";
    }

    namespace {

        constexpr std::string_view push_resource = "monitor/system/external-resource/dynamic";

        json push_op(std::string_view feed, std::string_view command, std::vector<std::string>::const_iterator first,
                     std::vector<std::string>::const_iterator last) {
            json entries = json::array();
            for (auto it = first; it != last; ++it) entries.push_back(*it);
            return {{"op", "action"},
                    {"resource", push_resource},
                    {"body", {{"commands", json::array({{{"name", feed}, {"command", command}, {"entries", std::move(entries)}}})}}}};
        }

        void push_chunks(json& out, std::string_view feed, std::string_view command, const std::vector<std::string>& lines, std::size_t chunk) {
            for (std::size_t at = 0; at < lines.size(); at += chunk) {
                auto first = lines.begin() + static_cast<std::ptrdiff_t>(at);
                auto last = lines.begin() + static_cast<std::ptrdiff_t>(std::min(lines.size(), at + chunk));
                out.push_back(push_op(feed, command, first, last));
            }
        }

        // FortiGuard filters forti-hole writes on a profile: one block entry per category.
        json filters_for(const std::vector<int>& categories, bool log) {
            json filters = json::array();
            int id = 1;
            for (int category: categories)
                filters.push_back({{"id", id++}, {"category", category}, {"action", "block"}, {"log", log ? "enable" : "disable"}});
            return filters;
        }

        // The (category, action, log) set of a device ftgd-dns filter list, for comparison.
        std::set<std::tuple<int, std::string, std::string>> filter_set(const json& filters) {
            std::set<std::tuple<int, std::string, std::string>> out;
            for (const auto& f: filters)
                out.insert({f.value("category", 0), f.value("action", std::string("monitor")), f.value("log", std::string("enable"))});
            return out;
        }

        std::string join(const std::vector<std::string>& items, std::string_view separator = ", ") {
            std::string out;
            for (const auto& item: items) out += (out.empty() ? "" : std::string(separator)) + item;
            return out;
        }

        json with_target(const Config& config, json request) {
            if (!config.profile.empty()) request["profile"] = config.profile;
            if (!config.vdom.empty()) request["vdom"] = config.vdom;
            return request;
        }

    }  // namespace

    SyncPlan plan_sync(const Config& config, const State& state, const Home& home, const Inventory& inventory, const CompileRun& run) {
        SyncPlan plan;
        const auto& result = run.result;
        if (!inventory.collisions.empty())
            throw SyncError(std::format("objects named {}* exist without forti-hole's marker ({}); rename them or change \"prefix\"", config.prefix,
                                        join(inventory.collisions)),
                            6);

        // Categories: one per group, stable across runs.
        std::set<int> taken = inventory.foreign_categories;
        std::set<std::string, std::less<>> labels;
        for (const auto& feed: result.feeds) labels.insert(feed.group);
        for (const auto& label: labels)
            if (auto it = state.groups.find(label); it != state.groups.end() && it->second.category >= config.feeds.category_min &&
                                                    it->second.category <= config.feeds.category_max && !taken.contains(it->second.category)) {
                plan.categories[label] = it->second.category;
                taken.insert(it->second.category);
            }
        // A group whose feeds are already on the device keeps their category even when state.json
        // does not know it (a run that stopped before saving): recategorizing them would leave the
        // profiles listing a category no feed provides, which FortiOS refuses.
        for (const auto& feed: inventory.feeds) {
            if (!feed.owned || !feed.name.starts_with(config.prefix)) continue;
            std::string_view rest = std::string_view(feed.name).substr(config.prefix.size());
            auto dash = rest.rfind('-');
            if (dash == std::string_view::npos) continue;
            std::string label(rest.substr(0, dash));
            if (!labels.contains(label) || plan.categories.contains(label) || taken.contains(feed.category)) continue;
            if (feed.category < config.feeds.category_min || feed.category > config.feeds.category_max) continue;
            plan.categories[label] = feed.category;
            taken.insert(feed.category);
        }
        // Categories still on the device for groups that are going away stay reserved until phase 3.
        std::set<int> ours_anywhere;
        for (const auto& [label, group]: state.groups) ours_anywhere.insert(group.category);
        for (const auto& feed: inventory.feeds)
            if (feed.owned && feed.category >= 192 && feed.category <= 221) ours_anywhere.insert(feed.category);
        for (int category: ours_anywhere) taken.insert(category);
        for (const auto& label: labels) {
            if (plan.categories.contains(label)) continue;
            int found = 0;
            for (int c = config.feeds.category_min; c <= config.feeds.category_max && !found; ++c)
                if (!taken.contains(c)) found = c;
            if (!found)
                throw SyncError(std::format("no free remote category in {}-{} for group {} (operator feeds use {})", config.feeds.category_min,
                                            config.feeds.category_max, label, inventory.foreign_categories.size()),
                                6);
            plan.categories[label] = found;
            taken.insert(found);
        }

        // Feeds.
        std::set<std::string, std::less<>> compiled_names;
        for (const auto& feed: result.feeds) {
            compiled_names.insert(feed.name);
            FeedAction action;
            action.feed = feed.name;
            action.group = feed.group;
            action.category = plan.categories.at(feed.group);
            action.profiles = feed.profiles;
            action.lines_after = feed.lines.size();
            action.compiled = &feed;
            plan.lines_after += feed.lines.size();
            const DeviceFeed* device = inventory.feed(feed.name);
            std::string comment = ownership_comment(std::format("group {}, profiles {}", feed.group, join(feed.profiles, " ")));
            if (!device) {
                action.kind = FeedAction::Kind::create;
                action.reason = "new feed";
                plan.phase1.push_back({{"op", "create"},
                                       {"resource", "system/external-resource"},
                                       {"object",
                                        {{"name", feed.name},
                                         {"type", "domain"},
                                         {"update-method", "push"},
                                         {"category", action.category},
                                         {"comments", comment},
                                         {"status", "enable"}}}});
                plan.snapshot_lines += feed.lines.size();
                plan.feeds.push_back(std::move(action));
                continue;
            }
            if (device->type != "domain" || device->update_method != "push")
                throw SyncError(std::format("feed {} is forti-hole's but is now {} / {}; delete it to let forti-hole recreate it", feed.name,
                                            device->type, device->update_method),
                                6);
            json set = json::object();
            if (device->category != action.category) {
                set["category"] = action.category;
                action.category_changes = true;
            }
            if (device->comments != comment) set["comments"] = comment;
            if (device->status == "disable") set["status"] = "enable";
            if (!set.empty()) plan.phase1.push_back({{"op", "update"}, {"resource", "system/external-resource"}, {"key", feed.name}, {"set", set}});

            const FeedStatus* status = device->state ? &*device->state : nullptr;
            action.lines_device = status ? status->accepted : 0;
            action.last_update_before = status ? status->last_update : 0;
            auto recorded = state.feeds.find(feed.name);
            std::optional<std::vector<std::string>> baseline;
            if (recorded == state.feeds.end() || !recorded->second.confirmed) {
                action.reason = "no confirmed contents on record";
            } else if (!status || !status->loaded) {
                action.reason = "the device has not loaded the feed";
            } else if (status->accepted != recorded->second.lines || status->overflow) {
                action.reason = std::format("the device holds {} entries, {} were confirmed", status->accepted, recorded->second.lines);
            } else if (status->last_update != recorded->second.device_update) {
                action.reason = "the feed changed on the device since it was confirmed";
            } else if (!(baseline = load_feed_lines(home, feed.name, recorded->second.sha256))) {
                action.reason = "the confirmed contents file is missing or damaged";
            }
            if (baseline && recorded->second.sha256 == feed.sha256) {
                action.kind = FeedAction::Kind::unchanged;
                action.reason = "unchanged";
                plan.feeds.push_back(std::move(action));
                continue;
            }
            if (baseline) {
                std::set_difference(feed.lines.begin(), feed.lines.end(), baseline->begin(), baseline->end(), std::back_inserter(action.adds));
                std::set_difference(baseline->begin(), baseline->end(), feed.lines.begin(), feed.lines.end(), std::back_inserter(action.removes));
                if (action.adds.size() + action.removes.size() < feed.lines.size()) {
                    action.kind = FeedAction::Kind::delta;
                    action.reason = std::format("{} added, {} removed", action.adds.size(), action.removes.size());
                    plan.adds += action.adds.size();
                    plan.removes += action.removes.size();
                    plan.feeds.push_back(std::move(action));
                    continue;
                }
                action.adds.clear();
                action.removes.clear();
                action.reason = "the change is larger than the feed";
            }
            action.kind = FeedAction::Kind::snapshot;
            plan.snapshot_lines += feed.lines.size();
            plan.feeds.push_back(std::move(action));
        }
        for (const auto& device: inventory.feeds) {
            if (!device.owned || compiled_names.contains(device.name)) continue;
            FeedAction action;
            action.kind = FeedAction::Kind::remove;
            action.feed = device.name;
            action.category = device.category;
            action.lines_device = device.state ? device.state->accepted : 0;
            action.reason = "no longer in the compiled set";
            plan.feeds.push_back(std::move(action));
            plan.phase3.push_back({{"op", "delete"}, {"resource", "system/external-resource"}, {"key", device.name}});
        }

        // The categories some feed provides once phase 1 has run: every compiled group's, and those of
        // feeds that are only deleted in phase 3. A profile may list no other remote category.
        std::set<int> provided;
        for (const auto& [label, category]: plan.categories) provided.insert(category);
        for (const auto& action: plan.feeds)
            if (action.kind == FeedAction::Kind::remove) provided.insert(action.category);

        // Managed DNS filter profiles.
        if (config.dnsfilter.manage_profiles) {
            for (std::size_t p = 0; p < config.profiles.size(); ++p) {
                const auto& spec = config.profiles[p];
                std::string name = dnsfilter_profile_name(config, spec.name);
                std::set<int> wanted;
                for (const auto& feed: result.feeds)
                    if (feed.profile_mask & (1u << p)) wanted.insert(plan.categories.at(feed.group));
                ProfileAction action;
                action.name = name;
                action.categories.assign(wanted.begin(), wanted.end());
                const DeviceProfile* device = inventory.profile(name);
                std::set<int> now;
                if (device)
                    for (const auto& [category, verb]: device->remote_categories)
                        if (ours_anywhere.contains(category) && verb == "block" && provided.contains(category)) now.insert(category);
                // Phase 1 keeps what it blocks now (of forti-hole's) and adds the new; phase 3 is exact.
                std::set<int> transitional = wanted;
                transitional.insert(now.begin(), now.end());
                std::vector<int> first(transitional.begin(), transitional.end());
                json ftgd1 = {{"options", config.dnsfilter.error_allow ? "error-allow" : ""}, {"filters", filters_for(first, config.dnsfilter.log)}};
                json ftgd3 = {{"options", config.dnsfilter.error_allow ? "error-allow" : ""}, {"filters", filters_for(action.categories, config.dnsfilter.log)}};
                std::string comment = ownership_comment(std::format("profile {}: tiers {}", spec.name, join(spec.tiers, " ")));
                if (!device) {
                    action.kind = ProfileAction::Kind::create;
                    plan.phase1.push_back({{"op", "create"},
                                           {"resource", "dnsfilter/profile"},
                                           {"object", {{"name", name}, {"comment", comment}, {"block-action", config.dnsfilter.block_action}, {"ftgd-dns", ftgd1}}}});
                    if (first != action.categories)
                        plan.phase3.push_back({{"op", "update"}, {"resource", "dnsfilter/profile"}, {"key", name}, {"set", {{"ftgd-dns", ftgd3}}}});
                    plan.profiles.push_back(std::move(action));
                    continue;
                }
                if (!device->owned)
                    throw SyncError(std::format("DNS filter profile {} exists without forti-hole's marker; rename it or change \"prefix\"", name), 6);
                std::string options = device->ftgd_dns.is_object() && device->ftgd_dns.contains("options") && device->ftgd_dns["options"].is_string() ? device->ftgd_dns["options"].get<std::string>() : std::string();
                bool options_differ = options != (config.dnsfilter.error_allow ? "error-allow" : "");
                bool other_filters = false;
                json device_filters = device->ftgd_dns.contains("filters") && device->ftgd_dns["filters"].is_array() ? device->ftgd_dns["filters"] : json::array();
                for (const auto& f: device_filters)
                    if (!ours_anywhere.contains(f.value("category", 0))) other_filters = true;
                json set1 = json::object();
                if (device->comment != comment) set1["comment"] = comment;
                if (device->block_action != config.dnsfilter.block_action) set1["block-action"] = config.dnsfilter.block_action;
                if (options_differ || other_filters || filter_set(device_filters) != filter_set(ftgd1["filters"]))
                    set1["ftgd-dns"] = ftgd1;
                if (!set1.empty()) {
                    action.kind = ProfileAction::Kind::update;
                    action.phase1 = set1;
                    plan.phase1.push_back({{"op", "update"}, {"resource", "dnsfilter/profile"}, {"key", name}, {"set", set1}});
                }
                if (first != action.categories) {
                    action.kind = ProfileAction::Kind::update;
                    action.phase3 = {{"ftgd-dns", ftgd3}};
                    plan.phase3.push_back({{"op", "update"}, {"resource", "dnsfilter/profile"}, {"key", name}, {"set", action.phase3}});
                }
                plan.profiles.push_back(std::move(action));
            }
        }

        // Pushes: per feed a queue of requests (a snapshot; or additions, then removals, in chunks);
        // round r takes the r-th request of every queue. New feeds come first.
        std::vector<std::vector<json>> queues;
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& action: plan.feeds) {
                // Pass 0: new feeds. Pass 1: snapshots of existing feeds and deltas.
                bool wanted = pass == 0 ? action.kind == FeedAction::Kind::create
                                        : action.kind == FeedAction::Kind::snapshot || action.kind == FeedAction::Kind::delta;
                if (!wanted) continue;
                bool snapshot = action.kind != FeedAction::Kind::delta;
                std::vector<json> queue;
                if (snapshot) {
                    queue.push_back(push_op(action.feed, "snapshot", action.compiled->lines.begin(), action.compiled->lines.end()));
                } else {
                    json requests = json::array();
                    push_chunks(requests, action.feed, "add", action.adds, config.sync.chunk_entries);
                    push_chunks(requests, action.feed, "remove", action.removes, config.sync.chunk_entries);
                    for (auto& request: requests) queue.push_back(std::move(request));
                }
                if (!queue.empty()) queues.push_back(std::move(queue));
            }
        for (std::size_t round = 0;; ++round) {
            json ops = json::array();
            for (const auto& queue: queues)
                if (round < queue.size()) ops.push_back(queue[round]);
            if (ops.empty()) break;
            plan.pushes += ops.size();
            plan.push_rounds.push_back(std::move(ops));
        }

        // Profiles must stop blocking a category before its last feed is deleted: phase 3 lists the
        // profile updates first.
        std::stable_partition(plan.phase3.begin(), plan.phase3.end(), [](const json& op) { return op.value("op", "") == "update"; });
        if (!result.exceptions.empty())
            plan.notes.push_back(std::format("{} exception(s) sit below blocked subtrees and need a static domain filter: forti-hole status lists them",
                                             result.exceptions.size()));
        return plan;
    }

    json apply_operations(Gateway& gateway, const Config& config, const json& operations, const std::shared_ptr<WritePermits>& permits,
                          const WritePermits& granted, std::string* plan_id) {
        check_operations(operations, config.prefix, granted);
        PermitScope scope(permits, granted);
        auto planned = gateway.plan(with_target(config, {{"operations", operations}}));
        if (!planned.value("ok", false)) {
            const auto& error = planned["error"];
            throw SyncError(std::format("planning failed: {}", error.value("message", std::string("?"))), 6);
        }
        std::string id = planned["data"].value("plan_id", std::string());
        if (id.empty()) id = planned["data"].value("id", std::string());
        if (plan_id) *plan_id = id;
        auto applied = gateway.apply({{"plan_id", id}});
        if (applied.value("ok", false)) return applied["data"];
        const auto& error = applied["error"];
        std::string code = error.value("code", std::string("internal")), message = error.value("message", std::string("?"));
        // The engine's per-operation record says which operation failed and what the device answered.
        if (error.contains("details") && error["details"].is_object()) {
            const auto& details = error["details"];
            for (const auto& record: details.value("operations", json::array())) {
                if (!record.is_object() || record.value("status", std::string()) != "failed") continue;
                std::string why;
                if (record.contains("error") && record["error"].is_object()) why = record["error"].value("message", std::string());
                message += std::format(" (operation {} failed{}{})", record.value("index", 0), why.empty() ? "" : ": ", why);
                break;
            }
        }
        if (code == "approval_required")
            throw SyncError(std::format("waiting for approval of plan {}: run `forti approve {}`, then sync again", id, id), 7);
        if (code == "policy_denied") throw SyncError(std::format("the forti profile does not allow changes ({}); plan {} was not applied", message, id), 7);
        if (code == "stale_plan") throw SyncError(std::format("the device changed while planning ({}); sync again", message), 7);
        if (code == "partial_failure" || code == "outcome_unknown" || code == "management_connectivity_lost")
            throw SyncError(std::format("plan {} stopped part-way: {}", id, message), 8);
        throw SyncError(std::format("plan {} failed: {}", id, message), code == "connection" || code == "auth" ? 4 : 6);
    }

    FeedVerification verify_feed(Gateway& gateway, const Config& config, std::string_view feed, const std::vector<std::string>& expected,
                                 std::int64_t updated_after, const Clock& clock, bool full) {
        FeedVerification v;
        v.feed = std::string(feed);
        v.expected = expected.size();
        auto deadline = clock.now() + config.sync.load_timeout_seconds;
        auto settle = clock.now() + 10;
        FeedStatus status;
        while (true) {
            status = gateway.feed_status(feed);
            bool counts = status.loaded && status.accepted == expected.size() && status.invalid == 0;
            if (counts && (status.last_update > updated_after || clock.now() >= settle)) break;
            if (clock.now() >= deadline) break;
            clock.sleep(2000);
        }
        v.accepted = status.accepted;
        v.valid = status.valid;
        v.invalid = status.invalid;
        v.overflow = status.overflow;
        v.last_update = status.last_update;
        if (!status.loaded) v.problem = std::format("not loaded: {}", status.error);
        else if (status.overflow) v.problem = "the device's entry limit truncated it (overflow)";
        else if (status.accepted != expected.size())
            v.problem = std::format("the device accepted {} entries, {} were pushed", status.accepted, expected.size());
        else if (status.invalid != 0) v.problem = std::format("the device marked {} entries invalid", status.invalid);
        if (!v.problem.empty()) return v;

        // Entry comparison: every entry the device returns must be one of the compiled lines.
        auto check_page = [&](std::size_t start, std::size_t count) {
            for (const auto& entry: gateway.feed_entries(feed, start, count)) {
                ++v.sampled;
                if (!entry.valid || !std::binary_search(expected.begin(), expected.end(), entry.entry)) ++v.sample_mismatches;
            }
        };
        if (expected.empty()) {
            v.verified = true;
            return v;
        }
        std::size_t pages = std::max<std::size_t>(1, config.sync.verify_samples);
        std::size_t page = std::max<std::size_t>(1, config.sync.verify_page);
        // A feed no larger than the sample is read whole; so is every feed with `full`.
        if (full || expected.size() <= pages * page) {
            for (std::size_t start = 0; start < expected.size(); start += 5000) check_page(start, 5000);
            if (v.sampled != expected.size() && v.problem.empty())
                v.problem = std::format("read back {} entries, expected {}", v.sampled, expected.size());
        } else {
            for (std::size_t i = 0; i < pages; ++i) check_page((expected.size() - page) * i / std::max<std::size_t>(1, pages - 1), page);
        }
        if (v.sample_mismatches > 0) v.problem = std::format("{} of {} sampled entries are not in the compiled feed", v.sample_mismatches, v.sampled);
        v.verified = v.problem.empty();
        return v;
    }

    SyncOutcome execute_sync(Gateway& gateway, const Config& config, State& state, const Home& home, const Inventory& inventory,
                             const SyncPlan& plan, std::shared_ptr<WritePermits> permits, const Clock& clock,
                             const std::function<void(const std::string&)>& progress) {
        SyncOutcome outcome;
        auto say = [&](const std::string& line) {
            if (progress) progress(line);
        };

        Journal journal;
        journal.started_at = now_iso8601();
        journal.phase = "cmdb";
        for (const auto& action: plan.feeds)
            if (action.kind != FeedAction::Kind::unchanged) journal.feeds.push_back(action.feed);
        if (!plan.writes()) return outcome;
        save_journal(home, journal);

        auto record_plan = [&](const std::string& id) {
            if (id.empty()) return;
            outcome.plan_ids.push_back(id);
            journal.plan_ids.push_back(id);
            save_journal(home, journal);
        };

        try {
            // Phase 1: feeds and profiles.
            if (!plan.phase1.empty()) {
                say(std::format("Applying {} configuration change(s)...", plan.phase1.size()));
                std::string id;
                try {
                    apply_operations(gateway, config, plan.phase1, permits, {}, &id);
                } catch (...) {
                    record_plan(id);
                    throw;
                }
                record_plan(id);
            }

            // New feeds load asynchronously; pushes before that are dropped by FortiOS.
            for (const auto& action: plan.feeds) {
                if (action.kind != FeedAction::Kind::create) continue;
                auto deadline = clock.now() + config.sync.load_timeout_seconds;
                while (true) {
                    FeedStatus status = gateway.feed_status(action.feed);
                    if (status.loaded) break;
                    if (clock.now() >= deadline)
                        throw SyncError(std::format("FortiOS did not load the new feed {} within {} s ({})", action.feed,
                                                    config.sync.load_timeout_seconds, status.error),
                                        8);
                    clock.sleep(2000);
                }
            }

            // Phase 2: pushes, round by round.
            if (plan.pushes > 0) {
                journal.phase = "push";
                save_journal(home, journal);
                say(std::format("Pushing {} request(s) in {} round(s): {} snapshot lines, {} additions, {} removals...", plan.pushes,
                                plan.push_rounds.size(), plan.snapshot_lines, plan.adds, plan.removes));
                std::map<std::string, std::int64_t, std::less<>> updated;   // feed -> last_content_update_time seen
                for (const auto& action: plan.feeds) updated[action.feed] = action.last_update_before;
                for (std::size_t r = 0; r < plan.push_rounds.size(); ++r) {
                    const auto& round = plan.push_rounds[r];
                    if (r > 0) {
                        // The previous round's feeds must have applied before they are pushed again.
                        for (const auto& op: plan.push_rounds[r - 1]) {
                            std::string feed = op["body"]["commands"][0]["name"];
                            auto deadline = clock.now() + config.sync.load_timeout_seconds;
                            while (true) {
                                FeedStatus status = gateway.feed_status(feed);
                                if (status.last_update > updated[feed]) {
                                    updated[feed] = status.last_update;
                                    break;
                                }
                                if (clock.now() >= deadline) break;   // verification decides
                                clock.sleep(1000);
                            }
                        }
                    }
                    std::string id;
                    try {
                        apply_operations(gateway, config, round, permits, {}, &id);
                    } catch (...) {
                        record_plan(id);
                        throw;
                    }
                    record_plan(id);
                }
            }

            // Phase 3 (verification) of every feed that was pushed.
            journal.phase = "verify";
            save_journal(home, journal);
            std::uint64_t lines_on_device = 0;
            for (const auto& action: plan.feeds) {
                if (action.kind == FeedAction::Kind::remove || !action.compiled) continue;
                auto& record = state.feeds[action.feed];
                record.group = action.group;
                record.category = action.category;
                if (action.kind == FeedAction::Kind::unchanged) {
                    lines_on_device += record.lines;
                    continue;
                }
                say(std::format("Verifying {} on the device...", action.feed));
                auto v = verify_feed(gateway, config, action.feed, action.compiled->lines, action.last_update_before, clock);
                if (v.verified) {
                    save_feed_lines(home, action.feed, action.compiled->lines);
                    record.confirmed = true;
                    record.sha256 = action.compiled->sha256;
                    record.lines = action.compiled->lines.size();
                    record.device_update = v.last_update;
                    record.confirmed_at = now_iso8601();
                    lines_on_device += record.lines;
                } else {
                    record.confirmed = false;
                    outcome.problems.push_back(std::format("{}: {}", action.feed, v.problem));
                    lines_on_device += v.accepted;
                }
                outcome.verified.push_back(std::move(v));
            }
            save_state(home, state);

            // What the push cost in device memory (only a large enough growth is a measurement).
            if (inventory.facts.memory_used && plan.lines_after > inventory.facts.owned_entries) {
                try {
                    auto performance = expect_ok(gateway.read(with_target(config, {{"resource", "monitor/system/performance/status"}})));
                    auto used_after = performance.value("mem", json::object()).value("used", std::uint64_t(0));
                    if (auto measured = measure_bytes_per_line(*inventory.facts.memory_used, used_after, inventory.facts.owned_entries, lines_on_device)) {
                        Measurement m{now_iso8601(), inventory.facts.owned_entries, lines_on_device, *inventory.facts.memory_used, used_after, *measured};
                        state.bytes_per_line = learn_bytes_per_line(state.bytes_per_line, *measured);
                        state.measurements.push_back(m);
                        if (state.measurements.size() > 10) state.measurements.erase(state.measurements.begin());
                        outcome.measurement = m;
                    }
                } catch (const GatewayError&) {
                    // a measurement is a bonus
                }
            }

            // Phase 4: final profile categories and obsolete feeds, only when every push verified:
            // a category is never unblocked while the feeds replacing it are in doubt.
            if (!plan.phase3.empty()) {
                if (!outcome.problems.empty()) {
                    outcome.problems.push_back("left obsolete feeds and categories in place until the next sync verifies");
                } else {
                    journal.phase = "cleanup";
                    save_journal(home, journal);
                    say(std::format("Applying {} cleanup change(s)...", plan.phase3.size()));
                    std::string id;
                    try {
                        apply_operations(gateway, config, plan.phase3, permits, {}, &id);
                    } catch (...) {
                        record_plan(id);
                        throw;
                    }
                    record_plan(id);
                    for (const auto& action: plan.feeds)
                        if (action.kind == FeedAction::Kind::remove) {
                            state.feeds.erase(action.feed);
                            remove_feed_lines(home, action.feed);
                            outcome.deleted.push_back(action.feed);
                        }
                }
            }

            // Groups that still have feeds keep their category; the rest are released.
            std::map<std::string, GroupState, std::less<>> groups;
            for (const auto& [label, category]: plan.categories) groups[label] = {category, 1};
            for (const auto& action: plan.feeds)
                if (action.compiled) groups[action.group].partitions = action.compiled->partitions;
            if (outcome.problems.empty()) state.groups = std::move(groups);
            else
                for (auto& [label, group]: groups) state.groups[label] = group;
            save_state(home, state);
            if (outcome.problems.empty()) clear_journal(home);
            outcome.exit_code = outcome.problems.empty() ? 0 : 8;
        } catch (const SyncError& error) {
            // Whatever was pushed is in doubt: the journal stays, and these feeds are re-snapshotted.
            for (const auto& name: journal.feeds)
                if (auto it = state.feeds.find(name); it != state.feeds.end()) it->second.confirmed = false;
            save_state(home, state);
            if (error.exit_code() == 7 && journal.phase == "cmdb") clear_journal(home);   // nothing was written
            throw;
        }
        return outcome;
    }

}  // namespace fh
