# User Memory

This document defines the implemented User Memory contract at the current
portable-core checkpoint. It does not imply that production TSF storage wiring,
settings UI, sync, or user-facing dictionary management is complete.

## Runtime data flow and privacy gates

`RecordAccepted`, `RecordRejected`, and `RecordDeleted` validate field limits,
copy a bounded event, and attempt a non-blocking lock on the bounded queue. They
perform no disk I/O. A successful call returns `kQueued`, which means only that
the event was accepted into the in-memory queue. It does not mean that the event
has reached the journal or snapshot.

The background worker drains at most `worker_batch_size` events per batch. With
persistent storage enabled, it durably appends CRC32-framed journal records
before updating the model. It then publishes an atomic immutable read view used
through the decoder's `PersonalizationView` interface. Queue contention returns
`kBusy`; a full queue returns `kQueueFull`; neither condition blocks the key
path.

Recording is rejected when global learning is disabled, privacy mode is active,
the caller marks the context sensitive, or the normalized application identity
is excluded. Privacy mode, a sensitive direct `View` query, and an excluded
application also hide existing personalization from reads. Disabling learning
stops new records but does not by itself hide previously learned data.

Password-field and incognito detection are not automatic in this checkpoint.
The integrating TSF layer must identify those contexts, mark record/direct-view
contexts `sensitive`, and enable privacy mode or omit `PersonalizationView` from
decoder requests while the sensitive context is active.

## Flush and storage location

A successful `Flush()` means every event queued before that flush barrier was
processed and, when storage is enabled, a durable snapshot was written.
`Flush()` waits and performs persistence work indirectly, so it must never run
under a TSF edit lock or on the keystroke path. Orderly destruction invokes the
same flush behavior.

An empty `UserMemoryOptions::storage_directory` selects in-memory-only mode.
No default production directory is wired yet. When a directory is supplied,
the implementation uses:

| File | Purpose |
| --- | --- |
| `user-memory.snapshot` | Current compact model snapshot |
| `user-memory.snapshot.bak` | Previous valid snapshot for crash recovery |
| `user-memory.journal` | Durable events not fully retired from recovery history |
| `user-memory.lock` | Windows exclusive-writer lock |

The internal `.bak` file is part of crash recovery. It is not a user backup,
export, or restore facility.

## Storage format and recovery

The implemented storage format is version 1. Snapshot frames use the `ZRMS`
magic and journal frames use `ZRMJ`. Each frame contains a version, payload
length, and CRC32 over the payload. Journal events carry strictly increasing,
nonzero 64-bit sequence numbers.

Writes use `FlushFileBuffers` on Windows. Snapshot and compacted-journal files
are first written to a temporary file and then replaced with write-through
rename semantics. A snapshot is created on successful `Flush()`, orderly
shutdown, or journal-budget compaction; there is no timer-based snapshot.

Startup prefers a valid current snapshot, falls back to the previous valid
snapshot when necessary, and replays only valid complete journal frames newer
than the loaded snapshot. Replay requires monotonically increasing sequences.
An incomplete, corrupt, or non-monotonic journal tail is ignored and truncated
to the last valid frame. Resource limits are enforced both while learning and
while loading untrusted or damaged storage. A journal containing more valid
records than the configured record budget is rejected instead of being
silently truncated. If an existing journal cannot be read within its configured
budget, or a damaged tail cannot be safely truncated, construction fails and
the writer never starts; new records are not appended behind unknown data.

Windows storage has single-writer semantics. Opening the same directory from a
second `UserMemory` instance fails while the first writer owns the lock. The
future shared decoder service must own the single persistent instance; each TSF
host process must not instantiate its own writer for the same store. Constructing
the persistent instance performs directory, lock, and recovery I/O and therefore
must also occur outside TSF edit locks.

## Scoring

All exposed score features are clamped to `[0, 1]`. For count `c`, saturation
constant `s`, event time `t`, query time `now`, and half-life `H`:

```text
decay(t, now, H) = 2 ^ (-max(0, now - t) / H)
saturate(c, s)   = clamp(log1p(c) / log1p(s), 0, 1)

recency       = decay(lastAccepted, now, H)
userFrequency = saturate(acceptedCount, acceptedSaturation) * recency
context       = saturate(contextCount, contextSaturation)
                * decay(contextLastUsed, now, H)
application   = saturate(applicationCount, applicationSaturation)
                * decay(applicationLastUsed, now, H)

weightedNegative = rejectedCount + deletePenalty * deletedCount
negativeFeedback = saturate(weightedNegative, negativeSaturation)
                   * decay(lastNegative, now, H)
```

A deletion creates a hard suppression tombstone when its timestamp is at least
the most recent accepted timestamp. Suppressed candidates are removed by the
decoder rather than merely ranked lower. A later accepted event at the same or
a newer timestamp clears the tombstone; historical negative feedback continues
to decay normally.

## Default limits

Defaults are centralized in `UserMemoryOptions`:

| Option | Default |
| --- | ---: |
| Queue capacity | 2,048 events |
| Worker batch size | 128 events |
| Candidate entries | 200,000 |
| Context values per candidate | 32 |
| Application values per candidate | 32 |
| Total context/application values | 1,000,000 |
| Total stored string bytes | 64 MiB |
| Journal records | 1,000,000 |
| Reading field | 256 bytes |
| Candidate text field | 1,024 bytes |
| Context field | 512 bytes |
| Application field | 512 bytes |
| Snapshot and journal file budget | 128 MiB each |
| Half-life | 30 days |
| Accepted-count saturation | 32 |
| Context-count saturation | 8 |
| Application-count saturation | 16 |
| Negative-count saturation | 8 |
| Delete penalty | 4 |

The per-candidate feature limit is shared as one limit applied independently to
the context map and application map. When entry or aggregate feature/string
budgets are exceeded, the least recently active candidate is evicted. Within a
candidate, the least recently used context or application value is evicted.
The exact encoded snapshot budget is checked as:

```text
28 + 64 * candidateEntries + 20 * featureValues + storedStringBytes
```

Configuration validation also proves that one maximum-size journal batch fits
the file budget. A journal event uses 56 fixed bytes plus its reading, candidate
text, context, and application bytes.

## Durability and failure semantics

- `kQueued` acknowledges queue ownership only. Call `Flush()` from a safe
  background or shutdown path when an explicit durability barrier is required.
- In persistent mode, a batch is published only after its journal append is
  flushed. A journal or snapshot failure is reflected by `Flush() == false`
  and the persistence diagnostics.
- Queue contention or exhaustion is explicit through `kBusy` and `kQueueFull`;
  events are not silently reported as queued.
- Reads are nonthrowing and use the most recently published immutable view.
- Storage files and loaded values are bounded before allocation or replay.
- Journal replay and live learning use the same per-event deterministic
  eviction boundary, so worker batch grouping cannot change the restored model.

## Not yet implemented

- Automatic password-field or incognito-window detection.
- Production selection and permission hardening of the storage directory.
- Persistence of learning, privacy, and per-application policy settings.
- User dictionary import, export, clear, backup, and restore APIs.
- Encrypted backup envelopes or multi-device sync.
- Migration from future or otherwise unsupported storage versions; such files
  currently fail validation and may trigger backup recovery.
- Verified disk-full and forced-termination recovery behavior.
- One-hour and eight-hour endurance evidence under concurrent TSF workloads.
- Integration into the shared decoder service, candidate UI, settings app, and
  installer.
