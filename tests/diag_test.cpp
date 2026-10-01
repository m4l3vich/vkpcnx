// Unit tests for the debug-report pieces that must not regress silently:
// redaction (a miss leaks a token into a public issue) and the zip writer.
// Build: see VKPCNX_BUILD_TESTS in CMakeLists.txt

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <zlib.h>

#include "core/diag/redact.hpp"
#include "core/diag/zip_writer.hpp"

using namespace vkpcnx::diag;

static int failures = 0;
#define CHECK_EQ(actual, expected)                                                                 \
  do {                                                                                             \
    std::string a_ = (actual), e_ = (expected);                                                    \
    if (a_ != e_) {                                                                                \
      std::printf("FAIL %s:%d:\n  got      %s\n  expected %s\n", __FILE__, __LINE__, a_.c_str(),   \
                  e_.c_str());                                                                     \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static void testSecrets() {
  CHECK_EQ(redact("GET /api?token=abcdef123456&x=1"), "GET /api?token=***&x=1");
  CHECK_EQ(redact("playkey:///?access_token=eyJhbGciOi.xyz-_=&fps=60"), "playkey:///?access_token=***&fps=60");
  CHECK_EQ(redact(R"(body={"access_token":"s3cr3t-value","expires":3600})"),
           R"(body={"access_token":"***","expires":3600})");
  CHECK_EQ(redact(R"({"refresh_token": "abcdefgh"})"), R"({"refresh_token": "***"})");
  CHECK_EQ(redact(R"(json \"token\":\"abcdefgh\" end)"), R"(json \"token\":\"***\" end)");
  CHECK_EQ(redact("got oauth2_code, performing exchange; code=Zx81kQ0pLm"),
           "got oauth2_code, performing exchange; code=***");
  CHECK_EQ(redact("M_TOKEN { token: 1234567890 }"), "M_TOKEN { token: *** }");
  CHECK_EQ(redact("Authorization: Bearer abc.def.ghi"), "Authorization: Bearer ***");
  CHECK_EQ(redact("a=ice-pwd:9b1c3e5a7f2d4b6c8e0a1c3e\r\n"), "a=ice-pwd:***\r\n");
  CHECK_EQ(redact("pullToken=0123456789"), "pullToken=***");
  // Short values and non-secret keys survive
  CHECK_EQ(redact("error code=5, status code: 404"), "error code=5, status code: 404");
  CHECK_EQ(redact("InputChannel: token accepted"), "InputChannel: token accepted");
  CHECK_EQ(redact("M_TOKEN_ACCEPTED tokens=3"), "M_TOKEN_ACCEPTED tokens=3");
  CHECK_EQ(redact("a=ice-ufrag:abcd"), "a=ice-ufrag:abcd");
}

static void testAddresses() {
  CHECK_EQ(redact("connected to 95.163.32.7:443"), "connected to 95.163.32.x:443");
  CHECK_EQ(redact("candidate:1 1 UDP 2122 192.168.1.20 50000 typ host"),
           "candidate:1 1 UDP 2122 192.168.1.20 50000 typ host");
  CHECK_EQ(redact("10.0.0.1 172.16.5.4 100.64.1.1 127.0.0.1 0.0.0.0"),
           "10.0.0.1 172.16.5.4 100.64.1.1 127.0.0.1 0.0.0.0");
  CHECK_EQ(redact("srflx 8.8.8.8 relay"), "srflx 8.8.8.x relay");
  // Versions and times aren't addresses
  CHECK_EQ(redact("version 1.2.3.4.5 at 12:34:56.789"), "version 1.2.3.4.5 at 12:34:56.789");
  CHECK_EQ(redact("libdatachannel 0.24.5"), "libdatachannel 0.24.5");
  CHECK_EQ(redact("host 2a00:1450:4010:c0e::64 port"), "host 2a00:1450:4010:x port");
  CHECK_EQ(redact("v6 2001:db8::1"), "v6 2001:db8::x");
  CHECK_EQ(redact("fe80::1c2b:3d4e:5f60:7182 ::1"), "fe80::1c2b:3d4e:5f60:7182 ::1");
  CHECK_EQ(redact("std::string vkpcnx::diag::redact"), "std::string vkpcnx::diag::redact");
  std::string fp = "a=fingerprint:sha-256 7B:2A:11:C4:9E:00:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD";
  CHECK_EQ(redact(fp), fp);
}

static void testHome() {
  setRedactedHome("/Users/someone");
  CHECK_EQ(redact("Settings: loaded /Users/someone/Library/x.json"), "Settings: loaded ~/Library/x.json");
  setRedactedHome("");
  CHECK_EQ(redact("GET https://avatar.vkplay.ru/avatar/11262774.jpeg -> 200"),
           "GET https://avatar.vkplay.ru/avatar/***.jpeg -> 200");
}

// Reads back the archive with zlib to check the structure, sizes and CRCs
static void testZip() {
  std::string path = "diag_test_tmp.zip";
  std::string big(200000, 'a');
  for (size_t i = 0; i < big.size(); i += 97)
    big[i] = static_cast<char>('A' + i % 26);
  {
    ZipWriter zip(path);
    CHECK(zip.ok());
    CHECK(zip.add("report.txt", "hello\n"));
    CHECK(zip.add("logs/big.log", big));
    CHECK(zip.add("empty.txt", ""));
    CHECK(zip.finish());
  }
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string z = ss.str();
  auto u16 = [&](size_t o) { return uint32_t(uint8_t(z[o])) | uint32_t(uint8_t(z[o + 1])) << 8; };
  auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };

  CHECK(z.size() > 22);
  size_t eocd = z.size() - 22;
  CHECK(u32(eocd) == 0x06054b50);
  CHECK(u16(eocd + 10) == 3);
  size_t cd = u32(eocd + 16);
  const char *names[] = {"report.txt", "logs/big.log", "empty.txt"};
  const std::string *bodies[] = {nullptr, &big, nullptr};
  std::string small = "hello\n", none;
  bodies[0] = &small;
  bodies[2] = &none;
  for (int i = 0; i < 3; i++) {
    CHECK(u32(cd) == 0x02014b50);
    uint32_t method = u16(cd + 10), crc = u32(cd + 16), csize = u32(cd + 20), size = u32(cd + 24);
    uint32_t nameLen = u16(cd + 28), local = u32(cd + 42);
    CHECK_EQ(z.substr(cd + 46, nameLen), names[i]);
    CHECK(u32(local) == 0x04034b50);
    size_t dataAt = local + 30 + u16(local + 26) + u16(local + 28);
    std::string data = z.substr(dataAt, csize);
    if (method == 8) {
      std::string out(size, '\0');
      z_stream zs{};
      inflateInit2(&zs, -MAX_WBITS);
      zs.next_in = reinterpret_cast<Bytef *>(&data[0]);
      zs.avail_in = csize;
      zs.next_out = reinterpret_cast<Bytef *>(&out[0]);
      zs.avail_out = size;
      CHECK(inflate(&zs, Z_FINISH) == Z_STREAM_END);
      inflateEnd(&zs);
      data = out;
    }
    CHECK(data == *bodies[i]);
    CHECK(crc == crc32(0, reinterpret_cast<const Bytef *>(data.data()), uInt(data.size())));
    cd += 46 + nameLen;
  }
  std::remove(path.c_str());
}

int main() {
  testSecrets();
  testAddresses();
  testHome();
  testZip();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
