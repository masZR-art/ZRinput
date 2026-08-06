# ZRinput

ZRinput is a clean-room, privacy-first Chinese text service for Windows 11.
The current checkpoint contains a portable C++20 composition engine, pinyin
parser, offline dictionary and decoder, cancellable candidate pipeline, local
prediction service, asynchronous User Memory, and a versioned theme engine.

Registered TSF input, the native candidate renderer, the shared decoder
service, installer, and settings application remain in development. This
checkpoint is therefore not yet an installable system input method.

The active development status, verified commands, and known limitations are
tracked in [docs/STATUS.md](docs/STATUS.md). Architecture decisions and the
explicit MVP boundary are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/MVP.md](docs/MVP.md). The User Memory runtime, privacy, scoring, and
storage contract is documented in [docs/USER_MEMORY.md](docs/USER_MEMORY.md).

## Clean-room boundary

This branch was created as an empty orphan worktree. It does not contain code,
data, visual assets, measured values, or tests copied from Microsoft IMEs or
from the discarded repository contents. Windows 11-like presentation is a
replaceable theme built from documented Windows APIs and repeatable external
screen measurements.

## Build

```powershell
cmake --preset windows-x64
cmake --build --preset x64-release
ctest --preset x64-release
```

ARM64 is configured independently with `windows-arm64` and
`arm64-release`. Installation commands will be documented when the TSF package
checkpoint is complete.
