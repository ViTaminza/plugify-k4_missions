// k4_missions package regression — the public zip must never contain live secrets.
//
// The package ships the original config.json / missions.json, the three Discord templates and the
// credential-less databases.cfg.example (key names only, never a real host/user/pass). It must
// never ship a databases.cfg: the deploy merges configs with `cp -n`, so a placeholder file would
// take the place of the operator's real connection.
//
// The test runs the real package.sh from an isolated copy of the plugin directory (no ref/ at
// all), with a temp BUILD_DIR/OUT_DIR and fake build artifacts:
//   * the package succeeds without ref/ and ships the checked-in files byte-identically;
//   * there is no databases.cfg in the package, and the example's credential fields are empty;
//   * neither package.sh nor the shipped configs contain a real password/token/webhook URL;
//   * package.sh's source has no ref/ or live-configs reference;
//   * an existing server config is never modified (the script only writes OUT_DIR);
//   * OUT_DIR is refused unless it is a strict subdirectory of BUILD_DIR (outside dir, BUILD_DIR
//     itself, `/`, a symlink escape and a server-root hook are all rejected *before* rm -rf);
//   * missing build artifacts fail cleanly and create no output.
//
// No engine, no bindings, no network. Runs from build/tests via ./test.sh.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL %s\n", what.c_str());
	}
}

std::string Quote(const std::string& text) {
	std::string quoted = "'";
	for (const char c : text) {
		if (c == '\'') {
			quoted += "'\\''";
		} else {
			quoted += c;
		}
	}
	quoted += "'";
	return quoted;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << content;
}

void CopyFile(const std::filesystem::path& from, const std::filesystem::path& to) {
	std::filesystem::create_directories(to.parent_path());
	std::error_code error;
	std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, error);
	if (error) {
		std::printf("FAIL: cannot copy %s -> %s: %s\n", from.c_str(), to.c_str(), error.message().c_str());
		++g_failures;
	}
}

std::string Lower(std::string_view text) {
	std::string out(text);
	for (char& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

int RunPackage(const std::filesystem::path& script, const std::filesystem::path& buildDir,
			   const std::filesystem::path& outDir, const std::filesystem::path& logFile,
			   const std::string& envPrefix = {}) {
	std::string command = envPrefix;
	command += "BUILD_DIR=" + Quote(buildDir.string()) + " OUT_DIR=" + Quote(outDir.string()) + " /bin/bash "
			   + Quote(script.string()) + " >" + Quote(logFile.string()) + " 2>&1";
	const int status = std::system(command.c_str());
	if (status == -1) {
		return -1;
	}
	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	return 128;
}

void PrintLogOnFailure(int exitCode, const std::filesystem::path& logFile, std::string_view what) {
	if (exitCode == 0) {
		return;
	}
	std::printf("package.sh failed for %s (exit %d):\n", std::string(what).c_str(), exitCode);
	if (const auto log = ReadFile(logFile)) {
		std::printf("%s\n", log->c_str());
	}
}

bool ContainsAnySecret(std::string_view text) {
	const std::string lower = Lower(text);
	for (const char* needle : {"password", "supersecret", "discord.com/api/webhooks", "\"token\"",
							   "\"webhookurl\": \"http"}) {
		if (lower.find(Lower(needle)) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// The example file's credential values must be empty or a harmless default.
bool CredentialsEmpty(std::string_view example) {
	for (const char* field : {"\"host\"", "\"user\"", "\"pass\""}) {
		const std::size_t key = example.find(field);
		if (key == std::string_view::npos) {
			continue;
		}
		const std::size_t open = example.find('"', key + std::string_view(field).size());
		if (open == std::string_view::npos) {
			continue;
		}
		const std::size_t close = example.find('"', open + 1);
		if (close == std::string_view::npos) {
			continue;
		}
		const std::string_view value = example.substr(open + 1, close - open - 1);
		if (!value.empty() && value != "127.0.0.1") {
			return false;
		}
	}
	return true;
}

void TestPackageLayout(const std::filesystem::path& outDir, const std::filesystem::path& pluginDir) {
	for (const char* relative :
		 {"k4_missions.pplugin", "bin/libk4_missions.so", "configs/k4_missions/config.json",
		  "configs/k4_missions/missions.json", "configs/k4_missions/databases.cfg.example",
		  "configs/k4_missions/discord/complete.json", "configs/k4_missions/discord/complete_all.json",
		  "configs/k4_missions/discord/reset.json",
		  "configs/k4_missions/translations/k4_missions.phrases.txt"}) {
		Check(std::filesystem::is_regular_file(outDir / relative), std::string("the package contains ") + relative);
	}
	// The real connection file must NEVER be shipped.
	Check(!std::filesystem::exists(outDir / "configs/k4_missions/databases.cfg"),
		  "the package does not ship databases.cfg");
	for (const char* relative : {"config.json", "missions.json", "databases.cfg.example",
								 "discord/complete.json"}) {
		const auto packaged = ReadFile(outDir / "configs/k4_missions" / relative);
		const auto source = ReadFile(pluginDir / "assets" / relative);
		Check(source.has_value(), std::string("the checked-in ") + relative + " exists");
		Check(packaged.has_value() && source.has_value() && *packaged == *source,
			  std::string("the package ships ") + relative + " byte-identically");
	}
	const auto example = ReadFile(outDir / "configs/k4_missions/databases.cfg.example");
	if (example) {
		Check(CredentialsEmpty(*example), "the databases template has empty credentials");
		Check(example->find("\"pass\"") != std::string::npos, "the template keeps the key names");
		Check(example->find("driver") != std::string::npos, "the template carries the driver key");
	}
	const auto config = ReadFile(outDir / "configs/k4_missions/config.json");
	if (config) {
		const std::size_t key = config->find("\"WebhookUrl\"");
		Check(key != std::string::npos, "the shipped config has the webhook key");
		const std::size_t colon = key == std::string::npos ? key : config->find(':', key);
		const std::size_t open = colon == std::string::npos ? colon : config->find('"', colon);
		const std::size_t close = open == std::string::npos ? open : config->find('"', open + 1);
		Check(open != std::string::npos && close == open + 1, "the shipped config's webhook URL is empty");
	}
}

void TestScriptSource(const std::filesystem::path& pluginDir) {
	const auto script = ReadFile(pluginDir / "package.sh");
	Check(script.has_value(), "package.sh is readable");
	if (!script) {
		return;
	}
	Check(script->find("ref/") == std::string::npos, "package.sh does not read ref/");
	Check(script->find("live-configs") == std::string::npos, "package.sh does not read live configs");
	Check(script->find("server-configs") == std::string::npos, "package.sh does not read server configs");
	Check(script->find("realpath") != std::string::npos, "package.sh resolves OUT_DIR through realpath");
	Check(script->find("databases.cfg.example") != std::string::npos, "package.sh ships the example template");
	Check(script->find("cp -n") != std::string::npos, "package.sh documents the no-clobber deploy rule");
}

void TestGuards(const std::filesystem::path& script, const std::filesystem::path& root,
				const std::filesystem::path& buildDir, const std::filesystem::path& logFile) {
	// A directory outside BUILD_DIR is refused before anything is created.
	const std::filesystem::path outside = root / "outside";
	std::error_code error;
	std::filesystem::remove_all(outside, error);
	int exitCode = RunPackage(script, buildDir, outside, logFile);
	PrintLogOnFailure(exitCode, logFile, "OUT outside BUILD_DIR");
	Check(exitCode != 0, "OUT_DIR outside BUILD_DIR is refused");
	Check(!std::filesystem::exists(outside), "the refused OUT_DIR is not created");

	// BUILD_DIR itself and `/` are refused.
	exitCode = RunPackage(script, buildDir, buildDir, logFile);
	Check(exitCode != 0, "OUT_DIR == BUILD_DIR is refused");
	exitCode = RunPackage(script, buildDir, "/", logFile);
	Check(exitCode != 0, "OUT_DIR == / is refused");

	// A symlink inside the build tree pointing outside is resolved and refused.
	const std::filesystem::path escape = root / "escape";
	std::filesystem::create_directories(escape);
	std::filesystem::create_directory_symlink(escape, buildDir / "package-link", error);
	if (!error) {
		exitCode = RunPackage(script, buildDir, buildDir / "package-link" / "k4_missions", logFile);
		Check(exitCode != 0, "a symlink escape is refused");
		std::filesystem::remove(buildDir / "package-link", error);
	}
	std::vector<std::filesystem::path> escapeContents;
	for (const auto& entry : std::filesystem::directory_iterator(escape)) {
		escapeContents.push_back(entry.path());
	}
	Check(escapeContents.empty(), "nothing is written through the symlink escape");

	// A server-root hook refuses an otherwise valid path inside the server tree.
	const std::filesystem::path serverTree = root / "server-tree";
	std::filesystem::create_directories(serverTree / "build");
	const std::string env = "K4_MISSIONS_SERVER_ROOTS=" + Quote(serverTree.string()) + " ";
	exitCode = RunPackage(script, serverTree / "build", serverTree / "build" / "package" / "k4_missions", logFile,
						  env);
	Check(exitCode != 0, "an OUT_DIR inside K4_MISSIONS_SERVER_ROOTS is refused");

	// Missing artifacts fail cleanly and create no output.
	const std::filesystem::path emptyBuild = root / "empty-build";
	std::filesystem::create_directories(emptyBuild);
	exitCode = RunPackage(script, emptyBuild, emptyBuild / "package" / "k4_missions", logFile);
	Check(exitCode != 0, "missing build artifacts fail");
	Check(!std::filesystem::exists(emptyBuild / "package"), "no output appears without artifacts");
}

} // namespace

int main() {
	std::printf("k4_missions package tests (isolated, no ref/live configs)\n");

	const std::filesystem::path sourceDir = K4_MISSIONS_TEST_SOURCE_DIR;
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / ("k4_missions_package_tests_" + std::to_string(::getpid()));
	std::error_code error;
	std::filesystem::remove_all(root, error);

	// An isolated plugin tree: only package.sh, assets/ and translations/, with no ref/ at all.
	const std::filesystem::path repo = root / "repo";
	const std::filesystem::path pluginDir = repo / "plugins" / "k4_missions";
	for (const char* relative : {"package.sh", "assets/config.json", "assets/missions.json",
								 "assets/databases.cfg.example", "assets/discord/complete.json",
								 "assets/discord/complete_all.json", "assets/discord/reset.json",
								 "translations/k4_missions.phrases.txt"}) {
		CopyFile(sourceDir / relative, pluginDir / relative);
	}
	const std::filesystem::path script = pluginDir / "package.sh";
	Check(!std::filesystem::exists(repo / "ref"), "the isolated tree has no ref/ (nothing to fall back to)");

	const std::filesystem::path buildDir = repo / "build";
	const std::filesystem::path outDir = buildDir / "package" / "k4_missions";
	const std::filesystem::path logFile = root / "package.log";
	WriteFile(buildDir / "plugins" / "k4_missions" / "libk4_missions.so", "fake shared object\n");
	WriteFile(buildDir / "plugins" / "k4_missions" / "k4_missions.pplugin", "{\"name\":\"k4_missions\"}\n");

	// A pre-existing server config, as a deployment target would hold it. package.sh must never
	// touch it; it only writes OUT_DIR.
	const std::filesystem::path serverConfig = root / "server-configs" / "k4_missions" / "databases.cfg";
	const std::string serverConfigContent =
		"\"Databases\"\n{\n    \"k4_missions\"\n    {\n        \"host\" \"10.0.0.5\"\n"
		"        \"pass\" \"live-only-marker\"\n    }\n}\n";
	WriteFile(serverConfig, serverConfigContent);

	// 1. The normal isolated run.
	int exitCode = RunPackage(script, buildDir, outDir, logFile);
	PrintLogOnFailure(exitCode, logFile, "the temp build/output run");
	Check(exitCode == 0, "package.sh succeeds without ref/ and with fake artifacts");
	TestPackageLayout(outDir, pluginDir);
	TestScriptSource(pluginDir);
	Check(ReadFile(serverConfig).value_or("") == serverConfigContent,
		  "package.sh does not touch an existing server config");

	// 2. The output guard tests (they all run before rm -rf).
	TestGuards(script, root, buildDir, logFile);

	// 3. A second run overwrites its own OUT_DIR but still never the server config.
	exitCode = RunPackage(script, buildDir, outDir, logFile);
	Check(exitCode == 0, "package.sh is re-runnable");
	Check(ReadFile(serverConfig).value_or("") == serverConfigContent,
		  "the second run still leaves the server config alone");

	// 4. The shipped files carry no secret at all.
	for (const char* relative : {"assets/config.json", "assets/missions.json", "assets/databases.cfg.example",
								 "assets/discord/complete.json", "assets/discord/complete_all.json",
								 "assets/discord/reset.json"}) {
		const auto text = ReadFile(pluginDir / relative);
		Check(text.has_value(), std::string("shipped file exists: ") + relative);
		if (text) {
			Check(!ContainsAnySecret(*text), std::string("no secret in ") + relative);
		}
	}

	std::filesystem::remove_all(root, error);
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
