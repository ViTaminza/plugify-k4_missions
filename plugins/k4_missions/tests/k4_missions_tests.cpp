// Engine-free tests of plugins/k4_missions/src/k4_core.{hpp,cpp}.
//
// Covers the parts of the original K4-Missions (v1.0.6) that the port extracted into the core:
//   * config.json parsing (section K4Missions, case-insensitive properties, JSONC),
//   * missions.json parsing and the shipped default file,
//   * event class name -> CS2 game event name conversion,
//   * EventProperties matching (bool exact / number >= / string contains / missing / null),
//   * mission progress accounting (including PlayTime),
//   * daily/weekly/monthly/per-map/instant reset period computation,
//   * VIP vs normal mission amount selection,
//   * reward command placeholder expansion and phrase formatting,
//   * the plugin-local KV1 reader and the phrase table/language fallback (kv1.hpp, phrases.hpp),
//   * database event_properties serialization round-trip and the store (SQLite, and MySQL when
//     ./test.sh exports MYSQL_*).
//
// Run by ./test.sh; the executable lives in build/tests.

#include "k4_core.hpp"
#include "k4_store.hpp"
#include "kv1.hpp"
#include "phrases.hpp"

#include <plgdb/db.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef K4_MISSIONS_TEST_DATA_DIR
#define K4_MISSIONS_TEST_DATA_DIR "."
#endif

namespace {

int g_checks = 0;
int g_failures = 0;

void Fail(const char* file, int line, const std::string& text) {
	++g_failures;
	std::printf("FAIL %s:%d: %s\n", file, line, text.c_str());
}

#define CHECK(condition) \
	do { \
		++g_checks; \
		if (!(condition)) { \
			Fail(__FILE__, __LINE__, #condition); \
		} \
	} while (0)

#define CHECK_EQ(actual, expected) \
	do { \
		++g_checks; \
		const auto& actualValue = (actual); \
		const auto& expectedValue = (expected); \
		if (!(actualValue == expectedValue)) { \
			std::ostringstream message; \
			message << #actual << " == " << #expected; \
			Fail(__FILE__, __LINE__, message.str()); \
		} \
	} while (0)

#define CHECK_NEAR(actual, expected, tolerance) \
	do { \
		++g_checks; \
		const double actualValue = static_cast<double>(actual); \
		const double expectedValue = static_cast<double>(expected); \
		const double delta = actualValue - expectedValue; \
		if (!(delta <= (tolerance) && delta >= -(tolerance))) { \
			std::ostringstream message; \
			message << #actual << " ~= " << #expected << " (" << actualValue << " vs " << expectedValue << ")"; \
			Fail(__FILE__, __LINE__, message.str()); \
		} \
	} while (0)

std::string AssetPath(const std::string& name) {
	return std::string(K4_MISSIONS_TEST_DATA_DIR) + "/" + name;
}

std::string ReadFile(const std::string& path) {
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		return {};
	}
	std::ostringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
}

std::time_t LocalTime(int year, int month, int day, int hour, int minute, int second) {
	std::tm tm {};
	tm.tm_year = year - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_sec = second;
	tm.tm_isdst = -1;
	return std::mktime(&tm);
}

const k4::MissionValue* FindProperty(const k4::EventProperties& properties, std::string_view key) {
	for (const auto& [name, value] : properties) {
		if (name == key) {
			return &value;
		}
	}
	return nullptr;
}

// The lookup every filter test uses: the event carries weapon/headshot/distance/penetrated.
k4::EventPropertyLookup TestLookup() {
	return [](std::string_view key, k4::ValueKind kind, k4::EventValue& out) {
		if (key == "Weapon" && kind == k4::ValueKind::String) {
			out.text = "weapon_ak47";
			return true;
		}
		if (key == "Headshot" && kind == k4::ValueKind::Bool) {
			out.boolean = true;
			return true;
		}
		if (key == "Distance" && kind == k4::ValueKind::Double) {
			out.number = 500.5;
			return true;
		}
		if (key == "Penetrated" && kind == k4::ValueKind::Int) {
			out.integer = 1;
			return true;
		}
		return false;
	};
}

k4::Mission MakeMission(std::string event, std::string target, int amount) {
	k4::Mission mission;
	mission.event = std::move(event);
	mission.target = std::move(target);
	mission.amount = amount;
	mission.phrase = "test";
	mission.rewardPhrase = "reward";
	return mission;
}

k4::MissionDefinition MakeDefinition(std::string event, std::string target, int amount, std::string phrase) {
	k4::MissionDefinition definition;
	definition.event = std::move(event);
	definition.target = std::move(target);
	definition.amount = amount;
	definition.phrase = std::move(phrase);
	definition.rewardPhrase = "reward";
	return definition;
}

void TestEventNames() {
	CHECK_EQ(k4::EventClassNameToEventName("EventPlayerDeath"), std::string("player_death"));
	CHECK_EQ(k4::EventClassNameToEventName("EventRoundMvp"), std::string("round_mvp"));
	CHECK_EQ(k4::EventClassNameToEventName("EventBombPlanted"), std::string("bomb_planted"));
	CHECK_EQ(k4::EventClassNameToEventName("EventBombDefused"), std::string("bomb_defused"));
	CHECK_EQ(k4::EventClassNameToEventName("EventHostageRescued"), std::string("hostage_rescued"));
	CHECK_EQ(k4::EventClassNameToEventName("EventGrenadeThrown"), std::string("grenade_thrown"));
	CHECK_EQ(k4::EventClassNameToEventName("EventRoundEnd"), std::string("round_end"));
	CHECK_EQ(k4::EventClassNameToEventName("EventPlayerActivate"), std::string("player_activate"));
	CHECK_EQ(k4::EventClassNameToEventName("EventPlayerDisconnect"), std::string("player_disconnect"));
	CHECK_EQ(k4::EventClassNameToEventName("EventRoundStart"), std::string("round_start"));
	// The original found the SwiftlyS2 class by name; a name without the Event prefix never has one.
	CHECK_EQ(k4::EventClassNameToEventName("player_death"), std::string(""));
	CHECK_EQ(k4::EventClassNameToEventName("Event"), std::string(""));
}

void TestConfig() {
	k4::Config config;
	std::string error;
	CHECK(k4::ParseConfig(ReadFile(AssetPath("config.json")), config, error));
	CHECK_EQ(config.databaseConnection, std::string("host"));
	CHECK_EQ(config.missionCommands.size(), std::size_t {2});
	CHECK_EQ(config.missionCommands[0], std::string("mission"));
	CHECK_EQ(config.missionCommands[1], std::string("missions"));
	CHECK_EQ(config.vipNameDomain, std::string(""));
	CHECK_EQ(config.minimumPlayers, 4);
	CHECK_EQ(config.missionAmountNormal, 1);
	CHECK_EQ(config.missionAmountVip, 3);
	CHECK_EQ(config.vipFlags.size(), std::size_t {1});
	CHECK_EQ(config.vipFlags[0], std::string("any.vip.flag"));
	CHECK(!config.eventDebugLogs);
	CHECK(!config.allowProgressDuringWarmup);
	CHECK(config.resetMode == k4::ResetMode::Daily);
	CHECK_EQ(config.webhookUrl, std::string(""));

	// Case-insensitive section and property names, JSONC comments, enum values case-insensitive.
	const std::string custom = R"json(
	{
		// K4Missions may be spelled in any case, like System.Text.Json's PropertyNameCaseInsensitive.
		"k4missions": {
			"missioncommands": ["m"],
			"vipnamedomain": ".gg",
			"minimumplayers": 2,
			"missionamountnormal": 2,
			"missionamountvip": 5,
			"eventdebuglogs": true,
			"allowprogressduringwarmup": true,
			"resetmode": "weekly",
			"webhookurl": "https://discord.com/api/webhooks/1/x"
		}
	}
	)json";
	k4::Config custom_parsed;
	error.clear();
	CHECK(k4::ParseConfig(custom, custom_parsed, error));
	CHECK(error.empty());
	CHECK_EQ(custom_parsed.missionCommands.size(), std::size_t {1});
	CHECK_EQ(custom_parsed.vipNameDomain, std::string(".gg"));
	CHECK_EQ(custom_parsed.minimumPlayers, 2);
	CHECK_EQ(custom_parsed.missionAmountNormal, 2);
	CHECK_EQ(custom_parsed.missionAmountVip, 5);
	CHECK(custom_parsed.eventDebugLogs);
	CHECK(custom_parsed.allowProgressDuringWarmup);
	CHECK(custom_parsed.resetMode == k4::ResetMode::Weekly);
	CHECK_EQ(custom_parsed.webhookUrl, std::string("https://discord.com/api/webhooks/1/x"));

	// A missing section keeps the defaults, with a notice; broken JSON is a hard error.
	k4::Config defaults;
	error.clear();
	CHECK(k4::ParseConfig("{\"Other\":{}}", defaults, error));
	CHECK(!error.empty());
	CHECK_EQ(defaults.minimumPlayers, 4);

	k4::Config broken;
	error.clear();
	CHECK(!k4::ParseConfig("{ not json", broken, error));
	CHECK(!error.empty());

	// An unknown ResetMode falls back to Daily with a warning.
	k4::Config unknown;
	error.clear();
	CHECK(k4::ParseConfig("{\"K4Missions\":{\"ResetMode\":\"Hourly\"}}", unknown, error));
	CHECK(unknown.resetMode == k4::ResetMode::Daily);
	CHECK(error.find("Hourly") != std::string::npos);
}

void TestMissionsFile() {
	std::vector<k4::MissionDefinition> missions;
	std::string error;
	CHECK(k4::LoadMissions(ReadFile(AssetPath("missions.json")), missions, error));
	CHECK_EQ(missions.size(), std::size_t {12});
	if (missions.size() == 12) {
		CHECK_EQ(missions[0].event, std::string("EventPlayerDeath"));
		CHECK_EQ(missions[0].target, std::string("Attacker"));
		CHECK_EQ(missions[0].amount, 10);
		CHECK(missions[0].hasFlag);
		CHECK_EQ(missions[0].flag, std::string("vipcore.supervip"));
		CHECK(missions[0].hasMapName);
		CHECK_EQ(missions[0].mapName, std::string("de_dust2"));
		CHECK_EQ(missions[0].rewardCommands.size(), std::size_t {1});
		CHECK_EQ(missions[0].rewardCommands[0], std::string("sw_givecredits {steamid64} 30"));

		CHECK_EQ(missions[2].eventProperties.size(), std::size_t {2});
		const k4::MissionValue* weapon = FindProperty(missions[2].eventProperties, "Weapon");
		const k4::MissionValue* headshot = FindProperty(missions[2].eventProperties, "Headshot");
		CHECK(weapon != nullptr && weapon->kind == k4::ValueKind::String && weapon->text == "ak47");
		CHECK(headshot != nullptr && headshot->kind == k4::ValueKind::Bool && headshot->boolean);

		CHECK_EQ(missions[9].event, std::string("PlayTime"));
		CHECK_EQ(missions[10].event, std::string("EventRoundEnd"));
		CHECK_EQ(missions[10].target, std::string("winner"));
		CHECK_EQ(missions[11].event, std::string("EventGrenadeThrown"));
	}

	// Comments + lower-case property names are accepted.
	const std::string custom = R"json([
		{
			"event": "EventRoundEnd", // comment
			"target": "winner",
			"amount": 3,
			"phrase": "Win",
			"rewardphrase": "Coins",
			"rewardcommands": ["give {steamid64} 1"],
			"eventproperties": {"Penetrated": 1}
		}
	])json";
	std::vector<k4::MissionDefinition> parsed;
	CHECK(k4::LoadMissions(custom, parsed, error));
	CHECK_EQ(parsed.size(), std::size_t {1});
	if (!parsed.empty()) {
		CHECK_EQ(parsed[0].amount, 3);
		CHECK_EQ(parsed[0].eventProperties.size(), std::size_t {1});
	}

	// Wrong types are reported instead of being silently ignored (System.Text.Json throws too).
	error.clear();
	CHECK(!k4::LoadMissions("[{\"Event\": 5}]", parsed, error));
	CHECK(!error.empty());
	error.clear();
	CHECK(!k4::LoadMissions("{}", parsed, error));
	CHECK(!error.empty());
}

void TestFilters() {
	const k4::EventPropertyLookup lookup = TestLookup();
	k4::Mission mission = MakeMission("EventPlayerDeath", "Attacker", 5);
	mission.eventProperties = {
		{"Weapon", k4::MissionValue {k4::ValueKind::String, false, 0, 0.0, "ak47"}},
		{"Headshot", k4::MissionValue {k4::ValueKind::Bool, true, 0, 0.0, ""}},
	};
	CHECK(k4::MissionMatches(mission, "EventPlayerDeath", "Attacker", "", &lookup));
	CHECK(k4::MissionMatches(mission, "eventplayerdeath", "attacker", "", &lookup)); // case-insensitive
	CHECK(!k4::MissionMatches(mission, "EventRoundMvp", "Attacker", "", &lookup));
	CHECK(!k4::MissionMatches(mission, "EventPlayerDeath", "Assister", "", &lookup));

	// Missing event property -> no match.
	k4::Mission missing = MakeMission("EventPlayerDeath", "Attacker", 5);
	missing.eventProperties = {{"Noscope", k4::MissionValue {k4::ValueKind::Bool, true, 0, 0.0, ""}}};
	CHECK(!k4::MissionMatches(missing, "EventPlayerDeath", "Attacker", "", &lookup));

	// A null property dictionary skips the filters (the original's round_end path).
	CHECK(k4::MissionMatches(mission, "EventPlayerDeath", "Attacker", "", nullptr));

	// Numbers compare with >= (Penetrated is an integer property, Distance a float).
	k4::Mission penetrated = MakeMission("EventPlayerDeath", "Attacker", 5);
	penetrated.eventProperties = {{"Penetrated", k4::MissionValue {k4::ValueKind::Int, false, 1, 0.0, ""}}};
	CHECK(k4::MissionMatches(penetrated, "EventPlayerDeath", "Attacker", "", &lookup));
	penetrated.eventProperties[0].second.integer = 2;
	CHECK(!k4::MissionMatches(penetrated, "EventPlayerDeath", "Attacker", "", &lookup));

	k4::Mission distance = MakeMission("EventPlayerDeath", "Attacker", 5);
	distance.eventProperties = {{"Distance", k4::MissionValue {k4::ValueKind::Double, false, 0, 500.0, ""}}};
	CHECK(k4::MissionMatches(distance, "EventPlayerDeath", "Attacker", "", &lookup));
	distance.eventProperties[0].second.number = 501.0;
	CHECK(!k4::MissionMatches(distance, "EventPlayerDeath", "Attacker", "", &lookup));

	// Strings are case-insensitive contains matches.
	k4::Mission weapon = MakeMission("EventPlayerDeath", "Attacker", 5);
	weapon.eventProperties = {{"Weapon", k4::MissionValue {k4::ValueKind::String, false, 0, 0.0, "AK47"}}};
	CHECK(k4::MissionMatches(weapon, "EventPlayerDeath", "Attacker", "", &lookup));
	weapon.eventProperties[0].second.text = "awp";
	CHECK(!k4::MissionMatches(weapon, "EventPlayerDeath", "Attacker", "", &lookup));

	// MapName is an exact match against the running map.
	k4::Mission map = MakeMission("EventPlayerDeath", "Attacker", 5);
	map.hasMapName = true;
	map.mapName = "de_dust2";
	CHECK(k4::MissionMatches(map, "EventPlayerDeath", "Attacker", "de_dust2", &lookup));
	CHECK(!k4::MissionMatches(map, "EventPlayerDeath", "Attacker", "de_inferno", &lookup));

	// A completed mission never matches.
	k4::Mission completed = MakeMission("EventPlayerDeath", "Attacker", 5);
	completed.completed = true;
	CHECK(!k4::MissionMatches(completed, "EventPlayerDeath", "Attacker", "", &lookup));

	// Unsupported JSON values (null/array/object) never match, like the original's switch.
	k4::Mission unsupported = MakeMission("EventPlayerDeath", "Attacker", 5);
	unsupported.eventProperties = {{"Weapon", k4::MissionValue {k4::ValueKind::Unsupported, false, 0, 0.0, "[]"}}};
	CHECK(!k4::MissionMatches(unsupported, "EventPlayerDeath", "Attacker", "", &lookup));
}

void TestProgressAccounting() {
	std::vector<k4::Mission> missions;
	missions.push_back(MakeMission("EventPlayerDeath", "Attacker", 10));
	missions[0].progress = 9;
	missions.push_back(MakeMission("EventPlayerDeath", "Attacker", 10));
	missions.push_back(MakeMission("EventRoundMvp", "Userid", 1));
	k4::Mission completed = MakeMission("EventPlayerDeath", "Attacker", 1);
	completed.completed = true;
	missions.push_back(completed);

	const k4::EventPropertyLookup lookup = TestLookup();
	const std::vector<std::size_t> reached =
		k4::AdvanceMissionProgress(missions, "EventPlayerDeath", "Attacker", "", &lookup);
	CHECK_EQ(reached.size(), std::size_t {1});
	CHECK(!reached.empty() && reached[0] == 0);
	CHECK_EQ(missions[0].progress, 10);
	CHECK_EQ(missions[1].progress, 1);
	CHECK_EQ(missions[2].progress, 0);
	CHECK_EQ(missions[3].progress, 0);

	// After the caller flags the mission completed it no longer accumulates.
	missions[0].completed = true;
	const std::vector<std::size_t> second =
		k4::AdvanceMissionProgress(missions, "EventPlayerDeath", "Attacker", "", &lookup);
	CHECK(second.empty());
	CHECK_EQ(missions[0].progress, 10);
	CHECK_EQ(missions[1].progress, 2);

	// PlayTime: one point per tick for unfinished PlayTime missions only (MapName ignored).
	std::vector<k4::Mission> playtime;
	playtime.push_back(MakeMission("PlayTime", "Userid", 2));
	playtime.push_back(MakeMission("EventPlayerDeath", "Attacker", 1));
	k4::Mission done = MakeMission("PlayTime", "Userid", 1);
	done.completed = true;
	playtime.push_back(done);
	const std::vector<std::size_t> first = k4::AdvancePlayTimeProgress(playtime);
	CHECK(first.empty());
	CHECK_EQ(playtime[0].progress, 1);
	CHECK_EQ(playtime[1].progress, 0);
	CHECK_EQ(playtime[2].progress, 0);
	const std::vector<std::size_t> secondTick = k4::AdvancePlayTimeProgress(playtime);
	CHECK_EQ(secondTick.size(), std::size_t {1});
	CHECK(!secondTick.empty() && secondTick[0] == 0);
}

void TestResetPeriods() {
	// The fixed dates are in the test's local timezone; both the calculation and the formatting
	// use localtime, so the expected strings do not depend on the server's TZ.
	const std::time_t wednesday = LocalTime(2026, 10, 14, 10, 30, 0);
	CHECK_EQ(k4::CalculateExpirationDate(k4::ResetMode::Daily, wednesday), std::string("2026-10-14 23:59:59"));
	// The original's AddDays(daysUntilSunday).AddSeconds(-1) is the last second of Saturday:
	// the missions reset when Sunday starts.
	CHECK_EQ(k4::CalculateExpirationDate(k4::ResetMode::Weekly, wednesday), std::string("2026-10-17 23:59:59"));

	// A Sunday counts as "next Sunday" (+7 days), like the original GetEndOfWeek().
	const std::time_t sunday = LocalTime(2026, 10, 18, 00, 00, 0);
	CHECK_EQ(k4::CalculateExpirationDate(k4::ResetMode::Weekly, sunday), std::string("2026-10-24 23:59:59"));

	const std::time_t february = LocalTime(2026, 2, 10, 12, 0, 0);
	CHECK_EQ(k4::CalculateExpirationDate(k4::ResetMode::Monthly, february), std::string("2026-02-28 23:59:59"));
	const std::time_t leapFebruary = LocalTime(2024, 2, 10, 12, 0, 0);
	CHECK_EQ(k4::CalculateExpirationDate(k4::ResetMode::Monthly, leapFebruary), std::string("2024-02-29 23:59:59"));

	CHECK_EQ(k4::CalculateExpirationDate(k4::ResetMode::PerMap, wednesday), std::string(""));
	CHECK_EQ(k4::CalculateExpirationDate(k4::ResetMode::Instant, wednesday), std::string(""));

	// Counting down to the expiry.
	const std::time_t now = LocalTime(2026, 10, 14, 10, 0, 0);
	const k4::Countdown countdown = k4::TimeUntilExpiration("2026-10-15 12:30:00", now);
	CHECK_EQ(countdown.days, 1);
	CHECK_EQ(countdown.hours, 2);
	CHECK_EQ(countdown.minutes, 30);
	const k4::Countdown past = k4::TimeUntilExpiration("2026-10-13 12:00:00", now);
	CHECK_EQ(past.days, 0);
	CHECK_EQ(past.hours, 0);
	CHECK_EQ(past.minutes, 0);
	const k4::Countdown missing = k4::TimeUntilExpiration("", now);
	CHECK_EQ(missing.days + missing.hours + missing.minutes, 0);

	// The stored form round-trips, including the ISO 'T' separator and microseconds.
	const std::optional<std::time_t> parsed = k4::ParseDateTime("2026-10-14 10:00:00");
	CHECK(parsed.has_value());
	if (parsed.has_value()) {
		CHECK_EQ(k4::FormatDateTime(*parsed), std::string("2026-10-14 10:00:00"));
	}
	CHECK(k4::ParseDateTime("2026-10-14T10:00:00.123Z").has_value());
	CHECK(!k4::ParseDateTime("").has_value());
	CHECK(!k4::ParseDateTime("yesterday").has_value());
}

void TestVipAmountAndPlaceholders() {
	k4::Config config;
	config.missionAmountNormal = 1;
	config.missionAmountVip = 3;
	CHECK_EQ(k4::MissionAmountForVip(config, false), 1);
	CHECK_EQ(k4::MissionAmountForVip(config, true), 3);
	config.missionAmountNormal = 4;
	CHECK_EQ(k4::MissionAmountForVip(config, false), 4);

	k4::PlayerPlaceholders player;
	player.slot = 3;
	player.userid = 4;
	player.name = "Bob";
	player.steamid64 = 76561198000000000ULL;
	CHECK_EQ(k4::ExpandRewardCommand("give {steamid64} 30", player), std::string("give 76561198000000000 30"));
	CHECK_EQ(k4::ExpandRewardCommand("{steamid} {slot} {userid} {name}", player),
			 std::string("76561198000000000 3 4 Bob"));
	CHECK_EQ(k4::ExpandRewardCommand("say u0022hiu0022", player), std::string("say \"hi\""));
	CHECK_EQ(k4::ExpandRewardCommand("nothing", player), std::string("nothing"));

	// The name is player-controlled and the expanded line goes into the engine command buffer:
	// ';', newlines and quotes must not be able to start or break out of a command.
	player.name = "x; sv_cheats 1";
	CHECK_EQ(k4::ExpandRewardCommand("say {name} joined", player), std::string("say x sv_cheats 1 joined"));
	player.name = "a\"b'c\nd\re\tf\x01g";
	CHECK_EQ(k4::ExpandRewardCommand("{name}", player), std::string("abcdefg"));

	CHECK_EQ(k4::FormatPhrase("A {0} B {1} {0}", {"x", "y"}), std::string("A x B y x"));
	CHECK_EQ(k4::FormatPhrase("no placeholders", {}), std::string("no placeholders"));
}

void TestEventPropertyPersistence() {
	k4::EventProperties properties;
	properties.emplace_back("Headshot", k4::MissionValue {k4::ValueKind::Bool, true, 0, 0.0, ""});
	properties.emplace_back("Penetrated", k4::MissionValue {k4::ValueKind::Int, false, 2, 0.0, ""});
	properties.emplace_back("Distance", k4::MissionValue {k4::ValueKind::Double, false, 0, 250.5, ""});
	properties.emplace_back("Weapon", k4::MissionValue {k4::ValueKind::String, false, 0, 0.0, "ak47"});
	properties.emplace_back("Odd", k4::MissionValue {k4::ValueKind::Unsupported, false, 0, 0.0, "[1,2]"});

	const std::string json = k4::SerializeEventPropertiesJson(properties);
	CHECK(!json.empty());
	CHECK(json.find("\"Headshot\":true") != std::string::npos);
	CHECK(json.find("\"Penetrated\":2") != std::string::npos);

	k4::EventProperties parsed;
	CHECK(k4::ParseEventPropertiesJson(json, parsed));
	CHECK_EQ(parsed.size(), std::size_t {5});
	const k4::MissionValue* headshot = FindProperty(parsed, "Headshot");
	const k4::MissionValue* penetrated = FindProperty(parsed, "Penetrated");
	const k4::MissionValue* distance = FindProperty(parsed, "Distance");
	const k4::MissionValue* weapon = FindProperty(parsed, "Weapon");
	const k4::MissionValue* odd = FindProperty(parsed, "Odd");
	CHECK(headshot != nullptr && headshot->kind == k4::ValueKind::Bool && headshot->boolean);
	CHECK(penetrated != nullptr && penetrated->kind == k4::ValueKind::Int && penetrated->integer == 2);
	CHECK(distance != nullptr && distance->kind == k4::ValueKind::Double);
	CHECK(weapon != nullptr && weapon->kind == k4::ValueKind::String && weapon->text == "ak47");
	CHECK(odd != nullptr && odd->kind == k4::ValueKind::Unsupported);

	CHECK_EQ(k4::SerializeEventPropertiesJson({}), std::string(""));
	k4::EventProperties invalid;
	CHECK(!k4::ParseEventPropertiesJson("{not json", invalid));
	CHECK(invalid.empty());
}

void TestMissionAvailability() {
	std::vector<k4::MissionDefinition> definitions;
	definitions.push_back(MakeDefinition("EventPlayerDeath", "Attacker", 10, "Kill 10"));
	definitions.push_back(MakeDefinition("EventPlayerDeath", "Attacker", 25, "Kill 25"));
	k4::MissionDefinition flagged = MakeDefinition("EventRoundMvp", "Userid", 3, "MVP 3");
	flagged.hasFlag = true;
	flagged.flag = "vipcore.supervip";
	definitions.push_back(flagged);

	std::vector<k4::Mission> current;
	current.push_back(k4::CreateMission(definitions[0], "")); // already assigned (same MissionKey)

	std::vector<std::size_t> available = k4::AvailableMissionIndices(definitions, current, [](const std::string&) {
		return false;
	});
	CHECK_EQ(available.size(), std::size_t {1});
	CHECK(!available.empty() && available[0] == 1); // #0 is held, #2 needs the flag

	std::vector<std::string> requested;
	available = k4::AvailableMissionIndices(definitions, current, [&requested](const std::string& flag) {
		requested.push_back(flag);
		return true;
	});
	CHECK_EQ(available.size(), std::size_t {2});
	CHECK_EQ(requested.size(), std::size_t {1});
	if (!requested.empty()) {
		CHECK_EQ(requested[0], std::string("vipcore.supervip"));
	}

	CHECK_EQ(k4::MissionKey("EventPlayerDeath", "Attacker", 10, "Kill 10"), std::string("EventPlayerDeath|Attacker|10|Kill 10"));
	CHECK_EQ(k4::MissionKey(definitions[1]), std::string("EventPlayerDeath|Attacker|25|Kill 25"));

	k4::Mission created = k4::CreateMission(definitions[2], "2026-10-15 23:59:59");
	CHECK_EQ(created.event, std::string("EventRoundMvp"));
	CHECK_EQ(created.amount, 3);
	CHECK(created.hasFlag);
	CHECK_EQ(created.flag, std::string("vipcore.supervip"));
	CHECK(created.hasExpiresAt);
	CHECK_EQ(created.expiresAt, std::string("2026-10-15 23:59:59"));
	CHECK_EQ(created.id, int64_t {-1});
	const k4::Mission noExpiry = k4::CreateMission(definitions[0], "");
	CHECK(!noExpiry.hasExpiresAt);
}

// Runs the async queries of the store exactly like the plugin: queue from the test thread and
// deliver the callbacks from Poll().
bool PollUntil(plgdb::Database& database, const std::function<bool()>& done) {
	for (int attempt = 0; attempt < 5000; ++attempt) {
		database.Poll();
		if (done()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	database.Poll();
	return done();
}

void TestStoreAgainst(plgdb::Config config) {
	std::unique_ptr<plgdb::Database> database = plgdb::Database::Create(config);
	CHECK(database != nullptr);
	if (database == nullptr) {
		return;
	}
	k4::MissionStore store(*database);
	std::string error;
	CHECK(store.EnsureSchema(error));
	if (!error.empty()) {
		std::printf("store note: %s\n", error.c_str());
	}

	const uint64_t steamid = 76561199000000000ULL + static_cast<uint64_t>(std::time(nullptr) % 1000000);
	const uint64_t otherSteamid = steamid + 1;

	k4::MissionDefinition definition = MakeDefinition("EventPlayerDeath", "Attacker", 10, "Kill 10");
	definition.rewardCommands = {"give {steamid64} 30", "say done"};
	k4::Mission mission = k4::CreateMission(definition, "2026-10-15 23:59:59");
	mission.hasMapName = true;
	mission.mapName = "de_dust2";
	mission.eventProperties = {{"Headshot", k4::MissionValue {k4::ValueKind::Bool, true, 0, 0.0, ""}}};

	int64_t id = -1;
	bool inserted = false;
	store.InsertMission(steamid, mission, [&id, &inserted](int64_t value) {
		id = value;
		inserted = true;
	});
	CHECK(PollUntil(*database, [&inserted] { return inserted; }));
	CHECK(id > 0);

	std::vector<k4::Mission> loaded;
	bool loadDone = false;
	bool loadOk = false;
	store.LoadPlayer(steamid, [&loaded, &loadDone, &loadOk](bool ok, std::vector<k4::Mission> missions) {
		loaded = std::move(missions);
		loadDone = true;
		loadOk = ok;
	});
	CHECK(PollUntil(*database, [&loadDone] { return loadDone; }));
	CHECK(loadOk);
	CHECK_EQ(loaded.size(), std::size_t {1});
	if (loaded.size() == 1) {
		CHECK_EQ(loaded[0].id, id);
		CHECK_EQ(loaded[0].event, std::string("EventPlayerDeath"));
		CHECK_EQ(loaded[0].target, std::string("Attacker"));
		CHECK_EQ(loaded[0].amount, 10);
		CHECK_EQ(loaded[0].phrase, std::string("Kill 10"));
		CHECK_EQ(loaded[0].rewardPhrase, std::string("reward"));
		CHECK_EQ(loaded[0].rewardCommands.size(), std::size_t {2});
		CHECK(loaded[0].hasExpiresAt);
		CHECK_EQ(loaded[0].expiresAt, std::string("2026-10-15 23:59:59"));
		CHECK(loaded[0].hasMapName);
		CHECK_EQ(loaded[0].mapName, std::string("de_dust2"));
		CHECK_EQ(loaded[0].eventProperties.size(), std::size_t {1});
		const k4::MissionValue* headshot = FindProperty(loaded[0].eventProperties, "Headshot");
		CHECK(headshot != nullptr && headshot->kind == k4::ValueKind::Bool && headshot->boolean);
		CHECK_EQ(loaded[0].progress, 0);
		CHECK(!loaded[0].completed);
	}

	// progress/completed are the only columns the original's UpdateMissionsAsync writes.
	loaded[0].progress = 7;
	store.SaveMissions(loaded);
	bool sentinel = false;
	database->Query("SELECT 1", {}, [&sentinel](const plgdb::Result&) { sentinel = true; });
	CHECK(PollUntil(*database, [&sentinel] { return sentinel; }));

	std::vector<k4::Mission> saved;
	bool savedDone = false;
	store.LoadPlayer(steamid, [&saved, &savedDone](bool, std::vector<k4::Mission> missions) {
		saved = std::move(missions);
		savedDone = true;
	});
	CHECK(PollUntil(*database, [&savedDone] { return savedDone; }));
	CHECK_EQ(saved.size(), std::size_t {1});
	if (saved.size() == 1) {
		CHECK_EQ(saved[0].progress, 7);
	}

	// MarkCompleted on the same row; a second call finds completed = 1 and reports false, exactly
	// like the original's GetAsync check (so a shared database cannot pay a reward twice).
	bool completed = false;
	store.MarkCompleted(id, [&completed](bool value) { completed = value; });
	CHECK(PollUntil(*database, [&completed] { return completed; }));
	CHECK(completed);
	bool completedAgain = true;
	store.MarkCompleted(id, [&completedAgain](bool value) { completedAgain = value; });
	CHECK(PollUntil(*database, [&completedAgain] { return !completedAgain; }));
	CHECK(!completedAgain);

	// A stale snapshot (another server of a shared database advanced the row) must not undo the
	// completion or lower the stored progress: SaveMissions keeps the stored maximum and never
	// clears `completed`, or the completion claim could be paid twice.
	std::vector<k4::Mission> stale = loaded;
	stale[0].progress = 3;
	stale[0].completed = false;
	store.SaveMissions(stale);
	sentinel = false;
	database->Query("SELECT 1", {}, [&sentinel](const plgdb::Result&) { sentinel = true; });
	CHECK(PollUntil(*database, [&sentinel] { return sentinel; }));
	std::vector<k4::Mission> preserved;
	bool preservedDone = false;
	store.LoadPlayer(steamid, [&preserved, &preservedDone](bool, std::vector<k4::Mission> missions) {
		preserved = std::move(missions);
		preservedDone = true;
	});
	CHECK(PollUntil(*database, [&preservedDone] { return preservedDone; }));
	CHECK_EQ(preserved.size(), std::size_t {1});
	if (preserved.size() == 1) {
		CHECK_EQ(preserved[0].progress, 7);
		CHECK(preserved[0].completed);
	}

	// A second mission for the same player, then remove both.
	k4::Mission second = k4::CreateMission(definition, "");
	int64_t secondId = -1;
	bool secondInserted = false;
	store.InsertMission(steamid, second, [&secondId, &secondInserted](int64_t value) {
		secondId = value;
		secondInserted = true;
	});
	CHECK(PollUntil(*database, [&secondInserted] { return secondInserted; }));
	CHECK(secondId > 0);

	store.RemoveMissions({id, secondId});
	sentinel = false;
	database->Query("SELECT 1", {}, [&sentinel](const plgdb::Result&) { sentinel = true; });
	CHECK(PollUntil(*database, [&sentinel] { return sentinel; }));
	std::vector<k4::Mission> removed;
	bool removedDone = false;
	store.LoadPlayer(steamid, [&removed, &removedDone](bool, std::vector<k4::Mission> missions) {
		removed = std::move(missions);
		removedDone = true;
	});
	CHECK(PollUntil(*database, [&removedDone] { return removedDone; }));
	CHECK(removed.empty());

	// Expired rows: selected for the reset pass and removed by the cleanup.
	k4::Mission expired = k4::CreateMission(definition, "2000-01-01 00:00:00");
	bool expiredInserted = false;
	store.InsertMission(otherSteamid, expired, [&expiredInserted](int64_t value) {
		expiredInserted = value > 0;
	});
	CHECK(PollUntil(*database, [&expiredInserted] { return expiredInserted; }));

	std::vector<uint64_t> expiredPlayers;
	bool expiredQueried = false;
	bool expiredOk = false;
	store.PlayersWithExpiredMissions("2026-10-14 00:00:00", [&expiredPlayers, &expiredQueried, &expiredOk](
																 bool ok, std::vector<uint64_t> ids) {
		expiredPlayers = std::move(ids);
		expiredQueried = true;
		expiredOk = ok;
	});
	CHECK(PollUntil(*database, [&expiredQueried] { return expiredQueried; }));
	CHECK(expiredOk);
	CHECK(std::find(expiredPlayers.begin(), expiredPlayers.end(), otherSteamid) != expiredPlayers.end());

	store.CleanupExpiredMissions("2026-10-14 00:00:00");
	sentinel = false;
	database->Query("SELECT 1", {}, [&sentinel](const plgdb::Result&) { sentinel = true; });
	CHECK(PollUntil(*database, [&sentinel] { return sentinel; }));
	std::vector<k4::Mission> gone;
	bool goneDone = false;
	store.LoadPlayer(otherSteamid, [&gone, &goneDone](bool, std::vector<k4::Mission> missions) {
		gone = std::move(missions);
		goneDone = true;
	});
	CHECK(PollUntil(*database, [&goneDone] { return goneDone; }));
	CHECK(gone.empty());
}

// The plugin's database path: the schema migration runs on the worker and reports through Poll(),
// and a failed SELECT must be distinguishable from a player with no rows (a transient error used
// to look like an empty mission list for the whole session).
void TestAsyncSchemaAndLoadResult() {
	plgdb::Config sqlite;
	sqlite.driver = plgdb::Driver::SQLite;
	sqlite.sqlitePath = ":memory:";
	std::unique_ptr<plgdb::Database> database = plgdb::Database::Create(sqlite);
	CHECK(database != nullptr);
	if (database == nullptr) {
		return;
	}
	k4::MissionStore store(*database);

	bool failedDone = false;
	bool failedOk = true;
	bool failedEmpty = false;
	store.LoadPlayer(123, [&failedDone, &failedOk, &failedEmpty](bool ok, std::vector<k4::Mission> missions) {
		failedDone = true;
		failedOk = ok;
		failedEmpty = missions.empty();
	});
	CHECK(PollUntil(*database, [&failedDone] { return failedDone; }));
	CHECK(!failedOk); // no table yet: the SELECT failed, this is not "no missions"
	CHECK(failedEmpty);

	bool schemaDone = false;
	bool schemaOk = false;
	std::string schemaError;
	k4::EnsureSchemaAsync(*database, [&schemaDone, &schemaOk, &schemaError](bool ok, std::string error) {
		schemaDone = true;
		schemaOk = ok;
		schemaError = std::move(error);
	});
	CHECK(PollUntil(*database, [&schemaDone] { return schemaDone; }));
	CHECK(schemaOk);
	CHECK(schemaError.empty());

	// The same migration over the table it just created (the two ALTERs answer "duplicate column",
	// which is the expected answer and must not fail the migration).
	schemaDone = false;
	schemaOk = false;
	schemaError = "sentinel";
	k4::EnsureSchemaAsync(*database, [&schemaDone, &schemaOk, &schemaError](bool ok, std::string error) {
		schemaDone = true;
		schemaOk = ok;
		schemaError = std::move(error);
	});
	CHECK(PollUntil(*database, [&schemaDone] { return schemaDone; }));
	CHECK(schemaOk);
	CHECK(schemaError.empty());

	// After the migration the same SELECT is a successful empty result.
	bool emptyDone = false;
	bool emptyOk = false;
	store.LoadPlayer(123, [&emptyDone, &emptyOk](bool ok, std::vector<k4::Mission>) {
		emptyDone = true;
		emptyOk = ok;
	});
	CHECK(PollUntil(*database, [&emptyDone] { return emptyDone; }));
	CHECK(emptyOk);

	// An impossible SQLite path reports the failure through the same callback.
	plgdb::Config broken;
	broken.driver = plgdb::Driver::SQLite;
	broken.sqlitePath = K4_MISSIONS_TEST_DATA_DIR; // a directory: sqlite cannot open it as a database
	std::unique_ptr<plgdb::Database> brokenDatabase = plgdb::Database::Create(broken);
	CHECK(brokenDatabase != nullptr);
	if (brokenDatabase == nullptr) {
		return;
	}
	bool brokenDone = false;
	bool brokenOk = true;
	std::string brokenError;
	k4::EnsureSchemaAsync(*brokenDatabase, [&brokenDone, &brokenOk, &brokenError](bool ok, std::string error) {
		brokenDone = true;
		brokenOk = ok;
		brokenError = std::move(error);
	});
	CHECK(PollUntil(*brokenDatabase, [&brokenDone] { return brokenDone; }));
	CHECK(!brokenOk);
	CHECK(!brokenError.empty());

	// A failed expiry SELECT must say so: the plugin skips the whole expiry pass in that case
	// (treating it as "no rows expired" would erase missions while the replacement INSERTs fail).
	k4::MissionStore brokenStore(*brokenDatabase);
	bool expiredDone = false;
	bool expiredOk = true;
	std::size_t expiredCount = 1;
	brokenStore.PlayersWithExpiredMissions("2026-10-14 00:00:00", [&expiredDone, &expiredOk, &expiredCount](
																   bool ok, std::vector<uint64_t> ids) {
		expiredDone = true;
		expiredOk = ok;
		expiredCount = ids.size();
	});
	CHECK(PollUntil(*brokenDatabase, [&expiredDone] { return expiredDone; }));
	CHECK(!expiredOk);
	CHECK_EQ(expiredCount, std::size_t {0});
}

void TestStore() {
	plgdb::Config sqlite;
	sqlite.driver = plgdb::Driver::SQLite;
	sqlite.sqlitePath = ":memory:";
	TestStoreAgainst(sqlite);

	// ./test.sh exports the throwaway MariaDB; the same statements then run on MySQL too, which
	// is the driver the production database uses.
	const char* host = std::getenv("MYSQL_HOST");
	if (host == nullptr || *host == '\0') {
		return;
	}
	plgdb::Config mysql;
	mysql.driver = plgdb::Driver::MySQL;
	mysql.host = host;
	mysql.port = static_cast<uint16_t>(std::atoi(std::getenv("MYSQL_PORT") ? std::getenv("MYSQL_PORT") : "3306"));
	mysql.user = std::getenv("MYSQL_USER") ? std::getenv("MYSQL_USER") : "";
	mysql.password = std::getenv("MYSQL_PASSWORD") ? std::getenv("MYSQL_PASSWORD") : "";
	mysql.database = std::getenv("MYSQL_DATABASE") ? std::getenv("MYSQL_DATABASE") : "plgtest";
	TestStoreAgainst(mysql);
}

void TestStringHelpers() {
	// Event property names: SwiftlyS2's CLR spelling -> the engine event key.
	CHECK_EQ(k4::PascalToSnake("DmgHealth"), std::string("dmg_health"));
	CHECK_EQ(k4::PascalToSnake("Weapon"), std::string("weapon"));
	CHECK_EQ(k4::PascalToSnake("dmg_health"), std::string("dmg_health"));
	CHECK_EQ(k4::PascalToSnake("Thrusmoke"), std::string("thrusmoke"));
	CHECK_EQ(k4::PascalToSnake("Attackerblind"), std::string("attackerblind"));
	CHECK_EQ(k4::PascalToSnake("PlayerID"), std::string("player_id"));

	CHECK_EQ(k4::LowerAscii("EventRoundMVP"), std::string("eventroundmvp"));
	CHECK(k4::EqualsIgnoreCase("Daily", "DAILY"));
	CHECK(!k4::EqualsIgnoreCase("Daily", "Day"));
	CHECK(k4::ContainsIgnoreCase("weapon_ak47", "AK47"));
	CHECK(!k4::ContainsIgnoreCase("weapon_awp", "ak47"));
	CHECK(k4::IsVipByName("Player.VIP.gg", "vip.gg"));
	CHECK(!k4::IsVipByName("Player.gg", "vip.gg"));
	CHECK(!k4::IsVipByName("Anything", ""));
	// A domain that is present in a name of any case.
	CHECK(k4::IsVipByName("MyVipPlayer", "vip"));

	k4::ResetMode mode = k4::ResetMode::Daily;
	CHECK(k4::ParseResetMode("PERMAP", mode));
	CHECK(mode == k4::ResetMode::PerMap);
	CHECK(k4::ParseResetMode("instant", mode));
	CHECK(mode == k4::ResetMode::Instant);
	CHECK(!k4::ParseResetMode("hourly", mode));
}

// The plugin-local KV1 reader and the phrase table (they replace pisexcompat's KeyValues and
// LoadPhrases): the grammar the production files use, the language fallback order and the
// "unknown key is empty" lookup.
void TestKv1AndPhrases() {
	// A "Databases" document: the root block is unwrapped, keys are matched case-insensitively and
	// // comments are ignored.
	const std::optional<k4::KvNode> databases = k4::ParseKvDocument(R"("Databases"
	{
		"k4_missions"
		{
			// the production connection
			"driver"    "mysql"
			"port"      "3306"
			"sqlite_path" "addons/data/k4.sqlite"
		}
	})");
	CHECK(databases.has_value());
	if (databases.has_value()) {
		const k4::KvNode* entry = databases->Find("k4_missions");
		CHECK(entry != nullptr);
		if (entry != nullptr) {
			CHECK(std::string(entry->GetString("DRIVER")) == "mysql");
			CHECK_EQ(entry->GetInt("port", 0), 3306);
			CHECK(std::string(entry->GetString("missing", "fallback")) == "fallback");
			// A block is not a value.
			CHECK(std::string(entry->GetString("nested", "x")) == "x");
		}
	}

	// A malformed document is rejected as a whole (never half-parsed).
	CHECK(!k4::ParseKvDocument("\"Root\" { \"key\" \"value\"").has_value());
	CHECK(!k4::ParseKvDocument("\"Root\" { } }").has_value());

	// A document with several top-level keys keeps them all (no unwrapping).
	const std::optional<k4::KvNode> flat = k4::ParseKvDocument("\"a\" \"1\"\n\"b\" \"2\"");
	CHECK(flat.has_value());
	if (flat.has_value()) {
		CHECK(std::string(flat->GetString("b")) == "2");
	}

	// Phrase files: the requested language, then "en", then the first entry of the phrase.
	std::map<std::string, std::string> phrases;
	CHECK(k4::ParsePhrasesText(R"("Phrases"
	{
		"key.one" { "en" "One" "ru" "Один" }
		"key.two" { "en" "Two" }
		"key.three" { "de" "Drei" }
	})", "ru", phrases));
	CHECK(phrases["key.one"] == "Один");
	CHECK(phrases["key.two"] == "Two");    // missing "ru": the English fallback
	CHECK(phrases["key.three"] == "Drei"); // no "ru"/"en": the first language
	CHECK(k4::LookupPhrase(phrases, "key.one") == "Один");
	CHECK(k4::LookupPhrase(phrases, "no.such.key").empty());

	// An unknown language is normalized only where a known spelling exists.
	CHECK(k4::NormalizeLanguage("Russian") == "ru");
	CHECK(k4::NormalizeLanguage("ENGLISH") == "en");
	CHECK(k4::NormalizeLanguage("pt-br") == "pt-BR");
	CHECK(k4::NormalizeLanguage("Klingon") == "klingon");

	// A malformed phrase file is rejected (the plugin then keeps its keys as-is).
	CHECK(!k4::ParsePhrasesText("\"Phrases\" { \"broken\"", "en", phrases));
	CHECK(phrases.empty());
}

} // namespace

int main() {
	TestEventNames();
	TestConfig();
	TestMissionsFile();
	TestFilters();
	TestProgressAccounting();
	TestResetPeriods();
	TestVipAmountAndPlaceholders();
	TestEventPropertyPersistence();
	TestMissionAvailability();
	TestAsyncSchemaAndLoadResult();
	TestStore();
	TestStringHelpers();
	TestKv1AndPhrases();

	std::printf("k4_missions_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
