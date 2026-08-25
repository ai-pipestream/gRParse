#include "zip_writer.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

#include <miniz.h>

namespace grparse::targets {
namespace {

constexpr uint32_t kLocalHeaderSignature = 0x04034b50u;
constexpr uint32_t kCentralHeaderSignature = 0x02014b50u;
constexpr uint32_t kEndOfDirectorySignature = 0x06054b50u;

// PKZIP 2.0: the floor for deflate, which is the only method written here
// besides stored.
constexpr uint16_t kVersionNeeded = 20;
// Unix, ZIP specification 3.0, so the permission bits below are read as
// such rather than as MS-DOS attributes.
constexpr uint16_t kVersionMadeBy = 0x031e;
// 0100644: a regular file, owner-writable and world-readable.  A constant,
// not the umask of whatever process happened to build the archive.
constexpr uint32_t kExternalAttributes = 0100644u << 16;

constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflate = 8;

// The MS-DOS timestamp epoch, 1980-01-01 00:00:00: the earliest instant the
// field can express.  Every entry carries it, so nothing about when the
// archive was produced reaches its bytes.
constexpr uint16_t kFixedDosTime = 0;
constexpr uint16_t kFixedDosDate = 0x0021;

void put16(std::string* out, uint16_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
}

void put32(std::string* out, uint32_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
  out->push_back(static_cast<char>((value >> 16) & 0xff));
  out->push_back(static_cast<char>((value >> 24) & 0xff));
}

// Raw deflate at one fixed level, or the original bytes when deflating did
// not pay: an incompressible member stores rather than growing, and that
// decision is a function of the bytes alone.
struct Compressed {
  std::string bytes;
  uint16_t method = kMethodStored;
};

Compressed deflate(const std::string& data) {
  Compressed result;
  if (!data.empty()) {
    const mz_uint flags =
        tdefl_create_comp_flags_from_zip_params(MZ_DEFAULT_LEVEL, -MZ_DEFAULT_WINDOW_BITS,
                                                MZ_DEFAULT_STRATEGY);
    size_t compressed_size = 0;
    void* compressed =
        tdefl_compress_mem_to_heap(data.data(), data.size(), &compressed_size, flags);
    if (compressed == nullptr) throw std::runtime_error("zip writer could not compress a member");
    const std::unique_ptr<void, void (*)(void*)> owned(compressed, mz_free);
    if (compressed_size < data.size()) {
      result.bytes.assign(static_cast<const char*>(compressed), compressed_size);
      result.method = kMethodDeflate;
      return result;
    }
  }
  result.bytes = data;
  result.method = kMethodStored;
  return result;
}

uint32_t checked_size(size_t size, const char* what) {
  if (size > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string("zip writer would need ZIP64: ") + what);
  }
  return static_cast<uint32_t>(size);
}

}  // namespace

std::string write_zip(const std::vector<BundleFile>& files) {
  if (files.size() > std::numeric_limits<uint16_t>::max()) {
    throw std::runtime_error("zip writer would need ZIP64: too many members");
  }

  std::string archive;
  std::string directory;
  for (const auto& file : files) {
    const uint32_t offset = checked_size(archive.size(), "archive size");
    const uint32_t uncompressed = checked_size(file.bytes.size(), "member size");
    const uint32_t crc = static_cast<uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(file.bytes.data()),
                 file.bytes.size()));
    const Compressed payload = deflate(file.bytes);
    const uint32_t compressed = checked_size(payload.bytes.size(), "member size");
    const uint16_t name_length = static_cast<uint16_t>(
        checked_size(file.path.size(), "member name length") & 0xffffu);
    if (file.path.size() != name_length) {
      throw std::runtime_error("zip writer rejects a member name past 65535 bytes");
    }

    put32(&archive, kLocalHeaderSignature);
    put16(&archive, kVersionNeeded);
    // No general-purpose flags: no data descriptor, no encryption, and the
    // names are the bundle's own ASCII paths, so no UTF-8 flag either.
    put16(&archive, 0);
    put16(&archive, payload.method);
    put16(&archive, kFixedDosTime);
    put16(&archive, kFixedDosDate);
    put32(&archive, crc);
    put32(&archive, compressed);
    put32(&archive, uncompressed);
    put16(&archive, name_length);
    put16(&archive, 0);
    archive += file.path;
    archive += payload.bytes;

    put32(&directory, kCentralHeaderSignature);
    put16(&directory, kVersionMadeBy);
    put16(&directory, kVersionNeeded);
    put16(&directory, 0);
    put16(&directory, payload.method);
    put16(&directory, kFixedDosTime);
    put16(&directory, kFixedDosDate);
    put32(&directory, crc);
    put32(&directory, compressed);
    put32(&directory, uncompressed);
    put16(&directory, name_length);
    put16(&directory, 0);
    put16(&directory, 0);
    put16(&directory, 0);
    put16(&directory, 0);
    put32(&directory, kExternalAttributes);
    put32(&directory, offset);
    directory += file.path;
  }

  const uint32_t directory_offset = checked_size(archive.size(), "archive size");
  const uint32_t directory_size = checked_size(directory.size(), "central directory size");
  const auto entries = static_cast<uint16_t>(files.size());
  archive += directory;
  put32(&archive, kEndOfDirectorySignature);
  put16(&archive, 0);
  put16(&archive, 0);
  put16(&archive, entries);
  put16(&archive, entries);
  put32(&archive, directory_size);
  put32(&archive, directory_offset);
  put16(&archive, 0);
  return archive;
}

}  // namespace grparse::targets
