// A writer for the OLE compound files ([MS-CFB]) the Office binary formats
// and encrypted Office packages live in, small enough for a test to build a
// fixture in a few lines: a tree of storages and streams under the root,
// written with 512-byte sectors, every stream under the 4096-byte cutoff in
// the mini stream (as Office writes them) and the rest in regular sectors.
// Header only, and test-only: nothing under src/ or include/ may include it.
#ifndef GRPARSE_TESTS_SUPPORT_COMPOUND_FILE_H
#define GRPARSE_TESTS_SUPPORT_COMPOUND_FILE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace grparse_test {

// One directory object: a stream when `children` is empty and `storage` is
// false, a storage (whose `data` is ignored) otherwise.
struct CompoundObject {
  std::string name;
  std::string data;
  bool storage = false;
  std::vector<CompoundObject> children;

  static CompoundObject stream(std::string name, std::string data) {
    return CompoundObject{std::move(name), std::move(data), false, {}};
  }
  static CompoundObject folder(std::string name, std::vector<CompoundObject> children) {
    return CompoundObject{std::move(name), {}, true, std::move(children)};
  }
};

namespace compound_detail {

constexpr std::size_t kSector = 512;
constexpr std::size_t kMiniSector = 64;
constexpr std::uint32_t kMiniCutoff = 4096;
constexpr std::uint32_t kFreeSect = 0xFFFFFFFF;
constexpr std::uint32_t kEndOfChain = 0xFFFFFFFE;
constexpr std::uint32_t kFatSect = 0xFFFFFFFD;
constexpr std::uint32_t kNoStream = 0xFFFFFFFF;

inline void put16(std::string& out, std::size_t at, unsigned value) {
  out[at] = static_cast<char>(value & 0xFF);
  out[at + 1] = static_cast<char>((value >> 8) & 0xFF);
}

inline void put32(std::string& out, std::size_t at, std::uint32_t value) {
  put16(out, at, value & 0xFFFF);
  put16(out, at + 2, (value >> 16) & 0xFFFF);
}

inline std::string padded(std::string data, std::size_t unit) {
  while (data.size() % unit != 0) data.push_back('\0');
  return data;
}

// A directory entry under construction, in the flat order it is written.
struct Slot {
  const CompoundObject* object = nullptr;  // null for the root
  std::uint8_t type = 0;
  std::uint32_t left = kNoStream;
  std::uint32_t right = kNoStream;
  std::uint32_t child = kNoStream;
  std::uint32_t start = kEndOfChain;
  std::uint32_t size = 0;
};

// Lays the children of one storage out as a right-leaning chain: the first
// child is the parent's child entry and each child's right sibling is the
// next. A valid directory tree, if a lopsided one.
inline std::uint32_t lay_out(const std::vector<CompoundObject>& children,
                             std::vector<Slot>& slots) {
  std::uint32_t first = kNoStream;
  std::uint32_t previous = kNoStream;
  for (const CompoundObject& object : children) {
    const auto id = static_cast<std::uint32_t>(slots.size());
    slots.push_back(Slot{&object, static_cast<std::uint8_t>(object.storage ? 1 : 2)});
    if (first == kNoStream) first = id;
    if (previous != kNoStream) slots[previous].right = id;
    previous = id;
    if (object.storage) {
      const std::uint32_t child = lay_out(object.children, slots);
      slots[id].child = child;
    }
  }
  return first;
}

inline std::string entry_bytes(const std::string& name, const Slot& slot) {
  std::string entry(128, '\0');
  const std::size_t units = name.size() > 31 ? 31 : name.size();
  for (std::size_t i = 0; i < units; ++i) put16(entry, 2 * i, static_cast<unsigned char>(name[i]));
  put16(entry, 64, static_cast<unsigned>((units + 1) * 2));
  entry[66] = static_cast<char>(slot.type);
  entry[67] = 1;  // black
  put32(entry, 68, slot.left);
  put32(entry, 72, slot.right);
  put32(entry, 76, slot.child);
  put32(entry, 116, slot.start);
  put32(entry, 120, slot.size);
  return entry;
}

}  // namespace compound_detail

// The bytes of a compound file holding `objects` under its root storage.
inline std::string compound_file(const std::vector<CompoundObject>& objects) {
  using namespace compound_detail;
  std::vector<Slot> slots;
  slots.push_back(Slot{nullptr, 5});
  slots[0].child = lay_out(objects, slots);

  // Sector plan: FAT, directory, mini FAT, mini stream, then every large
  // stream, in that order.
  const std::size_t directory_sectors = (slots.size() * 128 + kSector - 1) / kSector;
  std::vector<std::uint32_t> fat;
  const std::uint32_t fat_sector = 0;
  const std::uint32_t directory_start = 1;
  const std::uint32_t mini_fat_sector = directory_start + static_cast<std::uint32_t>(directory_sectors);
  const std::uint32_t mini_stream_start = mini_fat_sector + 1;

  // The mini stream and its FAT.
  std::string mini_stream;
  std::vector<std::uint32_t> mini_fat;
  for (Slot& slot : slots) {
    if (slot.object == nullptr || slot.type != 2) continue;
    const std::string& data = slot.object->data;
    if (data.empty() || data.size() >= kMiniCutoff) continue;
    const std::string body = padded(data, kMiniSector);
    const auto first = static_cast<std::uint32_t>(mini_fat.size());
    const std::size_t count = body.size() / kMiniSector;
    for (std::size_t i = 0; i < count; ++i) {
      mini_fat.push_back(i + 1 < count ? first + static_cast<std::uint32_t>(i) + 1 : kEndOfChain);
    }
    slot.start = first;
    slot.size = static_cast<std::uint32_t>(data.size());
    mini_stream += body;
  }
  mini_stream = padded(mini_stream, kSector);
  const std::size_t mini_stream_sectors = mini_stream.size() / kSector;
  slots[0].start = mini_stream_sectors == 0 ? kEndOfChain : mini_stream_start;
  slots[0].size = static_cast<std::uint32_t>(mini_stream.size());

  // Large streams.
  std::string large;
  std::uint32_t next_sector = mini_stream_start + static_cast<std::uint32_t>(mini_stream_sectors);
  std::vector<std::pair<std::uint32_t, std::size_t>> large_chains;  // start, sector count
  for (Slot& slot : slots) {
    if (slot.object == nullptr || slot.type != 2) continue;
    const std::string& data = slot.object->data;
    if (data.size() < kMiniCutoff) continue;
    const std::string body = padded(data, kSector);
    slot.start = next_sector;
    slot.size = static_cast<std::uint32_t>(data.size());
    large_chains.emplace_back(next_sector, body.size() / kSector);
    next_sector += static_cast<std::uint32_t>(body.size() / kSector);
    large += body;
  }

  // The FAT: one sector covers 128 sectors, which is all a fixture needs.
  fat.assign(kSector / 4, kFreeSect);
  auto chain = [&fat](std::uint32_t start, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      fat[start + i] = i + 1 < count ? start + static_cast<std::uint32_t>(i) + 1 : kEndOfChain;
    }
  };
  fat[fat_sector] = kFatSect;
  chain(directory_start, directory_sectors);
  fat[mini_fat_sector] = kEndOfChain;
  if (mini_stream_sectors > 0) chain(mini_stream_start, mini_stream_sectors);
  for (const auto& [start, count] : large_chains) chain(start, count);

  // Header.
  std::string header(kSector, '\0');
  header.replace(0, 8, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
  put16(header, 24, 0x003E);  // minor version
  put16(header, 26, 0x0003);  // major version 3: 512-byte sectors
  put16(header, 28, 0xFFFE);  // little endian
  put16(header, 30, 9);       // sector shift
  put16(header, 32, 6);       // mini sector shift
  put32(header, 44, 1);       // FAT sectors
  put32(header, 48, directory_start);
  put32(header, 56, kMiniCutoff);
  put32(header, 60, mini_fat.empty() ? kEndOfChain : mini_fat_sector);
  put32(header, 64, mini_fat.empty() ? 0 : 1);
  put32(header, 68, kEndOfChain);  // no DIFAT sectors
  put32(header, 72, 0);
  for (std::size_t i = 0; i < 109; ++i) put32(header, 76 + 4 * i, i == 0 ? fat_sector : kFreeSect);

  std::string file = header;
  std::string fat_bytes(kSector, '\0');
  for (std::size_t i = 0; i < fat.size(); ++i) put32(fat_bytes, 4 * i, fat[i]);
  file += fat_bytes;
  std::string directory;
  for (const Slot& slot : slots) {
    directory += entry_bytes(slot.object == nullptr ? "Root Entry" : slot.object->name, slot);
  }
  while (directory.size() % kSector != 0) directory += entry_bytes("", Slot{});
  file += directory;
  std::string mini_fat_bytes(kSector, '\0');
  for (std::size_t i = 0; i < kSector / 4; ++i) {
    put32(mini_fat_bytes, 4 * i, i < mini_fat.size() ? mini_fat[i] : kFreeSect);
  }
  file += mini_fat_bytes;
  file += mini_stream;
  file += large;
  return file;
}

}  // namespace grparse_test

#endif  // GRPARSE_TESTS_SUPPORT_COMPOUND_FILE_H
