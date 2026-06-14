# Rules

- Read `EMULEBB_WORKSPACE_ROOT\repos\emulebb-tooling\docs\WORKSPACE-POLICY.md`
  first; it is authoritative for workspace-wide rules.
- Start from
  `EMULEBB_WORKSPACE_ROOT\repos\emulebb-tooling\docs\reference\AGENT-CHECKLIST.md`
  for the repeatable operating path.

Everything below is this repo's local guidance for the eMuleBB-maintained
libtorrent fork.

## What this fork is

- This is `emulebb-libtorrent`, a fork of `arvidn/libtorrent` tracked on branch
  `RC_2_0` (libtorrent 2.0.x). It is the BT engine under
  [qBittorrentBB](https://github.com/emulebb/qbittorrentbb) and is tuned for the
  BB world: VPN-first egress correctness and a DHT-harvester-friendly engine.
- The exact delta over upstream (owned files, shared seams, features, rebase
  workflow) is described in `fork-delta.json` — keep it current when the delta
  changes.

## Working on the fork

- **Stay additive and close to upstream.** Prefer new files (`stun.*`,
  `http_ip_probe.*`, `dns_resolver.*`) over edits to shared files; when a shared
  seam must change, keep it minimal so upstream rebases stay conflict-driven.
- **ABI is append-only.** New `settings_pack` (str/int/bool) and `alert_types`
  entries go at the END (before `max_*_setting_internal`; bump `num_alert_types`
  within `abi_alert_count`). Never reorder or remove enum entries.
- **No AI/Claude/Anthropic attribution** in commits, code, or docs.
- Tracked text is **LF**.

## Build & output

- Build ONLY under `%EMULEBB_WORKSPACE_OUTPUT_ROOT%` (never this tree, never
  `c:\prj`). Generator: VS 2022 + vcpkg toolchain (x64-windows).
- Install for qBittorrentBB to consume with
  `cmake --install <build> --config Release --prefix <out>\deps\libtorrent` — the
  **default prefix wrongly targets `C:\Program Files\libtorrent`**, always pass
  `--prefix`. After installing, copy the fresh `torrent-rasterbar.dll` next to the
  running `qbittorrent.exe`.

## Egress / VPN policy (this fork's whole point)

- The egress hardening (L1-L5), the bound DNS resolver, and the VPN egress guard
  exist so BT/DHT traffic cannot leave outside the VPN tunnel. On Windows a plain
  `bind()` does NOT pin the egress NIC (weak host model) — `IP_UNICAST_IF` does.
- Validate egress changes with the qBittorrentBB live-wire harness
  (`qbittorrentbb/test/livewire/vpn_guard_test.py`): it asserts ALLOW (clean) and
  LEAK (fail-closed) end to end against the live tunnel.

## Upstream sync

- `.github/workflows/nightly-upstream.yml` checks `upstream/RC_2_0` nightly and
  pushes a rebased `automation/upstream-nightly` branch only when upstream moved
  and the rebase is clean. It does NOT run the heavy C++/Qt build or touch
  `RC_2_0` — a human/local build validates before fast-forwarding `RC_2_0`.
