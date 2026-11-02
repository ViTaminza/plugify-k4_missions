// k4_missions — engine-free core of the K4-Missions port.
//
// The original (K4ryuu/K4-Missions-SwiftlyS2, C#, v1.0.6) keeps the mission model, the filter
// matching, the reset-period arithmetic, the VIP amount selection and the reward placeholder
// expansion inside its plugin classes. This header extracts exactly those pieces so they can be
// unit-tested without a server (tests/k4_missions_tests.cpp), the same way lr_core/plgdb are
// tested in this repository.
//
// Nothing here includes s2sdk, plugify or plgdb: JSON is parsed with the bundled nlohmann header,
// time is std::time, and the only external input of the matching code is an EventPropertyLookup
// callback that the plugin fills from the engine's game event.

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace k4 {

// ---------------------------------------------------------------------------------------------
// Config (config.json, section "K4Missions")
// ---------------------------------------------------------------------------------------------

enum class ResetMode {
	PerMap,
	Daily,
	Weekly,
	Monthly,
	Instant,
};

// The System.Text.Json / configuration-binder spelling of the enum ("PerMap", "Daily", ...),
// parsed case-insensitively like the .NET binder.
bool ParseResetMode(std::string_view text, ResetMode& out);
const char* ResetModeName(ResetMode mode);

struct Config {
	// Kept for configuration compatibility: the original named a SwiftlyS2 database.json
	// connection. This port reads <configs>/k4_missions/databases.cfg instead (documented).
	std::string databaseConnection = "host";

	std::vector<std::string> missionCommands = {"mission", "missions"};

	// Name substring that grants VIP status (case-insensitive). Empty = disabled.
	std::string vipNameDomain;

	int minimumPlayers = 4;
	int missionAmountNormal = 1;
	int missionAmountVip = 3;

	// Kept for configuration compatibility. The port grants VIP status through the vip core
	// (VIP_IsClientVIP) and the name domain; the original checked SwiftlyS2 permission flags.
	std::vector<std::string> vipFlags = {"vipcore.access"};

	bool eventDebugLogs = false;
	bool allowProgressDuringWarmup = false;
	ResetMode resetMode = ResetMode::Daily;
	std::string webhookUrl;
};

// Parses the whole config.json text. Returns false only when the file is not a JSON object;
// `error` is also filled with a non-fatal notice (unknown reset mode, ...) that the caller logs.
bool ParseConfig(std::string_view text, Config& out, std::string& error);

// The number of missions an assigned player holds (MissionAmountVip / MissionAmountNormal).
int MissionAmountForVip(const Config& config, bool isVip);

// ---------------------------------------------------------------------------------------------
// Game event names
// ---------------------------------------------------------------------------------------------

// "EventPlayerDeath" -> "player_death", "EventRoundMvp" -> "round_mvp". Returns an empty string
// when the name does not start with "Event" (the original's GetType() lookup failed for those).
std::string EventClassNameToEventName(std::string_view className);

// PascalCase -> engine snake_case, used for the event property names too: SwiftlyS2 exposes the
// CLR property names ("DmgHealth", "Thrusmoke"), the engine event keys are "dmg_health" /
// "thrusmoke". A name that is already lower-case / snake_case is returned unchanged.
std::string PascalToSnake(std::string_view name);

// ---------------------------------------------------------------------------------------------
// Mission definitions (missions.json) and assigned missions
// ---------------------------------------------------------------------------------------------

// The JSON value kinds the original's ComparePropertyValue() distinguishes. Unsupported is an
// object/array/null filter value: the original's switch default is `false`, so such a filter
// never matches.
enum class ValueKind {
	Bool,
	Int,
	Double,
	String,
	Unsupported,
};

struct MissionValue {
	ValueKind kind = ValueKind::Unsupported;
	bool boolean = false;
	int64_t integer = 0;
	double number = 0.0;
	std::string text;
};

struct EventValue {
	bool boolean = false;
	int64_t integer = 0;
	double number = 0.0;
	std::string text;
};

using EventProperties = std::vector<std::pair<std::string, MissionValue>>;

struct MissionDefinition {
	std::string event;
	std::string target;
	std::vector<std::string> rewardCommands;
	int amount = 0;
	std::string phrase;
	std::string rewardPhrase;
	EventProperties eventProperties;
	bool hasMapName = false;
	std::string mapName;
	bool hasFlag = false;
	std::string flag;
};

struct Mission {
	int64_t id = -1;
	std::string event;
	std::string target;
	int amount = 0;
	std::string phrase;
	std::string rewardPhrase;
	std::vector<std::string> rewardCommands;
	EventProperties eventProperties;
	bool hasMapName = false;
	std::string mapName;
	bool hasFlag = false;
	std::string flag;
	int progress = 0;
	bool completed = false;
	// "YYYY-MM-DD HH:MM:SS" in server-local time (the C# original's DateTime.Now), empty for the
	// PerMap/Instant modes.
	bool hasExpiresAt = false;
	std::string expiresAt;

	MissionDefinition ToDefinition() const;
};

// MissionDefinition.CreatePlayerMission(): a fresh assignment with progress 0 and the given
// expiry ("YYYY-MM-DD HH:MM:SS"; empty for PerMap/Instant).
Mission CreateMission(const MissionDefinition& definition, const std::string& expiresAt);

// The duplicate-detection key of the original: "Event|Target|Amount|Phrase".
std::string MissionKey(std::string_view event, std::string_view target, int amount, std::string_view phrase);
std::string MissionKey(const MissionDefinition& mission);
std::string MissionKey(const Mission& mission);

// Parses missions.json (JSONC comments are accepted, property names are matched
// case-insensitively like System.Text.Json's PropertyNameCaseInsensitive). Returns false on a
// malformed file; `error` explains why.
bool LoadMissions(std::string_view json, std::vector<MissionDefinition>& out, std::string& error);

// Filled by the plugin from the engine game event (s2sdk GetEvent*). Returns false when the event
// has no such property;
// the s2sdk-backed lookup cannot distinguish a missing key from the getter's default (0/false/
// empty), so it always returns true (see README deviation 12).
using EventPropertyLookup = std::function<bool(std::string_view key, ValueKind kind, EventValue& out)>;

// The database's event_properties column: the original stores the Dictionary<string, JsonElement>
// as JSON. An empty filter set serializes to an empty string (the column stays NULL).
std::string SerializeEventPropertiesJson(const EventProperties& properties);
// Parses a stored JSON object back. Returns false (and clears `out`) on invalid JSON, exactly like
// the original DbMission.GetEventProperties() catch block.
bool ParseEventPropertiesJson(std::string_view json, EventProperties& out);

// True when the event property `key` exists and satisfies the mission's filter.
bool MatchesEventProperties(const EventProperties& filters, const EventPropertyLookup& lookup);

// The original PlayerMission.Matches(): completed missions never match; Event and Target compare
// case-insensitively; MapName compares exactly; a null property dictionary (round_end in the
// original) skips the filter check entirely - hence the nullable lookup.
bool MissionMatches(const Mission& mission, std::string_view event, std::string_view target,
					std::string_view currentMap, const EventPropertyLookup* lookup);

// The original's assignment filter: a definition with a Flag is only available when the
// permission check allows it, and a mission already held (same MissionKey) is not offered again.
std::vector<std::size_t> AvailableMissionIndices(const std::vector<MissionDefinition>& definitions,
												 const std::vector<Mission>& current,
												 const std::function<bool(const std::string&)>& hasPermission);

// The original PlayerManager.ProcessEventForPlayer(): every matching mission gains one progress
// point. Returns the indices of the missions that reached their amount (the caller completes them;
// this function does not change `completed`, so the decision stays with the caller).
std::vector<std::size_t> AdvanceMissionProgress(std::vector<Mission>& missions, std::string_view event,
												std::string_view target, std::string_view currentMap,
												const EventPropertyLookup* lookup);

// ProcessPlayTime() of the original: one point for every Event == "PlayTime" mission that is not
// completed, ignoring MapName and EventProperties (the original does the same).
std::vector<std::size_t> AdvancePlayTimeProgress(std::vector<Mission>& missions);

// ---------------------------------------------------------------------------------------------
// Reset periods and time formatting
// ---------------------------------------------------------------------------------------------

// The original ResetService.CalculateExpirationDate() in server-local time:
//   Daily   -> today 23:59:59
//   Weekly  -> Saturday 23:59:59, i.e. the second before the coming Sunday starts (a Sunday
//              counts as "next Sunday", +7 days) - the original's AddDays(...).AddSeconds(-1)
//   Monthly -> the last day of the current month 23:59:59
//   PerMap / Instant -> empty (no expiry stored)
// `now` is a local time_t (time(nullptr)); the returned string is "YYYY-MM-DD HH:MM:SS".
std::string CalculateExpirationDate(ResetMode mode, std::time_t now);

// Parses "YYYY-MM-DD HH:MM:SS" (also accepts 'T' as the separator and ignores a trailing
// fractional part / 'Z'), interpreted as server-local time like the C# DateTime.
std::optional<std::time_t> ParseDateTime(std::string_view text);
std::string FormatDateTime(std::time_t local);

struct Countdown {
	int days = 0;
	int hours = 0;
	int minutes = 0;
};

// The original ResetService.GetTimeUntilExpiration(): 0/0/0 once the expiry is in the past.
Countdown TimeUntilExpiration(std::string_view expiresAt, std::time_t now);

// ---------------------------------------------------------------------------------------------
// Phrases and reward placeholders
// ---------------------------------------------------------------------------------------------

// The .NET localizer's "{0}"/"{1}" substitution used by every k4.missions phrase.
std::string FormatPhrase(std::string_view phrase, const std::vector<std::string>& args);

struct PlayerPlaceholders {
	int slot = 0;
	// The engine "userid" of a CS2 player (controller index = slot + 1); SwiftlyS2's
	// IPlayer.PlayerID is the same server-side user id the event carries.
	int userid = 0;
	std::string name;
	uint64_t steamid64 = 0;
};

// The original PlayerManager.ReplacePlaceholders(): {slot}, {userid}, {name}, {steamid64},
// {steamid} and the literal "u0022" -> '"'.
std::string ExpandRewardCommand(std::string_view command, const PlayerPlaceholders& player);

// The original WebhookService.EscapeJson().
std::string JsonEscape(std::string_view value);

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

std::string LowerAscii(std::string_view text);
bool EqualsIgnoreCase(std::string_view left, std::string_view right);
bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle);

// The original Plugin.CheckVipStatus() name-domain rule.
bool IsVipByName(std::string_view playerName, std::string_view domain);

} // namespace k4
