// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

#ifndef ZEN_SWITCHBOARD_DOOR_HPP
#define ZEN_SWITCHBOARD_DOOR_HPP

// A door: a name on a send rule to an office, finer than the office, that the office itself
// judges at the act. Loom holds the grammar -- it parses a door, refuses a malformed one in
// words, says whether one door contains another, and renders it -- and never what a door names:
// a door rule is read by its owner through `describe_authority`, and no delivery carries one.
// docs/reference/capabilities.md#doors
//
//   door       pane door | root door
//   pane door  <office> "/" "*"                     every door beneath the office
//            | <office> "/" <pane>                  the pane's own door
//            | <office> "/" <pane> ":" <id>         a control of the pane
//   root door  <root> ":" <id>                      a root the office declares (`arrange:<id>`)
//   id         <name> | "*" | <name> ".*"           one id, every id, every id beneath <name>
//   name       segments of [A-Za-z0-9_-] joined by single dots
//
// `*` stands only as a whole last segment or as an id's tail after a dot, so containment
// compares whole segments and never a prefix of one: `zengine.files/*` does not contain
// `zengine.files-evil/x`, and `builder.build` does not contain `builder.build-realize`.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loom {

/// The most bytes a door may have. A door is shown to a person beside the rule that carries it;
/// it is a name, not a document.
inline constexpr std::size_t kMaxDoorBytes = 256;

/// One parsed door. Only `parse` makes one, so a door held anywhere is well formed, and two
/// doors are the same door exactly when their texts are equal.
class Door {
public:
    /// Read `text` as a door, or say in `*why` which rule of the grammar it breaks.
    static std::optional<Door> parse(std::string_view text, std::string* why = nullptr) {
        const auto refuse = [&](const std::string& reason) -> std::optional<Door> {
            if (why != nullptr) {
                *why = reason;
            }
            return std::nullopt;
        };
        const std::string quoted = "'" + std::string(text.substr(0, kMaxDoorBytes)) + "'";
        if (text.empty()) {
            return refuse("a door is not empty: it is <office>/<pane>[:<id>], <office>/*, or "
                          "<root>:<id>");
        }
        if (text.size() > kMaxDoorBytes) {
            return refuse("a door is at most " + std::to_string(kMaxDoorBytes) + " bytes");
        }
        Door door;
        door.text_ = std::string(text);
        const std::size_t slash = text.find('/');
        if (slash != std::string_view::npos) {
            // A pane door: <office>/*, <office>/<pane>, or <office>/<pane>:<id>.
            const std::string_view office = text.substr(0, slash);
            const std::string_view rest = text.substr(slash + 1);
            std::string bad;
            if (!plain_name(office, &bad)) {
                return refuse(quoted + " is not a door: its office " + bad);
            }
            door.head_ = std::string(office);
            if (rest == "*") {
                door.pane_ = "*";
                return door;
            }
            const std::size_t colon = rest.find(':');
            const std::string_view pane = rest.substr(0, colon);
            if (!plain_name(pane, &bad)) {
                return refuse(quoted + " is not a door: its pane " + bad +
                              " ('*' stands alone, as <office>/* or <office>/<pane>:*)");
            }
            door.pane_ = std::string(pane);
            if (colon == std::string_view::npos) {
                return door;
            }
            if (!read_id(rest.substr(colon + 1), &door.id_, &bad)) {
                return refuse(quoted + " is not a door: its id " + bad);
            }
            return door;
        }
        // A root door: <root>:<id>.
        const std::size_t colon = text.find(':');
        if (colon == std::string_view::npos) {
            return refuse(quoted + " is not a door: a door is <office>/<pane>[:<id>], "
                                   "<office>/*, or <root>:<id>");
        }
        std::string bad;
        if (!plain_name(text.substr(0, colon), &bad)) {
            return refuse(quoted + " is not a door: its root " + bad);
        }
        door.rooted_ = true;
        door.head_ = std::string(text.substr(0, colon));
        if (!read_id(text.substr(colon + 1), &door.id_, &bad)) {
            return refuse(quoted + " is not a door: its id " + bad);
        }
        return door;
    }

    /// The door as written, which is the door as parsed: the grammar has one spelling per door.
    const std::string& text() const noexcept { return text_; }

    /// Does this door contain `inner`, segment by segment? A door contains itself; `<office>/*`
    /// contains every pane door of that office; `<office>/<pane>:*` contains the pane's controls
    /// and not the pane's own door; an id ending `.*` contains the ids beneath it and not itself
    /// without the star. A pane door and a root door contain one another in neither direction.
    bool contains(const Door& inner) const {
        if (rooted_ != inner.rooted_ || head_ != inner.head_) {
            return false;
        }
        if (rooted_) {
            return id_contains(id_, inner.id_);
        }
        if (pane_ == "*") {
            return true;
        }
        if (inner.pane_ != pane_) {
            return false; // a different pane, or `<office>/*`, which is wider
        }
        if (id_.empty() || inner.id_.empty()) {
            return id_.empty() && inner.id_.empty(); // the pane's own door contains only itself
        }
        return id_contains(id_, inner.id_);
    }

    friend bool operator==(const Door& a, const Door& b) noexcept { return a.text_ == b.text_; }
    friend bool operator!=(const Door& a, const Door& b) noexcept { return a.text_ != b.text_; }

private:
    Door() = default;

    /// One or more segments of letters, digits, '-' and '_', joined by single dots.
    static bool plain_name(std::string_view s, std::string* bad) {
        if (s.empty()) {
            *bad = "is empty";
            return false;
        }
        bool segment_empty = true;
        for (const char c : s) {
            if (c == '.') {
                if (segment_empty) {
                    *bad = "'" + std::string(s) + "' has an empty segment between dots";
                    return false;
                }
                segment_empty = true;
                continue;
            }
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) {
                *bad = c == '*' ? "'" + std::string(s) + "' has a '*' that is not a whole last "
                                                         "segment"
                                : "'" + std::string(s) +
                                      "' has a character outside letters, digits, '-', '_' and "
                                      "'.'";
                return false;
            }
            segment_empty = false;
        }
        if (segment_empty) {
            *bad = "'" + std::string(s) + "' ends with a dot";
            return false;
        }
        return true;
    }

    /// An id: `*`, a name, or a name followed by `.*`; kept as its dot segments.
    static bool read_id(std::string_view s, std::vector<std::string>* out, std::string* bad) {
        if (s == "*") {
            out->push_back("*");
            return true;
        }
        const bool tail = s.size() >= 2 && s.substr(s.size() - 2) == ".*";
        const std::string_view name = tail ? s.substr(0, s.size() - 2) : s;
        if (!plain_name(name, bad)) {
            return false;
        }
        std::size_t from = 0;
        for (std::size_t dot = name.find('.'); dot != std::string_view::npos;
             dot = name.find('.', from)) {
            out->emplace_back(name.substr(from, dot - from));
            from = dot + 1;
        }
        out->emplace_back(name.substr(from));
        if (tail) {
            out->push_back("*");
        }
        return true;
    }

    /// Does the id `outer` contain the id `inner`? Without a star, only itself; with one, every
    /// id that shares the segments before it and has at least as many.
    static bool id_contains(const std::vector<std::string>& outer,
                            const std::vector<std::string>& inner) {
        if (outer.back() != "*") {
            return outer == inner;
        }
        if (inner.size() < outer.size()) {
            return false;
        }
        for (std::size_t k = 0; k + 1 < outer.size(); ++k) {
            if (outer[k] != inner[k]) {
                return false;
            }
        }
        return true;
    }

    std::string text_;
    bool rooted_ = false;          ///< a root door (`<root>:<id>`) rather than a pane door
    std::string head_;             ///< the office of a pane door, or the root of a root door
    std::string pane_;             ///< a pane door's pane, or "*"; empty for a root door
    std::vector<std::string> id_;  ///< the id's dot segments, the last possibly "*"; empty: none
};

} // namespace loom

#endif // ZEN_SWITCHBOARD_DOOR_HPP
