#pragma once
#include <borealis.hpp>
#include <nlohmann/json.hpp>
#include <vector>

namespace vkpcnx {
class GamesDataSource : public brls::RecyclerDataSource {
public:
  using GamesList = std::vector<nlohmann::json>;
  using MoreGamesCallback = std::function<void(std::vector<nlohmann::json> gamesList)>;
  using OnNeedMore = std::function<void(MoreGamesCallback cb)>;
  explicit GamesDataSource(OnNeedMore onNeedMoreFn);

  int numberOfRows(brls::RecyclerFrame *recycler, int section) override;
  brls::RecyclerCell *cellForRow(brls::RecyclerFrame *recycler, brls::IndexPath index) override;

  float heightForRow(brls::RecyclerFrame *recycler, brls::IndexPath index) override;

private:
  static constexpr int columns = 4;

  GamesList games;
  OnNeedMore needMoreCallback;
  brls::RecyclerFrame *recycler = nullptr;
  std::optional<brls::IndexPath> pendingFocusRow;

  void reloadRecycler();
};
}; // namespace vkpcnx
