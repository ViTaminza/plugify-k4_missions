#include "k4_store.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

namespace k4 {
namespace {

// DbMission.GetRewardCommandsList(): Split('|', RemoveEmptyEntries).
std::vector<std::string> SplitRewardCommands(std::string_view text) {
	std::vector<std::string> commands;
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t separator = text.find('|', start);
		const std::string_view part =
			separator == std::string_view::npos ? text.substr(start) : text.substr(start, separator - start);
		if (!part.empty()) {
			commands.emplace_back(part);
		}
		if (separator == std::string_view::npos) {
			break;
		}
		start = separator + 1;
	}
	return commands;
}

std::string JoinRewardCommands(const std::vector<std::string>& commands) {
	std::string joined;
	for (std::size_t index = 0; index < commands.size(); ++index) {
		if (index != 0) {
			joined.push_back('|');
		}
		joined += commands[index];
	}
	return joined;
}

bool IsDuplicateColumnError(std::string_view error) {
	const std::string lower = LowerAscii(error);
	return lower.find("duplicate column") != std::string::npos || lower.find("duplicate column name") != std::string::npos;
}

std::string CreateTableSql(bool mysql) {
	if (mysql) {
		return "CREATE TABLE IF NOT EXISTS k4_missions ("
			   "id INT NOT NULL AUTO_INCREMENT,"
			   "steamid64 BIGINT NOT NULL,"
			   "`event` VARCHAR(64) NOT NULL,"
			   "target VARCHAR(64) NOT NULL,"
			   "amount INT NOT NULL,"
			   "phrase VARCHAR(255) NOT NULL,"
			   "reward_phrase VARCHAR(255) NOT NULL,"
			   "reward_commands LONGTEXT NOT NULL,"
			   "progress INT NOT NULL DEFAULT 0,"
			   "completed TINYINT(1) NOT NULL DEFAULT 0,"
			   "expires_at DATETIME NULL,"
			   "event_properties LONGTEXT NULL,"
			   "map_name VARCHAR(64) NULL,"
			   "PRIMARY KEY (id),"
			   "KEY idx_steamid (steamid64),"
			   "KEY idx_expires_at (expires_at)"
			   ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
	}
	return "CREATE TABLE IF NOT EXISTS k4_missions ("
		   "id INTEGER PRIMARY KEY AUTOINCREMENT,"
		   "steamid64 INTEGER NOT NULL,"
		   "`event` VARCHAR(64) NOT NULL,"
		   "target VARCHAR(64) NOT NULL,"
		   "amount INTEGER NOT NULL,"
		   "phrase VARCHAR(255) NOT NULL,"
		   "reward_phrase VARCHAR(255) NOT NULL,"
		   "reward_commands TEXT NOT NULL,"
		   "progress INTEGER NOT NULL DEFAULT 0,"
		   "completed INTEGER NOT NULL DEFAULT 0,"
		   "expires_at DATETIME NULL,"
		   "event_properties TEXT NULL,"
		   "map_name VARCHAR(64) NULL"
		   ")";
}

std::string AlterEventPropertiesSql(bool mysql) {
	return mysql ? "ALTER TABLE k4_missions ADD COLUMN event_properties LONGTEXT NULL"
				 : "ALTER TABLE k4_missions ADD COLUMN event_properties TEXT NULL";
}

std::string AlterMapNameSql(bool mysql) {
	return mysql ? "ALTER TABLE k4_missions ADD COLUMN map_name VARCHAR(64) NULL"
				 : "ALTER TABLE k4_missions ADD COLUMN map_name VARCHAR(64) NULL";
}

// The migration of MissionStore::EnsureSchema(), shared by the blocking (tests) and the
// asynchronous (plugin) form so the two can never drift apart.
struct SchemaStep {
	std::string sql;
	std::string errorPrefix;
	enum class Failure {
		Fatal,           // the schema is unusable
		DuplicateColumn, // expected on a fresh table (MySQL has no ADD COLUMN IF NOT EXISTS)
		Tolerated,       // an old index of that name: the table is still usable, report a note
	} failure = Failure::Fatal;
};

std::vector<SchemaStep> SchemaSteps(bool mysql) {
	std::vector<SchemaStep> steps;
	steps.push_back({CreateTableSql(mysql), "k4_missions: cannot create the k4_missions table: ",
					 SchemaStep::Failure::Fatal});
	// M002 columns. The CREATE above already has them; an existing C# database may not, and on
	// that one the ALTER succeeds.
	steps.push_back({AlterEventPropertiesSql(mysql), "k4_missions: cannot apply the M002 migration: ",
					 SchemaStep::Failure::DuplicateColumn});
	steps.push_back({AlterMapNameSql(mysql), "k4_missions: cannot apply the M002 migration: ",
					 SchemaStep::Failure::DuplicateColumn});
	if (!mysql) {
		steps.push_back({"CREATE INDEX IF NOT EXISTS idx_steamid ON k4_missions (steamid64)",
						 "k4_missions: index creation reported: ", SchemaStep::Failure::Tolerated});
		steps.push_back({"CREATE INDEX IF NOT EXISTS idx_expires_at ON k4_missions (expires_at)",
						 "k4_missions: index creation reported: ", SchemaStep::Failure::Tolerated});
	}
	return steps;
}

struct SchemaMigration {
	plgdb::Database* database = nullptr;
	std::vector<SchemaStep> steps;
	std::size_t next = 0;
	std::string note;
	std::function<void(bool, std::string)> callback;
};

// Queues one statement and, from its completion callback, the next one. The state is kept alive
// by the in-flight statement's callback only (no self-referencing std::function cycle).
void AdvanceSchema(const std::shared_ptr<SchemaMigration>& state) {
	if (state->next >= state->steps.size()) {
		if (state->callback) {
			state->callback(true, std::move(state->note));
		}
		return;
	}
	const SchemaStep step = state->steps[state->next++];
	state->database->Query(step.sql, {}, [state, step](const plgdb::Result& result) {
		if (!result.Ok()) {
			bool fatal = false;
			switch (step.failure) {
			case SchemaStep::Failure::Fatal:
				fatal = true;
				break;
			case SchemaStep::Failure::DuplicateColumn:
				fatal = !IsDuplicateColumnError(result.Error());
				break;
			case SchemaStep::Failure::Tolerated:
				state->note = step.errorPrefix + result.Error();
				break;
			}
			if (fatal) {
				if (state->callback) {
					state->callback(false, step.errorPrefix + result.Error());
				}
				return;
			}
		}
		AdvanceSchema(state);
	});
}

} // namespace

bool MissionStore::EnsureSchema(std::string& error) {
	for (const SchemaStep& step : SchemaSteps(m_database.GetDriver() == plgdb::Driver::MySQL)) {
		const plgdb::Result result = m_database.QuerySync(step.sql);
		if (result.Ok()) {
			continue;
		}
		switch (step.failure) {
		case SchemaStep::Failure::Fatal:
			error = step.errorPrefix + result.Error();
			return false;
		case SchemaStep::Failure::DuplicateColumn:
			if (!IsDuplicateColumnError(result.Error())) {
				error = step.errorPrefix + result.Error();
				return false;
			}
			break;
		case SchemaStep::Failure::Tolerated:
			error = step.errorPrefix + result.Error();
			break;
		}
	}
	return true;
}

void EnsureSchemaAsync(plgdb::Database& database, std::function<void(bool, std::string)> callback) {
	auto state = std::make_shared<SchemaMigration>();
	state->database = &database;
	state->steps = SchemaSteps(database.GetDriver() == plgdb::Driver::MySQL);
	state->callback = std::move(callback);
	AdvanceSchema(state);
}

void MissionStore::LoadPlayer(uint64_t steamid64, std::function<void(bool ok, std::vector<Mission>)> callback) {
	const std::string sql = "SELECT id, steamid64, `event`, target, amount, phrase, reward_phrase, "
							"reward_commands, progress, completed, expires_at, event_properties, map_name "
							"FROM k4_missions WHERE steamid64 = ?";
	m_database.Query(sql, {static_cast<int64_t>(steamid64)},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 std::vector<Mission> missions;
						 if (!result.Ok()) {
							 callback(false, std::move(missions));
							 return;
						 }
						 const std::optional<std::size_t> idColumn = result.ColumnIndex("id");
						 const std::optional<std::size_t> eventColumn = result.ColumnIndex("event");
						 if (!idColumn.has_value() || !eventColumn.has_value()) {
							 // The table exists (a failed SELECT reports Ok() false), so this is a
							 // row-less read of a schema this build cannot use; keep the old
							 // "empty list" answer.
							 callback(true, std::move(missions));
							 return;
						 }
						 const std::size_t columnId = *idColumn;
						 const std::size_t columnEvent = *eventColumn;
						 const std::size_t columnTarget = result.ColumnIndex("target").value_or(columnEvent);
						 const std::size_t columnAmount = result.ColumnIndex("amount").value_or(columnEvent);
						 const std::size_t columnPhrase = result.ColumnIndex("phrase").value_or(columnEvent);
						 const std::size_t columnRewardPhrase = result.ColumnIndex("reward_phrase").value_or(columnEvent);
						 const std::size_t columnRewardCommands = result.ColumnIndex("reward_commands").value_or(columnEvent);
						 const std::size_t columnProgress = result.ColumnIndex("progress").value_or(columnEvent);
						 const std::size_t columnCompleted = result.ColumnIndex("completed").value_or(columnEvent);
						 const std::optional<std::size_t> columnExpires = result.ColumnIndex("expires_at");
						 const std::optional<std::size_t> columnEventProperties = result.ColumnIndex("event_properties");
						 const std::optional<std::size_t> columnMapName = result.ColumnIndex("map_name");

						 missions.reserve(result.RowCount());
						 for (std::size_t row = 0; row < result.RowCount(); ++row) {
							 Mission mission;
							 mission.id = result.GetInt(row, columnId);
							 mission.event = result.GetString(row, columnEvent);
							 mission.target = result.GetString(row, columnTarget);
							 mission.amount = static_cast<int>(result.GetInt(row, columnAmount));
							 mission.phrase = result.GetString(row, columnPhrase);
							 mission.rewardPhrase = result.GetString(row, columnRewardPhrase);
							 mission.rewardCommands = SplitRewardCommands(result.GetString(row, columnRewardCommands));
							 mission.progress = static_cast<int>(result.GetInt(row, columnProgress));
							 mission.completed = result.GetInt(row, columnCompleted) != 0;
							 if (columnExpires.has_value() && !result.IsNull(row, *columnExpires)) {
								 mission.hasExpiresAt = true;
								 mission.expiresAt = result.GetString(row, *columnExpires);
							 }
							 if (columnEventProperties.has_value() && !result.IsNull(row, *columnEventProperties)) {
								 ParseEventPropertiesJson(result.GetString(row, *columnEventProperties),
														  mission.eventProperties);
							 }
							 if (columnMapName.has_value() && !result.IsNull(row, *columnMapName)) {
								 mission.hasMapName = true;
								 mission.mapName = result.GetString(row, *columnMapName);
							 }
							 missions.push_back(std::move(mission));
						 }
						 callback(true, std::move(missions));
					 });
}

void MissionStore::InsertMission(uint64_t steamid64, const Mission& mission,
								 std::function<void(int64_t)> callback) {
	const std::string rewardCommands = JoinRewardCommands(mission.rewardCommands);
	const std::string eventProperties = SerializeEventPropertiesJson(mission.eventProperties);

	plgdb::Params params;
	params.emplace_back(static_cast<int64_t>(steamid64));
	params.emplace_back(mission.event);
	params.emplace_back(mission.target);
	params.emplace_back(static_cast<int64_t>(mission.amount));
	params.emplace_back(mission.phrase);
	params.emplace_back(mission.rewardPhrase);
	params.emplace_back(rewardCommands);
	params.emplace_back(static_cast<int64_t>(mission.progress));
	params.emplace_back(static_cast<int64_t>(mission.completed ? 1 : 0));
	if (mission.hasExpiresAt && !mission.expiresAt.empty()) {
		params.emplace_back(mission.expiresAt);
	} else {
		params.emplace_back(std::monostate {});
	}
	if (eventProperties.empty()) {
		params.emplace_back(std::monostate {});
	} else {
		params.emplace_back(eventProperties);
	}
	if (mission.hasMapName) {
		params.emplace_back(mission.mapName);
	} else {
		params.emplace_back(std::monostate {});
	}

	const std::string sql = "INSERT INTO k4_missions (steamid64, `event`, target, amount, phrase, "
							"reward_phrase, reward_commands, progress, completed, expires_at, "
							"event_properties, map_name) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
	m_database.Query(sql, std::move(params),
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 callback(result.Ok() ? static_cast<int64_t>(result.InsertId()) : -1);
					 });
}

void MissionStore::SaveMissions(const std::vector<Mission>& missions) {
	for (const Mission& mission : missions) {
		if (mission.id <= 0) {
			continue;
		}
		m_database.Query("UPDATE k4_missions SET progress = CASE WHEN progress > ? THEN progress ELSE ? END, "
						 "completed = CASE WHEN completed <> 0 THEN completed ELSE ? END WHERE id = ?",
						 {static_cast<int64_t>(mission.progress), static_cast<int64_t>(mission.progress),
						  static_cast<int64_t>(mission.completed ? 1 : 0), mission.id});
	}
}

void MissionStore::RemoveMissions(const std::vector<int64_t>& ids) {
	if (ids.empty()) {
		return;
	}
	std::string sql = "DELETE FROM k4_missions WHERE id IN (";
	plgdb::Params params;
	params.reserve(ids.size());
	for (std::size_t index = 0; index < ids.size(); ++index) {
		if (index != 0) {
			sql += ", ";
		}
		sql += "?";
		params.emplace_back(ids[index]);
	}
	sql += ")";
	m_database.Query(std::move(sql), std::move(params));
}

void MissionStore::MarkCompleted(int64_t missionId, std::function<void(bool)> callback) {
	if (missionId <= 0) {
		callback(false);
		return;
	}
	// The original read the row first and returned false when it was already completed (another
	// server sharing the database may have completed it): the conditional UPDATE keeps that.
	m_database.Query("UPDATE k4_missions SET completed = 1 WHERE id = ? AND completed = 0", {missionId},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 callback(result.Ok() && result.AffectedRows() > 0);
					 });
}

void MissionStore::PlayersWithExpiredMissions(const std::string& now,
											  std::function<void(bool ok, std::vector<uint64_t>)> callback) {
	m_database.Query("SELECT DISTINCT steamid64 FROM k4_missions WHERE expires_at IS NOT NULL AND expires_at < ?",
					 {now}, [callback = std::move(callback)](const plgdb::Result& result) {
						 std::vector<uint64_t> steamids;
						 if (!result.Ok()) {
							 callback(false, std::move(steamids));
							 return;
						 }
						 steamids.reserve(result.RowCount());
						 for (std::size_t row = 0; row < result.RowCount(); ++row) {
							 const int64_t value = result.GetInt(row, 0);
							 if (value > 0) {
								 steamids.push_back(static_cast<uint64_t>(value));
							 }
						 }
						 callback(true, std::move(steamids));
					 });
}

void MissionStore::CleanupExpiredMissions(const std::string& now) {
	m_database.Query("DELETE FROM k4_missions WHERE expires_at IS NOT NULL AND expires_at < ?", {now});
}

} // namespace k4
