#include "core/diag/report.hpp"

#include "core/diag/build_info.hpp"
#include "core/diag/log.hpp"
#include "core/diag/redact.hpp"
#include "core/diag/zip_writer.hpp"
#include "core/settings.hpp"

#include <borealis/core/logger.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace vkpcnx::diag {

namespace {

constexpr size_t kLogBudget = 24 * 1024 * 1024; // uncompressed, all logs together
constexpr size_t kMaxReports = 5;
[[maybe_unused]] constexpr size_t kMaxCrashReports = 5; // Switch

std::string readFile(const std::string &path, size_t maxBytes, bool *truncated = nullptr) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  in.seekg(0, std::ios::end);
  auto size = static_cast<size_t>(std::max<std::streamoff>(0, in.tellg()));
  size_t skip = size > maxBytes ? size - maxBytes : 0;
  if (truncated)
    *truncated = skip > 0;
  in.seekg(static_cast<std::streamoff>(skip));
  std::string data(size - skip, '\0');
  in.read(&data[0], static_cast<std::streamsize>(data.size()));
  data.resize(static_cast<size_t>(in.gcount()));
  return data;
}

std::string lower(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string timestamp(const char *format) {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof buf, format, &tm);
  return buf;
}

void redactJson(nlohmann::json &j) {
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == "account")
        it.value() = it.value().is_null() ? nlohmann::json() : nlohmann::json("<redacted>");
      else if (isSecretKey(it.key()) && !it.value().is_null())
        it.value() = "<redacted>";
      else
        redactJson(it.value());
    }
  } else if (j.is_array()) {
    for (auto &v : j)
      redactJson(v);
  } else if (j.is_string()) {
    j = redact(j.get<std::string>());
  }
}

std::string redactedSettings() {
  std::ifstream in(Settings::configFile());
  if (!in)
    return "{}\n";
  try {
    nlohmann::json j = nlohmann::json::parse(in);
    redactJson(j);
    return j.dump(2) + "\n";
  } catch (const std::exception &e) {
    return std::string("settings.json could not be parsed: ") + e.what() + "\n";
  }
}

// Build IDs named in the logs' headers, to pick out matching crash reports
std::set<std::string> buildIdsIn(const std::string &log) {
  std::set<std::string> ids;
  size_t pos = 0;
  while ((pos = log.find("build id:", pos)) != std::string::npos) {
    size_t start = log.find_first_not_of(' ', pos + 9);
    size_t end = log.find_first_of("\r\n ", start);
    if (start != std::string::npos) {
      std::string id = lower(log.substr(start, end - start));
      if (id.size() >= 16 && id != "unknown")
        ids.insert(id);
    }
    pos += 9;
  }
  return ids;
}

struct Item {
  std::string name;
  std::string data;
  std::string note;
};

// Newest-first files in `dir` whose names pass `match`
std::vector<fs::path> listFiles(const fs::path &dir, bool (*match)(const std::string &)) {
  std::vector<std::pair<fs::file_time_type, fs::path>> found;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::string name = it->path().filename().string();
    if (!match(name))
      continue;
    std::error_code tec;
    auto t = fs::last_write_time(it->path(), tec);
    found.emplace_back(tec ? fs::file_time_type::min() : t, it->path());
  }
  std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
    return a.first > b.first;
  });
  std::vector<fs::path> out;
  for (auto &f : found)
    out.push_back(f.second);
  return out;
}

void addCrashReports(std::vector<Item> &items, const std::set<std::string> &buildIds) {
#if defined(__SWITCH__)
  // Atmosphère names reports by time and the host title's program id, so the
  // build ids (its "Module Id") are what ties a report to vkpcnx
  auto files = listFiles("sdmc:/atmosphere/crash_reports", [](const std::string &n) {
    return n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0;
  });
  size_t added = 0;
  for (size_t i = 0; i < files.size() && i < 40 && added < kMaxCrashReports; i++) {
    std::string text = readFile(files[i].string(), 1024 * 1024);
    std::string lowered = lower(text);
    bool ours = std::any_of(buildIds.begin(), buildIds.end(), [&](const std::string &id) {
      return lowered.find(id.substr(0, 32)) != std::string::npos;
    });
    if (!ours)
      continue;
    items.push_back({"crash_reports/" + files[i].filename().string(), text, ""});
    added++;
  }
#elif defined(__APPLE__)
  (void)buildIds;
  const char *home = std::getenv("HOME");
  if (!home)
    return;
  auto files = listFiles(fs::path(home) / "Library/Logs/DiagnosticReports", [](const std::string &n) {
    return n.rfind("vkpcnx", 0) == 0 && (n.find(".ips") != std::string::npos ||
                                         n.find(".crash") != std::string::npos);
  });
  for (size_t i = 0; i < files.size() && i < 3; i++)
    items.push_back(
      {"crash_reports/" + files[i].filename().string(), redact(readFile(files[i].string(), 4 << 20)), ""}
    );
#else
  (void)items;
  (void)buildIds;
#endif
}

void pruneReports() {
  auto files = listFiles(reportDir(), [](const std::string &n) {
    return n.rfind("vkpcnx-report-", 0) == 0;
  });
  for (size_t i = kMaxReports; i < files.size(); i++) {
    std::error_code ec;
    fs::remove(files[i], ec);
  }
}

} // namespace

std::string reportDir() {
  std::string dir = Settings::configDir() + "/reports";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

ReportResult createReport(const std::string &reason) {
  ReportResult result;
  brls::Logger::info("Report: creating ({})", reason);
  flushLogs();

  std::vector<Item> items;
  std::set<std::string> buildIds;
  if (!buildInfo().buildId.empty())
    buildIds.insert(lower(buildInfo().buildId));

  size_t budget = kLogBudget;
  for (const std::string &path : logFiles()) {
    if (budget == 0)
      break;
    bool truncated = false;
    std::string data = readFile(path, budget, &truncated);
    budget -= std::min(budget, data.size());
    auto ids = buildIdsIn(data);
    buildIds.insert(ids.begin(), ids.end());
    std::string name = "logs/" + fs::path(path).filename().string();
    if (truncated)
      data = "--- beginning of this log omitted (report size limit) ---\n" + data;
    items.push_back({name, std::move(data), truncated ? "tail only" : ""});
    if (items.size() >= kMaxLogFiles)
      break;
  }
  items.push_back({"settings.json", redactedSettings(), "account and secrets removed"});
  addCrashReports(items, buildIds);

  const PreviousRun &prev = previousRun();
  std::ostringstream txt;
  txt << "vkpcnx debug report\n"
      << "created:          " << timestamp("%Y-%m-%d %H:%M:%S %z") << "\n"
      << "reason:           " << reason << "\n";
  for (const auto &[key, value] : systemInfo()) {
    std::string label = key + ":";
    label.resize(std::max<size_t>(label.size(), 17), ' ');
    txt << label << " " << value << "\n";
  }
  txt << "previous run:     "
      << (prev.unclean ? (prev.crashed ? "crashed" : "did not exit cleanly") : "exited cleanly")
      << (prev.logPath.empty() ? "" : " (" + fs::path(prev.logPath).filename().string() + ")")
      << "\n"
      << "current log:      " << fs::path(currentLogPath()).filename().string() << "\n\n"
      << "files:\n";
  for (const Item &item : items)
    txt << "  " << item.name << "  " << item.data.size() << " bytes"
        << (item.note.empty() ? "" : "  (" + item.note + ")") << "\n";
  txt << "\nTokens, passwords, cookies and public IP addresses are removed from these\n"
         "files. Attach this .zip to an issue at github.com/m4l3vich/vkpcnx/issues or\n"
         "send it to t.me/vkpcnx, together with what you did and what went wrong.\n";

  std::string dir = reportDir();
  result.path = dir + "/vkpcnx-report-" + timestamp("%Y%m%d-%H%M%S") + ".zip";
  {
    ZipWriter zip(result.path);
    bool ok = zip.add("report.txt", txt.str());
    for (const Item &item : items)
      ok = ok && zip.add(item.name, item.data);
    ok = ok && zip.finish();
    if (!ok) {
      result.error = "cannot write " + result.path;
      brls::Logger::error("Report: {}", result.error);
      std::error_code ec;
      fs::remove(result.path, ec);
      return result;
    }
  }
  std::error_code ec;
  result.bytes = static_cast<size_t>(fs::file_size(result.path, ec));
  result.ok = true;
  pruneReports();
  brls::Logger::info("Report: wrote {} ({} bytes)", result.path, result.bytes);
  return result;
}

void revealInFileManager(const std::string &path) {
#if defined(__SWITCH__)
  (void)path;
#else
#if defined(__APPLE__)
  std::string cmd = "open -R \"" + path + "\"";
#elif defined(_WIN32)
  std::string native = path;
  std::replace(native.begin(), native.end(), '/', '\\');
  std::string cmd = "explorer /select,\"" + native + "\"";
#else
  std::string cmd = "xdg-open \"" + fs::path(path).parent_path().string() + "\" &";
#endif
  // explorer exits with 1 even when it worked, so the status says nothing
  (void)std::system(cmd.c_str());
#endif
}

} // namespace vkpcnx::diag
