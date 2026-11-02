// k4_missions — the phrase file reader and the language codes (engine-free).
//
// The previous port loaded phrases through pisexcompat: LoadPhrases() parsed the Pisex KV1 file,
// DetectLanguage() read core.cfg "ServerLang" -> s2sdk's ServerLanguage -> "en", and GetPhrase()
// answered "" for an unknown key (which is what the plugin logs/prints for a missing key).
//
// This file keeps the parsing and the lookup only; the two host-specific inputs (the language
// chain's s2sdk fallback and the file path) stay in src/plugin.cpp, so the tests parse the real
// shipped file and check the same fallback order without an engine.
//
// Fallback order per phrase, exactly the old wrapper's: requested language, then "en", then the
// first language the entry carries (a partially translated file still shows something).

#pragma once

#include <cctype>
#include <map>
#include <string>
#include <string_view>

#include "kv1.hpp"

namespace k4 {

// "Russian"/"russian"/"ru" -> "ru", ... ; an unknown name is lower-cased unchanged (the old
// wrapper's NormalizeLanguage).
inline std::string NormalizeLanguage(std::string_view name) {
	std::string lower;
	lower.reserve(name.size());
	for (const char c : name) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (lower == "russian" || lower == "ru") return "ru";
	if (lower == "english" || lower == "en") return "en";
	if (lower == "ukrainian" || lower == "ua" || lower == "uk") return "ua";
	if (lower == "portuguese" || lower == "pt-br" || lower == "brazilian") return "pt-BR";
	if (lower == "spanish" || lower == "es") return "es";
	if (lower == "german" || lower == "de") return "de";
	if (lower == "french" || lower == "fr") return "fr";
	if (lower == "polish" || lower == "pl") return "pl";
	if (lower == "turkish" || lower == "tr") return "tr";
	if (lower == "schinese" || lower == "zh-cn") return "zh-CN";
	return lower;
}

// Parses a "Phrases" { "<key>" { "<lang>" "<text>" } } document. Returns false (and clears `out`)
// when the text is not valid KV1, so the caller keeps its previous table instead of a half one.
inline bool ParsePhrasesText(std::string_view text, std::string_view language,
							 std::map<std::string, std::string>& out) {
	out.clear();
	const std::optional<KvNode> document = ParseKvDocument(text);
	if (!document) {
		return false;
	}
	for (const KvNode& phrase : document->children) {
		if (phrase.name.empty() || phrase.children.empty()) {
			continue;
		}
		std::string_view text = phrase.GetString(language, {});
		if (text.empty()) {
			text = phrase.GetString("en", {});
		}
		if (text.empty()) {
			// Last resort: any language the entry carries, so a partially translated file still
			// shows something instead of an empty chat line.
			for (const KvNode& candidate : phrase.children) {
				if (candidate.hasValue && !candidate.value.empty()) {
					text = candidate.value;
					break;
				}
			}
		}
		out[phrase.name] = std::string(text);
	}
	return true;
}

// The old GetPhrase(): an unknown key is the empty string (the plugin prints nothing then).
inline const std::string& LookupPhrase(const std::map<std::string, std::string>& phrases,
									   const std::string& key) {
	static const std::string kEmpty;
	const auto found = phrases.find(key);
	return found == phrases.end() ? kEmpty : found->second;
}

} // namespace k4
