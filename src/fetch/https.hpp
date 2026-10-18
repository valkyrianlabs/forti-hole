//
// One HTTPS request with a JSON body, for `import pihole` (the Pi-hole v6 API). The same guarantees
// as the list fetcher (TLS verification always on; a CA bundle only adds trust), plus: redirects are
// never followed (a session ID must not be replayed to another host), plain http:// only to a
// loopback address, and errors never contain headers or bodies (that is where secrets are).
//

#ifndef FORTI_HOLE_FETCH_HTTPS_HPP
#define FORTI_HOLE_FETCH_HTTPS_HPP

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fh {

    struct HttpsRequest {
        std::string method = "GET";
        std::string url;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        std::string ca_bundle;
        long timeout_seconds = 20;
    };

    struct HttpsResponse {
        long status = 0;
        std::string body;
    };

    class HttpsError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // Throws HttpsError when no reply arrived or the URL is not acceptable; any status is a response.
    HttpsResponse https_request(const HttpsRequest& request);

    // "https://host[:port][/prefix]" with no credentials, query or fragment; http:// only to loopback.
    // Returns the URL without a trailing '/'. Throws HttpsError.
    std::string validated_base_url(std::string_view url);

}  // namespace fh

#endif  // FORTI_HOLE_FETCH_HTTPS_HPP
