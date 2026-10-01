#include "view/title_bar.hpp"

#include "borealis/views/applet_frame.hpp"
#include "core/account_info.hpp"
#include "core/utils/config.hpp"
#include "core/utils/image.hpp"
#include "view/account_details.hpp"
#include "view/side_panel.hpp"

namespace vkpcnx {

void CustomTitlebarActivity::setupTitleBar() {
  appletFrame->setIcon(brls::View::getFilePathXMLAttributeValue("@res/icon/icon.png"));

  brls::Box *header = appletFrame->getHeader();
  auto *titleBox = static_cast<brls::Box *>(header->getChildren().front());

  // Replace the titlebox with a custom view
  brls::Box *myTitleBox = new brls::Box();
  myTitleBox->setAxis(brls::Axis::COLUMN);
  myTitleBox->setJustifyContent(brls::JustifyContent::CENTER);

  brls::Label *title = new brls::Label();
  title->setText("VK Play Cloud");
  title->setFontSize(28);
  title->setMarginBottom(8);

  brls::Label *subtitle = new brls::Label();
  subtitle->setText("Неофициальный клиент для Switch");
  subtitle->setFontSize(14);
  subtitle->setAlpha(.5);

  myTitleBox->addView(title);
  myTitleBox->addView(subtitle);

  brls::Label *titleLabel = static_cast<brls::Label *>(titleBox->getChildren().back());
  titleBox->removeView(titleLabel);
  titleBox->addView(myTitleBox, 1);

  // Add the right-side view
  brls::Box *accountBox = new brls::Box();
  accountBox->setAxis(brls::Axis::ROW);
  accountBox->setAlignItems(brls::AlignItems::CENTER);
  accountBox->setMargins(16, 0, 16, 0);

  brls::Label *accountLabel = new brls::Label();
  brls::Image *accountAvatar = new brls::Image();
  accountLabel->setMarginLeft(8);
  accountAvatar->setWidth(56);
  accountAvatar->setHeight(56);
  accountAvatar->setCornerRadius(999);
  accountAvatar->setMarginLeft(16);
  accountAvatar->setMarginRight(8);

  accountBox->addView(accountLabel);
  accountBox->addView(accountAvatar);

  header->addView(new brls::Padding());
  header->addView(accountBox);

  // Hidden until there is an account to show (login screen, or before the
  // first fetch): an empty box would take the focus
  accountBox->setVisibility(brls::Visibility::GONE);
  accountBox->setFocusable(false);
  accountBox->addGestureRecognizer(new brls::TapGestureRecognizer(accountBox));
  accountBox->registerClickAction([](brls::View *view) {
    auto content = new AccountDetails();
    vkpcnx::SidePanel *panel = new vkpcnx::SidePanel(content, 480);
    panel->open();
    return true;
  });

  auto cb = [accountBox, accountLabel, accountAvatar](const nlohmann::json &account) {
    accountLabel->setText(account.at("name").get<std::string>());
    auto userId = account.at("session").at("user_id").get<int>();
    utils::loadImageFromUrl(accountAvatar, fmt::format(utils::AVATAR_URL_FMT, userId));
    accountBox->setVisibility(brls::Visibility::VISIBLE);
    accountBox->setFocusable(true);
  };

  auto &account = AccountInfo::instance();
  if (account.get())
    cb(*account.get());

  this->accountSub = account.updated().subscribe(cb);
}

} // namespace vkpcnx
