// k4_missions plugin tests — no engine: the plugin TU is compiled in (its handlers live in an
// anonymous namespace), every __s2sdk_* / __permissions_* / __hudmenu_* / __vip_* binding slot is a
// recording fake, and the database half runs against a real throwaway SQLite file through plgdb.
//
// Covered:
//   * PITFALLS B1/B2 — the map gate: no client/entity/schema call in OnPluginStart, before the first
//     map, in the window between OnMapEnd and the next OnMapStart, or after OnPluginEnd; the
//     mid-map latch of the first OnPluginUpdate only with the four map-listener bindings, no
//     OnMapEnd since start and GetCurrentMap() naming a real map ("", "<empty>" and a missing
//     binding never open it);
//   * PITFALLS C1/C2 — slot range, connected / in-game / SourceTV / fake / controller layers, the
//     "<unknown>" name, and a negative-but-valid CEntityHandle (index | serial << 15) used for
//     {userid} through EntHandleToEntIndex;
//   * PITFALLS C8/C9 — the schema offset is resolved only behind the gate, cached only when >= 0,
//     and no s2sdk timer is used: the one-second reconcile, the minute playtime/expiry ticks and
//     the retry clocks are driven by OnPluginUpdate(dt);
//   * the whole mission flow through the real command/event/menu callbacks and the real SQLite
//     store: assignment, progress, completion, the reward ServerCommand with placeholders, Instant
//     re-roll, round_end/winner, playtime, filters, warmup, VIP amounts, permissions, the
//     bomb_pickup player_pawn field;
//   * identity: every deferred action, menu action and async DB callback re-checks SteamID64 +
//     controller handle, so a reused slot never sees the previous player's menu, chat or reward;
//   * the command/hook/menu lifecycles (A7/C10/C11): the complete register+unregister pairs are
//     required before anything is registered, the server console (caller == -1) is refused, a
//     pre-existing command only loses its listener, the reserved menu id 0 is burned, and the
//     map-end/unload teardown cancels sessions before DestroyMenu (A7_UNRESOLVED when it cannot);
//   * secrets: the webhook URL/token never appears in a log line;
//   * fail-soft: every binding null, malformed/missing configs, an unsupported database driver, an
//     unreachable SQLite path, a missing vip/permissions/hudmenu.
//
// Run by ./test.sh (build/tests/k4_missions_plugin_tests).

#include "../src/plugin.cpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                           \
		}                                                                                          \
	} while (false)

void CheckEq(std::string_view actual, std::string_view expected, const char* what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s: got '%s', expected '%s'\n", what, std::string(actual).c_str(),
					std::string(expected).c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const char* what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s: got %lld, expected %lld\n", what, static_cast<long long>(actual),
					static_cast<long long>(expected));
	}
}

std::string ToStdString(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// =============================================================================================
// fake plugify host
// =============================================================================================

std::filesystem::path g_root;
std::filesystem::path g_configsRoot;
std::filesystem::path g_dataRoot;
std::string g_serverLanguage = "en";

plg::string FakeGetConfigsDir() {
	return plg::string(g_configsRoot.string().c_str());
}
plg::string FakeGetDataDir() {
	return plg::string(g_dataRoot.string().c_str());
}
plg::string FakeGetServerLanguage() {
	return plg::string(g_serverLanguage.c_str());
}

struct LogEntry {
	plg::Severity severity = plg::Severity::Info;
	std::string message;
};
std::vector<LogEntry> g_logs;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location&) {
	g_logs.push_back(LogEntry{severity, std::string(message)});
}

bool Logged(plg::Severity severity, std::string_view needle) {
	for (const LogEntry& entry : g_logs) {
		if (entry.severity == severity && entry.message.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool AnyLogContains(std::string_view needle) {
	for (const LogEntry& entry : g_logs) {
		if (entry.message.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// =============================================================================================
// fake map lifecycle
// =============================================================================================

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;
std::string g_currentMap;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
	++g_mapStartRegisters;
}
void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
	++g_mapStartUnregisters;
}
void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
	++g_mapEndRegisters;
}
void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
	++g_mapEndUnregisters;
}

void FireMapStart() {
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}
void FireMapEnd() {
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

plg::string FakeGetCurrentMap() {
	return plg::string(g_currentMap.c_str());
}

// =============================================================================================
// fake clients
// =============================================================================================

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool fake = false;
	bool hasController = false;
	bool hasPawn = false;
	int32_t controllerHandle = 0; // 0: derive from slot; may be set to a negative valid handle
	bool forceNoHandle = false;
	int32_t team = 0;
	uint64_t steamid = 0;
	std::string name = "Player";
};

std::array<FakeClient, 64> g_clients;
int g_clientCalls = 0;
int g_steamIdCalls[64] = {};

const FakeClient& Slot(int32_t slot) {
	static const FakeClient kEmpty;
	return slot >= 0 && slot < 64 ? g_clients[static_cast<std::size_t>(slot)] : kEmpty;
}

// index | serial << 15 (the low 15 bits are the entity index; the serial makes it unique).
int32_t MakeHandle(int32_t slot, int32_t serial = 1) {
	return (serial << 15) | (slot + 1);
}
// A handle whose serial >= 0x8000: bit 31 is set, so the int32 is negative but the handle is VALID.
int32_t MakeNegativeHandle(int32_t slot) {
	return static_cast<int32_t>(0x80000000u | static_cast<uint32_t>(slot + 1));
}

int32_t FakeEntHandleToEntIndex(int32_t handle) {
	if (handle == 0 || handle == -1) {
		return -1;
	}
	return handle & 0x7FFF;
}
int32_t FakeEntPointerToEntIndex(void* entity) {
	if (entity == nullptr) {
		return 0;
	}
	return static_cast<int32_t>(reinterpret_cast<uintptr_t>(entity) & 0xFFFF);
}
bool FakeIsValidEntHandle(int32_t handle) {
	return handle != 0 && handle != -1;
}
bool FakeIsValidEntPointer(void* entity) {
	return entity != nullptr;
}

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	return slot >= 0 && slot < 64 && Slot(slot).connected;
}
bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	return slot >= 0 && slot < 64 && Slot(slot).inGame;
}
bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	return slot >= 0 && slot < 64 && Slot(slot).sourceTV;
}
bool FakeIsFakeClient(int32_t slot) {
	++g_clientCalls;
	return slot >= 0 && slot < 64 && Slot(slot).fake;
}
bool FakeIsClientAlive(int32_t slot) {
	++g_clientCalls;
	return slot >= 0 && slot < 64 && Slot(slot).inGame;
}
uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_clientCalls;
	if (slot >= 0 && slot < 64) {
		++g_steamIdCalls[static_cast<std::size_t>(slot)];
	}
	return slot >= 0 && slot < 64 ? Slot(slot).steamid : 0;
}
plg::string FakeGetClientName(int32_t slot) {
	++g_clientCalls;
	return plg::string(Slot(slot).name.c_str());
}
s2sdk::CSTeam FakeGetClientTeam(int32_t slot) {
	++g_clientCalls;
	return static_cast<s2sdk::CSTeam>(Slot(slot).team);
}
void* FakeGetClientPawn(int32_t slot) {
	++g_clientCalls;
	if (!Slot(slot).hasPawn) {
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<uintptr_t>(0x2000) + static_cast<uintptr_t>(slot));
}
int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_clientCalls;
	if (slot < 0 || slot >= 64 || !Slot(slot).hasController || Slot(slot).forceNoHandle) {
		return 0;
	}
	if (Slot(slot).controllerHandle != 0) {
		return Slot(slot).controllerHandle;
	}
	return MakeHandle(slot);
}
void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_clientCalls;
	if (slot < 0 || slot >= 64 || !Slot(slot).hasController) {
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000) + static_cast<uintptr_t>(slot));
}
int32_t FakeClientPtrToPlayerSlot(void* client) {
	if (client == nullptr) {
		return -1;
	}
	const auto value = static_cast<int32_t>(reinterpret_cast<uintptr_t>(client) & 0xFFFF);
	if (value >= 0x2000 && value < 0x2040) {
		return value - 0x2000; // the pawn pointer encodes the slot
	}
	if (value >= 0x1000 && value < 0x1040) {
		return value - 0x1000; // the controller pointer encodes the slot
	}
	return -1;
}
int32_t FakeGetMaxClients() {
	return 64;
}

int32_t FakeTeamOffsetValue = 100;
int g_schemaOffsetCalls = 0;
int32_t FakeGetSchemaOffset(const plg::string& /*className*/, const plg::string& /*memberName*/) {
	++g_schemaOffsetCalls;
	return FakeTeamOffsetValue;
}
int64_t FakeGetEntData(int32_t handle, int32_t offset, int32_t size) {
	const int32_t index = FakeEntHandleToEntIndex(handle);
	const int32_t slot = index - 1;
	if (slot < 0 || slot >= 64 || size != 1 || offset != FakeTeamOffsetValue) {
		return 0;
	}
	return static_cast<int64_t>(g_clients[static_cast<std::size_t>(slot)].team) & 0xFF;
}
int64_t FakeGetEntData2(void* entity, int32_t offset, int32_t size) {
	if (entity == nullptr || size != 1 || offset != FakeTeamOffsetValue) {
		return 0;
	}
	const auto value = static_cast<int32_t>(reinterpret_cast<uintptr_t>(entity) & 0xFF);
	if (value < 0 || value >= 64) {
		return 0;
	}
	return static_cast<int64_t>(g_clients[static_cast<std::size_t>(value)].team) & 0xFF;
}

// =============================================================================================
// fake commands / listeners
// =============================================================================================

struct FakeCommand {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};
std::vector<FakeCommand> g_commands;
struct FakeListener {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};
std::vector<FakeListener> g_listeners;
std::vector<std::string> g_removedCommands;
std::vector<std::string> g_removedListeners;
int g_addConsoleCommandCalls = 0;
bool g_addConsoleCommandReturnsCreated = true;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& /*description*/,
						   s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback callback,
						   s2sdk::HookMode mode) {
	++g_addConsoleCommandCalls;
	g_commands.push_back(FakeCommand{ToStdString(name), callback, flags, mode});
	return g_addConsoleCommandReturnsCreated;
}
bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback callback) {
	const std::string text = ToStdString(name);
	g_removedCommands.push_back(text);
	g_commands.erase(std::remove_if(g_commands.begin(), g_commands.end(),
									[&](const FakeCommand& command) {
										return command.name == text && command.callback == callback;
									}),
					 g_commands.end());
	return true;
}
bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::HookMode mode) {
	g_listeners.push_back(FakeListener{ToStdString(name), callback, mode});
	return true;
}
bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							   s2sdk::HookMode mode) {
	const std::string text = ToStdString(name);
	g_removedListeners.push_back(text);
	g_listeners.erase(std::remove_if(g_listeners.begin(), g_listeners.end(),
									 [&](const FakeListener& listener) {
										 return listener.name == text && listener.callback == callback
												&& listener.mode == mode;
									 }),
					  g_listeners.end());
	return true;
}

const FakeCommand* FindCommand(const std::string& name) {
	for (const FakeCommand& command : g_commands) {
		if (command.name == name) {
			return &command;
		}
	}
	return nullptr;
}

s2sdk::ResultType FireCommand(const std::string& name, int32_t caller, s2sdk::ConCommandContext context) {
	const FakeCommand* command = FindCommand(name);
	if (command == nullptr || command->callback == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	plg::vector<plg::string> args;
	args.push_back(plg::string(name.c_str()));
	return command->callback(caller, context, args);
}

// =============================================================================================
// fake events
// =============================================================================================

struct FakeEventField {
	enum class Kind { Bool, Int, Float, String, Slot, Pawn } kind = Kind::Int;
	bool boolean = false;
	int32_t integer = 0;
	float number = 0.0f;
	std::string text;
	int32_t slot = -1;
	int32_t pawnSlot = -1;
};

struct FakeEvent {
	std::string name;
	std::map<std::string, FakeEventField> fields;
};

FakeEvent g_event;
std::vector<std::pair<std::string, s2sdk::EventCallback>> g_eventHooks;
std::vector<std::string> g_unhookedEvents;
std::map<std::string, int> g_eventIntReads;
int g_eventPawnCalls = 0;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback,
									s2sdk::HookMode mode) {
	if (mode != s2sdk::HookMode::Post) {
		return s2sdk::EventHookError::InvalidEvent;
	}
	g_eventHooks.emplace_back(ToStdString(name), callback);
	return s2sdk::EventHookError::Okay;
}
s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback,
									  s2sdk::HookMode /*mode*/) {
	g_unhookedEvents.push_back(ToStdString(name));
	g_eventHooks.erase(std::remove_if(g_eventHooks.begin(), g_eventHooks.end(),
									  [&](const std::pair<std::string, s2sdk::EventCallback>& entry) {
										  return entry.first == ToStdString(name) && entry.second == callback;
									  }),
					   g_eventHooks.end());
	return s2sdk::EventHookError::Okay;
}

const FakeEventField* Field(const plg::string& key) {
	const auto found = g_event.fields.find(ToStdString(key));
	return found == g_event.fields.end() ? nullptr : &found->second;
}

bool FakeGetEventBool(void* event, const plg::string& key) {
	const FakeEventField* field = event == &g_event ? Field(key) : nullptr;
	return field != nullptr && field->kind == FakeEventField::Kind::Bool && field->boolean;
}
int32_t FakeGetEventInt(void* event, const plg::string& key) {
	++g_eventIntReads[ToStdString(key)];
	const FakeEventField* field = event == &g_event ? Field(key) : nullptr;
	return field != nullptr && field->kind == FakeEventField::Kind::Int ? field->integer : 0;
}
float FakeGetEventFloat(void* event, const plg::string& key) {
	const FakeEventField* field = event == &g_event ? Field(key) : nullptr;
	return field != nullptr && field->kind == FakeEventField::Kind::Float ? field->number : 0.0f;
}
plg::string FakeGetEventString(void* event, const plg::string& key) {
	const FakeEventField* field = event == &g_event ? Field(key) : nullptr;
	return plg::string(field != nullptr && field->kind == FakeEventField::Kind::String ? field->text.c_str()
																					   : "");
}
int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key) {
	const FakeEventField* field = event == &g_event ? Field(key) : nullptr;
	return field != nullptr && field->kind == FakeEventField::Kind::Slot ? field->slot : -1;
}
void* FakeGetEventPlayerPawn(void* event, const plg::string& key) {
	++g_eventPawnCalls;
	const FakeEventField* field = event == &g_event ? Field(key) : nullptr;
	if (field == nullptr || field->kind != FakeEventField::Kind::Pawn || field->pawnSlot < 0) {
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<uintptr_t>(0x2000) + static_cast<uintptr_t>(field->pawnSlot));
}

s2sdk::ResultType FireEvent(const FakeEvent& event) {
	g_event = event;
	s2sdk::ResultType result = s2sdk::ResultType::Continue;
	for (const auto& [name, callback] : g_eventHooks) {
		if (name != event.name) {
			continue;
		}
		result = callback(plg::string(name.c_str()), &g_event, false);
	}
	return result;
}

FakeEvent PlayerDeathEvent(int32_t attacker, int32_t assister = -1, const std::string& weapon = "ak47",
						   bool headshot = true, int32_t dmgHealth = 100) {
	FakeEvent event;
	event.name = "player_death";
	event.fields["userid"].kind = FakeEventField::Kind::Slot;
	event.fields["userid"].slot = 0; // the victim
	event.fields["attacker"].kind = FakeEventField::Kind::Slot;
	event.fields["attacker"].slot = attacker;
	event.fields["assister"].kind = FakeEventField::Kind::Slot;
	event.fields["assister"].slot = assister;
	event.fields["weapon"].kind = FakeEventField::Kind::String;
	event.fields["weapon"].text = weapon;
	event.fields["headshot"].kind = FakeEventField::Kind::Bool;
	event.fields["headshot"].boolean = headshot;
	event.fields["dmg_health"].kind = FakeEventField::Kind::Int;
	event.fields["dmg_health"].integer = dmgHealth;
	return event;
}

// =============================================================================================
// fake menus
// =============================================================================================

struct FakeMenu {
	bool valid = false;
	std::string title;
	std::string type;
	s2sdk::MenuHandlerCallback handler = nullptr;
	std::vector<std::pair<std::string, std::string>> items;
	int32_t pagination = -1;
	bool exitButton = false;
	bool exitBackButton = true;
	bool closeOnSelect = true;
};

std::map<s2sdk::MenuId, FakeMenu> g_menus;
std::array<s2sdk::MenuId, 64> g_displayed {};
s2sdk::MenuId g_nextMenuId = 0;
int g_createMenuCalls = 0;
int g_destroyMenuCalls = 0;
int g_displayMenuCalls = 0;
int g_cancelMenuCalls = 0;
bool g_menuTypeRegisteredHud = true;
bool g_menuTypeRegisteredWasd = true;
bool g_menuTypeRegisteredButton = true;
bool g_cancelClearsDisplay = true;
bool g_setMenuTypeFails = false;
std::string g_lastMenuType;

s2sdk::MenuId FakeCreateMenu(const plg::string& title, s2sdk::MenuHandlerCallback handler,
							 const plg::string& menuType) {
	++g_createMenuCalls;
	const s2sdk::MenuId id = g_nextMenuId++;
	FakeMenu menu;
	menu.valid = true;
	menu.title = ToStdString(title);
	menu.type = ToStdString(menuType);
	menu.handler = handler;
	g_lastMenuType = menu.type;
	g_menus[id] = menu;
	return id;
}
bool FakeDestroyMenu(s2sdk::MenuId id) {
	++g_destroyMenuCalls;
	const auto found = g_menus.find(id);
	if (found == g_menus.end() || !found->second.valid) {
		return false;
	}
	const s2sdk::MenuHandlerCallback handler = found->second.handler;
	found->second.valid = false;
	for (int32_t slot = 0; slot < 64; ++slot) {
		if (g_displayed[static_cast<std::size_t>(slot)] == id) {
			g_displayed[static_cast<std::size_t>(slot)] = 0;
			if (handler != nullptr) {
				handler(id, s2sdk::MenuAction::End, slot, 0);
			}
		}
	}
	return true;
}
bool FakeIsValidMenu(s2sdk::MenuId id) {
	const auto found = g_menus.find(id);
	return found != g_menus.end() && found->second.valid;
}
int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle /*style*/) {
	const auto found = g_menus.find(id);
	if (found == g_menus.end() || !found->second.valid) {
		return -1;
	}
	found->second.items.emplace_back(ToStdString(info), ToStdString(display));
	return static_cast<int32_t>(found->second.items.size()) - 1;
}
bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t /*time*/) {
	++g_displayMenuCalls;
	const auto found = g_menus.find(id);
	if (found == g_menus.end() || !found->second.valid || slot < 0 || slot >= 64) {
		return false;
	}
	g_displayed[static_cast<std::size_t>(slot)] = id;
	if (found->second.handler != nullptr) {
		found->second.handler(id, s2sdk::MenuAction::Start, slot, 0);
	}
	return true;
}
s2sdk::MenuId FakeGetClientMenu(int32_t slot) {
	return slot >= 0 && slot < 64 ? g_displayed[static_cast<std::size_t>(slot)] : 0;
}
bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason) {
	++g_cancelMenuCalls;
	if (slot < 0 || slot >= 64) {
		return false;
	}
	const s2sdk::MenuId id = g_displayed[static_cast<std::size_t>(slot)];
	if (id == 0) {
		return false;
	}
	const auto found = g_menus.find(id);
	if (g_cancelClearsDisplay) {
		g_displayed[static_cast<std::size_t>(slot)] = 0;
	}
	if (found != g_menus.end() && found->second.handler != nullptr) {
		found->second.handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(reason));
		if (g_cancelClearsDisplay) {
			found->second.handler(id, s2sdk::MenuAction::End, slot, 0);
		}
	}
	return true;
}
bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& typeName) {
	if (g_setMenuTypeFails) {
		return false;
	}
	const auto found = g_menus.find(id);
	if (found == g_menus.end() || !found->second.valid) {
		return false;
	}
	found->second.type = ToStdString(typeName);
	g_lastMenuType = found->second.type;
	return true;
}
plg::string FakeGetMenuType(s2sdk::MenuId id) {
	const auto found = g_menus.find(id);
	return plg::string(found == g_menus.end() ? "" : found->second.type.c_str());
}
bool FakeIsMenuTypeRegistered(const plg::string& name) {
	const std::string text = ToStdString(name);
	if (text == "chat" || text == "console") {
		return true;
	}
	if (text == "hud") {
		return g_menuTypeRegisteredHud;
	}
	if (text == "wasd") {
		return g_menuTypeRegisteredWasd;
	}
	if (text == "button") {
		return g_menuTypeRegisteredButton;
	}
	return false;
}
bool FakeSetMenuPagination(s2sdk::MenuId id, int32_t items) {
	const auto found = g_menus.find(id);
	if (found == g_menus.end()) {
		return false;
	}
	found->second.pagination = items;
	return true;
}
bool FakeSetMenuExitButton(s2sdk::MenuId id, bool enabled) {
	const auto found = g_menus.find(id);
	if (found == g_menus.end()) {
		return false;
	}
	found->second.exitButton = enabled;
	return true;
}
bool FakeSetMenuExitBackButton(s2sdk::MenuId id, bool enabled) {
	const auto found = g_menus.find(id);
	if (found == g_menus.end()) {
		return false;
	}
	found->second.exitBackButton = enabled;
	return true;
}
bool FakeSetMenuCloseOnSelect(s2sdk::MenuId id, bool enabled) {
	const auto found = g_menus.find(id);
	if (found == g_menus.end()) {
		return false;
	}
	found->second.closeOnSelect = enabled;
	return true;
}

// =============================================================================================
// fake misc: chat, server commands, vip, permissions, hudmenu
// =============================================================================================

struct ChatLine {
	int32_t slot = -1;
	std::string text;
};
std::vector<ChatLine> g_chatLines;
std::vector<std::string> g_serverCommands;

void FakePrintToChat(int32_t slot, const plg::string& message) {
	g_chatLines.push_back(ChatLine{slot, ToStdString(message)});
}
void FakeServerCommand(const plg::string& command) {
	g_serverCommands.push_back(ToStdString(command));
}

bool g_vipClient = false;
bool g_vipWarmup = false;
bool FakeVipIsVIPLoaded() {
	return true;
}
bool FakeVipIsClientVIP(int32_t /*slot*/) {
	return g_vipClient;
}
bool FakeVipWarmupPeriod() {
	return g_vipWarmup;
}

bool g_permissionAllow = false;
int g_permissionCalls = 0;
permissions::Status FakeHasPermission(uint64_t /*steamid*/, const plg::string& /*flag*/) {
	++g_permissionCalls;
	return g_permissionAllow ? permissions::Status::Allow : permissions::Status::Disallow;
}

std::string g_hudmenuChoice;
plg::string FakeGetClientMenuType(int32_t /*slot*/) {
	return plg::string(g_hudmenuChoice.c_str());
}

// =============================================================================================
// binding installation / reset
// =============================================================================================

void InstallFakes() {
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;
	plg::Log = &FakeLog;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientTeam = &FakeGetClientTeam;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_ClientPtrToPlayerSlot = &FakeClientPtrToPlayerSlot;
	__s2sdk_EntHandleToEntIndex = &FakeEntHandleToEntIndex;
	__s2sdk_EntPointerToEntIndex = &FakeEntPointerToEntIndex;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetMaxClients = &FakeGetMaxClients;
	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
	__s2sdk_GetServerLanguage = &FakeGetServerLanguage;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetEntData = &FakeGetEntData;
	__s2sdk_GetEntData2 = &FakeGetEntData2;
	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_ServerCommand = &FakeServerCommand;

	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventBool = &FakeGetEventBool;
	__s2sdk_GetEventInt = &FakeGetEventInt;
	__s2sdk_GetEventFloat = &FakeGetEventFloat;
	__s2sdk_GetEventString = &FakeGetEventString;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventPlayerPawn = &FakeGetEventPlayerPawn;

	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_IsValidMenu = &FakeIsValidMenu;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
	__s2sdk_SetMenuPagination = &FakeSetMenuPagination;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_SetMenuCloseOnSelect = &FakeSetMenuCloseOnSelect;

	__vip_VIP_IsVIPLoaded = &FakeVipIsVIPLoaded;
	__vip_VIP_IsClientVIP = &FakeVipIsClientVIP;
	__vip_VIP_WarmupPeriod = &FakeVipWarmupPeriod;

	__permissions_HasPermission = &FakeHasPermission;
	__hudmenu_GetClientMenuType = &FakeGetClientMenuType;
}

void NullAllS2sdkBindings() {
	__s2sdk_IsClientConnected = nullptr;
	__s2sdk_IsClientInGame = nullptr;
	__s2sdk_IsClientSourceTV = nullptr;
	__s2sdk_IsFakeClient = nullptr;
	__s2sdk_IsClientAlive = nullptr;
	__s2sdk_GetClientSteamID64 = nullptr;
	__s2sdk_GetClientName = nullptr;
	__s2sdk_GetClientTeam = nullptr;
	__s2sdk_GetClientPawn = nullptr;
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	__s2sdk_PlayerSlotToEntPointer = nullptr;
	__s2sdk_ClientPtrToPlayerSlot = nullptr;
	__s2sdk_EntHandleToEntIndex = nullptr;
	__s2sdk_EntPointerToEntIndex = nullptr;
	__s2sdk_IsValidEntHandle = nullptr;
	__s2sdk_IsValidEntPointer = nullptr;
	__s2sdk_GetMaxClients = nullptr;
	__s2sdk_GetCurrentMap = nullptr;
	__s2sdk_GetServerLanguage = nullptr;
	__s2sdk_GetSchemaOffset = nullptr;
	__s2sdk_GetEntData = nullptr;
	__s2sdk_GetEntData2 = nullptr;
	__s2sdk_PrintToChat = nullptr;
	__s2sdk_ServerCommand = nullptr;

	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapStart_Unregister = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_OnMapEnd_Unregister = nullptr;

	__s2sdk_AddConsoleCommand = nullptr;
	__s2sdk_RemoveCommand = nullptr;
	__s2sdk_AddCommandListener = nullptr;
	__s2sdk_RemoveCommandListener = nullptr;

	__s2sdk_HookEvent = nullptr;
	__s2sdk_UnhookEvent = nullptr;
	__s2sdk_GetEventBool = nullptr;
	__s2sdk_GetEventInt = nullptr;
	__s2sdk_GetEventFloat = nullptr;
	__s2sdk_GetEventString = nullptr;
	__s2sdk_GetEventPlayerSlot = nullptr;
	__s2sdk_GetEventPlayerPawn = nullptr;

	__s2sdk_CreateMenu = nullptr;
	__s2sdk_DestroyMenu = nullptr;
	__s2sdk_IsValidMenu = nullptr;
	__s2sdk_AddMenuItem = nullptr;
	__s2sdk_DisplayMenu = nullptr;
	__s2sdk_GetClientMenu = nullptr;
	__s2sdk_CancelClientMenu = nullptr;
	__s2sdk_SetMenuType = nullptr;
	__s2sdk_GetMenuType = nullptr;
	__s2sdk_IsMenuTypeRegistered = nullptr;
	__s2sdk_SetMenuPagination = nullptr;
	__s2sdk_SetMenuExitButton = nullptr;
	__s2sdk_SetMenuExitBackButton = nullptr;
	__s2sdk_SetMenuCloseOnSelect = nullptr;

	__vip_VIP_IsVIPLoaded = nullptr;
	__vip_VIP_IsClientVIP = nullptr;
	__vip_VIP_WarmupPeriod = nullptr;
	__permissions_HasPermission = nullptr;
	__hudmenu_GetClientMenuType = nullptr;
}

void ResetFakes() {
	g_logs.clear();
	g_chatLines.clear();
	g_serverCommands.clear();
	g_currentMap.clear();
	g_clients.fill(FakeClient {});
	std::fill(std::begin(g_steamIdCalls), std::end(g_steamIdCalls), 0);
	g_clientCalls = 0;
	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;
	g_commands.clear();
	g_listeners.clear();
	g_removedCommands.clear();
	g_removedListeners.clear();
	g_addConsoleCommandCalls = 0;
	g_addConsoleCommandReturnsCreated = true;
	g_eventHooks.clear();
	g_unhookedEvents.clear();
	g_eventIntReads.clear();
	g_eventPawnCalls = 0;
	g_event = FakeEvent {};
	g_menus.clear();
	g_displayed.fill(0);
	g_nextMenuId = 0;
	g_createMenuCalls = 0;
	g_destroyMenuCalls = 0;
	g_displayMenuCalls = 0;
	g_cancelMenuCalls = 0;
	g_menuTypeRegisteredHud = true;
	g_menuTypeRegisteredWasd = true;
	g_menuTypeRegisteredButton = true;
	g_cancelClearsDisplay = true;
	g_setMenuTypeFails = false;
	g_lastMenuType.clear();
	g_vipClient = false;
	g_vipWarmup = false;
	g_permissionAllow = false;
	g_permissionCalls = 0;
	g_hudmenuChoice.clear();
	g_serverLanguage = "en";
	g_schemaOffsetCalls = 0;
	FakeTeamOffsetValue = 100;

	InstallFakes();
}

// =============================================================================================
// files
// =============================================================================================

void WriteFile(const std::filesystem::path& path, std::string_view content) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << content;
}

std::string ReadFile(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return {};
	}
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::filesystem::path PluginConfigDir() {
	return g_configsRoot / "k4_missions";
}

std::filesystem::path PluginDataDir() {
	return g_dataRoot / "k4_missions";
}

const char* kDefaultConfig = R"({
	"K4Missions": {
		"DatabaseConnection": "host",
		"MissionCommands": ["mission", "missions"],
		"MinimumPlayers": 1,
		"MissionAmountNormal": 1,
		"MissionAmountVip": 3,
		"ResetMode": "Daily",
		"WebhookUrl": ""
	}
})";

const char* kDefaultMissions = R"([
	{
		"Event": "EventPlayerDeath",
		"Target": "Attacker",
		"Amount": 1,
		"Phrase": "Kill one player",
		"RewardPhrase": "1 credit",
		"RewardCommands": ["sw_givecredits {steamid64} 1"]
	}
])";

const char* kDefaultDatabases = R"("Databases"
{
	"k4_missions"
	{
		"driver" "sqlite"
		"sqlite_path" "test.sqlite3"
	}
})";

void ResetFiles(const char* config = kDefaultConfig, const char* missions = kDefaultMissions,
				const char* databases = kDefaultDatabases) {
	std::error_code error;
	std::filesystem::remove_all(g_root, error);
	std::filesystem::create_directories(PluginConfigDir() / "translations");
	std::filesystem::create_directories(PluginDataDir());

	WriteFile(PluginConfigDir() / "config.json", config);
	WriteFile(PluginConfigDir() / "missions.json", missions);
	WriteFile(PluginConfigDir() / "databases.cfg", databases);
	// The real shipped phrase file (the same one the package installs).
	const std::filesystem::path source = std::filesystem::path(K4_MISSIONS_TEST_SOURCE_DIR);
	WriteFile(PluginConfigDir() / "translations" / "k4_missions.phrases.txt",
			  ReadFile(source / "translations" / "k4_missions.phrases.txt"));
}

// =============================================================================================
// lifecycle helpers
// =============================================================================================

bool StartPlugin() {
	return static_cast<bool>(k4::g_plugin.OnPluginStart());
}

void EndPlugin() {
	(void)k4::g_plugin.OnPluginEnd();
}

void Pump(int32_t milliseconds = 20) {
	(void)k4::g_plugin.OnPluginUpdate(std::chrono::milliseconds(milliseconds));
}

bool PumpUntil(const std::function<bool()>& condition, int attempts = 500) {
	for (int attempt = 0; attempt < attempts; ++attempt) {
		Pump();
		if (condition()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return false;
}

// A started plugin with a map up.
void StartWithMap(const char* config = kDefaultConfig, const char* missions = kDefaultMissions,
				  const char* databases = kDefaultDatabases) {
	ResetFakes();
	ResetFiles(config, missions, databases);
	CHECK(StartPlugin());
	g_currentMap = "de_dust2";
	FireMapStart();
	Pump();
	CHECK(k4::g_mapGate.Ready());
}

void AddPlayer(int32_t slot, uint64_t steamid, int32_t team = 2, const std::string& name = "Player") {
	FakeClient& client = g_clients[static_cast<std::size_t>(slot)];
	client.connected = true;
	client.inGame = true;
	client.hasController = true;
	client.hasPawn = true;
	client.team = team;
	client.steamid = steamid;
	client.name = name;
}

k4::PlayerRecord* FindRecord(int32_t slot) {
	return k4::FindBySlot(slot);
}

// The mission record of a player once its database load has completed.
k4::PlayerRecord* LoadedRecord(int32_t slot) {
	k4::PlayerRecord* record = k4::FindBySlot(slot);
	if (record == nullptr) {
		return nullptr;
	}
	if (!PumpUntil([record] { return record->loaded; })) {
		return nullptr;
	}
	return record;
}

// Waits until the deferred assignment pass has produced `count` missions.
bool WaitForMissions(k4::PlayerRecord* record, size_t count) {
	return PumpUntil([record, count] { return record != nullptr && record->missions.size() == count; });
}

void JoinPlayer(int32_t slot, uint64_t steamid, int32_t team = 2, const std::string& name = "Player") {
	AddPlayer(slot, steamid, team, name);
	FakeEvent event;
	event.name = "player_activate";
	event.fields["userid"].kind = FakeEventField::Kind::Slot;
	event.fields["userid"].slot = slot;
	FireEvent(event);
	Pump();
}

bool ChatContains(std::string_view needle) {
	return std::any_of(g_chatLines.begin(), g_chatLines.end(), [needle](const ChatLine& line) {
		return line.text.find(needle) != std::string::npos;
	});
}

// =============================================================================================
// path rules (HEAD priority)
// =============================================================================================

void TestPaths() {
	ResetFakes();
	ResetFiles();
	CheckEq(k4::ConfigFilePath("config.json"), (PluginConfigDir() / "config.json").string(), "config path");
	CheckEq(k4::CoreConfigPath(), (g_configsRoot / "core.cfg").string(), "core.cfg path");
	CheckEq(k4::ResolveDataPath("k4_missions.sqlite3"), (PluginDataDir() / "k4_missions.sqlite3").string(),
			"data path");
	CheckEq(k4::ResolveDataPath("addons/data/sub/file.db"), (PluginDataDir() / "sub/file.db").string(),
			"metamod data prefix");
	// Only the deployed translations file exists: it wins over the config-directory path.
	CheckEq(k4::ResolvePhrasesPath("k4_missions.phrases.txt"),
			(PluginConfigDir() / "translations" / "k4_missions.phrases.txt").string(), "phrases path");
	// A copy in the config directory is next in the old order when translations is missing.
	std::filesystem::remove(PluginConfigDir() / "translations" / "k4_missions.phrases.txt");
	WriteFile(PluginConfigDir() / "k4_missions.phrases.txt", "\"Phrases\" {}");
	CheckEq(k4::ResolvePhrasesPath("k4_missions.phrases.txt"),
			(PluginConfigDir() / "k4_missions.phrases.txt").string(), "phrases config fallback");
	// A raw file in the working directory wins over every deployed copy (the old ResolvePath order).
	const std::filesystem::path cwd = std::filesystem::current_path();
	std::filesystem::create_directories(g_root / "cwd");
	std::filesystem::current_path(g_root / "cwd");
	WriteFile(g_root / "cwd" / "k4_missions.phrases.txt", "\"Phrases\" {}");
	CheckEq(k4::ResolvePhrasesPath("k4_missions.phrases.txt"), "k4_missions.phrases.txt", "raw phrases wins");
	std::filesystem::current_path(cwd);
	// databases.cfg: the plugin directory first, the legacy "addons/configs" spelling when it exists.
	CheckEq(k4::ResolveDatabasesPath(), (PluginConfigDir() / "databases.cfg").string(), "databases path");
	std::filesystem::create_directories(g_root / "legacy" / "addons" / "configs");
	WriteFile(g_root / "legacy" / "addons" / "configs" / "databases.cfg", "\"Databases\" {}");
	std::filesystem::current_path(g_root / "legacy");
	CheckEq(k4::ResolveDatabasesPath(), "addons/configs/databases.cfg", "legacy databases wins");
	std::filesystem::current_path(cwd);
}

// =============================================================================================
// phrases and language chain
// =============================================================================================

void TestPhraseLanguage() {
	ResetFakes();
	ResetFiles();
	// core.cfg "ServerLang" wins over the s2sdk language.
	WriteFile(g_configsRoot / "core.cfg", "\"Core\" { \"ServerLang\" \"Russian\" }");
	WriteFile(PluginConfigDir() / "translations" / "k4_missions.phrases.txt",
			  "\"Phrases\" { \"k4.general.prefix\" { \"en\" \"[EN]\" \"ru\" \"[RU]\" } }");
	g_serverLanguage = "english";
	CHECK(StartPlugin());
	CheckEq(k4::Phrase("k4.general.prefix"), "[RU]", "core.cfg language wins");
	CheckEq(k4::Phrase("missing.key"), "", "unknown phrase is empty");
	EndPlugin();

	// No core.cfg: the s2sdk server language is the next step in the chain.
	ResetFakes();
	ResetFiles();
	WriteFile(PluginConfigDir() / "translations" / "k4_missions.phrases.txt",
			  "\"Phrases\" { \"k4.general.prefix\" { \"en\" \"[EN]\" \"de\" \"[DE]\" } }");
	g_serverLanguage = "German";
	CHECK(StartPlugin());
	CheckEq(k4::Phrase("k4.general.prefix"), "[DE]", "s2sdk language used");
	EndPlugin();

	// Neither source: "en", and a phrase with no "en" entry falls back to its first language.
	ResetFakes();
	ResetFiles();
	WriteFile(PluginConfigDir() / "translations" / "k4_missions.phrases.txt",
			  "\"Phrases\" { \"k4.general.prefix\" { \"de\" \"[DE]\" } }");
	CHECK(StartPlugin());
	CheckEq(k4::Phrase("k4.general.prefix"), "[DE]", "first available language");
	EndPlugin();

	// A malformed phrase file is logged and leaves the keys as-is.
	ResetFakes();
	ResetFiles();
	WriteFile(PluginConfigDir() / "translations" / "k4_missions.phrases.txt", "\"Phrases\" { broken");
	CHECK(StartPlugin());
	CHECK(Logged(plg::Severity::Error, "malformed phrase file"));
	CheckEq(k4::Phrase("k4.general.prefix"), "", "no phrases after a malformed file");
	EndPlugin();
}

// =============================================================================================
// PITFALLS B1/B2: the map gate
// =============================================================================================

void TestMapGate() {
	ResetFakes();
	ResetFiles();
	CHECK(StartPlugin());
	// The map listeners, the commands and the event hooks are registered in OnPluginStart; not one
	// client/entity/schema call is made there.
	CheckEqInt(g_clientCalls, 0, "no client call in OnPluginStart");
	CheckEqInt(g_schemaOffsetCalls, 0, "no schema lookup in OnPluginStart");
	CheckEqInt(g_mapStartRegisters, 1, "map start listener registered");
	CheckEqInt(g_mapEndRegisters, 1, "map end listener registered");
	CHECK(FindCommand("mission") != nullptr && FindCommand("missions") != nullptr);
	CHECK(g_eventHooks.size() >= 3); // player_activate, player_disconnect, round_end (+ player_death)

	// Before the first map: the gate stays shut and no client call is made.
	Pump();
	CHECK(!k4::g_mapGate.Ready());
	CheckEqInt(g_clientCalls, 0, "no client call before the first map");

	// "<empty>" is a level without a name: not a map.
	g_currentMap = "<empty>";
	Pump();
	CHECK(!k4::g_mapGate.Ready());

	// A real map name opens the latch on the next update (the plugin was loaded mid-map).
	g_currentMap = "de_mirage";
	Pump();
	CHECK(k4::g_mapGate.Ready());
	CHECK(g_clientCalls > 0); // the first reconcile ran

	// OnMapEnd closes the gate; the next update must not open it again on its own (a changelevel is
	// in progress) and must not touch a client.
	FireMapEnd();
	CHECK(!k4::g_mapGate.Ready());
	const int callsAfterMapEnd = g_clientCalls;
	g_currentMap = "de_inferno";
	Pump();
	CHECK(!k4::g_mapGate.Ready());
	CheckEqInt(g_clientCalls, callsAfterMapEnd, "no client call between maps");

	// Only a new OnMapStart reopens it.
	FireMapStart();
	Pump();
	CHECK(k4::g_mapGate.Ready());

	// Without the four map-listener bindings the gate never opens (nothing could close it again).
	EndPlugin();
	ResetFakes();
	ResetFiles();
	__s2sdk_OnMapEnd_Register = nullptr;
	CHECK(StartPlugin());
	g_currentMap = "de_dust2";
	Pump();
	CHECK(!k4::g_mapGate.Ready());
	CheckEqInt(g_clientCalls, 0, "no client call without map listeners");
	CHECK(Logged(plg::Severity::Error, "map listener Register/Unregister pair is incomplete"));
	EndPlugin();

	// A missing GetCurrentMap binding blocks the mid-map latch only; OnMapStart still opens the gate.
	ResetFakes();
	ResetFiles();
	__s2sdk_GetCurrentMap = nullptr;
	CHECK(StartPlugin());
	Pump();
	CHECK(!k4::g_mapGate.Ready());
	FireMapStart();
	Pump();
	CHECK(k4::g_mapGate.Ready());
	EndPlugin();
}

// =============================================================================================
// PITFALLS C1/C2: slot, client and handle guards
// =============================================================================================

void TestClientGuards() {
	StartWithMap();
	// A connected slot that is not in game is not a real player.
	AddPlayer(4, 76561198000000004ULL);
	g_clients[4].inGame = false;
	CHECK(!k4::IsRealPlayer(4));
	g_clients[4].inGame = true;
	CHECK(k4::IsRealPlayer(4));

	// Out of range slots never reach s2sdk.
	const int before = g_clientCalls;
	CHECK(!k4::ClientExists(-1));
	CHECK(!k4::ClientExists(64));
	CheckEqInt(g_clientCalls, before, "out-of-range slot never queried");

	// A disconnected slot has no SteamID (the export would otherwise answer the previous id).
	g_clients[5].steamid = 76561198000000005ULL;
	g_clients[5].connected = false;
	g_steamIdCalls[5] = 0;
	CheckEqInt(static_cast<int64_t>(k4::ClientSteamID64(5)), 0, "disconnected steamid");
	CheckEqInt(g_steamIdCalls[5], 0, "no GetClientSteamID64 for a disconnected slot");

	// Bots and SourceTV are not players.
	AddPlayer(6, 76561198000000006ULL);
	g_clients[6].fake = true;
	CHECK(!k4::IsRealPlayer(6));
	g_clients[6].fake = false;
	g_clients[6].sourceTV = true;
	CHECK(!k4::IsRealPlayer(6));
	g_clients[6].sourceTV = false;

	// "<unknown>" is "no name".
	g_clients[6].name = "<unknown>";
	CheckEq(k4::ClientName(6), "", "<unknown> name");
	g_clients[6].name = "Bob";
	CheckEq(k4::ClientName(6), "Bob", "normal name");

	// A controller-less slot is not ready: no name/team/placeholder query reaches the controller.
	g_clients[6].hasController = false;
	CHECK(!k4::IsClientReady(6));
	CheckEq(k4::ClientName(6), "", "no name without a controller");
	CheckEqInt(k4::PlayerTeam(6), 0, "no team without a controller");
	g_clients[6].hasController = true;

	// A negative CEntityHandle is valid once the serial passed 0x8000; the entity INDEX is what
	// {userid} shows, and it is derived with EntHandleToEntIndex (never a `handle > 0` test).
	AddPlayer(7, 76561198000000007ULL);
	g_clients[7].controllerHandle = MakeNegativeHandle(7);
	CHECK(k4::ControllerHandle(7) == MakeNegativeHandle(7));
	CheckEqInt(k4::ControllerEntityIndex(7), 8, "negative handle -> entity index");
	k4::PlayerRecord record;
	record.steamid = 76561198000000007ULL;
	record.slot = 7;
	record.controllerHandle = MakeNegativeHandle(7);
	k4::g_slotSteam[7] = record.steamid;
	CHECK(k4::RecordIdentityCurrent(record));
	CheckEqInt(k4::Placeholders(record).userid, 8, "userid is the controller entity index");

	// A reused slot (different handle) invalidates the record.
	g_clients[7].controllerHandle = MakeHandle(7, 2);
	CHECK(!k4::RecordIdentityCurrent(record));
	g_clients[7].controllerHandle = 0;
	CHECK(!k4::RecordIdentityCurrent(record));
	EndPlugin();
}

// =============================================================================================
// PITFALLS C8: schema offsets only behind the gate, cached only when >= 0
// =============================================================================================

void TestSchemaGate() {
	ResetFakes();
	ResetFiles();
	CHECK(StartPlugin());
	AddPlayer(3, 76561198000000003ULL);
	g_currentMap = "de_dust2";
	Pump();
	CHECK(k4::g_mapGate.Ready());
	CheckEqInt(k4::PlayerTeam(3), 2, "team read through the cached offset");
	const int callsAfterFirstRead = g_schemaOffsetCalls;
	CHECK(callsAfterFirstRead >= 1);
	CheckEqInt(k4::PlayerTeam(3), 2, "second read");
	CheckEqInt(g_schemaOffsetCalls, callsAfterFirstRead, "successful offset cached");

	// A negative offset is not cached: the next read asks again and falls back to the pawn export.
	EndPlugin();
	ResetFakes();
	ResetFiles();
	FakeTeamOffsetValue = -1;
	CHECK(StartPlugin());
	AddPlayer(3, 76561198000000003ULL);
	g_currentMap = "de_dust2";
	Pump();
	const int before = g_schemaOffsetCalls;
	(void)k4::PlayerTeam(3);
	(void)k4::PlayerTeam(3);
	CHECK(g_schemaOffsetCalls > before + 1);
	CheckEqInt(k4::PlayerTeam(3), 2, "pawn-based team fallback");
	EndPlugin();
}

// =============================================================================================
// commands (C11) and the menu-type table
// =============================================================================================

void TestCommands() {
	StartWithMap();
	// Both configured names are client-executable ConCommands, registered Pre with Release.
	const FakeCommand* command = FindCommand("mission");
	CHECK(command != nullptr);
	if (command != nullptr) {
		const int64_t flags = static_cast<int64_t>(command->flags);
		CHECK((flags & static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute)) != 0);
		CHECK((flags & static_cast<int64_t>(s2sdk::ConVarFlag::Release)) != 0);
		CheckEqInt(static_cast<int64_t>(command->mode), static_cast<int64_t>(s2sdk::HookMode::Pre), "hook mode");
	}
	JoinPlayer(3, 76561198000000003ULL);
	CHECK(LoadedRecord(3) != nullptr);
	CHECK(WaitForMissions(FindRecord(3), 1));
	// The server console is strictly caller == -1 and is refused, as is any other off-slot caller.
	Pump();
	CheckEqInt(g_createMenuCalls, 0, "no menu yet");
	FireCommand("mission", -1, s2sdk::ConCommandContext::Console);
	Pump();
	CheckEqInt(g_createMenuCalls, 0, "server console refused");
	FireCommand("mission", -2, s2sdk::ConCommandContext::Console);
	Pump();
	CheckEqInt(g_createMenuCalls, 0, "invalid caller refused");
	FireCommand("mission", 64, s2sdk::ConCommandContext::Console);
	Pump();
	CheckEqInt(g_createMenuCalls, 0, "out-of-range caller refused");
	// A disconnected slot with a stale record is refused too.
	g_clients[3].connected = false;
	FireCommand("mission", 3, s2sdk::ConCommandContext::Console);
	Pump();
	CheckEqInt(g_createMenuCalls, 0, "disconnected caller refused");
	g_clients[3].connected = true;
	// The player's console and the chat trigger both open the menu (the chat context is Handled).
	const FakeCommand* raw = FindCommand("mission");
	CHECK(raw != nullptr);
	if (raw != nullptr) {
		plg::vector<plg::string> args;
		args.push_back(plg::string("!mission"));
		CheckEqInt(static_cast<int64_t>(raw->callback(3, s2sdk::ConCommandContext::Chat, args)),
				   static_cast<int64_t>(s2sdk::ResultType::Handled), "chat context handled");
	}
	Pump();
	CheckEqInt(g_createMenuCalls, 2, "the reserved id 0 dummy plus the mission menu");
	CheckEqInt(g_displayMenuCalls, 1, "menu displayed");
	CHECK(g_displayed[3] != 0);

	// The menu-type table is pisexcompat's ResolveS2sdkMenuTypeName with the HUD_LAYOUT default.
	CheckEq(k4::ResolveMenuType(3), "hud", "default hud");
	g_menuTypeRegisteredHud = false;
	CheckEq(k4::ResolveMenuType(3), "button", "hud missing -> button");
	g_menuTypeRegisteredWasd = true;
	g_hudmenuChoice = "wasd";
	CheckEq(k4::ResolveMenuType(3), "wasd", "wasd registered");
	g_menuTypeRegisteredWasd = false;
	CheckEq(k4::ResolveMenuType(3), "button", "wasd missing -> button (never hud)");
	g_hudmenuChoice = "chat";
	CheckEq(k4::ResolveMenuType(3), "chat", "builtin chat");
	g_hudmenuChoice = "centerhtml";
	CheckEq(k4::ResolveMenuType(3), "centerhtml", "builtin centerhtml");
	g_hudmenuChoice = "hud";
	g_menuTypeRegisteredHud = true;
	CheckEq(k4::ResolveMenuType(3), "hud", "hud registered");
	g_hudmenuChoice = "banana";
	g_menuTypeRegisteredHud = false;
	CheckEq(k4::ResolveMenuType(3), "button", "unknown choice -> default");
	g_menuTypeRegisteredButton = false;
	CheckEq(k4::ResolveMenuType(3), "", "no menu type registered");
	g_menuTypeRegisteredButton = true;
	g_hudmenuChoice.clear();
	EndPlugin();

	// A pre-existing command (AddConsoleCommand == false) is only listened to, never RemoveCommand'd.
	ResetFakes();
	ResetFiles();
	g_addConsoleCommandReturnsCreated = false;
	CHECK(StartPlugin());
	CHECK(FindCommand("mission") != nullptr);
	EndPlugin();
	CHECK(std::find(g_removedCommands.begin(), g_removedCommands.end(), "mission") == g_removedCommands.end());
	CHECK(std::find(g_removedListeners.begin(), g_removedListeners.end(), "mission") != g_removedListeners.end());
	CHECK(std::find(g_removedListeners.begin(), g_removedListeners.end(), "missions")
		  != g_removedListeners.end());

	// The full removal set is required before anything is registered.
	ResetFakes();
	ResetFiles();
	__s2sdk_RemoveCommandListener = nullptr;
	CHECK(StartPlugin());
	CheckEqInt(g_addConsoleCommandCalls, 0, "no command without the removal pair");
	CHECK(Logged(plg::Severity::Error, "AddConsoleCommand/RemoveCommand/RemoveCommandListener set is incomplete"));
	EndPlugin();
}

// =============================================================================================
// the mission flow: assignment, progress, completion, rewards
// =============================================================================================

void TestMissionFlow() {
	StartWithMap();
	JoinPlayer(3, 76561198000000003ULL, 2, "Bob");
	k4::PlayerRecord* record = LoadedRecord(3);
	CHECK(record != nullptr);
	if (record == nullptr) {
		return;
	}
	CHECK(WaitForMissions(record, 1));
	CheckEqInt(static_cast<int64_t>(record->missions.size()), 1, "one normal mission assigned");
	CHECK(record->missions.front().id > 0);
	CHECK(AnyLogContains("database ready"));
	CHECK(ChatContains("new missions"));

	// A player_death event with the attacker on slot 3 advances the mission and completes it.
	const s2sdk::ResultType result = FireEvent(PlayerDeathEvent(3));
	CheckEqInt(static_cast<int64_t>(result), static_cast<int64_t>(s2sdk::ResultType::Continue),
			   "Post trampoline always continues");
	CHECK(std::any_of(record->missions.begin(), record->missions.end(),
					  [](const k4::Mission& mission) { return mission.completed; }));
	CHECK(PumpUntil([] { return !g_serverCommands.empty(); }));
	CheckEq(g_serverCommands.front(), "sw_givecredits 76561198000000003 1", "reward command");
	CHECK(ChatContains("You have completed"));

	// Player fields were read through GetEventPlayerSlot only: no GetEventInt("attacker").
	CHECK(g_eventIntReads.find("attacker") == g_eventIntReads.end());
	CHECK(g_eventIntReads.find("userid") == g_eventIntReads.end());
	CHECK(g_eventIntReads.find("assister") == g_eventIntReads.end());

	// The completion is persisted (completed = 1).
	k4::MissionStore store(*k4::g_database);
	bool loadedOk = false;
	std::vector<k4::Mission> stored;
	store.LoadPlayer(76561198000000003ULL, [&](bool ok, std::vector<k4::Mission> missions) {
		loadedOk = ok;
		stored = std::move(missions);
	});
	CHECK(PumpUntil([&] { return loadedOk; }));
	if (loadedOk) {
		CheckEqInt(static_cast<int64_t>(stored.size()), 1, "one stored row");
		CHECK(!stored.empty() && stored.front().completed);
	}

	// A mission the player already completed never fires again.
	g_serverCommands.clear();
	FireEvent(PlayerDeathEvent(3));
	Pump(1500);
	CHECK(g_serverCommands.empty());
	EndPlugin();
}

void TestMissionVariants() {
	// EventProperties filter: a non-matching weapon does not progress, the matching one does.
	StartWithMap(kDefaultConfig, R"([
		{
			"Event": "EventPlayerDeath",
			"Target": "Attacker",
			"EventProperties": { "Weapon": "ak47", "Headshot": true },
			"Amount": 1,
			"Phrase": "AK47 headshots",
			"RewardPhrase": "1 credit",
			"RewardCommands": ["reward_one"]
		}
	])");
	JoinPlayer(3, 76561198000000033ULL);
	k4::PlayerRecord* record = LoadedRecord(3);
	CHECK(record != nullptr);
	if (record == nullptr) {
		return;
	}
	CHECK(WaitForMissions(record, 1));
	FireEvent(PlayerDeathEvent(3, -1, "awp", true));
	Pump(60);
	CHECK(record->missions.front().progress == 0);
	FireEvent(PlayerDeathEvent(3, -1, "ak47", false));
	Pump(60);
	CHECK(record->missions.front().progress == 0);
	FireEvent(PlayerDeathEvent(3, -1, "ak47", true));
	CHECK(PumpUntil([] { return !g_serverCommands.empty(); }));
	CheckEq(g_serverCommands.front(), "reward_one", "filtered reward");
	EndPlugin();

	// Assister target: the assister is the mission holder.
	StartWithMap(kDefaultConfig, R"([
		{
			"Event": "EventPlayerDeath",
			"Target": "Assister",
			"Amount": 1,
			"Phrase": "Assist",
			"RewardPhrase": "1 credit",
			"RewardCommands": ["assist_reward"]
		}
	])");
	JoinPlayer(4, 76561198000000044ULL);
	CHECK(LoadedRecord(4) != nullptr);
	CHECK(WaitForMissions(FindRecord(4), 1));
	AddPlayer(3, 76561198000000043ULL);
	FireEvent(PlayerDeathEvent(3, 4));
	CHECK(PumpUntil([] { return !g_serverCommands.empty(); }));
	CheckEq(g_serverCommands.front(), "assist_reward", "assister reward");
	EndPlugin();

	// round_end: the "winner" target and the numeric winner field through GetEventInt. One
	// definition per start: the assignment is random and the test must be deterministic.
	StartWithMap(kDefaultConfig, R"([
		{
			"Event": "EventRoundEnd",
			"Target": "winner",
			"Amount": 1,
			"Phrase": "Win a round",
			"RewardPhrase": "1 credit",
			"RewardCommands": ["win_reward"]
		}
	])");
	JoinPlayer(3, 76561198000000035ULL, 2);
	k4::PlayerRecord* winner = LoadedRecord(3);
	CHECK(winner != nullptr);
	if (winner != nullptr) {
		CHECK(WaitForMissions(winner, 1));
		FakeEvent roundEnd;
		roundEnd.name = "round_end";
		roundEnd.fields["winner"].kind = FakeEventField::Kind::Int;
		roundEnd.fields["winner"].integer = 2;
		FireEvent(roundEnd);
		CHECK(PumpUntil([] { return !g_serverCommands.empty(); }));
		if (!g_serverCommands.empty()) {
			CheckEq(g_serverCommands.front(), "win_reward", "round winner reward");
		}
		CheckEqInt(g_eventIntReads["winner"], 1, "round_end winner read through GetEventInt");
	}
	EndPlugin();
	StartWithMap(kDefaultConfig, R"([
		{
			"Event": "EventRoundEnd",
			"Target": "loser",
			"Amount": 1,
			"Phrase": "Lose a round",
			"RewardPhrase": "1 credit",
			"RewardCommands": ["lose_reward"]
		}
	])");
	JoinPlayer(3, 76561198000000034ULL, 2);
	k4::PlayerRecord* loser = LoadedRecord(3);
	CHECK(loser != nullptr);
	if (loser != nullptr) {
		CHECK(WaitForMissions(loser, 1));
		FakeEvent roundEnd;
		roundEnd.name = "round_end";
		roundEnd.fields["winner"].kind = FakeEventField::Kind::Int;
		roundEnd.fields["winner"].integer = 3; // the player is on team 2: a loser round
		FireEvent(roundEnd);
		CHECK(PumpUntil([] { return !g_serverCommands.empty(); }));
		if (!g_serverCommands.empty()) {
			CheckEq(g_serverCommands.front(), "lose_reward", "round loser reward");
		}
	}
	EndPlugin();

	// bomb_pickup.userid is a player_pawn field: GetEventPlayerSlot fails, the map-gated
	// GetEventPlayerPawn + ClientPtrToPlayerSlot fallback resolves the slot.
	StartWithMap(kDefaultConfig, R"([
		{
			"Event": "EventBombPickup",
			"Target": "Userid",
			"Amount": 1,
			"Phrase": "Pick up the bomb",
			"RewardPhrase": "1 credit",
			"RewardCommands": ["bomb_reward"]
		}
	])");
	JoinPlayer(3, 76561198000000036ULL);
	CHECK(LoadedRecord(3) != nullptr);
	CHECK(WaitForMissions(FindRecord(3), 1));
	FakeEvent pickup;
	pickup.name = "bomb_pickup";
	pickup.fields["userid"].kind = FakeEventField::Kind::Pawn;
	pickup.fields["userid"].pawnSlot = 3;
	FireEvent(pickup);
	CHECK(PumpUntil([] { return !g_serverCommands.empty(); }));
	CheckEq(g_serverCommands.front(), "bomb_reward", "pawn-field reward");
	CHECK(g_eventPawnCalls > 0);
	EndPlugin();
}

void TestPlaytimeAndGates() {
	// PlayTime: one point per minute tick, completed at the mission amount; Instant re-rolls it.
	StartWithMap(R"({
		"K4Missions": {
			"MissionCommands": ["mission"],
			"MinimumPlayers": 1,
			"MissionAmountNormal": 1,
			"MissionAmountVip": 1,
			"ResetMode": "Instant",
			"WebhookUrl": ""
		}
	})", R"([
		{
			"Event": "PlayTime",
			"Target": "Userid",
			"Amount": 2,
			"Phrase": "Play two minutes",
			"RewardPhrase": "1 credit",
			"RewardCommands": ["playtime_reward"]
		}
	])");
	JoinPlayer(3, 76561198000000053ULL);
	k4::PlayerRecord* record = LoadedRecord(3);
	CHECK(record != nullptr);
	if (record == nullptr) {
		return;
	}
	CHECK(WaitForMissions(record, 1));
	const int64_t firstId = record->missions.front().id;
	Pump(60000); // one minute
	CHECK(std::any_of(record->missions.begin(), record->missions.end(),
					  [](const k4::Mission& mission) { return mission.progress == 1; }));
	Pump(60000); // second minute: the mission completes and Instant re-rolls it
	CHECK(PumpUntil([] { return !g_serverCommands.empty(); }));
	CheckEq(g_serverCommands.front(), "playtime_reward", "playtime reward");
	CHECK(PumpUntil([&] {
		return record->missions.size() == 1 && record->missions.front().progress == 0
			   && record->missions.front().id != firstId;
	}));
	CheckEqInt(static_cast<int64_t>(record->missions.size()), 1, "Instant re-roll keeps one mission");
	EndPlugin();

	// The minimum-player gate: one active player below the configured minimum earns nothing.
	StartWithMap(R"({
		"K4Missions": {
			"MissionCommands": ["mission"],
			"MinimumPlayers": 2,
			"MissionAmountNormal": 1,
			"MissionAmountVip": 1,
			"ResetMode": "Daily",
			"WebhookUrl": ""
		}
	})");
	JoinPlayer(3, 76561198000000063ULL);
	k4::PlayerRecord* alone = LoadedRecord(3);
	CHECK(alone != nullptr);
	if (alone != nullptr) {
		CHECK(WaitForMissions(alone, 1));
		FireEvent(PlayerDeathEvent(3));
		Pump(60);
		CHECK(alone->missions.front().progress == 0);
	}
	EndPlugin();

	// Warmup protection, and the switch that disables it.
	StartWithMap();
	JoinPlayer(3, 76561198000000073ULL);
	k4::PlayerRecord* warm = LoadedRecord(3);
	CHECK(warm != nullptr);
	if (warm != nullptr) {
		CHECK(WaitForMissions(warm, 1));
		g_vipWarmup = true;
		FireEvent(PlayerDeathEvent(3));
		Pump(60);
		CHECK(warm->missions.front().progress == 0);
	}
	EndPlugin();
	StartWithMap(R"({
		"K4Missions": {
			"MissionCommands": ["mission"],
			"MinimumPlayers": 1,
			"MissionAmountNormal": 1,
			"MissionAmountVip": 1,
			"AllowProgressDuringWarmup": true,
			"ResetMode": "Daily",
			"WebhookUrl": ""
		}
	})");
	JoinPlayer(3, 76561198000000074ULL);
	k4::PlayerRecord* warm2 = LoadedRecord(3);
	CHECK(warm2 != nullptr);
	if (warm2 != nullptr) {
		CHECK(WaitForMissions(warm2, 1));
		g_vipWarmup = true;
		FireEvent(PlayerDeathEvent(3));
		CHECK(PumpUntil([] { return !g_serverCommands.empty(); }));
	}
	EndPlugin();

	// VIP amount comes from the vip core; the name domain is the fallback. Three distinct
	// definitions so a VIP can actually hold three missions.
	const char* threeMissions = R"([
		{ "Event": "EventPlayerDeath", "Target": "Attacker", "Amount": 1, "Phrase": "One",
		  "RewardPhrase": "1 credit", "RewardCommands": ["r1"] },
		{ "Event": "EventPlayerDeath", "Target": "Attacker", "Amount": 2, "Phrase": "Two",
		  "RewardPhrase": "1 credit", "RewardCommands": ["r2"] },
		{ "Event": "EventRoundMvp", "Target": "Userid", "Amount": 2, "Phrase": "Three",
		  "RewardPhrase": "1 credit", "RewardCommands": ["r3"] }
	])";
	StartWithMap(R"({
		"K4Missions": {
			"MissionCommands": ["mission"],
			"MinimumPlayers": 1,
			"MissionAmountNormal": 1,
			"MissionAmountVip": 3,
			"VipNameDomain": "vip",
			"ResetMode": "Daily",
			"WebhookUrl": ""
		}
	})", threeMissions);
	JoinPlayer(3, 76561198000000083ULL, 2, "Bob.VIP.gg");
	k4::PlayerRecord* named = LoadedRecord(3);
	CHECK(named != nullptr);
	if (named != nullptr) {
		CHECK(WaitForMissions(named, 3));
		CheckEqInt(static_cast<int64_t>(named->missions.size()), 3, "name-domain VIP amount");
	}
	EndPlugin();
	StartWithMap(kDefaultConfig, threeMissions);
	g_vipClient = true;
	JoinPlayer(3, 76561198000000084ULL, 2, "Alice");
	k4::PlayerRecord* vipRecord = LoadedRecord(3);
	CHECK(vipRecord != nullptr);
	if (vipRecord != nullptr) {
		CHECK(WaitForMissions(vipRecord, 3));
		CheckEqInt(static_cast<int64_t>(vipRecord->missions.size()), 3, "vip core amount");
	}
	EndPlugin();

	// A mission with a Flag is only offered when the permissions plugin allows it.
	const char* flagged = R"([
		{
			"Event": "EventPlayerDeath",
			"Target": "Attacker",
			"Amount": 1,
			"Phrase": "VIP kill",
			"RewardPhrase": "1 credit",
			"RewardCommands": ["flagged_reward"],
			"Flag": "vipcore.supervip"
		}
	])";
	StartWithMap(kDefaultConfig, flagged);
	JoinPlayer(3, 76561198000000090ULL);
	k4::PlayerRecord* denied = LoadedRecord(3);
	CHECK(denied != nullptr);
	if (denied != nullptr) {
		Pump(1200);
		CheckEqInt(static_cast<int64_t>(denied->missions.size()), 0, "flagged mission denied");
		CHECK(g_permissionCalls > 0);
	}
	EndPlugin();
	StartWithMap(kDefaultConfig, flagged);
	g_permissionAllow = true;
	JoinPlayer(3, 76561198000000091ULL);
	k4::PlayerRecord* allowed = LoadedRecord(3);
	CHECK(allowed != nullptr);
	if (allowed != nullptr) {
		CHECK(WaitForMissions(allowed, 1));
		CheckEqInt(static_cast<int64_t>(allowed->missions.size()), 1, "flagged mission allowed");
	}
	EndPlugin();
}

// =============================================================================================
// identity: SteamID64 + controller handle in deferred actions, menus and DB callbacks
// =============================================================================================

void TestIdentity() {
	// A queued menu action is dropped when the slot has been reused.
	StartWithMap();
	JoinPlayer(3, 76561198100000001ULL);
	CHECK(LoadedRecord(3) != nullptr);
	CHECK(WaitForMissions(FindRecord(3), 1));
	Pump();
	FireCommand("mission", 3, s2sdk::ConCommandContext::Console);
	// The slot gets a new player before OnPluginUpdate runs the queued action.
	AddPlayer(3, 76561198100000002ULL, 2, "Someone else");
	Pump();
	CheckEqInt(g_createMenuCalls, 0, "menu dropped after the slot was reused");
	EndPlugin();

	// A DB load that finishes after the player was replaced cannot land on the new record.
	StartWithMap();
	AddPlayer(5, 76561198100000005ULL);
	FakeEvent activate;
	activate.name = "player_activate";
	activate.fields["userid"].kind = FakeEventField::Kind::Slot;
	activate.fields["userid"].slot = 5;
	FireEvent(activate);
	k4::PlayerRecord* old = FindRecord(5);
	CHECK(old != nullptr);
	if (old == nullptr) {
		return;
	}
	const uint64_t oldSteamid = old->steamid;
	AddPlayer(5, 76561198100000006ULL, 2, "New owner");
	CHECK(PumpUntil([] {
		k4::PlayerRecord* current = k4::FindBySlot(5);
		return current != nullptr && current->steamid == 76561198100000006ULL;
	}, 60));
	// The slot now resolves to the new owner; the old record can never be addressed through it
	// (a deferred action or DB callback of the old player is keyed by SteamID64 + handle).
	k4::PlayerRecord* current = FindRecord(5);
	CHECK(current != nullptr && current->steamid == 76561198100000006ULL);
	CHECK(k4::FindBySlot(5) == current);
	(void)oldSteamid;
	EndPlugin();

	// A completion whose player disconnected before the deferred reward is never paid out.
	StartWithMap();
	JoinPlayer(3, 76561198100000003ULL);
	k4::PlayerRecord* record = LoadedRecord(3);
	CHECK(record != nullptr);
	if (record != nullptr) {
		CHECK(WaitForMissions(record, 1));
		FireEvent(PlayerDeathEvent(3));
		// The player leaves before the MarkCompleted callback is consumed.
		g_clients[3].connected = false;
		Pump(400);
		CHECK(g_serverCommands.empty());
	}
	EndPlugin();
}

// =============================================================================================
// menus (C10): creation, selection, destruction, map-end/unload teardown
// =============================================================================================

void TestMenus() {
	StartWithMap();
	JoinPlayer(3, 76561198200000003ULL);
	k4::PlayerRecord* record = LoadedRecord(3);
	CHECK(record != nullptr);
	if (record == nullptr) {
		return;
	}
	CHECK(WaitForMissions(record, 1));
	Pump();
	FireCommand("mission", 3, s2sdk::ConCommandContext::Console);
	Pump();
	CheckEqInt(g_createMenuCalls, 2, "the reserved id 0 dummy plus the mission menu");
	const s2sdk::MenuId menuId = g_displayed[3];
	CHECK(menuId != 0);
	CHECK(k4::g_menuIdZeroReserved);
	CheckEq(g_lastMenuType, "hud", "menu type stored");
	const FakeMenu& menu = g_menus[menuId];
	// One informational reset line, the mission and the two locked VIP slots of the original menu.
	CheckEqInt(static_cast<int64_t>(menu.items.size()), 4, "reset line + mission + two locked VIP slots");
	CHECK(menu.exitButton);
	CHECK(!menu.exitBackButton);
	CHECK(!menu.closeOnSelect);
	CheckEqInt(menu.pagination, 5, "five items per page");
	CHECK(menu.title.find('{') == std::string::npos);
	for (const auto& item : menu.items) {
		CHECK(item.second.find('{') == std::string::npos);
	}
	int missionIndex = -1;
	for (std::size_t index = 0; index < menu.items.size(); ++index) {
		if (!menu.items[index].first.empty()) {
			missionIndex = static_cast<int>(index);
			CheckEq(menu.items[index].first, std::to_string(record->missions.front().id),
					"item info is the mission id");
		}
	}
	CHECK(missionIndex >= 0);

	// Selecting the item prints the four detail lines and keeps the menu displayed.
	menu.handler(menuId, s2sdk::MenuAction::Select, 3, missionIndex);
	Pump();
	CHECK(ChatContains("Mission #1"));
	CHECK(ChatContains("Task: "));
	CheckEqInt(static_cast<int64_t>(g_displayed[3]), static_cast<int64_t>(menuId), "menu stays open");

	// A stale index does nothing.
	menu.handler(menuId, s2sdk::MenuAction::Select, 3, 99);
	Pump();
	CheckEqInt(g_destroyMenuCalls, 0, "no destroy from a select");

	// Exit cancels the session and the next update destroys the menu.
	menu.handler(menuId, s2sdk::MenuAction::Cancel, 3, static_cast<int32_t>(s2sdk::MenuCancelReason::Exit));
	Pump();
	CHECK(g_destroyMenuCalls >= 1);
	CheckEqInt(static_cast<int64_t>(g_displayed[3]), 0, "no menu displayed after exit");
	CHECK(!g_menus[menuId].valid);

	// Reopening replaces the previous session and resolves the player's hudmenu choice.
	g_hudmenuChoice = "wasd";
	FireCommand("mission", 3, s2sdk::ConCommandContext::Console);
	Pump();
	CheckEq(g_lastMenuType, "wasd", "player choice honoured");
	g_hudmenuChoice.clear();

	// The map end ends the session first (Disconnect) and destroys the menu: no session survives.
	const s2sdk::MenuId openId = g_displayed[3];
	CHECK(openId != 0);
	const int cancelBefore = g_cancelMenuCalls;
	FireMapEnd();
	CHECK(g_cancelMenuCalls > cancelBefore);
	CheckEqInt(static_cast<int64_t>(g_displayed[3]), 0, "map end closed the session");
	CHECK(!g_menus[openId].valid);
	EndPlugin();

	// A backend that cannot be switched to a builtin type keeps its menu (A7_UNRESOLVED) instead of
	// running a custom close over the dying client layer.
	StartWithMap();
	JoinPlayer(3, 76561198200000013ULL);
	CHECK(LoadedRecord(3) != nullptr);
	CHECK(WaitForMissions(FindRecord(3), 1));
	Pump();
	FireCommand("mission", 3, s2sdk::ConCommandContext::Console);
	Pump();
	const s2sdk::MenuId stuckId = g_displayed[3];
	CHECK(stuckId != 0);
	g_cancelClearsDisplay = false; // CancelClientMenu no longer ends the session (the fake)
	g_setMenuTypeFails = true;     // and the type cannot be switched
	g_logs.clear();
	FireMapEnd();
	CHECK(Logged(plg::Severity::Error, "A7_UNRESOLVED"));
	CHECK(g_menus[stuckId].valid);
	g_cancelClearsDisplay = true;
	g_setMenuTypeFails = false;
	EndPlugin();

	// A session that stays displayed but CAN be switched to a builtin type is destroyed safely.
	StartWithMap();
	JoinPlayer(3, 76561198200000014ULL);
	CHECK(LoadedRecord(3) != nullptr);
	CHECK(WaitForMissions(FindRecord(3), 1));
	Pump();
	FireCommand("mission", 3, s2sdk::ConCommandContext::Console);
	Pump();
	const s2sdk::MenuId switchId = g_displayed[3];
	CHECK(switchId != 0);
	g_cancelClearsDisplay = false; // the cancel does not end the session
	g_logs.clear();
	FireMapEnd();
	CheckEq(g_menus[switchId].type, "chat", "type switched to a builtin before destroy");
	CHECK(!g_menus[switchId].valid);
	CHECK(!Logged(plg::Severity::Error, "A7_UNRESOLVED"));
	g_cancelClearsDisplay = true;
	EndPlugin();

	// An incomplete binding set disables the menu (no CreateMenu, one log line).
	ResetFakes();
	ResetFiles();
	__s2sdk_GetMenuType = nullptr;
	CHECK(StartPlugin());
	g_currentMap = "de_dust2";
	FireMapStart();
	Pump();
	JoinPlayer(3, 76561198200000023ULL);
	CHECK(LoadedRecord(3) != nullptr);
	CHECK(WaitForMissions(FindRecord(3), 1));
	Pump();
	FireCommand("mission", 3, s2sdk::ConCommandContext::Console);
	Pump();
	CheckEqInt(g_createMenuCalls, 0, "no menu without the full binding set");
	CHECK(Logged(plg::Severity::Error, "menu bindings are incomplete"));
	EndPlugin();

	// No registered menu type: no dummy menu is burned either.
	ResetFakes();
	ResetFiles();
	g_menuTypeRegisteredHud = false;
	g_menuTypeRegisteredWasd = false;
	g_menuTypeRegisteredButton = false;
	CHECK(StartPlugin());
	g_currentMap = "de_dust2";
	FireMapStart();
	Pump();
	JoinPlayer(3, 76561198200000033ULL);
	CHECK(LoadedRecord(3) != nullptr);
	CHECK(WaitForMissions(FindRecord(3), 1));
	Pump();
	FireCommand("mission", 3, s2sdk::ConCommandContext::Console);
	Pump();
	CheckEqInt(g_createMenuCalls, 0, "no menu without a menu type");
	CHECK(!k4::g_menuIdZeroReserved);
	CHECK(Logged(plg::Severity::Error, "no s2sdk menu type is registered"));
	EndPlugin();
}

// =============================================================================================
// recovery: an unfinished player load must not leave the player without missions forever
// =============================================================================================

void TestLoadRecovery() {
	StartWithMap();
	JoinPlayer(3, 76561198700000003ULL);
	k4::PlayerRecord* record = LoadedRecord(3);
	CHECK(record != nullptr);
	if (record == nullptr) {
		return;
	}
	CHECK(WaitForMissions(record, 1));

	// Simulate a load that never completed (a lost retry): the in-memory record is reset, the row
	// is still in the database.
	record->loaded = false;
	record->missions.clear();
	// The once-a-minute pass is the safety net: it asks again for a connected, unloaded player.
	Pump(60000);
	CHECK(PumpUntil([record] { return record->loaded; }));
	CHECK(WaitForMissions(record, 1));
	CheckEqInt(static_cast<int64_t>(record->missions.size()), 1, "one mission after the minute recovery");

	// The same reset across a map change: HandleMapStarted asks again instead of skipping the
	// player for the rest of the session.
	record->loaded = false;
	record->missions.clear();
	FireMapEnd();
	g_currentMap = "de_overpass";
	FireMapStart();
	Pump();
	CHECK(PumpUntil([record] { return record->loaded; }));
	CHECK(WaitForMissions(record, 1));
	CheckEqInt(static_cast<int64_t>(record->missions.size()), 1, "one mission after the map-start recovery");
	EndPlugin();
}

// =============================================================================================
// A7/C11: the unload teardown and a clean reload
// =============================================================================================

void TestUnloadAndReload() {
	StartWithMap();
	JoinPlayer(3, 76561198300000003ULL);
	CHECK(LoadedRecord(3) != nullptr);
	CHECK(WaitForMissions(FindRecord(3), 1));
	Pump();
	FireCommand("mission", 3, s2sdk::ConCommandContext::Console);
	Pump();
	CHECK(g_displayed[3] != 0);
	CHECK(!g_eventHooks.empty());
	const int removedBefore = static_cast<int>(g_removedCommands.size());
	EndPlugin();
	// Every hook and command is gone, and the map listeners are unregistered.
	CheckEqInt(static_cast<int64_t>(g_eventHooks.size()), 0, "all events unhooked");
	CHECK(static_cast<int>(g_removedCommands.size()) > removedBefore);
	CHECK(std::find(g_removedCommands.begin(), g_removedCommands.end(), "mission") != g_removedCommands.end());
	CheckEqInt(g_mapStartUnregisters, 1, "map start unregistered");
	CheckEqInt(g_mapEndUnregisters, 1, "map end unregistered");
	CHECK(std::none_of(g_menus.begin(), g_menus.end(),
					   [](const auto& entry) { return entry.second.valid; }));
	CHECK(k4::g_players.empty());
	CHECK(!k4::g_mapGate.Ready());
	// A second teardown is harmless.
	EndPlugin();

	// A reload registers everything exactly once and works again.
	const int commandCallsBefore = g_addConsoleCommandCalls;
	const int mapStartRegistersBefore = g_mapStartRegisters;
	CHECK(StartPlugin());
	CHECK(g_addConsoleCommandCalls >= commandCallsBefore + 2);
	CheckEqInt(g_mapStartRegisters, mapStartRegistersBefore + 1, "one map start registration per load");
	g_currentMap = "de_nuke";
	FireMapStart();
	Pump();
	CHECK(k4::g_mapGate.Ready());
	JoinPlayer(4, 76561198300000004ULL);
	CHECK(LoadedRecord(4) != nullptr);
	CHECK(WaitForMissions(FindRecord(4), 1));
	Pump();
	FireCommand("mission", 4, s2sdk::ConCommandContext::Console);
	Pump();
	CHECK(g_displayed[4] != 0);
	EndPlugin();
}

// =============================================================================================
// secrets: the webhook URL and token never reach the log
// =============================================================================================

void TestWebhookSecrets() {
	ResetFakes();
	ResetFiles(R"({
		"K4Missions": {
			"MissionCommands": ["mission"],
			"MinimumPlayers": 1,
			"MissionAmountNormal": 1,
			"MissionAmountVip": 1,
			"ResetMode": "Daily",
			"WebhookUrl": "https://discord.com/api/webhooks/1234567890/SuperSecretToken"
		}
	})");
	CHECK(StartPlugin());
	CHECK(Logged(plg::Severity::Warning, "webhook template not found"));
	CHECK(!AnyLogContains("SuperSecretToken"));
	CHECK(!AnyLogContains("discord.com/api/webhooks"));
	// A completed mission queues a webhook whose missing template is reported without the URL.
	g_currentMap = "de_dust2";
	FireMapStart();
	Pump();
	JoinPlayer(3, 76561198400000003ULL, 2, "Bob");
	k4::PlayerRecord* record = LoadedRecord(3);
	CHECK(record != nullptr);
	if (record != nullptr) {
		CHECK(WaitForMissions(record, 1));
		g_logs.clear();
		FireEvent(PlayerDeathEvent(3));
		Pump(400);
		CHECK(!AnyLogContains("SuperSecretToken"));
		CHECK(!AnyLogContains("discord.com/api/webhooks"));
		CHECK(!g_chatLines.empty());
	}
	EndPlugin();
	CHECK(!AnyLogContains("SuperSecretToken"));
}

// =============================================================================================
// fail soft: null bindings, broken files, unreachable database
// =============================================================================================

void TestFailSoft() {
	// Every s2sdk binding null: the plugin still loads, stays harmless and never returns Failed.
	ResetFakes();
	ResetFiles();
	NullAllS2sdkBindings();
	CHECK(StartPlugin());
	Pump();
	g_currentMap = "de_dust2";
	Pump();
	CHECK(!k4::g_mapGate.Ready());
	CheckEqInt(g_clientCalls, 0, "no client call without bindings");
	CHECK(Logged(plg::Severity::Error, "map listener Register/Unregister pair is incomplete"));
	CHECK(Logged(plg::Severity::Error, "AddConsoleCommand/RemoveCommand/RemoveCommandListener set is incomplete"));
	CHECK(Logged(plg::Severity::Error, "HookEvent/UnhookEvent pair is unavailable"));
	EndPlugin();
	ResetFakes();

	// Malformed config.json and missions.json: defaults/no missions, the plugin still loads.
	ResetFakes();
	ResetFiles("{ not json", "also not json");
	CHECK(StartPlugin());
	Pump();
	CHECK(Logged(plg::Severity::Error, "the built-in defaults are used"));
	CHECK(Logged(plg::Severity::Error, "missions.json"));
	EndPlugin();

	// Missing config, missions and databases files.
	ResetFakes();
	ResetFiles();
	std::filesystem::remove(PluginConfigDir() / "config.json");
	std::filesystem::remove(PluginConfigDir() / "missions.json");
	std::filesystem::remove(PluginConfigDir() / "databases.cfg");
	CHECK(StartPlugin());
	CHECK(Logged(plg::Severity::Error, "cannot read"));
	CHECK(Logged(plg::Severity::Error, "falling back to a local SQLite database"));
	// Without missions no mission can be assigned; the empty SQLite database still loads players.
	g_currentMap = "de_dust2";
	FireMapStart();
	Pump();
	JoinPlayer(3, 76561198500000003ULL);
	k4::PlayerRecord* record = LoadedRecord(3);
	CHECK(record != nullptr);
	if (record != nullptr) {
		CheckEqInt(static_cast<int64_t>(record->missions.size()), 0, "no missions without definitions");
	}
	EndPlugin();

	// An unsupported driver is refused (the original's PostgreSQL), missions are not persisted.
	ResetFakes();
	ResetFiles(kDefaultConfig, kDefaultMissions, R"("Databases" { "k4_missions" { "driver" "postgres" } })");
	CHECK(StartPlugin());
	CHECK(Logged(plg::Severity::Error, "is not supported (MySQL/MariaDB and SQLite only)"));
	CHECK(k4::g_database == nullptr);
	EndPlugin();

	// An unreachable SQLite path: the migration fails once, logs once, arms a retry and never blocks
	// the game thread; a joining player stays unloaded instead of getting an empty list.
	ResetFakes();
	ResetFiles(kDefaultConfig, kDefaultMissions, R"("Databases"
	{
		"k4_missions" { "driver" "sqlite" "sqlite_path" "unusable" }
	})");
	std::filesystem::create_directories(PluginDataDir() / "unusable"); // a directory, not a file
	CHECK(StartPlugin());
	g_currentMap = "de_dust2";
	FireMapStart();
	Pump();
	JoinPlayer(3, 76561198500000013ULL);
	CHECK(PumpUntil([] { return !k4::g_schemaAttemptRunning && k4::g_schemaRetryArmed; }, 400));
	CHECK(Logged(plg::Severity::Error, "retrying in the background"));
	k4::PlayerRecord* pending = FindRecord(3);
	CHECK(pending != nullptr);
	if (pending != nullptr) {
		CHECK(!pending->loaded);
		CheckEqInt(static_cast<int64_t>(pending->missions.size()), 0, "no missions while the schema is missing");
	}
	// The retry clock is an accumulator: a > 5 s update fires the next attempt.
	Pump(6000);
	CHECK(k4::g_schemaAttemptRunning || k4::g_schemaRetryArmed);
	EndPlugin();

	// A missing vip/permissions/hudmenu never breaks the plugin.
	ResetFakes();
	ResetFiles();
	__vip_VIP_IsVIPLoaded = nullptr;
	__vip_VIP_IsClientVIP = nullptr;
	__vip_VIP_WarmupPeriod = nullptr;
	__permissions_HasPermission = nullptr;
	__hudmenu_GetClientMenuType = nullptr;
	CHECK(StartPlugin());
	g_currentMap = "de_dust2";
	FireMapStart();
	Pump();
	JoinPlayer(3, 76561198500000023ULL);
	CHECK(LoadedRecord(3) != nullptr);
	CHECK(WaitForMissions(FindRecord(3), 1));
	Pump();
	FireCommand("mission", 3, s2sdk::ConCommandContext::Console);
	Pump();
	CheckEq(g_lastMenuType, "hud", "default type without hudmenu");
	EndPlugin();
}

// =============================================================================================
// the database of the plugin: real SQLite file, the same schema migration OnPluginStart runs
// =============================================================================================

void TestStoreAgainstSqlite() {
	StartWithMap();
	CHECK(PumpUntil([] { return k4::g_store != nullptr; }));
	CHECK(k4::g_store != nullptr);
	if (k4::g_store == nullptr) {
		return;
	}
	JoinPlayer(3, 76561198600000003ULL);
	k4::PlayerRecord* record = LoadedRecord(3);
	CHECK(record != nullptr);
	if (record != nullptr) {
		CHECK(WaitForMissions(record, 1));
		CHECK(record->missions.front().id > 0);
		FireEvent(PlayerDeathEvent(3));
		CHECK(PumpUntil([] { return !g_serverCommands.empty(); }));
	}
	// The DB file exists under <data>/k4_missions.
	CHECK(std::filesystem::exists(PluginDataDir() / "test.sqlite3"));
	EndPlugin();
}

} // namespace

int main() {
	++g_checks;
	g_root = std::filesystem::temp_directory_path()
			 / ("k4_missions_plugin_tests_" + std::to_string(static_cast<long>(::getpid())));
	g_configsRoot = g_root / "configs";
	g_dataRoot = g_root / "data";
	ResetFakes();

	TestPaths();
	TestPhraseLanguage();
	TestMapGate();
	TestClientGuards();
	TestSchemaGate();
	TestCommands();
	TestMissionFlow();
	TestMissionVariants();
	TestPlaytimeAndGates();
	TestIdentity();
	TestMenus();
	TestLoadRecovery();
	TestUnloadAndReload();
	TestWebhookSecrets();
	TestFailSoft();
	TestStoreAgainstSqlite();

	std::error_code error;
	std::filesystem::remove_all(g_root, error);
	std::printf("k4_missions_plugin_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
