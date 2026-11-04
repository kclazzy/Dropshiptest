#include "ai/ContentSanitizer.hpp"

#include <array>
#include <cctype>

namespace dropship::ai {

namespace {

bool isAscii(char c, int (*pred)(int)) { return static_cast<unsigned char>(c) < 0x80 && pred(c) != 0; }

std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

bool isAllowedTag(std::string_view name) {
    static constexpr std::array<std::string_view, 11> kAllowed{"p", "br", "ul", "ol", "li", "b",
                                                               "strong", "i", "em", "h2", "h3"};
    for (const auto allowed : kAllowed) {
        if (allowed == name) return true;
    }
    return name == "h4";
}

// Элементы, содержимое которых вообще не должно попасть в описание.
bool isDroppedWithContent(std::string_view name) {
    static constexpr std::array<std::string_view, 7> kDropped{"script", "style", "iframe", "object",
                                                              "embed", "template", "noscript"};
    for (const auto dropped : kDropped) {
        if (dropped == name) return true;
    }
    return false;
}

bool isNameChar(char c) { return isAscii(c, ::isalnum) || c == '-'; }

// Длина сущности (вместе с ';'), начиная с '&', либо 0, если это не сущность.
std::size_t entityLength(std::string_view text, std::size_t at) {
    std::size_t j = at + 1;
    if (j >= text.size()) return 0;
    const std::size_t start = j;
    if (text[j] == '#') {
        ++j;
        const bool hex = j < text.size() && (text[j] == 'x' || text[j] == 'X');
        if (hex) ++j;
        const std::size_t digits = j;
        while (j < text.size() && (hex ? isAscii(text[j], ::isxdigit) : isAscii(text[j], ::isdigit))) ++j;
        if (j == digits || j - digits > 8) return 0;
    } else {
        if (!isAscii(text[j], ::isalpha)) return 0;
        while (j < text.size() && isAscii(text[j], ::isalnum) && j - start <= 32) ++j;
    }
    return (j < text.size() && text[j] == ';') ? j + 1 - at : 0;
}

// Позиция сразу после закрывающего тега </name ...>; npos, если его нет.
std::size_t skipToClosingTag(std::string_view html, std::size_t from, std::string_view name) {
    for (std::size_t pos = html.find("</", from); pos != std::string_view::npos; pos = html.find("</", pos + 2)) {
        const auto candidate = html.substr(pos + 2, name.size());
        if (lowerAscii(candidate) == name) {
            const auto gt = html.find('>', pos);
            return gt == std::string_view::npos ? std::string_view::npos : gt + 1;
        }
    }
    return std::string_view::npos;
}

}  // namespace

std::string sanitizeHtml(std::string_view html) {
    std::string out;
    out.reserve(html.size());
    const std::size_t n = html.size();
    std::size_t i = 0;

    while (i < n) {
        const char c = html[i];

        if (c == '>') {
            out += "&gt;";
            ++i;
        } else if (c == '&') {
            if (const auto len = entityLength(html, i)) {
                out.append(html.substr(i, len));
                i += len;
            } else {
                out += "&amp;";
                ++i;
            }
        } else if (c != '<') {
            out += c;
            ++i;
        } else if (html.substr(i, 4) == "<!--") {
            const auto end = html.find("-->", i + 4);
            if (end == std::string_view::npos) break;  // комментарий не закрыт — остаток отбрасываем
            i = end + 3;
        } else if (i + 1 < n && (html[i + 1] == '!' || html[i + 1] == '?')) {
            const auto end = html.find('>', i);
            if (end == std::string_view::npos) break;
            i = end + 1;
        } else {
            std::size_t j = i + 1;
            const bool closing = j < n && html[j] == '/';
            if (closing) ++j;
            const std::size_t nameStart = j;
            while (j < n && isNameChar(html[j])) ++j;
            if (j == nameStart || !isAscii(html[nameStart], ::isalpha)) {
                out += "&lt;";  // одиночный '<' — это текст
                ++i;
                continue;
            }
            const std::string name = lowerAscii(html.substr(nameStart, j - nameStart));

            // Конец тега ищем с учётом кавычек: <a title="a>b">
            char quote = 0;
            std::size_t k = j;
            bool terminated = false;
            for (; k < n; ++k) {
                const char ch = html[k];
                if (quote != 0) {
                    if (ch == quote) quote = 0;
                } else if (ch == '"' || ch == '\'') {
                    quote = ch;
                } else if (ch == '>') {
                    terminated = true;
                    break;
                }
            }
            if (!terminated) {
                out += "&lt;";
                ++i;
                continue;
            }
            const std::size_t tagEnd = k + 1;

            if (!closing && isDroppedWithContent(name)) {
                const auto after = skipToClosingTag(html, tagEnd, name);
                if (after == std::string_view::npos) break;  // не закрыт — остаток отбрасываем
                i = after;
                continue;
            }
            if (isAllowedTag(name)) {
                if (name == "br") {
                    if (!closing) out += "<br>";
                } else {
                    out += closing ? "</" : "<";
                    out += name;
                    out += '>';
                }
            }
            i = tagEnd;
        }
    }
    return out;
}

}  // namespace dropship::ai
