# Test-release scope

## Included in the first installable checkpoint

- x64 and ARM64 C++20 builds from one source tree.
- TSF registration, activation, focus/composition lifecycle, and secure-mode
  learning suppression.
- Full-pinyin input with tolerant segmentation, abbreviated prefixes, common
  corrections, sentence decoding, Chinese punctuation, paging, numeric choice,
  cursor editing, cancellation, and raw-input commit.
- Layered system/domain/user/session dictionaries loaded offline.
- Explicit weighted ranking, time-decayed learning, negative feedback,
  application context, local export/import/clear, and asynchronous persistence.
- Direct2D/DirectWrite horizontal and vertical candidate layouts.
- Versioned themes, safe install/uninstall/preview, fallback, and hot reload.
- A native settings application that opens directly to editable settings.
- Deterministic unit, integration, fuzz, benchmark, and screenshot tests.
- PowerShell install, update, repair, and uninstall entry points plus ZIP
  artifacts.

## Deliberately deferred

- Speech recognition and cloud prediction implementations. Their ports exist,
  but network use remains disabled and no input leaves the computer.
- Production encrypted multi-device sync. The adapter and encrypted backup
  envelope are present; server/account integration is not.
- Handwriting/stroke decoder and a production double-pinyin scheme editor.
- Microsoft-owned icons, sounds, private measurements, dictionaries, or code.
- 32-bit host-process support in the first package. Windows 11 x64 requires a
  separate x86 proxy DLL for 32-bit applications; it is tracked after x64 and
  ARM64 correctness.
- A claim of pixel identity with Microsoft Pinyin. Visual regression is against
  clean-room reference captures and reports numeric thresholds.

