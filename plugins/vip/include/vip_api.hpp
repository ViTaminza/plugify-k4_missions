#pragma once
// Consumer-side wrapper for the vip plugin API (the IAdminApi pattern of plugins/admin_system,
// applied to the Pisex IVIPApi).
//
// The generated headers (vip/vip.hpp and friends, produced by bin/plugify-gen from vip.pplugin)
// expose the raw boundary: plain C function pointers, plg::string values, enum classes and no
// captures. This header restores the shape the Pisex VIP modules were written against:
//
//   * the IVIPApi class (here vip::IVIPApi, alias of vip::VipApi) with the exact method names,
//     argument order and default arguments of the original's vip.h;
//   * std::function callbacks with captures (the ItemSelectableCallback / ItemTogglableCallback /
//     ItemDisplayCallback typedefs and the six event callbacks), dispatched from static
//     trampolines keyed by feature name (menu callbacks) or per plugin (events, which the
//     original keyed by PluginId - one listener per plugin, re-registering replaces);
//   * the plain C++ enums VIP_ValueType / VIP_ToggleState / VIP_FeatureType;
//   * std::string instead of plg::string and the original's const char* returns.

//   * VIP_PrintToCenter(int, const char*, ...) stays variadic: it formats into a buffer (256
//     bytes, like the original) and calls the exported method.
//
// Porting a module is then mostly mechanical: keep `IVIPApi* g_pVIPCore` / `g_pVIPCore`, make it
// point at vip::VipApi::Get(), and drop the SourceMM::PluginId of the registration calls (they
// never had one on the VIP interface, so VIP_OnVIPLoaded(cb) etc. are unchanged):
//
//   #include <vip_api.hpp>                    // -I plugins/vip/include
//   #include <vip/vip.hpp>                    // -I .../include/external/plugify/include
//
//   vip::IVIPApi* g_pVIPCore = &vip::IVIPApi::Get();
//
//   g_pVIPCore->VIP_RegisterFeature("bhop", VIP_BOOL, TOGGLABLE);
//   if (g_pVIPCore->VIP_IsClientVIP(i)) { float f = g_pVIPCore->VIP_GetClientFeatureFloat(i, "speed"); }
//
// Lifetime notes (identical to the original's behaviour):
//   * VIP_GetTranslate() returns a pointer into a per-plugin cache that keeps the value until the
//     same key is queried again (the original's g_vecPhrases map pointers did not move either).
//   * VIP_GetClientFeatureString() returns a pointer into a single per-plugin buffer, valid until
//     the next call - exactly the static thread_local buffer the original used.
//   * VIP_GetClientVIPGroup() returns a pointer into a per-slot cache, valid until the group of
//     that slot is queried again (the original returned the std::string of the player record).
//   * Every trampoline swallows exceptions (the boundary must not unwind into the plugin) and
//     logs them through plg::Log.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vip/vip.hpp>

namespace vip {

// --- enums (exactly the ones of the original's vip.h) --------------------------
enum VIP_ValueType {
	VIP_NULL = 0, // no data
	VIP_INT,      // integer
	VIP_FLOAT,    // floating point
	VIP_BOOL,     // boolean (1/0)
	VIP_STRING,   // string
};

enum VIP_ToggleState {
	DISABLED = 0, // switched off
	ENABLED,      // switched on
	NO_ACCESS,    // no access (unused by the core, kept for parity)
};

enum VIP_FeatureType {
	TOGGLABLE = 0, // on/off in the menu
	SELECTABLE,    // click only
	HIDE,          // hidden
};

// --- callback types (the IVIPApi typedefs, captured std::function) ---------------
using ItemTogglableCallback = std::function<bool(int iSlot, const char* szFeature, VIP_ToggleState eOldStatus, VIP_ToggleState& eNewStatus)>;
using ItemSelectableCallback = std::function<bool(int iSlot, const char* szFeature)>;
using ItemDisplayCallback = std::function<std::string(int iSlot, const char* szFeature)>;

using ReadyCallbackFunc = std::function<void()>;
using SpawnCallbackFunc = std::function<void(int iSlot, int iTeam, bool bIsVIP)>;
using ClientLoadedOrDisconnectCallbackFunc = std::function<void(int iSlot, bool bIsVIP)>;
using VIPAddCallbackFunc = std::function<void(int iSlot)>;
using VIPRemoveCallbackFunc = std::function<void(int iSlot, int iReason)>;

namespace detail {

inline std::string ToStd(const plg::string& value) {
	return std::string(std::string_view(value));
}

inline plg::string ToPlg(std::string_view value) {
	return plg::string(value);
}

// One non-empty plg::Log call; the consumer may run with logging disabled.
inline void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// True when the plugify module filled in the binding slots, i.e. when vip is loaded.
inline bool Available() {
	return __vip_VIP_IsVIPLoaded != nullptr;
}

struct FeatureEntry {
	ItemSelectableCallback select;
	ItemTogglableCallback toggle;
	ItemDisplayCallback display;
};

// Everything this plugin registered, plus the string caches that back the const char* returns.
// One instance per plugin (the header is included by exactly one translation unit of each
// module, but the accessor is a function-local static so several TUs share it).
struct State {
	std::map<std::string, FeatureEntry, std::less<>> features;

	ReadyCallbackFunc ready;
	ClientLoadedOrDisconnectCallbackFunc clientLoaded;
	ClientLoadedOrDisconnectCallbackFunc clientDisconnect;
	SpawnCallbackFunc playerSpawn;
	VIPRemoveCallbackFunc vipRemoved;
	VIPAddCallbackFunc vipAdded;

	// Stable addresses: node based containers, so a returned c_str() survives later queries.
	std::map<std::string, std::string, std::less<>> translations;
	std::vector<std::string> groups = std::vector<std::string>(64); // index slot
	std::string featureValue;                                       // the original's thread_local buffer
	std::string cookieValue;                                        // separate: reading a cookie must not clobber it
};

inline State& S() {
	static State state;
	return state;
}

// --- trampolines ---------------------------------------------------------------
// All of them are plain function pointers, so they can be handed to the vip plugin; the captures
// live in State. Exceptions never cross the boundary.

inline bool SelectTrampoline(int32_t slot, const plg::string& feature) {
	try {
		State& state = S();
		const auto it = state.features.find(ToStd(feature));
		if (it == state.features.end() || !it->second.select) {
			return false;
		}
		return it->second.select(slot, it->first.c_str());
	} catch (...) {
		LogError("vip: item select callback threw");
		return false;
	}
}

inline bool ToggleTrampoline(int32_t slot, const plg::string& feature, ::vip::ToggleState oldStatus, ::vip::ToggleState& newStatus) {
	try {
		State& state = S();
		const auto it = state.features.find(ToStd(feature));
		if (it == state.features.end() || !it->second.toggle) {
			return false;
		}
		// The boundary uses the generated enum class; the modules use the plain enums of the
		// original, so both directions are mapped through the integer value.
		VIP_ToggleState oldValue = static_cast<VIP_ToggleState>(static_cast<int>(oldStatus));
		VIP_ToggleState newValue = static_cast<VIP_ToggleState>(static_cast<int>(newStatus));
		const bool blocked = it->second.toggle(slot, it->first.c_str(), oldValue, newValue);
		newStatus = static_cast<::vip::ToggleState>(static_cast<int>(newValue));
		return blocked;
	} catch (...) {
		LogError("vip: item toggle callback threw");
		return false;
	}
}

inline plg::string DisplayTrampoline(int32_t slot, const plg::string& feature) {
	try {
		State& state = S();
		const auto it = state.features.find(ToStd(feature));
		if (it == state.features.end() || !it->second.display) {
			return plg::string();
		}
		return ToPlg(it->second.display(slot, it->first.c_str()));
	} catch (...) {
		LogError("vip: item display callback threw");
		return plg::string();
	}
}

inline void ReadyTrampoline() {
	try {
		if (const auto& callback = S().ready) {
			callback();
		}
	} catch (...) {
		LogError("vip: OnVIPLoaded callback threw");
	}
}

inline void ClientLoadedTrampoline(int32_t slot, bool isVIP) {
	try {
		if (const auto& callback = S().clientLoaded) {
			callback(slot, isVIP);
		}
	} catch (...) {
		LogError("vip: OnClientLoaded callback threw");
	}
}

inline void ClientDisconnectTrampoline(int32_t slot, bool isVIP) {
	try {
		if (const auto& callback = S().clientDisconnect) {
			callback(slot, isVIP);
		}
	} catch (...) {
		LogError("vip: OnClientDisconnect callback threw");
	}
}

inline void PlayerSpawnTrampoline(int32_t slot, int32_t team, bool isVIP) {
	try {
		if (const auto& callback = S().playerSpawn) {
			callback(slot, team, isVIP);
		}
	} catch (...) {
		LogError("vip: OnPlayerSpawn callback threw");
	}
}

inline void VIPRemovedTrampoline(int32_t slot, int32_t reason) {
	try {
		if (const auto& callback = S().vipRemoved) {
			callback(slot, reason);
		}
	} catch (...) {
		LogError("vip: OnVIPClientRemoved callback threw");
	}
}

inline void VIPAddedTrampoline(int32_t slot) {
	try {
		if (const auto& callback = S().vipAdded) {
			callback(slot);
		}
	} catch (...) {
		LogError("vip: OnVIPClientAdded callback threw");
	}
}

} // namespace detail

// --- the API -------------------------------------------------------------------
// One instance per plugin; methods mirror IVIPApi one to one (same names, same argument order,
// same defaults), so ported modules only have to change the type of g_pVIPCore.
class VipApi {
public:
	static VipApi& Get() {
		static VipApi api;
		return api;
	}

	VipApi(const VipApi&) = delete;
	VipApi& operator=(const VipApi&) = delete;

	// --- core ------------------------------------------------------------------
	bool VIP_IsVIPLoaded() const { return detail::Available() && ::vip::VIP_IsVIPLoaded(); }

	// Translated phrase, cached per key; the original's map lookup returned an empty string for an
	// unknown key (it inserted an empty value), and so does this.
	const char* VIP_GetTranslate(const char* phrase) {
		if (!phrase) {
			return "";
		}
		detail::State& state = detail::S();
		const auto it = state.translations.find(phrase);
		if (it != state.translations.end()) {
			return it->second.c_str();
		}
		std::string translated;
		if (detail::Available()) {
			translated = detail::ToStd(::vip::VIP_GetTranslate(detail::ToPlg(phrase)));
		}
		return state.translations.emplace(phrase, std::move(translated)).first->second.c_str();
	}

	// Drops the phrase cache (vip_reload reloads vip.phrases.txt in the core).
	void ReloadTranslations() { detail::S().translations.clear(); }

	// --- client state ------------------------------------------------------------
	bool VIP_IsClientVIP(int iSlot) const { return detail::Available() && ::vip::VIP_IsClientVIP(iSlot); }

	int VIP_GetClientAccessTime(int iSlot) const {
		return detail::Available() ? static_cast<int>(::vip::VIP_GetClientAccessTime(iSlot)) : -1;
	}

	bool VIP_SetClientAccessTime(int iSlot, int iTime, bool bInDB = true) const {
		return detail::Available() && ::vip::VIP_SetClientAccessTime(iSlot, iTime, bInDB);
	}

	bool VIP_SetClientVIPGroup(int iSlot, const char* szGroup, bool bInDB = true) const {
		return detail::Available() && szGroup && ::vip::VIP_SetClientVIPGroup(iSlot, detail::ToPlg(szGroup), bInDB);
	}

	// Pointer into a per-slot cache; valid until the group of the slot is queried again.
	const char* VIP_GetClientVIPGroup(int iSlot) {
		if (iSlot < 0 || iSlot >= 64) {
			return "";
		}
		std::string group;
		if (detail::Available()) {
			group = detail::ToStd(::vip::VIP_GetClientVIPGroup(iSlot));
		}
		std::string& slot = detail::S().groups[static_cast<size_t>(iSlot)];
		slot = std::move(group);
		return slot.c_str();
	}

	bool VIP_GiveClientVIP(int iSlot, int iTime, const char* szGroup, bool bAddToDB = 1) const {
		return detail::Available() && szGroup &&
			   ::vip::VIP_GiveClientVIP(iSlot, iTime, detail::ToPlg(szGroup), bAddToDB);
	}

	bool VIP_RemoveClientVIP(int iSlot, bool bNotify, bool bInDB = 1) const {
		return detail::Available() && ::vip::VIP_RemoveClientVIP(iSlot, bNotify, bInDB);
	}

	// --- feature values -------------------------------------------------------------
	int VIP_GetClientFeatureInt(int iSlot, const char* szFeature) const {
		return detail::Available() && szFeature ? static_cast<int>(::vip::VIP_GetClientFeatureInt(iSlot, detail::ToPlg(szFeature))) : -1;
	}

	bool VIP_GetClientFeatureBool(int iSlot, const char* szFeature) const {
		return detail::Available() && szFeature && ::vip::VIP_GetClientFeatureBool(iSlot, detail::ToPlg(szFeature));
	}

	float VIP_GetClientFeatureFloat(int iSlot, const char* szFeature) const {
		return detail::Available() && szFeature ? ::vip::VIP_GetClientFeatureFloat(iSlot, detail::ToPlg(szFeature)) : 1.0f;
	}

	// Pointer into a single per-plugin buffer, valid until the next call (the original's
	// static thread_local std::string).
	const char* VIP_GetClientFeatureString(int iSlot, const char* szFeature) {
		if (!szFeature) {
			return "";
		}
		std::string value;
		if (detail::Available()) {
			value = detail::ToStd(::vip::VIP_GetClientFeatureString(iSlot, detail::ToPlg(szFeature)));
		}
		detail::S().featureValue = std::move(value);
		return detail::S().featureValue.c_str();
	}

	// --- features registration --------------------------------------------------------
	void VIP_RegisterFeature(const char* szFeature, VIP_ValueType eValType = VIP_NULL, VIP_FeatureType eType = TOGGLABLE,
							 ItemSelectableCallback Item_select_callback = nullptr,
							 ItemTogglableCallback Item_togglable_callback = nullptr,
							 ItemDisplayCallback Item_display_callback = nullptr) {
		if (!szFeature || !*szFeature || !detail::Available()) {
			return;
		}
		detail::State& state = detail::S();
		detail::FeatureEntry& entry = state.features[szFeature];
		entry.select = std::move(Item_select_callback);
		entry.toggle = std::move(Item_togglable_callback);
		entry.display = std::move(Item_display_callback);
		// Null callbacks stay null at the boundary, exactly like the VIPFunctions fields of the
		// original (the core then toggles/selects without a veto hook).
		::vip::VIP_RegisterFeature(detail::ToPlg(szFeature), static_cast<::vip::ValueType>(eValType),
								   static_cast<::vip::FeatureType>(eType),
								   entry.select ? &detail::SelectTrampoline : nullptr,
								   entry.toggle ? &detail::ToggleTrampoline : nullptr,
								   entry.display ? &detail::DisplayTrampoline : nullptr);
	}

	// --- groups / game state -------------------------------------------------------------
	bool VIP_IsValidVIPGroup(const char* szGroup) const {
		return detail::Available() && szGroup && ::vip::VIP_IsValidVIPGroup(detail::ToPlg(szGroup));
	}

	bool VIP_PistolRound() const { return detail::Available() && ::vip::VIP_PistolRound(); }
	bool VIP_WarmupPeriod() const { return detail::Available() && ::vip::VIP_WarmupPeriod(); }
	int VIP_GetTotalRounds() const { return detail::Available() ? static_cast<int>(::vip::VIP_GetTotalRounds()) : 0; }

	// The original handed out the CGameEntitySystem pointer of the utils API; s2sdk exposes no
	// engine layout, so no pointer can cross the boundary. Ported modules use the s2sdk entity
	// functions instead; this returns nullptr for source parity.
	void* VIP_GetEntitySystem() const { return nullptr; }

	// --- cookies ---------------------------------------------------------------------------
	bool VIP_SetClientCookie(int iSlot, const char* sCookieName, const char* sData) const {
		return detail::Available() && sCookieName && sData &&
			   ::vip::VIP_SetClientCookie(iSlot, detail::ToPlg(sCookieName), detail::ToPlg(sData));
	}

	const char* VIP_GetClientCookie(int iSlot, const char* sCookieName) {
		if (!sCookieName) {
			return "";
		}
		std::string value;
		if (detail::Available()) {
			value = detail::ToStd(::vip::VIP_GetClientCookie(iSlot, detail::ToPlg(sCookieName)));
		}
		detail::S().cookieValue = std::move(value);
		return detail::S().cookieValue.c_str();
	}

	// --- output / menu -----------------------------------------------------------------------
	// Variadic like the original; formats into 256 bytes (the original's buffer) and forwards.
	void VIP_PrintToCenter(int iSlot, const char* msg, ...) {
		if (!msg || !detail::Available()) {
			return;
		}
		char buffer[256];
		va_list args;
		va_start(args, msg);
		std::vsnprintf(buffer, sizeof(buffer), msg, args);
		va_end(args);
		::vip::VIP_PrintToCenter(iSlot, detail::ToPlg(buffer));
	}

	void VIP_OpenMenu(int iSlot) const {
		if (detail::Available()) {
			::vip::VIP_OpenMenu(iSlot);
		}
	}

	// --- callbacks ----------------------------------------------------------------------------
	// Every event keeps the semantics of the original: one listener per plugin, registering again
	// replaces the previous one, and Unregister removes it. The server deduplicates by the
	// trampoline pointer, which is constant per event and plugin.
	void VIP_OnVIPLoaded(ReadyCallbackFunc callback) {
		detail::S().ready = std::move(callback);
		if (detail::Available()) {
			::vip::VIP_OnVIPLoaded_Register(&detail::ReadyTrampoline);
		}
	}
	void VIP_OnVIPLoadedUnregister() {
		detail::S().ready = nullptr;
		if (detail::Available()) {
			::vip::VIP_OnVIPLoaded_Unregister(&detail::ReadyTrampoline);
		}
	}

	void VIP_OnClientLoaded(ClientLoadedOrDisconnectCallbackFunc callback) {
		detail::S().clientLoaded = std::move(callback);
		if (detail::Available()) {
			::vip::VIP_OnClientLoaded_Register(&detail::ClientLoadedTrampoline);
		}
	}
	void VIP_OnClientLoadedUnregister() {
		detail::S().clientLoaded = nullptr;
		if (detail::Available()) {
			::vip::VIP_OnClientLoaded_Unregister(&detail::ClientLoadedTrampoline);
		}
	}

	void VIP_OnClientDisconnect(ClientLoadedOrDisconnectCallbackFunc callback) {
		detail::S().clientDisconnect = std::move(callback);
		if (detail::Available()) {
			::vip::VIP_OnClientDisconnect_Register(&detail::ClientDisconnectTrampoline);
		}
	}
	void VIP_OnClientDisconnectUnregister() {
		detail::S().clientDisconnect = nullptr;
		if (detail::Available()) {
			::vip::VIP_OnClientDisconnect_Unregister(&detail::ClientDisconnectTrampoline);
		}
	}

	void VIP_OnPlayerSpawn(SpawnCallbackFunc callback) {
		detail::S().playerSpawn = std::move(callback);
		if (detail::Available()) {
			::vip::VIP_OnPlayerSpawn_Register(&detail::PlayerSpawnTrampoline);
		}
	}
	void VIP_OnPlayerSpawnUnregister() {
		detail::S().playerSpawn = nullptr;
		if (detail::Available()) {
			::vip::VIP_OnPlayerSpawn_Unregister(&detail::PlayerSpawnTrampoline);
		}
	}

	void VIP_OnVIPClientRemoved(VIPRemoveCallbackFunc callback) {
		detail::S().vipRemoved = std::move(callback);
		if (detail::Available()) {
			::vip::VIP_OnVIPClientRemoved_Register(&detail::VIPRemovedTrampoline);
		}
	}
	void VIP_OnVIPClientRemovedUnregister() {
		detail::S().vipRemoved = nullptr;
		if (detail::Available()) {
			::vip::VIP_OnVIPClientRemoved_Unregister(&detail::VIPRemovedTrampoline);
		}
	}

	void VIP_OnVIPClientAdded(VIPAddCallbackFunc callback) {
		detail::S().vipAdded = std::move(callback);
		if (detail::Available()) {
			::vip::VIP_OnVIPClientAdded_Register(&detail::VIPAddedTrampoline);
		}
	}
	void VIP_OnVIPClientAddedUnregister() {
		detail::S().vipAdded = nullptr;
		if (detail::Available()) {
			::vip::VIP_OnVIPClientAdded_Unregister(&detail::VIPAddedTrampoline);
		}
	}

private:
	VipApi() = default;
};

// The original interface name, so a ported module only changes the initialization line:
//
//     vip::IVIPApi* g_pVIPCore = &vip::IVIPApi::Get();
using IVIPApi = VipApi;

inline VipApi& Core() {
	return VipApi::Get();
}

} // namespace vip
