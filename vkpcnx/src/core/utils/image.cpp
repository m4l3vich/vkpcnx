#include "core/utils/image.hpp"

#include "core/http.hpp"
#include <list>
#include <unordered_map>

namespace vkpcnx::utils {

namespace {
constexpr size_t kMaxCachedImages = 128;

std::unordered_map<std::string, std::string> &imageDataCache() {
  static std::unordered_map<std::string, std::string> cache;
  return cache;
}

std::list<std::string> &imageDataCacheOrder() {
  static std::list<std::string> order;
  return order;
}

void cacheImageData(const std::string &url, const std::string &data) {
  auto &cache = imageDataCache();
  if (cache.count(url))
    return;

  auto &order = imageDataCacheOrder();
  if (cache.size() >= kMaxCachedImages) {
    cache.erase(order.front());
    order.pop_front();
  }

  cache[url] = data;
  order.push_back(url);
}
} // namespace

void loadImageFromUrl(brls::Image *image, std::string url, std::function<bool()> isValid) {
  auto &cache = imageDataCache();
  auto cached = cache.find(url);
  if (cached != cache.end()) {
    image->setImageFromMem(
      (const unsigned char *)cached->second.data(), (int)cached->second.size()
    );
    return;
  }

  image->setImageAsync([url, isValid](std::function<void(const std::string &, size_t)> setter) {
    Http::getAsync(url, [url, setter, isValid](Http::Response res) {
      if (!res.ok() || (isValid && !isValid()))
        return;
      cacheImageData(url, res.body);
      setter(res.body, res.body.size());
    });
  });
}

} // namespace vkpcnx::utils
