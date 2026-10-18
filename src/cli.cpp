#include "cli.hpp"

#include "config.hpp"
#include "device.hpp"
#include "fetch/https.hpp"
#include "forti_hole_version.hpp"
#include "legacy.hpp"
#include "manage.hpp"
#include "pipeline.hpp"
#include "report.hpp"
#include "sources.hpp"

#include <forti/engine/profile.hpp>

#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <map>
#include <set>
#include <thread>

#include <termios.h>
#include <unistd.h>

namespace fh {

    using json = nlohmann::json;

    namespace {

        class UsageError : public std::runtime_error {
        public:
            using std::runtime_error::runtime_error;
        };

        // Option names that take a value; every other --name is a flag.
        const std::set<std::string, std::less<>> valued = {"config", "profile", "write", "policy", "url", "name", "ca-bundle", "prefix"};
        const std::set<std::string, std::less<>> flags = {"json",  "quiet", "offline", "device", "lines",          "force", "yes",
                                                          "verify", "full", "apply",   "help",   "force-snapshot", "version"};

        struct Args {
            std::vector<std::string> words;   // command words and positional arguments
            std::map<std::string, std::string, std::less<>> values;
            std::set<std::string, std::less<>> set;

            [[nodiscard]] bool has(std::string_view flag) const { return set.contains(flag); }
            [[nodiscard]] std::optional<std::string> value(std::string_view name) const {
                auto it = values.find(name);
                return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
            }
        };

        Args parse_args(const std::vector<std::string>& argv) {
            Args args;
            for (std::size_t i = 0; i < argv.size(); ++i) {
                const std::string& a = argv[i];
                if (a == "-h") {
                    args.set.insert("help");
                    continue;
                }
                if (a == "-q") {
                    args.set.insert("quiet");
                    continue;
                }
                if (!a.starts_with("--")) {
                    args.words.push_back(a);
                    continue;
                }
                std::string name = a.substr(2), value;
                bool inline_value = false;
                if (auto eq = name.find('='); eq != std::string::npos) {
                    value = name.substr(eq + 1);
                    name = name.substr(0, eq);
                    inline_value = true;
                }
                if (valued.contains(name)) {
                    if (!inline_value) {
                        if (i + 1 >= argv.size()) throw UsageError(std::format("--{} needs a value", name));
                        value = argv[++i];
                    }
                    args.values[name] = value;
                } else if (flags.contains(name)) {
                    if (inline_value) throw UsageError(std::format("--{} takes no value", name));
                    args.set.insert(name);
                } else {
                    throw UsageError(std::format("unknown option --{}", name));
                }
            }
            return args;
        }

        struct Context {
            Environment& env;
            Args args;
            bool json_mode = false, quiet = false;
            std::optional<Config> config;
            std::filesystem::path config_path;

            std::ostream& out() { return *env.out; }
            std::ostream& err() { return *env.err; }

            Config& load() {
                if (!config) {
                    config = load_config(config_path);
                    if (auto profile = args.value("profile")) config->profile = *profile;
                }
                return *config;
            }

            void progress(const std::string& line) {
                if (!json_mode && !quiet) err() << line << "\n";
            }

            // Prints the data (JSON mode) or the text.
            int emit(const json& data, const std::string& text, int exit = 0) {
                if (json_mode) out() << json{{"ok", true}, {"data", data}}.dump(2) << "\n";
                else if (!quiet || exit != 0) out() << text;
                return exit;
            }
        };

        std::unique_ptr<Fetcher> make_fetcher(Context& ctx, const Config& config) {
            FetchOptions options;
            options.timeout_seconds = config.fetch.timeout_seconds;
            options.max_bytes = config.fetch.max_bytes;
            options.parallel = config.fetch.parallel;
            options.ca_bundle = config.fetch.ca_bundle;
            options.user_agent = std::string("forti-hole/") + FORTI_HOLE_VERSION;
            return ctx.env.fetcher(options);
        }

        // Device facts from the last sync, for compiles that do not read the device.
        DeviceFacts remembered_facts(const State& state) {
            DeviceFacts facts;
            facts.entry_limit = state.entry_limit;
            for (const auto& [name, feed]: state.feeds)
                if (feed.confirmed) facts.owned_entries += feed.lines;
            return facts;
        }

        std::string version_line() { return std::format("forti-hole {} ({})", FORTI_HOLE_VERSION, FORTI_HOLE_BUILT_FROM); }

        // ---- setup ----

        int cmd_setup(Context& ctx) {
            std::error_code ignored;
            if (std::filesystem::exists(ctx.config_path, ignored) && !ctx.args.has("force"))
                throw ConfigError(std::format("{} exists; edit it, or pass --force to start over", ctx.config_path.string()));
            json config = example_config();
            if (auto profile = ctx.args.value("profile")) config["profile"] = *profile;
            Config parsed = parse_config(config);
            json data = {{"config", ctx.config_path.string()}, {"enabled", false}};
            std::string text = std::format("Wrote {} (\"enabled\": false: nothing is written to the FortiGate yet).\n", ctx.config_path.string());
            if (!ctx.args.has("offline")) {
                auto permits = std::make_shared<WritePermits>();
                auto gateway = ctx.env.gateway(parsed, permits);
                Inventory inventory = read_inventory(*gateway, parsed);
                data["device"] = device_json(inventory);
                text += device_text(inventory);
                if (!inventory.collisions.empty()) text += "warning: objects named fh-* exist without forti-hole's marker; set another \"prefix\"\n";
            }
            write_private(ctx.config_path, config.dump(2) + "\n");
            text += "\nNext:\n  forti-hole compile --device   what the lists compile to, and whether it fits this device\n"
                    "  forti-hole plan               every change a sync would make\n"
                    "  set \"enabled\": true in the config, then: forti-hole sync\n";
            return ctx.emit(data, text);
        }

        // ---- compile ----

        int cmd_compile(Context& ctx) {
            Config& config = ctx.load();
            State state = load_state(ctx.env.home);
            auto fetcher = make_fetcher(ctx, config);
            LoadOptions options;
            options.offline = ctx.args.has("offline");
            ctx.progress(options.offline ? "Reading cached lists..." : "Fetching lists...");
            auto lists = load_lists(config, ctx.env.home, state, *fetcher, options);
            save_state(ctx.env.home, state);
            DeviceFacts facts = remembered_facts(state);
            std::string device_text_part;
            json device = nullptr;
            if (ctx.args.has("device")) {
                auto gateway = ctx.env.gateway(config, std::make_shared<WritePermits>());
                Inventory inventory = read_inventory(*gateway, config);
                facts = inventory.facts;
                device = device_json(inventory);
                device_text_part = device_text(inventory) + "\n";
            }
            ctx.progress("Compiling...");
            CompileRun run = compile_lists(config, lists, state, facts);
            if (auto dir = ctx.args.value("write")) {
                std::filesystem::path out = *dir;
                for (const auto& feed: run.result.feeds) {
                    std::string text;
                    for (const auto& line: feed.lines) text += line + "\n";
                    write_private(out / (feed.name + ".txt"), text);
                }
                write_private(out / "report.json", compile_json(run).dump(2) + "\n");
            }
            json data = compile_json(run, ctx.args.has("lines"));
            data["lists"] = lists_json(lists);
            if (!device.is_null()) data["device"] = device;
            std::string text = lists_text(lists) + "\n" + device_text_part + compile_text(run);
            return ctx.emit(data, text, run.fits() ? 0 : 9);
        }

        // ---- plan / sync ----

        struct Prepared {
            State state;
            std::map<std::string, LoadedList, std::less<>> lists;
            Inventory inventory;
            CompileRun run;
            SyncPlan plan;
        };

        Prepared prepare(Context& ctx, Gateway& gateway, bool writing) {
            Config& config = ctx.load();
            Prepared p;
            p.state = load_state(ctx.env.home);
            if (writing) {
                if (auto journal = load_journal(ctx.env.home)) {
                    ctx.progress(std::format("The previous sync stopped in its {} phase; its feeds will be re-sent in full.", journal->phase));
                    if (journal->feeds.empty())
                        for (auto& [name, feed]: p.state.feeds) feed.confirmed = false;
                    for (const auto& name: journal->feeds)
                        if (auto it = p.state.feeds.find(name); it != p.state.feeds.end()) it->second.confirmed = false;
                }
                if (ctx.args.has("force-snapshot"))
                    for (auto& [name, feed]: p.state.feeds) feed.confirmed = false;
            }
            auto fetcher = make_fetcher(ctx, config);
            ctx.progress("Fetching lists...");
            p.lists = load_lists(config, ctx.env.home, p.state, *fetcher);
            ctx.progress("Reading the FortiGate...");
            p.inventory = read_inventory(gateway, config);
            if (!p.state.device_serial.empty() && p.state.device_serial != p.inventory.serial) {
                ctx.progress(std::format("The device is {} now, not {}: every feed is re-sent in full.", p.inventory.serial, p.state.device_serial));
                for (auto& [name, feed]: p.state.feeds) feed.confirmed = false;
            }
            ctx.progress("Compiling...");
            p.run = compile_lists(config, p.lists, p.state, p.inventory.facts);
            p.plan = plan_sync(config, p.state, ctx.env.home, p.inventory, p.run);
            return p;
        }

        int cmd_plan(Context& ctx) {
            Config& config = ctx.load();
            auto gateway = ctx.env.gateway(config, std::make_shared<WritePermits>());
            Prepared p = prepare(ctx, *gateway, false);
            save_state(ctx.env.home, p.state);   // source cache metadata only
            json data = {{"compile", compile_json(p.run)}, {"plan", plan_json(p.plan)}, {"lists", lists_json(p.lists)}, {"device", device_json(p.inventory)}};
            std::string text = lists_text(p.lists) + "\n" + device_text(p.inventory) + "\n" + compile_text(p.run) + "\n" + plan_text(p.plan);
            if (!p.run.fits())
                text += std::format("\nA sync would refuse this set: {}\n", p.run.budget.reason());
            return ctx.emit(data, text, p.run.fits() ? 0 : 9);
        }

        int cmd_sync(Context& ctx) {
            Config& config = ctx.load();
            if (!config.enabled)
                return ctx.emit({{"skipped", "disabled"}},
                                std::format("forti-hole is not enabled: review `forti-hole plan`, then set \"enabled\": true in {}\n", config.path.string()), 7);
            State peek = load_state(ctx.env.home);
            if (peek.paused) return ctx.emit({{"skipped", "paused"}}, std::format("forti-hole is paused (since {}); `forti-hole resume`\n", peek.paused_at));
            RunLock lock(ctx.env.home.lock());
            auto permits = std::make_shared<WritePermits>();
            auto gateway = ctx.env.gateway(config, permits);
            Prepared p = prepare(ctx, *gateway, true);
            auto started = std::chrono::steady_clock::now();
            json history = {{"at", now_iso8601()}, {"command", "sync"}, {"lines", p.run.result.stats.lines}, {"selected", p.run.selected}};
            if (!p.run.fits()) {
                std::string why = p.run.budget.reason();
                if (config.budget.refuse) {
                    save_state(ctx.env.home, p.state);
                    history["result"] = "refused";
                    history["reason"] = why;
                    append_history(ctx.env.home.history(), history);
                    return ctx.emit({{"compile", compile_json(p.run)}, {"refused", why}},
                                    compile_text(p.run) + std::format("\nNothing was written: {}\n", why), 9);
                }
                ctx.progress("warning: " + why + " (budget.on_overrun is \"warn\": syncing anyway)");
            }
            p.state.device_serial = p.inventory.serial;
            if (p.inventory.facts.entry_limit) p.state.entry_limit = p.inventory.facts.entry_limit;
            p.state.selected = p.run.selected;
            SyncOutcome outcome;
            int exit = 0;
            std::string failure;
            try {
                outcome = execute_sync(*gateway, config, p.state, ctx.env.home, p.inventory, p.plan, permits, ctx.env.clock,
                                       [&](const std::string& line) { ctx.progress(line); });
                exit = outcome.exit_code;
            } catch (const SyncError& error) {
                exit = error.exit_code();
                failure = error.what();
            }
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            p.state.last_run = {{"at", now_iso8601()},
                                {"lines", p.run.result.stats.lines},
                                {"names", p.run.result.stats.names},
                                {"writes", p.plan.writes()},
                                {"exit", exit},
                                {"budget", budget_json(p.run.budget)}};
            save_state(ctx.env.home, p.state);
            history["result"] = exit == 0 ? (p.plan.writes() ? "synced" : "unchanged") : "failed";
            history["exit"] = exit;
            history["plan_ids"] = outcome.plan_ids;
            history["seconds"] = seconds;
            if (!failure.empty()) history["error"] = failure;
            append_history(ctx.env.home.history(), history);

            json data = {{"compile", compile_json(p.run)}, {"plan", plan_json(p.plan)}, {"outcome", outcome_json(outcome)}, {"seconds", seconds}};
            if (!failure.empty()) data["error"] = failure;
            std::string text = std::format("{} lines in {} feeds ({} unique names); ", grouped(p.run.result.stats.lines), p.run.result.feeds.size(),
                                           grouped(p.run.result.stats.names));
            for (const auto& [source, variant]: p.run.selected) text += std::format("{} {}; ", source, variant);
            text += "\n" + plan_text(p.plan) + outcome_text(outcome);
            if (!failure.empty()) text += "error: " + failure + "\n";
            for (const auto& w: p.run.warnings) text += "warning: " + w + "\n";
            text += std::format("Done in {:.1f} s.\n", seconds);
            if (ctx.json_mode && !failure.empty()) {
                ctx.out() << json{{"ok", false}, {"error", {{"code", "sync"}, {"message", failure}}}, {"data", data}}.dump(2) << "\n";
                return exit;
            }
            return ctx.emit(data, text, exit);
        }

        // ---- status ----

        int cmd_status(Context& ctx) {
            Config& config = ctx.load();
            State state = load_state(ctx.env.home);
            auto gateway = ctx.env.gateway(config, std::make_shared<WritePermits>());
            Inventory inventory = read_inventory(*gateway, config);
            bool verify = ctx.args.has("verify"), full = ctx.args.has("full");
            json feeds = json::array();
            std::string text = device_text(inventory) + "\nforti-hole feeds\n";
            int exit = 0;
            for (const auto& feed: inventory.feeds) {
                if (!feed.owned) continue;
                auto record = state.feeds.find(feed.name);
                json j = {{"name", feed.name}, {"category", feed.category}};
                std::string verdict;
                std::uint64_t accepted = feed.state ? feed.state->accepted : 0;
                j["device_entries"] = accepted;
                if (record == state.feeds.end() || !record->second.confirmed) {
                    verdict = "unconfirmed (next sync sends it in full)";
                } else if (!feed.state || accepted != record->second.lines || feed.state->last_update != record->second.device_update) {
                    verdict = std::format("differs from the confirmed {} entries", grouped(record->second.lines));
                    exit = 8;
                } else {
                    verdict = "matches the confirmed contents";
                }
                if (verify && record != state.feeds.end()) {
                    auto lines = load_feed_lines(ctx.env.home, feed.name, record->second.sha256);
                    if (!lines) verdict = "no confirmed contents to verify against";
                    else {
                        auto v = verify_feed(*gateway, config, feed.name, *lines, 0, ctx.env.clock, full);
                        j["verification"] = {{"verified", v.verified}, {"sampled", v.sampled}, {"mismatches", v.sample_mismatches}, {"problem", v.problem}};
                        verdict += v.verified ? std::format("; verified ({} entries read back)", grouped(v.sampled)) : "; VERIFICATION FAILED: " + v.problem;
                        if (!v.verified) exit = 8;
                    }
                }
                j["status"] = verdict;
                if (feed.state && feed.state->overflow) {
                    j["overflow"] = true;
                    verdict += "; TRUNCATED by the device's entry limit";
                    exit = 8;
                }
                feeds.push_back(j);
                text += std::format("  {:<24} cat {:>3} {:>9} entries  {}\n", feed.name, feed.category, grouped(accepted), verdict);
            }
            json refs = profile_references(*gateway, config);
            text += "\nAttached profiles\n";
            if (refs.empty()) text += "  none: no traffic is filtered by forti-hole yet (`forti-hole attach`)\n";
            for (const auto& r: refs) text += std::format("  policy {} ({}) uses {}\n", r["target"].dump(), r.value("name", ""), r["profile"].get<std::string>());
            if (state.paused) text += std::format("\nPaused since {}\n", state.paused_at);
            if (load_journal(ctx.env.home)) text += "\nThe last sync did not finish; the next one re-sends its feeds.\n";
            if (!state.last_run.is_null()) text += std::format("\nLast sync {}: exit {}\n", state.last_run.value("at", "?"), state.last_run.value("exit", 0));
            json data = {{"device", device_json(inventory)}, {"feeds", feeds}, {"attached", refs}, {"paused", state.paused}, {"last_run", state.last_run}};
            return ctx.emit(data, text, exit);
        }

        // ---- doctor ----

        int cmd_doctor(Context& ctx) {
            Config& config = ctx.load();
            State state = load_state(ctx.env.home);
            auto gateway = ctx.env.gateway(config, std::make_shared<WritePermits>());
            InventoryOptions options;
            options.licence = true;
            Inventory inventory = read_inventory(*gateway, config, options);
            json checks = json::array();
            int worst = 0;
            auto check = [&](std::string_view level, std::string name, std::string detail) {
                checks.push_back({{"level", level}, {"check", name}, {"detail", detail}});
                if (level == "fail") worst = 6;
            };
            check("ok", "device", std::format("{} {} FortiOS {}", inventory.model, inventory.hostname, inventory.version));
            // "v7.4.8" -> (7, 4, 8)
            int major = 0, minor = 0, patch = 0;
            std::sscanf(inventory.version.c_str(), "v%d.%d.%d", &major, &minor, &patch);
            if (major * 10000 + minor * 100 + patch < 70201) check("fail", "push API", "push-mode feeds need FortiOS 7.2.1 or later");
            if (inventory.facts.entry_limit)
                check("ok", "entry limit", std::format("the device reports {} domain entries device-wide", grouped(*inventory.facts.entry_limit)));
            else
                check("warn", "entry limit",
                      std::format("not reported yet (no domain feed, or FortiOS before 7.4.4); assuming {}", grouped(config.budget.fallback_limit)));
            if (inventory.facts.memory_total) {
                double used = 100.0 * static_cast<double>(*inventory.facts.memory_used) / static_cast<double>(*inventory.facts.memory_total);
                double red = inventory.facts.memory_red_percent.value_or(88);
                std::string detail = std::format("{:.1f}% used, 24 h peak {}, conserve mode at {:.0f}%", used,
                                                 inventory.facts.memory_peak_percent ? std::format("{:.0f}%", *inventory.facts.memory_peak_percent) : "?", red);
                check(used >= red - 5 ? "fail" : used >= 100 - config.budget.memory_headroom_percent ? "warn" : "ok", "memory", detail);
            } else {
                check("warn", "memory", "the device did not report its memory");
            }
            std::set<int> free;
            for (int c = config.feeds.category_min; c <= config.feeds.category_max; ++c)
                if (!inventory.foreign_categories.contains(c)) free.insert(c);
            check(free.size() >= 4 ? "ok" : free.empty() ? "fail" : "warn", "categories",
                  std::format("{} of the remote categories {}-{} are free of operator feeds", free.size(), config.feeds.category_min, config.feeds.category_max));
            if (!inventory.collisions.empty()) {
                std::string list;
                for (const auto& c: inventory.collisions) list += (list.empty() ? "" : ", ") + c;
                check("fail", "ownership", "prefixed objects without forti-hole's marker: " + list);
            }
            if (inventory.fortiguard_web_filtering && *inventory.fortiguard_web_filtering != "licensed")
                check("info", "FortiGuard",
                      std::format("web filtering licence: {}. forti-hole's feeds do not need FortiGuard; managed profiles set error-allow{}",
                                  *inventory.fortiguard_web_filtering, config.dnsfilter.error_allow ? "" : " (disabled in config!)"));
            json refs = profile_references(*gateway, config);
            if (refs.empty())
                check("warn", "attachment",
                      "no firewall policy uses a forti-hole profile: DNS is filtered only on policies DNS queries cross (`forti-hole attach --profile P --policy ID`)");
            else
                check("ok", "attachment", std::format("{} policy(ies) use forti-hole profiles", refs.size()));
            if (!config.enabled) check("info", "enabled", "\"enabled\" is false: sync writes nothing");
            if (state.paused) check("info", "paused", "forti-hole is paused");
            if (load_journal(ctx.env.home)) check("warn", "journal", "the last sync did not finish; the next one re-sends its feeds in full");
            for (const auto& w: inventory.warnings) check("warn", "read", w);
            std::string text;
            for (const auto& c: checks)
                text += std::format("{:<5} {:<12} {}\n", c["level"].get<std::string>(), c["check"].get<std::string>(), c["detail"].get<std::string>());
            return ctx.emit({{"checks", checks}, {"device", device_json(inventory)}}, text, worst);
        }

        // ---- pause / resume ----

        int cmd_pause(Context& ctx, bool pause) {
            ctx.load();
            RunLock lock(ctx.env.home.lock());
            State state = load_state(ctx.env.home);
            state.paused = pause;
            state.paused_at = pause ? now_iso8601() : std::string();
            save_state(ctx.env.home, state);
            return ctx.emit({{"paused", pause}}, pause ? "Paused: sync writes nothing until `forti-hole resume`.\n" : "Resumed.\n");
        }

        // ---- attach / detach / remove ----

        std::uint64_t policy_id(const Args& args) {
            auto value = args.value("policy");
            if (!value) throw UsageError("--policy ID is required");
            std::uint64_t id = 0;
            for (char c: *value) {
                if (c < '0' || c > '9') throw UsageError("--policy takes a numeric policy id");
                id = id * 10 + static_cast<std::uint64_t>(c - '0');
            }
            return id;
        }

        int cmd_attach(Context& ctx, bool attach) {
            Config& config = ctx.load();
            RunLock lock(ctx.env.home.lock());
            State state = load_state(ctx.env.home);
            auto permits = std::make_shared<WritePermits>();
            auto gateway = ctx.env.gateway(config, permits);
            ManageContext m{*gateway, config, state, ctx.env.home, permits};
            std::uint64_t policy = policy_id(ctx.args);
            json data;
            std::string text;
            if (attach) {
                // `--profile` is the global forti profile option; attach names the forti-hole profile positionally.
                if (ctx.args.words.size() < 2) throw UsageError("usage: forti-hole attach PROFILE --policy ID");
                data = attach_policy(m, ctx.args.words[1], policy);
                text = data.value("changed", false) ? std::format("Policy {} now uses {}.\n", policy, data["profile"].get<std::string>())
                                                    : std::format("Policy {} already uses {}.\n", policy, data["profile"].get<std::string>());
            } else {
                data = detach_policy(m, policy, ctx.args.has("force"));
                text = std::format("Policy {}: removed {}{}.\n", policy, data["removed"].get<std::string>(),
                                   data["restored"].get<std::string>().empty() ? "" : ", restored " + data["restored"].get<std::string>());
            }
            return ctx.emit(data, text);
        }

        int cmd_remove(Context& ctx) {
            Config& config = ctx.load();
            if (!ctx.args.has("yes"))
                throw UsageError("remove deletes every forti-hole feed and DNS filter profile from the FortiGate; pass --yes to confirm");
            RunLock lock(ctx.env.home.lock());
            State state = load_state(ctx.env.home);
            auto permits = std::make_shared<WritePermits>();
            auto gateway = ctx.env.gateway(config, permits);
            Inventory inventory = read_inventory(*gateway, config);
            ManageContext m{*gateway, config, state, ctx.env.home, permits};
            json data = remove_all(m, inventory);
            std::string text = "Removed:\n";
            for (const auto& r: data["removed"]) text += "  " + r.get<std::string>() + "\n";
            if (data["removed"].empty()) text = "Nothing of forti-hole's is on the device.\n";
            return ctx.emit(data, text);
        }

        // ---- import / migrate ----

        int write_or_print(Context& ctx, const json& config, const std::vector<std::string>& notes) {
            json data = {{"config", config}, {"notes", notes}};
            std::string text;
            for (const auto& n: notes) text += "note: " + n + "\n";
            if (auto target = ctx.args.value("write")) {
                std::filesystem::path path = *target == "-" ? ctx.config_path : std::filesystem::path(*target);
                std::error_code ignored;
                if (std::filesystem::exists(path, ignored) && !ctx.args.has("force"))
                    throw ConfigError(std::format("{} exists; pass --force to replace it", path.string()));
                write_private(path, config.dump(2) + "\n");
                data["written"] = path.string();
                text += std::format("Wrote {} (\"enabled\": false). Review it, then `forti-hole compile --device`.\n", path.string());
                return ctx.emit(data, text);
            }
            if (ctx.json_mode) return ctx.emit(data, "");
            ctx.out() << config.dump(2) << "\n";
            for (const auto& n: notes) ctx.err() << "note: " << n << "\n";
            return 0;
        }

        int cmd_import(Context& ctx) {
            if (ctx.args.words.size() < 2 || ctx.args.words[1] != "pihole") throw UsageError("usage: forti-hole import pihole --url URL [--write PATH|-]");
            auto url = ctx.args.value("url");
            if (!url) throw UsageError("--url https://pihole.example.net is required");
            std::string name = ctx.args.value("name").value_or("default");
            auto password = ctx.env.pihole_password(name, *url);
            if (!password) throw ConfigError("no Pi-hole password: set FORTI_HOLE_PIHOLE_PASSWORD, or run on a terminal to be asked");
            auto reader = ctx.env.pihole(*url, *password, ctx.args.value("ca-bundle").value_or(""));
            password->assign(password->size(), '\0');
            ImportResult imported = import_pihole(reader->read());
            return write_or_print(ctx, imported.config, imported.notes);
        }

        int cmd_migrate(Context& ctx) {
            if (ctx.args.words.size() < 2) throw UsageError("usage: forti-hole migrate config OLD.yaml [--write PATH|-] | migrate feeds [--apply]");
            if (ctx.args.words[1] == "config") {
                if (ctx.args.words.size() < 3) throw UsageError("usage: forti-hole migrate config OLD.yaml [--write PATH|-]");
                auto text = read_file(ctx.args.words[2]);
                if (!text) throw ConfigError(std::format("cannot read {}", ctx.args.words[2]));
                LegacyConfig legacy = convert_legacy_config(parse_legacy_yaml(*text));
                text->assign(text->size(), '\0');
                auto notes = legacy.notes;
                notes.push_back(std::format("old feeds on the device: `forti-hole migrate feeds --prefix {}` lists them", legacy.feed_prefix));
                return write_or_print(ctx, legacy.config, notes);
            }
            if (ctx.args.words[1] == "feeds") {
                Config& config = ctx.load();
                std::string prefix = ctx.args.value("prefix").value_or("forti-hole_");
                if (prefix.starts_with(config.prefix))
                    throw UsageError(std::format("--prefix {} would match forti-hole's own feeds ({}*)", prefix, config.prefix));
                bool apply = ctx.args.has("apply");
                std::optional<RunLock> lock;
                if (apply) lock.emplace(ctx.env.home.lock());
                State state = load_state(ctx.env.home);
                auto permits = std::make_shared<WritePermits>();
                auto gateway = ctx.env.gateway(config, permits);
                Inventory inventory = read_inventory(*gateway, config);
                ManageContext m{*gateway, config, state, ctx.env.home, permits};
                json data = migrate_feeds(m, inventory, prefix, apply);
                std::string text;
                if (data["feeds"].empty()) text = std::format("No feeds named {}* on the device.\n", prefix);
                for (const auto& f: data["feeds"])
                    text += std::format("  {:<40} category {:>3} {:>9} entries\n", f["name"].get<std::string>(), f["category"].get<int>(),
                                        grouped(f["entries"].get<std::uint64_t>()));
                for (const auto& p: data["profiles_updated"]) text += std::format("  profile {} lists their categories\n", p.get<std::string>());
                if (!data["feeds"].empty())
                    text += data["applied"].get<bool>() ? "Deleted them and freed their categories.\n" : "Nothing changed; --apply deletes them.\n";
                return ctx.emit(data, text);
            }
            throw UsageError(std::format("unknown migrate target \"{}\"", ctx.args.words[1]));
        }

        int failure(Context& ctx, std::string_view code, const std::string& message, int exit) {
            if (ctx.json_mode) ctx.out() << json{{"ok", false}, {"error", {{"code", code}, {"message", message}}}}.dump(2) << "\n";
            else ctx.err() << "forti-hole: " << message << "\n";
            return exit;
        }

        int gateway_exit(const std::string& code) {
            if (code == "connection" || code == "auth") return 4;
            if (code == "config") return 3;
            if (code == "not_found" || code == "unknown_resource") return 5;
            if (code == "approval_required" || code == "policy_denied" || code == "stale_plan") return 7;
            if (code == "partial_failure" || code == "outcome_unknown" || code == "management_connectivity_lost") return 8;
            if (code == "usage") return 2;
            if (code == "internal") return 1;
            return 6;
        }

    }  // namespace

    std::string usage_text() {
        return R"(forti-hole: DNS blocklists as FortiGate threat feeds

Usage: forti-hole [--config PATH] [--profile FORTI_PROFILE] [--json] [--quiet] COMMAND

  setup [--force] [--offline]   write ~/.forti/hole/config.json (disabled) and check the device
  doctor                        what the device can hold, and where filtering applies
  compile [--offline] [--device] [--write DIR] [--lines]
                                fetch and compile the lists; report deduplication and fit
  plan                          everything a sync would change (writes nothing to the device)
  sync [--force-snapshot]       push the compiled feeds (deltas), verify them on the device
  status [--verify [--full]]    the device's forti-hole feeds against what was confirmed
  pause | resume                stop / restart writes by sync
  attach PROFILE --policy ID    filter a firewall policy's DNS with a forti-hole profile
  detach --policy ID [--force]  undo attach (restores the policy's previous DNS filter)
  remove --yes                  delete every forti-hole feed and DNS filter profile
  import pihole --url URL [--name NAME] [--ca-bundle PEM] [--write PATH|-]
                                a config from a Pi-hole v6's lists, groups and exceptions
  migrate config OLD.yaml [--write PATH|-]
                                a config from forti-hole 0.x's config.yaml
  migrate feeds [--prefix forti-hole_] [--apply]
                                forti-hole 0.x feeds on the device: list, or delete with --apply
  version

The FortiGate connection is a forti profile: `forti configure` (forti-cli).
Exit: 0 ok, 2 usage, 3 config, 4 connection, 5 not found, 6 refused, 7 not enabled/approval,
      8 partial or unverified, 9 does not fit the device.
)";
    }

    int run(const std::vector<std::string>& argv, Environment& env) {
        Context ctx{env, {}, false, false, std::nullopt, {}};
        try {
            ctx.args = parse_args(argv);
            ctx.json_mode = ctx.args.has("json");
            ctx.quiet = ctx.args.has("quiet");
            ctx.config_path = ctx.args.value("config") ? std::filesystem::path(*ctx.args.value("config")) : env.home.config();
            if (ctx.args.has("version") || (!ctx.args.words.empty() && ctx.args.words[0] == "version"))
                return ctx.emit({{"version", FORTI_HOLE_VERSION}, {"built_from", FORTI_HOLE_BUILT_FROM}}, version_line() + "\n");
            if (ctx.args.words.empty() || ctx.args.has("help") || ctx.args.words[0] == "help") {
                ctx.out() << usage_text();
                return ctx.args.words.empty() && !ctx.args.has("help") ? 2 : 0;
            }
            const std::string& command = ctx.args.words[0];
            if (command == "setup") return cmd_setup(ctx);
            if (command == "compile") return cmd_compile(ctx);
            if (command == "plan") return cmd_plan(ctx);
            if (command == "sync") return cmd_sync(ctx);
            if (command == "status") return cmd_status(ctx);
            if (command == "doctor") return cmd_doctor(ctx);
            if (command == "pause") return cmd_pause(ctx, true);
            if (command == "resume") return cmd_pause(ctx, false);
            if (command == "attach") return cmd_attach(ctx, true);
            if (command == "detach") return cmd_attach(ctx, false);
            if (command == "remove") return cmd_remove(ctx);
            if (command == "import") return cmd_import(ctx);
            if (command == "migrate") return cmd_migrate(ctx);
            throw UsageError(std::format("unknown command \"{}\" (forti-hole --help)", command));
        } catch (const UsageError& e) {
            return failure(ctx, "usage", e.what(), 2);
        } catch (const ConfigError& e) {
            return failure(ctx, "config", e.what(), 3);
        } catch (const LegacyError& e) {
            return failure(ctx, "config", e.what(), 3);
        } catch (const StateError& e) {
            return failure(ctx, "config", e.what(), 3);
        } catch (const FileError& e) {
            return failure(ctx, "config", e.what(), 3);
        } catch (const SyncError& e) {
            return failure(ctx, "refused", e.what(), e.exit_code());
        } catch (const PipelineError& e) {
            return failure(ctx, "lists", e.what(), 6);
        } catch (const CompileError& e) {
            return failure(ctx, "config", e.what(), 3);
        } catch (const GatewayError& e) {
            return failure(ctx, e.code(), e.what(), gateway_exit(e.code()));
        } catch (const HttpsError& e) {
            return failure(ctx, "connection", e.what(), 4);
        } catch (const GuardViolation& e) {
            return failure(ctx, "internal", std::format("guard: {} (a bug: please report it)", e.what()), 1);
        } catch (const std::exception& e) {
            return failure(ctx, "internal", e.what(), 1);
        }
    }

    namespace {

        std::optional<std::string> read_password_from_terminal(const std::string& prompt) {
            if (!::isatty(STDIN_FILENO)) return std::nullopt;
            std::cerr << prompt << std::flush;
            termios old{};
            bool restore = ::tcgetattr(STDIN_FILENO, &old) == 0;
            if (restore) {
                termios quiet = old;
                quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
                ::tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
            }
            std::string line;
            std::getline(std::cin, line);
            if (restore) ::tcsetattr(STDIN_FILENO, TCSANOW, &old);
            std::cerr << "\n";
            if (line.empty()) return std::nullopt;
            return line;
        }

    }  // namespace

    Environment production_environment() {
        Environment env;
        env.gateway = [](const Config& config, std::shared_ptr<WritePermits> permits) { return engine_gateway(config, std::move(permits)); };
        env.fetcher = [](const FetchOptions& options) { return curl_fetcher(options); };
        env.pihole = [](const std::string& url, const std::string& password, const std::string& ca) { return pihole_api(url, password, ca); };
        env.pihole_password = [](const std::string& name, const std::string& url) -> std::optional<std::string> {
            if (const char* value = std::getenv("FORTI_HOLE_PIHOLE_PASSWORD"); value && *value) return std::string(value);
            // forti-pihole keeps the Pi-hole app password in forti's secret store, bound to the URL.
            try {
                auto paths = forti::engine::Paths::from_env();
                auto store = forti::engine::default_secret_store(paths);
                if (auto secret = store->get(std::format("pihole/{}/password", name)); secret && !secret->secret.empty()) {
                    std::string base = validated_base_url(url);
                    if (secret->binding.empty() || secret->binding == base || secret->binding.starts_with(base)) return secret->secret;
                }
            } catch (const std::exception&) {
                // fall through to the prompt
            }
            return read_password_from_terminal("Pi-hole password (app password): ");
        };
        env.clock.now = [] {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        env.clock.sleep = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
        env.out = &std::cout;
        env.err = &std::cerr;
        return env;
    }

}  // namespace fh
