#include "view/game_card.hpp"
#include "activity/launch_queue_activity.hpp"
#include "core/utils/image.hpp"

namespace vkpcnx {
GameCard::GameCard() {
  this->inflateFromXMLRes("xml/views/game_card.xml");
  this->setFocusable(true);

  this->addGestureRecognizer(new brls::TapGestureRecognizer(this));
  this->registerClickAction([this](brls::View *view) {
    brls::Application::pushActivity(new LaunchQueueActivity(this->game));
    return true;
  });
}

void GameCard::setGame(Game game) {
  this->game = game;
  this->label->setText(game.name);
  this->image->clear();

  int generation = ++(*this->loadGeneration);
  std::shared_ptr<int> token = this->loadGeneration;
  vkpcnx::utils::loadImageFromUrl(this->image, game.pictureUrl, [token, generation] {
    return *token == generation;
  });
}
} // namespace vkpcnx
