#include "fetch/https.hpp"

#include <curl/curl.h>

#include <format>
#include <memory>

namespace fh {

    namespace {

        constexpr std::size_t max_reply = 16u << 20;

        std::size_t on_body(char* data, std::size_t size, std::size_t count, void* user) {
            auto* body = static_cast<std::string*>(user);
            if (body->size() + size * count > max_reply) return 0;
            body->append(data, size * count);
            return size * count;
        }

        std::string_view host_of(std::string_view rest) {
            auto end = rest.find_first_of("/?#");
            std::string_view authority = rest.substr(0, end);
            if (authority.starts_with('[')) return authority.substr(0, authority.find(']') + 1);
            return authority.substr(0, authority.rfind(':') == std::string_view::npos ? authority.size() : authority.rfind(':'));
        }

    }  // namespace

    std::string validated_base_url(std::string_view url) {
        std::string_view rest;
        bool plain = false;
        if (url.starts_with("https://")) rest = url.substr(8);
        else if (url.starts_with("http://")) {
            rest = url.substr(7);
            plain = true;
        } else throw HttpsError("the URL must start with https:// (http:// only for a loopback address)");
        if (rest.empty() || rest.front() == '/') throw HttpsError("the URL has no host");
        if (rest.substr(0, rest.find('/')).find('@') != std::string_view::npos) throw HttpsError("the URL must not contain credentials");
        if (rest.find_first_of("?#") != std::string_view::npos) throw HttpsError("the URL must not contain a query or fragment");
        if (plain) {
            auto host = host_of(rest);
            if (host != "localhost" && host != "[::1]" && !host.starts_with("127."))
                throw HttpsError("plain http:// is accepted only for a loopback address; use https://");
        }
        std::string out(url);
        while (out.ends_with('/')) out.pop_back();
        return out;
    }

    HttpsResponse https_request(const HttpsRequest& request) {
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
        if (!easy) throw HttpsError("curl_easy_init failed");
        HttpsResponse response;
        curl_slist* headers = nullptr;
        for (const auto& [name, value]: request.headers) headers = curl_slist_append(headers, std::format("{}: {}", name, value).c_str());
        if (!request.body.empty()) headers = curl_slist_append(headers, "Content-Type: application/json");
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_list(headers, curl_slist_free_all);
        CURL* e = easy.get();
        curl_easy_setopt(e, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "https,http");
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!request.ca_bundle.empty()) curl_easy_setopt(e, CURLOPT_CAINFO, request.ca_bundle.c_str());
        curl_easy_setopt(e, CURLOPT_TIMEOUT, request.timeout_seconds);
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (!request.body.empty()) {
            curl_easy_setopt(e, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        }
        if (header_list) curl_easy_setopt(e, CURLOPT_HTTPHEADER, header_list.get());
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, &response.body);
        CURLcode code = curl_easy_perform(e);
        if (code != CURLE_OK) {
            // The path only: the query never carries secrets here, but headers and bodies do.
            auto path_start = request.url.find('/', request.url.find("//") + 2);
            throw HttpsError(std::format("{} {}: {}", request.method, path_start == std::string::npos ? "/" : request.url.substr(path_start),
                                         curl_easy_strerror(code)));
        }
        curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &response.status);
        return response;
    }

}  // namespace fh
