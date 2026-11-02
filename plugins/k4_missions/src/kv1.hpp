// k4_missions — the read-only Valve KeyValues (KV1) reader the plugin needs for its own files.
//
// The plugin is self-contained on purpose (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no helper
// library), so it carries the one parser it needs. Three files are read with it:
//
//   databases.cfg           "Databases" { "k4_missions" { "driver" "mysql" ... } }
//   translations/*.phrases  "Phrases"   { "k4.missions.menu.title" { "en" "Missions" } }
//   core.cfg                "Core"      { "ServerLang" "en" }
//
// The grammar and the getter semantics are the ones the previous port relied on through
// pisexcompat's KeyValues wrapper:
//
//   file  := node* EOF
//   node  := token [ value ] [ '{' node* '}' ]     // a key may carry a value and a block
//   token := '"' escaped '"' | raw
//
//   * quoted and unquoted tokens, "// line" and "/* block */" comments, an optional UTF-8 BOM;
//   * escapes inside quoted strings: \" \\ \n \r \t (an unknown escape keeps its backslash);
//   * #base / #include / #define lines are skipped, never followed;
//   * find/lookup by child name is case-insensitive (Valve's KeyValues::FindKey);
//   * GetInt falls back to atoi semantics ("5 // comment", "1.5", "abc" -> 0);
//   * a file that is a single "Root" { ... } block is returned as that block, so
//     `ParseKvDocument(text)->Find("k4_missions")` reads a key inside "Databases" { ... };
//   * a malformed file (unterminated string, missing '}', stray top-level '}') is rejected as a
//     whole, so the caller keeps its defaults instead of using half a config.
//
// Engine-free and shared by the plugin and both test executables.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace k4 {

struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing, has no value or is a block.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The child's value as an int with the old wrapper's atoi fallback; `def` when missing/empty.
	int GetInt(std::string_view key, int def = 0) const;
};

// The whole document, or nullopt when the text is malformed. A single top-level block is
// unwrapped ("Databases" { ... } -> children are the connection sections).
std::optional<KvNode> ParseKvDocument(std::string_view text);

// The whole file, or nullopt when it cannot be opened. Kept separate from ParseKvDocument so the
// caller can tell "no file installed" (try the next candidate path) from "the file is malformed"
// (log it and keep the defaults).
std::optional<std::string> ReadKvFile(const std::string& path);

} // namespace k4
