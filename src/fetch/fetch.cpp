#include "fetch/fetch.hpp"

#include <curl/curl.h>

#include <chrono>
#include <format>
#include <mutex>

namespace fh {

    namespace {

        // curl_global_init is not thread-safe; once per process is all it needs.
        void global_init() {
            static std::once_flag once;
            std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        }

        struct Transfer {
            FetchResponse response;
            std::uint64_t max_bytes = 0;
            bool too_large = false;
            curl_slist* headers = nullptr;
            CURL* easy = nullptr;
            std::chrono::steady_clock::time_point started;

            ~Transfer() {
                if (headers) curl_slist_free_all(headers);
                if (easy) curl_easy_cleanup(easy);
            }
        };

        std::size_t on_body(char* data, std::size_t size, std::size_t count, void* user) {
            auto* transfer = static_cast<Transfer*>(user);
            std::size_t bytes = size * count;
            if (transfer->response.body.size() + bytes > transfer->max_bytes) {
                transfer->too_large = true;
                return 0;   // aborts the transfer (CURLE_WRITE_ERROR)
            }
            transfer->response.body.append(data, bytes);
            return bytes;
        }

        std::string_view trim(std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.remove_suffix(1);
            return text;
        }

        bool header_is(std::string_view line, std::string_view name) {
            if (line.size() <= name.size() || line[name.size()] != ':') return false;
            for (std::size_t i = 0; i < name.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(line[i])) != name[i]) return false;
            return true;
        }

        std::size_t on_header(char* data, std::size_t size, std::size_t count, void* user) {
            auto* transfer = static_cast<Transfer*>(user);
            std::string_view line(data, size * count);
            // A redirect's headers come first; a new status line starts the final reply's headers.
            if (line.starts_with("HTTP/")) {
                transfer->response.etag.clear();
                transfer->response.last_modified.clear();
                transfer->response.content_type.clear();
            } else if (header_is(line, "etag")) {
                transfer->response.etag = std::string(trim(line.substr(5)));
            } else if (header_is(line, "last-modified")) {
                transfer->response.last_modified = std::string(trim(line.substr(14)));
            } else if (header_is(line, "content-type")) {
                transfer->response.content_type = std::string(trim(line.substr(13)));
            }
            return size * count;
        }

        class CurlFetcher final : public Fetcher {
            FetchOptions options_;

        public:
            explicit CurlFetcher(FetchOptions options) : options_(std::move(options)) { global_init(); }

            std::vector<FetchResponse> fetch(const std::vector<FetchRequest>& requests) override {
                std::vector<std::unique_ptr<Transfer>> transfers;
                std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
                if (!multi) throw std::runtime_error("curl_multi_init failed");
                curl_multi_setopt(multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, static_cast<long>(options_.parallel));
                curl_multi_setopt(multi.get(), CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

                for (const auto& request: requests) {
                    auto transfer = std::make_unique<Transfer>();
                    transfer->response.url = request.url;
                    transfer->max_bytes = options_.max_bytes;
                    transfer->easy = curl_easy_init();
                    if (!transfer->easy) throw std::runtime_error("curl_easy_init failed");
                    CURL* easy = transfer->easy;
                    curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
                    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https,file");
                    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https");
                    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
                    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
                    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
                    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
                    if (!options_.ca_bundle.empty()) curl_easy_setopt(easy, CURLOPT_CAINFO, options_.ca_bundle.c_str());
                    curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(options_.timeout_seconds));
                    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 20L);
                    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");   // gzip/deflate/br as libcurl supports
                    curl_easy_setopt(easy, CURLOPT_USERAGENT, options_.user_agent.c_str());
                    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
                    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
                    curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get());
                    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header);
                    curl_easy_setopt(easy, CURLOPT_HEADERDATA, transfer.get());
                    curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer.get());
                    if (!request.etag.empty())
                        transfer->headers = curl_slist_append(transfer->headers, std::format("If-None-Match: {}", request.etag).c_str());
                    if (!request.last_modified.empty())
                        transfer->headers = curl_slist_append(transfer->headers, std::format("If-Modified-Since: {}", request.last_modified).c_str());
                    if (transfer->headers) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer->headers);
                    transfer->started = std::chrono::steady_clock::now();
                    curl_multi_add_handle(multi.get(), easy);
                    transfers.push_back(std::move(transfer));
                }

                int running = 0;
                do {
                    if (CURLMcode code = curl_multi_perform(multi.get(), &running); code != CURLM_OK)
                        throw std::runtime_error(std::format("curl_multi_perform: {}", curl_multi_strerror(code)));
                    int queued = 0;
                    while (CURLMsg* message = curl_multi_info_read(multi.get(), &queued)) {
                        if (message->msg != CURLMSG_DONE) continue;
                        Transfer* transfer = nullptr;
                        curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &transfer);
                        finish(*transfer, message->data.result);
                        curl_multi_remove_handle(multi.get(), message->easy_handle);
                    }
                    if (running > 0) curl_multi_poll(multi.get(), nullptr, 0, 1000, nullptr);
                } while (running > 0);

                std::vector<FetchResponse> responses;
                responses.reserve(transfers.size());
                for (auto& transfer: transfers) responses.push_back(std::move(transfer->response));
                return responses;
            }

        private:
            static void finish(Transfer& transfer, CURLcode result) {
                auto& response = transfer.response;
                response.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - transfer.started).count();
                if (result != CURLE_OK) {
                    response.transferred = false;
                    response.body.clear();
                    response.error = transfer.too_large ? std::format("larger than {} bytes", transfer.max_bytes) : curl_easy_strerror(result);
                    return;
                }
                response.transferred = true;
                long status = 0;
                curl_easy_getinfo(transfer.easy, CURLINFO_RESPONSE_CODE, &status);
                // file:// reports 0.
                response.status = status == 0 ? 200 : status;
            }
        };

    }  // namespace

    std::unique_ptr<Fetcher> curl_fetcher(FetchOptions options) { return std::make_unique<CurlFetcher>(std::move(options)); }

}  // namespace fh
