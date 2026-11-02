// k4_missions — Discord webhook notifications.
//
// The original WebhookService (Services/WebhookService.cs) reads a template from the plugin's
// discord/ directory, replaces {placeholders} with JSON-escaped values and POSTs the result to
// the configured Discord webhook URL. The three templates (complete.json, complete_all.json,
// reset.json) are the original's, shipped under configs/k4_missions/discord/.
//
// Transport: s2sdk has no HTTP API, so this re-creates ISteamHTTP on the server's own
// libsteam_api.so exactly like plugins/cs2ac/src/webhook.cpp does (dlopen + the documented
// SteamAPI_ISteamHTTP_* C accessors + a CCallbackBase-layout call result). The code is copied
// on purpose - the task forbids linking cs2ac. Requests are asynchronous; Pump() (called from
// OnPluginUpdate) never blocks.

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace k4 {

class WebhookService {
public:
	struct Impl;

	WebhookService();
	~WebhookService();
	WebhookService(const WebhookService&) = delete;
	WebhookService& operator=(const WebhookService&) = delete;

	// `discordDir` is <configs>/k4_missions/discord; an empty `url` disables the service.
	void Configure(std::string discordDir, std::string url);
	bool Enabled() const;

	// Queues the payload built from the <discordDir>/<templateFile> template cached by Configure().
	// A template that was missing at Configure() is skipped with a warning.
	void Send(const std::string& templateFile, const std::vector<std::pair<std::string, std::string>>& replacements);

	// Advances the request state machine; call once per OnPluginUpdate.
	void Pump();

	// Cancels an in-flight request (plugin unload / reload).
	void Shutdown();

private:
	std::unique_ptr<Impl> m_impl;
};

} // namespace k4
