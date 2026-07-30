#include "http_client.h"

#include <curl/curl.h>

namespace hyperion {
namespace {

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb,
                     void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  const std::size_t n = size * nmemb;
  out->append(ptr, n);
  return n;
}

}  // namespace

HttpResult http_get(const std::string& url, long timeout_ms) {
  return http_request("GET", url, {}, timeout_ms);
}

HttpResult http_request(const std::string& method, const std::string& url,
                        const std::string& body, long timeout_ms) {
  HttpResult result;
  CURL* curl = curl_easy_init();
  if (!curl) {
    result.error = "curl_easy_init failed";
    return result;
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "hyperion/0.1");
  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
  }

  const CURLcode code = curl_easy_perform(curl);
  if (code != CURLE_OK) {
    result.error = curl_easy_strerror(code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return result;
}

}  // namespace hyperion
