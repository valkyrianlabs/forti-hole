#include "state.hpp"

#include "core/sha256.hpp"

#include <chrono>
#include <format>

namespace fh {

    using json = nlohmann::json;

    namespace {

        std::string str(const json& object, const char* key) {
            auto it = object.find(key);
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        template<typename T>
        T num(const json& object, const char* key, T fallback = 0) {
            auto it = object.find(key);
            return it != object.end() && it->is_number() ? it->get<T>() : fallback;
        }

        bool flag(const json& object, const char* key) {
            auto it = object.find(key);
            return it != object.end() && it->is_boolean() && it->get<bool>();
        }

        std::filesystem::path feed_path(const Home& home, std::string_view feed) { return home.feeds() / (std::string(feed) + ".txt"); }

    }  // namespace

    std::string now_iso8601() {
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
    }

    json State::to_json() const {
        json j = {{"format", 1}, {"paused", paused}};
        if (!paused_at.empty()) j["paused_at"] = paused_at;
        if (!device_serial.empty()) j["device_serial"] = device_serial;
        if (entry_limit) j["entry_limit"] = *entry_limit;
        j["sources"] = json::object();
        for (const auto& [key, s]: sources)
            j["sources"][key] = {{"url", s.url},         {"etag", s.etag},       {"last_modified", s.last_modified}, {"sha256", s.sha256},
                                 {"entries", s.entries}, {"bytes", s.bytes},     {"fetched_at", s.fetched_at},       {"checked_at", s.checked_at}};
        j["selected"] = selected;
        j["groups"] = json::object();
        for (const auto& [label, g]: groups) j["groups"][label] = {{"category", g.category}, {"partitions", g.partitions}};
        j["feeds"] = json::object();
        for (const auto& [name, f]: feeds)
            j["feeds"][name] = {{"group", f.group},   {"category", f.category},           {"confirmed", f.confirmed},
                                {"sha256", f.sha256}, {"lines", f.lines},                 {"device_update", f.device_update},
                                {"confirmed_at", f.confirmed_at}};
        j["attachments"] = json::object();
        for (const auto& [key, a]: attachments)
            j["attachments"][key] = {{"kind", a.kind},
                                     {"target", a.target},
                                     {"profile", a.profile},
                                     {"previous_profile", a.previous_profile},
                                     {"previous_utm", a.previous_utm},
                                     {"created", a.created},
                                     {"at", a.at}};
        if (bytes_per_line) j["bytes_per_line"] = *bytes_per_line;
        j["measurements"] = json::array();
        for (const auto& m: measurements)
            j["measurements"].push_back({{"at", m.at},
                                         {"lines_before", m.lines_before},
                                         {"lines_after", m.lines_after},
                                         {"used_before", m.used_before},
                                         {"used_after", m.used_after},
                                         {"bytes_per_line", m.bytes_per_line}});
        if (!last_run.is_null()) j["last_run"] = last_run;
        return j;
    }

    State State::from_json(const json& j) {
        State s;
        if (!j.is_object()) return s;
        s.paused = flag(j, "paused");
        s.paused_at = str(j, "paused_at");
        s.device_serial = str(j, "device_serial");
        if (j.contains("entry_limit") && j["entry_limit"].is_number()) s.entry_limit = j["entry_limit"].get<std::uint64_t>();
        if (auto it = j.find("sources"); it != j.end() && it->is_object())
            for (const auto& [key, v]: it->items())
                s.sources[key] = {str(v, "url"),           str(v, "etag"),           str(v, "last_modified"), str(v, "sha256"),
                                  num<std::uint64_t>(v, "entries"), num<std::uint64_t>(v, "bytes"), str(v, "fetched_at"),
                                  str(v, "checked_at")};
        if (auto it = j.find("selected"); it != j.end() && it->is_object())
            for (const auto& [key, v]: it->items())
                if (v.is_string()) s.selected[key] = v.get<std::string>();
        if (auto it = j.find("groups"); it != j.end() && it->is_object())
            for (const auto& [label, v]: it->items()) s.groups[label] = {num<int>(v, "category"), num<std::size_t>(v, "partitions", 1)};
        if (auto it = j.find("feeds"); it != j.end() && it->is_object())
            for (const auto& [name, v]: it->items())
                s.feeds[name] = {str(v, "group"),          num<int>(v, "category"),           flag(v, "confirmed"), str(v, "sha256"),
                                 num<std::uint64_t>(v, "lines"), num<std::int64_t>(v, "device_update"), str(v, "confirmed_at")};
        if (auto it = j.find("attachments"); it != j.end() && it->is_object())
            for (const auto& [key, v]: it->items())
                s.attachments[key] = {str(v, "kind"),         str(v, "target"),       str(v, "profile"), str(v, "previous_profile"),
                                      str(v, "previous_utm"), flag(v, "created"), str(v, "at")};
        if (j.contains("bytes_per_line") && j["bytes_per_line"].is_number()) s.bytes_per_line = j["bytes_per_line"].get<double>();
        if (auto it = j.find("measurements"); it != j.end() && it->is_array())
            for (const auto& m: *it)
                s.measurements.push_back({str(m, "at"), num<std::uint64_t>(m, "lines_before"), num<std::uint64_t>(m, "lines_after"),
                                          num<std::uint64_t>(m, "used_before"), num<std::uint64_t>(m, "used_after"),
                                          num<double>(m, "bytes_per_line")});
        if (auto it = j.find("last_run"); it != j.end()) s.last_run = *it;
        return s;
    }

    State load_state(const Home& home) {
        auto text = read_file(home.state());
        if (!text) return {};
        auto parsed = json::parse(*text, nullptr, false);
        if (parsed.is_discarded())
            throw StateError(std::format("{} is not valid JSON; move it aside to start over (the next sync then re-snapshots every feed)",
                                         home.state().string()));
        return State::from_json(parsed);
    }

    void save_state(const Home& home, const State& state) { write_private(home.state(), state.to_json().dump(2) + "\n"); }

    std::optional<std::vector<std::string>> load_feed_lines(const Home& home, std::string_view feed, std::string_view sha256) {
        auto text = read_file(feed_path(home, feed));
        if (!text || sha256_hex(*text) != sha256) return std::nullopt;
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < text->size()) {
            auto end = text->find('\n', start);
            if (end == std::string::npos) end = text->size();
            lines.emplace_back(text->substr(start, end - start));
            start = end + 1;
        }
        return lines;
    }

    void save_feed_lines(const Home& home, std::string_view feed, const std::vector<std::string>& lines) {
        std::string text;
        std::size_t size = 0;
        for (const auto& line: lines) size += line.size() + 1;
        text.reserve(size);
        for (const auto& line: lines) {
            text += line;
            text += '\n';
        }
        write_private(feed_path(home, feed), text);
    }

    void remove_feed_lines(const Home& home, std::string_view feed) { remove_file(feed_path(home, feed)); }

    json Journal::to_json() const { return {{"started_at", started_at}, {"phase", phase}, {"feeds", feeds}, {"plan_ids", plan_ids}}; }

    Journal Journal::from_json(const json& j) {
        Journal journal;
        journal.started_at = str(j, "started_at");
        journal.phase = str(j, "phase");
        for (const char* key: {"feeds", "plan_ids"})
            if (auto it = j.find(key); it != j.end() && it->is_array())
                for (const auto& v: *it)
                    if (v.is_string()) (std::string_view(key) == "feeds" ? journal.feeds : journal.plan_ids).push_back(v.get<std::string>());
        return journal;
    }

    std::optional<Journal> load_journal(const Home& home) {
        auto text = read_file(home.journal());
        if (!text) return std::nullopt;
        auto parsed = json::parse(*text, nullptr, false);
        // A journal that cannot be read still says a sync was interrupted: assume every feed changed.
        if (parsed.is_discarded()) return Journal{"", "unknown", {}, {}};
        return Journal::from_json(parsed);
    }

    void save_journal(const Home& home, const Journal& journal) { write_private(home.journal(), journal.to_json().dump(2) + "\n"); }

    void clear_journal(const Home& home) { remove_file(home.journal()); }

}  // namespace fh
