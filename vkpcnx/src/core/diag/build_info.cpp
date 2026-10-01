#include "core/diag/build_info.hpp"

#include "vkpcnx_version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#if defined(__SWITCH__)
#include <switch.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <elf.h>
#include <link.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifndef VKPCNX_VERSION
#define VKPCNX_VERSION "0.0.0"
#endif
#ifndef VKPCNX_BUILD_TYPE
#define VKPCNX_BUILD_TYPE "unknown"
#endif

namespace vkpcnx::diag {

static std::string hex(const uint8_t *data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; i++) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 15];
  }
  return out;
}

static std::string addrHex(uintptr_t v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%#llx", static_cast<unsigned long long>(v));
  return buf;
}

#if defined(__SWITCH__)

static void probeImage(BuildInfo &b) {
  // The NRO is mapped from file offset 0, header included; NroHeader::build_id
  // (offset 0x40, 32 bytes) is what Atmosphère's crash report calls Module Id.
  MemoryInfo info{};
  u32 pageInfo;
  svcQueryMemory(&info, &pageInfo, reinterpret_cast<u64>(&probeImage));
  b.moduleBase = info.addr;
  const auto *header = reinterpret_cast<const uint8_t *>(info.addr);
  if (std::memcmp(header + 0x10, "NRO0", 4) == 0) {
    size_t len = 0x20;
    while (len > 0 && header[0x40 + len - 1] == 0)
      len--;
    b.buildId = hex(header + 0x40, len);
  }
}

#elif defined(_WIN32)

static void probeImage(BuildInfo &b) {
  auto *base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
  b.moduleBase = reinterpret_cast<uintptr_t>(base);
  // linkBase stays 0: the loader rewrites ImageBase in the mapped header, so
  // the triage script reads the preferred base from the .exe instead.
  auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  auto *entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY *>(base + dir.VirtualAddress);
  for (size_t i = 0; dir.VirtualAddress && i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); i++) {
    if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !entries[i].AddressOfRawData)
      continue;
    const uint8_t *cv = base + entries[i].AddressOfRawData;
    if (std::memcmp(cv, "RSDS", 4) == 0) // GUID (16) + age (4), from ld --build-id
      b.buildId = hex(cv + 4, 20);
  }
}

#elif defined(__APPLE__)

static void probeImage(BuildInfo &b) {
  auto *header = reinterpret_cast<const mach_header_64 *>(_dyld_get_image_header(0));
  b.moduleBase = reinterpret_cast<uintptr_t>(header);
  b.linkBase = b.moduleBase - static_cast<uintptr_t>(_dyld_get_image_vmaddr_slide(0));
  auto *cmd = reinterpret_cast<const load_command *>(header + 1);
  for (uint32_t i = 0; i < header->ncmds; i++) {
    if (cmd->cmd == LC_UUID) {
      b.buildId = hex(reinterpret_cast<const uuid_command *>(cmd)->uuid, 16);
      break;
    }
    cmd = reinterpret_cast<const load_command *>(reinterpret_cast<const uint8_t *>(cmd) +
                                                 cmd->cmdsize);
  }
}

#else

static int phdrCallback(dl_phdr_info *info, size_t, void *data) {
  auto &b = *static_cast<BuildInfo *>(data);
  b.moduleBase = info->dlpi_addr; // the main program comes first
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const auto &ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_NOTE)
      continue;
    auto *p = reinterpret_cast<const uint8_t *>(info->dlpi_addr + ph.p_vaddr);
    auto *end = p + ph.p_memsz;
    while (p + sizeof(ElfW(Nhdr)) <= end) {
      auto *note = reinterpret_cast<const ElfW(Nhdr) *>(p);
      const uint8_t *name = p + sizeof(ElfW(Nhdr));
      const uint8_t *desc = name + ((note->n_namesz + 3) & ~3u);
      if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
          std::memcmp(name, "GNU", 4) == 0) {
        b.buildId = hex(desc, note->n_descsz);
        return 1;
      }
      p = desc + ((note->n_descsz + 3) & ~3u);
    }
  }
  return 1;
}

static void probeImage(BuildInfo &b) { dl_iterate_phdr(phdrCallback, &b); }

#endif

const BuildInfo &buildInfo() {
  static const BuildInfo info = [] {
    BuildInfo b;
    b.version = VKPCNX_VERSION;
    b.git = VKPCNX_GIT_DESCRIBE;
    b.buildType = VKPCNX_BUILD_TYPE;
#if defined(__SWITCH__)
    b.platform = "switch-aarch64";
#else
#if defined(_WIN32)
    b.platform = "windows";
#elif defined(__APPLE__)
    b.platform = "macos";
#else
    b.platform = "linux";
#endif
#if defined(__aarch64__)
    b.platform += "-arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    b.platform += "-x86_64";
#endif
#endif
#if defined(__clang__)
    b.compiler = "clang " __clang_version__;
#elif defined(__GNUC__)
    b.compiler = "gcc " __VERSION__;
#endif
    probeImage(b);
    return b;
  }();
  return info;
}

std::string buildSummary() {
  const auto &b = buildInfo();
  return b.version + " (" + b.git + ", " + b.buildType + ", " + b.platform + ")";
}

static std::mutex infoMutex;
static std::vector<std::pair<std::string, std::string>> extraInfo;

void setSystemInfo(const std::string &key, const std::string &value) {
  std::lock_guard<std::mutex> lock(infoMutex);
  for (auto &kv : extraInfo)
    if (kv.first == key) {
      kv.second = value;
      return;
    }
  extraInfo.emplace_back(key, value);
}

static std::vector<std::pair<std::string, std::string>> collectSystemInfo() {
  std::vector<std::pair<std::string, std::string>> out;
  auto add = [&](const char *key, const std::string &value) {
    if (!value.empty())
      out.emplace_back(key, value);
  };
  [[maybe_unused]] char buf[256];

#if defined(__SWITCH__)
  u32 hos = hosversionGet();
  std::snprintf(buf, sizeof buf, "%u.%u.%u", HOSVER_MAJOR(hos), HOSVER_MINOR(hos),
                HOSVER_MICRO(hos));
  add("firmware", buf);
  if (hosversionIsAtmosphere()) {
    std::string ams = "yes";
    if (R_SUCCEEDED(splInitialize())) {
      u64 v = 0;
      // 65000 = ExosphereApiVersion: major.minor.micro in the top three bytes
      if (R_SUCCEEDED(splGetConfig(static_cast<SplConfigItem>(65000), &v))) {
        std::snprintf(buf, sizeof buf, "%u.%u.%u", unsigned((v >> 56) & 0xff),
                      unsigned((v >> 48) & 0xff), unsigned((v >> 40) & 0xff));
        ams = buf;
      }
      splExit();
    }
    add("atmosphere", ams);
  }
  // Applet mode (album) gets a fraction of the memory a title override does
  switch (appletGetAppletType()) {
  case AppletType_Application:
  case AppletType_SystemApplication:
    add("launch mode", "application (title override)");
    break;
  case AppletType_LibraryApplet:
    add("launch mode", "library applet (album) - reduced memory");
    break;
  default:
    add("launch mode", std::to_string(static_cast<int>(appletGetAppletType())));
  }
  add("operation mode", appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  std::snprintf(buf, sizeof buf, "%llu MiB (used at startup: %llu MiB)",
                (unsigned long long)(total >> 20), (unsigned long long)(used >> 20));
  add("process memory", buf);
#elif defined(_WIN32)
  using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
  if (auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))
      )) {
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (fn(&v) == 0) {
      std::snprintf(buf, sizeof buf, "Windows %lu.%lu build %lu", v.dwMajorVersion,
                    v.dwMinorVersion, v.dwBuildNumber);
      add("os", buf);
    }
  }
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof mem;
  if (GlobalMemoryStatusEx(&mem)) {
    std::snprintf(buf, sizeof buf, "%llu MiB", (unsigned long long)(mem.ullTotalPhys >> 20));
    add("memory", buf);
  }
#else
  utsname u{};
  if (uname(&u) == 0)
    add("kernel", std::string(u.sysname) + " " + u.release + " " + u.machine);
#if defined(__APPLE__)
  auto sysctlString = [](const char *name) {
    char value[256] = {};
    size_t size = sizeof value - 1;
    return sysctlbyname(name, value, &size, nullptr, 0) == 0 ? std::string(value) : std::string();
  };
  add("os", "macOS " + sysctlString("kern.osproductversion"));
  add("model", sysctlString("hw.model"));
  add("cpu", sysctlString("machdep.cpu.brand_string"));
  uint64_t memsize = 0;
  size_t size = sizeof memsize;
  if (sysctlbyname("hw.memsize", &memsize, &size, nullptr, 0) == 0)
    add("memory", std::to_string(memsize >> 20) + " MiB");
#else
  std::ifstream osRelease("/etc/os-release");
  for (std::string line; std::getline(osRelease, line);)
    if (line.rfind("PRETTY_NAME=", 0) == 0) {
      std::string v = line.substr(12);
      if (v.size() >= 2 && v.front() == '"')
        v = v.substr(1, v.size() - 2);
      add("os", v);
    }
  const char *session = std::getenv("XDG_SESSION_TYPE");
  const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
  add("session", std::string(session ? session : "?") + " / " + (desktop ? desktop : "?"));
  long pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && pageSize > 0)
    add("memory", std::to_string((static_cast<uint64_t>(pages) * pageSize) >> 20) + " MiB");
#endif
#endif
#if !defined(__SWITCH__)
  add("cpu threads", std::to_string(std::thread::hardware_concurrency()));
#endif
  return out;
}

std::vector<std::pair<std::string, std::string>> systemInfo() {
  static const auto base = collectSystemInfo();
  const auto &b = buildInfo();
  std::vector<std::pair<std::string, std::string>> out = {
    {"version", b.version},
    {"git", b.git},
    {"build type", b.buildType},
    {"platform", b.platform},
    {"compiler", b.compiler},
    {"build id", b.buildId.empty() ? "unknown" : b.buildId},
    {"module base", addrHex(b.moduleBase)},
    {"link base", addrHex(b.linkBase)},
  };
  out.insert(out.end(), base.begin(), base.end());
  std::lock_guard<std::mutex> lock(infoMutex);
  out.insert(out.end(), extraInfo.begin(), extraInfo.end());
  return out;
}

} // namespace vkpcnx::diag
