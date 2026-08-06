#include "theme/theme.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "common/json.h"

namespace zrinput::theme {
namespace {

using JsonArray = json::Value::Array;
using JsonObject = json::Value::Object;

Color Rgba(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
           std::uint8_t alpha = 255) {
  return Color{red, green, blue, alpha};
}

bool FiniteInRange(double value, double minimum, double maximum) noexcept {
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool IsAsciiToken(std::string_view value, std::size_t minimum,
                  std::size_t maximum) noexcept {
  if (value.size() < minimum || value.size() > maximum) {
    return false;
  }
  const auto first = static_cast<unsigned char>(value.front());
  if (!((first >= static_cast<unsigned char>('a') &&
         first <= static_cast<unsigned char>('z')) ||
        (first >= static_cast<unsigned char>('0') &&
         first <= static_cast<unsigned char>('9')))) {
    return false;
  }
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    const bool valid = (byte >= static_cast<unsigned char>('a') &&
                        byte <= static_cast<unsigned char>('z')) ||
                       (byte >= static_cast<unsigned char>('0') &&
                        byte <= static_cast<unsigned char>('9')) ||
                       byte == static_cast<unsigned char>('-') ||
                       byte == static_cast<unsigned char>('_') ||
                       byte == static_cast<unsigned char>('.');
    if (!valid) {
      return false;
    }
  }
  return true;
}

bool IsThemeIdentifier(std::string_view value) noexcept {
  if (value.size() < 3 || value.size() > 64) {
    return false;
  }
  bool segment_start = true;
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte == static_cast<unsigned char>('.')) {
      if (segment_start) {
        return false;
      }
      segment_start = true;
      continue;
    }
    const bool alphanumeric =
        (byte >= static_cast<unsigned char>('a') &&
         byte <= static_cast<unsigned char>('z')) ||
        (byte >= static_cast<unsigned char>('0') &&
         byte <= static_cast<unsigned char>('9'));
    if ((!alphanumeric &&
         byte != static_cast<unsigned char>('-') &&
         byte != static_cast<unsigned char>('_')) ||
        (segment_start && !alphanumeric)) {
      return false;
    }
    segment_start = false;
  }
  return !segment_start;
}

bool IsWindowsDeviceComponent(std::string_view component) noexcept {
  const std::string_view stem = component.substr(0, component.find('.'));
  std::array<char, 4> lower{};
  if (stem.size() < 3 || stem.size() > lower.size()) {
    return false;
  }
  for (std::size_t index = 0; index < stem.size(); ++index) {
    const auto byte = static_cast<unsigned char>(stem[index]);
    lower[index] = byte >= static_cast<unsigned char>('A') &&
                           byte <= static_cast<unsigned char>('Z')
                       ? static_cast<char>(byte + ('a' - 'A'))
                       : static_cast<char>(byte);
  }
  const std::string_view normalized(lower.data(), stem.size());
  if (normalized == "con" || normalized == "prn" || normalized == "aux" ||
      normalized == "nul") {
    return true;
  }
  return normalized.size() == 4 &&
         (normalized.starts_with("com") || normalized.starts_with("lpt")) &&
         normalized.back() >= '1' && normalized.back() <= '9';
}

bool IsValidUtf8(std::string_view value) noexcept {
  try {
    static_cast<void>(json::Serialize(json::Value(std::string(value))));
    return true;
  } catch (...) {
    return false;
  }
}

bool HasControlCharacter(std::string_view value) noexcept {
  return std::any_of(value.begin(), value.end(), [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte < 0x20u || byte == 0x7Fu;
  });
}

std::size_t Utf8CodePointCount(std::string_view value) noexcept {
  return static_cast<std::size_t>(std::count_if(
      value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte & 0xC0u) != 0x80u;
      }));
}

void AddIssue(std::vector<ValidationIssue>& issues, std::string path,
              std::string message) {
  issues.push_back({std::move(path), std::move(message)});
}

void ValidateText(std::vector<ValidationIssue>& issues, std::string_view path,
                  std::string_view value, std::size_t minimum,
                  std::size_t maximum) {
  if (!IsValidUtf8(value) || HasControlCharacter(value)) {
    AddIssue(issues, std::string(path),
             "must be valid UTF-8 without control characters");
  } else {
    const std::size_t length = Utf8CodePointCount(value);
    if (length < minimum || length > maximum) {
      AddIssue(issues, std::string(path),
               "length is outside the allowed range");
    }
  }
}

void ValidateInsets(std::vector<ValidationIssue>& issues, std::string_view path,
                    const Insets& insets, double maximum) {
  const std::array<std::pair<std::string_view, double>, 4> values{{
      {"left", insets.left},
      {"top", insets.top},
      {"right", insets.right},
      {"bottom", insets.bottom},
  }};
  for (const auto& [name, value] : values) {
    if (!FiniteInRange(value, 0.0, maximum)) {
      AddIssue(issues, std::string(path) + "." + std::string(name),
               "must be finite and within the allowed range");
    }
  }
}

bool IsKnownLayout(CandidateLayout layout) noexcept {
  switch (layout) {
    case CandidateLayout::kHorizontal:
    case CandidateLayout::kVertical:
    case CandidateLayout::kSingleLine:
    case CandidateLayout::kCompact:
    case CandidateLayout::kExpanded:
      return true;
  }
  return false;
}

Theme MakeSafeDefaultTheme() {
  Theme theme;
  theme.id = "zrinput.windows-11-reference";
  theme.name = "Windows 11 Reference";
  theme.description =
      "Clean-room Windows 11 inspired theme using only data tokens and "
      "generic glyph semantics.";
  theme.color_mode = ColorMode::kFollowSystem;
  theme.material = {MaterialKind::kSystemBackdrop, 0.98};

  theme.fonts.families = {"Segoe UI", "Microsoft YaHei UI", "sans-serif"};
  theme.fonts.candidate_size = 16.0;
  theme.fonts.composition_size = 14.0;
  theme.fonts.annotation_size = 12.0;
  theme.fonts.index_size = 12.0;
  theme.fonts.status_size = 13.0;
  theme.fonts.candidate_weight = 400;
  theme.fonts.highlight_weight = 400;
  theme.fonts.number_style = CandidateNumberStyle::kPlain;

  theme.metrics.min_width = 240.0;
  theme.metrics.max_width = 720.0;
  theme.metrics.max_height = 480.0;
  theme.metrics.border_width = 1.0;
  theme.metrics.corner_radius = 8.0;
  theme.metrics.highlight_corner_radius = 4.0;
  theme.metrics.row_height = 38.0;
  theme.metrics.candidate_gap = 4.0;
  theme.metrics.column_gap = 12.0;
  theme.metrics.composition_gap = 6.0;
  theme.metrics.window_padding = {8.0, 8.0, 8.0, 8.0};
  theme.metrics.candidate_padding = {10.0, 6.0, 10.0, 6.0};
  theme.metrics.composition_padding = {10.0, 5.0, 10.0, 5.0};

  theme.light.colors = {
      Rgba(249, 249, 249, 245), Rgba(0, 0, 0, 24), Rgba(38, 38, 38),
      Rgba(32, 32, 32),         Rgba(96, 96, 96),  Rgba(102, 102, 102),
      Rgba(0, 95, 184, 28),     Rgba(16, 16, 16),  Rgba(0, 0, 0, 10),
      Rgba(0, 0, 0, 18),        Rgba(90, 90, 90),  Rgba(0, 0, 0, 18),
      Rgba(62, 62, 62),         Rgba(0, 0, 0, 12), Rgba(0, 0, 0, 20),
      Rgba(120, 120, 120, 120),
  };
  theme.light.shadow = {true, Rgba(0, 0, 0), 0.20, 20.0, 0.0, 8.0};

  theme.dark.colors = {
      Rgba(32, 32, 32, 245),   Rgba(255, 255, 255, 24),
      Rgba(245, 245, 245),     Rgba(248, 248, 248),
      Rgba(186, 186, 186),     Rgba(177, 177, 177),
      Rgba(96, 205, 255, 38),  Rgba(255, 255, 255),
      Rgba(255, 255, 255, 14), Rgba(255, 255, 255, 24),
      Rgba(190, 190, 190),     Rgba(255, 255, 255, 18),
      Rgba(232, 232, 232),     Rgba(255, 255, 255, 16),
      Rgba(255, 255, 255, 26), Rgba(150, 150, 150, 130),
  };
  theme.dark.shadow = {true, Rgba(0, 0, 0), 0.42, 24.0, 0.0, 8.0};

  theme.layout.default_layout = CandidateLayout::kHorizontal;
  theme.layout.supported = {
      CandidateLayout::kHorizontal, CandidateLayout::kVertical,
      CandidateLayout::kSingleLine, CandidateLayout::kCompact,
      CandidateLayout::kExpanded};
  theme.layout.candidate_count = 9;
  theme.layout.show_composition = true;
  theme.layout.horizontal_wrap = false;
  theme.layout.page_indicator = PageIndicatorStyle::kChevrons;

  theme.animation.enabled = true;
  theme.animation.show_ms = 120;
  theme.animation.hide_ms = 90;
  theme.animation.update_ms = 80;
  theme.animation.hover_ms = 70;
  theme.animation.easing = {0.2, 0.0, 0.0, 1.0};
  theme.animation.reduce_motion_scale = 0.0;

  theme.dpi.reference_dpi = 96;
  theme.dpi.scales = {1.0, 1.25, 1.5, 1.75, 2.0};
  theme.dpi.snap_to_pixels = true;
  theme.dpi.work_area_margin = 8.0;

  theme.hdr.adapt_to_display = true;
  theme.hdr.sdr_reference_white_nits = 203.0;
  theme.hdr.clamp_translucent_layers = true;

  theme.status_bar.visible = true;
  theme.status_bar.button_size = 28.0;
  theme.status_bar.gap = 2.0;
  theme.status_bar.padding = {4.0, 4.0, 4.0, 4.0};
  theme.status_bar.auto_hide_ms = 2500;
  theme.status_bar.buttons = {
      {"language", "language", true, 0},
      {"punctuation", "punctuation", true, 1},
      {"width", "character-width", false, 2},
      {"script", "script", true, 3},
      {"settings", "settings", true, 4},
  };
  return theme;
}

std::string ColorModeName(ColorMode mode) {
  switch (mode) {
    case ColorMode::kLight:
      return "light";
    case ColorMode::kDark:
      return "dark";
    case ColorMode::kFollowSystem:
      return "follow_system";
  }
  throw std::invalid_argument("unknown color mode");
}

std::string MaterialName(MaterialKind material) {
  switch (material) {
    case MaterialKind::kSolid:
      return "solid";
    case MaterialKind::kSystemBackdrop:
      return "system_backdrop";
  }
  throw std::invalid_argument("unknown material kind");
}

std::string LayoutName(CandidateLayout layout) {
  switch (layout) {
    case CandidateLayout::kHorizontal:
      return "horizontal";
    case CandidateLayout::kVertical:
      return "vertical";
    case CandidateLayout::kSingleLine:
      return "single_line";
    case CandidateLayout::kCompact:
      return "compact";
    case CandidateLayout::kExpanded:
      return "expanded";
  }
  throw std::invalid_argument("unknown candidate layout");
}

std::string NumberStyleName(CandidateNumberStyle style) {
  switch (style) {
    case CandidateNumberStyle::kPlain:
      return "plain";
    case CandidateNumberStyle::kCompact:
      return "compact";
  }
  throw std::invalid_argument("unknown candidate number style");
}

std::string PageIndicatorName(PageIndicatorStyle style) {
  switch (style) {
    case PageIndicatorStyle::kNone:
      return "none";
    case PageIndicatorStyle::kChevrons:
      return "chevrons";
    case PageIndicatorStyle::kFraction:
      return "fraction";
  }
  throw std::invalid_argument("unknown page indicator style");
}

class Decoder {
 public:
  Theme Decode(const json::Value& root) const {
    const JsonObject& object = AsObject(root, "$");
    CheckKeys(object, "$",
              {"schema_version", "id", "name", "description", "color_mode",
               "material", "fonts", "metrics", "light", "dark", "layout",
               "animation", "dpi", "hdr", "status_bar", "assets"});
    Theme theme;
    theme.schema_version =
        AsUnsigned(Require(object, "schema_version", "$"), "$.schema_version",
                   std::numeric_limits<std::uint32_t>::max());
    theme.id = AsString(Require(object, "id", "$"), "$.id");
    theme.name = AsString(Require(object, "name", "$"), "$.name");
    theme.description =
        AsString(Require(object, "description", "$"), "$.description");
    theme.color_mode = DecodeColorMode(
        AsString(Require(object, "color_mode", "$"), "$.color_mode"));
    theme.material = DecodeMaterial(Require(object, "material", "$"));
    theme.fonts = DecodeFonts(Require(object, "fonts", "$"));
    theme.metrics = DecodeMetrics(Require(object, "metrics", "$"));
    theme.light = DecodeVariant(Require(object, "light", "$"), "$.light");
    theme.dark = DecodeVariant(Require(object, "dark", "$"), "$.dark");
    theme.layout = DecodeLayout(Require(object, "layout", "$"));
    theme.animation = DecodeAnimation(Require(object, "animation", "$"));
    theme.dpi = DecodeDpi(Require(object, "dpi", "$"));
    theme.hdr = DecodeHdr(Require(object, "hdr", "$"));
    theme.status_bar = DecodeStatusBar(Require(object, "status_bar", "$"));
    theme.assets = DecodeAssets(Require(object, "assets", "$"));
    return theme;
  }

 private:
  [[noreturn]] static void Fail(std::string_view path, std::string message) {
    throw std::invalid_argument(std::string(path) + ": " + message);
  }

  static const JsonObject& AsObject(const json::Value& value,
                                    std::string_view path) {
    if (!value.IsObject()) {
      Fail(path, "expected an object");
    }
    return value.AsObject();
  }

  static const JsonArray& AsArray(const json::Value& value,
                                  std::string_view path) {
    if (!value.IsArray()) {
      Fail(path, "expected an array");
    }
    return value.AsArray();
  }

  static std::string AsString(const json::Value& value, std::string_view path) {
    if (!value.IsString()) {
      Fail(path, "expected a string");
    }
    return value.AsString();
  }

  static bool AsBool(const json::Value& value, std::string_view path) {
    if (!value.IsBool()) {
      Fail(path, "expected a boolean");
    }
    return value.AsBool();
  }

  static double AsNumber(const json::Value& value, std::string_view path) {
    if (!value.IsNumber()) {
      Fail(path, "expected a number");
    }
    return value.AsNumber();
  }

  template <typename Integer>
  static Integer AsUnsigned(const json::Value& value, std::string_view path,
                            Integer maximum) {
    const double number = AsNumber(value, path);
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(maximum) || std::floor(number) != number) {
      Fail(path, "expected a non-negative integer in range");
    }
    return static_cast<Integer>(number);
  }

  static const json::Value& Require(const JsonObject& object,
                                    std::string_view key,
                                    std::string_view path) {
    const auto found = object.find(key);
    if (found == object.end()) {
      Fail(path, "missing required property '" + std::string(key) + "'");
    }
    return found->second;
  }

  static void CheckKeys(const JsonObject& object, std::string_view path,
                        std::initializer_list<std::string_view> allowed) {
    for (const auto& [key, unused] : object) {
      static_cast<void>(unused);
      if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
        Fail(path, "contains an unknown property");
      }
    }
  }

  static ColorMode DecodeColorMode(std::string_view value) {
    if (value == "light") {
      return ColorMode::kLight;
    }
    if (value == "dark") {
      return ColorMode::kDark;
    }
    if (value == "follow_system") {
      return ColorMode::kFollowSystem;
    }
    Fail("$.color_mode", "unknown color mode");
  }

  static MaterialKind DecodeMaterialKind(std::string_view value) {
    if (value == "solid") {
      return MaterialKind::kSolid;
    }
    if (value == "system_backdrop") {
      return MaterialKind::kSystemBackdrop;
    }
    Fail("$.material.kind", "unknown material kind");
  }

  static CandidateLayout DecodeCandidateLayout(std::string_view value,
                                               std::string_view path) {
    if (value == "horizontal") {
      return CandidateLayout::kHorizontal;
    }
    if (value == "vertical") {
      return CandidateLayout::kVertical;
    }
    if (value == "single_line") {
      return CandidateLayout::kSingleLine;
    }
    if (value == "compact") {
      return CandidateLayout::kCompact;
    }
    if (value == "expanded") {
      return CandidateLayout::kExpanded;
    }
    Fail(path, "unknown candidate layout");
  }

  static CandidateNumberStyle DecodeNumberStyle(std::string_view value) {
    if (value == "plain") {
      return CandidateNumberStyle::kPlain;
    }
    if (value == "compact") {
      return CandidateNumberStyle::kCompact;
    }
    Fail("$.fonts.number_style", "unknown candidate number style");
  }

  static PageIndicatorStyle DecodePageIndicator(std::string_view value) {
    if (value == "none") {
      return PageIndicatorStyle::kNone;
    }
    if (value == "chevrons") {
      return PageIndicatorStyle::kChevrons;
    }
    if (value == "fraction") {
      return PageIndicatorStyle::kFraction;
    }
    Fail("$.layout.page_indicator", "unknown page indicator style");
  }

  static Insets DecodeInsets(const json::Value& value, std::string_view path) {
    const auto& object = AsObject(value, path);
    CheckKeys(object, path, {"left", "top", "right", "bottom"});
    return {
        AsNumber(Require(object, "left", path), std::string(path) + ".left"),
        AsNumber(Require(object, "top", path), std::string(path) + ".top"),
        AsNumber(Require(object, "right", path), std::string(path) + ".right"),
        AsNumber(Require(object, "bottom", path),
                 std::string(path) + ".bottom"),
    };
  }

  static Color DecodeColor(const json::Value& value, std::string_view path) {
    const std::string encoded = AsString(value, path);
    const auto color = ParseColor(encoded);
    if (!color) {
      Fail(path, "expected #RRGGBB or #RRGGBBAA");
    }
    return *color;
  }

  static MaterialTokens DecodeMaterial(const json::Value& value) {
    const auto& object = AsObject(value, "$.material");
    CheckKeys(object, "$.material", {"kind", "opacity"});
    return {
        DecodeMaterialKind(
            AsString(Require(object, "kind", "$.material"), "$.material.kind")),
        AsNumber(Require(object, "opacity", "$.material"),
                 "$.material.opacity"),
    };
  }

  static FontTokens DecodeFonts(const json::Value& value) {
    const auto& object = AsObject(value, "$.fonts");
    CheckKeys(object, "$.fonts",
              {"families", "candidate_size", "composition_size",
               "annotation_size", "index_size", "status_size",
               "candidate_weight", "highlight_weight", "number_style"});
    FontTokens fonts;
    const auto& families =
        AsArray(Require(object, "families", "$.fonts"), "$.fonts.families");
    fonts.families.reserve(families.size());
    for (std::size_t index = 0; index < families.size(); ++index) {
      fonts.families.push_back(AsString(
          families[index], "$.fonts.families[" + std::to_string(index) + "]"));
    }
    fonts.candidate_size = AsNumber(
        Require(object, "candidate_size", "$.fonts"), "$.fonts.candidate_size");
    fonts.composition_size =
        AsNumber(Require(object, "composition_size", "$.fonts"),
                 "$.fonts.composition_size");
    fonts.annotation_size =
        AsNumber(Require(object, "annotation_size", "$.fonts"),
                 "$.fonts.annotation_size");
    fonts.index_size = AsNumber(Require(object, "index_size", "$.fonts"),
                                "$.fonts.index_size");
    fonts.status_size = AsNumber(Require(object, "status_size", "$.fonts"),
                                 "$.fonts.status_size");
    fonts.candidate_weight = AsUnsigned<std::uint16_t>(
        Require(object, "candidate_weight", "$.fonts"),
        "$.fonts.candidate_weight", 1000);
    fonts.highlight_weight = AsUnsigned<std::uint16_t>(
        Require(object, "highlight_weight", "$.fonts"),
        "$.fonts.highlight_weight", 1000);
    fonts.number_style = DecodeNumberStyle(AsString(
        Require(object, "number_style", "$.fonts"), "$.fonts.number_style"));
    return fonts;
  }

  static MetricTokens DecodeMetrics(const json::Value& value) {
    const auto& object = AsObject(value, "$.metrics");
    CheckKeys(object, "$.metrics",
              {"min_width", "max_width", "max_height", "border_width",
               "corner_radius", "highlight_corner_radius", "row_height",
               "candidate_gap", "column_gap", "composition_gap",
               "window_padding", "candidate_padding", "composition_padding"});
    MetricTokens metrics;
    metrics.min_width = AsNumber(Require(object, "min_width", "$.metrics"),
                                 "$.metrics.min_width");
    metrics.max_width = AsNumber(Require(object, "max_width", "$.metrics"),
                                 "$.metrics.max_width");
    metrics.max_height = AsNumber(Require(object, "max_height", "$.metrics"),
                                  "$.metrics.max_height");
    metrics.border_width = AsNumber(
        Require(object, "border_width", "$.metrics"), "$.metrics.border_width");
    metrics.corner_radius =
        AsNumber(Require(object, "corner_radius", "$.metrics"),
                 "$.metrics.corner_radius");
    metrics.highlight_corner_radius =
        AsNumber(Require(object, "highlight_corner_radius", "$.metrics"),
                 "$.metrics.highlight_corner_radius");
    metrics.row_height = AsNumber(Require(object, "row_height", "$.metrics"),
                                  "$.metrics.row_height");
    metrics.candidate_gap =
        AsNumber(Require(object, "candidate_gap", "$.metrics"),
                 "$.metrics.candidate_gap");
    metrics.column_gap = AsNumber(Require(object, "column_gap", "$.metrics"),
                                  "$.metrics.column_gap");
    metrics.composition_gap =
        AsNumber(Require(object, "composition_gap", "$.metrics"),
                 "$.metrics.composition_gap");
    metrics.window_padding =
        DecodeInsets(Require(object, "window_padding", "$.metrics"),
                     "$.metrics.window_padding");
    metrics.candidate_padding =
        DecodeInsets(Require(object, "candidate_padding", "$.metrics"),
                     "$.metrics.candidate_padding");
    metrics.composition_padding =
        DecodeInsets(Require(object, "composition_padding", "$.metrics"),
                     "$.metrics.composition_padding");
    return metrics;
  }

  static ColorTokens DecodeColors(const json::Value& value,
                                  std::string_view path) {
    const auto& object = AsObject(value, path);
    CheckKeys(
        object, path,
        {"window_background", "border", "composition_text", "candidate_text",
         "candidate_index", "candidate_annotation", "highlight_background",
         "highlight_text", "candidate_hover_background",
         "candidate_pressed_background", "page_indicator", "separator",
         "status_icon", "status_hover", "status_pressed", "status_disabled"});
    const auto read = [&](std::string_view key) {
      return DecodeColor(Require(object, key, path),
                         std::string(path) + "." + std::string(key));
    };
    return {read("window_background"),
            read("border"),
            read("composition_text"),
            read("candidate_text"),
            read("candidate_index"),
            read("candidate_annotation"),
            read("highlight_background"),
            read("highlight_text"),
            read("candidate_hover_background"),
            read("candidate_pressed_background"),
            read("page_indicator"),
            read("separator"),
            read("status_icon"),
            read("status_hover"),
            read("status_pressed"),
            read("status_disabled")};
  }

  static ShadowTokens DecodeShadow(const json::Value& value,
                                   std::string_view path) {
    const auto& object = AsObject(value, path);
    CheckKeys(
        object, path,
        {"enabled", "color", "opacity", "blur_radius", "offset_x", "offset_y"});
    return {
        AsBool(Require(object, "enabled", path),
               std::string(path) + ".enabled"),
        DecodeColor(Require(object, "color", path),
                    std::string(path) + ".color"),
        AsNumber(Require(object, "opacity", path),
                 std::string(path) + ".opacity"),
        AsNumber(Require(object, "blur_radius", path),
                 std::string(path) + ".blur_radius"),
        AsNumber(Require(object, "offset_x", path),
                 std::string(path) + ".offset_x"),
        AsNumber(Require(object, "offset_y", path),
                 std::string(path) + ".offset_y"),
    };
  }

  static VariantTokens DecodeVariant(const json::Value& value,
                                     std::string_view path) {
    const auto& object = AsObject(value, path);
    CheckKeys(object, path, {"colors", "shadow"});
    return {
        DecodeColors(Require(object, "colors", path),
                     std::string(path) + ".colors"),
        DecodeShadow(Require(object, "shadow", path),
                     std::string(path) + ".shadow"),
    };
  }

  static LayoutTokens DecodeLayout(const json::Value& value) {
    const auto& object = AsObject(value, "$.layout");
    CheckKeys(object, "$.layout",
              {"default", "supported", "candidate_count", "show_composition",
               "horizontal_wrap", "page_indicator"});
    LayoutTokens layout;
    layout.default_layout = DecodeCandidateLayout(
        AsString(Require(object, "default", "$.layout"), "$.layout.default"),
        "$.layout.default");
    const auto& supported =
        AsArray(Require(object, "supported", "$.layout"), "$.layout.supported");
    layout.supported.reserve(supported.size());
    for (std::size_t index = 0; index < supported.size(); ++index) {
      const std::string path =
          "$.layout.supported[" + std::to_string(index) + "]";
      layout.supported.push_back(
          DecodeCandidateLayout(AsString(supported[index], path), path));
    }
    layout.candidate_count =
        AsUnsigned<std::uint8_t>(Require(object, "candidate_count", "$.layout"),
                                 "$.layout.candidate_count", 255);
    layout.show_composition =
        AsBool(Require(object, "show_composition", "$.layout"),
               "$.layout.show_composition");
    layout.horizontal_wrap =
        AsBool(Require(object, "horizontal_wrap", "$.layout"),
               "$.layout.horizontal_wrap");
    layout.page_indicator = DecodePageIndicator(
        AsString(Require(object, "page_indicator", "$.layout"),
                 "$.layout.page_indicator"));
    return layout;
  }

  static AnimationTokens DecodeAnimation(const json::Value& value) {
    const auto& object = AsObject(value, "$.animation");
    CheckKeys(object, "$.animation",
              {"enabled", "show_ms", "hide_ms", "update_ms", "hover_ms",
               "easing", "reduce_motion_scale"});
    AnimationTokens animation;
    animation.enabled = AsBool(Require(object, "enabled", "$.animation"),
                               "$.animation.enabled");
    animation.show_ms = AsUnsigned<std::uint16_t>(
        Require(object, "show_ms", "$.animation"), "$.animation.show_ms",
        std::numeric_limits<std::uint16_t>::max());
    animation.hide_ms = AsUnsigned<std::uint16_t>(
        Require(object, "hide_ms", "$.animation"), "$.animation.hide_ms",
        std::numeric_limits<std::uint16_t>::max());
    animation.update_ms = AsUnsigned<std::uint16_t>(
        Require(object, "update_ms", "$.animation"), "$.animation.update_ms",
        std::numeric_limits<std::uint16_t>::max());
    animation.hover_ms = AsUnsigned<std::uint16_t>(
        Require(object, "hover_ms", "$.animation"), "$.animation.hover_ms",
        std::numeric_limits<std::uint16_t>::max());
    const auto& easing =
        AsArray(Require(object, "easing", "$.animation"), "$.animation.easing");
    if (easing.size() != animation.easing.size()) {
      Fail("$.animation.easing", "expected exactly four numbers");
    }
    for (std::size_t index = 0; index < easing.size(); ++index) {
      animation.easing[index] = AsNumber(
          easing[index], "$.animation.easing[" + std::to_string(index) + "]");
    }
    animation.reduce_motion_scale =
        AsNumber(Require(object, "reduce_motion_scale", "$.animation"),
                 "$.animation.reduce_motion_scale");
    return animation;
  }

  static DpiTokens DecodeDpi(const json::Value& value) {
    const auto& object = AsObject(value, "$.dpi");
    CheckKeys(object, "$.dpi",
              {"reference", "scales", "snap_to_pixels", "work_area_margin"});
    DpiTokens dpi;
    dpi.reference_dpi = AsUnsigned<std::uint16_t>(
        Require(object, "reference", "$.dpi"), "$.dpi.reference",
        std::numeric_limits<std::uint16_t>::max());
    const auto& scales =
        AsArray(Require(object, "scales", "$.dpi"), "$.dpi.scales");
    dpi.scales.reserve(scales.size());
    for (std::size_t index = 0; index < scales.size(); ++index) {
      dpi.scales.push_back(AsNumber(
          scales[index], "$.dpi.scales[" + std::to_string(index) + "]"));
    }
    dpi.snap_to_pixels = AsBool(Require(object, "snap_to_pixels", "$.dpi"),
                                "$.dpi.snap_to_pixels");
    dpi.work_area_margin = AsNumber(
        Require(object, "work_area_margin", "$.dpi"), "$.dpi.work_area_margin");
    return dpi;
  }

  static HdrTokens DecodeHdr(const json::Value& value) {
    const auto& object = AsObject(value, "$.hdr");
    CheckKeys(object, "$.hdr",
              {"adapt_to_display", "sdr_reference_white_nits",
               "clamp_translucent_layers"});
    return {
        AsBool(Require(object, "adapt_to_display", "$.hdr"),
               "$.hdr.adapt_to_display"),
        AsNumber(Require(object, "sdr_reference_white_nits", "$.hdr"),
                 "$.hdr.sdr_reference_white_nits"),
        AsBool(Require(object, "clamp_translucent_layers", "$.hdr"),
               "$.hdr.clamp_translucent_layers"),
    };
  }

  static StatusBarTokens DecodeStatusBar(const json::Value& value) {
    const auto& object = AsObject(value, "$.status_bar");
    CheckKeys(object, "$.status_bar",
              {"visible", "button_size", "gap", "padding", "auto_hide_ms",
               "buttons"});
    StatusBarTokens status;
    status.visible = AsBool(Require(object, "visible", "$.status_bar"),
                            "$.status_bar.visible");
    status.button_size =
        AsNumber(Require(object, "button_size", "$.status_bar"),
                 "$.status_bar.button_size");
    status.gap =
        AsNumber(Require(object, "gap", "$.status_bar"), "$.status_bar.gap");
    status.padding = DecodeInsets(Require(object, "padding", "$.status_bar"),
                                  "$.status_bar.padding");
    status.auto_hide_ms = AsUnsigned<std::uint32_t>(
        Require(object, "auto_hide_ms", "$.status_bar"),
        "$.status_bar.auto_hide_ms", std::numeric_limits<std::uint32_t>::max());
    const auto& buttons = AsArray(Require(object, "buttons", "$.status_bar"),
                                  "$.status_bar.buttons");
    status.buttons.reserve(buttons.size());
    for (std::size_t index = 0; index < buttons.size(); ++index) {
      const std::string path =
          "$.status_bar.buttons[" + std::to_string(index) + "]";
      const auto& button = AsObject(buttons[index], path);
      CheckKeys(button, path, {"id", "glyph", "visible", "order"});
      status.buttons.push_back({
          AsString(Require(button, "id", path), path + ".id"),
          AsString(Require(button, "glyph", path), path + ".glyph"),
          AsBool(Require(button, "visible", path), path + ".visible"),
          AsUnsigned<std::uint8_t>(Require(button, "order", path),
                                   path + ".order", 255),
      });
    }
    return status;
  }

  static AssetTokens DecodeAssets(const json::Value& value) {
    const auto& object = AsObject(value, "$.assets");
    CheckKeys(object, "$.assets", {"icons", "nine_slice"});
    AssetTokens assets;
    const auto& icons =
        AsObject(Require(object, "icons", "$.assets"), "$.assets.icons");
    std::size_t index = 0;
    for (const auto& [name, path] : icons) {
      assets.icons.emplace(
          name, AsString(path, "$.assets.icons[" + std::to_string(index) + "]"));
      ++index;
    }
    const auto nine_slice = object.find("nine_slice");
    if (nine_slice != object.end()) {
      const auto& resource =
          AsObject(nine_slice->second, "$.assets.nine_slice");
      CheckKeys(resource, "$.assets.nine_slice", {"path", "slice"});
      assets.nine_slice = NineSliceResource{
          AsString(Require(resource, "path", "$.assets.nine_slice"),
                   "$.assets.nine_slice.path"),
          DecodeInsets(Require(resource, "slice", "$.assets.nine_slice"),
                       "$.assets.nine_slice.slice"),
      };
    }
    return assets;
  }
};

json::Value Object(
    std::initializer_list<std::pair<std::string, json::Value>> members) {
  JsonObject object;
  for (const auto& [name, value] : members) {
    object.emplace(name, value);
  }
  return json::Value(std::move(object));
}

json::Value EncodeInsets(const Insets& value) {
  return Object({{"left", value.left},
                 {"top", value.top},
                 {"right", value.right},
                 {"bottom", value.bottom}});
}

json::Value EncodeColors(const ColorTokens& colors) {
  return Object({
      {"window_background", FormatColor(colors.window_background)},
      {"border", FormatColor(colors.border)},
      {"composition_text", FormatColor(colors.composition_text)},
      {"candidate_text", FormatColor(colors.candidate_text)},
      {"candidate_index", FormatColor(colors.candidate_index)},
      {"candidate_annotation", FormatColor(colors.candidate_annotation)},
      {"highlight_background", FormatColor(colors.highlight_background)},
      {"highlight_text", FormatColor(colors.highlight_text)},
      {"candidate_hover_background",
       FormatColor(colors.candidate_hover_background)},
      {"candidate_pressed_background",
       FormatColor(colors.candidate_pressed_background)},
      {"page_indicator", FormatColor(colors.page_indicator)},
      {"separator", FormatColor(colors.separator)},
      {"status_icon", FormatColor(colors.status_icon)},
      {"status_hover", FormatColor(colors.status_hover)},
      {"status_pressed", FormatColor(colors.status_pressed)},
      {"status_disabled", FormatColor(colors.status_disabled)},
  });
}

json::Value EncodeShadow(const ShadowTokens& shadow) {
  return Object({{"enabled", shadow.enabled},
                 {"color", FormatColor(shadow.color)},
                 {"opacity", shadow.opacity},
                 {"blur_radius", shadow.blur_radius},
                 {"offset_x", shadow.offset_x},
                 {"offset_y", shadow.offset_y}});
}

json::Value EncodeVariant(const VariantTokens& variant) {
  return Object({{"colors", EncodeColors(variant.colors)},
                 {"shadow", EncodeShadow(variant.shadow)}});
}

json::Value EncodeTheme(const Theme& theme) {
  JsonArray font_families;
  for (const auto& family : theme.fonts.families) {
    font_families.emplace_back(family);
  }
  JsonArray layouts;
  for (const auto layout : theme.layout.supported) {
    layouts.emplace_back(LayoutName(layout));
  }
  JsonArray easing;
  for (const double value : theme.animation.easing) {
    easing.emplace_back(value);
  }
  JsonArray scales;
  for (const double value : theme.dpi.scales) {
    scales.emplace_back(value);
  }
  JsonArray buttons;
  for (const auto& button : theme.status_bar.buttons) {
    buttons.emplace_back(Object({
        {"id", button.id},
        {"glyph", button.glyph},
        {"visible", button.visible},
        {"order", static_cast<double>(button.order)},
    }));
  }
  JsonObject icons;
  for (const auto& [name, path] : theme.assets.icons) {
    icons.emplace(name, json::Value(path));
  }
  JsonObject assets;
  assets.emplace("icons", json::Value(std::move(icons)));
  if (theme.assets.nine_slice) {
    assets.emplace(
        "nine_slice",
        Object({{"path", theme.assets.nine_slice->path},
                {"slice", EncodeInsets(theme.assets.nine_slice->slice)}}));
  }

  return Object({
      {"schema_version", static_cast<double>(theme.schema_version)},
      {"id", theme.id},
      {"name", theme.name},
      {"description", theme.description},
      {"color_mode", ColorModeName(theme.color_mode)},
      {"material", Object({{"kind", MaterialName(theme.material.kind)},
                           {"opacity", theme.material.opacity}})},
      {"fonts", Object({
                    {"families", json::Value(std::move(font_families))},
                    {"candidate_size", theme.fonts.candidate_size},
                    {"composition_size", theme.fonts.composition_size},
                    {"annotation_size", theme.fonts.annotation_size},
                    {"index_size", theme.fonts.index_size},
                    {"status_size", theme.fonts.status_size},
                    {"candidate_weight",
                     static_cast<double>(theme.fonts.candidate_weight)},
                    {"highlight_weight",
                     static_cast<double>(theme.fonts.highlight_weight)},
                    {"number_style", NumberStyleName(theme.fonts.number_style)},
                })},
      {"metrics",
       Object({
           {"min_width", theme.metrics.min_width},
           {"max_width", theme.metrics.max_width},
           {"max_height", theme.metrics.max_height},
           {"border_width", theme.metrics.border_width},
           {"corner_radius", theme.metrics.corner_radius},
           {"highlight_corner_radius", theme.metrics.highlight_corner_radius},
           {"row_height", theme.metrics.row_height},
           {"candidate_gap", theme.metrics.candidate_gap},
           {"column_gap", theme.metrics.column_gap},
           {"composition_gap", theme.metrics.composition_gap},
           {"window_padding", EncodeInsets(theme.metrics.window_padding)},
           {"candidate_padding", EncodeInsets(theme.metrics.candidate_padding)},
           {"composition_padding",
            EncodeInsets(theme.metrics.composition_padding)},
       })},
      {"light", EncodeVariant(theme.light)},
      {"dark", EncodeVariant(theme.dark)},
      {"layout",
       Object({
           {"default", LayoutName(theme.layout.default_layout)},
           {"supported", json::Value(std::move(layouts))},
           {"candidate_count",
            static_cast<double>(theme.layout.candidate_count)},
           {"show_composition", theme.layout.show_composition},
           {"horizontal_wrap", theme.layout.horizontal_wrap},
           {"page_indicator", PageIndicatorName(theme.layout.page_indicator)},
       })},
      {"animation",
       Object({
           {"enabled", theme.animation.enabled},
           {"show_ms", static_cast<double>(theme.animation.show_ms)},
           {"hide_ms", static_cast<double>(theme.animation.hide_ms)},
           {"update_ms", static_cast<double>(theme.animation.update_ms)},
           {"hover_ms", static_cast<double>(theme.animation.hover_ms)},
           {"easing", json::Value(std::move(easing))},
           {"reduce_motion_scale", theme.animation.reduce_motion_scale},
       })},
      {"dpi", Object({
                  {"reference", static_cast<double>(theme.dpi.reference_dpi)},
                  {"scales", json::Value(std::move(scales))},
                  {"snap_to_pixels", theme.dpi.snap_to_pixels},
                  {"work_area_margin", theme.dpi.work_area_margin},
              })},
      {"hdr",
       Object({
           {"adapt_to_display", theme.hdr.adapt_to_display},
           {"sdr_reference_white_nits", theme.hdr.sdr_reference_white_nits},
           {"clamp_translucent_layers", theme.hdr.clamp_translucent_layers},
       })},
      {"status_bar",
       Object({
           {"visible", theme.status_bar.visible},
           {"button_size", theme.status_bar.button_size},
           {"gap", theme.status_bar.gap},
           {"padding", EncodeInsets(theme.status_bar.padding)},
           {"auto_hide_ms", static_cast<double>(theme.status_bar.auto_hide_ms)},
           {"buttons", json::Value(std::move(buttons))},
       })},
      {"assets", json::Value(std::move(assets))},
  });
}

void ValidateVariant(std::vector<ValidationIssue>& issues,
                     std::string_view path, const VariantTokens& variant) {
  if (variant.colors.window_background.alpha < 160) {
    AddIssue(issues, std::string(path) + ".colors.window_background",
             "background alpha must be at least 160");
  }
  const std::array<std::pair<std::string_view, Color>, 7> readable_colors{{
      {"composition_text", variant.colors.composition_text},
      {"candidate_text", variant.colors.candidate_text},
      {"candidate_index", variant.colors.candidate_index},
      {"candidate_annotation", variant.colors.candidate_annotation},
      {"highlight_text", variant.colors.highlight_text},
      {"page_indicator", variant.colors.page_indicator},
      {"status_icon", variant.colors.status_icon},
  }};
  for (const auto& [name, color] : readable_colors) {
    if (color.alpha < 128) {
      AddIssue(issues, std::string(path) + ".colors." + std::string(name),
               "readable foreground alpha must be at least 128");
    }
  }
  if (!FiniteInRange(variant.shadow.opacity, 0.0, 1.0)) {
    AddIssue(issues, std::string(path) + ".shadow.opacity",
             "must be between 0 and 1");
  }
  if (!FiniteInRange(variant.shadow.blur_radius, 0.0, 64.0)) {
    AddIssue(issues, std::string(path) + ".shadow.blur_radius",
             "must be between 0 and 64 DIPs");
  }
  if (!FiniteInRange(variant.shadow.offset_x, -32.0, 32.0) ||
      !FiniteInRange(variant.shadow.offset_y, -32.0, 32.0)) {
    AddIssue(issues, std::string(path) + ".shadow",
             "shadow offsets must be finite and between -32 and 32 DIPs");
  }
}

}  // namespace

const Theme& SafeDefaultTheme() {
  static const Theme theme = MakeSafeDefaultTheme();
  return theme;
}

std::optional<Color> ParseColor(std::string_view text) noexcept {
  if ((text.size() != 7 && text.size() != 9) || text.front() != '#') {
    return std::nullopt;
  }
  std::array<std::uint8_t, 4> channels{0, 0, 0, 255};
  const std::size_t channel_count = text.size() == 9 ? 4 : 3;
  for (std::size_t index = 0; index < channel_count; ++index) {
    const int high = [](char value) noexcept {
      if (value >= '0' && value <= '9') return value - '0';
      if (value >= 'a' && value <= 'f') return value - 'a' + 10;
      if (value >= 'A' && value <= 'F') return value - 'A' + 10;
      return -1;
    }(text[1 + index * 2]);
    const int low = [](char value) noexcept {
      if (value >= '0' && value <= '9') return value - '0';
      if (value >= 'a' && value <= 'f') return value - 'a' + 10;
      if (value >= 'A' && value <= 'F') return value - 'A' + 10;
      return -1;
    }(text[2 + index * 2]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    channels[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return Color{channels[0], channels[1], channels[2], channels[3]};
}

std::string FormatColor(Color color) {
  constexpr char kHex[] = "0123456789ABCDEF";
  const bool include_alpha = color.alpha != 255;
  std::string result(include_alpha ? 9 : 7, '#');
  const std::array<std::uint8_t, 4> channels{color.red, color.green, color.blue,
                                             color.alpha};
  const std::size_t count = include_alpha ? 4 : 3;
  for (std::size_t index = 0; index < count; ++index) {
    result[1 + index * 2] = kHex[(channels[index] >> 4u) & 0x0Fu];
    result[2 + index * 2] = kHex[channels[index] & 0x0Fu];
  }
  return result;
}

bool IsSafeAssetPath(std::string_view path) noexcept {
  if (path.empty() || path.size() > ThemeSecurityLimits::kMaxAssetPathBytes ||
      !path.starts_with("assets/") || path.back() == '/' ||
      path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos ||
      path.find('%') != std::string_view::npos) {
    return false;
  }
  std::size_t start = 0;
  while (start < path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end =
        slash == std::string_view::npos ? path.size() : slash;
    const std::string_view component = path.substr(start, end - start);
    if (component.empty() || component == "." || component == ".." ||
        component.back() == '.' || IsWindowsDeviceComponent(component)) {
      return false;
    }
    for (const char character : component) {
      const auto byte = static_cast<unsigned char>(character);
      const bool valid = (byte >= static_cast<unsigned char>('a') &&
                          byte <= static_cast<unsigned char>('z')) ||
                         (byte >= static_cast<unsigned char>('A') &&
                          byte <= static_cast<unsigned char>('Z')) ||
                         (byte >= static_cast<unsigned char>('0') &&
                          byte <= static_cast<unsigned char>('9')) ||
                         byte == static_cast<unsigned char>('_') ||
                         byte == static_cast<unsigned char>('-') ||
                         byte == static_cast<unsigned char>('.');
      if (!valid) {
        return false;
      }
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return path.ends_with(".png") || path.ends_with(".webp");
}

bool IsAssetWithinSecurityBudget(const AssetResourceInfo& resource) noexcept {
  return resource.encoded_bytes > 0 && resource.decoded_bytes > 0 &&
         resource.package_encoded_bytes >= resource.encoded_bytes &&
         resource.package_decoded_bytes >= resource.decoded_bytes &&
         resource.width > 0 && resource.height > 0 &&
         resource.encoded_bytes <= ThemeSecurityLimits::kMaxAssetFileBytes &&
         resource.package_encoded_bytes <=
             ThemeSecurityLimits::kMaxPackageAssetBytes &&
         resource.decoded_bytes <= ThemeSecurityLimits::kMaxDecodedAssetBytes &&
         resource.package_decoded_bytes <=
             ThemeSecurityLimits::kMaxPackageDecodedBytes &&
         resource.width <= ThemeSecurityLimits::kMaxDecodedAssetDimension &&
         resource.height <= ThemeSecurityLimits::kMaxDecodedAssetDimension;
}

std::vector<ValidationIssue> ValidateTheme(const Theme& theme) {
  std::vector<ValidationIssue> issues;
  if (theme.schema_version != kThemeSchemaVersion) {
    AddIssue(issues, "$.schema_version", "unsupported theme schema version");
  }
  if (!IsThemeIdentifier(theme.id)) {
    AddIssue(issues, "$.id",
             "must be a portable lowercase identifier without empty segments");
  }
  ValidateText(issues, "$.name", theme.name, 1, 64);
  ValidateText(issues, "$.description", theme.description, 0, 512);

  if (theme.color_mode != ColorMode::kLight &&
      theme.color_mode != ColorMode::kDark &&
      theme.color_mode != ColorMode::kFollowSystem) {
    AddIssue(issues, "$.color_mode", "unknown color mode");
  }

  if (theme.material.kind != MaterialKind::kSolid &&
      theme.material.kind != MaterialKind::kSystemBackdrop) {
    AddIssue(issues, "$.material.kind", "unknown material kind");
  }
  if (!FiniteInRange(theme.material.opacity, 0.5, 1.0)) {
    AddIssue(issues, "$.material.opacity", "must be between 0.5 and 1");
  }

  if (theme.fonts.families.empty() || theme.fonts.families.size() > 8) {
    AddIssue(issues, "$.fonts.families", "must contain between 1 and 8 fonts");
  }
  std::set<std::string, std::less<>> font_names;
  for (std::size_t index = 0; index < theme.fonts.families.size(); ++index) {
    const std::string path = "$.fonts.families[" + std::to_string(index) + "]";
    ValidateText(issues, path, theme.fonts.families[index], 1, 64);
    if (!font_names.insert(theme.fonts.families[index]).second) {
      AddIssue(issues, path, "duplicate font family");
    }
  }
  const std::array<std::pair<std::string_view, double>, 5> font_sizes{{
      {"candidate_size", theme.fonts.candidate_size},
      {"composition_size", theme.fonts.composition_size},
      {"annotation_size", theme.fonts.annotation_size},
      {"index_size", theme.fonts.index_size},
      {"status_size", theme.fonts.status_size},
  }};
  for (const auto& [name, value] : font_sizes) {
    if (!FiniteInRange(value, 8.0, 48.0)) {
      AddIssue(issues, "$.fonts." + std::string(name),
               "must be between 8 and 48 DIPs");
    }
  }
  if (theme.fonts.candidate_weight < 100 ||
      theme.fonts.candidate_weight > 900 ||
      theme.fonts.highlight_weight < 100 ||
      theme.fonts.highlight_weight > 900) {
    AddIssue(issues, "$.fonts", "font weights must be between 100 and 900");
  }
  if (theme.fonts.number_style != CandidateNumberStyle::kPlain &&
      theme.fonts.number_style != CandidateNumberStyle::kCompact) {
    AddIssue(issues, "$.fonts.number_style", "unknown number style");
  }

  if (!FiniteInRange(theme.metrics.min_width, 80.0, 2048.0) ||
      !FiniteInRange(theme.metrics.max_width, 120.0, 4096.0) ||
      theme.metrics.max_width < theme.metrics.min_width) {
    AddIssue(issues, "$.metrics",
             "window widths are invalid or max_width is below min_width");
  }
  if (!FiniteInRange(theme.metrics.max_height, 80.0, 2160.0)) {
    AddIssue(issues, "$.metrics.max_height",
             "must be between 80 and 2160 DIPs");
  }
  if (!FiniteInRange(theme.metrics.border_width, 0.0, 4.0) ||
      !FiniteInRange(theme.metrics.corner_radius, 0.0, 32.0) ||
      !FiniteInRange(theme.metrics.highlight_corner_radius, 0.0, 24.0) ||
      !FiniteInRange(theme.metrics.row_height, 20.0, 96.0) ||
      !FiniteInRange(theme.metrics.candidate_gap, 0.0, 48.0) ||
      !FiniteInRange(theme.metrics.column_gap, 0.0, 64.0) ||
      !FiniteInRange(theme.metrics.composition_gap, 0.0, 48.0)) {
    AddIssue(issues, "$.metrics", "one or more metric tokens are out of range");
  }
  ValidateInsets(issues, "$.metrics.window_padding",
                 theme.metrics.window_padding, 64.0);
  ValidateInsets(issues, "$.metrics.candidate_padding",
                 theme.metrics.candidate_padding, 64.0);
  ValidateInsets(issues, "$.metrics.composition_padding",
                 theme.metrics.composition_padding, 64.0);

  ValidateVariant(issues, "$.light", theme.light);
  ValidateVariant(issues, "$.dark", theme.dark);

  if (!IsKnownLayout(theme.layout.default_layout)) {
    AddIssue(issues, "$.layout.default", "unknown candidate layout");
  }
  if (theme.layout.supported.empty() || theme.layout.supported.size() > 5) {
    AddIssue(issues, "$.layout.supported",
             "must contain between 1 and 5 layouts");
  }
  std::set<CandidateLayout> layouts;
  for (const CandidateLayout layout : theme.layout.supported) {
    if (!IsKnownLayout(layout)) {
      AddIssue(issues, "$.layout.supported", "contains an unknown layout");
    } else if (!layouts.insert(layout).second) {
      AddIssue(issues, "$.layout.supported", "contains a duplicate layout");
    }
  }
  if (!layouts.contains(theme.layout.default_layout)) {
    AddIssue(issues, "$.layout.default",
             "default layout must be included in supported layouts");
  }
  if (theme.layout.candidate_count < 1 || theme.layout.candidate_count > 10) {
    AddIssue(issues, "$.layout.candidate_count", "must be between 1 and 10");
  }
  if (theme.layout.page_indicator != PageIndicatorStyle::kNone &&
      theme.layout.page_indicator != PageIndicatorStyle::kChevrons &&
      theme.layout.page_indicator != PageIndicatorStyle::kFraction) {
    AddIssue(issues, "$.layout.page_indicator", "unknown page indicator style");
  }

  if (theme.animation.show_ms > 1000 || theme.animation.hide_ms > 1000 ||
      theme.animation.update_ms > 1000 || theme.animation.hover_ms > 1000) {
    AddIssue(issues, "$.animation",
             "animation durations must not exceed 1000 ms");
  }
  if (!FiniteInRange(theme.animation.easing[0], 0.0, 1.0) ||
      !FiniteInRange(theme.animation.easing[1], -2.0, 2.0) ||
      !FiniteInRange(theme.animation.easing[2], 0.0, 1.0) ||
      !FiniteInRange(theme.animation.easing[3], -2.0, 2.0)) {
    AddIssue(issues, "$.animation.easing",
             "invalid cubic Bezier control points");
  }
  if (!FiniteInRange(theme.animation.reduce_motion_scale, 0.0, 1.0)) {
    AddIssue(issues, "$.animation.reduce_motion_scale",
             "must be between 0 and 1");
  }

  if (theme.dpi.reference_dpi < 72 || theme.dpi.reference_dpi > 240) {
    AddIssue(issues, "$.dpi.reference", "must be between 72 and 240 DPI");
  }
  if (theme.dpi.scales.empty() || theme.dpi.scales.size() > 16) {
    AddIssue(issues, "$.dpi.scales", "must contain between 1 and 16 scales");
  }
  bool contains_one = false;
  double previous = 0.0;
  for (std::size_t index = 0; index < theme.dpi.scales.size(); ++index) {
    const double scale = theme.dpi.scales[index];
    if (!FiniteInRange(scale, 0.75, 4.0) || (index != 0 && scale <= previous)) {
      AddIssue(
          issues, "$.dpi.scales",
          "scales must be finite, unique, ascending, and between 0.75 and 4");
      break;
    }
    contains_one = contains_one || std::abs(scale - 1.0) < 0.000001;
    previous = scale;
  }
  if (!contains_one) {
    AddIssue(issues, "$.dpi.scales", "must include the 1.0 scale");
  }
  if (!FiniteInRange(theme.dpi.work_area_margin, 0.0, 64.0)) {
    AddIssue(issues, "$.dpi.work_area_margin", "must be between 0 and 64 DIPs");
  }
  if (!FiniteInRange(theme.hdr.sdr_reference_white_nits, 80.0, 400.0)) {
    AddIssue(issues, "$.hdr.sdr_reference_white_nits",
             "must be between 80 and 400 nits");
  }

  if (!FiniteInRange(theme.status_bar.button_size, 20.0, 64.0) ||
      !FiniteInRange(theme.status_bar.gap, 0.0, 32.0) ||
      theme.status_bar.auto_hide_ms > 60000) {
    AddIssue(issues, "$.status_bar",
             "status bar dimensions or timing are invalid");
  }
  ValidateInsets(issues, "$.status_bar.padding", theme.status_bar.padding,
                 32.0);
  if (theme.status_bar.buttons.size() > 16) {
    AddIssue(issues, "$.status_bar.buttons",
             "must not contain more than 16 buttons");
  }
  std::set<std::string, std::less<>> button_ids;
  std::set<std::uint8_t> button_orders;
  for (std::size_t index = 0; index < theme.status_bar.buttons.size();
       ++index) {
    const auto& button = theme.status_bar.buttons[index];
    const std::string path =
        "$.status_bar.buttons[" + std::to_string(index) + "]";
    if (!IsAsciiToken(button.id, 1, 32)) {
      AddIssue(issues, path + ".id", "must be a portable identifier");
    }
    if (!IsAsciiToken(button.glyph, 1, 32)) {
      AddIssue(issues, path + ".glyph", "must be a generic glyph identifier");
    }
    if (!button_ids.insert(button.id).second) {
      AddIssue(issues, path + ".id", "duplicate button id");
    }
    if (!button_orders.insert(button.order).second) {
      AddIssue(issues, path + ".order", "duplicate button order");
    }
  }

  const std::size_t asset_count =
      theme.assets.icons.size() + (theme.assets.nine_slice ? 1u : 0u);
  if (asset_count > ThemeSecurityLimits::kMaxAssetCount) {
    AddIssue(issues, "$.assets", "asset count exceeds the security budget");
  }
  std::size_t icon_index = 0;
  for (const auto& [name, path] : theme.assets.icons) {
    const std::string issue_path =
        "$.assets.icons[" + std::to_string(icon_index) + "]";
    if (!IsAsciiToken(name, 1, 32)) {
      AddIssue(issues, issue_path,
               "icon name must be a portable identifier");
    }
    if (!IsSafeAssetPath(path)) {
      AddIssue(issues, issue_path,
               "asset path must remain below assets/ and use .png or .webp");
    }
    ++icon_index;
  }
  if (theme.assets.nine_slice) {
    if (!IsSafeAssetPath(theme.assets.nine_slice->path)) {
      AddIssue(issues, "$.assets.nine_slice.path",
               "asset path must remain below assets/ and use .png or .webp");
    }
    ValidateInsets(issues, "$.assets.nine_slice.slice",
                   theme.assets.nine_slice->slice, 512.0);
  }
  return issues;
}

ThemeLoadResult LoadThemeJson(std::string_view json_text) {
  ThemeLoadResult result;
  result.theme = SafeDefaultTheme();
  result.used_fallback = true;
  try {
    json::ParseLimits limits;
    limits.max_input_bytes = ThemeSecurityLimits::kMaxThemeJsonBytes;
    limits.max_depth = 24;
    limits.max_string_bytes = 4096;
    limits.max_number_characters = 48;
    limits.max_container_items = 512;
    limits.max_total_values = 4096;
    Theme decoded = Decoder{}.Decode(json::Parse(json_text, limits));
    auto issues = ValidateTheme(decoded);
    if (!issues.empty()) {
      result.issues = std::move(issues);
      return result;
    }
    result.theme = std::move(decoded);
    result.used_fallback = false;
    return result;
  } catch (const std::exception& error) {
    result.issues.push_back({"$", error.what()});
    return result;
  } catch (...) {
    result.issues.push_back({"$", "unknown theme loading failure"});
    return result;
  }
}

std::string SerializeTheme(const Theme& theme, bool pretty) {
  const auto issues = ValidateTheme(theme);
  if (!issues.empty()) {
    throw std::invalid_argument(issues.front().path + ": " +
                                issues.front().message);
  }
  return json::Serialize(EncodeTheme(theme), pretty);
}

}  // namespace zrinput::theme
