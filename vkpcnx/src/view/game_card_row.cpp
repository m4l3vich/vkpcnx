#include "view/game_card_row.hpp"
#include "view/game_card.hpp"
#include <algorithm>

namespace vkpcnx {
GameCardRow::GameCardRow() {
  this->setAxis(brls::Axis::ROW);
  this->setLineBottom(0);
  this->setHeight(GameCardRow::height);
  this->setWidth(brls::View::AUTO);
}

void GameCardRow::setGames(const std::vector<nlohmann::json> &games) {
  while (cards.size() < games.size()) {
    auto *card = new GameCard();
    this->addView(card);
    cards.push_back(card);
  }

  for (size_t i = 0; i < cards.size(); i++) {
    if (i < games.size()) {
      auto &game = games[i];
      GameCard::Game gameStruct;
      gameStruct.id = game.at("id").get<int>();
      gameStruct.name = game.at("name").get<std::string>();
      gameStruct.pictureUrl =
        game.contains("picture") && game["picture"].is_string() ? game["picture"].get<std::string>() : "";
      gameStruct.gamepadSupport = game.contains("gamepad_support") && game["gamepad_support"].is_boolean()
        ? game["gamepad_support"].get<bool>()
        : false;
      if (game.contains("current_launcher") && game["current_launcher"].is_number())
        gameStruct.launcherId = game["current_launcher"].get<int>();
      else if (game.contains("game_launchers") && game["game_launchers"].is_array() &&
               !game["game_launchers"].empty())
        gameStruct.launcherId = game["game_launchers"][0].value("id", 0);

      cards[i]->setGame(gameStruct);
      cards[i]->setVisibility(brls::Visibility::VISIBLE);
    } else {
      cards[i]->setVisibility(brls::Visibility::INVISIBLE);
    }
  }
}

brls::View *GameCardRow::getDefaultFocus() {
  if (cards.empty())
    return Box::getDefaultFocus();

  int column = std::min(focusedColumn, (int)cards.size() - 1);
  while (column > 0 && cards[column]->getVisibility() != brls::Visibility::VISIBLE)
    column--;

  return cards[column]->getDefaultFocus();
}

void GameCardRow::onChildFocusGained(brls::View *directChild, brls::View *focusedView) {
  auto it = std::find(cards.begin(), cards.end(), directChild);
  if (it != cards.end())
    focusedColumn = (int)std::distance(cards.begin(), it);

  Box::onChildFocusGained(directChild, focusedView);
}

brls::RecyclerCell *GameCardRow::create() { return new GameCardRow(); }
} // namespace vkpcnx
