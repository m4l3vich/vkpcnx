#pragma once

#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

// Minimal libcurl-based HTTPS client.
//
// Synchronous calls block the calling thread — never call them from the UI
// thread. The async variants run on a worker thread and invoke the callback
// on the UI thread (via brls::sync), so it's safe to touch views from there.
class Http {
public:
  using Headers = std::map<std::string, std::string>;

  struct Response {
    long status = 0; // HTTP status, 0 if the request never completed
    std::string body;
    Headers headers;
    std::string error; // curl error message, empty on success

    bool ok() const { return error.empty() && status >= 200 && status < 300; }

    // Parses body as JSON; returns null json (and sets `err`) on failure
    nlohmann::json json(std::string *err = nullptr) const;
  };

  using Callback = std::function<void(Response)>;

  // Must be called once at startup / shutdown (main thread)
  static void init();
  static void shutdown();

  // Default headers sent with every request (e.g. Authorization)
  static void setDefaultHeader(const std::string &name, const std::string &value);
  static void removeDefaultHeader(const std::string &name);
  static void setUserAgent(const std::string &ua);
  static void setTimeout(long seconds);

  // Blocking
  static Response get(const std::string &url, const Headers &headers = {});
  static Response del(const std::string &url, const Headers &headers = {});
  static Response post(
    const std::string &url,
    const std::string &body,
    const std::string &contentType = "application/json",
    const Headers &headers = {}
  );
  static Response
  postJson(const std::string &url, const nlohmann::json &body, const Headers &headers = {});

  // Non-blocking; callback runs on the UI thread
  static void getAsync(const std::string &url, Callback cb, const Headers &headers = {});
  static void postAsync(
    const std::string &url,
    const std::string &body,
    Callback cb,
    const std::string &contentType = "application/json",
    const Headers &headers = {}
  );
  static void postJsonAsync(
    const std::string &url, const nlohmann::json &body, Callback cb, const Headers &headers = {}
  );

  static std::string urlEncode(const std::string &s);

private:
  static Response perform(
    const std::string &method,
    const std::string &url,
    const std::string *body,
    const std::string &contentType,
    const Headers &headers
  );
  static void runAsync(std::function<Response()> work, Callback cb);
};
