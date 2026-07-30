#pragma once

#include <string>

namespace hyperion {

struct HttpResult {
  long status = 0;
  std::string body;
  std::string error;
  bool ok() const { return status >= 200 && status < 300 && error.empty(); }
};

HttpResult http_get(const std::string& url, long timeout_ms = 3000);
HttpResult http_request(const std::string& method, const std::string& url,
                        const std::string& body = {},
                        long timeout_ms = 3000);
inline HttpResult http_post(const std::string& url, const std::string& body,
                            long timeout_ms = 3000) {
  return http_request("POST", url, body, timeout_ms);
}
inline HttpResult http_delete(const std::string& url, long timeout_ms = 3000) {
  return http_request("DELETE", url, {}, timeout_ms);
}

}  // namespace hyperion
