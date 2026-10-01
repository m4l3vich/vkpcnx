#include "core/http.hpp"

#include "core/utils/thread.hpp"
#include <borealis.hpp>
#include <curl/curl.h>
#include <mutex>
#include <thread>

static std::mutex g_mutex;
static Http::Headers g_defaultHeaders;
static std::string g_userAgent = "VKPlayCloudNX/1.0";
static long g_timeout = 30;

// ---- curl callbacks --------------------------------------------------------

static size_t writeBody(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *out = static_cast<std::string *>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

static size_t writeHeader(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *out = static_cast<Http::Headers *>(userdata);
  std::string line(ptr, size * nmemb);
  auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    auto trim = [](std::string &s) {
      const char *ws = " \t\r\n";
      s.erase(0, s.find_first_not_of(ws));
      s.erase(s.find_last_not_of(ws) + 1);
    };
    trim(name);
    trim(value);
    for (auto &c : name)
      c = static_cast<char>(tolower(c));
    (*out)[name] = value;
  }
  return size * nmemb;
}

// ---- lifecycle -------------------------------------------------------------

void Http::init() {
  // On Switch, socketInitializeDefault() is already done by switch_wrapper.c
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

void Http::shutdown() { curl_global_cleanup(); }

void Http::setDefaultHeader(const std::string &name, const std::string &value) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_defaultHeaders[name] = value;
}

void Http::removeDefaultHeader(const std::string &name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_defaultHeaders.erase(name);
}

void Http::setUserAgent(const std::string &ua) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_userAgent = ua;
}

void Http::setTimeout(long seconds) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_timeout = seconds;
}

// ---- core ------------------------------------------------------------------

Http::Response Http::perform(
  const std::string &method,
  const std::string &url,
  const std::string *body,
  const std::string &contentType,
  const Headers &headers
) {
  Response res;

  CURL *curl = curl_easy_init();
  if (!curl) {
    res.error = "curl_easy_init failed";
    return res;
  }

  Headers merged;
  std::string userAgent;
  long timeout;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    merged = g_defaultHeaders;
    userAgent = g_userAgent;
    timeout = g_timeout;
  }
  for (auto &[k, v] : headers)
    merged[k] = v;
  if (body && !contentType.empty())
    merged["Content-Type"] = contentType;

  struct curl_slist *list = nullptr;
  for (auto &[k, v] : merged)
    list = curl_slist_append(list, (k + ": " + v).c_str());

  char errbuf[CURL_ERROR_SIZE] = {0};

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res.headers);

  if (body) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
  }

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    res.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    brls::Logger::error("Http: {} {} failed: {}", method, url, res.error);
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
    brls::Logger::debug("Http: {} {} -> {}", method, url, res.status);
  }

  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return res;
}

void Http::runAsync(std::function<Response()> work, Callback cb) {
  vkpcnx::utils::runDetached([work = std::move(work), cb = std::move(cb)]() {
    Response res = work();
    if (cb)
      brls::sync([cb, res = std::move(res)]() mutable { cb(std::move(res)); });
  });
}

// ---- public API ------------------------------------------------------------

nlohmann::json Http::Response::json(std::string *err) const {
  try {
    return nlohmann::json::parse(body);
  } catch (const std::exception &e) {
    if (err)
      *err = e.what();
    return nullptr;
  }
}

Http::Response Http::get(const std::string &url, const Headers &headers) {
  return perform("GET", url, nullptr, "", headers);
}

Http::Response Http::del(const std::string &url, const Headers &headers) {
  return perform("DELETE", url, nullptr, "", headers);
}

Http::Response Http::post(
  const std::string &url,
  const std::string &body,
  const std::string &contentType,
  const Headers &headers
) {
  return perform("POST", url, &body, contentType, headers);
}

Http::Response
Http::postJson(const std::string &url, const nlohmann::json &body, const Headers &headers) {
  return post(url, body.dump(), "application/json", headers);
}

void Http::getAsync(const std::string &url, Callback cb, const Headers &headers) {
  runAsync([url, headers] { return get(url, headers); }, std::move(cb));
}

void Http::postAsync(
  const std::string &url,
  const std::string &body,
  Callback cb,
  const std::string &contentType,
  const Headers &headers
) {
  runAsync(
    [url, body, contentType, headers] { return post(url, body, contentType, headers); },
    std::move(cb)
  );
}

void Http::postJsonAsync(
  const std::string &url, const nlohmann::json &body, Callback cb, const Headers &headers
) {
  postAsync(url, body.dump(), std::move(cb), "application/json", headers);
}

std::string Http::urlEncode(const std::string &s) {
  CURL *curl = curl_easy_init();
  if (!curl)
    return s;
  char *enc = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
  std::string out = enc ? enc : s;
  curl_free(enc);
  curl_easy_cleanup(curl);
  return out;
}
