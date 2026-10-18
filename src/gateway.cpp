#include "gateway.hpp"

#include "config.hpp"

#include <forti/engine/engine.hpp>
#include <forti_api/threat_feed.hpp>

namespace fh {

    using json = nlohmann::json;

    json expect_ok(const json& envelope) {
        if (envelope.value("ok", false)) return envelope.value("data", json());
        throw GatewayError(envelope.value("error", json{{"code", "internal"}, {"message", "malformed engine reply"}}));
    }

    json read_all(Gateway& gateway, json request) {
        json all = json::array();
        std::size_t start = 0;
        while (true) {
            request["start"] = start;
            request["count"] = 1000;
            auto envelope = gateway.read(request);
            auto data = expect_ok(envelope);
            if (!data.is_array()) return data;
            for (auto& item: data) all.push_back(std::move(item));
            auto meta = envelope.value("meta", json::object());
            if (!meta.contains("next_start")) break;
            start = meta["next_start"].get<std::size_t>();
        }
        return all;
    }

    namespace {

        // A thrown SDK or engine failure as the engine would report it.
        template<typename F>
        auto translated(F&& call) {
            json envelope;
            decltype(call()) value{};
            // Engine::envelope returns what the operation returns, or the translated failure.
            envelope = forti::engine::Engine::envelope([&] {
                value = call();
                return json{{"ok", true}};
            });
            if (!envelope.is_object() || !envelope.value("ok", false))
                throw GatewayError(envelope.is_object() ? envelope.value("error", json::object()) : json::object());
            return value;
        }

        class EngineGateway final : public Gateway {
            forti::engine::Engine engine_;
            std::string profile_;

            fortiapi::Client& client() { return engine_.client(engine_.profiles().select(profile_)); }

        public:
            EngineGateway(forti::engine::Options options, std::string profile) : engine_(std::move(options)), profile_(std::move(profile)) {}

            json read(const json& request) override { return forti::engine::Engine::envelope([&] { return engine_.read(request); }); }
            json plan(const json& request) override { return forti::engine::Engine::envelope([&] { return engine_.plan(request); }); }
            json apply(const json& request) override { return forti::engine::Engine::envelope([&] { return engine_.apply(request); }); }
            json discover(const json& request) override { return forti::engine::Engine::envelope([&] { return engine_.discover(request); }); }

            FeedStatus feed_status(std::string_view feed) override {
                return translated([&] {
                    auto s = fortiapi::system::feed_status(feed, client());
                    return FeedStatus{s.loaded, s.file_status, s.error, s.accepted, s.valid, s.invalid, s.limit, s.overflow, s.last_update};
                });
            }

            std::vector<FeedEntry> feed_entries(std::string_view feed, std::size_t start, std::size_t count) override {
                return translated([&] {
                    std::vector<FeedEntry> out;
                    for (const auto& e: fortiapi::system::feed_entries(feed, start, count, client())) out.push_back({e.entry, e.valid});
                    return out;
                });
            }
        };

    }  // namespace

    std::unique_ptr<Gateway> engine_gateway(const Config& config, std::shared_ptr<WritePermits> permits) {
        if (!permits) permits = std::make_shared<WritePermits>();
        forti::engine::Options options;
        options.actor = "forti-hole";
        std::string prefix = config.prefix;
        // The guard sits under the engine, around the SDK's own libcurl transport, so it sees every
        // request the engine makes, including the ones plan and apply make on their own.
        options.transport = [prefix, permits](const fortiapi::Config& sdk) -> std::shared_ptr<fortiapi::Transport> {
            return std::make_shared<GuardTransport>(std::make_shared<fortiapi::CurlTransport>(sdk), prefix, permits);
        };
        return std::make_unique<EngineGateway>(std::move(options), config.profile);
    }

}  // namespace fh
