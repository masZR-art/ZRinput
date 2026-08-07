# Development status

Last updated: 2026-08-07

Branch: `codex/zrinput-cleanroom`

## Current checkpoint

Completed: transactional native TSF adapter skeleton checkpoint.

## Completed

- Created an empty orphan worktree connected to the requested GitHub remote.
- Recorded architecture decisions, clean-room boundary, limits, and MVP scope.
- Added x64 and ARM64 CMake presets with strict C++20 warnings.
- Implemented a dynamic UTF-16 composition buffer with selection replacement,
  code-point-safe cursor movement, Home/End, forward/backward deletion,
  Ctrl+Backspace semantics, centralized limits, and monotonic versions.
- Added a dependency-free test harness and eight deterministic composition
  tests.
- Added a bounded, multi-path pinyin parser that keeps raw UTF-16 input intact,
  normalizes only its query view, accepts repeated separators, represents
  abbreviations/incomplete syllables explicitly, applies bounded duplicate and
  transposition corrections, and retains an unparsed tail.
- Stable-prefix selection considers only complete syllable boundaries and
  refuses unsafe separator-only splits.
- Added CRC32-protected, versioned binary dictionary packages with byte/entry/
  field budgets, v1 migration, atomic replacement, and corruption rejection.
- Added system/domain/user/session dictionary layers that publish immutable
  exact, compact-prefix, and initial indexes by generation.
- Added version-preserving decode requests and candidates that retain raw
  unparsed tails. Exact full pinyin, simplified initials, and incomplete
  prefixes share one explicit scoring path.
- Documented and implemented named ranking weights with per-candidate raw and
  weighted score breakdowns, logarithmic static frequency, time decay,
  application/context hooks, correction/completion costs, and negative feedback.
- Added a reproducible maintenance pipeline using freshly downloaded,
  SHA-256-pinned MIT-licensed sources. The generated system package has 348,918
  entries and 411 syllables; an independent second generation was byte-identical.
- Added strict TSV-to-ZRDICT compiler and query tools. Replaced the memory-heavy
  per-prefix hash expansion with a sorted compact-reading range index.
- Added bounded beam sentence decoding, so continuous input can be composed
  from independently indexed words without an exact long-phrase row.
- Added one permanent replace-latest candidate worker with cooperative
  cancellation, internal submission IDs, composition-version gates, lock-free
  publication callbacks, bounded pending state, callback re-entry, and
  idempotent shutdown.
- Added safe prefix removal with selection translation and surrogate-boundary
  validation, plus deterministic limit cases and seeded composition fuzzing.
- Added a real-dictionary long-composition benchmark for 32, 64, 128, 256, and
  1024 UTF-16 units with reported P50/P95/P99 edit, parse, and decode latency.
- Added a bounded, local-only next-word prediction service with named weights,
  longest-context backoff, time decay, per-application isolation, session
  learning, explicit score terms, and collision-safe context keys.
- Added a strict, bounded UTF-8 JSON parser and a typed v1 theme model whose
  unknown fields, invalid values, unsafe resource paths, and unsupported
  versions fall back to an in-binary safe default.
- Added the clean-room Windows 11 Reference light/dark token set, Draft 2020-12
  schema, authoring/security specification, and explicit visual-regression
  thresholds and reference-measurement procedure.
- Added atomic immutable theme snapshots, stable-file hot loading, exact-content
  periodic rescans, last-valid rollback, queryable errors, interruptible
  shutdown, and concurrent idempotent stopping. External bitmap references are
  rejected until the bounded package loader validates and decodes every asset.
- Added accepted, rejected, and deleted feedback with half-life decay,
  context/application affinity, and decoder-facing `PersonalizationView`
  integration.
- Added hard suppression after deletion. A later accepted event rehabilitates
  the candidate, including when it follows deletion in the same wall-clock
  second.
- Added learning, privacy, caller-supplied sensitive-context, and per-application
  policy gates for recording and personalization reads.
- Added a bounded try-lock queue, background batching, and atomic publication of
  immutable read views. The key path performs no persistence I/O.
- Added v1 CRC32-framed snapshots and journals, monotonic record sequences,
  `FlushFileBuffers` durability, atomic replacement, and a Windows
  single-writer lock.
- Added recovery from the previous valid snapshot and truncation of corrupt or
  incomplete journal tails. Runtime and load-time entry, feature, string,
  journal-record, field, and storage budgets are enforced.
- Added a production `ZRinputTsf.dll` target and shared `zrinput_tsf_impl`
  library so the DLL and native tests execute the same class-factory,
  registration, and text-service sources.
- Added COM unload accounting plus TSF activation, key, composition,
  document/context focus, and teardown lifecycles. Sink unadvise failures retain
  state for a complete retry instead of silently leaking an untracked sink.
- Added a single-flight edit-session queue with update coalescing, ordered
  commit/split/cancel barriers, version and context-epoch gates, bounded pending
  state, current/stale failure budgets, transactional key rollback, and
  fail-open behavior after a host repeatedly refuses edit sessions.
- Queue admission distinguishes rejection before mutation, accepted asynchronous
  work, and synchronous completion. Barrier requests persist explicit
  before-write, composition-started/await-write, text-written/await-end,
  ended/await-remainder, before-clear, and cleared/await-end phases so retries
  never replay an irreversible mutation.
- Added external-composition termination invalidation, so already accepted
  delayed barrier sessions cannot rewrite or end a composition destroyed by the
  host. Split-dependent remainder updates are invalidated together.
- Added a post-`SetText` transaction boundary. Selection placement is retried
  from a fresh range; persistent failure commits the already-written text, and
  a host that also refuses termination receives ordinary keys directly with no
  stale internal snapshot left to overwrite its text. Enter and Escape remain
  deterministic recovery paths for that host-owned composition.
- Added a pre-`SetText` composition-start boundary. If the first write fails,
  the adapter ends the new empty composition before returning the key; if the
  host also refuses that cleanup, the accepted request retains its snapshot and
  retries without exposing the key. Focus loss converts that state to a bounded
  cancel, and every successful end releases the intentional-end COM reference.
- Added TSF-level 256-unit soft-limit behavior. A letter-delimited apostrophe
  may split the raw composition; no boundary, separator-only input, and repeated
  separators reject growth without preventing Backspace or Escape recovery.
- Added fail-closed password/private/PIN InputScope checks. Every queued edit
  closes the learning gate until its current edit session successfully proves a
  nonsensitive scope.
- Added preflighted COM/TSF registration transactions, immediate pre-write COM
  snapshots, exact type/byte restoration, mutation ownership, reverse rollback,
  owned uninstall, a per-user cross-process mutex, and rollback-failure behavior
  that keeps the DLL path reachable for repair.
- Added a fixed export definition, version resource, explicit Windows API
  baseline and system libraries, static MSVC runtime, CFG, ASLR, NX, and
  high-entropy VA checks.

## Verification

Verified on Windows 11 x64 with MSVC 19.40.33811 and SDK 10.0.22621.0:

```text
clean x64 configure                         PASS
clean Release build                         PASS (strict warnings as errors)
Release CTest                               PASS (8/8)
Debug build                                 PASS
Debug functional/iterator CTest             PASS (7/7; Release memory gate excluded)
MSVC Native Code Analysis                   PASS (zero warnings)
```

The harness-backed executables report 65 portable-core, 23 theme, 76 TSF
lifecycle/COM, and 26 registration-transaction cases: 190/190 deterministic
cases in total. The complete TSF executable passed 100 consecutive independent
Release processes (7,600/7,600 case executions), and the registration executable
passed 100 consecutive Release runs. MSVC native code analysis covered common,
core, and every TSF production source with warnings treated as errors and
reported zero warnings.
The Debug functional run included all harness cases, fuzzing, and PE checks.
The real-dictionary memory gate is Release-only; a diagnostic Debug run reached
287.832 MiB and correctly exceeded the 140 MiB Release budget.

The cases include a 1024 UTF-16-unit internal buffer, non-mutating soft-limit
notification, hard-limit recovery through Backspace, arbitrary selection
replacement, word deletion, and surrogate-pair-safe movement/deletion.
Parser cases cover ambiguous segmentation, mixed case/umlaut normalization,
repeated separators, invalid tails, abbreviations, corrections, stable splits,
and 256 separator characters.
Dictionary/decoder cases cover package round-trip, migration, CRC corruption,
immutable snapshot replacement, every ranking term, half-life decay, exact/
initial/incomplete lookup, raw-tail retention, and personalization ordering.
User Memory cases cover privacy and learning policies, score decay, deletion
and rehabilitation, resource eviction, bounded-queue barriers, durable shutdown,
journal-budget compaction, live/replay eviction equivalence, single-writer
ownership, corrupt-tail recovery, refusal of unreadable or untruncatable
journals, and fallback to the previous snapshot.

Real-package checks on 2026-08-07:

```text
system.zrdict                         11,034,733 bytes
entries / syllables                   348,918 / 411
fresh-generation package hash match  PASS
xianzai / zhongguo / shurufa / nihao PASS (real Chinese candidates)
package read / index construction     45.275 / 305.938 ms
1,000-query P50 / P95 / P99           0.088 / 0.295 / 0.462 ms
integration peak working set          93.113 MiB
```

`zrinput_lexicon_integration_tests` enforces 30 ms query P95, 2.5 s cold
initialization, and 140 MiB peak working-set gates while checking five known
Chinese results against the actual 348,918-entry package.

The current portable core has 65 deterministic cases and passed 100 consecutive
runs under MSVC. The separate composition fuzzer ran 12 seeds x 10,000 events
with no invariant failure; it observed 347 limit rejections and 3 stable prefix
splits. One-hour and eight-hour endurance results are not claimed yet.

Theme parsing and management have 23 deterministic cases. After the final
security review, both theme executables passed 30 consecutive paired runs.
Draft 2020-12 schema validation passed for the bundled theme and for rejection
fixtures covering invalid identifiers, Windows device names, trailing-dot
components, and the 64-resource aggregate boundary.

`cmake --preset windows-arm64` was attempted and failed during compiler
detection because this machine does not have the Visual Studio ARM64 C++ tools
or libraries installed. The preset remains ready for a machine with
`Microsoft.VisualStudio.Component.VC.Tools.ARM64`.

Native TSF evidence from the clean Release build:

```text
ZRinputTsf.dll                         217,600 bytes
SHA-256                                7FD7645D17D5297A128C410D069F19786F3BB526D5AC6A81AD8C57225D32B930
machine / exports                      AMD64 / exact four COM exports
CFG / ASLR / NX / high-entropy VA      PASS
dynamic VC/UCRT dependency             none
file/product version                   0.2.0.0
Authenticode                           NotSigned
known HKCU/HKLM COM and TIP residue     none
```

`cmake --install` copied the same-hash DLL to the staging prefix. This tests
the install manifest only; it does not register a TIP and is not a supported
installer.

## Next resume point

1. Define and implement the shared decoder-service ownership and request
   boundary without loading the full dictionary in each host process.
2. Connect versioned full-pinyin decode results to TSF composition state.
3. Add candidate selection controls and the first nonactivating native
   candidate surface before any claim of usable Chinese input.

## Known risks

- The Visual Studio ARM64 component is absent, so ARM64 cannot be built on this
  machine until that optional workload is installed.
- End-to-end TSF input requires registration and interactive host-app tests;
  portable tests alone are insufficient.
- The DLL is not signed. It must not be distributed as a production text
  service until signing and package trust are in place.
- The adapter currently composes and commits raw lowercase pinyin only. It does
  not call the decoder, display candidates, consume Space/numeric/page keys, or
  output Chinese text.
- If an old context refuses a confirmed commit/split/cancel edit session twice,
  the adapter retires that operation so the new foreground application remains
  usable. This explicit degradation and the case where a host accepts an async
  session but never invokes it require real-host logging and validation.
- Registration tests use a fake backend. The production RegisterServer and
  UnregisterServer paths have not been executed; install/repair/rollback/
  uninstall remain isolated-VM work.
- Password fields and incognito windows are not detected automatically;
  record/direct-view suppression depends on the caller setting
  `PersonalizationContext::sensitive`, while decoder callers must enable
  privacy mode or omit the personalization view.
- No production storage-directory wiring exists, and learning/privacy/
  per-application policy settings are not persisted.
- User Memory has no import, export, clear, user-facing backup/restore, or
  encrypted-sync API. Its internal `.bak` file is only crash recovery.
- The User Memory storage format is v1. Files with unsupported versions fail
  validation and may trigger backup recovery; no cross-version migration is
  implemented yet.
- Disk-full, forced-termination, one-hour, and eight-hour endurance evidence is
  still missing.
- Persistent `Flush()` and orderly destruction have no bounded I/O timeout;
  production ownership therefore remains in the planned isolated service, not
  an application-host TSF edit path.
- The native candidate UI, settings application, supported installer, shared
  decoder service, and registered end-to-end TSF input path are not yet
  available.
- Theme ZIP extraction, actual PNG/WebP decoding, package installation, and
  reference screenshot baselines are not implemented at this checkpoint.
