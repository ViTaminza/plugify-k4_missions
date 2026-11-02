# k4_missions (Plugify)

K4-Missions: a dynamic mission system for Counter-Strike 2 (K4ryuu/K4-Missions-SwiftlyS2 v1.0.6, C#/.NET), ported to Plugify on native s2sdk. Missions are defined in missions.json with event-based progress (kills, assists, MVP, bomb plant/defuse, hostage rescue, round wins, playtime), event property filters (weapon, headshot, map, distance, ...), a minimum-player requirement and optional warmup protection. Assignment counts differ for VIP (the vip core) and normal players; missions reset daily/weekly/monthly/per-map/instantly; rewards are server commands with the original placeholders; Discord webhooks report completions and resets. Configuration: config.json, missions.json, discord/*.json and databases.cfg (section k4_missions, MySQL or SQLite; PostgreSQL is not supported).

Plugify port (fork) of **K4 Missions** by K4ryuu.
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=3.0.0), vip (>=1.0.0), hudmenu (>=1.0.0, optional), permissions (>=1.3.1, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `k4_missions-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/k4_missions`.

`plugins/vip` carry only the API headers of the plugins this one uses.

**Testing status: NOT TESTED IN GAME. / Не тестировался в игре.** Standalone compilation and package checks do not validate gameplay, database, Discord or external service integrations.

## License
GPL-3.0, see LICENSE.

