# ZRinput Theme Specification v1

ZRinput themes are inert data. A theme can select validated design tokens and
refer to bounded bitmap resources, but it cannot load a DLL, run a script,
declare a URI, or execute an action. The normative machine-readable schema is
`themes/schema/theme.schema.json`. Runtime validation in `src/theme/theme.cpp`
is authoritative because JSON Schema alone cannot enforce every cross-field
invariant or resource budget.

`Windows 11 Reference` is a clean-room theme inspired by Windows 11 geometry
and interaction timing. It contains no Microsoft logo, icon, sound, binary, or
other extracted resource. Font family names are ordinary fallback requests;
the theme does not redistribute those fonts. The renderer must use generic,
ZRinput-owned glyphs for status actions.

## Package layout

A v1 theme package has this logical shape:

```text
theme-package/
  theme.json
  assets/                 # optional
    icon-name.png
    panel.webp
```

Archives are never interpreted as executable packages. The installer must
extract into a newly-created staging directory, reject links and alternate
data streams, validate every normalized destination, validate the manifest and
all decoded images, and then atomically rename the staging directory into the
theme store. Installation failure leaves the active theme unchanged. Removal
first switches away from the theme and then removes its package.

Only `.png` and `.webp` resources are accepted in v1. SVG, HTML, fonts, audio,
video, shaders, DLLs, shortcuts, and scripts are intentionally unsupported.
Built-in glyph names in `status_bar.buttons[*].glyph` are semantics, not file
paths.

## JSON contract

JSON is UTF-8 without a byte-order mark. The parser rejects duplicate object
keys, invalid UTF-8, unpaired Unicode surrogates, unescaped control characters,
non-finite or unrepresentable numbers, leading-zero numbers, trailing data,
unknown theme properties, and malformed escapes. Comments and trailing commas
are not part of JSON and are rejected.

The runtime parser applies these limits before creating a theme snapshot:

| Budget | v1 limit |
| --- | ---: |
| `theme.json` encoded size | 512 KiB |
| JSON nesting depth | 24 for theme loading |
| One decoded JSON string | 4 KiB for theme loading |
| One JSON number | 48 characters for theme loading |
| Items in one JSON container | 512 |
| Total JSON values | 4096 |
| Referenced assets | 64 |
| One asset path | 160 bytes |
| One encoded asset | 2 MiB |
| All encoded assets | 8 MiB |
| One decoded asset | 16 MiB |
| All resident decoded assets | 64 MiB |
| Decoded image width or height | 2048 pixels |

The final five image budgets are package-loader obligations. They must be
checked both before and after decoding so a compressed image cannot cause an
unbounded allocation. Package loaders pass their accumulated byte counts and
decoded dimensions to `IsAssetWithinSecurityBudget()` before publishing a
resource.

Every object except `assets.icons` rejects unknown properties. This makes a
misspelled token a load failure instead of a silently ignored customization.
All required fields are explicit so serialization is deterministic.

## Versioning

`schema_version` is an integer and must be `1`. A future reader may migrate an
older version into the current in-memory model, but it must never guess how to
interpret a newer version. ZRinput v1 therefore rejects every other value and
uses the safe built-in theme. Migration must happen on a copy and must not
modify the installed source package until the migrated result is fully valid.

## Color and material

`color_mode` accepts:

| Value | Behavior |
| --- | --- |
| `light` | Always use `light` tokens. |
| `dark` | Always use `dark` tokens. |
| `follow_system` | Select the variant from the current Windows app/system mode. |

Each variant is always present, even for a fixed mode, so preview and mode
switching never require reparsing. Colors use `#RRGGBB` or `#RRGGBBAA`; alpha
is the final byte. Values are unpremultiplied sRGB. The renderer premultiplies
at the drawing boundary. On an HDR surface it converts sRGB into the surface
color space and clamps UI luminance according to the Windows advanced-color
policy; theme authors do not encode HDR nits in v1.

`material.kind` is `solid` or `system_backdrop`. `system_backdrop` is a request,
not a guarantee: remote sessions, battery policy, accessibility settings, and
unsupported popup composition paths must fall back to the same variant's
`window_background`. `material.opacity` is applied after choosing that
fallback and is restricted to 0.5 through 1.0.

Each `colors` object defines:

| Token | Use |
| --- | --- |
| `window_background` | Candidate/status surface fallback fill. |
| `border` | One-pixel-aligned outer border. |
| `composition_text` | Raw composition/pinyin text. |
| `candidate_text` | Candidate body text. |
| `candidate_index` | Numeric selection label. |
| `candidate_annotation` | Reading or explanatory annotation. |
| `highlight_background` | Current/hovered candidate fill. |
| `highlight_text` | Text on the current candidate. |
| `candidate_hover_background` | Pointer-hover candidate fill. |
| `candidate_pressed_background` | Mouse/touch pressed candidate fill. |
| `page_indicator` | Page fraction or chevrons. |
| `separator` | Composition and status separators. |
| `status_icon` | Normal generic status glyph. |
| `status_hover` | Status button hover fill. |
| `status_pressed` | Status button pressed fill. |
| `status_disabled` | Disabled glyph/fill. |

Readable foreground colors require alpha of at least 128. Window background
alpha requires at least 160. These checks prevent an otherwise valid package
from making the candidate list accidentally invisible.

## Typography

`fonts.families` is an ordered fallback list of one to eight installed family
names. The reference order is `Segoe UI`, `Microsoft YaHei UI`, then generic
`sans-serif`. Missing families are skipped without failing theme activation.

The five size tokens are DIPs and accept 8 through 48:

- `candidate_size`
- `composition_size`
- `annotation_size`
- `index_size`
- `status_size`

`candidate_weight` and `highlight_weight` accept 100 through 900. The
`number_style` values are `plain` and `compact`; neither substitutes decorative
Unicode digits that might be missing from the selected font.

## Geometry

All geometry is expressed in DIPs at `dpi.reference`. Insets always use the
explicit `left`, `top`, `right`, and `bottom` fields.

| Metric | Meaning |
| --- | --- |
| `min_width`, `max_width`, `max_height` | Bounds before work-area avoidance. |
| `border_width` | Outer stroke width. |
| `corner_radius` | Candidate surface radius. |
| `highlight_corner_radius` | Selected/hover candidate radius. |
| `row_height` | Stable candidate row track. |
| `candidate_gap` | Gap between candidate rows/items. |
| `column_gap` | Gap between number, text, annotation, and controls. |
| `composition_gap` | Gap from composition to candidates. |
| `window_padding` | Content inset from candidate surface edge. |
| `candidate_padding` | Inset inside each candidate hit target. |
| `composition_padding` | Inset around composition text. |

`min_width` may not exceed `max_width`. Stable row tracks and explicit padding
must be used by the renderer so hover, selection, or a changing index label
cannot resize the popup.

## Layout

`layout.default` must appear exactly once in `layout.supported`. Supported
layout names are:

| Value | Intended arrangement |
| --- | --- |
| `horizontal` | Candidates flow left to right. |
| `vertical` | One candidate per row. |
| `single_line` | Composition and candidates share a single clipped line. |
| `compact` | Reduced auxiliary text and tighter visible set. |
| `expanded` | Full annotations and available candidates. |

`candidate_count` accepts 1 through 10. `show_composition` controls the
composition row. `horizontal_wrap` permits horizontal candidates to wrap only
when the renderer can preserve stable popup placement. `page_indicator` is
`none`, `chevrons`, or `fraction`. Generic chevrons are drawn by ZRinput.

Long composition text is never truncated in the input model. The view scrolls
to keep the insertion caret visible; when unfocused, it may elide the remote
side of the display string. Layout tokens affect only presentation.

## Shadow and animation

Each light/dark variant has independent `shadow` tokens: `enabled`, `color`,
`opacity`, `blur_radius`, `offset_x`, and `offset_y`. Blur is limited to 64 DIPs
and offsets to plus or minus 32 DIPs. Work-area positioning includes the visible
shadow extent, but caret anchoring uses the non-shadow candidate bounds.

Animation durations are integral milliseconds from 0 through 1000:

- `show_ms`: first visible frame to fully-present popup.
- `hide_ms`: dismissal duration after the TSF state is closed.
- `update_ms`: candidate content/selection transition.
- `hover_ms`: pointer and touch feedback transition.

`easing` is cubic Bezier `[x1, y1, x2, y2]`; x values remain in `[0, 1]` and y
values in `[-2, 2]`. `reduce_motion_scale` multiplies durations when Windows
requests reduced motion. A value of zero gives immediate state changes. The
renderer must also disable animation after device loss or while a popup is
repositioned across monitors; geometry correctness has priority over a
transition.

## DPI, monitors, and work area

`dpi.reference` is the coordinate basis, normally 96. `dpi.scales` lists the
calibration and regression points and must be unique, ascending, within 0.75
through 4.0, and include 1.0. The reference theme explicitly covers 100%, 125%,
150%, 175%, and 200%.

When `snap_to_pixels` is true, strokes and final popup bounds are snapped in
physical pixels after scaling. Text layout remains in DIPs and is rasterized at
the destination monitor DPI. `work_area_margin` is the minimum distance from
the destination monitor's work area. Placement is recalculated from the TSF
text extent whenever the caret, monitor, DPI, taskbar edge, or candidate size
changes. A stale placement calculation must not overwrite a newer composition
version.

`hdr.adapt_to_display` enables conversion from the theme's sRGB tokens into the
destination advanced-color surface. `hdr.sdr_reference_white_nits` supplies the
SDR white level used by that conversion and accepts 80 through 400 nits.
`hdr.clamp_translucent_layers` keeps translucent popup layers at SDR reference
white so a candidate window does not become painfully bright on an HDR panel.
If display metadata is unavailable, rendering uses the ordinary sRGB path.

## Status bar

`status_bar` controls `visible`, fixed `button_size`, inter-button `gap`, outer
`padding`, and `auto_hide_ms`. Each button has a portable `id`, generic `glyph`
semantic, `visible`, and unique `order`. The reference uses only the semantics
`language`, `punctuation`, `character-width`, `script`, and `settings`; these
are rendered from ZRinput-owned vector paths in the application binary, not
from Microsoft resources.

Theme packages may map an icon token to a safe bitmap path through
`assets.icons`, but product-defined status commands do not come from the theme.
A theme can change presentation and order, never command behavior.

## Bitmap and nine-slice resources

Every resource path is relative, uses `/`, starts with `assets/`, contains only
ASCII letters, digits, `_`, `-`, `.`, and `/`, and ends in lowercase `.png` or
`.webp`. Empty, `.` and `..` components, backslashes, drive prefixes, colons,
percent-encoded components, trailing-dot components, Windows device names,
absolute paths, and all other extensions are rejected.

`assets.nine_slice` optionally supplies a bitmap `path` and non-negative
`slice` insets up to 512 source pixels. The package loader verifies that the
insets fit the decoded bitmap and that scaling cannot create a negative center
region. A missing or rejected resource causes whole-theme activation failure;
partial activation is prohibited.

The current manifest hot-loader accepts data-only themes without external
bitmaps. Until the bounded package loader and image decoder complete their
checks, any manifest that references `icons` or `nine_slice` is rejected rather
than partially activated.

## Safe fallback and hot loading

`SafeDefaultTheme()` is built from typed constants in the binary, references no
files, and validates independently of installed packages. Loading follows one
transaction:

1. Read at most 512 KiB into a private buffer.
2. Strictly parse JSON under depth, string, number, container, and value limits.
3. Decode only known v1 fields into a new typed `Theme`.
4. Validate ranges, cross-field invariants, identifiers, and resource paths.
5. Decode bounded resources off the TSF and UI critical paths.
6. Construct the renderer resources for the new immutable snapshot.
7. Atomically publish the snapshot and invalidate the candidate window once.

File watching should debounce changes for at least 100 ms and require a stable
size/last-write pair before step 1. Failure at any step keeps the previous
active snapshot. If there is no previous valid snapshot, ZRinput uses the safe
default. Diagnostics include the manifest path and validation message but never
include composition text.

## Clean-room reference measurement

The reference skin is maintained through measurement, not memory. A calibration
run uses a stock Windows 11 installation owned for testing and a fixed,
non-personal corpus. No resource is extracted from the system input method.
Only rendered screenshots and manually observable timing are measured.

For each Windows build under test:

1. Reset display calibration, font smoothing, text scale, and accessibility
   settings to the recorded test profile.
2. Capture light and dark modes at 100%, 125%, 150%, 175%, and 200% on at least
   two monitors with different DPI. Record OS build, GPU, color profile, taskbar
   edge, and physical pixel bounds.
3. Use fixed composition/candidate fixtures covering short, long, annotated,
   paged, selected, hovered, pressed, and status-bar states.
4. Capture at least five frames per static state. Measure the median outer and
   inner bounds, baseline positions, row tracks, gaps, radii, border alpha, and
   shadow falloff. Convert physical pixels back to DIPs for the token record.
5. Record show, hide, update, and hover frame timestamps at 60 Hz or faster and
   fit the nearest bounded cubic Bezier curve.
6. Update `theme.json` only together with the dated measurement record and new
   visual baselines. Never copy a captured icon or bitmap into the package.

Automated comparison uses deterministic ZRinput rendering with identical text
and font availability. The initial acceptance thresholds are explicit:

| Measurement | Threshold |
| --- | ---: |
| Non-shadow geometry at 100%-150% | at most 1 physical pixel error |
| Non-shadow geometry at 175%-200% | at most 2 physical pixels error |
| Structural similarity inside popup bounds | SSIM at least 0.985 |
| Mean CIEDE2000 color difference | at most 2.0 |
| 95th percentile CIEDE2000 | at most 5.0 |
| Pixels with CIEDE2000 above 10 | at most 0.25% |
| Shadow alpha RMSE | at most 0.03 |
| Animation landmark timing | within one captured frame |

Dynamic text antialiasing pixels and the independently drawn generic icons are
reported separately and cannot be masked to hide geometry errors. Any baseline
update stores the before/after images, numeric diff, measurement profile, and
rationale. Passing these thresholds establishes repeatability, not affiliation
with or byte-for-byte identity to a Microsoft product.
