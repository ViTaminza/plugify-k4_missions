// k4_missions — the cs2-menus colour tags of the K4 phrase file.
//
// The phrases carry cs2-menus colour tags ("{GRAY}[ {LIME}K4-Missions {GRAY}]") and the previous
// port printed them through pisexcompat: PrintToChat() colourised the tags into the CS2 chat
// control bytes, while menu titles/item texts and the server console got them stripped. The port
// keeps the same table and the same two operations (the sibling standalone ports as_maps /
// as_checkcheats / graffiti carry their own copy — no shared helper library, PLUGIN_STANDARD §7).
//
// Header-only and engine-free: the plugin and its tests share it.

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace k4 {

struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

// cs2-menus utils.h colors_text[]/colors_hex[], the same table pisexcompat carried. The last entry
// is the phrase-file line break: "\n" -> U+2029, which the CS2 chat renderer shows as a new line.
inline constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
														 {"{WHITE}", "\x01"},
														 {"{RED}", "\x02"},
														 {"{LIGHTPURPLE}", "\x03"},
														 {"{GREEN}", "\x04"},
														 {"{LIME}", "\x05"},
														 {"{LIGHTGREEN}", "\x06"},
														 {"{DARKRED}", "\x07"},
														 {"{GRAY}", "\x08"},
														 {"{LIGHTOLIVE}", "\x09"},
														 {"{OLIVE}", "\x10"},
														 {"{LIGHTBLUE}", "\x0B"},
														 {"{BLUE}", "\x0C"},
														 {"{PURPLE}", "\x0E"},
														 {"{LIGHTRED}", "\x0F"},
														 {"{GRAYBLUE}", "\x0A"},
														 {"\\n", "\xe2\x80\xa9"}}};

// strip=false: the tag becomes its chat control byte; strip=true: it is removed and the phrase-file
// line break "\n" becomes a real newline (menu texts / server console).
inline std::string ReplaceColorTags(std::string_view text, bool strip) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		const std::string_view replacement =
			strip ? (color.tag == "\\n" ? std::string_view("\n") : std::string_view()) : color.bytes;
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), replacement);
			pos += replacement.size();
		}
	}
	return out;
}

// What pisexcompat's PrintToChat() did with the formatted phrase.
inline std::string ColorizeChat(std::string_view text) {
	return ReplaceColorTags(text, /*strip=*/false);
}

// What the menu backends and the server console got (no colour markup there).
inline std::string StripColorTags(std::string_view text) {
	return ReplaceColorTags(text, /*strip=*/true);
}

} // namespace k4
