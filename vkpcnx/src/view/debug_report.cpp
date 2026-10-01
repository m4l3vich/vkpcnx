#include "view/debug_report.hpp"

#include "core/diag/log.hpp"
#include "core/diag/report.hpp"
#include "core/utils/thread.hpp"

#include <atomic>
#include <borealis.hpp>

namespace vkpcnx {

namespace {

std::atomic<bool> building{false};

void showResult(const diag::ReportResult &r) {
  std::string text;
  if (r.ok) {
    text = fmt::format(
      "Отчёт сохранён:\n{}\n({} КБ)\n\n"
#ifdef __SWITCH__
      "Скопируйте файл с SD-карты (по USB или FTP) и "
#else
      "Пожалуйста, "
#endif
      "приложите его к issue на GitHub или отправьте в Telegram, описав, что произошло.",
      r.path,
      (r.bytes + 1023) / 1024
    );
  } else {
    text = "Не удалось создать отчёт: " + r.error;
  }
  auto *dialog = new brls::Dialog(text);
#ifndef __SWITCH__
  if (r.ok)
    dialog->addButton("Показать файл", [path = r.path] { diag::revealInFileManager(path); });
#endif
  dialog->addButton("Закрыть", [] {});
  dialog->setCancelable(true);
  dialog->open();
}

void build(const std::string &reason) {
  if (building.exchange(true))
    return;
  brls::Application::notify("Создаю отчёт об ошибке…");
  vkpcnx::utils::runDetached([reason] {
    diag::ReportResult r = diag::createReport(reason);
    building = false;
    brls::sync([r] { showResult(r); });
  });
}

} // namespace

void openDebugReportDialog() {
  auto *dialog = new brls::Dialog(
    "Будет создан файл с журналами работы приложения и сведениями о системе. Он поможет "
    "найти причину ошибки.\n\nТокены, пароли и IP-адреса из него удаляются автоматически, "
    "а сам файл никуда не отправляется — поделиться им можно вручную."
  );
  dialog->addButton("Отмена", [] {});
  dialog->addButton("Создать отчёт", [] { build("manual"); });
  dialog->setCancelable(true);
  dialog->open();
}

void offerReportAfterUncleanExit() {
  diag::PreviousRun prev = diag::previousRun();
  if (!prev.unclean)
    return;
  diag::dismissPreviousRun();
  std::string reason = prev.crashed ? "after crash" : "after unclean exit";
  auto *dialog = new brls::Dialog(
    std::string(prev.crashed ? "При прошлом запуске приложение аварийно завершилось."
                             : "Прошлый запуск приложения завершился некорректно "
                               "(зависание или принудительное закрытие).") +
    "\n\nСоздать отчёт об ошибке? Он сохранится в файл, которым можно поделиться "
    "с разработчиком."
  );
  dialog->addButton("Не сейчас", [] {});
  dialog->addButton("Создать отчёт", [reason] { build(reason); });
  dialog->setCancelable(true);
  dialog->open();
}

} // namespace vkpcnx
