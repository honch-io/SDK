# Changelog

All notable changes to the Honch SDK, across every port. The version is the
single shared SDK version each port reports on the wire as `$sdk_version`.

This file is maintained **automatically** by `tools/release.py` during the
version bump (the release pipeline runs that script): each release prepends a
section below with the commits since the previous release tag. Do not
hand-edit released sections. Full notes are also on the GitHub Releases page.

## 0.2.4 - 2026-06-19

- fix(ci): install libcurl for posix verify; gate platform dispatch in-run not in if (1e5e322)
- ci(release): dispatch sdk-released to platform to auto-sync the conformance mirror (0839107)
- ci(release): gate arduino-publish on version match; skip channels without creds (b90e870)
- ci(release): one-button dispatch -> verify -> release PR -> merge-ships tagging (e97bd70)
- release: own per-port README status lines + drop hardcoded prose versions (fec4d16)
- test(posix): assert wire $sdk_version via HONCH_SDK_VERSION macro (b828b0d)
- test(sdk): derive version from canonical header instead of hardcoding (299b995)
- fix(sdk): shorten firmware_version state key to fit NVS 15-char limit (15f5a03)
