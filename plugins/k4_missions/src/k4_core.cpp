#include "k4_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

#include <nlohmann/json.hpp>

namespace k4 {
namespace {

constexpr const char* kDateTimeFormat = "%Y-%m-%d %H:%M:%S";

int DaysInMonth(int year, int month) {
	static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (month < 1 || month > 12) {
		return 30;
	}
	if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
		return 29;
	}
	return kDays[month - 1];
}

std::string ReplaceAll(std::string text, std::string_view needle, std::string_view replacement) {
	if (needle.empty()) {
		return text;
	}
	std::string::size_type position = 0;
	while ((position = text.find(needle, position)) != std::string::npos) {
		text.replace(position, needle.size(), replacement);
		position += replacement.size();
	}
	return text;
}

// The reward command reaches the engine's command buffer through s2sdk::ServerCommand(), which
// splits on ';' and newlines and honours quotes. The player name is the only placeholder a player
// controls, so a name like `x; sv_cheats 1` must not be able to start a second command.
std::string SanitizeCommandPlaceholder(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (const char character : text) {
		const auto byte = static_cast<unsigned char>(character);
		if (character == ';' || character == '"' || character == '\'' || byte < 0x20 || byte == 0x7F) {
			continue;
		}
		result.push_back(character);
	}
	return result;
}

std::string TrimSpaces(std::string_view text) {
	const std::size_t begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos) {
		return {};
	}
	const std::size_t end = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(begin, end - begin + 1));
}

// --- case-insensitive JSON key access (System.Text.Json PropertyNameCaseInsensitive) ---------

const nlohmann::json* FindMember(const nlohmann::json& object, std::string_view key) {
	if (!object.is_object()) {
		return nullptr;
	}
	for (auto it = object.begin(); it != object.end(); ++it) {
		if (EqualsIgnoreCase(it.key(), key)) {
			return &it.value();
		}
	}
	return nullptr;
}

bool ReadString(const nlohmann::json& object, std::string_view key, std::string& out, bool& present,
				std::string& error) {
	present = false;
	const nlohmann::json* value = FindMember(object, key);
	if (value == nullptr || value->is_null()) {
		return true;
	}
	if (!value->is_string()) {
		error = "property '" + std::string(key) + "' must be a string";
		return false;
	}
	out = value->get<std::string>();
	present = true;
	return true;
}

bool ReadInt(const nlohmann::json& object, std::string_view key, int& out, std::string& error) {
	const nlohmann::json* value = FindMember(object, key);
	if (value == nullptr || value->is_null()) {
		return true;
	}
	if (!value->is_number()) {
		error = "property '" + std::string(key) + "' must be a number";
		return false;
	}
	if (value->is_number_unsigned()) {
		const uint64_t raw = value->get<uint64_t>();
		out = raw > static_cast<uint64_t>(INT32_MAX) ? INT32_MAX : static_cast<int>(raw);
		return true;
	}
	if (value->is_number_integer()) {
		const int64_t raw = value->get<int64_t>();
		out = raw > INT32_MAX ? INT32_MAX : (raw < INT32_MIN ? INT32_MIN : static_cast<int>(raw));
		return true;
	}
	const double raw = value->get<double>();
	out = raw >= static_cast<double>(INT32_MAX) ? INT32_MAX
		: (raw <= static_cast<double>(INT32_MIN) ? INT32_MIN : static_cast<int>(raw));
	return true;
}

bool ReadBool(const nlohmann::json& object, std::string_view key, bool& out, std::string& error) {
	const nlohmann::json* value = FindMember(object, key);
	if (value == nullptr || value->is_null()) {
		return true;
	}
	if (!value->is_boolean()) {
		error = "property '" + std::string(key) + "' must be a boolean";
		return false;
	}
	out = value->get<bool>();
	return true;
}

bool ReadStringArray(const nlohmann::json& object, std::string_view key, std::vector<std::string>& out,
					 std::string& error) {
	const nlohmann::json* value = FindMember(object, key);
	if (value == nullptr || value->is_null()) {
		return true;
	}
	if (!value->is_array()) {
		error = "property '" + std::string(key) + "' must be an array of strings";
		return false;
	}
	std::vector<std::string> values;
	for (const nlohmann::json& element : *value) {
		if (!element.is_string()) {
			error = "property '" + std::string(key) + "' must be an array of strings";
			return false;
		}
		values.push_back(element.get<std::string>());
	}
	out = std::move(values);
	return true;
}

MissionValue ToMissionValue(const nlohmann::json& value) {
	MissionValue out;
	if (value.is_boolean()) {
		out.kind = ValueKind::Bool;
		out.boolean = value.get<bool>();
	} else if (value.is_number_integer()) {
		out.kind = ValueKind::Int;
		if (value.is_number_unsigned()) {
			const uint64_t raw = value.get<uint64_t>();
			out.integer = raw > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(raw);
		} else {
			out.integer = value.get<int64_t>();
		}
	} else if (value.is_number_float()) {
		out.kind = ValueKind::Double;
		out.number = value.get<double>();
	} else if (value.is_string()) {
		out.kind = ValueKind::String;
		out.text = value.get<std::string>();
	} else {
		// null / array / object: the original's switch falls through to `false`. The raw JSON is
		// kept so the value survives a database round-trip unchanged.
		out.kind = ValueKind::Unsupported;
		out.text = value.dump();
	}
	return out;
}

bool ReadEventProperties(const nlohmann::json& object, EventProperties& out, std::string& error) {
	const nlohmann::json* value = FindMember(object, "EventProperties");
	if (value == nullptr || value->is_null()) {
		return true;
	}
	if (!value->is_object()) {
		error = "property 'EventProperties' must be an object";
		return false;
	}
	EventProperties properties;
	for (auto it = value->begin(); it != value->end(); ++it) {
		properties.emplace_back(it.key(), ToMissionValue(it.value()));
	}
	out = std::move(properties);
	return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------------------------

bool ParseResetMode(std::string_view text, ResetMode& out) {
	const std::string lower = LowerAscii(text);
	if (lower == "permap") {
		out = ResetMode::PerMap;
	} else if (lower == "daily") {
		out = ResetMode::Daily;
	} else if (lower == "weekly") {
		out = ResetMode::Weekly;
	} else if (lower == "monthly") {
		out = ResetMode::Monthly;
	} else if (lower == "instant") {
		out = ResetMode::Instant;
	} else {
		return false;
	}
	return true;
}

const char* ResetModeName(ResetMode mode) {
	switch (mode) {
	case ResetMode::PerMap: return "PerMap";
	case ResetMode::Daily: return "Daily";
	case ResetMode::Weekly: return "Weekly";
	case ResetMode::Monthly: return "Monthly";
	case ResetMode::Instant: return "Instant";
	}
	return "Daily";
}

bool ParseConfig(std::string_view text, Config& out, std::string& error) {
	error.clear();
	nlohmann::json root;
	try {
		root = nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/true,
									 /*ignore_comments=*/true);
	} catch (const std::exception& exception) {
		error = std::string("config.json parse error: ") + exception.what();
		return false;
	}
	if (!root.is_object()) {
		error = "config.json must be a JSON object";
		return false;
	}

	const nlohmann::json* section = FindMember(root, "K4Missions");
	if (section == nullptr || !section->is_object()) {
		error = "config.json has no object section 'K4Missions' - the built-in defaults are used";
		return true;
	}

	Config config;
	std::string typeError;
	bool present = false;
	if (!ReadString(*section, "DatabaseConnection", config.databaseConnection, present, typeError)) {
		error = typeError;
	}
	if (!ReadStringArray(*section, "MissionCommands", config.missionCommands, typeError)) {
		error = typeError;
	}
	std::string vipDomain;
	if (ReadString(*section, "VipNameDomain", vipDomain, present, typeError)) {
		config.vipNameDomain = vipDomain;
	} else {
		error = typeError;
	}
	if (!ReadInt(*section, "MinimumPlayers", config.minimumPlayers, typeError)) {
		error = typeError;
	}
	if (!ReadInt(*section, "MissionAmountNormal", config.missionAmountNormal, typeError)) {
		error = typeError;
	}
	if (!ReadInt(*section, "MissionAmountVip", config.missionAmountVip, typeError)) {
		error = typeError;
	}
	if (!ReadStringArray(*section, "VipFlags", config.vipFlags, typeError)) {
		error = typeError;
	}
	if (!ReadBool(*section, "EventDebugLogs", config.eventDebugLogs, typeError)) {
		error = typeError;
	}
	if (!ReadBool(*section, "AllowProgressDuringWarmup", config.allowProgressDuringWarmup, typeError)) {
		error = typeError;
	}
	std::string resetMode;
	present = false;
	if (ReadString(*section, "ResetMode", resetMode, present, typeError)) {
		if (!present) {
			config.resetMode = ResetMode::Daily;
		} else if (!ParseResetMode(resetMode, config.resetMode)) {
			error = "unknown ResetMode '" + resetMode + "' - Daily is used";
			config.resetMode = ResetMode::Daily;
		}
	} else {
		error = typeError;
	}
	if (!ReadString(*section, "WebhookUrl", config.webhookUrl, present, typeError)) {
		error = typeError;
	}

	out = std::move(config);
	return true;
}

int MissionAmountForVip(const Config& config, bool isVip) {
	return isVip ? config.missionAmountVip : config.missionAmountNormal;
}

// ---------------------------------------------------------------------------------------------
// event class names
// ---------------------------------------------------------------------------------------------

std::string PascalToSnake(std::string_view name) {
	std::string out;
	out.reserve(name.size() + 4);
	for (std::size_t index = 0; index < name.size(); ++index) {
		const char character = name[index];
		if (std::isupper(static_cast<unsigned char>(character)) != 0) {
			const bool previousLower = index > 0 && (std::islower(static_cast<unsigned char>(name[index - 1])) != 0
													 || std::isdigit(static_cast<unsigned char>(name[index - 1])) != 0);
			const bool nextLower = index + 1 < name.size()
								   && std::islower(static_cast<unsigned char>(name[index + 1])) != 0;
			if (!out.empty() && (previousLower || nextLower)) {
				out.push_back('_');
			}
			out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
		} else {
			out.push_back(character);
		}
	}
	return out;
}

std::string EventClassNameToEventName(std::string_view className) {
	constexpr std::string_view prefix = "Event";
	if (className.size() <= prefix.size() || className.substr(0, prefix.size()) != prefix) {
		return {};
	}
	return PascalToSnake(className.substr(prefix.size()));
}

// ---------------------------------------------------------------------------------------------
// missions
// ---------------------------------------------------------------------------------------------

MissionDefinition Mission::ToDefinition() const {
	MissionDefinition definition;
	definition.event = event;
	definition.target = target;
	definition.rewardCommands = rewardCommands;
	definition.amount = amount;
	definition.phrase = phrase;
	definition.rewardPhrase = rewardPhrase;
	definition.eventProperties = eventProperties;
	definition.hasMapName = hasMapName;
	definition.mapName = mapName;
	definition.hasFlag = hasFlag;
	definition.flag = flag;
	return definition;
}

Mission CreateMission(const MissionDefinition& definition, const std::string& expiresAt) {
	Mission mission;
	mission.id = -1;
	mission.event = definition.event;
	mission.target = definition.target;
	mission.amount = definition.amount;
	mission.phrase = definition.phrase;
	mission.rewardPhrase = definition.rewardPhrase;
	mission.rewardCommands = definition.rewardCommands;
	mission.eventProperties = definition.eventProperties;
	mission.hasMapName = definition.hasMapName;
	mission.mapName = definition.mapName;
	mission.hasFlag = definition.hasFlag;
	mission.flag = definition.flag;
	mission.progress = 0;
	mission.completed = false;
	mission.hasExpiresAt = !expiresAt.empty();
	mission.expiresAt = expiresAt;
	return mission;
}

std::string MissionKey(std::string_view event, std::string_view target, int amount, std::string_view phrase) {
	std::string key;
	key.reserve(event.size() + target.size() + phrase.size() + 16);
	key.append(event);
	key.push_back('|');
	key.append(target);
	key.push_back('|');
	key.append(std::to_string(amount));
	key.push_back('|');
	key.append(phrase);
	return key;
}

std::string MissionKey(const MissionDefinition& mission) {
	return MissionKey(mission.event, mission.target, mission.amount, mission.phrase);
}

std::string MissionKey(const Mission& mission) {
	return MissionKey(mission.event, mission.target, mission.amount, mission.phrase);
}

bool LoadMissions(std::string_view text, std::vector<MissionDefinition>& out, std::string& error) {
	error.clear();
	nlohmann::json root;
	try {
		root = nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/true,
									 /*ignore_comments=*/true);
	} catch (const std::exception& exception) {
		error = std::string("missions.json parse error: ") + exception.what();
		return false;
	}
	if (!root.is_array()) {
		error = "missions.json must be a JSON array";
		return false;
	}

	std::vector<MissionDefinition> missions;
	missions.reserve(root.size());
	for (const nlohmann::json& element : root) {
		if (!element.is_object()) {
			error = "missions.json entries must be objects";
			return false;
		}
		MissionDefinition mission;
		std::string typeError;
		bool present = false;
		if (!ReadString(element, "Event", mission.event, present, typeError) ||
			!ReadString(element, "Target", mission.target, present, typeError) ||
			!ReadInt(element, "Amount", mission.amount, typeError) ||
			!ReadString(element, "Phrase", mission.phrase, present, typeError) ||
			!ReadString(element, "RewardPhrase", mission.rewardPhrase, present, typeError) ||
			!ReadStringArray(element, "RewardCommands", mission.rewardCommands, typeError) ||
			!ReadEventProperties(element, mission.eventProperties, typeError)) {
			error = typeError;
			return false;
		}
		present = false;
		if (!ReadString(element, "MapName", mission.mapName, present, typeError)) {
			error = typeError;
			return false;
		}
		mission.hasMapName = present;
		present = false;
		if (!ReadString(element, "Flag", mission.flag, present, typeError)) {
			error = typeError;
			return false;
		}
		mission.hasFlag = present;
		missions.push_back(std::move(mission));
	}

	out = std::move(missions);
	return true;
}

std::string SerializeEventPropertiesJson(const EventProperties& properties) {
	if (properties.empty()) {
		return {};
	}
	nlohmann::json object = nlohmann::json::object();
	for (const auto& [key, value] : properties) {
		switch (value.kind) {
		case ValueKind::Bool: object[key] = value.boolean; break;
		case ValueKind::Int: object[key] = value.integer; break;
		case ValueKind::Double: object[key] = value.number; break;
		case ValueKind::String: object[key] = value.text; break;
		case ValueKind::Unsupported:
			try {
				object[key] = nlohmann::json::parse(value.text);
			} catch (const std::exception&) {
				object[key] = nullptr;
			}
			break;
		}
	}
	return object.dump();
}

bool ParseEventPropertiesJson(std::string_view json, EventProperties& out) {
	out.clear();
	if (json.empty()) {
		return true;
	}
	nlohmann::json object;
	try {
		object = nlohmann::json::parse(json.begin(), json.end(), nullptr, /*allow_exceptions=*/true,
									   /*ignore_comments=*/true);
	} catch (const std::exception&) {
		return false;
	}
	if (!object.is_object()) {
		return false;
	}
	EventProperties properties;
	for (auto it = object.begin(); it != object.end(); ++it) {
		properties.emplace_back(it.key(), ToMissionValue(it.value()));
	}
	out = std::move(properties);
	return true;
}

bool MatchesEventProperties(const EventProperties& filters, const EventPropertyLookup& lookup) {
	for (const auto& [key, missionValue] : filters) {
		if (missionValue.kind == ValueKind::Unsupported) {
			return false;
		}
		EventValue eventValue;
		if (!lookup || !lookup(key, missionValue.kind, eventValue)) {
			return false;
		}
		switch (missionValue.kind) {
		case ValueKind::Bool:
			if (eventValue.boolean != missionValue.boolean) {
				return false;
			}
			break;
		case ValueKind::Int:
			if (!(eventValue.integer >= missionValue.integer)) {
				return false;
			}
			break;
		case ValueKind::Double:
			if (!(eventValue.number >= missionValue.number)) {
				return false;
			}
			break;
		case ValueKind::String:
			if (!ContainsIgnoreCase(eventValue.text, missionValue.text)) {
				return false;
			}
			break;
		case ValueKind::Unsupported:
			return false;
		}
	}
	return true;
}

bool MissionMatches(const Mission& mission, std::string_view event, std::string_view target,
					std::string_view currentMap, const EventPropertyLookup* lookup) {
	if (mission.completed) {
		return false;
	}
	if (!EqualsIgnoreCase(mission.event, event) || !EqualsIgnoreCase(mission.target, target)) {
		return false;
	}
	if (mission.hasMapName && mission.mapName != currentMap) {
		return false;
	}
	// The original only evaluates the filters when the event carried a property dictionary; the
	// round_end handler passes null, so filters are ignored there.
	if (!mission.eventProperties.empty() && lookup != nullptr) {
		if (!MatchesEventProperties(mission.eventProperties, *lookup)) {
			return false;
		}
	}
	return true;
}

std::vector<std::size_t> AvailableMissionIndices(const std::vector<MissionDefinition>& definitions,
												 const std::vector<Mission>& current,
												 const std::function<bool(const std::string&)>& hasPermission) {
	std::set<std::string> existing;
	for (const Mission& mission : current) {
		existing.insert(MissionKey(mission));
	}
	std::vector<std::size_t> available;
	for (std::size_t index = 0; index < definitions.size(); ++index) {
		const MissionDefinition& definition = definitions[index];
		if (definition.hasFlag && (!hasPermission || !hasPermission(definition.flag))) {
			continue;
		}
		if (existing.count(MissionKey(definition)) != 0) {
			continue;
		}
		available.push_back(index);
	}
	return available;
}

std::vector<std::size_t> AdvanceMissionProgress(std::vector<Mission>& missions, std::string_view event,
												std::string_view target, std::string_view currentMap,
												const EventPropertyLookup* lookup) {
	std::vector<std::size_t> reached;
	for (std::size_t index = 0; index < missions.size(); ++index) {
		Mission& mission = missions[index];
		if (!MissionMatches(mission, event, target, currentMap, lookup)) {
			continue;
		}
		++mission.progress;
		if (mission.progress >= mission.amount) {
			reached.push_back(index);
		}
	}
	return reached;
}

std::vector<std::size_t> AdvancePlayTimeProgress(std::vector<Mission>& missions) {
	std::vector<std::size_t> reached;
	for (std::size_t index = 0; index < missions.size(); ++index) {
		Mission& mission = missions[index];
		if (mission.event != "PlayTime" || mission.completed) {
			continue;
		}
		++mission.progress;
		if (mission.progress >= mission.amount) {
			reached.push_back(index);
		}
	}
	return reached;
}

// ---------------------------------------------------------------------------------------------
// reset periods
// ---------------------------------------------------------------------------------------------

std::string CalculateExpirationDate(ResetMode mode, std::time_t now) {
	if (mode == ResetMode::PerMap || mode == ResetMode::Instant) {
		return {};
	}
	std::tm local {};
	localtime_r(&now, &local);
	local.tm_isdst = -1;

	std::time_t expiration = static_cast<std::time_t>(-1);
	switch (mode) {
	case ResetMode::Daily:
		local.tm_hour = 23;
		local.tm_min = 59;
		local.tm_sec = 59;
		expiration = std::mktime(&local); // today 23:59:59
		break;
	case ResetMode::Weekly: {
		// The original GetEndOfWeek(): DateTime.Now.Date.AddDays(daysUntilSunday).AddSeconds(-1),
		// i.e. the second before Sunday 00:00 - Saturday 23:59:59. A Sunday moves to the next one.
		int daysUntilSunday = (7 - local.tm_wday) % 7;
		if (daysUntilSunday == 0) {
			daysUntilSunday = 7;
		}
		local.tm_mday += daysUntilSunday;
		local.tm_hour = 0;
		local.tm_min = 0;
		local.tm_sec = 0;
		const std::time_t sunday = std::mktime(&local);
		if (sunday != static_cast<std::time_t>(-1)) {
			expiration = sunday - 1;
		}
		break;
	}
	case ResetMode::Monthly:
		local.tm_mday = DaysInMonth(local.tm_year + 1900, local.tm_mon + 1);
		local.tm_hour = 23;
		local.tm_min = 59;
		local.tm_sec = 59;
		expiration = std::mktime(&local);
		break;
	case ResetMode::PerMap:
	case ResetMode::Instant:
		return {};
	}
	if (expiration == static_cast<std::time_t>(-1)) {
		return {};
	}
	return FormatDateTime(expiration);
}

std::optional<std::time_t> ParseDateTime(std::string_view text) {
	const std::string trimmed = TrimSpaces(text);
	if (trimmed.size() < 10) {
		return std::nullopt;
	}
	int year = 0;
	int month = 0;
	int day = 0;
	int hour = 0;
	int minute = 0;
	int second = 0;
	char separator = 0;
	const int read = std::sscanf(trimmed.c_str(), "%d-%d-%d%c%d:%d:%d", &year, &month, &day, &separator, &hour,
								 &minute, &second);
	if (read != 3 && !(read == 7 && (separator == ' ' || separator == 'T'))) {
		return std::nullopt;
	}
	std::tm tm {};
	tm.tm_year = year - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_sec = second;
	tm.tm_isdst = -1;
	const std::time_t value = std::mktime(&tm);
	if (value == static_cast<std::time_t>(-1)) {
		return std::nullopt;
	}
	return value;
}

std::string FormatDateTime(std::time_t local) {
	std::tm tm {};
	localtime_r(&local, &tm);
	char buffer[32] = {};
	std::strftime(buffer, sizeof(buffer), kDateTimeFormat, &tm);
	return std::string(buffer);
}

Countdown TimeUntilExpiration(std::string_view expiresAt, std::time_t now) {
	Countdown result;
	const std::optional<std::time_t> expiration = ParseDateTime(expiresAt);
	if (!expiration.has_value()) {
		return result;
	}
	const int64_t remaining = static_cast<int64_t>(*expiration) - static_cast<int64_t>(now);
	if (remaining <= 0) {
		return result;
	}
	const int64_t totalHours = remaining / 3600;
	result.days = static_cast<int>(totalHours / 24);
	result.hours = static_cast<int>(totalHours % 24);
	result.minutes = static_cast<int>((remaining % 3600) / 60);
	return result;
}

// ---------------------------------------------------------------------------------------------
// phrases / placeholders
// ---------------------------------------------------------------------------------------------

std::string FormatPhrase(std::string_view phrase, const std::vector<std::string>& args) {
	std::string result(phrase);
	for (std::size_t index = 0; index < args.size(); ++index) {
		result = ReplaceAll(std::move(result), "{" + std::to_string(index) + "}", args[index]);
	}
	return result;
}

std::string ExpandRewardCommand(std::string_view command, const PlayerPlaceholders& player) {
	std::string result(command);
	result = ReplaceAll(std::move(result), "{slot}", std::to_string(player.slot));
	result = ReplaceAll(std::move(result), "{userid}", std::to_string(player.userid));
	result = ReplaceAll(std::move(result), "{name}", SanitizeCommandPlaceholder(player.name));
	result = ReplaceAll(std::move(result), "{steamid64}", std::to_string(player.steamid64));
	result = ReplaceAll(std::move(result), "{steamid}", std::to_string(player.steamid64));
	// The original also replaced the bare "u0022" spelling produced by JSON escaping.
	result = ReplaceAll(std::move(result), "u0022", "\"");
	return result;
}

std::string JsonEscape(std::string_view value) {
	std::string result;
	result.reserve(value.size() + 8);
	for (const char character : value) {
		switch (character) {
		case '\\': result += "\\\\"; break;
		case '"': result += "\\\""; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		default:
			if (static_cast<unsigned char>(character) < 0x20) {
				constexpr char hex[] = "0123456789abcdef";
				const auto byte = static_cast<unsigned char>(character);
				result += "\\u00";
				result.push_back(hex[byte >> 4]);
				result.push_back(hex[byte & 0x0f]);
			} else {
				result.push_back(character);
			}
			break;
		}
	}
	return result;
}

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------

std::string LowerAscii(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (const char character : text) {
		result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	}
	return result;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (std::tolower(static_cast<unsigned char>(left[index]))
			!= std::tolower(static_cast<unsigned char>(right[index]))) {
			return false;
		}
	}
	return true;
}

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
	if (needle.empty()) {
		return true;
	}
	if (needle.size() > haystack.size()) {
		return false;
	}
	const std::string loweredHaystack = LowerAscii(haystack);
	const std::string loweredNeedle = LowerAscii(needle);
	return loweredHaystack.find(loweredNeedle) != std::string::npos;
}

bool IsVipByName(std::string_view playerName, std::string_view domain) {
	if (domain.empty()) {
		return false;
	}
	return ContainsIgnoreCase(playerName, domain);
}

} // namespace k4
