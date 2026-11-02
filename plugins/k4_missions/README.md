# k4_missions — K4-Missions on Plugify

Port of **K4ryuu/K4-Missions-SwiftlyS2 v1.0.6** (C#, .NET 10) to a native C++ Plugify plugin.
The plugin is **standalone** (`docs/PLUGIN_STANDARD.md` §7): it talks to `s2sdk` directly, keeps no
`pisexcompat` and no helper library, and the only library it links is `libs/plgdb` (the asynchronous
MySQL/SQLite store). `hudmenu` is a normal *optional* dependency, used only to honour the player's
own `!menu` choice.

| Path | Role |
| --- | --- |
| `src/k4_core.{hpp,cpp}` | Engine-free core: config.json/missions.json parsing, event-name conversion, filter matching, progress accounting, reset-period arithmetic, VIP amount, phrase/reward placeholder expansion, event_properties JSON. |
| `src/k4_store.{hpp,cpp}` | The `k4_missions` table on plgdb (MySQL/MariaDB + SQLite), asynchronous schema migration. |
| `src/k4_webhook.{hpp,cpp}` | Discord webhook queue and the ISteamHTTP transport copied from `plugins/cs2ac/src/webhook.cpp` (never logs a URL or token). |
| `src/plugin.cpp` | The s2sdk glue: lifecycle, map gate, Post event hooks, player registry, mission assignment/progress/completion, native menu, command ownership, accumulator clocks, unload teardown. |
| `src/kv1.{hpp,cpp}` | Read-only Valve KeyValues reader (databases.cfg, the phrase file, core.cfg) replacing pisexcompat's `KeyValues`. |
| `src/paths.hpp` | The previous port's path rules, byte for byte (see "Configuration files"). |
| `src/phrases.hpp` | The phrase table: language chain, per-key fallback, "unknown key is empty". |
| `src/chat_text.hpp` | The cs2-menus `{TAG}` colour table (`ColorizeChat` for chat, `StripColorTags` for menus/console). |
| `src/map_gate.hpp` | The PITFALLS B1/B2 readiness gate (`GetCurrentMap()` proof for the mid-map latch). |
| `k4_missions.pplugin.in` | Manifest (name `k4_missions`, dependencies s2sdk >= 3.0.0, vip >= 1.0.0, optional hudmenu/permissions). |
| `assets/` | Default `config.json`, `missions.json`, the `databases.cfg.example` template, `discord/*.json`. |
| `translations/k4_missions.phrases.txt` | The original `resources/translations/en.jsonc` keys/texts in the Pisex phrases format. |
| `package.sh` | Lays out `build/package/k4_missions/` (realpath-guarded, no secrets, never `databases.cfg`). |
| `tests/` | `k4_missions_tests` (core + KV1/phrases + store on SQLite/MySQL), `k4_missions_plugin_tests` (the real TU against recording fakes + SQLite), `k4_missions_package_tests` (package.sh + secrets). |

## Configuration files

All files live in `<plg configs>/k4_missions/`:

* `config.json` — the original file, section `K4Missions`, same keys and defaults:
  `DatabaseConnection` (kept for compatibility, informational), `MissionCommands`, `VipNameDomain`,
  `MinimumPlayers`, `MissionAmountNormal`, `MissionAmountVip`, `VipFlags` (kept, unused),
  `EventDebugLogs`, `AllowProgressDuringWarmup`, `ResetMode` (`PerMap`/`Daily`/`Weekly`/`Monthly`/
  `Instant`), `WebhookUrl`. Property names are matched case-insensitively; JSONC comments are
  accepted.
* `missions.json` — the original mission definitions (`Event`, `Target`, `Amount`, `Phrase`,
  `RewardCommands`, `RewardPhrase`, `EventProperties`, `MapName`, `Flag`). Event names are the
  SwiftlyS2 class names (`EventPlayerDeath` -> engine `player_death`). A mission with a `Flag` is
  only assigned to a player the official permissions plugin answers `Allow` for that permission
  string (the shipped example uses `vipcore.supervip`).
* `databases.cfg` — a Pisex KeyValues file with a section named `k4_missions`:
  `driver` (`mysql` or `sqlite`), `host`, `port`, `user`, `pass`, `database`, and `sqlite_path`
  (SQLite only; relative paths resolve against the plugin data directory). Without the file (or the
  section) the plugin logs one error and falls back to `<data>/k4_missions/k4_missions.sqlite3`,
  like `admin_system`/`levels_ranks`. The package ships a credential-less `databases.cfg.example`
  (never `databases.cfg`, so the deploy's `cp -n` cannot shadow the real file).
* `discord/complete.json`, `discord/complete_all.json`, `discord/reset.json` — the original
  templates, shipped byte for byte. They are read once at plugin start and `{placeholder}` values
  are JSON-escaped exactly like the original.
* `translations/k4_missions.phrases.txt` — the original phrase keys and texts, with the cs2-menus
  colour markup (`[silver]` -> `{GRAY}`, `[lime]` -> `{LIME}`, `[white]` -> `{WHITE}`,
  `[yellow]`/`[gold]` -> `{LIGHTOLIVE}`, `[lightred]` -> `{LIGHTRED}`).

File lookup keeps the previous port's order exactly (`src/paths.hpp` documents each step):

| File | Order |
| --- | --- |
| `config.json`, `missions.json`, `discord/*` | `<configs>/k4_missions/<file>`, no fallback |
| phrases | raw CWD spelling, then `<configs>/k4_missions/translations/`, then `<configs>/k4_missions/` |
| `databases.cfg` | raw `addons/configs/databases.cfg` (legacy Metamod install), then `<configs>/k4_missions/databases.cfg`, then translations/, then data/ |
| SQLite path | `addons/data/<x>` -> `<data>/k4_missions/<x>` |
| language | `<configs>/core.cfg` `ServerLang`, then s2sdk `ServerLanguage`, then `en` |

## Behaviour

Ported 1:1:

* event-based progress for `player_death` (attacker/assister), `round_mvp`, `bomb_planted`,
  `bomb_defused`, `hostage_rescued`, `grenade_thrown`, `round_end` (winner/loser) and the internal
  `PlayTime` minute tick; all mission events are **Post** hooks like the original's `HookPost`, and
  the trampoline always returns `ResultType::Continue`;
* `EventProperties` matching: booleans exact, numbers `event >= mission`, strings case-insensitive
  contains, `MapName` exact, and the round-end path ignores the filters (the original passes a
  null property dictionary there); property names are converted from the SwiftlyS2 CLR spelling
  to the engine key (`Headshot` -> `headshot`, `DmgHealth` -> `dmg_health`);
* minimum-player gate (`MinimumPlayers`, counting connected non-bot players on T/CT) and warmup
  protection (`AllowProgressDuringWarmup`, `m_bWarmupPeriod` read through the vip core's
  `VIP_WarmupPeriod()`);
* per-player assignment: `MissionAmountNormal` / `MissionAmountVip`, random unique picks filtered
  by `Flag` (official permissions plugin, same flag strings) and by the original duplicate key
  `Event|Target|Amount|Phrase`; excess missions removed highest-database-id first;
* VIP status from the `vip` core (`VIP_IsClientVIP`) plus the original `VipNameDomain`
  name-substring rule (case-insensitive);
* reset modes: Daily (today 23:59:59), Weekly (Saturday 23:59:59, i.e. as the coming Sunday
  starts; a Sunday moves +7 days), Monthly (last day of month 23:59:59), PerMap, Instant; a
  60-second expiry check (run once at startup too) cleans the database, resets online players
  (with the original `dailyreset` message) and sends the reset webhook; missions are persisted
  exactly like the original (insert, update progress/completed at round end and on disconnect);
* rewards: every `RewardCommands` entry is executed as a server command with `{slot}`, `{userid}`,
  `{name}`, `{steamid64}`, `{steamid}` and `u0022` replaced (`userid` is the controller's entity
  index, i.e. slot + 1 on a normal server); completion chat line, Discord
  `complete`/`complete_all` webhooks;
* the `!mission`/`!missions` chat and console commands open the mission menu. The menu shows the
  player-limit warning, the reset countdown, the mission list with percentage/completed state, and
  locked VIP slots; selecting a mission prints the original four chat details lines.

## Deviations

1. **Database**: PostgreSQL is not ported (plgdb supports MySQL/MariaDB and SQLite only). The M001
   table and the M002 columns (`event_properties`, `map_name`) are created with one idempotent
   `CREATE TABLE IF NOT EXISTS` plus two `ALTER TABLE ... ADD COLUMN` statements whose
   "duplicate column" error is ignored, instead of FluentMigrator.
2. **`ResetMode.PerMap`** is implemented as documented ("missions reset on map change"). The
   original v1.0.6 never removes PerMap missions (its `CalculateExpirationDate` returns null and
   no map hook clears them), so this is a deliberate fix.
3. **Config hot-reload** (`reloadOnChange`) is not ported: `config.json` is read at plugin start.
4. **`VipFlags`** is parsed and kept in the file shape but unused: VIP status comes from the vip
   core (the task's mapping decision). `DatabaseConnection` is likewise informational.
5. **`EventDebugLogs`** logs the properties the loaded missions actually filter on; the original
   reflected over every property of the event type (s2sdk exposes no property list).
6. **Player registration/validation** uses the `player_activate` Post event plus a once-per-second
   slot reconcile (and a prune every minute), because s2sdk's `OnClientAuthenticated` is not
   reliable in this stack (PITFALLS B6). The original used SwiftlyS2's player manager.
7. **`{userid}`** is the CS2 engine user id, taken from the player's controller entity **index**
   (`EntHandleToEntIndex` of the signed `CEntityHandle`, falling back to `slot + 1` when no
   controller exists); this is what SwiftlyS2's `IPlayer.PlayerID` and the event `userid` carry.
8. The original's `resources/missions.json` auto-copy is not reproduced: the package installs
   `missions.json` into the configs directory, and a missing file is logged.
9. **No s2sdk timers.** The reconcile (1 s), playtime (60 s), expiration (60 s), schema retry and
   the player-load retries are accumulators in `OnPluginUpdate(dt)` (PITFALLS C9); a server hitch
   can fire a tick at most once. "Next frame" work is a deferred queue drained from the same
   update, outside the engine's command/menu/timer locks.
10. **Expiry comparisons use server-local time everywhere.** The original stores `expires_at`
    from `DateTime.Now` (local) but the periodic cleanup queried it against `DateTime.UtcNow`, so
    on a non-UTC server daily/weekly/monthly resets fired hours early. The port uses local time.
11. Mission completion is a conditional `UPDATE ... SET completed = 1 WHERE id = ? AND
    completed = 0`; a mission another server already completed in a shared database is not paid
    twice, which is what the original's read-then-check did.
12. s2sdk has no "does this event carry property X" query, so `EventProperties` lookups are typed
    getters: a filter naming a property the event does not declare receives the getter's default
    (0 / false / empty). A misspelled boolean `false` filter can therefore match; all properties
    documented by the original and shipped in the default `missions.json` exist on their events.
13. **Database start-up is asynchronous and self-healing.** The CREATE/ALTER migration runs on
    plgdb's worker thread, so a slow or unreachable database cannot hold the game thread during
    plugin start / `plg reload`; a failed migration is retried with backoff (5 s, 15 s, 30 s, then
    once a minute). A failed player SELECT is retried too: a transient error is not "no missions".
14. **Reward-command `{name}` is sanitized.** The expanded line goes to the engine command buffer,
    so `;`, quotes, newlines and control characters are stripped from the player-controlled name.
15. **Native menus instead of pisexcompat menus.** The menu type follows
    `pisexcompat::ResolveS2sdkMenuTypeName` exactly with the old port's HUD_LAYOUT default: the
    player's own `!menu` choice from the optional hudmenu plugin, `hud` -> `hud` (registered) or
    `button`, `wasd` -> `wasd` (registered) or `button`, anything else -> `hud` when registered
    otherwise `button`. The reserved s2sdk menu id 0 is burned with a dummy menu and released on
    unload, every action is queued for `OnPluginUpdate`, and closed menus are destroyed there.

## Safety checklist (handoff/weaponskins/PITFALLS.md)

* **A1/A7** — `OnPluginStart()` never returns `Failed`. Every registration is guarded by its full
  register+unregister pair (map listeners, `HookEvent`+`UnhookEvent`,
  `AddConsoleCommand`+`RemoveCommand`+`RemoveCommandListener`, the complete menu set), and
  `OnPluginEnd()` removes all of it: hooks, created commands (a pre-existing name only loses its
  listener), map listeners, every menu (reserved id 0 included), the webhook worker and the plgdb
  worker (its destructor flushes and joins). `plg reload` is clean.
* **B1/B2** — `src/map_gate.hpp`: no client/entity/schema call before the first map or between
  `OnMapEnd` and the next `OnMapStart`. The mid-map latch of the first `OnPluginUpdate` requires
  all four map-listener bindings, no `OnMapEnd` since start, and `GetCurrentMap()` naming a real
  map (not `""`, not `"<empty>"`). `OnMapEnd` only clears state.
* **C1/C2** — slots strictly `0..63`; a player needs connected + in-game + not SourceTV + not fake
  + a valid controller `CEntityHandle` (only `0`/`-1` are invalid; negative high-serial handles
  are valid and validated with `IsValidEntHandle`); entity *indices* come from
  `EntHandleToEntIndex` (the `{userid}` placeholder). Every player record carries
  `SteamID64 + controllerHandle`, re-checked by every deferred action, menu action and async
  database callback.
* **C8/C9** — schema offsets are resolved only behind the map gate and cached only when `>= 0`
  (`m_iTeamNum`, 1 byte); no s2sdk timer drives gameplay/client/DB logic (accumulators instead).
* **C10** — native menus with the full API required before the first `CreateMenu`; menu id 0
  reserved; all actions deferred; map-end/unload teardown does `CancelClientMenu(Disconnect)` +
  `GetClientMenu` recheck + `SetMenuType("chat"/"console")` + `GetMenuType` verification before
  `DestroyMenu`, otherwise it logs `A7_UNRESOLVED` and does not destroy.
* **C11** — the server console is strictly `caller == -1` and is refused (player menu command);
  every other out-of-range caller is refused; a command that already existed is never
  `RemoveCommand`'d.
* **Events** — player fields are read only via `GetEventPlayerSlot` (the `bomb_pickup` `userid`
  `player_pawn` field uses the map-gated `GetEventPlayerPawn` + `ClientPtrToPlayerSlot` fallback);
  numeric fields such as `round_end.winner` use `GetEventInt`; all hooks are Post and always
  return `Continue`.
* **Secrets** — the webhook URL/token never appears in a log line; the package ships
  `databases.cfg.example` with empty credentials and never `databases.cfg`.

## Build, test, package

```bash
./build.sh plugify-k4_missions k4_missions_tests k4_missions_plugin_tests k4_missions_package_tests
./test.sh                       # all build/tests binaries, SQLite + throwaway MariaDB
bash plugins/k4_missions/package.sh
```

The tests cover the config/mission/KeyValues/phrase readers, event-name conversion, all filter
kinds, progress accounting (including PlayTime), reset-period arithmetic, VIP amount selection,
reward placeholders, event_properties round-trip, the store against SQLite and MySQL, the map
gate, the client/handle guards, the schema cache, the command/event/menu lifecycles, deferred
actions and database callbacks keyed by SteamID64 + controller handle, the unload/reload teardown,
the webhook secret rule and the package layout (no `databases.cfg`, empty credentials, realpath
guards).

## Live-check list

1. Mission assignment on join (SQLite fallback and the production MySQL section), `!mission`
   menu in the player's menu type, details lines, locked VIP slots.
2. Kill/assist/MVP/plant/defuse/hostage/grenade/round-win progress with filters
   (`Weapon` string contains against CS2's weapon names, `Headshot`, `Penetrated`, `Distance`).
3. Warmup protection and the minimum-player gate.
4. Daily/weekly/monthly expiry across a restart, Instant re-roll, PerMap reset on changelevel
   (including the boot double map load).
5. Reward command execution (the production commands are `sw_givecredits ...` — the shop plugin
   must be present) and `{userid}`/`{slot}` semantics for admin-command rewards.
6. Discord completion/all-complete/reset webhooks on a live server (Steam HTTP availability).
7. `plg reload` with players online: no duplicate missions, no crash, hooks/commands/menus
   removed.
8. MySQL leg of `./test.sh`.
