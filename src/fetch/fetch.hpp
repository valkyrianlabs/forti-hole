//
// Downloading upstream lists (never the FortiGate: that is forti-api's, through the engine).
//
// `Fetcher` is the seam: CurlFetcher downloads in parallel with libcurl's multi interface; tests
// script FakeFetcher. CurlFetcher guarantees, whatever the configuration says:
//   - TLS peer and host verification always on (a CA bundle adds trust, never removes it);
//   - https:// and file:// only, redirects followed only to https:// (at most 5);
//   - a reply larger than max_bytes fails (the transfer is aborted, nothing is kept);
//   - conditional requests: If-None-Match / If-Modified-Since from the last good copy, so an
//     unchanged list costs a 304 and no download.
//

#ifndef FORTI_HOLE_FETCH_FETCH_HPP
#define FORTI_HOLE_FETCH_FETCH_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fh {

    struct FetchRequest {
        std::string url;
        std::string etag;            // from the last good copy, if any
        std::string last_modified;
    };

    struct FetchResponse {
        std::string url;
        bool transferred = false;    // an HTTP reply (or file) arrived; false: error says why not
        long status = 0;             // HTTP status; 200 for file://
        std::string body;
        std::string etag, last_modified, content_type;
        std::string error;
        double seconds = 0;
    };

    struct FetchOptions {
        int timeout_seconds = 120;
        std::uint64_t max_bytes = 128ull << 20;
        std::size_t parallel = 6;
        std::string ca_bundle;
        std::string user_agent = "forti-hole";
    };

    class Fetcher {
    public:
        virtual ~Fetcher() = default;
        // One response per request, in request order.
        virtual std::vector<FetchResponse> fetch(const std::vector<FetchRequest>& requests) = 0;
    };

    std::unique_ptr<Fetcher> curl_fetcher(FetchOptions options);

}  // namespace fh

#endif  // FORTI_HOLE_FETCH_FETCH_HPP
