//
// A temporary forti home, a scripted list server and the CLI wired to the in-memory FortiGate.
//

#ifndef FORTI_HOLE_TESTS_FIXTURE_HPP
#define FORTI_HOLE_TESTS_FIXTURE_HPP

#include "cli.hpp"
#include "config.hpp"
#include "fake_gateway.hpp"
#include "fetch/fetch.hpp"
#include "files.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <unistd.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fh::test {

    // Serves lists by URL with ETag support; anything unknown is a transport failure.
    class FakeFetcher final : public Fetcher {
    public:
        struct Page {
            long status = 200;
            std::string body, etag, content_type = "text/plain";
            bool fail = false;
        };
        std::map<std::string, Page, std::less<>> pages;
        std::vector<FetchRequest> requests;

        std::vector<FetchResponse> fetch(const std::vector<FetchRequest>& batch) override {
            std::vector<FetchResponse> out;
            for (const auto& request: batch) {
                requests.push_back(request);
                FetchResponse r;
                r.url = request.url;
                auto it = pages.find(request.url);
                if (it == pages.end() || it->second.fail) {
                    r.error = "connection refused";
                    out.push_back(std::move(r));
                    continue;
                }
                r.transferred = true;
                const auto& page = it->second;
                if (!page.etag.empty() && request.etag == page.etag) {
                    r.status = 304;
                } else {
                    r.status = page.status;
                    r.body = page.body;
                    r.etag = page.etag;
                    r.content_type = page.content_type;
                }
                out.push_back(std::move(r));
            }
            return out;
        }
    };

    class TempHome {
    public:
        std::filesystem::path root;
        Home home;

        TempHome() {
            auto base = std::filesystem::temp_directory_path() / std::format("forti-hole-test-{}-{}", ::getpid(), counter()++);
            std::filesystem::remove_all(base);
            std::filesystem::create_directories(base);
            root = base;
            home = Home{base / "hole"};
        }
        ~TempHome() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
        TempHome(const TempHome&) = delete;
        TempHome& operator=(const TempHome&) = delete;

    private:
        static int& counter() {
            static int n = 0;
            return n;
        }
    };

    inline std::string hosts_list(int from, int to, std::string_view suffix = "example") {
        std::string out = std::format("# Number of unique domains: {}\n", to - from);
        for (int i = from; i < to; ++i) out += std::format("0.0.0.0 h{}.{}\n", i, suffix);
        return out;
    }

    inline std::string adblock_list(int from, int to, std::string_view stem = "a") {
        std::string out = std::format("! Number of entries: {}\n", to - from);
        for (int i = from; i < to; ++i) out += std::format("||{}{}.example^\n", stem, i);
        return out;
    }

    // A small configuration: three sources in two tiers, two profiles, one auto-variant source.
    inline nlohmann::json small_config() {
        return {{"format", 1},
                {"enabled", true},
                {"prefix", "fh-"},
                {"tiers", {"base", "threat"}},
                {"sources",
                 {{{"id", "hosts"}, {"tier", "base"}, {"format", "hosts"}, {"url", "https://lists.test/hosts"}},
                  {{"id", "ads"}, {"tier", "base"}, {"format", "adblock"}, {"url", "https://lists.test/ads.txt"}},
                  {{"id", "tif"},
                   {"tier", "threat"},
                   {"format", "adblock"},
                   {"variants", {{"medium", "https://lists.test/tif.medium.txt"}, {"mini", "https://lists.test/tif.mini.txt"}}},
                   {"variant", "auto"},
                   {"auto", {"medium", "mini"}}}}},
                {"profiles", {{"home", {{"tiers", {"base", "threat"}}}}, {"kids", {{"tiers", {"base"}}}}}},
                {"feeds", {{"max_entries", 1000}}},
                {"sync", {{"chunk_entries", 120}, {"load_timeout_seconds", 30}}}};
    }

    struct Harness {
        TempHome temp;
        FakeGateway device;
        FakeFetcher lists;
        std::ostringstream out, err;
        Environment env;
        std::int64_t now = 0;

        Harness() {
            env.home = temp.home;
            env.gateway = [this](const Config&, std::shared_ptr<WritePermits>) -> std::unique_ptr<Gateway> {
                return std::make_unique<Proxy>(device);
            };
            env.fetcher = [this](const FetchOptions&) -> std::unique_ptr<Fetcher> { return std::make_unique<FetcherProxy>(lists); };
            env.clock.now = [this] { return now; };
            env.clock.sleep = [this](int ms) { now += std::max(1, ms / 1000); };
            env.out = &out;
            env.err = &err;
            lists.pages["https://lists.test/hosts"] = {200, hosts_list(0, 400), "\"h1\""};
            lists.pages["https://lists.test/ads.txt"] = {200, adblock_list(0, 300), "\"a1\""};
            lists.pages["https://lists.test/tif.medium.txt"] = {200, adblock_list(0, 2000, "t"), "\"m1\""};
            lists.pages["https://lists.test/tif.mini.txt"] = {200, adblock_list(0, 200, "t"), "\"n1\""};
        }

        void write_config(const nlohmann::json& config) { write_private(temp.home.config(), config.dump(2)); }

        int run(std::vector<std::string> args) {
            out.str("");
            err.str("");
            return fh::run(args, env);
        }

        nlohmann::json run_json(std::vector<std::string> args, int* exit = nullptr) {
            args.push_back("--json");
            int code = run(args);
            if (exit) *exit = code;
            return nlohmann::json::parse(out.str());
        }

    private:
        // The CLI owns its gateway; the device outlives it.
        struct Proxy final : Gateway {
            FakeGateway& d;
            explicit Proxy(FakeGateway& device) : d(device) {}
            nlohmann::json read(const nlohmann::json& r) override { return d.read(r); }
            nlohmann::json plan(const nlohmann::json& r) override { return d.plan(r); }
            nlohmann::json apply(const nlohmann::json& r) override { return d.apply(r); }
            nlohmann::json discover(const nlohmann::json& r) override { return d.discover(r); }
            FeedStatus feed_status(std::string_view f) override { return d.feed_status(f); }
            std::vector<FeedEntry> feed_entries(std::string_view f, std::size_t s, std::size_t c) override { return d.feed_entries(f, s, c); }
        };
        struct FetcherProxy final : Fetcher {
            FakeFetcher& f;
            explicit FetcherProxy(FakeFetcher& fetcher) : f(fetcher) {}
            std::vector<FetchResponse> fetch(const std::vector<FetchRequest>& r) override { return f.fetch(r); }
        };
    };

}  // namespace fh::test

#endif  // FORTI_HOLE_TESTS_FIXTURE_HPP
