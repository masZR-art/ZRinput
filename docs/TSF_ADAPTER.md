# TSF adapter checkpoint

This document defines the clean-room native adapter checkpoint and prevents it
from being confused with the first usable Chinese input-method release.

## Implemented boundary

`ZRinputTsf.dll` is a C++20 in-process COM server built on Text Services
Framework interfaces. The production DLL and native tests link the same
`zrinput_tsf_impl` sources. The adapter currently provides:

- COM class-factory and unload accounting;
- text-service activation/deactivation, key-event, composition, focus, and
  document/context event sinks;
- serialized edit-session dispatch with update coalescing and ordered
  commit/split/cancel barriers, bounded to 64 normal requests plus eight
  teardown requests;
- queue results that distinguish rejection before any host mutation, accepted
  asynchronous work, and synchronous completion, so a caller never infers
  rollback eligibility from an `HRESULT` alone;
- explicit retry phases for before-write, composition-started/await-write,
  text-written/await-end, ended/await-remainder, before-clear, and
  cleared/await-end operations;
- generation checks that reject stale updates;
- transactional rollback of a key whose request is blocked by a failed
  predecessor, including the case where a later request is accepted
  asynchronously in the same dispatch pass;
- three-failure backpressure for a current context and two-failure retirement
  for a stale context, so a dead text control cannot consume keys indefinitely
  or block a newly focused application;
- invalidation of delayed commit, split, and cancel sessions when the host
  externally terminates their captured composition;
- a post-`SetText` transaction boundary: selection placement retries from a
  fresh range, persistent selection failure commits the text already accepted
  by the host, and failure of that commit switches to a host-owned composition
  without retaining a stale internal text snapshot;
- dynamic composition state using the portable UTF-16 buffer;
- 256-code-unit active-limit handling with a conservative explicit-apostrophe
  split whose immediate neighbors are lowercase pinyin letters, otherwise a
  recoverable insertion rejection; separator-only and repeated-separator
  boundaries are never committed automatically;
- fail-closed sensitive InputScope handling for password, private, password/PIN
  variants, missing properties, and failed queries;
- transactional COM/TSF registration helpers with exact registry-value
  snapshots, owned uninstall, fail-safe rollback behavior, and a per-user
  cross-process registration mutex;
- a fixed four-function export table, version resource, static MSVC runtime,
  Windows 10/11 API baseline, ASLR, NX, high-entropy VA, and Control Flow Guard.

The active-limit split does not yet call the pinyin parser. An apostrophe with
letter neighbors is treated as a user-supplied syntactic boundary, not proof
that either side is valid pinyin. Parser-derived stable-prefix splitting belongs
to the decoder integration milestone.

## Current keyboard behavior

While a raw composition is active, the skeleton accepts `A`-`Z`, an unshifted
apostrophe, Backspace, Delete, Left/Right, Home/End, Escape, and Enter. Letters
are normalized to lowercase. Enter commits the raw composition and Escape
cancels it.

Space commit, numeric candidate selection, candidate paging, Chinese
punctuation, mode switching, decoder output, prediction, and candidate UI are
not connected. Enabling this checkpoint would therefore commit `nihao`, not
`\u4f60\u597d`. It must not be distributed as a usable Chinese IME.

## Registration transaction

Registration performs a full preflight snapshot before mutation. Rollback is
reverse ordered. If TSF cleanup fails, the COM path is deliberately retained
so a profile cannot point at a removed DLL and an installer can keep the DLL
for repair or uninstall retry. Unregistration removes state only when the COM
registration is owned by the exact module path.

The automated registration tests use a deterministic fake backend and never
write production CLSID or TIP state. `DllRegisterServer` and
`DllUnregisterServer` have not been executed on the development workstation.
Real registration, concurrent install/repair, upgrade interruption, and
rollback must be verified in a disposable Windows 11 VM before packaging.
The mutex coordinates only ZRinput builds that use the same lock contract; it
cannot serialize an old build or a third party that writes the same identifiers
directly.

## Failure containment

A current context may retry a failed edit request three times. Once that budget
is exhausted, `OnTestKeyDown` and `OnKeyDown` stop consuming ordinary keys;
Escape and shortening Backspace/Delete operations remain recovery paths. A key
queued behind a failing barrier is removed transactionally and returned to the
host rather than hidden in the internal buffer.

After `ITfRange::SetText` succeeds, a later range or selection failure cannot be
reported as an ordinary failed key because the host has already accepted the
text. The adapter retries the complete selection operation from a fresh range.
If both attempts fail, it ends the composition to preserve the written text and
clears the internal buffer. If the host also refuses `EndComposition`, ordinary
keys fail open to that host-owned composition; Enter retries ending its actual
text and Escape clears its actual range before ending. The adapter does not
replay an obsolete buffer over text entered by the host during this state.
Likewise, commit, split, and cancel retries resume at their recorded phase:
written text is not written a second time, a completed end is not replayed, and
a cleared range is not cleared again.

Starting a TSF composition is also a host mutation even before its first text
write. If that first write fails, the adapter first tries to end the still-empty
composition. A successful end makes synchronous key rollback safe. If the host
also refuses the end, the request remains in the explicit
composition-started/await-write phase and the key remains consumed; later edits
retry the retained snapshot. Focus loss converts a stale update in this phase
to bounded cancellation. Successful end paths explicitly release the
intentional-end reference even when a host does not synchronously deliver the
termination callback.

After focus leaves a context, a confirmed commit/split/cancel barrier gets two
bounded attempts. If the old context refuses both edit sessions, the barrier is
retired so it cannot block the new application. This is an explicit degradation:
the adapter cannot complete that old operation without an edit session. The
real-host harness must record this condition. If a host accepts an asynchronous
request but never invokes its edit session, the skeleton has no wall-clock
timeout; that lifecycle guarantee also requires interactive host validation.
Deactivation consumes this same bounded stale budget before releasing the old
context and composition references; it does not leave a failed split queued
indefinitely merely because the service has become inactive.

## Verification layers

1. Native state-machine tests exercise activation, focus/context transitions,
   synchronous and delayed edit sessions, stale-result handling, rollback,
   long-composition limits, sensitive scopes, teardown, and class-factory
   lifetime.
2. Registration transaction tests exercise preflight, exact restoration,
   partial failures, rollback failures, ownership, idempotence, and cleanup.
3. PE verification inspects machine type, exact exports, mitigations, runtime
   imports, and version metadata from the produced DLL.
4. MSVC native code analysis checks the native implementation with warnings as
   errors.

The exact commands, counts, toolchain, and latest results are recorded in
[STATUS.md](STATUS.md), not duplicated here.

## Isolated VM acceptance still required

- install, repair, upgrade, uninstall, reboot, and interrupted-upgrade flows;
- activation and unload in Notepad, Office, Chromium, Electron, Terminal,
  WinUI/UWP, and traditional Win32 controls;
- focus/context destruction while an asynchronous edit session is pending;
- password/PIN and configured-sensitive applications;
- x64 and ARM64 packages, plus a separately scoped x86 host strategy;
- signed binaries and package trust behavior;
- host-process fault isolation and crash/recovery evidence.

Until these checks and the decoder/candidate milestones are complete, the DLL
is a development artifact only.
