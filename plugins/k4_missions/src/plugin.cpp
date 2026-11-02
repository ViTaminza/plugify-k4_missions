// k4_missions — the s2sdk glue of the K4-Missions port (native, no pisexcompat).
//
// Behaviour is the original's (K4ryuu/K4-Missions-SwiftlyS2 v1.0.6), with the differences the
// port task requires; each deviation is marked with "Deviation:" below and collected in README.md.
//
// The original Plugin.cs plus its services map onto this file as follows:
//   Plugin.Load                 -> MissionsPlugin::OnPluginStart (config, missions, phrases, database)
//   RegisterMissionEvents       -> RegisterEventHooks (Post hooks; SwiftlyS2's HookPost)
//   RegisterEventHandlers       -> the same trampoline (player_activate/disconnect/round_end)
//   RegisterCommands            -> RegisterCommands
//   OnMapLoad                   -> OnMapStartListener -> HandleMapStarted
//   PlayerManager               -> the PlayerRecord registry and its functions
//   ResetService                -> the expiration accumulator and the reset-mode helpers
//   WebhookService              -> k4_webhook
//
// Standalone (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no helper library; the optional
// hudmenu plugin is a normal optional dependency, used only to honour the player's own "!menu"
// choice (hudmenu::GetClientMenuType) with the exact pisexcompat::ResolveS2sdkMenuTypeName table.
// plgdb (libs/plgdb) is the only library, for the asynchronous MySQL/SQLite store. The guards of
// handoff/weaponskins/PITFALLS.md live right here:
//
//   * A1/A7  OnPluginStart() never returns Failed (that stops every Plugify plugin). Before any
//            registration the complete register+unregister pair is checked (map listeners,
//            HookEvent+UnhookEvent, AddConsoleCommand+RemoveCommand+RemoveCommandListener, the
//            full menu set), so OnPluginEnd can always undo exactly what was registered: event
//            hooks are unhooked, created commands are RemoveCommand'ed, pre-existing names only
//            get their listener detached (never RemoveCommand on a foreign command, C11), the map
//            listeners are unregistered, every menu (the reserved id 0 included) is closed through
//            CancelClientMenu/SetMenuType/GetMenuType before DestroyMenu, and the webhook/DB
//            workers are shut down (the plgdb destructor flushes and joins its worker).
//   * B1/B2  the map gate in src/map_gate.hpp. No client, entity or schema call is made before the
//            first map or between OnMapEnd and the next OnMapStart. Ready is latched by OnMapStart,
//            or by the first OnPluginUpdate of a plugin loaded into a running map - only with all
//            four map-listener bindings registered, no OnMapEnd seen since OnPluginStart, and
//            GetCurrentMap() naming a real map ("", "<empty>" and a missing binding never open the
//            gate). OnMapEnd only clears state.
//   * C1/C2  slots are strictly 0..63; a client query needs the map gate, and a player needs
//            IsClientConnected + IsClientInGame + !IsClientSourceTV + !IsFakeClient + a valid
//            controller. Entity identity is a signed CEntityHandle (index | serial << 15): only 0
//            and -1 are invalid (a negative handle with serial >= 0x8000 is a perfectly valid
//            entity, so no `> 0` test is ever applied to a handle), validated with
//            IsValidEntHandle. Entity *indices* are positive and come from EntHandleToEntIndex.
//            The `{userid}` placeholder is the controller's entity index (what SwiftlyS2's
//            IPlayer.PlayerID and the event "userid" carry), never a raw handle.
//            Every player record keeps the SteamID64 AND the controller CEntityHandle captured
//            when it was created; every deferred action, menu action and async database callback
//            re-checks both, so a reused slot can never act for (or pay) the previous occupant.
//   * C8/C9  schema offsets are resolved only behind the map gate and only cached when >= 0 (the
//            controller's m_iTeamNum, 1 byte). No s2sdk timer is used for gameplay/client/DB
//            logic: the reconcile/playtime/expiration/schema-retry/player-load-retry clocks are
//            accumulators in OnPluginUpdate(dt), and the delta is bounded so a server hitch can
//            never fire a tick more than once.
//   * C10    menus: the full set (Create/Destroy/Display/AddItem/GetClientMenu/CancelClientMenu/
//            SetMenuType/GetMenuType/IsMenuTypeRegistered) is required before the first CreateMenu;
//            the reserved id 0 is burned with a dummy menu nobody can display (as pisexcompat did).
//            Every menu action is queued and applied from OnPluginUpdate, outside the menu
//            manager's lock. On map end and on unload the display sessions are ended first with
//            CancelClientMenu(Disconnect), re-checked with GetClientMenu, and a menu that is still
//            displayed gets its type switched to a builtin ("chat", then "console") verified with
//            GetMenuType before DestroyMenu - if that is impossible the cleanup logs A7_UNRESOLVED
//            and leaves the menu alone instead of running a custom backend's close over the dying
//            client layer.
//   * C11    command ownership: AddConsoleCommand() answering false means the name already
//            existed and only a listener was attached, so the teardown detaches that listener
//            and never RemoveCommand's it. The server console is strictly caller == -1 and is
//            refused (this is a player menu command); any other out-of-range caller is refused
//            too. Chat "!mission" reaches the same callback through s2sdk's chat-trigger path.
//   * events the player fields (userid/attacker/assister/...) are read ONLY through
//            s2sdk::GetEventPlayerSlot, never GetEventInt; numeric fields (round_end.winner) use
//            GetEventInt. The one exception the engine forces is a "player_pawn" event field (the
//            gameevents dump marks bomb_pickup.userid as player_pawn, not player_controller): for
//            those the map-gated GetEventPlayerPawn + ClientPtrToPlayerSlot path is the fallback.
//            All hooks are Post and the trampoline always returns ResultType::Continue.
//   * C12    the previous port's "no map" startup is preserved: file/config work happens in
//            OnPluginStart, every engine-facing service starts on the first ready update.
//   * files  the plugin reads only from <configs>/k4_missions (paths.hpp keeps the exact previous
//            candidate order, including the legacy databases.cfg location) and writes only the
//            SQLite file under <data>/k4_missions.
//
// JSON is parsed by the bundled nlohmann header through k4_core; the KeyValues files (databases.cfg,
// the phrase file, core.cfg) are read by the plugin-local KV1 reader (kv1.hpp) that replaces
// pisexcompat's KeyValues.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <hudmenu/hudmenu.hpp>
#include <permissions/permissions.hpp>
#include <plg/plugin.hpp>
#include <plgdb/db.hpp>
#include <s2sdk/s2sdk.hpp>
#include <vip_api.hpp>

#include "chat_text.hpp"
#include "k4_core.hpp"
#include "k4_store.hpp"
#include "k4_webhook.hpp"
#include "kv1.hpp"
#include "map_gate.hpp"
#include "paths.hpp"
#include "phrases.hpp"

namespace k4 {

// The VIP core wrapper of plugins/vip (null-safe: every method checks that the vip bindings are
// filled, so a missing vip plugin degrades to "not a VIP" instead of a null call).
vip::IVIPApi* const g_vip = &vip::IVIPApi::Get();

namespace {

// ---------------------------------------------------------------------------------------------
// constants
// ---------------------------------------------------------------------------------------------

constexpr const char* kPluginName = "k4_missions";
constexpr const char* kLogTag = "k4_missions"; // the original's module prefix in every log line
constexpr const char* kPhrasesFile = "k4_missions.phrases.txt";
constexpr const char* kConfigFile = "config.json";
constexpr const char* kMissionsFile = "missions.json";
constexpr const char* kDiscordDir = "discord";
constexpr const char* kCoreConfigFile = "core.cfg";
constexpr int kMaxSlots = 64;

// The original Core.Scheduler.RepeatBySeconds(60f, ProcessPlayTime) / the expiration service.
constexpr float kMinuteTimer = 60.0f;
// s2sdk's OnClientAuthenticated is not reliable in this stack (PITFALLS B6) and player_activate can
// fire a moment before the controller is queryable; a cheap once-per-second slot reconcile is the
// perm_connector pattern and shortens that window to one second.
constexpr float kReconcileTimer = 1.0f;

// pisexcompat's menu defaults: chat pages of five, a 60 second display timeout, exit button on,
// back button off (the original's SetExitMenu(true) / SetBackMenu(false)).
constexpr int32_t kMenuItemsPerPage = 5;
constexpr int32_t kMenuTimeoutSeconds = 60;

// A hacky-but-cheap upper bound for one update's delta: a server hitch must never run the minute
// tick more than once (the accumulators are edge-triggered below).
constexpr float kMaxUpdateDelta = 60.0f;

// Retries of a database operation that failed while the database was down. The first retries are
// quick enough for a database that starts a few seconds after the game server (docker-compose),
// then the delay is capped at the minute tick so an outage is not hammered.
float SchemaRetryDelay(int failures) {
	if (failures <= 1) {
		return 5.0f;
	}
	if (failures == 2) {
		return 15.0f;
	}
	if (failures == 3) {
		return 30.0f;
	}
	return kMinuteTimer;
}

float PlayerLoadRetryDelay(int attempt) {
	if (attempt <= 1) {
		return 5.0f;
	}
	if (attempt == 2) {
		return 15.0f;
	}
	return kMinuteTimer;
}

// --- small conversions -------------------------------------------------------------------------

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

plg::string ToPlg(const std::string& text) {
	return plg::string(text.c_str());
}

plg::string ToPlg(const char* text) {
	return plg::string(text != nullptr ? text : "");
}

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(std::string(message), severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogWarning(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

bool RecordExpired(const Mission& mission, std::time_t now) {
	if (!mission.hasExpiresAt) {
		return false;
	}
	const std::optional<std::time_t> expiration = ParseDateTime(mission.expiresAt);
	return expiration.has_value() && *expiration < now;
}

std::vector<int64_t> MissionIds(const std::vector<Mission>& missions) {
	std::vector<int64_t> ids;
	ids.reserve(missions.size());
	for (const Mission& mission : missions) {
		if (mission.id > 0) {
			ids.push_back(mission.id);
		}
	}
	return ids;
}

// s2sdk entity handles are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != 0 && h != -1. The sign means nothing - once the serial counter passes 0x8000 bit
// 31 is set and the handle is a negative int, and a `h > 0` test would silently drop that entity.
// Never apply this to an entity *index*: indexes are positive, handles are signed.
bool IsUsableHandle(int32_t handle) {
	return handle != 0 && handle != -1;
}

// ---------------------------------------------------------------------------------------------
// types
// ---------------------------------------------------------------------------------------------

// One online player (the original MissionPlayer). `slot` is re-validated against the engine's
// SteamID and controller handle before every use; `generation` invalidates async loads of a
// previous session.
struct PlayerRecord {
	uint64_t steamid = 0;
	int32_t slot = -1;
	// The controller's CEntityHandle captured together with the record: the identity of the entity
	// the deferred actions and database callbacks were stamped for.
	int32_t controllerHandle = 0;
	bool loaded = false;
	bool vip = false;
	uint64_t generation = 0;
	int pendingInserts = 0;
	std::vector<Mission> missions;
};

// One hooked game event: the SwiftlyS2 class name the missions use and the player fields they
// track (the original's Dictionary<string, HashSet<string>> _registeredEvents).
struct EventRegistration {
	std::string className;
	std::vector<std::string> targets;
	std::vector<std::pair<std::string, ValueKind>> debugKeys;
};

// One assignment batch (the original's per-join insert bookkeeping).
struct AssignmentBatch {
	uint64_t steamid = 0;
	uint64_t generation = 0;
	uint64_t mapGeneration = 0;
	int32_t controllerHandle = 0;
	int remaining = 0;
	int added = 0;
};

// One pending player load retry: the row of a player whose SELECT failed while the database was
// down. Re-armed with PlayerLoadRetryDelay() until it succeeds or the player leaves.
struct PlayerLoadRetry {
	uint64_t steamid = 0;
	uint64_t generation = 0;
	int attempt = 0;
	float delay = 0.0f;
};

// One menu this plugin displayed for a slot. s2sdk menus are shared objects with a handler, so the
// plugin owns exactly one per slot (the original opened one menu too) and destroys it when the
// display session ends.
struct OpenMenu {
	bool active = false;
	s2sdk::MenuId id = 0;
	uint64_t steamid = 0;
	int32_t controllerHandle = 0;
	// Item index -> database mission id (0 for a disabled/informational item). The menu handler is
	// given the index, not the info string, so this is the mapping the deferred action needs.
	std::vector<int64_t> missionIds;
};

// One entry of the mission menu under construction.
struct MenuItemSpec {
	std::string info;
	std::string text;
	s2sdk::MenuItemStyle style = s2sdk::MenuItemStyle::Default;
	int64_t missionId = 0;
};

// ---------------------------------------------------------------------------------------------
// forward declarations (the engine trampolines call into these)
// ---------------------------------------------------------------------------------------------

void StartSchemaMigration();
void LoadPendingPlayers();
void RequestPlayerLoad(uint64_t steamid, uint64_t generation, int attempt = 0);
void OnPlayerDataLoaded(uint64_t steamid, uint64_t generation, bool ok, std::vector<Mission> missions,
						int attempt);
void ReconcilePlayers(bool prune);
void EnsurePlayer(int32_t slot, uint64_t steamid);
void EnsureCorrectMissionCount(PlayerRecord& record);
void AssignRandomMissions(PlayerRecord& record, int count);
void RemoveExcessMissions(PlayerRecord& record, int count);
void OnMissionInserted(const std::shared_ptr<AssignmentBatch>& batch, int64_t id, Mission mission);
void RefreshVip(PlayerRecord& record);
void ProcessEvent(PlayerRecord& record, std::string_view className, std::string_view target,
				  const EventPropertyLookup* lookup);
void ProcessPlayTime();
void CompleteMission(uint64_t steamid, int64_t missionId);
void RunCompletion(uint64_t steamid, int64_t missionId);
void RemoveMission(uint64_t steamid, int64_t missionId);
void HandleExpiredMissions(uint64_t steamid);
void CheckExpiredMissions();
void SaveAllPlayers();
void ResetMissionsOnMapChange();
void ChatMessage(int32_t slot, const std::string& text);
PlayerPlaceholders Placeholders(const PlayerRecord& record);
void SendCompletionWebhook(const PlayerRecord& record, const Mission& mission);
void SendAllMissionsCompletedWebhook(const PlayerRecord& record);
void SendResetWebhook();

void RegisterEventHooks();
void UnregisterEventHooks();
void RegisterCommands();
void UnregisterCommands();
void RegisterMapListeners();
void UnregisterMapListeners();
void StartServices();
void HandleMapStarted();
void ProcessDeferred();
void ProcessMenuDestroyQueue();
void CloseMenusAtMapEnd();
void CloseMenusAtUnload();
void OpenMissionsMenu(int32_t slot, uint64_t steamid, int32_t controllerHandle);
void PrintMissionDetails(int32_t slot, uint64_t steamid, int32_t controllerHandle, int64_t missionId);
std::string ResetInfo(const PlayerRecord& record);

// ---------------------------------------------------------------------------------------------
// state (all of it is reset by OnPluginStart / OnPluginEnd, so `plg reload` is clean)
// ---------------------------------------------------------------------------------------------

// s2sdk command/menu/event callbacks are plain function pointers: the state they work on lives at
// file scope, not in a plugin object.
MapGate g_mapGate;

Config g_config;
std::vector<MissionDefinition> g_definitions;
std::unordered_map<std::string, EventRegistration> g_events;
std::vector<std::string> g_hookedEvents;

std::unique_ptr<plgdb::Database> g_database;
// Non-null only once EnsureSchemaAsync() has succeeded: until then players stay unloaded (the
// migration callback then loads them) instead of silently getting a permanently empty list.
std::unique_ptr<MissionStore> g_store;
bool g_schemaAttemptRunning = false;
int g_schemaFailures = 0;
bool g_schemaFailureLogged = false;
bool g_schemaRetryArmed = false;
float g_schemaRetryCountdown = 0.0f;
WebhookService g_webhook;

std::unordered_map<uint64_t, PlayerRecord> g_players;
std::array<uint64_t, kMaxSlots> g_slotSteam {};
uint64_t g_nextGeneration = 0;
uint64_t g_mapGeneration = 0;

std::map<std::string, std::string> g_phrases;
std::string g_phraseLanguage = "en";
std::string g_currentMap;

// CBaseEntity::m_iTeamNum of the controller: resolved only behind the map gate, cached only when a
// real offset came back (PITFALLS C8).
int32_t g_teamOffset = -1;

// The plugin's lifecycle flags.
bool g_running = false; // OnPluginStart ran and OnPluginEnd has not
bool g_started = false; // the first ready update ran (services started)
bool g_mapStartPending = false;

// Accumulators (PITFALLS C9: no s2sdk timer drives gameplay/client/DB logic).
float g_reconcileCountdown = 0.0f;
float g_playtimeCountdown = 0.0f;
float g_expirationCountdown = 0.0f;
std::vector<PlayerLoadRetry> g_loadRetries;

// Deferred work ("next frame" in the previous port): every closure runs from OnPluginUpdate, with
// the map gate open, outside the engine's command/menu/timer locks.
std::deque<std::function<void()>> g_deferred;

// Commands. `g_commandNames` are the registered spellings; a name whose AddConsoleCommand()
// answered false existed before this plugin and must only lose the attached listener (C11).
std::vector<std::string> g_commandNames;
std::vector<std::string> g_foreignCommands;
bool g_commandsRegistered = false;

// Menus.
std::array<OpenMenu, kMaxSlots> g_openMenus;
std::vector<s2sdk::MenuId> g_menuDestroyQueue;
bool g_menuIdZeroChecked = false;
bool g_menuIdZeroReserved = false;
bool g_menuUnavailableLogged = false;

// ---------------------------------------------------------------------------------------------
// PITFALLS C1/C2: the client guards
// ---------------------------------------------------------------------------------------------

bool SlotInRange(int32_t slot) {
	return slot >= 0 && slot < kMaxSlots;
}

// The map gate plus the slot range: the only requirement to address a client without reading it.
bool CanQueryClient(int32_t slot) {
	return g_mapGate.Ready() && SlotInRange(slot);
}

// A connected slot whose client object exists (pisexcompat's ClientExists). Bots are real clients
// and pass; the SourceTV proxy is excluded by the player-level checks below.
bool ClientExists(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot);
	} catch (...) {
		return false;
	}
}

bool IsFakeClientSlot(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsFakeClient == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsFakeClient(slot);
	} catch (...) {
		return true; // treat as unavailable rather than as a real player
	}
}

bool IsSourceTVSlot(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientSourceTV == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientSourceTV(slot);
	} catch (...) {
		return true;
	}
}

bool IsInGameSlot(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientInGame == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientInGame(slot);
	} catch (...) {
		return false;
	}
}

// The controller's CEntityHandle. 0 means "no controller" (the s2sdk export answers 0/-1 then).
// A negative handle with a high serial is valid and is returned as-is (PITFALLS C1).
int32_t ControllerHandle(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_PlayerSlotToEntHandle == nullptr) {
		return 0;
	}
	try {
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!IsUsableHandle(handle)) {
			return 0;
		}
		if (__s2sdk_IsValidEntHandle != nullptr && !s2sdk::IsValidEntHandle(handle)) {
			return 0;
		}
		return handle;
	} catch (...) {
		return 0;
	}
}

// The controller's entity INDEX (positive) for the `{userid}` placeholder. This is the conversion
// pisexcompat's GetControllerEntity() did; the index is what a CS2 game event's "userid" carries.
int32_t ControllerEntityIndex(int32_t slot) {
	const int32_t handle = ControllerHandle(slot);
	if (handle == 0 || __s2sdk_EntHandleToEntIndex == nullptr) {
		return 0;
	}
	try {
		const int32_t index = s2sdk::EntHandleToEntIndex(handle);
		return index > 0 ? index : 0;
	} catch (...) {
		return 0;
	}
}

// The Pisex IsClientReady(): a client object and a controller entity. Everything that reaches the
// controller/pawn goes through this (PITFALLS C2: s2sdk exports do not validate their arguments).
bool IsClientReady(int32_t slot) {
	return ClientExists(slot) && ControllerHandle(slot) != 0;
}

// A connected, in-game, non-bot, non-SourceTV player with a controller - the only shape this plugin
// ever registers or acts for.
bool IsRealPlayer(int32_t slot) {
	return IsClientReady(slot) && IsInGameSlot(slot) && !IsFakeClientSlot(slot) && !IsSourceTVSlot(slot);
}

uint64_t ClientSteamID64(int32_t slot) {
	// The s2sdk export is null-safe but answers from m_unauthenticatedSteamID for a slot without a
	// client - which keeps the previous occupant's id after a disconnect. Pisex returned 0 then.
	if (!ClientExists(slot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

// GetPlayerName(): pisexcompat required a client AND a controller, and turned s2sdk's "<unknown>"
// into "". Callers treat "" as "no name".
std::string ClientName(int32_t slot) {
	if (!IsClientReady(slot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		std::string name = ToStd(s2sdk::GetClientName(slot));
		if (name == "<unknown>") {
			name.clear();
		}
		return name;
	} catch (...) {
		return {};
	}
}

void* ClientPawnPointer(int32_t slot) {
	if (!IsClientReady(slot) || __s2sdk_GetClientPawn == nullptr) {
		return nullptr;
	}
	try {
		void* pawn = s2sdk::GetClientPawn(slot);
		if (pawn == nullptr) {
			return nullptr;
		}
		if (__s2sdk_IsValidEntPointer != nullptr && !s2sdk::IsValidEntPointer(pawn)) {
			return nullptr;
		}
		return pawn;
	} catch (...) {
		return nullptr;
	}
}

// The controller's team (CBaseEntity::m_iTeamNum, 1 byte), what pisexcompat's GetPlayerTeam read.
// The schema offset is resolved only while a map is up and only a successful offset is cached; a
// missing offset/binding falls back to the pawn-based export, which is only asked when a pawn
// exists (it prints a console warning otherwise).
int PlayerTeam(int32_t slot) {
	if (!IsClientReady(slot)) {
		return 0;
	}
	if (g_teamOffset < 0 && __s2sdk_GetSchemaOffset != nullptr) {
		try {
			const int32_t offset = s2sdk::GetSchemaOffset(ToPlg("CBaseEntity"),
														 ToPlg("m_iTeamNum"));
			if (offset >= 0) {
				g_teamOffset = offset;
			}
		} catch (...) {
		}
	}
	const int32_t handle = ControllerHandle(slot);
	if (g_teamOffset >= 0 && handle != 0 && __s2sdk_GetEntData != nullptr) {
		try {
			return static_cast<int>(s2sdk::GetEntData(handle, g_teamOffset, 1)) & 0xFF;
		} catch (...) {
			return 0;
		}
	}
	if (ClientPawnPointer(slot) != nullptr && __s2sdk_GetClientTeam != nullptr) {
		try {
			return static_cast<int>(s2sdk::GetClientTeam(slot));
		} catch (...) {
			return 0;
		}
	}
	return 0;
}

int MaxPlayers() {
	if (__s2sdk_GetMaxClients == nullptr) {
		return kMaxSlots;
	}
	try {
		const int32_t maxClients = s2sdk::GetMaxClients();
		return maxClients > 0 && maxClients <= kMaxSlots ? maxClients : kMaxSlots;
	} catch (...) {
		return kMaxSlots;
	}
}

// ---------------------------------------------------------------------------------------------
// state reset
// ---------------------------------------------------------------------------------------------

void ResetRuntimeState() {
	g_mapGate.Reset();
	g_config = Config {};
	g_definitions.clear();
	g_events.clear();
	g_hookedEvents.clear();

	g_store.reset();
	// g_database is shut down by OnPluginEnd; this defensive reset only releases a leftover of a
	// start that failed before the teardown ran.
	g_database.reset();
	g_schemaAttemptRunning = false;
	g_schemaFailures = 0;
	g_schemaFailureLogged = false;
	g_schemaRetryArmed = false;
	g_schemaRetryCountdown = 0.0f;

	g_players.clear();
	g_slotSteam.fill(0);
	g_nextGeneration = 0;
	g_mapGeneration = 0;
	g_phrases.clear();
	g_phraseLanguage = "en";
	g_currentMap.clear();
	g_teamOffset = -1;

	g_reconcileCountdown = 0.0f;
	g_playtimeCountdown = 0.0f;
	g_expirationCountdown = 0.0f;
	g_loadRetries.clear();
	g_deferred.clear();

	g_commandNames.clear();
	g_foreignCommands.clear();
	g_commandsRegistered = false;

	g_openMenus.fill(OpenMenu {});
	g_menuDestroyQueue.clear();
	g_menuIdZeroChecked = false;
	g_menuIdZeroReserved = false;
	g_menuUnavailableLogged = false;

	g_started = false;
	g_mapStartPending = false;
}

// ---------------------------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------------------------

void LoadConfiguration() {
	const std::string path = ConfigFilePath(kConfigFile);
	bool ok = false;
	const std::string text = ReadTextFile(path, &ok);
	if (!ok) {
		LogError(std::string("[") + kLogTag + "] cannot read '" + path + "' - the built-in defaults are used");
		return;
	}
	std::string error;
	if (!ParseConfig(text, g_config, error)) {
		LogError(std::string("[") + kLogTag + "] " + error + " - the built-in defaults are used");
		return;
	}
	if (!error.empty()) {
		LogWarning(std::string("[") + kLogTag + "] " + error);
	}
	if (g_config.databaseConnection != "host") {
		// Kept for compatibility with the SwiftlyS2 database.json name; the connection of this
		// port comes from databases.cfg (section "k4_missions").
		LogInfo(std::string("[") + kLogTag + "] DatabaseConnection is kept for compatibility; the database "
				"is read from configs/k4_missions/databases.cfg (section \"k4_missions\").");
	}
	if (g_config.missionAmountNormal > g_config.missionAmountVip) {
		LogWarning(std::string("[") + kLogTag + "] Normal mission amount (" +
				   std::to_string(g_config.missionAmountNormal) + ") is higher than VIP amount (" +
				   std::to_string(g_config.missionAmountVip) + "). This may cause issues.");
	}
}

void LoadMissionDefinitions() {
	const std::string path = ConfigFilePath(kMissionsFile);
	bool ok = false;
	const std::string text = ReadTextFile(path, &ok);
	if (!ok) {
		// The original copied its bundled resources/missions.json on first start. Plugify
		// installs the package's configs directory, so a missing file is a broken installation.
		LogError(std::string("[") + kLogTag + "] missions.json not found at '" + path + "' - no missions");
		return;
	}
	std::string error;
	if (!LoadMissions(text, g_definitions, error)) {
		LogError(std::string("[") + kLogTag + "] " + error);
		g_definitions.clear();
		return;
	}
	if (g_definitions.empty()) {
		LogError(std::string("[") + kLogTag + "] No missions found in missions.json");
		return;
	}

	g_events.clear();
	for (const MissionDefinition& definition : g_definitions) {
		if (definition.event == "PlayTime") {
			continue;
		}
		const std::string gameEvent = EventClassNameToEventName(definition.event);
		if (gameEvent.empty()) {
			LogWarning(std::string("[") + kLogTag + "] Event type " + definition.event + " not found.");
			continue;
		}
		EventRegistration& registration = g_events[gameEvent];
		registration.className = definition.event;
		if (std::find(registration.targets.begin(), registration.targets.end(), definition.target)
			== registration.targets.end()) {
			registration.targets.push_back(definition.target);
		}
		for (const auto& [key, value] : definition.eventProperties) {
			const bool known = std::any_of(registration.debugKeys.begin(), registration.debugKeys.end(),
										   [&key](const auto& entry) { return entry.first == key; });
			if (!known) {
				registration.debugKeys.emplace_back(key, value.kind);
			}
		}
	}
	LogInfo(std::string("[") + kLogTag + "] Loaded " + std::to_string(g_definitions.size()) +
			" missions from configuration (" + std::to_string(g_events.size()) + " event types).");
}

// --- phrase language: <configs>/core.cfg "ServerLang" -> s2sdk ServerLanguage -> "en" -----------

std::string CoreConfigLanguage() {
	bool ok = false;
	const std::string text = ReadTextFile(CoreConfigPath(), &ok);
	if (!ok) {
		return {};
	}
	const std::optional<KvNode> core = ParseKvDocument(text);
	if (!core) {
		return {};
	}
	return std::string(core->GetString("ServerLang", ""));
}

std::string ServerLanguage() {
	// A plain config read (s2sdk's CoreConfig), so it works at plugin start - no map is needed.
	if (__s2sdk_GetServerLanguage == nullptr) {
		return {};
	}
	try {
		return ToStd(s2sdk::GetServerLanguage());
	} catch (...) {
		return {};
	}
}

std::string DetectPhraseLanguage() {
	if (const std::string core = CoreConfigLanguage(); !core.empty()) {
		return NormalizeLanguage(core);
	}
	if (const std::string server = ServerLanguage(); !server.empty()) {
		return NormalizeLanguage(server);
	}
	return "en";
}

void LoadPhrases() {
	g_phrases.clear();
	g_phraseLanguage = DetectPhraseLanguage();
	if (g_phraseLanguage.empty()) {
		g_phraseLanguage = "en";
	}
	const std::string path = ResolvePhrasesPath(kPhrasesFile);
	bool ok = false;
	const std::string text = ReadTextFile(path, &ok);
	if (!ok) {
		LogError(std::string("[") + kLogTag + "] cannot read " + path + " - phrase keys are printed as-is");
		return;
	}
	if (!ParsePhrasesText(text, g_phraseLanguage, g_phrases)) {
		LogError(std::string("[") + kLogTag + "] malformed phrase file " + path + " - phrase keys are printed as-is");
		g_phrases.clear();
	}
}

std::string Phrase(std::string_view key) {
	return LookupPhrase(g_phrases, std::string(key));
}

std::string FormatKey(std::string_view key, const std::vector<std::string>& args) {
	return FormatPhrase(Phrase(key), args);
}

std::string WithPrefix(const std::string& text) {
	return Phrase("k4.general.prefix") + " " + text;
}

std::string NowString() {
	return FormatDateTime(std::time(nullptr));
}

// ---------------------------------------------------------------------------------------------
// database (plgdb): the section of <configs>/k4_missions/databases.cfg, async migration
// ---------------------------------------------------------------------------------------------

void EnsureDatabase() {
	// The connection of the original lived in SwiftlyS2's database.json under the name in
	// DatabaseConnection. This port reads the Pisex databases.cfg section named after the plugin,
	// like admin_system/levels_ranks, with "driver"/"sqlite_path" as the port's extensions.
	plgdb::Config database;
	database.driver = plgdb::Driver::MySQL;
	database.database = kPluginName;

	bool haveConfig = false;
	bool unsupportedDriver = false;
	const std::string configPath = ResolveDatabasesPath();
	const std::optional<std::string> text = ReadKvFile(configPath);
	if (!text) {
		LogError(std::string("[") + kLogTag + "] cannot read '" + configPath +
				 "' - falling back to a local SQLite database");
	} else {
		const std::optional<KvNode> root = ParseKvDocument(*text);
		if (!root) {
			LogError(std::string("[") + kLogTag + "] malformed '" + configPath +
					 "' - falling back to a local SQLite database");
		} else if (const KvNode* entry = root->Find(kPluginName)) {
			haveConfig = true;
			const std::string driver = std::string(entry->GetString("driver", "mysql"));
			if (driver == "sqlite") {
				database.driver = plgdb::Driver::SQLite;
			} else if (driver != "mysql" && driver != "mariadb") {
				// The original also accepted PostgreSQL; plgdb does not.
				LogError(std::string("[") + kLogTag + "] database driver '" + driver +
						 "' is not supported (MySQL/MariaDB and SQLite only) - missions will not persist");
				unsupportedDriver = true;
			}
			database.host = std::string(entry->GetString("host", "127.0.0.1"));
			database.port = static_cast<uint16_t>(std::max(1, std::min(65535, entry->GetInt("port", 3306))));
			database.user = std::string(entry->GetString("user", ""));
			database.password = std::string(entry->GetString("pass", ""));
			database.database = std::string(entry->GetString("database", kPluginName));
			if (database.driver == plgdb::Driver::SQLite) {
				std::string sqlitePath = std::string(entry->GetString("sqlite_path", "k4_missions.sqlite3"));
				if (sqlitePath.empty()) {
					sqlitePath = "k4_missions.sqlite3";
				}
				database.sqlitePath = ResolveDataPath(sqlitePath);
			}
		}
	}
	if (!haveConfig && !unsupportedDriver) {
		// Fail soft: one local SQLite file keeps the plugin usable without a databases.cfg, the
		// same fallback admin_system and levels_ranks use.
		LogError(std::string("[") + kLogTag + "] no '" + kPluginName + "' entry in " + configPath +
				 " - falling back to a local SQLite database");
		database.driver = plgdb::Driver::SQLite;
		database.sqlitePath = ResolveDataPath("k4_missions.sqlite3");
	}

	if (database.driver == plgdb::Driver::SQLite && database.sqlitePath != ":memory:") {
		// plugify does not necessarily create <data>/<plugin>; SQLite would fail to open there.
		std::error_code ec;
		const std::filesystem::path parent = std::filesystem::path(database.sqlitePath).parent_path();
		if (!parent.empty()) {
			std::filesystem::create_directories(parent, ec);
			if (ec) {
				LogError(std::string("[") + kLogTag + "] cannot create the data directory '" + parent.string() +
						 "': " + ec.message());
			}
		}
	}

	if (unsupportedDriver) {
		return;
	}

	g_database = plgdb::Database::Create(database);
	if (!g_database) {
		LogError(std::string("[") + kLogTag + "] the database configuration is invalid - missions will not persist");
		return;
	}
	// The connection and the CREATE/ALTER migration run on plgdb's worker thread: OnPluginStart
	// must never wait on the network (a blackholed MySQL would hold the main thread for seconds and
	// trip the engine watchdog). If the database is down, StartSchemaMigration() keeps trying and
	// players connect without missions until it succeeds instead of for the whole uptime.
	StartSchemaMigration();
}

void StartSchemaMigration() {
	if (!g_database || g_store || g_schemaAttemptRunning || g_schemaRetryArmed) {
		return;
	}
	g_schemaAttemptRunning = true;
	EnsureSchemaAsync(*g_database, [](bool ok, std::string error) {
		g_schemaAttemptRunning = false;
		if (!g_running || !g_database) {
			return; // unloaded while the migration was in flight
		}
		if (ok) {
			g_store = std::make_unique<MissionStore>(*g_database);
			g_schemaFailures = 0;
			g_schemaFailureLogged = false;
			if (!error.empty()) {
				LogWarning(std::string("[") + kLogTag + "] " + error);
			}
			LogInfo(std::string("[") + kLogTag + "] database ready (table k4_missions).");
			LoadPendingPlayers();
			return;
		}
		++g_schemaFailures;
		if (!g_schemaFailureLogged) {
			// Log once: the retries below would otherwise repeat the same error every minute.
			g_schemaFailureLogged = true;
			LogError(std::string("[") + kLogTag + "] " + error +
					 " - missions are unavailable until the database is reachable; retrying in the background");
		}
		g_schemaRetryArmed = true;
		g_schemaRetryCountdown = SchemaRetryDelay(g_schemaFailures);
	});
}

void LoadPendingPlayers() {
	if (!g_store || !g_running) {
		return;
	}
	// Players who joined while the schema migration was pending (or before it recovered).
	std::vector<uint64_t> pending;
	for (const auto& [steamid, record] : g_players) {
		if (!record.loaded) {
			pending.push_back(steamid);
		}
	}
	for (const uint64_t steamid : pending) {
		const auto record = g_players.find(steamid);
		if (record != g_players.end()) {
			RequestPlayerLoad(steamid, record->second.generation);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// players
// ---------------------------------------------------------------------------------------------

PlayerRecord* FindBySlot(int32_t slot) {
	if (!SlotInRange(slot)) {
		return nullptr;
	}
	const uint64_t steamid = g_slotSteam[static_cast<std::size_t>(slot)];
	if (steamid == 0) {
		return nullptr;
	}
	const auto record = g_players.find(steamid);
	return record == g_players.end() ? nullptr : &record->second;
}

PlayerRecord* FindBySteam(uint64_t steamid) {
	const auto record = g_players.find(steamid);
	return record == g_players.end() ? nullptr : &record->second;
}

// The record's slot still belongs to this player and to this controller entity. The SteamID and
// the controller handle are the identity every deferred action and database callback carries;
// with no map up the controller cannot be queried, and the caller only touches in-memory state.
bool RecordIdentityCurrent(const PlayerRecord& record) {
	if (record.steamid == 0 || !SlotInRange(record.slot)) {
		return false;
	}
	if (g_slotSteam[static_cast<std::size_t>(record.slot)] != record.steamid) {
		return false;
	}
	if (!g_mapGate.Ready()) {
		return true;
	}
	const int32_t handle = ControllerHandle(record.slot);
	return handle != 0 && handle == record.controllerHandle;
}

// The full "this is still the same, live player" test used before anything is shown or paid.
bool IsRecordConnected(const PlayerRecord& record) {
	if (!g_mapGate.Ready() || record.steamid == 0 || !SlotInRange(record.slot)) {
		return false;
	}
	return ClientSteamID64(record.slot) == record.steamid && !IsFakeClientSlot(record.slot) &&
		   RecordIdentityCurrent(record);
}

// True when `slot` still belongs to `steamid` and to the controller `controllerHandle` captured
// with the deferred action (the slot may have been reused meanwhile).
bool SameIdentity(int32_t slot, uint64_t steamid, int32_t controllerHandle) {
	if (!g_mapGate.Ready() || !IsRealPlayer(slot) || steamid == 0 || controllerHandle == 0) {
		return false;
	}
	if (ClientSteamID64(slot) != steamid) {
		return false;
	}
	return ControllerHandle(slot) == controllerHandle;
}

int FindSlotOfSteam(uint64_t steamid) {
	if (!g_mapGate.Ready() || steamid == 0) {
		return -1;
	}
	const int maxSlots = MaxPlayers();
	for (int32_t slot = 0; slot < maxSlots; ++slot) {
		if (!IsRealPlayer(slot)) {
			continue;
		}
		if (ClientSteamID64(slot) == steamid) {
			return slot;
		}
	}
	return -1;
}

int ActivePlayerCount() {
	int count = 0;
	for (auto& [steamid, record] : g_players) {
		(void)steamid;
		if (IsRecordConnected(record) && PlayerTeam(record.slot) > 1) {
			++count;
		}
	}
	return count;
}

void RefreshVip(PlayerRecord& record) {
	if (!IsRecordConnected(record)) {
		return;
	}
	bool vipStatus = false;
	if (g_vip != nullptr) {
		try {
			// The port task: our VIP core decides, not SwiftlyS2's permission flags.
			vipStatus = g_vip->VIP_IsClientVIP(record.slot);
		} catch (...) {
			vipStatus = false;
		}
	}
	if (!vipStatus && !g_config.vipNameDomain.empty()) {
		vipStatus = IsVipByName(ClientName(record.slot), g_config.vipNameDomain);
	}
	record.vip = vipStatus;
}

bool HasMissionPermission(uint64_t steamid, const std::string& flag) {
	if (__permissions_HasPermission == nullptr) {
		// The permissions plugin is an optional dependency. The original's PlayerHasPermission
		// answered false for unknown permission systems too.
		return false;
	}
	try {
		return permissions::HasPermission(steamid, ToPlg(flag)) == permissions::Status::Allow;
	} catch (...) {
		return false;
	}
}

void SchedulePlayerLoadRetry(uint64_t steamid, uint64_t generation, int attempt) {
	if (!g_store) {
		return;
	}
	PlayerLoadRetry retry;
	retry.steamid = steamid;
	retry.generation = generation;
	retry.attempt = attempt;
	retry.delay = PlayerLoadRetryDelay(attempt);
	g_loadRetries.push_back(retry);
}

bool HasPendingLoadRetry(uint64_t steamid) {
	for (const PlayerLoadRetry& retry : g_loadRetries) {
		if (retry.steamid == steamid) {
			return true;
		}
	}
	return false;
}

void ProcessPlayerLoadRetries(float deltaSeconds) {
	if (g_loadRetries.empty()) {
		return;
	}
	std::vector<PlayerLoadRetry> due;
	for (auto it = g_loadRetries.begin(); it != g_loadRetries.end();) {
		it->delay -= deltaSeconds;
		if (it->delay > 0.0f) {
			++it;
			continue;
		}
		due.push_back(*it);
		it = g_loadRetries.erase(it);
	}
	for (const PlayerLoadRetry& retry : due) {
		const auto record = g_players.find(retry.steamid);
		if (!g_running || !g_store || record == g_players.end() || record->second.generation != retry.generation
			|| record->second.loaded) {
			continue;
		}
		if (!RecordIdentityCurrent(record->second)) {
			continue;
		}
		RequestPlayerLoad(retry.steamid, retry.generation, retry.attempt);
	}
}

void RequestPlayerLoad(uint64_t steamid, uint64_t generation, int attempt) {
	if (!g_store) {
		return;
	}
	g_store->LoadPlayer(steamid, [steamid, generation, attempt](bool ok, std::vector<Mission> missions) {
		OnPlayerDataLoaded(steamid, generation, ok, std::move(missions), attempt);
	});
}

void EnsurePlayer(int32_t slot, uint64_t steamid) {
	if (!SlotInRange(slot) || steamid == 0 || !g_mapGate.Ready()) {
		return;
	}
	const int32_t controllerHandle = ControllerHandle(slot);
	if (controllerHandle == 0) {
		// Without a controller entity the player cannot be addressed later; the once-per-second
		// reconcile retries once it exists.
		return;
	}
	const auto existing = g_players.find(steamid);
	if (existing != g_players.end()) {
		// The record followed the player to a new slot (a missed disconnect, a reconnect while the
		// old record is still alive): drop the old mapping, but only while it still belongs to this
		// player - the slot may already have been reused by someone else.
		const int32_t previous = existing->second.slot;
		if (previous >= 0 && previous < kMaxSlots && previous != slot
			&& g_slotSteam[static_cast<std::size_t>(previous)] == steamid) {
			g_slotSteam[static_cast<std::size_t>(previous)] = 0;
		}
		existing->second.slot = slot;
		existing->second.controllerHandle = controllerHandle;
		g_slotSteam[static_cast<std::size_t>(slot)] = steamid;
		return;
	}

	PlayerRecord record;
	record.steamid = steamid;
	record.slot = slot;
	record.controllerHandle = controllerHandle;
	record.generation = ++g_nextGeneration;
	const uint64_t generation = record.generation;
	g_players.emplace(steamid, std::move(record));
	g_slotSteam[static_cast<std::size_t>(slot)] = steamid;

	if (g_store) {
		RequestPlayerLoad(steamid, generation);
		return;
	}
	if (g_database) {
		// The schema migration is still pending: the record stays unloaded and the migration
		// callback loads it once the table exists.
		return;
	}
	// The original's DatabaseService when disabled: an empty mission list.
	OnPlayerDataLoaded(steamid, generation, true, {}, 0);
}

void ReconcilePlayers(bool prune) {
	if (!g_mapGate.Ready()) {
		return;
	}
	// s2sdk's OnClientAuthenticated is not reliable in this stack; player_activate covers normal
	// joins, and this reconcile covers a `plg reload`, a missed event and (with prune) a missed
	// player_disconnect. It is the perm_connector pattern.
	const int maxSlots = MaxPlayers();
	for (int32_t slot = 0; slot < maxSlots; ++slot) {
		if (!IsRealPlayer(slot)) {
			continue;
		}
		const uint64_t steamid = ClientSteamID64(slot);
		if (steamid == 0) {
			continue;
		}
		const auto record = g_players.find(steamid);
		if (record == g_players.end()) {
			EnsurePlayer(slot, steamid);
		} else {
			if (record->second.slot >= 0 && record->second.slot < kMaxSlots
				&& record->second.slot != slot
				&& g_slotSteam[static_cast<std::size_t>(record->second.slot)] == steamid) {
				g_slotSteam[static_cast<std::size_t>(record->second.slot)] = 0;
			}
			record->second.slot = slot;
			record->second.controllerHandle = ControllerHandle(slot);
			g_slotSteam[static_cast<std::size_t>(slot)] = steamid;
		}
	}

	if (!prune || g_players.empty()) {
		return;
	}
	std::vector<uint64_t> stale;
	for (const auto& [steamid, record] : g_players) {
		(void)record;
		if (FindSlotOfSteam(steamid) < 0) {
			stale.push_back(steamid);
		}
	}
	for (const uint64_t steamid : stale) {
		const auto record = g_players.find(steamid);
		if (record == g_players.end()) {
			continue;
		}
		if (record->second.loaded && g_store) {
			g_store->SaveMissions(record->second.missions);
		}
		if (record->second.slot >= 0 && record->second.slot < kMaxSlots
			&& g_slotSteam[static_cast<std::size_t>(record->second.slot)] == steamid) {
			g_slotSteam[static_cast<std::size_t>(record->second.slot)] = 0;
		}
		g_players.erase(record);
	}
}

// ---------------------------------------------------------------------------------------------
// missions
// ---------------------------------------------------------------------------------------------

void OnPlayerDataLoaded(uint64_t steamid, uint64_t generation, bool ok, std::vector<Mission> missions,
						int attempt) {
	if (!g_running) {
		return;
	}
	const auto record = g_players.find(steamid);
	if (record == g_players.end() || record->second.generation != generation) {
		return; // the player left (or reconnected) while the query was in flight
	}
	if (!RecordIdentityCurrent(record->second)) {
		// The slot no longer belongs to this player and controller: the rows stay in the database
		// for the next session and the stale record is pruned by the next reconcile.
		return;
	}
	if (!ok) {
		// A failed SELECT is not "this player has no missions": leaving the record loaded with an
		// empty list would hide the stored rows for the whole session. Retry in the background.
		if (attempt == 0) {
			LogWarning(std::string("[") + kLogTag + "] could not load the missions of " + std::to_string(steamid) +
					   " - retrying in the background");
		}
		SchedulePlayerLoadRetry(steamid, generation, attempt + 1);
		return;
	}

	const std::time_t now = std::time(nullptr);
	std::vector<Mission> kept;
	std::vector<int64_t> dropped;
	if (g_config.resetMode == ResetMode::PerMap) {
		// Deviation (documented): PerMap missions belong to one map. Rows written before a restart
		// (or by another server of the same database) must not survive the map they were rolled on,
		// so a load in PerMap mode keeps nothing and the normal pass re-rolls.
		for (const Mission& mission : missions) {
			if (mission.id > 0) {
				dropped.push_back(mission.id);
			}
		}
	} else {
		kept.reserve(missions.size());
		for (Mission& mission : missions) {
			if (RecordExpired(mission, now)) {
				// The original removes a stale row while loading the player's missions.
				if (mission.id > 0) {
					dropped.push_back(mission.id);
				}
				continue;
			}
			kept.push_back(std::move(mission));
		}
	}
	if (g_store && !dropped.empty()) {
		g_store->RemoveMissions(dropped);
	}
	record->second.missions = std::move(kept);
	record->second.loaded = true;

	// The original marks the player loaded on the main thread and then, on the next world update,
	// re-checks VIP status and the mission count.
	const uint64_t capturedGeneration = generation;
	const int32_t capturedHandle = record->second.controllerHandle;
	g_deferred.push_back([steamid, capturedGeneration, capturedHandle] {
		PlayerRecord* current = FindBySteam(steamid);
		if (current == nullptr || current->generation != capturedGeneration) {
			return;
		}
		if (!IsRecordConnected(*current) || current->controllerHandle != capturedHandle) {
			return;
		}
		RefreshVip(*current);
		EnsureCorrectMissionCount(*current);
	});
}

void NotifyNewMissions(const PlayerRecord& record, int count) {
	const std::string command = g_config.missionCommands.empty() ? "missions" : g_config.missionCommands.front();
	ChatMessage(record.slot, WithPrefix(FormatKey("k4.missions.new_mission", {std::to_string(count), command})));
}

void OnMissionInserted(const std::shared_ptr<AssignmentBatch>& batch, int64_t id, Mission mission) {
	if (batch->mapGeneration != g_mapGeneration) {
		// A mission rolled on the previous map (PerMap reset): ResetMissionsOnMapChange() could not
		// see its id because the INSERT was still in flight, so drop the row now instead of letting
		// it re-enter the new map's mission list. pendingInserts was zeroed by the reset - a new
		// batch already counts on its own callbacks, so this stale one must not touch it.
		if (id > 0 && g_store) {
			g_store->RemoveMissions({id});
		}
		return;
	}
	if (batch->remaining > 0) {
		--batch->remaining;
	}
	const auto record = g_players.find(batch->steamid);
	if (record != g_players.end() && record->second.generation == batch->generation
		&& record->second.controllerHandle == batch->controllerHandle) {
		if (record->second.pendingInserts > 0) {
			--record->second.pendingInserts;
		}
		if (id > 0) {
			mission.id = id;
			record->second.missions.push_back(std::move(mission));
			++batch->added;
		}
	}

	if (batch->remaining == 0 && batch->added > 0) {
		const auto current = g_players.find(batch->steamid);
		if (current != g_players.end() && current->second.generation == batch->generation
			&& current->second.controllerHandle == batch->controllerHandle && current->second.loaded
			&& IsRecordConnected(current->second)) {
			NotifyNewMissions(current->second, batch->added);
		}
	}
}

void AssignRandomMissions(PlayerRecord& record, int count) {
	if (count <= 0) {
		return;
	}
	const uint64_t steamid = record.steamid;
	const uint64_t generation = record.generation;
	const int32_t controllerHandle = record.controllerHandle;
	const std::function<bool(const std::string&)> hasPermission = [steamid](const std::string& flag) {
		return HasMissionPermission(steamid, flag);
	};
	std::vector<std::size_t> available = AvailableMissionIndices(g_definitions, record.missions, hasPermission);
	if (available.empty()) {
		return;
	}

	const std::string expires = CalculateExpirationDate(g_config.resetMode, std::time(nullptr));
	auto batch = std::make_shared<AssignmentBatch>();
	batch->steamid = steamid;
	batch->generation = generation;
	batch->mapGeneration = g_mapGeneration;
	batch->controllerHandle = controllerHandle;

	std::mt19937 random(static_cast<uint32_t>(std::random_device {}()));
	for (int index = 0; index < count && !available.empty(); ++index) {
		std::uniform_int_distribution<std::size_t> distribution(0, available.size() - 1);
		const std::size_t position = distribution(random);
		const std::size_t definitionIndex = available[position];
		available.erase(available.begin() + static_cast<std::ptrdiff_t>(position));

		Mission mission = CreateMission(g_definitions[definitionIndex], expires);
		++batch->remaining;
		++record.pendingInserts;
		if (g_store) {
			g_store->InsertMission(steamid, mission, [batch, mission](int64_t id) mutable {
				OnMissionInserted(batch, id, std::move(mission));
			});
		} else {
			// The original's AddMissionAsync returned -1 with a disabled database: the mission is
			// not added and the player is not told about it.
			g_deferred.push_back([batch, mission]() mutable {
				OnMissionInserted(batch, -1, std::move(mission));
			});
		}
	}
}

void EnsureCorrectMissionCount(PlayerRecord& record) {
	if (!record.loaded) {
		return;
	}
	const int required = MissionAmountForVip(g_config, record.vip);
	const int current = static_cast<int>(record.missions.size()) + record.pendingInserts;
	if (current > required) {
		int excess = current - required;
		excess = std::min<int>(excess, static_cast<int>(record.missions.size()));
		if (excess > 0) {
			RemoveExcessMissions(record, excess);
		}
		return;
	}
	if (current < required) {
		AssignRandomMissions(record, required - current);
	}
}

void RemoveExcessMissions(PlayerRecord& record, int count) {
	std::vector<std::size_t> order(record.missions.size());
	std::iota(order.begin(), order.end(), std::size_t {0});
	std::sort(order.begin(), order.end(), [&record](std::size_t left, std::size_t right) {
		return record.missions[left].id > record.missions[right].id; // the original orders by Id desc
	});

	std::vector<bool> remove(record.missions.size(), false);
	std::vector<int64_t> ids;
	for (int index = 0; index < count && index < static_cast<int>(order.size()); ++index) {
		const std::size_t position = order[static_cast<std::size_t>(index)];
		remove[position] = true;
		if (record.missions[position].id > 0) {
			ids.push_back(record.missions[position].id);
		}
	}
	std::vector<Mission> kept;
	kept.reserve(record.missions.size());
	for (std::size_t index = 0; index < record.missions.size(); ++index) {
		if (!remove[index]) {
			kept.push_back(std::move(record.missions[index]));
		}
	}
	record.missions = std::move(kept);
	if (g_store) {
		g_store->RemoveMissions(ids);
	}
}

void ProcessEvent(PlayerRecord& record, std::string_view className, std::string_view target,
				  const EventPropertyLookup* lookup) {
	if (ActivePlayerCount() < g_config.minimumPlayers) {
		return;
	}
	if (!record.loaded || !IsRecordConnected(record)) {
		return;
	}
	const std::vector<std::size_t> reached =
		AdvanceMissionProgress(record.missions, className, target, g_currentMap, lookup);
	for (const std::size_t index : reached) {
		if (index < record.missions.size()) {
			CompleteMission(record.steamid, record.missions[index].id);
		}
	}
}

void ProcessPlayTime() {
	if (ActivePlayerCount() < g_config.minimumPlayers) {
		return;
	}
	// The original ProcessPlayTime: every loaded, valid player gains one minute on each
	// Event == "PlayTime" mission. MapName/EventProperties are not consulted here in the original
	// either.
	for (auto& [steamid, record] : g_players) {
		if (!record.loaded || !IsRecordConnected(record)) {
			continue;
		}
		const std::vector<std::size_t> reached = AdvancePlayTimeProgress(record.missions);
		for (const std::size_t index : reached) {
			if (index < record.missions.size()) {
				CompleteMission(steamid, record.missions[index].id);
			}
		}
	}
}

void CompleteMission(uint64_t steamid, int64_t missionId) {
	PlayerRecord* record = FindBySteam(steamid);
	if (record == nullptr) {
		return;
	}
	Mission* mission = nullptr;
	for (Mission& candidate : record->missions) {
		if (candidate.id == missionId) {
			mission = &candidate;
			break;
		}
	}
	if (mission == nullptr || mission->completed) {
		return;
	}
	mission->completed = true;

	if (!g_store || missionId <= 0) {
		return; // the original only rewards a mission whose database row was marked completed
	}
	g_store->MarkCompleted(missionId, [steamid, missionId](bool completed) {
		if (!completed) {
			LogWarning(std::string("[") + kLogTag + "] could not mark a mission completed in the database (id " +
					   std::to_string(missionId) + ")");
			return;
		}
		g_deferred.push_back([steamid, missionId] { RunCompletion(steamid, missionId); });
	});
}

void RunCompletion(uint64_t steamid, int64_t missionId) {
	PlayerRecord* record = FindBySteam(steamid);
	if (record == nullptr || !IsRecordConnected(*record)) {
		return;
	}
	Mission* mission = nullptr;
	for (Mission& candidate : record->missions) {
		if (candidate.id == missionId) {
			mission = &candidate;
			break;
		}
	}
	if (mission == nullptr) {
		return;
	}

	// The original executes every RewardCommands entry as a server command after replacing the
	// placeholders.
	const PlayerPlaceholders placeholders = Placeholders(*record);
	for (const std::string& command : mission->rewardCommands) {
		const std::string expanded = ExpandRewardCommand(command, placeholders);
		if (__s2sdk_ServerCommand != nullptr) {
			try {
				s2sdk::ServerCommand(ToPlg(expanded));
			} catch (...) {
			}
		} else {
			LogWarning(std::string("[") + kLogTag + "] ServerCommand binding is missing; reward not executed: " + expanded);
		}
	}

	// Notify the player.
	ChatMessage(record->slot, WithPrefix(FormatKey("k4.missions.complete_mission",
												   {mission->phrase, mission->rewardPhrase})));

	// Webhooks (completion + all missions completed).
	SendCompletionWebhook(*record, *mission);

	// ResetService.OnMissionCompleted: Instant mode replaces the completed mission.
	if (g_config.resetMode == ResetMode::Instant) {
		const uint64_t capturedSteamid = steamid;
		const int64_t capturedMission = missionId;
		g_deferred.push_back([capturedSteamid, capturedMission] {
			PlayerRecord* current = FindBySteam(capturedSteamid);
			if (current == nullptr || !IsRecordConnected(*current)) {
				return;
			}
			RemoveMission(capturedSteamid, capturedMission);
			EnsureCorrectMissionCount(*current);
		});
	}
}

void RemoveMission(uint64_t steamid, int64_t missionId) {
	PlayerRecord* record = FindBySteam(steamid);
	if (record == nullptr) {
		return;
	}
	const auto position = std::find_if(record->missions.begin(), record->missions.end(),
									   [missionId](const Mission& mission) { return mission.id == missionId; });
	if (position == record->missions.end()) {
		return;
	}
	record->missions.erase(position);
	if (g_store && missionId > 0) {
		g_store->RemoveMissions({missionId});
	}
}

void HandleExpiredMissions(uint64_t steamid) {
	PlayerRecord* record = FindBySteam(steamid);
	if (record == nullptr || !IsRecordConnected(*record)) {
		return;
	}
	const std::time_t now = std::time(nullptr);
	const auto removed = std::erase_if(record->missions, [now](const Mission& mission) {
		return RecordExpired(mission, now);
	});
	if (removed == 0) {
		return;
	}
	ChatMessage(record->slot, WithPrefix(Phrase("k4.missions.dailyreset")));
	EnsureCorrectMissionCount(*record);
}

void CheckExpiredMissions() {
	if (!g_store || !g_mapGate.Ready() || !g_running) {
		return;
	}
	const std::string now = NowString();
	const std::time_t nowTime = std::time(nullptr);
	g_store->PlayersWithExpiredMissions(now, [now, nowTime](bool ok, std::vector<uint64_t> steamids) {
		if (!g_running) {
			return;
		}
		if (!ok) {
			// A failed SELECT is not "no rows expired on any server". Running the in-memory pass
			// below with the database unreachable would erase the player's expired missions and
			// drop every replacement INSERT (OnMissionInserted ignores a non-positive id), leaving
			// the player with no missions; the next 60-second tick retries the query instead.
			return;
		}
		// A database shared by two servers: CleanupExpiredMissions() deletes the rows globally, so
		// the server whose timer fires second gets an empty list even though its own online players
		// still hold expired missions in memory. Find those here as well, or they keep the stale
		// missions until the next reconnect/map change.
		for (const auto& [steamid, record] : g_players) {
			(void)record;
			if (std::find(steamids.begin(), steamids.end(), steamid) != steamids.end()) {
				continue;
			}
			const bool expired = std::any_of(record.missions.begin(), record.missions.end(),
											 [nowTime](const Mission& mission) { return RecordExpired(mission, nowTime); });
			if (expired) {
				steamids.push_back(steamid);
			}
		}
		if (steamids.empty()) {
			return;
		}
		if (g_store) {
			g_store->CleanupExpiredMissions(now);
		}
		for (const uint64_t steamid : steamids) {
			g_deferred.push_back([steamid] { HandleExpiredMissions(steamid); });
		}
		SendResetWebhook();
	});
}

void SaveAllPlayers() {
	if (!g_store) {
		return;
	}
	for (auto& [steamid, record] : g_players) {
		(void)steamid;
		if (record.loaded && !record.missions.empty()) {
			g_store->SaveMissions(record.missions);
		}
	}
}

void ResetMissionsOnMapChange() {
	// Deviation: the original's ResetMode.PerMap is documented as "missions reset on map change"
	// but v1.0.6 never removes them (CalculateExpirationDate returns null and no map hook clears
	// the list). The port implements the documented behaviour: every assigned mission is removed
	// and re-rolled on the new map; the common VIP/count pass that follows assigns the new ones.
	// The generation bump is what invalidates INSERTs that were still in flight when the map
	// changed (OnMissionInserted deletes those rows and ignores their callbacks).
	++g_mapGeneration;
	for (auto& [steamid, record] : g_players) {
		(void)steamid;
		if (!record.loaded) {
			continue;
		}
		if (g_store) {
			g_store->RemoveMissions(MissionIds(record.missions));
		}
		record.missions.clear();
		// No assignment can have started for the new map yet (the reset runs before the count pass
		// of HandleMapStarted), so every pending insert belongs to the old generation.
		record.pendingInserts = 0;
	}
}

// ---------------------------------------------------------------------------------------------
// chat / player placeholders
// ---------------------------------------------------------------------------------------------

void ChatMessage(int32_t slot, const std::string& text) {
	// PrintToChat() applies the {TAG} colour conversion of the phrases.
	if (!g_mapGate.Ready() || !SlotInRange(slot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	if (!ClientExists(slot)) {
		return;
	}
	try {
		s2sdk::PrintToChat(slot, ToPlg(ColorizeChat(text)));
	} catch (...) {
	}
}

PlayerPlaceholders Placeholders(const PlayerRecord& record) {
	PlayerPlaceholders placeholders;
	placeholders.slot = record.slot;
	// SwiftlyS2's IPlayer.PlayerID is the engine user id, i.e. the player's controller entity
	// index (which is also the "userid" a CS2 game event carries). The entity index equals
	// slot + 1 on a normal server, but the accessor is the correct source; a controller handle is
	// a signed CEntityHandle and must go through EntHandleToEntIndex (never `handle > 0`).
	const int32_t controller = ControllerEntityIndex(record.slot);
	placeholders.userid = controller > 0 ? controller : record.slot + 1;
	placeholders.name = ClientName(record.slot);
	placeholders.steamid64 = record.steamid;
	return placeholders;
}

// ---------------------------------------------------------------------------------------------
// webhook
// ---------------------------------------------------------------------------------------------

void SendCompletionWebhook(const PlayerRecord& record, const Mission& mission) {
	if (!g_webhook.Enabled() || !IsRecordConnected(record)) {
		return;
	}
	const std::string name = ClientName(record.slot);
	const std::string steamid = std::to_string(record.steamid);
	const std::string profile =
		"[" + name + "](https://steamcommunity.com/profiles/" + steamid + ")";
	g_webhook.Send("complete.json", {{"name", name},
									 {"steamid", steamid},
									 {"profile", profile},
									 {"mission", Phrase(mission.phrase)},
									 {"reward", Phrase(mission.rewardPhrase)}});

	const bool allCompleted = !record.missions.empty()
							  && std::all_of(record.missions.begin(), record.missions.end(),
											 [](const Mission& entry) { return entry.completed; });
	if (allCompleted) {
		SendAllMissionsCompletedWebhook(record);
	}
}

void SendAllMissionsCompletedWebhook(const PlayerRecord& record) {
	if (!g_webhook.Enabled() || !IsRecordConnected(record)) {
		return;
	}
	const std::string name = ClientName(record.slot);
	const std::string steamid = std::to_string(record.steamid);
	const std::string profile =
		"[" + name + "](https://steamcommunity.com/profiles/" + steamid + ")";
	g_webhook.Send("complete_all.json", {{"name", name},
										 {"steamid", steamid},
										 {"profile", profile},
										 {"total_missions", std::to_string(record.missions.size())}});
}

void SendResetWebhook() {
	if (!g_webhook.Enabled()) {
		return;
	}
	std::string nextReset = CalculateExpirationDate(g_config.resetMode, std::time(nullptr));
	if (nextReset.empty()) {
		nextReset = FormatDateTime(std::time(nullptr) + 24 * 60 * 60);
	}
	g_webhook.Send("reset.json", {{"reset_mode", ResetModeName(g_config.resetMode)},
								  {"next_reset", nextReset}});
}

// ---------------------------------------------------------------------------------------------
// game events
// ---------------------------------------------------------------------------------------------

bool IsWarmup() {
	if (g_vip == nullptr) {
		return false;
	}
	try {
		// The VIP core reads CCSGameRules::m_bWarmupPeriod through the schema (the same field the
		// original's Core.EntitySystem.GetGameRules().WarmupPeriod returned).
		return g_vip->VIP_WarmupPeriod();
	} catch (...) {
		return false;
	}
}

bool EventBool(void* event, const std::string& key) {
	if (event == nullptr || __s2sdk_GetEventBool == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetEventBool(event, ToPlg(key));
	} catch (...) {
		return false;
	}
}

int32_t EventInt(void* event, const std::string& key) {
	if (event == nullptr || __s2sdk_GetEventInt == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetEventInt(event, ToPlg(key));
	} catch (...) {
		return 0;
	}
}

float EventFloat(void* event, const std::string& key) {
	if (event == nullptr || __s2sdk_GetEventFloat == nullptr) {
		return 0.0f;
	}
	try {
		return s2sdk::GetEventFloat(event, ToPlg(key));
	} catch (...) {
		return 0.0f;
	}
}

std::string EventString(void* event, const std::string& key) {
	if (event == nullptr || __s2sdk_GetEventString == nullptr) {
		return {};
	}
	try {
		return ToStd(s2sdk::GetEventString(event, ToPlg(key)));
	} catch (...) {
		return {};
	}
}

// Whether that field of that event is a "player_pawn" reference rather than a player slot. The
// gameevents dump marks bomb_pickup.userid as player_pawn (every other player field the missions
// use is player_controller / player_controller_and_pawn, which GetEventPlayerSlot resolves).
bool IsPawnPlayerField(const std::string& eventName, const std::string& field) {
	static const std::pair<std::string_view, std::string_view> kPawnFields[] = {
		{"bomb_pickup", "userid"},
	};
	for (const auto& [event, name] : kPawnFields) {
		if (eventName == event && field == name) {
			return true;
		}
	}
	return false;
}

// The player slot of an event field: s2sdk::GetEventPlayerSlot, and only for a pawn-typed field of
// a pawn event the map-gated GetEventPlayerPawn + ClientPtrToPlayerSlot fallback. Never reads a
// player reference with GetEventInt.
int32_t EventPlayerSlot(void* event, const std::string& field, bool pawnField) {
	if (event == nullptr) {
		return -1;
	}
	if (__s2sdk_GetEventPlayerSlot != nullptr) {
		int32_t slot = -1;
		try {
			slot = s2sdk::GetEventPlayerSlot(event, ToPlg(field));
		} catch (...) {
			slot = -1;
		}
		if (SlotInRange(slot)) {
			return slot;
		}
	}
	if (!pawnField || !g_mapGate.Ready()) {
		return -1;
	}
	if (__s2sdk_GetEventPlayerPawn == nullptr || __s2sdk_ClientPtrToPlayerSlot == nullptr) {
		return -1;
	}
	try {
		void* pawn = s2sdk::GetEventPlayerPawn(event, ToPlg(field));
		if (pawn == nullptr) {
			return -1;
		}
		if (__s2sdk_IsValidEntPointer != nullptr && !s2sdk::IsValidEntPointer(pawn)) {
			return -1;
		}
		const int32_t slot = s2sdk::ClientPtrToPlayerSlot(pawn);
		return SlotInRange(slot) ? slot : -1;
	} catch (...) {
		return -1;
	}
}

void LogEventProperties(const EventRegistration& registration, const EventPropertyLookup& lookup) {
	// Deviation: the original reflected over every property of the event type; s2sdk exposes no
	// property list, so the debug log covers the properties the missions filter on.
	for (const auto& [key, kind] : registration.debugKeys) {
		EventValue value;
		if (!lookup(key, kind, value)) {
			continue;
		}
		std::string text;
		switch (kind) {
		case ValueKind::Bool: text = value.boolean ? "true" : "false"; break;
		case ValueKind::Int: text = std::to_string(value.integer); break;
		case ValueKind::Double: text = std::to_string(value.number); break;
		case ValueKind::String: text = value.text; break;
		case ValueKind::Unsupported: continue;
		}
		LogInfo(std::string("[") + registration.className + "] " + key + ": " + text);
	}
}

void HandleMissionEvent(const std::string& eventName, const EventRegistration& registration, void* raw) {
	const EventPropertyLookup lookup = [raw](std::string_view key, ValueKind kind, EventValue& out) {
		// Mission JSON uses the SwiftlyS2 property names ("Weapon", "Headshot", "DmgHealth"); the
		// engine event keys are the snake_case forms ("weapon", "headshot", "dmg_health").
		const std::string engineKey = PascalToSnake(key);
		switch (kind) {
		case ValueKind::Bool:
			out.boolean = EventBool(raw, engineKey);
			return true;
		case ValueKind::Int:
			out.integer = EventInt(raw, engineKey);
			return true;
		case ValueKind::Double:
			out.number = EventFloat(raw, engineKey);
			return true;
		case ValueKind::String:
			out.text = EventString(raw, engineKey);
			return true;
		case ValueKind::Unsupported:
			return false;
		}
		return false;
	};

	if (g_config.eventDebugLogs) {
		LogEventProperties(registration, lookup);
	}

	for (const std::string& target : registration.targets) {
		const std::string key = LowerAscii(target);
		const int32_t slot = EventPlayerSlot(raw, key, IsPawnPlayerField(eventName, key));
		if (slot < 0) {
			continue;
		}
		PlayerRecord* record = FindBySlot(slot);
		if (record == nullptr) {
			continue;
		}
		ProcessEvent(*record, registration.className, target, &lookup);
	}
}

void HandleRoundEnd(void* raw) {
	// round_end.winner is a numeric field, not a player reference: GetEventInt is the correct read.
	const int32_t winner = EventInt(raw, "winner");
	if (winner <= 1) { // Team.Spectator
		return;
	}
	for (auto& [steamid, record] : g_players) {
		(void)steamid;
		if (!IsRecordConnected(record)) {
			continue;
		}
		const int team = PlayerTeam(record.slot);
		if (team <= 1) {
			continue;
		}
		const std::string_view target = team == winner ? "winner" : "loser";
		// The original passes a null property dictionary for round_end, so EventProperties
		// filters are ignored here on purpose.
		ProcessEvent(record, "EventRoundEnd", target, nullptr);
	}
}

void HandlePlayerActivate(void* raw) {
	const int32_t slot = EventPlayerSlot(raw, "userid", false);
	if (!SlotInRange(slot)) {
		return;
	}
	if (!IsRealPlayer(slot)) {
		return;
	}
	const uint64_t steamid = ClientSteamID64(slot);
	if (steamid == 0) {
		return;
	}
	EnsurePlayer(slot, steamid);
}

void HandlePlayerDisconnect(void* raw) {
	const int32_t slot = EventPlayerSlot(raw, "userid", false);
	if (!SlotInRange(slot)) {
		return;
	}
	// No client query here: the slot may already be gone. The SteamID is the one recorded when
	// the player was registered.
	const uint64_t steamid = g_slotSteam[static_cast<std::size_t>(slot)];
	g_slotSteam[static_cast<std::size_t>(slot)] = 0;
	if (steamid == 0) {
		return;
	}
	const auto record = g_players.find(steamid);
	if (record == g_players.end()) {
		return;
	}
	if (record->second.loaded && g_store) {
		g_store->SaveMissions(record->second.missions);
	}
	g_players.erase(record);
}

void HandleGameEvent(const std::string& name, void* raw) {
	// Game events keep arriving during a changelevel. Between OnMapEnd and the next MapStart every
	// client/entity/schema access can crash (PITFALLS B2), and both the warmup read (game rules)
	// and the event's player resolution are such accesses. Ignoring the event in that window loses
	// nothing: the player records are saved by the round-end handler and the next minute-tick
	// prune/save, and progress cannot be earned without a running map anyway.
	if (!g_running || !g_mapGate.Ready()) {
		return;
	}
	const bool roundEnd = name == "round_end";
	const auto registration = g_events.find(name);
	if (registration != g_events.end()) {
		if (!g_config.allowProgressDuringWarmup && IsWarmup()) {
			// The original's warmup protection: no mission progress in warmup.
		} else if (roundEnd) {
			HandleRoundEnd(raw);
		} else {
			HandleMissionEvent(name, registration->second, raw);
		}
	}

	if (name == "player_activate") {
		HandlePlayerActivate(raw);
	} else if (name == "player_disconnect") {
		HandlePlayerDisconnect(raw);
	}

	if (roundEnd) {
		// The original's OnRoundEndSave (a second HookPost on the same event).
		SaveAllPlayers();
	}
}

s2sdk::ResultType OnGameEventTrampoline(const plg::string& name, void* event, bool /*dontBroadcast*/) {
	try {
		HandleGameEvent(ToStd(name), event);
	} catch (const std::exception& exception) {
		LogError(std::string("[") + kLogTag + "] game event handler failed: " + exception.what());
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] game event handler failed with an unknown exception");
	}
	// Never cancel or modify an event (the task's rule); Post callers only observe.
	return s2sdk::ResultType::Continue;
}

void RegisterEventHooks() {
	g_hookedEvents.clear();
	// Hook and unhook are only useful as a pair: a hook that cannot be removed would point into
	// this library after unload (PITFALLS A7).
	if (__s2sdk_HookEvent == nullptr || __s2sdk_UnhookEvent == nullptr) {
		LogError(std::string("[") + kLogTag + "] the s2sdk HookEvent/UnhookEvent pair is unavailable - "
				 "mission progress is disabled");
		return;
	}
	auto hook = [](const std::string& gameEvent, const std::string& className) {
		if (std::find(g_hookedEvents.begin(), g_hookedEvents.end(), gameEvent) != g_hookedEvents.end()) {
			return;
		}
		try {
			if (s2sdk::HookEvent(ToPlg(gameEvent), &OnGameEventTrampoline, s2sdk::HookMode::Post)
				== s2sdk::EventHookError::Okay) {
				g_hookedEvents.push_back(gameEvent);
			} else {
				LogWarning(std::string("[") + kLogTag + "] Event type " + className + " not found.");
			}
		} catch (...) {
			LogWarning(std::string("[") + kLogTag + "] Event type " + className + " could not be hooked.");
		}
	};

	// The map of mission events is unordered; sorting the names keeps the registration order (and
	// the log) deterministic across reloads.
	std::vector<std::string> missionEvents;
	missionEvents.reserve(g_events.size());
	for (const auto& [gameEvent, registration] : g_events) {
		(void)registration;
		missionEvents.push_back(gameEvent);
	}
	std::sort(missionEvents.begin(), missionEvents.end());
	for (const std::string& gameEvent : missionEvents) {
		const auto registration = g_events.find(gameEvent);
		if (registration != g_events.end()) {
			hook(gameEvent, registration->second.className);
		}
	}
	// The original's RegisterEventHandlers(): player lifecycle and the round-end save. They are
	// HookPost in the original too.
	hook("player_activate", "EventPlayerActivate");
	hook("player_disconnect", "EventPlayerDisconnect");
	hook("round_end", "EventRoundEnd");
}

void UnregisterEventHooks() {
	if (__s2sdk_UnhookEvent != nullptr) {
		for (const std::string& name : g_hookedEvents) {
			try {
				s2sdk::UnhookEvent(ToPlg(name), &OnGameEventTrampoline, s2sdk::HookMode::Post);
			} catch (...) {
			}
		}
	}
	g_hookedEvents.clear();
}

// ---------------------------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------------------------

s2sdk::ResultType OnMissionsCommand(int32_t caller, s2sdk::ConCommandContext context,
									const plg::vector<plg::string>& /*args*/) {
	const bool fromChat = context == s2sdk::ConCommandContext::Chat;
	try {
		// The server console is strictly caller == -1 and this is a player menu command: it is
		// refused, as is every other caller outside the slot range (an invalid -2 included).
		if (caller < 0 || caller >= kMaxSlots) {
			return fromChat ? s2sdk::ResultType::Handled : s2sdk::ResultType::Continue;
		}
		if (!g_running || !g_mapGate.Ready()) {
			return fromChat ? s2sdk::ResultType::Handled : s2sdk::ResultType::Continue;
		}
		PlayerRecord* record = FindBySlot(caller);
		if (record == nullptr || !record->loaded || !IsRecordConnected(*record)) {
			return fromChat ? s2sdk::ResultType::Handled : s2sdk::ResultType::Continue;
		}
		// The menu is created from OnPluginUpdate, outside the concommand dispatcher's lock; the
		// identity captured here is re-checked when it is actually displayed.
		const uint64_t steamid = record->steamid;
		const int32_t controllerHandle = record->controllerHandle;
		g_deferred.push_back([caller, steamid, controllerHandle] {
			OpenMissionsMenu(caller, steamid, controllerHandle);
		});
		return s2sdk::ResultType::Handled;
	} catch (const std::exception& exception) {
		LogError(std::string("[") + kLogTag + "] command handler failed: " + exception.what());
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] command handler failed with an unknown exception");
	}
	// Never propagate into the engine: a throwing handler still owns the command.
	return s2sdk::ResultType::Handled;
}

void RegisterCommands() {
	g_commandNames.clear();
	g_foreignCommands.clear();
	g_commandsRegistered = false;
	if (g_config.missionCommands.empty()) {
		return;
	}
	// The command, its removal and the listener removal are required together: a command whose
	// listener cannot be detached (or whose own entry cannot be removed) would keep pointing into
	// this library after unload (PITFALLS A7/C11).
	if (__s2sdk_AddConsoleCommand == nullptr || __s2sdk_RemoveCommand == nullptr
		|| __s2sdk_RemoveCommandListener == nullptr) {
		LogError(std::string("[") + kLogTag + "] the s2sdk AddConsoleCommand/RemoveCommand/"
				 "RemoveCommandListener set is incomplete - the mission commands are not registered");
		return;
	}
	// Pisex's RegCommand registered every console name as a client-executable ConCommand; the chat
	// spelling "!<name>" reaches the very same callback through s2sdk's chat-trigger path (C11).
	const int64_t flagBits = static_cast<int64_t>(s2sdk::ConVarFlag::Release)
							 | static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute);
	for (const std::string& name : g_config.missionCommands) {
		if (name.empty()) {
			continue;
		}
		// false = the name already existed (the engine or another plugin) and s2sdk only attached a
		// listener to it: OnPluginEnd must then detach the listener and never RemoveCommand a
		// command this plugin did not create (PITFALLS C11).
		const bool created = s2sdk::AddConsoleCommand(ToPlg(name), "k4_missions",
													  static_cast<s2sdk::ConVarFlag>(flagBits),
													  &OnMissionsCommand, s2sdk::HookMode::Pre);
		g_commandNames.push_back(name);
		if (!created) {
			g_foreignCommands.push_back(name);
		}
	}
	g_commandsRegistered = true;
}

void UnregisterCommands() {
	if (!g_commandsRegistered) {
		g_commandNames.clear();
		g_foreignCommands.clear();
		return;
	}
	for (const std::string& name : g_commandNames) {
		const bool foreign = std::find(g_foreignCommands.begin(), g_foreignCommands.end(), name)
							 != g_foreignCommands.end();
		try {
			if (foreign) {
				if (__s2sdk_RemoveCommandListener != nullptr) {
					s2sdk::RemoveCommandListener(ToPlg(name), &OnMissionsCommand, s2sdk::HookMode::Pre);
				}
			} else if (__s2sdk_RemoveCommand != nullptr) {
				s2sdk::RemoveCommand(ToPlg(name), &OnMissionsCommand);
			}
		} catch (...) {
		}
	}
	g_commandNames.clear();
	g_foreignCommands.clear();
	g_commandsRegistered = false;
}

// ---------------------------------------------------------------------------------------------
// menus (PITFALLS C10)
// ---------------------------------------------------------------------------------------------

bool IsMenuTypeRegisteredName(const std::string& name) {
	if (__s2sdk_IsMenuTypeRegistered == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsMenuTypeRegistered(ToPlg(name));
	} catch (...) {
		return false;
	}
}

// The player's own "!menu" choice from the optional hudmenu plugin (empty without it).
std::string PlayerMenuTypeChoice(int32_t slot) {
	if (__hudmenu_GetClientMenuType == nullptr || !g_mapGate.Ready() || !SlotInRange(slot)) {
		return {};
	}
	try {
		return ToStd(hudmenu::GetClientMenuType(slot));
	} catch (...) {
		return {};
	}
}

// pisexcompat's ResolveS2sdkMenuTypeName, table preserved exactly. The previous port never forced a
// menu type, so it always used the HUD_LAYOUT mapping:
//   * the player's own "!menu" choice from the optional hudmenu plugin:
//       "chat"/"centerhtml"/"button" -> that builtin type;
//       "hud"  -> "hud" when registered, otherwise "button" (never another custom type);
//       "wasd" -> "wasd" when registered, otherwise "button" (never "hud");
//       anything else -> the HUD_LAYOUT default below;
//   * HUD_LAYOUT default -> "hud" when registered, otherwise "button" (the wasd backend is never
//     used as a fallback for HUD_LAYOUT).
// Empty = the menu subsystem is disabled (not even the builtin button type exists).
std::string ResolveMenuType(int32_t slot) {
	const bool hudRegistered = IsMenuTypeRegisteredName("hud");
	const bool wasdRegistered = IsMenuTypeRegisteredName("wasd");
	if (!hudRegistered && !wasdRegistered && !IsMenuTypeRegisteredName("button")) {
		return {};
	}

	const std::string choice = PlayerMenuTypeChoice(slot);
	if (!choice.empty()) {
		if (choice == "chat" || choice == "centerhtml" || choice == "button") {
			return choice; // always-registered builtins
		}
		if (choice == "hud") {
			return hudRegistered ? std::string("hud") : std::string("button");
		}
		if (choice == "wasd") {
			return wasdRegistered ? std::string("wasd") : std::string("button");
		}
		// An unknown/stale answer falls through to the HUD_LAYOUT default.
	}
	return hudRegistered ? std::string("hud") : std::string("button");
}

bool MenuBindingsAvailable() {
	return __s2sdk_CreateMenu != nullptr && __s2sdk_AddMenuItem != nullptr && __s2sdk_DisplayMenu != nullptr
		   && __s2sdk_DestroyMenu != nullptr && __s2sdk_GetClientMenu != nullptr
		   && __s2sdk_CancelClientMenu != nullptr && __s2sdk_SetMenuType != nullptr
		   && __s2sdk_GetMenuType != nullptr && __s2sdk_IsMenuTypeRegistered != nullptr;
}

bool MenuSessionBindingsAvailable() {
	return __s2sdk_GetClientMenu != nullptr && __s2sdk_CancelClientMenu != nullptr
		   && __s2sdk_DestroyMenu != nullptr;
}

void QueueMenuDestroy(s2sdk::MenuId id) {
	// Menu id 0 is the reserved "no menu" id: the plugin keeps the dummy menu that owns it until
	// OnPluginEnd and destroys it there.
	if (id == 0) {
		return;
	}
	if (std::find(g_menuDestroyQueue.begin(), g_menuDestroyQueue.end(), id) != g_menuDestroyQueue.end()) {
		return;
	}
	g_menuDestroyQueue.push_back(id);
}

void DestroyMenuNow(s2sdk::MenuId id) {
	if (id == 0 || __s2sdk_DestroyMenu == nullptr) {
		return;
	}
	try {
		if (__s2sdk_IsValidMenu == nullptr || s2sdk::IsValidMenu(id)) {
			s2sdk::DestroyMenu(id);
		}
	} catch (...) {
	}
}

// A menu type whose backend Close() has no client side effect (s2sdk's builtin menu types: the chat
// and console close callbacks are empty). Used when a display session refuses to end between maps:
// destroying the menu would otherwise run the custom backend's close (hudmenu's HideForSlot) over
// the client layer that is being torn down (PITFALLS B2).
bool SwitchMenuToSafeBuiltinType(s2sdk::MenuId id) {
	if (id == 0 || __s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr) {
		return false;
	}
	for (const char* const typeName : {"chat", "console"}) {
		try {
			if (!s2sdk::SetMenuType(id, ToPlg(typeName))) {
				continue;
			}
			if (ToStd(s2sdk::GetMenuType(id)) == typeName) {
				return true; // switched and verified
			}
		} catch (...) {
		}
	}
	return false;
}

// Ends every display session of |id| so that DestroyMenu cannot run a custom backend's close over a
// dying client layer (the map-down/unload path):
//   1. CancelClientMenu(Disconnect) for each slot that currently displays the menu. Even a false
//      answer is re-checked with GetClientMenu (the session may have ended anyway).
//   2. If the menu is still displayed, switch its type to a builtin whose Close is a no-op and
//      verify with GetMenuType; DestroyMenu then invokes that empty close.
// Returns false when the display cannot be ended safely (A7_UNRESOLVED): the caller must not
// destroy the menu then.
bool EndMenuSessionsForSafeDestroy(s2sdk::MenuId id) {
	if (id == 0) {
		return true;
	}
	if (__s2sdk_GetClientMenu == nullptr || __s2sdk_CancelClientMenu == nullptr) {
		LogError("A7_UNRESOLVED: menu " + std::to_string(id) +
				 " cannot be checked/cancelled (s2sdk session bindings missing); not destroying it");
		return false;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (s2sdk::GetClientMenu(slot) != id) {
				continue;
			}
			s2sdk::CancelClientMenu(slot, s2sdk::MenuCancelReason::Disconnect);
			// CancelClientMenu's answer is not trusted on its own: a session that is still
			// displayed after the call must get the builtin-type fallback below.
			if (s2sdk::GetClientMenu(slot) != id) {
				continue;
			}
		} catch (...) {
			// A throwing query is treated as "still displayed".
		}
		if (!SwitchMenuToSafeBuiltinType(id)) {
			LogError("A7_UNRESOLVED: menu " + std::to_string(id) +
					 " is still displayed but its backend cannot be switched to a builtin type; not destroying it");
			return false;
		}
	}
	return true;
}

// The handler of the mission menu. It runs under the menu manager's lock, so it never touches the
// menu API and never runs an engine command: every outcome is queued for OnPluginUpdate.
void OnReservedMenuHandler(s2sdk::MenuId, s2sdk::MenuAction, int32_t, int32_t) {}

void OnMissionsMenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot, int32_t param) {
	try {
		if (!g_running || !SlotInRange(slot)) {
			return;
		}
		OpenMenu& menu = g_openMenus[static_cast<std::size_t>(slot)];
		switch (action) {
		case s2sdk::MenuAction::Start:
			return;
		case s2sdk::MenuAction::Select: {
			if (!menu.active || menu.id != id) {
				return; // a stale session (the slot was reused or the menu replaced)
			}
			if (param < 0 || static_cast<std::size_t>(param) >= menu.missionIds.size()) {
				return;
			}
			const int64_t missionId = menu.missionIds[static_cast<std::size_t>(param)];
			if (missionId <= 0) {
				return; // a disabled/informational item
			}
			const uint64_t steamid = menu.steamid;
			const int32_t controllerHandle = menu.controllerHandle;
			g_deferred.push_back([slot, steamid, controllerHandle, missionId] {
				PrintMissionDetails(slot, steamid, controllerHandle, missionId);
			});
			return; // the menu stays open after a selection (SetMenuCloseOnSelect(false))
		}
		case s2sdk::MenuAction::Cancel: {
			// The exit/back callback values of the original ("exit"/"back") only closed the menu;
			// the session is over, the id has to be destroyed or its MenuData leaks.
			if (menu.active && menu.id == id) {
				menu = OpenMenu {};
			}
			QueueMenuDestroy(id);
			return;
		}
		case s2sdk::MenuAction::End:
			// The display session is fully closed; End always follows Cancel (and is the only event
			// for a disconnect or a destroy).
			if (menu.active && menu.id == id) {
				menu = OpenMenu {};
			}
			QueueMenuDestroy(id);
			return;
		}
	} catch (...) {
	}
}

// Destroys the menus whose display ended. Runs from OnPluginUpdate (outside every menu handler),
// and only with a map up: between maps DestroyMenu would reach the client layer through the menu
// type backend, so those ids wait for the next map (or OnPluginEnd, where nothing is displayed).
void ProcessMenuDestroyQueue() {
	if (!g_mapGate.Ready() || g_menuDestroyQueue.empty() || __s2sdk_DestroyMenu == nullptr) {
		return;
	}
	std::vector<s2sdk::MenuId> ids;
	ids.swap(g_menuDestroyQueue);
	for (const s2sdk::MenuId id : ids) {
		if (id == 0) {
			continue;
		}
		try {
			if (__s2sdk_IsValidMenu != nullptr && !s2sdk::IsValidMenu(id)) {
				continue;
			}
			s2sdk::DestroyMenu(id);
		} catch (...) {
		}
	}
}

// s2sdk's client-menu state uses id 0 for "no menu", but 0 is also a valid CreateMenu() answer
// (ids start at 0). The plugin burns the id with a menu nobody can display, exactly like
// pisexcompat's BurnMenuIdZero(); the dummy is destroyed in OnPluginEnd.
bool EnsureMenuIdZeroBurned() {
	if (g_menuIdZeroChecked) {
		return true;
	}
	g_menuIdZeroChecked = true;
	if (__s2sdk_CreateMenu == nullptr || __s2sdk_DestroyMenu == nullptr) {
		return false;
	}
	try {
		const s2sdk::MenuId id = s2sdk::CreateMenu(ToPlg("k4_missions:reserved-id-0"),
												   &OnReservedMenuHandler, ToPlg("chat"));
		if (id == 0) {
			g_menuIdZeroReserved = true;
		} else {
			// Another plugin already burned id 0: the dummy is not needed.
			s2sdk::DestroyMenu(id);
		}
		return true;
	} catch (...) {
		return false;
	}
}

void CloseMenusAtMapEnd() {
	// Every display session is ended first with the Disconnect reason (s2sdk then skips the menu
	// backend's close callback, which would reach the client layer being torn down), and the menu is
	// freed right after: no menu survives a map change. Without the session bindings the destroy is
	// queued for the next map instead of risking a backend close here.
	std::vector<s2sdk::MenuId> ids;
	ids.reserve(static_cast<std::size_t>(kMaxSlots) + g_menuDestroyQueue.size());
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		OpenMenu& menu = g_openMenus[static_cast<std::size_t>(slot)];
		if (!menu.active) {
			continue;
		}
		ids.push_back(menu.id);
		menu = OpenMenu {};
	}
	ids.insert(ids.end(), g_menuDestroyQueue.begin(), g_menuDestroyQueue.end());
	g_menuDestroyQueue.clear();
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

	const bool sessionsAvailable = MenuSessionBindingsAvailable();
	std::vector<s2sdk::MenuId> retry;
	for (const s2sdk::MenuId id : ids) {
		if (id == 0) {
			continue;
		}
		if (!sessionsAvailable) {
			retry.push_back(id);
			continue;
		}
		if (!EndMenuSessionsForSafeDestroy(id)) {
			// A7_UNRESOLVED (already logged): the menu is still displayed and cannot be given a
			// builtin no-op Close. Keep it for the next map start / OnPluginEnd instead of running
			// the custom backend's close over the dying client layer.
			retry.push_back(id);
			continue;
		}
		DestroyMenuNow(id);
	}
	// The Cancel/End callbacks the destroys just delivered may have queued the same ids again; the
	// ids that were destroyed are dropped, the unresolved ones stay queued for a retry.
	g_menuDestroyQueue = std::move(retry);
}

void CloseMenusAtUnload() {
	std::vector<s2sdk::MenuId> ids;
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		OpenMenu& menu = g_openMenus[static_cast<std::size_t>(slot)];
		if (!menu.active) {
			continue;
		}
		ids.push_back(menu.id);
		menu = OpenMenu {};
	}
	ids.insert(ids.end(), g_menuDestroyQueue.begin(), g_menuDestroyQueue.end());
	g_menuDestroyQueue.clear();
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

	const bool sessionsAvailable = MenuSessionBindingsAvailable();
	for (const s2sdk::MenuId id : ids) {
		if (id == 0) {
			continue;
		}
		if (sessionsAvailable && !EndMenuSessionsForSafeDestroy(id)) {
			continue; // A7_UNRESOLVED, already logged: the menu stays instead of a dying close
		}
		DestroyMenuNow(id);
	}
	// The reserved id 0 dummy: nobody can display it, its handler is about to be unloaded.
	if (g_menuIdZeroReserved) {
		g_menuIdZeroReserved = false;
		if (__s2sdk_DestroyMenu != nullptr) {
			try {
				s2sdk::DestroyMenu(0);
			} catch (...) {
			}
		}
	}
	g_menuIdZeroChecked = false;
}

// Builds and displays the mission menu. Runs from OnPluginUpdate, outside the menu manager's lock;
// the identity captured when the command was handled is checked again here.
void DisplayMissionsMenu(int32_t slot, const PlayerRecord& record) {
	if (!g_running || !g_mapGate.Ready() || !IsRealPlayer(slot)) {
		return;
	}
	if (!MenuBindingsAvailable()) {
		if (!g_menuUnavailableLogged) {
			g_menuUnavailableLogged = true;
			LogError("the s2sdk menu bindings are incomplete (CreateMenu/AddMenuItem/DisplayMenu/DestroyMenu/"
					 "GetClientMenu/CancelClientMenu/SetMenuType/GetMenuType/IsMenuTypeRegistered) - "
					 "the mission menu is disabled");
		}
		return;
	}
	const std::string menuType = ResolveMenuType(slot);
	if (menuType.empty()) {
		if (!g_menuUnavailableLogged) {
			g_menuUnavailableLogged = true;
			LogError("no s2sdk menu type is registered (menus are disabled in the s2sdk config) - "
					 "the mission menu is disabled");
		}
		return;
	}
	// Only burn id 0 when the display can actually happen; a disabled menu subsystem must not leave
	// a reserved menu behind.
	if (!EnsureMenuIdZeroBurned()) {
		LogError("could not reserve the s2sdk menu id 0 - the mission menu is disabled");
		return;
	}

	std::vector<MenuItemSpec> items;
	const std::string title = Phrase("k4.missions.menu.title");
	if (ActivePlayerCount() < g_config.minimumPlayers) {
		MenuItemSpec item;
		item.text = "{LIGHTRED}⚠ " + FormatKey("k4.missions.menu.playerlimit", {std::to_string(g_config.minimumPlayers)});
		item.style = s2sdk::MenuItemStyle::Disabled;
		items.push_back(std::move(item));
	}
	if (const std::string resetInfo = ResetInfo(record); !resetInfo.empty()) {
		MenuItemSpec item;
		item.text = "{BLUE}⏱ " + resetInfo;
		item.style = s2sdk::MenuItemStyle::Disabled;
		items.push_back(std::move(item));
	}

	if (record.missions.empty()) {
		MenuItemSpec item;
		item.text = "{GRAY}" + Phrase("k4.missions.no_missions");
		item.style = s2sdk::MenuItemStyle::Disabled;
		items.push_back(std::move(item));
	} else {
		int counter = 1;
		for (const Mission& mission : record.missions) {
			const std::string missionLabel = FormatKey("k4.missions.menu.mission", {std::to_string(counter)});
			std::string text;
			if (mission.completed) {
				text = "{GREEN}" + missionLabel + " | ✓";
			} else {
				int percent = 100;
				if (mission.amount > 0) {
					percent = static_cast<int>(static_cast<float>(mission.progress) /
											   static_cast<float>(mission.amount) * 100.0f);
				}
				text = "{WHITE}" + missionLabel + " | " + std::to_string(percent) + "%";
			}
			MenuItemSpec item;
			// The info value carries the database id, not the list position: a reset can happen
			// while the menu is open.
			item.info = std::to_string(mission.id);
			item.text = std::move(text);
			item.style = s2sdk::MenuItemStyle::Default;
			item.missionId = mission.id;
			items.push_back(std::move(item));
			++counter;
		}

		// Locked VIP slots (the original's menu builder).
		if (!record.vip && g_config.missionAmountVip > g_config.missionAmountNormal) {
			for (int index = counter; index <= g_config.missionAmountVip; ++index) {
				MenuItemSpec item;
				item.text = "{GRAY}🔒 " + FormatKey("k4.missions.menu.mission", {std::to_string(index)})
							+ " - " + Phrase("k4.missions.menu.vip_locked");
				item.style = s2sdk::MenuItemStyle::Disabled;
				items.push_back(std::move(item));
			}
		}
	}

	// The s2sdk menu backends (and hudmenu) render titles and item texts verbatim - only chat
	// output goes through ColorizeChat(). Strip the tags here (like plugins/k4_guilds' AttachMenu
	// and as_maps) so the same phrases read correctly in chat and in every menu type.
	// Replace an older display of this slot. Its handler gets Interrupted/End and queues the
	// destroy; forgetting the state first keeps the stale events from touching the new menu.
	if (g_openMenus[static_cast<std::size_t>(slot)].active) {
		QueueMenuDestroy(g_openMenus[static_cast<std::size_t>(slot)].id);
		g_openMenus[static_cast<std::size_t>(slot)] = OpenMenu {};
	}

	s2sdk::MenuId id = 0;
	try {
		id = s2sdk::CreateMenu(ToPlg(StripColorTags(title)), &OnMissionsMenuHandler, ToPlg(menuType));
		if (id == 0) {
			LogError("s2sdk returned the reserved menu id 0 for the mission menu of slot " +
					 std::to_string(slot));
			return;
		}
		OpenMenu state;
		state.active = true;
		state.id = id;
		state.steamid = record.steamid;
		state.controllerHandle = record.controllerHandle;
		state.missionIds.reserve(items.size());
		for (const MenuItemSpec& item : items) {
			s2sdk::AddMenuItem(id, ToPlg(item.info), ToPlg(StripColorTags(item.text)), item.style);
			state.missionIds.push_back(item.missionId);
		}
		if (__s2sdk_SetMenuPagination != nullptr) {
			s2sdk::SetMenuPagination(id, kMenuItemsPerPage); // chat pages of five, like cs2-menus
		}
		if (__s2sdk_SetMenuExitButton != nullptr) {
			s2sdk::SetMenuExitButton(id, true); // the original's SetExitMenu(hMenu, true)
		}
		if (__s2sdk_SetMenuExitBackButton != nullptr) {
			s2sdk::SetMenuExitBackButton(id, false); // the original's SetBackMenu(hMenu, false)
		}
		if (__s2sdk_SetMenuCloseOnSelect != nullptr) {
			// Pisex keeps the menu open after a selection; the callback decides what happens next.
			s2sdk::SetMenuCloseOnSelect(id, false);
		}
		g_openMenus[static_cast<std::size_t>(slot)] = std::move(state);

		if (!s2sdk::DisplayMenu(id, slot, kMenuTimeoutSeconds)) {
			LogError("s2sdk could not display the mission menu on slot " + std::to_string(slot));
			g_openMenus[static_cast<std::size_t>(slot)] = OpenMenu {};
			QueueMenuDestroy(id);
		}
	} catch (const std::exception& exception) {
		LogError(std::string("could not create the mission menu: ") + exception.what());
		g_openMenus[static_cast<std::size_t>(slot)] = OpenMenu {};
		QueueMenuDestroy(id);
	} catch (...) {
		LogError("could not create the mission menu");
		g_openMenus[static_cast<std::size_t>(slot)] = OpenMenu {};
		QueueMenuDestroy(id);
	}
}

std::string ResetInfo(const PlayerRecord& record) {
	if (g_config.resetMode == ResetMode::PerMap) {
		return Phrase("k4.missions.menu.reset.map");
	}
	if (g_config.resetMode == ResetMode::Instant) {
		return Phrase("k4.missions.menu.reset.instant");
	}
	if (record.missions.empty() || !record.missions.front().hasExpiresAt) {
		return {};
	}
	const Countdown countdown = TimeUntilExpiration(record.missions.front().expiresAt, std::time(nullptr));
	if (g_config.resetMode == ResetMode::Weekly || g_config.resetMode == ResetMode::Monthly) {
		return FormatKey("k4.missions.menu.reset.days",
						 {std::to_string(countdown.days), std::to_string(countdown.hours),
						  std::to_string(countdown.minutes)});
	}
	return FormatKey("k4.missions.menu.reset.time",
					 {std::to_string(countdown.hours), std::to_string(countdown.minutes)});
}

void OpenMissionsMenu(int32_t slot, uint64_t steamid, int32_t controllerHandle) {
	if (!g_running || !g_mapGate.Ready() || !IsRealPlayer(slot)) {
		return;
	}
	// Re-validate at fire time: the command was handled in an earlier frame and the slot may have
	// been reused (SteamID64 + controller CEntityHandle, PITFALLS C1/C2).
	if (!SameIdentity(slot, steamid, controllerHandle)) {
		return;
	}
	PlayerRecord* record = FindBySlot(slot);
	if (record == nullptr || record->steamid != steamid || !record->loaded) {
		return;
	}
	DisplayMissionsMenu(slot, *record);
}

void PrintMissionDetails(int32_t slot, uint64_t steamid, int32_t controllerHandle, int64_t missionId) {
	// Re-validate at fire time: the menu callback may run after the player left or the mission
	// list changed.
	if (!g_running || !g_mapGate.Ready() || !SlotInRange(slot)) {
		return;
	}
	if (!SameIdentity(slot, steamid, controllerHandle)) {
		return;
	}
	PlayerRecord* record = FindBySlot(slot);
	if (record == nullptr || record->steamid != steamid || !record->loaded) {
		return;
	}
	std::size_t missionIndex = record->missions.size();
	for (std::size_t index = 0; index < record->missions.size(); ++index) {
		if (record->missions[index].id == missionId) {
			missionIndex = index;
			break;
		}
	}
	if (missionIndex >= record->missions.size()) {
		return;
	}
	const Mission& mission = record->missions[missionIndex];
	const std::string prefix = Phrase("k4.general.prefix");
	const std::string number = std::to_string(missionIndex + 1);

	ChatMessage(slot, prefix + " " + FormatKey("k4.missions.detail.header", {number}));
	ChatMessage(slot, prefix + " " + FormatKey("k4.missions.detail.task", {mission.phrase}));
	if (mission.completed) {
		ChatMessage(slot, prefix + " " + Phrase("k4.missions.detail.completed"));
	} else {
		ChatMessage(slot, prefix + " " + FormatKey("k4.missions.detail.progress",
												   {std::to_string(mission.progress), std::to_string(mission.amount)}));
	}
	ChatMessage(slot, prefix + " " + FormatKey("k4.missions.detail.reward", {mission.rewardPhrase}));
}

// ---------------------------------------------------------------------------------------------
// map lifecycle (PITFALLS B1/B2/B3)
// ---------------------------------------------------------------------------------------------

// s2sdk::GetCurrentMap() is gpGlobals->mapname: empty before the first map / with gpGlobals null,
// and "<empty>" for a level without a name. The mid-map latch may only open the gate when this
// reports a real map - the host can pump OnPluginUpdate through the pre-map startup or through a
// reload in the changelevel window.
bool MapNameAvailable() {
	if (__s2sdk_GetCurrentMap == nullptr) {
		return false;
	}
	try {
		const std::string map = ToStd(s2sdk::GetCurrentMap());
		return !map.empty() && map != "<empty>";
	} catch (...) {
		return false;
	}
}

std::string CurrentMapName() {
	if (__s2sdk_GetCurrentMap == nullptr) {
		return {};
	}
	try {
		return ToStd(s2sdk::GetCurrentMap());
	} catch (...) {
		return {};
	}
}

void OnMapStartListener() {
	// From here on the clients and entities of the new map are in place (PITFALLS B1).
	if (!g_running) {
		return;
	}
	g_mapGate.OnMapStart();
	g_mapStartPending = true;
}

void OnMapEndListener() {
	// From here until the next map start every s2sdk client query can crash (PITFALLS B2). This
	// only clears state; the menu teardown below talks to the menu manager, never to a client
	// entity (s2sdk's Disconnect cancel skips the backend's client-facing Close).
	g_mapGate.OnMapEnd();
	g_mapStartPending = false;
	g_reconcileCountdown = kReconcileTimer;
	g_playtimeCountdown = kMinuteTimer;
	g_expirationCountdown = kMinuteTimer;
	// Pending player-load retries stay queued: they are safe across the changelevel (they are only
	// processed while the map gate is open) and dropping them left a player unloaded for the whole
	// session if their SELECT had failed just before the map change.
	CloseMenusAtMapEnd();
}

void RegisterMapListeners() {
	// The Register/Unregister pair is required together, so the teardown can always take back
	// exactly what this start registered. Without the listeners nothing could ever close the gate
	// again, so a missing binding keeps it shut instead of opening it blindly.
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapStart_Unregister == nullptr
		|| __s2sdk_OnMapEnd_Register == nullptr || __s2sdk_OnMapEnd_Unregister == nullptr) {
		LogError(std::string("[") + kLogTag + "] the s2sdk map listener Register/Unregister pair is incomplete - "
				 "client queries and the mission menu stay disabled");
		return;
	}
	try {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
		g_mapGate.SetListenersRegistered(true);
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] the s2sdk map listeners could not be registered - "
				 "client queries stay disabled");
	}
}

void UnregisterMapListeners() {
	if (!g_mapGate.ListenersRegistered()) {
		return;
	}
	g_mapGate.SetListenersRegistered(false);
	try {
		if (__s2sdk_OnMapStart_Unregister != nullptr) {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		}
	} catch (...) {
	}
	try {
		if (__s2sdk_OnMapEnd_Unregister != nullptr) {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		}
	} catch (...) {
	}
}

// The first ready update: the work the previous port did in its startup callback (commands and
// events are already registered in OnPluginStart, which touches no map state).
void StartServices() {
	if (g_started) {
		return;
	}
	g_started = true;
	g_reconcileCountdown = kReconcileTimer;
	g_playtimeCountdown = kMinuteTimer;
	g_expirationCountdown = (g_config.resetMode == ResetMode::Instant || g_config.resetMode == ResetMode::PerMap)
								? 0.0f
								: kMinuteTimer;
	ReconcilePlayers(false);
	// A migration that is still pending (a database that was down at boot) gets another attempt as
	// soon as a map is up; a no-op while one is running, armed or already done.
	StartSchemaMigration();
	// The original runs CheckForExpiredMissionsAsync() right after the database initialization,
	// not only from the 60-second timer.
	CheckExpiredMissions();
}

void HandleMapStarted() {
	if (!g_running || !g_mapGate.Ready()) {
		return;
	}
	g_currentMap = CurrentMapName();
	// The original's HandleMapStart(): restart the expiration service on the new map.
	g_expirationCountdown = (g_config.resetMode == ResetMode::Instant || g_config.resetMode == ResetMode::PerMap)
								? 0.0f
								: kMinuteTimer;
	if (g_config.resetMode == ResetMode::PerMap) {
		ResetMissionsOnMapChange();
	}
	ReconcilePlayers(false);
	for (auto& [steamid, record] : g_players) {
		if (!record.loaded) {
			// A load that was still in flight at the map change (or whose SELECT failed) must not
			// leave the player without missions for the rest of the session: ask again on the new
			// map, and let the once-a-minute pass below retry it if the request is lost.
			if (g_store && IsRecordConnected(record) && !HasPendingLoadRetry(steamid)) {
				RequestPlayerLoad(steamid, record.generation);
			}
			continue;
		}
		if (!IsRecordConnected(record)) {
			continue;
		}
		RefreshVip(record);
		EnsureCorrectMissionCount(record);
	}
}

void ProcessDeferred() {
	// A callback may queue another one; the budget keeps that from starving the frame (the old
	// pisexcompat NextFrame semantics).
	std::size_t budget = g_deferred.size();
	while (budget-- > 0 && !g_deferred.empty()) {
		std::function<void()> action = std::move(g_deferred.front());
		g_deferred.pop_front();
		if (!action) {
			continue;
		}
		try {
			action();
		} catch (const std::exception& exception) {
			LogError(std::string("[") + kLogTag + "] deferred action failed: " + exception.what());
		} catch (...) {
			LogError(std::string("[") + kLogTag + "] deferred action failed with an unknown exception");
		}
	}
}

// ---------------------------------------------------------------------------------------------
// the Plugify plugin
// ---------------------------------------------------------------------------------------------

class MissionsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			g_running = true;
			ResetRuntimeState();
			// Nothing below touches a client, an entity or the schema: file work, listeners and
			// command registration are safe before the first map (PITFALLS B1).
			LoadConfiguration();
			LoadMissionDefinitions();
			LoadPhrases();
			EnsureDatabase();
			g_webhook.Configure(JoinPath(PluginConfigDir(), kDiscordDir), g_config.webhookUrl);
			RegisterMapListeners();
			RegisterCommands();
			RegisterEventHooks();
			LogInfo(std::string("[") + kLogTag + "] loaded: " + std::to_string(g_definitions.size()) +
					" mission definitions, reset mode " + ResetModeName(g_config.resetMode) + ".");
		} catch (const std::exception& exception) {
			LogError(std::string("[") + kLogTag + "] OnPluginStart failed: " + exception.what());
		} catch (...) {
			LogError(std::string("[") + kLogTag + "] OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed: that stops every Plugify plugin (PITFALLS A1)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		try {
			// The database and the webhook queue are engine-free; their workers are pumped even
			// while no map is up (a migration that finished during a changelevel must not wait).
			if (g_database) {
				g_database->Poll();
			}
			g_webhook.Pump();
			if (!g_running) {
				return {};
			}

			float delta = dt.count() > 0 ? static_cast<float>(dt.count()) / 1000.0f : 0.0f;
			if (delta > kMaxUpdateDelta) {
				delta = kMaxUpdateDelta;
			}

			// PITFALLS B1: a plugin loaded into a running map never saw OnMapStart, so its first
			// update may be the "a map is up" signal - but only when GetCurrentMap() names a real
			// map, all four map listeners are registered and no OnMapEnd was seen since start
			// (the gate enforces all three). Once open, the latch is not consulted again.
			const bool wasReady = g_mapGate.Ready();
			g_mapGate.OnPluginUpdate(wasReady || MapNameAvailable());
			if (!wasReady && g_mapGate.Ready()) {
				g_mapStartPending = true; // a plugin loaded mid-map gets its map-start pass too
			}
			if (!g_mapGate.Ready()) {
				return {};
			}

			if (!g_started) {
				StartServices();
			}
			if (g_mapStartPending) {
				g_mapStartPending = false;
				HandleMapStarted();
			}

			ProcessDeferred();
			ProcessPlayerLoadRetries(delta);

			g_reconcileCountdown -= delta;
			if (g_reconcileCountdown <= 0.0f) {
				g_reconcileCountdown = kReconcileTimer;
				ReconcilePlayers(false);
			}

			g_playtimeCountdown -= delta;
			if (g_playtimeCountdown <= 0.0f) {
				g_playtimeCountdown = kMinuteTimer;
				ReconcilePlayers(true);
				ProcessPlayTime();
				// Safety net for a failed retry arming; a no-op while a retry is armed or the
				// store is ready.
				StartSchemaMigration();
				// Self-healing for a player whose load never completed (a lost retry): ask again
				// once a minute while they are connected.
				if (g_store) {
					for (auto& [steamid, record] : g_players) {
						if (!record.loaded && IsRecordConnected(record) && !HasPendingLoadRetry(steamid)) {
							RequestPlayerLoad(steamid, record.generation);
						}
					}
				}
			}

			if (g_expirationCountdown > 0.0f) {
				g_expirationCountdown -= delta;
				if (g_expirationCountdown <= 0.0f) {
					g_expirationCountdown = kMinuteTimer;
					CheckExpiredMissions();
				}
			}

			if (g_schemaRetryArmed) {
				g_schemaRetryCountdown -= delta;
				if (g_schemaRetryCountdown <= 0.0f) {
					g_schemaRetryArmed = false;
					StartSchemaMigration();
				}
			}

			ProcessMenuDestroyQueue();
		} catch (const std::exception& exception) {
			LogError(std::string("[") + kLogTag + "] OnPluginUpdate failed: " + exception.what());
		} catch (...) {
			LogError(std::string("[") + kLogTag + "] OnPluginUpdate failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			g_running = false;
			// Everything that was hooked or created is removed again, in the reverse order of the
			// registrations; each step is guarded on its own so one throwing teardown cannot keep
			// the rest registered (PITFALLS A7/C12).
			UnregisterEventHooks();
			UnregisterCommands();
			UnregisterMapListeners();
			CloseMenusAtUnload();
			g_mapGate.Close();

			g_deferred.clear();
			g_loadRetries.clear();
			// The original saved every player on unload. OnPluginEnd does not run on a server quit
			// (PITFALLS A6), so round-end saves carry the normal case; this flush covers `plg reload`.
			SaveAllPlayers();
			g_webhook.Shutdown();
			g_store.reset();
			g_database.reset(); // flushes the queued saves on the worker, then joins it

			g_schemaAttemptRunning = false;
			g_schemaFailures = 0;
			g_schemaFailureLogged = false;
			g_schemaRetryArmed = false;
			g_schemaRetryCountdown = 0.0f;
			g_players.clear();
			g_slotSteam.fill(0);
			g_definitions.clear();
			g_events.clear();
			g_hookedEvents.clear();
			g_phrases.clear();
			g_commandNames.clear();
			g_foreignCommands.clear();
			g_commandsRegistered = false;
			g_openMenus.fill(OpenMenu {});
			g_menuDestroyQueue.clear();
			g_currentMap.clear();
			g_started = false;
			g_mapStartPending = false;
		} catch (...) {
			// nothing left to do at unload
		}
		return {};
	}
};

} // namespace

// The Plugify entry object. Outside the anonymous namespace on purpose: the fake-runtime tests
// include this file and drive the very same instance.
MissionsPlugin g_plugin;

} // namespace k4

PLUGIFY_PLUGIN(PLUGIN_API, &k4::g_plugin)
