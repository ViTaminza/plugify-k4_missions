// k4_missions — where the plugin reads its files (the previous port's path rules, kept exactly).
//
// HEAD resolved every file through pisexcompat's path helpers, which had this semantics:
//
//   ConfigDir        <plg::GetConfigsDir()>/k4_missions
//   TranslationsDir  <ConfigDir>/translations
//   DataDir          <plg::GetDataDir()>/k4_missions
//   JoinPath(a, b)   a + '/' + b (a trailing '/' on `a` is not doubled, a leading '/' on `b` dropped)
//
//   config.json / missions.json / discord/*.json   <ConfigDir>/<file>, read directly, no fallback.
//   phrases file    ResolveTranslationsPath(file): the raw CWD spelling first, then
//                   <TranslationsDir>/<file>, then <ConfigDir>/<file>; when none exists the
//                   <TranslationsDir> path is returned (so the error names the deployed location).
//   SQLite path     ResolveDataPath(path): "addons/data/<x>" loses the Metamod prefix and becomes
//                   <DataDir>/<x>; the path is used whether or not it exists yet.
//   databases.cfg   ResolvePath("addons/configs/databases.cfg"): the raw Metamod path relative to
//                   the working directory first (a legacy install keeps working), then the stripped
//                   relative name under <ConfigDir>, <TranslationsDir>, <DataDir>; the first file
//                   that *exists* is authoritative, otherwise <ConfigDir>/databases.cfg is reported.
//   core.cfg        <plg::GetConfigsDir()>/core.cfg (the shared server-config file, "ServerLang").
//
// The rules are kept byte-for-byte because production servers carry the files at those locations;
// the only difference is that no pisexcompat library is involved any more.
//
// Header-only and engine-free apart from the two Plugify host accessors (plg::GetConfigsDir /
// plg::GetDataDir), so the tests drive the same code with a fake host.

#pragma once

#include <plg/plugin.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace k4 {

inline std::string HostConfigsRoot() {
	if (plg::GetConfigsDir == nullptr) {
		return {};
	}
	try {
		const plg::string value = plg::GetConfigsDir();
		return std::string(value.data(), value.size());
	} catch (...) {
		return {};
	}
}

inline std::string HostDataRoot() {
	if (plg::GetDataDir == nullptr) {
		return {};
	}
	try {
		const plg::string value = plg::GetDataDir();
		return std::string(value.data(), value.size());
	} catch (...) {
		return {};
	}
}

inline std::string JoinPath(std::string_view a, std::string_view b) {
	if (a.empty()) {
		return std::string(b);
	}
	if (b.empty()) {
		return std::string(a);
	}
	std::string out(a);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (b.front() == '/') {
		b.remove_prefix(1);
	}
	out.append(b);
	return out;
}

inline bool StartsWith(std::string_view text, std::string_view prefix) {
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// <configs>/k4_missions; the plugin name is the folder name (PLUGIN_STANDARD §3).
inline const std::string& PluginConfigDir() {
	static const std::string dir = JoinPath(HostConfigsRoot(), "k4_missions");
	return dir;
}

inline const std::string& PluginTranslationsDir() {
	static const std::string dir = JoinPath(PluginConfigDir(), "translations");
	return dir;
}

inline const std::string& PluginDataDir() {
	static const std::string dir = JoinPath(HostDataRoot(), "k4_missions");
	return dir;
}

// <configs>/<file>: the plugin's config.json / missions.json.
inline std::string ConfigFilePath(std::string_view file) {
	return JoinPath(PluginConfigDir(), file);
}

// The shared server config the Pisex language detection read.
inline std::string CoreConfigPath() {
	return JoinPath(HostConfigsRoot(), "core.cfg");
}

inline bool FileExists(std::string_view path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(std::string(path), std::ios::binary);
	return file.good();
}

inline std::string ReadTextFile(std::string_view path, bool* ok) {
	std::string content;
	std::ifstream file(std::string(path), std::ios::binary);
	if (!file) {
		if (ok != nullptr) {
			*ok = false;
		}
		return content;
	}
	content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	if (ok != nullptr) {
		*ok = true;
	}
	return content;
}

// The first candidate that exists on disk; `fallback` when none does.
inline std::string FirstExisting(const std::vector<std::string>& candidates, const std::string& fallback) {
	for (const std::string& candidate : candidates) {
		if (!candidate.empty() && FileExists(candidate)) {
			return candidate;
		}
	}
	return fallback;
}

// pisexcompat's ResolveTranslationsPath(): the raw spelling first (a file in the working directory
// wins, exactly like the old wrapper), then the deployed translations directory, then the plugin
// config directory. The file name may be given with or without the ".txt" suffix.
inline std::string ResolvePhrasesPath(std::string_view file) {
	if (file.empty()) {
		return {};
	}
	std::string name(file);
	if (name.size() < 4 || name.compare(name.size() - 4, 4, ".txt") != 0) {
		name += ".txt";
	}
	std::vector<std::string> candidates;
	candidates.push_back(name); // raw, relative to the server working directory
	if (StartsWith(name, "addons/translations/")) {
		candidates.push_back(JoinPath(PluginTranslationsDir(), name.substr(std::string_view("addons/translations/").size())));
	} else {
		candidates.push_back(JoinPath(PluginTranslationsDir(), name));
		candidates.push_back(JoinPath(PluginConfigDir(), name));
	}
	return FirstExisting(candidates, candidates.size() > 1 ? candidates[1] : candidates.front());
}

// pisexcompat's ResolveDataPath(): "addons/data/<x>" is the Metamod spelling of <data>/<x>.
inline std::string ResolveDataPath(std::string_view path) {
	if (path.empty()) {
		return {};
	}
	std::string file(path);
	constexpr std::string_view kDataPrefix = "addons/data/";
	if (StartsWith(file, kDataPrefix)) {
		file = std::string(std::string_view(file).substr(kDataPrefix.size()));
	}
	return JoinPath(PluginDataDir(), file);
}

// pisexcompat's ResolvePath("addons/configs/databases.cfg"): the raw legacy path first, then the
// stripped relative name in the config, translations and data directories. The first existing
// file wins; when none exists the plugin-config candidate is reported (and the caller falls back
// to SQLite), so the log line names the file the operator has to install.
inline std::string ResolveDatabasesPath() {
	constexpr std::string_view kLegacy = "addons/configs/databases.cfg";
	const std::vector<std::string> candidates = {
		std::string(kLegacy),                         // a Metamod install keeps reading its own file
		JoinPath(PluginConfigDir(), "databases.cfg"), // the PLUGIN_STANDARD location
		JoinPath(PluginTranslationsDir(), "databases.cfg"),
		JoinPath(PluginDataDir(), "databases.cfg"),
	};
	return FirstExisting(candidates, candidates[1]);
}

} // namespace k4
