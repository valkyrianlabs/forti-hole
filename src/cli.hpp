//
// Command line: argv -> one command, rendered as text or (--json) the envelope
// {"ok": true, "data": ...} / {"ok": false, "error": {"code", "message"}}.
//
// Exit status:
//   0 success                      5 not found (feed, policy, profile)
//   1 internal (a guard refusal)   6 refused by the device or by forti-hole (collision, API error)
//   2 usage                        7 not enabled, read-only profile, waiting for approval, stale plan
//   3 configuration                8 partial outcome or failed verification
//   4 connection or credentials    9 the compiled set does not fit the device (nothing was written)
//

#ifndef FORTI_HOLE_CLI_HPP
#define FORTI_HOLE_CLI_HPP

#include "fetch/fetch.hpp"
#include "files.hpp"
#include "gateway.hpp"
#include "pihole_import.hpp"
#include "sync.hpp"

#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fh {

    struct Config;

    struct Environment {
        Home home = Home::from_env();
        std::function<std::unique_ptr<Gateway>(const Config&, std::shared_ptr<WritePermits>)> gateway;
        std::function<std::unique_ptr<Fetcher>(const FetchOptions&)> fetcher;
        std::function<std::unique_ptr<PiholeReader>(const std::string& url, const std::string& password, const std::string& ca_bundle)> pihole;
        // A secret for `import pihole`: the environment variable, forti's secret store (where
        // forti-pihole keeps the Pi-hole app password), or a prompt on a terminal.
        std::function<std::optional<std::string>(const std::string& name, const std::string& url)> pihole_password;
        Clock clock;
        std::ostream* out = nullptr;
        std::ostream* err = nullptr;
    };

    int run(const std::vector<std::string>& args, Environment& environment);

    std::string usage_text();

    // The production environment (engine gateway, libcurl, real clock, the terminal).
    Environment production_environment();

}  // namespace fh

#endif  // FORTI_HOLE_CLI_HPP
