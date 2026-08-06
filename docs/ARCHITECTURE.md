# Architecture decisions

## Boundaries

ZRinput uses a ports-and-adapters layout. The portable C++20 core has no Win32
types. Windows-specific code translates TSF edit sessions and UI events into
immutable core requests. The decoder never owns a window, COM pointer, or
thread-affine object.

```text
Host application
  -> TSF Adapter
     -> Composition Engine
     -> local IPC client -> shared Decoder Service
        -> cancellable Candidate Pipeline
        -> Pinyin Parser -> Decoder -> Dictionary Service
                         -> Ranking -> Prediction Service
                         -> User Memory (read snapshot)
     -> Candidate UI -> Theme Engine

Settings App -> Config Store -> atomic snapshot + change notification
             -> Theme Package Service
             -> Backup/Sync Adapter (local implementation in MVP)

User Memory -> bounded queue -> background journal/snapshot writer
Telemetry Adapter -> disabled no-op by default
```

The full dictionary is owned once by the per-user decoder service. The
in-process TSF DLL has a small emergency fallback only. This avoids multiplying
the measured dictionary working set across every text-hosting application and
keeps dictionary/database failure outside host process boundaries.

## ADR-001: TSF, not IMM32

The input service is an in-process COM server implementing TSF text input,
keystroke, composition, thread-manager, and text-edit lifecycle interfaces.
IMM32 is not used as an input architecture. Candidate windows are nonactivating
top-level Win32 windows anchored through `ITfContextView::GetTextExt`.

## ADR-002: native candidate surface

The candidate surface uses Direct2D and DirectWrite. It has no packaged-app
dependency, can be loaded from x64 and ARM64 host processes, and remains under
the TSF adapter's positioning control. The settings process is separate so a
settings failure cannot affect a host application.

## ADR-003: immutable, versioned decode requests

Every edit increments a 64-bit composition version. Decode requests contain a
copy of raw input, selection, context, application identity, and settings
generation. A single replace-latest worker executes requests. Results with a
non-current composition version are discarded before touching UI state.

## ADR-004: bounded composition, no fixed buffers

Composition text is a dynamic UTF-16 string. Default limits are centralized:

| Limit | Default | Purpose |
| --- | ---: | --- |
| UI display window | 96 code units | Scroll/ellipsis only; never truncates state |
| Active soft limit | 256 code units | Trigger stable-prefix split |
| Parser budget | 1024 code units | Supported internal analysis size |
| Hard safety limit | 4096 code units | Reject growth while preserving recovery keys |

At the active limit the TSF adapter first asks the parser for a stable prefix.
It commits that prefix and retains the unresolved tail. If no safe split exists,
the insertion is rejected but navigation, deletion, cancellation, commit, and
candidate selection continue to work.

## ADR-005: local memory with journaled writes

Personal learning is local by default. Key handling updates an in-memory model
and enqueues a bounded write record. A background writer appends checksummed
records and periodically replaces a checksummed snapshot atomically. Startup
replays only valid complete records and retains the last known-good snapshot.
No file, database, or network operation runs under a TSF edit lock.

## ADR-006: data-only themes

Theme packages are versioned JSON plus bounded raster assets. Packages cannot
contain executable code. Installation canonicalizes every path, rejects links
and traversal, enforces per-file and aggregate size budgets, validates decoded
image dimensions, and writes through a staging directory. Invalid active
themes fall back to the embedded safe theme.

## ADR-007: no input telemetry

The telemetry port accepts only aggregate timing/error counters and is wired to
a disabled no-op adapter by default. Raw composition, candidates, committed
text, and personal-memory records are never telemetry fields.
