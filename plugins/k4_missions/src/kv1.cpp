// k4_missions — the KV1 reader of src/kv1.hpp (grammar and semantics documented there).
#include "kv1.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <utility>

namespace k4 {
namespace {

char Lower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (Lower(a[i]) != Lower(b[i])) {
			return false;
		}
	}
	return true;
}

std::string_view TrimView(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

// A recursive descent over the token stream; builds the whole tree.
class Parser {
public:
	explicit Parser(std::string_view text) : text_(text) {}

	bool ParseDocument(std::vector<KvNode>& out) { return ParseNodes(&out, false); }

private:
	void SkipSpace() {
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++pos_;
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
				pos_ += 2;
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
				pos_ += 2;
				while (pos_ + 1 < text_.size() && !(text_[pos_] == '*' && text_[pos_ + 1] == '/')) {
					++pos_;
				}
				pos_ = (pos_ + 1 < text_.size()) ? pos_ + 2 : text_.size();
				continue;
			}
			// A UTF-8 BOM is not a token; the game's files carry one occasionally.
			if (pos_ + 2 < text_.size() && static_cast<unsigned char>(text_[pos_]) == 0xEF &&
				static_cast<unsigned char>(text_[pos_ + 1]) == 0xBB &&
				static_cast<unsigned char>(text_[pos_ + 2]) == 0xBF) {
				pos_ += 3;
				continue;
			}
			break;
		}
	}

	void SkipLine() {
		while (pos_ < text_.size() && text_[pos_] != '\n') {
			++pos_;
		}
	}

	// One token into scratch_. False at end of input / at a closing brace (the caller handles both).
	bool ReadToken() {
		SkipSpace();
		if (pos_ >= text_.size() || text_[pos_] == '}') {
			return false;
		}
		scratch_.clear();
		if (text_[pos_] == '"') {
			++pos_;
			while (pos_ < text_.size()) {
				const char c = text_[pos_++];
				if (c == '\\' && pos_ < text_.size()) {
					const char escaped = text_[pos_++];
					switch (escaped) {
					case 'n':
						scratch_.push_back('\n');
						break;
					case 'r':
						scratch_.push_back('\r');
						break;
					case 't':
						scratch_.push_back('\t');
						break;
					case '"':
						scratch_.push_back('"');
						break;
					case '\\':
						scratch_.push_back('\\');
						break;
					default:
						// Valve keeps the backslash of an unknown escape ("\x" -> "\x").
						scratch_.push_back('\\');
						scratch_.push_back(escaped);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				scratch_.push_back(c);
			}
			failed_ = true; // unterminated string
			return true;
		}
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			if (c == '/' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == '/' || text_[pos_ + 1] == '*')) {
				break;
			}
			scratch_.push_back(c);
			++pos_;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return pos_ < text_.size() && text_[pos_] == c;
	}

	// false only on a malformed document.
	bool ParseNodes(std::vector<KvNode>* out, bool insideBlock) {
		while (true) {
			SkipSpace();
			if (pos_ >= text_.size()) {
				if (insideBlock) {
					failed_ = true; // missing '}' - reject the whole document
				}
				return !failed_;
			}
			if (text_[pos_] == '}') {
				if (!insideBlock) {
					failed_ = true; // stray '}' at the top level
				}
				++pos_;
				return !failed_;
			}

			if (!ReadToken()) {
				return !failed_;
			}
			if (!scratch_.empty() && scratch_[0] == '#') {
				SkipLine(); // #base / #include / #define: parsed and dropped, never followed
				continue;
			}

			KvNode node;
			node.name = scratch_;

			if (!PeekIs('{')) {
				if (ReadToken()) {
					node.value = scratch_;
					node.hasValue = true;
				}
			}

			if (PeekIs('{')) {
				++pos_;
				if (!ParseNodes(&node.children, true)) {
					return false;
				}
			}
			out->push_back(std::move(node));
		}
	}

	std::string_view text_;
	std::size_t pos_ = 0;
	std::string scratch_;
	bool failed_ = false;
};

} // namespace

const KvNode* KvNode::Find(std::string_view key) const {
	if (key.empty()) {
		return nullptr;
	}
	for (const KvNode& child : children) {
		if (EqualNoCase(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

std::string_view KvNode::GetString(std::string_view key, std::string_view def) const {
	const KvNode* found = Find(key);
	if (found == nullptr || !found->hasValue) {
		return def;
	}
	return found->value;
}

int KvNode::GetInt(std::string_view key, int def) const {
	const std::string_view text = TrimView(GetString(key, {}));
	if (text.empty()) {
		return def;
	}
	int value = 0;
	const auto* begin = text.data();
	const auto* end = text.data() + text.size();
	const std::from_chars_result result = std::from_chars(begin, end, value);
	if (result.ec != std::errc {}) {
		// Valve falls back to atoi semantics ("1.5", "abc", "40 // comment" -> 40).
		return std::atoi(std::string(text).c_str());
	}
	return value;
}

std::optional<KvNode> ParseKvDocument(std::string_view text) {
	Parser parser(text);
	std::vector<KvNode> top;
	if (!parser.ParseDocument(top)) {
		return std::nullopt;
	}

	KvNode root;
	if (top.size() == 1 && !top.front().children.empty()) {
		// The Valve rule the old wrapper implemented: a single "Root" { ... } block becomes the
		// document itself, so callers read its keys directly.
		root = std::move(top.front());
	} else {
		root.name = "document";
		root.children = std::move(top);
	}
	return root;
}

std::optional<std::string> ReadKvFile(const std::string& path) {
	if (path.empty()) {
		return std::nullopt;
	}
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace k4
