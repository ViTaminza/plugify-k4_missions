// k4_missions — the k4_missions table on plgdb.
//
// The table, its columns and the pipe-separated reward_commands encoding are the original's
// (Migrations M001/M002, Models/DbMission.cs), so a production database written by the Metamod
// server keeps working. The port uses one idempotent CREATE TABLE IF NOT EXISTS plus an ALTER for
// the two M002 columns instead of FluentMigrator (plgdb has no migration runner).
//
// PostgreSQL is not supported: plgdb only speaks MySQL/MariaDB and SQLite (documented deviation).

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <plgdb/db.hpp>

#include "k4_core.hpp"

namespace k4 {

class MissionStore {
public:
	explicit MissionStore(plgdb::Database& database) : m_database(database) {}

	// Creates the table and adds the M002 columns to a table created by an older version.
	// Blocking (QuerySync): used by the unit tests; the plugin runs the same statements through
	// EnsureSchemaAsync() so a slow or unreachable database never stalls the game thread.
	bool EnsureSchema(std::string& error);

	// SELECT of every row of the player (the original's SelectAsync by steamid64). `ok` is false
	// when the statement failed: that must not be treated as "the player has no missions".
	void LoadPlayer(uint64_t steamid64, std::function<void(bool ok, std::vector<Mission>)> callback);

	// INSERT with the original column set; the callback gets the new id (-1 on failure/loss).
	void InsertMission(uint64_t steamid64, const Mission& mission, std::function<void(int64_t)> callback);

	// The original UpdateMissionsAsync(): progress and completed of every row with an id. The
	// statement keeps the stored maximum progress and never clears `completed`: a snapshot saved
	// from this server may be older than a row another server of a shared database just advanced,
	// and overwriting it would allow the completion claim to succeed (and pay the reward) twice.
	void SaveMissions(const std::vector<Mission>& missions);

	// DELETE by id list.
	void RemoveMissions(const std::vector<int64_t>& ids);

	// UPDATE completed = 1; callback true when the statement succeeded.
	void MarkCompleted(int64_t missionId, std::function<void(bool)> callback);

	// SELECT DISTINCT steamid64 WHERE expires_at < now. `ok` is false when the statement failed:
	// that must not be treated as "no rows expired on any server".
	void PlayersWithExpiredMissions(const std::string& now,
									std::function<void(bool ok, std::vector<uint64_t>)> callback);

	// DELETE WHERE expires_at < now.
	void CleanupExpiredMissions(const std::string& now);

private:
	plgdb::Database& m_database;
};

// Runs the same CREATE/ALTER migration as MissionStore::EnsureSchema() on plgdb's worker thread;
// the callback runs on the game thread from Database::Poll() with ok=false and the error when a
// required statement failed. Never blocks the caller, so a blackholed MySQL cannot hold the server
// main thread (the watchdog) during plugin start or `plg reload`.
void EnsureSchemaAsync(plgdb::Database& database, std::function<void(bool ok, std::string error)> callback);

} // namespace k4
