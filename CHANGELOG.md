# Changelog

## 1.1.0 - 2026-09-30

- Added an on-demand `Version` field sourced from OpenType Name ID 5.
- Release each font mapping before returning from WDX calls so files remain
  immediately renameable and deletable while the plugin stays loaded.
- Prefer the current Windows language, related language variants, and then
  English before unrelated localized names.

## 1.0.0 - 2026-09-28

- Reimplemented the four font-name fields in native C++20.
- Parse only the WDX field requested by the caller.
- Added memory-mapped file access and a per-thread last-file cache.
- Preserved localized CJK selection and malformed Chinese-name repair.
- Added Win32 and x64 builds with a statically linked MSVC runtime.
- Added parser tests, WDX export tests, a real-font probe, and GitHub CI.
- Focused supported formats on TTF, OTF, OTB, TTC, and OTC.
