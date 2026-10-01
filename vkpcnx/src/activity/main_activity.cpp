#include "activity/main_activity.hpp"
#include "borealis/core/view.hpp"
#include "core/account_info.hpp"
#include "core/utils/thread.hpp"
#include "core/vkpcapi.hpp"
#include "view/game_card_row.hpp"
#include "view/games_data_source.hpp"
#include <string>

using namespace brls::literals;

// After login this activity sits on top of LoginActivity (borealis never
// pops the root), so the frame's default B would pop back to the login
// screen. Behave like a root activity instead: offer to exit the app.
void MainActivity::overrideBackAction() {
  appletFrame->registerAction(
    "hints/exit"_i18n,
    brls::BUTTON_B,
    [](brls::View *) {
      auto *dialog = new brls::Dialog("hints/exit_hint"_i18n);
      dialog->addButton("hints/cancel"_i18n, [] {});
      dialog->addButton("hints/ok"_i18n, [] { brls::Application::quit(); });
      dialog->setCancelable(true);
      dialog->open();
      return true;
    },
    true,
    false,
    brls::SOUND_BACK
  );
}

void MainActivity::renderGamesList() {
  vkpcnx::GamesDataSource::OnNeedMore onNeedMore =
    [this](vkpcnx::GamesDataSource::MoreGamesCallback cb) { this->fetchMoreGames(cb); };

  this->content->registerCell("GameCardRow", []() { return vkpcnx::GameCardRow::create(); });
  this->content->setDataSource(new vkpcnx::GamesDataSource(onNeedMore));
}

void MainActivity::fetchMoreGames(vkpcnx::GamesDataSource::MoreGamesCallback cb) {
  if (this->loading || !this->hasMore)
    return;

  this->loading = true;
  this->page++;

  vkpcnx::utils::runDetached([cb = std::move(cb), this] {
    auto &account = AccountInfo::instance().get();
    auto params =
      VKPCAPI::QueryParams{{"page", std::to_string(page)}, {"pageSize", "24"}, {"o", "-priority"}};

    // TODO: indicate on main screen if user has no plan
    if (account->contains("plan") && !account->at("plan").is_null()) {
      params["plan_family_slug"] = account->at("plan").at("plan_family").get<std::string>();
    }

    auto games = VKPCAPI::indexGames(params);
    // TODO: error handling

    vkpcnx::GamesDataSource::GamesList gamesList;
    if (!games.is_null())
      for (auto &game : games.at("results").items())
        gamesList.push_back(game.value());

    brls::sync([this, cb, gamesList = std::move(gamesList)]() mutable {
      bool empty = gamesList.empty();
      cb(std::move(gamesList));

      this->loading = false;
      if (empty)
        this->hasMore = false;

      if (this->page == 1) {
        this->loader->setVisibility(brls::Visibility::GONE);
        this->content->setVisibility(brls::Visibility::VISIBLE);
      }
    });
  });
}