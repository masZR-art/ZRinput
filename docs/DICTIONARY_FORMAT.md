# ZRDICT package format

ZRDICT is a bounded little-endian binary format. Runtime lookup never parses
TSV and never synchronously scans the full dictionary for a key press.

## Header (32 bytes)

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | byte[8] | `ZRDICT\0\0` |
| 8 | uint32 | format version (`2`) |
| 12 | uint32 | header size (`32`) |
| 16 | uint64 | payload byte length |
| 24 | uint32 | CRC32 of the complete payload |
| 28 | uint32 | record count |

## Version 2 record

```text
uint16 reading_bytes
uint16 text_bytes
float32 static_frequency
uint16 flags
uint16 reserved
byte[reading_bytes] canonical lowercase ASCII reading
byte[text_bytes] UTF-8 candidate text
```

Canonical readings contain lowercase `a-z` syllables separated by one ASCII
space. The loader rejects unknown versions, mismatched lengths/checksums,
invalid readings, non-finite frequencies, trailing bytes, and configured byte,
entry, reading, or text budgets. Version 1 records omit `flags` and `reserved`;
they migrate in memory with flags set to zero.

Package replacement writes and flushes a sibling temporary file, rotates the
previous package to `.bak`, and only then renames the complete temporary file.
Loading builds exact, compact-prefix, and initial indexes off the key path and
publishes an immutable `shared_ptr` snapshot. Existing requests retain their
old snapshot until completion.

