#include "grparse/base64.h"

#include <array>
#include <cctype>
#include <stdexcept>

namespace grparse {

std::string decode_base64(const std::string& value) {
  static constexpr unsigned char kInvalid = 255;
  static const auto table = [] {
    std::array<unsigned char, 256> decode{};
    decode.fill(kInvalid);
    for (unsigned char index = 0; index < 26; ++index) {
      decode[static_cast<unsigned char>('A' + index)] = index;
      decode[static_cast<unsigned char>('a' + index)] = static_cast<unsigned char>(26 + index);
    }
    for (unsigned char index = 0; index < 10; ++index) {
      decode[static_cast<unsigned char>('0' + index)] = static_cast<unsigned char>(52 + index);
    }
    decode[static_cast<unsigned char>('+')] = 62;
    decode[static_cast<unsigned char>('/')] = 63;
    return decode;
  }();

  std::string compact;
  compact.reserve(value.size());
  for (const unsigned char character : value) {
    if (!std::isspace(character)) compact.push_back(static_cast<char>(character));
  }
  if (compact.empty() || compact.size() % 4 != 0) {
    throw std::invalid_argument("file.base64_string is not valid base64");
  }

  std::string decoded;
  decoded.reserve(compact.size() / 4 * 3);
  for (size_t offset = 0; offset < compact.size(); offset += 4) {
    const char first = compact[offset];
    const char second = compact[offset + 1];
    const char third = compact[offset + 2];
    const char fourth = compact[offset + 3];
    if (first == '=' || second == '=' || table[static_cast<unsigned char>(first)] == kInvalid ||
        table[static_cast<unsigned char>(second)] == kInvalid ||
        (third != '=' && table[static_cast<unsigned char>(third)] == kInvalid) ||
        (fourth != '=' && table[static_cast<unsigned char>(fourth)] == kInvalid) ||
        (third == '=' && fourth != '=') ||
        ((third == '=' || fourth == '=') && offset + 4 != compact.size())) {
      throw std::invalid_argument("file.base64_string is not valid base64");
    }
    const auto a = table[static_cast<unsigned char>(first)];
    const auto b = table[static_cast<unsigned char>(second)];
    const auto c = third == '=' ? 0 : table[static_cast<unsigned char>(third)];
    const auto d = fourth == '=' ? 0 : table[static_cast<unsigned char>(fourth)];
    decoded.push_back(static_cast<char>((a << 2) | (b >> 4)));
    if (third != '=') decoded.push_back(static_cast<char>(((b & 0x0F) << 4) | (c >> 2)));
    if (fourth != '=') decoded.push_back(static_cast<char>(((c & 0x03) << 6) | d));
  }
  return decoded;
}

std::string encode_base64(const void* data, size_t size) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::string encoded;
  encoded.reserve((size + 2) / 3 * 4);
  size_t offset = 0;
  for (; offset + 3 <= size; offset += 3) {
    const unsigned int chunk = (static_cast<unsigned int>(bytes[offset]) << 16) |
                               (static_cast<unsigned int>(bytes[offset + 1]) << 8) |
                               bytes[offset + 2];
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
    encoded.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
    encoded.push_back(kAlphabet[chunk & 0x3F]);
  }
  const size_t remaining = size - offset;
  if (remaining == 1) {
    const unsigned int chunk = static_cast<unsigned int>(bytes[offset]) << 16;
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
    encoded.append("==");
  } else if (remaining == 2) {
    const unsigned int chunk = (static_cast<unsigned int>(bytes[offset]) << 16) |
                               (static_cast<unsigned int>(bytes[offset + 1]) << 8);
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
    encoded.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
    encoded.push_back('=');
  }
  return encoded;
}

}  // namespace grparse
