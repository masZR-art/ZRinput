# ZRinput

ZRinput is a clean-room, privacy-first Chinese text service for Windows 11.
The repository contains a C++20 Text Services Framework (TSF) adapter, an
offline pinyin decoder, a native candidate surface, a versioned theme engine,
and a desktop settings application.

The active development status, verified commands, and known limitations are
tracked in [docs/STATUS.md](docs/STATUS.md). Architecture decisions and the
explicit MVP boundary are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/MVP.md](docs/MVP.md).

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

