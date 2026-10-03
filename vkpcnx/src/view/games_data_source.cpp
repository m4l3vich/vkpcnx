#include "view/games_data_source.hpp"
#include "view/game_card_row.hpp"

namespace vkpcnx {
GamesDataSource::GamesDataSource(OnNeedMore needMoreCallback) {
  this->needMoreCallback = needMoreCallback;
  this->needMoreCallback([this](GamesList games) {
    this->games = std::move(games);

    if (this->recycler)
      this->recycler->reloadData();
  });
}

int GamesDataSource::numberOfRows(brls::RecyclerFrame *recycler, int section) {
  this->recycler = recycler;
  return (int)((games.size() + columns - 1) / columns);
}

brls::RecyclerCell *
GamesDataSource::cellForRow(brls::RecyclerFrame *recycler, brls::IndexPath index) {
  auto *cell = (GameCardRow *)recycler->dequeueReusableCell("GameCardRow");

  std::vector<nlohmann::json> rowGames;
  size_t start = (size_t)index.row * columns;
  for (int i = 0; i < columns && start + i < games.size(); i++)
    rowGames.push_back(games[start + i]);

  cell->setGames(rowGames);
  if (pendingFocusRow && index.row == pendingFocusRow->row) {
    brls::Application::giveFocus(cell->getDefaultFocus());
    pendingFocusRow.reset();
  }

  int numberOfRows = this->numberOfRows(recycler, index.section);
  if (index.row >= numberOfRows - 2) {
    this->needMoreCallback([this](GamesList moreGames) {
      this->games.reserve(this->games.size() + moreGames.size());
      this->games.insert(
        this->games.end(),
        std::make_move_iterator(moreGames.begin()),
        std::make_move_iterator(moreGames.end())
      );

      moreGames.clear();
      this->reloadRecycler();
    });
  }

  return cell;
}

float GamesDataSource::heightForRow(brls::RecyclerFrame *recycler, brls::IndexPath index) {
  return GameCardRow::height;
}

void GamesDataSource::reloadRecycler() {
  brls::View *focused = brls::Application::getCurrentFocus();
  brls::RecyclerCell *focusedCell = nullptr;
  for (brls::View *v = focused; v != nullptr; v = v->getParent()) {
    focusedCell = dynamic_cast<brls::RecyclerCell *>(v);
    if (focusedCell)
      break;
  }
  if (focusedCell)
    this->pendingFocusRow = focusedCell->getIndexPath();

  float offset = this->recycler->getContentOffsetY();
  this->recycler->reloadData();
  this->recycler->setContentOffsetY(offset, false);
}
} // namespace vkpcnx
