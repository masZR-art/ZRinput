#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zrinput::theme {

inline constexpr std::uint32_t kThemeSchemaVersion = 1;

struct ThemeSecurityLimits {
  static constexpr std::size_t kMaxThemeJsonBytes = 512 * 1024;
  static constexpr std::size_t kMaxAssetCount = 64;
  static constexpr std::size_t kMaxAssetPathBytes = 160;
  static constexpr std::size_t kMaxAssetFileBytes = 2 * 1024 * 1024;
  static constexpr std::size_t kMaxPackageAssetBytes = 8 * 1024 * 1024;
  static constexpr std::size_t kMaxDecodedAssetBytes = 16 * 1024 * 1024;
  static constexpr std::size_t kMaxPackageDecodedBytes = 64 * 1024 * 1024;
  static constexpr std::size_t kMaxDecodedAssetDimension = 2048;
};

struct AssetResourceInfo {
  std::size_t encoded_bytes = 0;
  std::size_t decoded_bytes = 0;
  std::size_t package_encoded_bytes = 0;
  std::size_t package_decoded_bytes = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

enum class ColorMode { kLight, kDark, kFollowSystem };
enum class MaterialKind { kSolid, kSystemBackdrop };
enum class CandidateLayout {
  kHorizontal,
  kVertical,
  kSingleLine,
  kCompact,
  kExpanded,
};
enum class CandidateNumberStyle { kPlain, kCompact };
enum class PageIndicatorStyle { kNone, kChevrons, kFraction };

struct Color {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
  std::uint8_t alpha = 255;

  bool operator==(const Color&) const = default;
};

struct Insets {
  double left = 0.0;
  double top = 0.0;
  double right = 0.0;
  double bottom = 0.0;

  bool operator==(const Insets&) const = default;
};

struct MaterialTokens {
  MaterialKind kind = MaterialKind::kSolid;
  double opacity = 1.0;

  bool operator==(const MaterialTokens&) const = default;
};

struct FontTokens {
  std::vector<std::string> families;
  double candidate_size = 16.0;
  double composition_size = 14.0;
  double annotation_size = 12.0;
  double index_size = 12.0;
  double status_size = 13.0;
  std::uint16_t candidate_weight = 400;
  std::uint16_t highlight_weight = 400;
  CandidateNumberStyle number_style = CandidateNumberStyle::kPlain;

  bool operator==(const FontTokens&) const = default;
};

struct MetricTokens {
  double min_width = 240.0;
  double max_width = 720.0;
  double max_height = 480.0;
  double border_width = 1.0;
  double corner_radius = 8.0;
  double highlight_corner_radius = 4.0;
  double row_height = 38.0;
  double candidate_gap = 4.0;
  double column_gap = 12.0;
  double composition_gap = 6.0;
  Insets window_padding;
  Insets candidate_padding;
  Insets composition_padding;

  bool operator==(const MetricTokens&) const = default;
};

struct ColorTokens {
  Color window_background;
  Color border;
  Color composition_text;
  Color candidate_text;
  Color candidate_index;
  Color candidate_annotation;
  Color highlight_background;
  Color highlight_text;
  Color candidate_hover_background;
  Color candidate_pressed_background;
  Color page_indicator;
  Color separator;
  Color status_icon;
  Color status_hover;
  Color status_pressed;
  Color status_disabled;

  bool operator==(const ColorTokens&) const = default;
};

struct ShadowTokens {
  bool enabled = true;
  Color color;
  double opacity = 0.22;
  double blur_radius = 20.0;
  double offset_x = 0.0;
  double offset_y = 8.0;

  bool operator==(const ShadowTokens&) const = default;
};

struct VariantTokens {
  ColorTokens colors;
  ShadowTokens shadow;

  bool operator==(const VariantTokens&) const = default;
};

struct LayoutTokens {
  CandidateLayout default_layout = CandidateLayout::kHorizontal;
  std::vector<CandidateLayout> supported;
  std::uint8_t candidate_count = 9;
  bool show_composition = true;
  bool horizontal_wrap = false;
  PageIndicatorStyle page_indicator = PageIndicatorStyle::kChevrons;

  bool operator==(const LayoutTokens&) const = default;
};

struct AnimationTokens {
  bool enabled = true;
  std::uint16_t show_ms = 120;
  std::uint16_t hide_ms = 90;
  std::uint16_t update_ms = 80;
  std::uint16_t hover_ms = 70;
  std::array<double, 4> easing{0.2, 0.0, 0.0, 1.0};
  double reduce_motion_scale = 0.0;

  bool operator==(const AnimationTokens&) const = default;
};

struct DpiTokens {
  std::uint16_t reference_dpi = 96;
  std::vector<double> scales;
  bool snap_to_pixels = true;
  double work_area_margin = 8.0;

  bool operator==(const DpiTokens&) const = default;
};

struct HdrTokens {
  bool adapt_to_display = true;
  double sdr_reference_white_nits = 203.0;
  bool clamp_translucent_layers = true;

  bool operator==(const HdrTokens&) const = default;
};

struct StatusButtonTokens {
  std::string id;
  std::string glyph;
  bool visible = true;
  std::uint8_t order = 0;

  bool operator==(const StatusButtonTokens&) const = default;
};

struct StatusBarTokens {
  bool visible = true;
  double button_size = 28.0;
  double gap = 2.0;
  Insets padding;
  std::uint32_t auto_hide_ms = 2500;
  std::vector<StatusButtonTokens> buttons;

  bool operator==(const StatusBarTokens&) const = default;
};

struct NineSliceResource {
  std::string path;
  Insets slice;

  bool operator==(const NineSliceResource&) const = default;
};

struct AssetTokens {
  std::map<std::string, std::string, std::less<>> icons;
  std::optional<NineSliceResource> nine_slice;

  bool operator==(const AssetTokens&) const = default;
};

struct Theme {
  std::uint32_t schema_version = kThemeSchemaVersion;
  std::string id;
  std::string name;
  std::string description;
  ColorMode color_mode = ColorMode::kFollowSystem;
  MaterialTokens material;
  FontTokens fonts;
  MetricTokens metrics;
  VariantTokens light;
  VariantTokens dark;
  LayoutTokens layout;
  AnimationTokens animation;
  DpiTokens dpi;
  HdrTokens hdr;
  StatusBarTokens status_bar;
  AssetTokens assets;

  bool operator==(const Theme&) const = default;
};

struct ValidationIssue {
  std::string path;
  std::string message;

  bool operator==(const ValidationIssue&) const = default;
};

struct ThemeLoadResult {
  Theme theme;
  bool used_fallback = false;
  std::vector<ValidationIssue> issues;
};

[[nodiscard]] const Theme& SafeDefaultTheme();
[[nodiscard]] std::vector<ValidationIssue> ValidateTheme(const Theme& theme);
[[nodiscard]] ThemeLoadResult LoadThemeJson(std::string_view json_text);
[[nodiscard]] std::string SerializeTheme(const Theme& theme,
                                         bool pretty = true);
[[nodiscard]] bool IsSafeAssetPath(std::string_view path) noexcept;
[[nodiscard]] bool IsAssetWithinSecurityBudget(
    const AssetResourceInfo& resource) noexcept;
[[nodiscard]] std::optional<Color> ParseColor(std::string_view text) noexcept;
[[nodiscard]] std::string FormatColor(Color color);

}  // namespace zrinput::theme
