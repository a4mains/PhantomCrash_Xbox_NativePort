// Per-release XBE address profiles for multi-region asset extraction.
// Canonical addresses in the extractors describe the European release; other
// builds (such as USA/NTSC-U) remap canonical table and text VAs via these
// profiles while internal XBE pointers read via image.u32(...) remain native.
#pragma once

#include "binary.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace phantomcrash::extract {

struct XbeAddressRange {
  std::uint32_t start{};
  std::uint32_t end{};
  std::int32_t delta{};
};

struct XbeProfile {
  std::string id;
  std::string label;
  std::string sha256;
  std::string artifacts_dir;
  std::vector<XbeAddressRange> table_ranges;
  std::unordered_map<std::uint32_t, std::uint32_t> table_overrides;
  std::vector<XbeAddressRange> text_ranges;
  std::unordered_map<std::uint32_t, std::uint32_t> text_overrides;

  [[nodiscard]] std::uint32_t table(std::uint32_t canonical_va) const noexcept;
  [[nodiscard]] std::uint32_t text(std::uint32_t canonical_va) const noexcept;
};

[[nodiscard]] const XbeProfile &europe_profile();
[[nodiscard]] const XbeProfile &usa_profile();
[[nodiscard]] std::vector<XbeProfile> load_xbe_profiles(const std::filesystem::path &json_path);
[[nodiscard]] std::optional<XbeProfile> match_xbe_profile(
    std::string_view sha256_hex, const std::filesystem::path &profiles_json = {});
[[nodiscard]] XbeProfile require_xbe_profile(
    ByteView xbe, const std::filesystem::path &profiles_json = {});

} // namespace phantomcrash::extract
