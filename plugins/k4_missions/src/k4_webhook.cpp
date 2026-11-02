// k4_missions — the Steam HTTP transport and the webhook queue.
//
// The ISteamHTTP half is copied from plugins/cs2ac/src/webhook.cpp (see that file's header for the
// original design notes): s2sdk exposes no HTTP, so the server's libsteam_api.so is loaded with
// dlopen and the documented SteamAPI_ISteamHTTP_* accessors are used with a CCallbackBase-layout
// call result. Unlike cs2ac there is no avatar lookup and no retry/back-off: the K4 original posts
// each completed template once and only logs a warning when Discord answers with an error, which
// is exactly what this class does.

#include "k4_webhook.hpp"

#include "k4_core.hpp"

#include <plg/plugin.hpp>
#include <s2sdk/s2sdk.hpp>

#include <dlfcn.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace k4 {
namespace {

constexpr auto kRequestTimeout = std::chrono::seconds(45);

void LogWarning(std::string_view message) {
	if (plg::Log) {
		plg::Log(std::string(message), plg::Severity::Warning, plg::source_location::current());
	}
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

std::string TemplatePath(const std::string& discordDir, const std::string& templateFile) {
	return discordDir.empty() ? templateFile : discordDir + "/" + templateFile;
}

// HTTPRequestCompleted_t (isteamhttp.h) — the same layout cs2ac verified against the SDK.
struct HTTPRequestCompleted_t {
	enum { k_iCallback = 2102 }; // k_iSteamHTTPCallbacks (2101) + 1
	uint32_t m_hRequest;
	uint64_t m_ulContextValue;
	bool m_bRequestSuccessful;
	int32_t m_eStatusCode;
	uint32_t m_unBodySize;
};
static_assert(sizeof(HTTPRequestCompleted_t) == 32, "HTTPRequestCompleted_t layout");
static_assert(offsetof(HTTPRequestCompleted_t, m_eStatusCode) == 20, "HTTPRequestCompleted_t layout");

// Valve's CCallbackBase layout: vtable + the two data members.
class CallbackBase {
public:
	virtual void Run(void* pvParam) = 0;
	virtual void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) = 0;
	virtual int GetCallbackSizeBytes() = 0;

	enum {
		k_ECallbackFlagsRegistered = 0x01,
		k_ECallbackFlagsGameServer = 0x02,
	};
	uint8_t m_nCallbackFlags;
	int32_t m_iCallback;

protected:
	CallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
	~CallbackBase() = default;
};

struct SteamHttpApi {
	void* module {};
	void* (*getHttp)() {};
	uint32_t (*CreateHTTPRequest)(void*, int32_t, const char*) {};
	bool (*SetHTTPRequestRawPostBody)(void*, uint32_t, const char*, uint8_t*, uint32_t) {};
	bool (*SendHTTPRequest)(void*, uint32_t, uint64_t*) {};
	bool (*ReleaseHTTPRequest)(void*, uint32_t) {};
	void (*RegisterCallResult)(CallbackBase*, uint64_t) {};
	void (*UnregisterCallResult)(CallbackBase*, uint64_t) {};
	void* http {};
};

SteamHttpApi g_steam;
bool g_symbolFailure {};

template<typename Fn>
Fn ResolveSymbol(void* module, const char* name) {
	return reinterpret_cast<Fn>(dlsym(module, name));
}

bool ResolveSteamHttp() {
	if (g_symbolFailure) {
		return false;
	}
	if (g_steam.getHttp == nullptr) {
		std::vector<std::string> candidates = {"libsteam_api.so"};
		if (__s2sdk_GetGameDirectory) {
			const std::string gameDir(s2sdk::GetGameDirectory());
			if (!gameDir.empty()) {
				candidates.push_back(gameDir + "/bin/linuxsteamrt64/libsteam_api.so");
				candidates.push_back(gameDir + "/../bin/linuxsteamrt64/libsteam_api.so");
			}
		}
		candidates.push_back("bin/linuxsteamrt64/libsteam_api.so");
		for (const std::string& candidate : candidates) {
			if (void* handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
				g_steam.module = handle;
				break;
			}
		}
		if (g_steam.module == nullptr) {
			for (const std::string& candidate : candidates) {
				if (void* handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL)) {
					g_steam.module = handle;
					break;
				}
			}
		}
		if (g_steam.module == nullptr) {
			LogWarning("k4_missions: libsteam_api.so could not be loaded; Discord webhooks stay disabled.");
			return false;
		}
		for (const char* name : {"SteamAPI_SteamGameServerHTTP_v004", "SteamAPI_SteamGameServerHTTP_v003",
								 "SteamAPI_SteamGameServerHTTP_v002"}) {
			g_steam.getHttp = ResolveSymbol<decltype(g_steam.getHttp)>(g_steam.module, name);
			if (g_steam.getHttp) {
				break;
			}
		}
		g_steam.CreateHTTPRequest = ResolveSymbol<decltype(g_steam.CreateHTTPRequest)>(g_steam.module, "SteamAPI_ISteamHTTP_CreateHTTPRequest");
		g_steam.SetHTTPRequestRawPostBody =
			ResolveSymbol<decltype(g_steam.SetHTTPRequestRawPostBody)>(g_steam.module, "SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody");
		g_steam.SendHTTPRequest = ResolveSymbol<decltype(g_steam.SendHTTPRequest)>(g_steam.module, "SteamAPI_ISteamHTTP_SendHTTPRequest");
		g_steam.ReleaseHTTPRequest = ResolveSymbol<decltype(g_steam.ReleaseHTTPRequest)>(g_steam.module, "SteamAPI_ISteamHTTP_ReleaseHTTPRequest");
		g_steam.RegisterCallResult = ResolveSymbol<decltype(g_steam.RegisterCallResult)>(g_steam.module, "SteamAPI_RegisterCallResult");
		g_steam.UnregisterCallResult = ResolveSymbol<decltype(g_steam.UnregisterCallResult)>(g_steam.module, "SteamAPI_UnregisterCallResult");

		const bool complete = g_steam.getHttp && g_steam.CreateHTTPRequest && g_steam.SetHTTPRequestRawPostBody
							  && g_steam.SendHTTPRequest && g_steam.ReleaseHTTPRequest
							  && g_steam.RegisterCallResult && g_steam.UnregisterCallResult;
		if (!complete) {
			g_symbolFailure = true;
			LogWarning("k4_missions: libsteam_api.so does not export the expected ISteamHTTP entry points; "
					   "Discord webhooks stay disabled.");
			g_steam.getHttp = nullptr;
			return false;
		}
	}
	if (g_steam.http == nullptr) {
		g_steam.http = g_steam.getHttp();
	}
	return g_steam.http != nullptr;
}

} // namespace

struct WebhookService::Impl {
	struct PendingPayload {
		std::string body;
	};

	std::string discordDir;
	std::string url;
	// The three templates, read once in Configure(): Send() runs on the game thread and must not
	// open files there for every completion.
	std::unordered_map<std::string, std::string> templates;
	std::deque<PendingPayload> queue;

	uint32_t request {};
	uint64_t call {};
	std::chrono::steady_clock::time_point requestStarted {};
	bool responseReady {};
	bool responseFailed {};
	int responseStatus {};
	bool httpUnavailableWarned {};
	bool templateWarningLogged {};

	void SendNext();
	void CancelRequest();
	void CompleteResponse(bool failed, int status);
};

namespace {

class WebhookCallback final : public CallbackBase {
public:
	explicit WebhookCallback(WebhookService::Impl* impl) : impl(impl) {
		m_nCallbackFlags = static_cast<uint8_t>(k_ECallbackFlagsGameServer);
		m_iCallback = HTTPRequestCompleted_t::k_iCallback;
	}

	void Run(void* pvParam) override { Run(pvParam, false, 0); }
	void Run(void* pvParam, bool bIOFailure, uint64_t /*hSteamAPICall*/) override {
		try {
			if (impl == nullptr) {
				return;
			}
			if (bIOFailure || pvParam == nullptr) {
				impl->CompleteResponse(true, 0);
				return;
			}
			const auto* completed = static_cast<const HTTPRequestCompleted_t*>(pvParam);
			impl->CompleteResponse(false, static_cast<int>(completed->m_eStatusCode));
		} catch (...) {
			// Never unwind into Steam's callback pump.
		}
	}
	int GetCallbackSizeBytes() override { return static_cast<int>(sizeof(HTTPRequestCompleted_t)); }

	WebhookService::Impl* impl {};
};

WebhookCallback g_callback {nullptr};

} // namespace

void WebhookService::Impl::CancelRequest() {
	// Unregister before releasing: a pending call result would otherwise fire into a destroyed Impl.
	if (call != 0 && g_steam.UnregisterCallResult) {
		g_steam.UnregisterCallResult(&g_callback, call);
		call = 0;
	}
	if (request != 0 && g_steam.http && g_steam.ReleaseHTTPRequest) {
		g_steam.ReleaseHTTPRequest(g_steam.http, request);
	}
	request = 0;
	responseReady = false;
	g_callback.impl = nullptr;
}

void WebhookService::Impl::CompleteResponse(bool failed, int status) {
	responseReady = true;
	responseFailed = failed;
	responseStatus = status;
}

void WebhookService::Impl::SendNext() {
	if (queue.empty()) {
		return;
	}
	if (!ResolveSteamHttp()) {
		if (!httpUnavailableWarned) {
			httpUnavailableWarned = true;
			LogWarning("k4_missions: Steam's HTTP service is not available; the Discord webhook queue is dropped.");
		}
		queue.clear();
		return;
	}

	request = g_steam.CreateHTTPRequest(g_steam.http, 3 /*k_EHTTPMethodPOST*/, url.c_str());
	if (request == 0
		|| !g_steam.SetHTTPRequestRawPostBody(g_steam.http, request, "application/json",
											 reinterpret_cast<uint8_t*>(queue.front().body.data()),
											 static_cast<uint32_t>(queue.front().body.size()))) {
		if (request != 0) {
			g_steam.ReleaseHTTPRequest(g_steam.http, request);
			request = 0;
		}
		LogWarning("k4_missions: Discord webhook request could not be created; the report was dropped.");
		queue.pop_front();
		return;
	}

	uint64_t call = 0;
	if (!g_steam.SendHTTPRequest(g_steam.http, request, &call)) {
		g_steam.ReleaseHTTPRequest(g_steam.http, request);
		request = 0;
		LogWarning("k4_missions: Discord webhook request could not be sent; the report was dropped.");
		queue.pop_front();
		return;
	}
	requestStarted = std::chrono::steady_clock::now();
	responseReady = false;
	g_callback.impl = this;
	this->call = call;
	g_steam.RegisterCallResult(&g_callback, call);
}

WebhookService::WebhookService() : m_impl(std::make_unique<Impl>()) {}
WebhookService::~WebhookService() {
	Shutdown();
}

void WebhookService::Configure(std::string discordDir, std::string url) {
	m_impl->CancelRequest();
	m_impl->queue.clear();
	m_impl->templates.clear();
	m_impl->discordDir = std::move(discordDir);
	m_impl->url = std::move(url);
	m_impl->httpUnavailableWarned = false;
	if (m_impl->url.empty()) {
		return; // webhooks disabled: no need to touch the disk
	}
	for (const char* name : {"complete.json", "complete_all.json", "reset.json"}) {
		const std::string path = TemplatePath(m_impl->discordDir, name);
		std::ifstream input(path, std::ios::binary);
		std::string body;
		if (input) {
			std::ostringstream buffer;
			buffer << input.rdbuf();
			body = buffer.str();
		}
		if (body.empty()) {
			LogWarning("k4_missions: webhook template not found: " + path);
			continue;
		}
		m_impl->templates.emplace(name, std::move(body));
	}
}

bool WebhookService::Enabled() const {
	return !m_impl->url.empty();
}

void WebhookService::Send(const std::string& templateFile,
						  const std::vector<std::pair<std::string, std::string>>& replacements) {
	if (m_impl->url.empty()) {
		return;
	}
	const auto found = m_impl->templates.find(templateFile);
	if (found == m_impl->templates.end()) {
		LogWarning("k4_missions: webhook template not found: " + TemplatePath(m_impl->discordDir, templateFile));
		return;
	}
	std::string body = found->second;
	for (const auto& [key, value] : replacements) {
		body = ReplaceAll(std::move(body), "{" + key + "}", JsonEscape(value));
	}
	m_impl->queue.push_back({std::move(body)});
}

void WebhookService::Pump() {
	Impl& impl = *m_impl;
	if (!impl.url.empty() && impl.request != 0 && !impl.responseReady
		&& std::chrono::steady_clock::now() - impl.requestStarted > kRequestTimeout) {
		impl.CancelRequest();
		LogWarning("k4_missions: Discord webhook request timed out; the report was dropped.");
		impl.queue.pop_front();
	}

	if (impl.responseReady) {
		const bool failed = impl.responseFailed;
		const int status = impl.responseStatus;
		impl.responseReady = false;
		if (impl.request != 0 && g_steam.http && g_steam.ReleaseHTTPRequest) {
			g_steam.ReleaseHTTPRequest(g_steam.http, impl.request);
			impl.request = 0;
		}
		// Steam unregisters a call result once it has fired.
		impl.call = 0;
		g_callback.impl = nullptr;
		if (failed || status < 200 || status > 299) {
			LogWarning("k4_missions: Discord webhook request failed (HTTP " + std::to_string(status) + ").");
		}
		if (!impl.queue.empty()) {
			impl.queue.pop_front();
		}
		return;
	}

	if (impl.request == 0 && !impl.queue.empty()) {
		impl.SendNext();
	}
}

void WebhookService::Shutdown() {
	if (!m_impl) {
		return;
	}
	m_impl->CancelRequest();
	m_impl->queue.clear();
}

} // namespace k4
