#include "xbe_profile.hpp"

#include "phantomcrash/json.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace phantomcrash::extract {
namespace {

std::uint32_t remap_address(std::uint32_t canonical_va,
                            const std::unordered_map<std::uint32_t, std::uint32_t> &overrides,
                            const std::vector<XbeAddressRange> &ranges) noexcept {
  if (const auto found = overrides.find(canonical_va); found != overrides.end()) {
    return found->second;
  }
  for (const auto &range : ranges) {
    if (range.start <= canonical_va && canonical_va < range.end) {
      return static_cast<std::uint32_t>(static_cast<std::int64_t>(canonical_va) + range.delta);
    }
  }
  return canonical_va;
}

std::string lower_hex(std::string_view value) {
  std::string out(value);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::uint32_t parse_hex_u32(std::string_view text) {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
  }
  std::uint32_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw FormatError("Invalid XBE profile VA: " + std::string(text));
  }
  return value;
}

std::vector<XbeAddressRange> parse_ranges(const json::JsonValue *node) {
  std::vector<XbeAddressRange> ranges;
  if (node == nullptr) {
    return ranges;
  }
  for (const auto &entry : node->array()) {
    const auto &items = entry.array();
    if (items.size() != 3) {
      throw FormatError("XBE profile range must have 3 elements");
    }
    ranges.push_back({parse_hex_u32(items[0].string()), parse_hex_u32(items[1].string()),
                      static_cast<std::int32_t>(items[2].number())});
  }
  return ranges;
}

std::unordered_map<std::uint32_t, std::uint32_t> parse_overrides(const json::JsonValue *node) {
  std::unordered_map<std::uint32_t, std::uint32_t> overrides;
  if (node == nullptr) {
    return overrides;
  }
  for (const auto &[key, value] : node->object()) {
    overrides.emplace(parse_hex_u32(key), parse_hex_u32(value.string()));
  }
  return overrides;
}

XbeProfile make_usa_profile() {
  return {
      "usa",
      "Phantom Crash (USA / NTSC-U)",
      "ee1caf6b0bde47abb104368657c44d078904684482ff60be5958e2201f5162d0",
      "usa",
      {{0x19f870u, 0x1d6a80u, 0}, {0x2014c8u, 0x220000u, -224}, {0x220000u, 0x415000u, -232}},
      {
          {0x0d0f07u, 0x0d0e67u},
          {0x0d19eau, 0x0d194au},
          {0x11d060u, 0x11cfc0u},
          {0x11e660u, 0x11e500u},
          {0x17ac38u, 0x17aad8u},
          {0x17bec4u, 0x17bd64u},
          {0x1db4a0u, 0x17cf88u},
          {0x1efd38u, 0x1efcc8u},
      },
      {
          {0x17a620u, 0x17d110u, -352},
          {0x17d110u, 0x17d1b0u, -280},
          {0x17d1b0u, 0x190210u, -240},
          {0x190210u, 0x19f164u, -120},
          {0x19f164u, 0x1d6a80u, 0},
          {0x1d6a80u, 0x1fc000u, -112},
          {0x1fc000u, 0x240000u, -224},
          {0x240000u, 0x415000u, -232},
      },
      {
          {0x1896a8u, 0x18c620u}, {0x189aa8u, 0x18dc78u}, {0x189d20u, 0x18def0u},
          {0x18a118u, 0x18f4d0u}, {0x18a5b0u, 0x18ac10u}, {0x18fff0u, 0x190158u},
          {0x190210u, 0x190378u}, {0x190298u, 0x190400u}, {0x190368u, 0x1904d0u},
          {0x1905c8u, 0x190730u}, {0x190868u, 0x1909d0u}, {0x190ac0u, 0x190c28u},
          {0x190d60u, 0x190ec8u}, {0x190fd8u, 0x191140u}, {0x191270u, 0x1913d8u},
          {0x1914d0u, 0x191638u}, {0x191708u, 0x191870u}, {0x191978u, 0x191ae0u},
          {0x191b60u, 0x191cc8u}, {0x191d00u, 0x191e68u}, {0x191fc8u, 0x192130u},
          {0x192218u, 0x192380u}, {0x192440u, 0x1925a8u}, {0x1926b0u, 0x192818u},
          {0x192900u, 0x192a68u}, {0x192b08u, 0x192c70u}, {0x192d20u, 0x192e88u},
          {0x192ff0u, 0x193158u}, {0x1931d0u, 0x193338u}, {0x193448u, 0x18ff00u},
          {0x195428u, 0x196810u}, {0x195a60u, 0x1974c0u}, {0x195d78u, 0x196128u},
          {0x197e18u, 0x198048u}, {0x199b76u, 0x199c38u},
      },
  };
}

} // namespace

std::uint32_t XbeProfile::table(std::uint32_t canonical_va) const noexcept {
  return remap_address(canonical_va, table_overrides, table_ranges);
}

std::uint32_t XbeProfile::text(std::uint32_t canonical_va) const noexcept {
  return remap_address(canonical_va, text_overrides, text_ranges);
}

const XbeProfile &europe_profile() {
  static const XbeProfile profile{
      "europe",
      "Phantom Crash (Europe / PAL)",
      "bdbe2c3eb2361395babfe6095d1a2da72f6581dccb34812b8d45e084c6c903c6",
      "europe",
      {{0x000000u, 0xffffffffu, 0}},
      {},
      {{0x000000u, 0xffffffffu, 0}},
      {},
  };
  return profile;
}

const XbeProfile &usa_profile() {
  static const XbeProfile profile = make_usa_profile();
  return profile;
}

std::vector<XbeProfile> load_xbe_profiles(const std::filesystem::path &json_path) {
  std::error_code error;
  if (json_path.empty() || !std::filesystem::is_regular_file(json_path, error)) {
    return {europe_profile(), usa_profile()};
  }
  const auto root = json::read_file(json_path);
  const auto *profiles_obj = root.find("profiles");
  if (profiles_obj == nullptr) {
    throw FormatError("xbe_profiles.json is missing 'profiles'");
  }
  std::vector<XbeProfile> profiles;
  for (const auto &[key, value] : profiles_obj->object()) {
    XbeProfile p;
    p.id = value.string_or("id", key);
    p.label = value.string_or("label", p.id);
    p.sha256 = lower_hex(value.find("sha256")->string());
    p.artifacts_dir = value.string_or("artifacts_dir", p.id);
    p.table_ranges = parse_ranges(value.find("table_ranges"));
    p.table_overrides = parse_overrides(value.find("table_overrides"));
    p.text_ranges = parse_ranges(value.find("text_ranges"));
    p.text_overrides = parse_overrides(value.find("text_overrides"));
    profiles.push_back(std::move(p));
  }
  return profiles;
}

std::optional<XbeProfile> match_xbe_profile(const std::string_view sha256_hex_value,
                                            const std::filesystem::path &profiles_json) {
  const std::string needle = lower_hex(sha256_hex_value);
  for (const auto &profile : load_xbe_profiles(profiles_json)) {
    if (profile.sha256 == needle) {
      return profile;
    }
  }
  return std::nullopt;
}

XbeProfile require_xbe_profile(ByteView xbe, const std::filesystem::path &profiles_json) {
  const std::string digest = sha256_hex(xbe);
  if (auto matched = match_xbe_profile(digest, profiles_json)) {
    return *matched;
  }
  throw FormatError("Unsupported default.xbe sha256 " + digest +
                    "; expected European (PAL) or USA (NTSC-U) retail build");
}

} // namespace phantomcrash::extract
