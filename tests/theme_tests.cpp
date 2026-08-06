#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>

#include "common/json.h"
#include "test_harness.h"
#include "theme/theme.h"

namespace {

using zrinput::json::Parse;
using zrinput::json::ParseError;
using zrinput::json::ParseLimits;
using zrinput::json::Value;
using zrinput::theme::AssetResourceInfo;
using zrinput::theme::ColorMode;
using zrinput::theme::IsAssetWithinSecurityBudget;
using zrinput::theme::IsSafeAssetPath;
using zrinput::theme::LoadThemeJson;
using zrinput::theme::SafeDefaultTheme;
using zrinput::theme::SerializeTheme;
using zrinput::theme::ThemeSecurityLimits;
using zrinput::theme::ValidateTheme;

void ExpectParseFailure(std::string_view text, const ParseLimits& limits = {}) {
  bool threw = false;
  try {
    static_cast<void>(Parse(text, limits));
  } catch (const ParseError&) {
    threw = true;
  }
  ZR_EXPECT_TRUE(threw);
}

std::string ThemeJsonWithAssetPath(std::string path) {
  Value root = Parse(SerializeTheme(SafeDefaultTheme(), false));
  Value& icons = root.AsObject().at("assets").AsObject().at("icons");
  icons.AsObject().emplace("custom", Value(std::move(path)));
  return zrinput::json::Serialize(root);
}

std::filesystem::path FindBundledTheme() {
  const std::filesystem::path source_path(__FILE__);
  if (source_path.is_absolute()) {
    const auto candidate = source_path.parent_path().parent_path() / "themes" /
                           "windows-11-reference" / "theme.json";
    if (std::filesystem::is_regular_file(candidate)) {
      return candidate;
    }
  }
  std::filesystem::path directory = std::filesystem::current_path();
  for (std::size_t depth = 0; depth < std::size_t{8}; ++depth) {
    const auto candidate =
        directory / "themes" / "windows-11-reference" / "theme.json";
    if (std::filesystem::is_regular_file(candidate)) {
      return candidate;
    }
    if (!directory.has_parent_path() || directory.parent_path() == directory) {
      break;
    }
    directory = directory.parent_path();
  }
  return {};
}

ZR_TEST(JsonRoundTripsUnicodeAndEscapes) {
  const Value original = Parse(
      R"({"emoji":"\uD83D\uDE00","text":"line\n\u4E2D\u6587","v":-1.25e2})");
  const std::string encoded = zrinput::json::Serialize(original, true);
  ZR_EXPECT_EQ(Parse(encoded), original);
}

ZR_TEST(JsonRejectsNonCanonicalOrAmbiguousInput) {
  ExpectParseFailure(R"({"a":1,"a":2})");
  ExpectParseFailure("01");
  ExpectParseFailure("1.");
  ExpectParseFailure("[1,]");
  ExpectParseFailure("\"\\uD800\"");
  ExpectParseFailure(std::string("\"") + static_cast<char>(0xC0) +
                     static_cast<char>(0xAF) + "\"");
  ExpectParseFailure("true false");
  ExpectParseFailure("1e9999");
}

ZR_TEST(JsonEnforcesEveryResourceBudget) {
  ParseLimits limits;
  limits.max_input_bytes = 8;
  ExpectParseFailure("         ", limits);

  limits = {};
  limits.max_string_bytes = 4;
  ExpectParseFailure(R"("12345")", limits);
  ExpectParseFailure(R"("\u4E2D\u6587")", limits);

  limits = {};
  limits.max_number_characters = 4;
  ExpectParseFailure("12345", limits);

  limits = {};
  limits.max_depth = 2;
  ExpectParseFailure("[[[0]]]", limits);

  limits = {};
  limits.max_container_items = 2;
  ExpectParseFailure("[0,1,2]", limits);

  limits = {};
  limits.max_total_values = 3;
  ExpectParseFailure("[0,1,2]", limits);
}

ZR_TEST(SafeDefaultIsCompleteAndValid) {
  const auto& theme = SafeDefaultTheme();
  ZR_EXPECT_TRUE(ValidateTheme(theme).empty());
  ZR_EXPECT_EQ(theme.name, std::string("Windows 11 Reference"));
  ZR_EXPECT_EQ(theme.dpi.scales.size(), std::size_t{5});
  ZR_EXPECT_EQ(theme.layout.supported.size(), std::size_t{5});
  ZR_EXPECT_TRUE(theme.assets.icons.empty());
}

ZR_TEST(ThemeRoundTripPreservesEveryToken) {
  const auto& original = SafeDefaultTheme();
  const auto loaded = LoadThemeJson(SerializeTheme(original, true));
  ZR_EXPECT_TRUE(!loaded.used_fallback);
  ZR_EXPECT_TRUE(loaded.issues.empty());
  ZR_EXPECT_EQ(loaded.theme, original);
}

ZR_TEST(AllColorModesRoundTrip) {
  const ColorMode modes[] = {ColorMode::kLight, ColorMode::kDark,
                             ColorMode::kFollowSystem};
  for (const ColorMode mode : modes) {
    auto theme = SafeDefaultTheme();
    theme.color_mode = mode;
    const auto loaded = LoadThemeJson(SerializeTheme(theme, false));
    ZR_EXPECT_TRUE(!loaded.used_fallback);
    ZR_EXPECT_EQ(loaded.theme.color_mode, mode);
  }
}

ZR_TEST(BundledReferenceThemeMatchesTypedSafeDefault) {
  const auto path = FindBundledTheme();
  ZR_EXPECT_TRUE(!path.empty());
  std::ifstream input(path, std::ios::binary);
  ZR_EXPECT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto loaded = LoadThemeJson(contents.str());
  ZR_EXPECT_TRUE(!loaded.used_fallback);
  ZR_EXPECT_TRUE(loaded.issues.empty());
  ZR_EXPECT_EQ(loaded.theme, SafeDefaultTheme());
}

ZR_TEST(ThemeFallsBackForMalformedUnknownAndWrongVersion) {
  const auto malformed = LoadThemeJson("{not json}");
  ZR_EXPECT_TRUE(malformed.used_fallback);
  ZR_EXPECT_TRUE(!malformed.issues.empty());
  ZR_EXPECT_EQ(malformed.theme, SafeDefaultTheme());

  Value with_unknown = Parse(SerializeTheme(SafeDefaultTheme(), false));
  with_unknown.AsObject().emplace("executable", Value("payload.dll"));
  const auto unknown = LoadThemeJson(zrinput::json::Serialize(with_unknown));
  ZR_EXPECT_TRUE(unknown.used_fallback);

  Value wrong_version = Parse(SerializeTheme(SafeDefaultTheme(), false));
  wrong_version.AsObject().at("schema_version") = Value(2.0);
  const auto version = LoadThemeJson(zrinput::json::Serialize(wrong_version));
  ZR_EXPECT_TRUE(version.used_fallback);
  ZR_EXPECT_EQ(version.theme, SafeDefaultTheme());
}

ZR_TEST(ThemeRejectsOversizedInputBeforeDecoding) {
  const std::string oversized(
      ThemeSecurityLimits::kMaxThemeJsonBytes + std::size_t{1}, ' ');
  const auto loaded = LoadThemeJson(oversized);
  ZR_EXPECT_TRUE(loaded.used_fallback);
  ZR_EXPECT_TRUE(!loaded.issues.empty());
  ZR_EXPECT_EQ(loaded.theme, SafeDefaultTheme());
}

ZR_TEST(ThemeDiagnosticsDoNotEchoUntrustedPropertyNames) {
  Value unknown_property = Parse(SerializeTheme(SafeDefaultTheme(), false));
  unknown_property.AsObject().emplace("bad\n\x1b[31m", Value(true));
  const auto unknown =
      LoadThemeJson(zrinput::json::Serialize(unknown_property));
  ZR_EXPECT_TRUE(unknown.used_fallback);
  ZR_EXPECT_TRUE(!unknown.issues.empty());
  ZR_EXPECT_TRUE(unknown.issues.front().message.find("bad") ==
                 std::string::npos);
  ZR_EXPECT_TRUE(unknown.issues.front().message.find('\x1b') ==
                 std::string::npos);

  Value invalid_icon = Parse(SerializeTheme(SafeDefaultTheme(), false));
  invalid_icon.AsObject()
      .at("assets")
      .AsObject()
      .at("icons")
      .AsObject()
      .emplace("bad\nicon", Value("assets/icon.png"));
  const auto icon = LoadThemeJson(zrinput::json::Serialize(invalid_icon));
  ZR_EXPECT_TRUE(icon.used_fallback);
  ZR_EXPECT_TRUE(!icon.issues.empty());
  for (const auto& issue : icon.issues) {
    ZR_EXPECT_TRUE(issue.path.find('\n') == std::string::npos);
    ZR_EXPECT_TRUE(issue.message.find('\n') == std::string::npos);
  }
}

ZR_TEST(AssetPathsCannotEscapeOrSelectExecutableFormats) {
  ZR_EXPECT_TRUE(IsSafeAssetPath("assets/icons/language.png"));
  ZR_EXPECT_TRUE(IsSafeAssetPath("assets/panel.webp"));
  ZR_EXPECT_TRUE(!IsSafeAssetPath("../escape.png"));
  ZR_EXPECT_TRUE(!IsSafeAssetPath("assets/../escape.png"));
  ZR_EXPECT_TRUE(!IsSafeAssetPath("C:/escape.png"));
  ZR_EXPECT_TRUE(!IsSafeAssetPath("assets\\escape.png"));
  ZR_EXPECT_TRUE(!IsSafeAssetPath("assets/payload.dll"));
  ZR_EXPECT_TRUE(!IsSafeAssetPath("assets/%2e%2e/escape.png"));
  ZR_EXPECT_TRUE(!IsSafeAssetPath("assets/CON.png"));
  ZR_EXPECT_TRUE(!IsSafeAssetPath("assets/icons/lpt9.webp"));
  ZR_EXPECT_TRUE(!IsSafeAssetPath("assets/icons./language.png"));

  const std::string paths[] = {"../escape.png", "assets/../escape.png",
                               "C:/escape.png", "assets\\escape.png",
                               "assets/payload.dll"};
  for (const auto& path : paths) {
    const auto loaded = LoadThemeJson(ThemeJsonWithAssetPath(path));
    ZR_EXPECT_TRUE(loaded.used_fallback);
    ZR_EXPECT_EQ(loaded.theme, SafeDefaultTheme());
  }
}

ZR_TEST(DecodedAssetsMustRemainWithinAllBudgets) {
  const AssetResourceInfo valid{
      256 * 1024, 4 * 1024 * 1024, 1024 * 1024, 8 * 1024 * 1024, 1024, 1024};
  ZR_EXPECT_TRUE(IsAssetWithinSecurityBudget(valid));

  auto oversized = valid;
  oversized.encoded_bytes = ThemeSecurityLimits::kMaxAssetFileBytes + 1;
  ZR_EXPECT_TRUE(!IsAssetWithinSecurityBudget(oversized));
  oversized = valid;
  oversized.decoded_bytes = ThemeSecurityLimits::kMaxDecodedAssetBytes + 1;
  ZR_EXPECT_TRUE(!IsAssetWithinSecurityBudget(oversized));
  oversized = valid;
  oversized.package_encoded_bytes =
      ThemeSecurityLimits::kMaxPackageAssetBytes + 1;
  ZR_EXPECT_TRUE(!IsAssetWithinSecurityBudget(oversized));
  oversized = valid;
  oversized.package_decoded_bytes =
      ThemeSecurityLimits::kMaxPackageDecodedBytes + 1;
  ZR_EXPECT_TRUE(!IsAssetWithinSecurityBudget(oversized));
  oversized = valid;
  oversized.package_encoded_bytes = oversized.encoded_bytes - 1;
  ZR_EXPECT_TRUE(!IsAssetWithinSecurityBudget(oversized));
  oversized = valid;
  oversized.package_decoded_bytes = oversized.decoded_bytes - 1;
  ZR_EXPECT_TRUE(!IsAssetWithinSecurityBudget(oversized));
  oversized = valid;
  oversized.width = static_cast<std::uint32_t>(
      ThemeSecurityLimits::kMaxDecodedAssetDimension + 1);
  ZR_EXPECT_TRUE(!IsAssetWithinSecurityBudget(oversized));
  oversized = valid;
  oversized.height = 0;
  ZR_EXPECT_TRUE(!IsAssetWithinSecurityBudget(oversized));
}

ZR_TEST(ThemeValidationRejectsUnsafeProgrammaticValues) {
  auto theme = SafeDefaultTheme();
  theme.metrics.max_width = theme.metrics.min_width - 1.0;
  theme.dpi.scales = {1.0, 1.0};
  theme.status_bar.buttons[1].order = theme.status_bar.buttons[0].order;
  theme.assets.icons.emplace("bad", "assets/../bad.png");
  theme.hdr.sdr_reference_white_nits = 1000.0;
  theme.color_mode = static_cast<ColorMode>(99);
  const auto issues = ValidateTheme(theme);
  ZR_EXPECT_TRUE(issues.size() >= std::size_t{6});

  bool serialize_threw = false;
  try {
    static_cast<void>(SerializeTheme(theme));
  } catch (const std::invalid_argument&) {
    serialize_threw = true;
  }
  ZR_EXPECT_TRUE(serialize_threw);
}

ZR_TEST(ThemeIdentifierMatchesThePublishedSegmentGrammar) {
  auto theme = SafeDefaultTheme();
  theme.id = "valid.segment_2";
  ZR_EXPECT_TRUE(ValidateTheme(theme).empty());
  theme.id = "invalid.-segment";
  ZR_EXPECT_TRUE(!ValidateTheme(theme).empty());
  theme.id = "invalid._segment";
  ZR_EXPECT_TRUE(!ValidateTheme(theme).empty());
}

}  // namespace

int main() { return zrinput::test::RunAll(); }
