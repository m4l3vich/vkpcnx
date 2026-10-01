#pragma once

#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// Minimal .zip writer (deflate via zlib, no zip64): enough for a debug report,
// which every OS can open without extra tools.
namespace vkpcnx::diag {

class ZipWriter {
public:
  explicit ZipWriter(const std::string &path);
  ~ZipWriter();

  bool ok() const { return ok_; }
  // `name` uses '/' separators; it is stored as UTF-8
  bool add(const std::string &name, std::string_view data, std::time_t mtime = std::time(nullptr));
  // Writes the central directory. The file is unusable until this succeeds.
  bool finish();

private:
  struct Entry {
    std::string name;
    uint32_t crc;
    uint32_t compressedSize;
    uint32_t size;
    uint16_t method;
    uint16_t dosTime;
    uint16_t dosDate;
    uint32_t offset;
  };

  std::ofstream out_;
  std::vector<Entry> entries_;
  uint32_t offset_ = 0;
  bool ok_ = false;
  bool finished_ = false;

  void write(const void *data, size_t size);
  void u16(uint16_t v);
  void u32(uint32_t v);
};

} // namespace vkpcnx::diag
