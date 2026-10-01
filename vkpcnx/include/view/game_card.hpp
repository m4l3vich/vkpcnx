#pragma once
#include <borealis.hpp>
#include <memory>

namespace vkpcnx {
class GameCard : public brls::Box {
public:
  GameCard();

  struct Game {
    int id = 0;
    std::string name;
    std::string pictureUrl;
    int launcherId = 0; // game_launchers[].id / current_launcher, used for queue/run
    bool gamepadSupport = false;
  };
  void setGame(Game game);

private:
  BRLS_BIND(brls::Image, image, "game_card/image");
  BRLS_BIND(brls::Label, label, "game_card/label");

  Game game;

  // Bumped on every setGame() so an in-flight image load from a previous
  // (recycled) card contents can detect it's stale and skip applying itself.
  std::shared_ptr<int> loadGeneration = std::make_shared<int>(0);
};
}; // namespace vkpcnx
