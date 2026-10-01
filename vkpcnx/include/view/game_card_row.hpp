#pragma once
#include "view/game_card.hpp"
#include <borealis.hpp>
#include <nlohmann/json.hpp>
#include <vector>

namespace vkpcnx {
class GameCardRow : public brls::RecyclerCell {
public:
  GameCardRow();
  void setGames(const std::vector<nlohmann::json> &games);
  static brls::RecyclerCell *create();
  static constexpr float height = 220;

  brls::View *getDefaultFocus() override;
  void onChildFocusGained(brls::View *directChild, brls::View *focusedView) override;

private:
  std::vector<GameCard *> cards;
  static inline int focusedColumn = 0;
};
}; // namespace vkpcnx
