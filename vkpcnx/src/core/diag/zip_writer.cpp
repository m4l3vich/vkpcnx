#include "core/diag/zip_writer.hpp"

#include <zlib.h>

namespace vkpcnx::diag {

namespace {

constexpr uint16_t kVersion = 20;       // 2.0: deflate
constexpr uint16_t kFlagUtf8 = 1 << 11; // names are UTF-8
constexpr uint16_t kStored = 0, kDeflated = 8;

// Deflates `data` raw (no zlib header), as zip expects; empty on failure
std::string deflateRaw(std::string_view data) {
  z_stream zs{};
  if (deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    return {};
  std::string out(deflateBound(&zs, static_cast<uLong>(data.size())), '\0');
  zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
  zs.avail_in = static_cast<uInt>(data.size());
  zs.next_out = reinterpret_cast<Bytef *>(&out[0]);
  zs.avail_out = static_cast<uInt>(out.size());
  int rc = deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return rc == Z_STREAM_END ? out : std::string();
}

} // namespace

ZipWriter::ZipWriter(const std::string &path) : out_(path, std::ios::binary | std::ios::trunc) {
  ok_ = static_cast<bool>(out_);
}

ZipWriter::~ZipWriter() {
  if (!finished_)
    finish();
}

void ZipWriter::write(const void *data, size_t size) {
  out_.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
  offset_ += static_cast<uint32_t>(size);
  ok_ = ok_ && static_cast<bool>(out_);
}

void ZipWriter::u16(uint16_t v) {
  uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
  write(b, 2);
}

void ZipWriter::u32(uint32_t v) {
  uint8_t b[4] = {
    static_cast<uint8_t>(v),
    static_cast<uint8_t>(v >> 8),
    static_cast<uint8_t>(v >> 16),
    static_cast<uint8_t>(v >> 24)
  };
  write(b, 4);
}

bool ZipWriter::add(const std::string &name, std::string_view data, std::time_t mtime) {
  if (!ok_ || finished_ || data.size() > 0xFFFFFFF0u)
    return false;

  Entry e{};
  e.name = name;
  e.size = static_cast<uint32_t>(data.size());
  e.crc = static_cast<uint32_t>(
    crc32(0, reinterpret_cast<const Bytef *>(data.data()), static_cast<uInt>(data.size()))
  );
  std::string packed = deflateRaw(data);
  bool deflated = !packed.empty() && packed.size() < data.size();
  e.method = deflated ? kDeflated : kStored;
  std::string_view body = deflated ? std::string_view(packed) : data;
  e.compressedSize = static_cast<uint32_t>(body.size());

  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &mtime);
#else
  localtime_r(&mtime, &tm);
#endif
  int year = tm.tm_year + 1900 < 1980 ? 1980 : tm.tm_year + 1900;
  e.dosTime = static_cast<uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  e.dosDate = static_cast<uint16_t>(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
  e.offset = offset_;

  u32(0x04034b50);
  u16(kVersion);
  u16(kFlagUtf8);
  u16(e.method);
  u16(e.dosTime);
  u16(e.dosDate);
  u32(e.crc);
  u32(e.compressedSize);
  u32(e.size);
  u16(static_cast<uint16_t>(e.name.size()));
  u16(0); // extra field
  write(e.name.data(), e.name.size());
  write(body.data(), body.size());

  entries_.push_back(std::move(e));
  return ok_;
}

bool ZipWriter::finish() {
  if (finished_)
    return ok_;
  finished_ = true;
  if (!ok_)
    return false;

  uint32_t dirOffset = offset_;
  for (const Entry &e : entries_) {
    u32(0x02014b50);
    u16(kVersion); // made by: MS-DOS / FAT attributes
    u16(kVersion);
    u16(kFlagUtf8);
    u16(e.method);
    u16(e.dosTime);
    u16(e.dosDate);
    u32(e.crc);
    u32(e.compressedSize);
    u32(e.size);
    u16(static_cast<uint16_t>(e.name.size()));
    u16(0); // extra
    u16(0); // comment
    u16(0); // disk
    u16(0); // internal attributes
    u32(0); // external attributes
    u32(e.offset);
    write(e.name.data(), e.name.size());
  }
  uint32_t dirSize = offset_ - dirOffset;

  u32(0x06054b50);
  u16(0);
  u16(0);
  u16(static_cast<uint16_t>(entries_.size()));
  u16(static_cast<uint16_t>(entries_.size()));
  u32(dirSize);
  u32(dirOffset);
  u16(0); // comment
  out_.close();
  return ok_ && !out_.fail();
}

} // namespace vkpcnx::diag
