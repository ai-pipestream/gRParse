// The directory connector plugin the storage tests register under the kind
// "test_directory", with DirectoryConnectorConfig as its typed config.
// Header only, and test-only.
#ifndef GRPARSE_TESTS_SUPPORT_STORAGE_FIXTURE_PLUGIN_H
#define GRPARSE_TESTS_SUPPORT_STORAGE_FIXTURE_PLUGIN_H

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/storage/connectors.h"
#include "storage_plugin_fixture.pb.h"

namespace grparse_test {

inline std::string read_file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  return std::string(std::istreambuf_iterator<char>(in), {});
}

inline void register_directory_plugin() {
  using grparse::test::storage::DirectoryConnectorConfig;
  grparse::storage::ConnectorPlugin plugin;
  plugin.kind = "test_directory";
  plugin.config_type = DirectoryConnectorConfig::descriptor();
  plugin.fetch = [](const google::protobuf::Message& message) {
    const auto& config = dynamic_cast<const DirectoryConnectorConfig&>(message);
    return grparse::storage::FetchedDocument{
        .filename = std::filesystem::path(config.name()).filename().string(),
        .bytes = read_file_bytes(std::filesystem::path(config.root()) / config.name())};
  };
  plugin.deliver = [](const google::protobuf::Message& message,
                      const std::vector<grparse::targets::BundleFile>& files) {
    const auto& config = dynamic_cast<const DirectoryConnectorConfig&>(message);
    std::vector<grparse::storage::WrittenObject> written;
    for (const auto& file : files) {
      const std::filesystem::path out = std::filesystem::path(config.root()) / file.path;
      std::filesystem::create_directories(out.parent_path());
      std::ofstream(out, std::ios::binary) << file.bytes;
      written.push_back({.key = file.path, .etag = "", .size_bytes = file.bytes.size(), .store_id = ""});
    }
    return written;
  };
  grparse::storage::register_connector_plugin(plugin);
}

}  // namespace grparse_test

#endif
