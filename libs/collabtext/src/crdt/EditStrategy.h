#pragma once

#include "crdt/Buffer.h"
#include <random>
#include <string>
#include <vector>

namespace CollabText::Crdt {

struct EditAction {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<std::string> new_text;
};

class EditStrategy {
public:
    virtual ~EditStrategy() = default;
    virtual EditAction next_edit(const Buffer& buf, std::mt19937& rng) = 0;
};

/// Random position, random size, random UTF-8 text. Maximizes edge case
/// coverage. Used for correctness tests.
class RandomStrategy : public EditStrategy {
public:
    EditAction next_edit(const Buffer& buf, std::mt19937& rng) override {
        std::string text = buf.text();
        uint32_t len = static_cast<uint32_t>(text.size());
        uint32_t start = random_byte_offset(rng, text);
        uint32_t end = start;
        if (start < len) {
            std::string suffix = text.substr(start);
            end = start + random_byte_offset(rng, suffix);
        }
        std::string replacement;
        if (rng() % 3 != 0) {
            replacement = random_text(rng, 5);
        }
        return {{{{start, end}}}, {replacement}};
    }

private:
    static std::string random_text(std::mt19937& rng, int maxChars) {
        int count = 1 + (rng() % maxChars);
        std::string result;
        for (int i = 0; i < count; ++i) {
            int kind = rng() % 100;
            if (kind < 60) {
                result += static_cast<char>('a' + (rng() % 26));
            } else if (kind < 75) {
                result += '\xc3';
                result += static_cast<char>(0xa0 + (rng() % 32));
            } else if (kind < 90) {
                result += '\xe4';
                result += static_cast<char>(0xb8 + (rng() % 4));
                result += static_cast<char>(0x80 + (rng() % 64));
            } else {
                result += '\xf0';
                result += '\x9f';
                result += static_cast<char>(0x98 + (rng() % 8));
                result += static_cast<char>(0x80 + (rng() % 64));
            }
        }
        return result;
    }

    static uint32_t random_byte_offset(std::mt19937& rng, const std::string& text) {
        if (text.empty()) return 0;
        std::vector<uint32_t> boundaries = {0};
        for (size_t b = 0; b < text.size(); ) {
            unsigned char c = static_cast<unsigned char>(text[b]);
            if (c < 0x80) b += 1;
            else if ((c & 0xE0) == 0xC0) b += 2;
            else if ((c & 0xF0) == 0xE0) b += 3;
            else b += 4;
            boundaries.push_back(static_cast<uint32_t>(b));
        }
        return boundaries[rng() % boundaries.size()];
    }
};

/// Models a user typing in a text editor. Maintains a cursor position.
/// Action distribution: 50% sequential type, 15% backspace, 10% select+delete,
/// 10% cursor jump, 10% paste, 5% select+replace.
class RealisticStrategy : public EditStrategy {
public:
    EditAction next_edit(const Buffer& buf, std::mt19937& rng) override {
        uint32_t len = buf.visible_length();
        if (m_cursor > len) m_cursor = len;

        int action = rng() % 100;

        if (action < 50) {
            // Sequential type: insert 1-5 chars at cursor
            int count = 1 + static_cast<int>(rng() % 5);
            std::string text = random_ascii(rng, count);
            uint32_t pos = m_cursor;
            m_cursor = pos + static_cast<uint32_t>(text.size());
            return {{{{pos, pos}}}, {text}};
        } else if (action < 65) {
            // Backspace: delete 1-3 chars behind cursor
            uint32_t del = 1 + (rng() % 3);
            if (del > m_cursor) del = m_cursor;
            if (del == 0) return next_edit(buf, rng);  // nothing to delete, retry
            uint32_t start = m_cursor - del;
            m_cursor = start;
            return {{{{start, start + del}}}, {""}};
        } else if (action < 75) {
            // Select+delete: delete 5-20 chars near cursor
            if (len == 0) return next_edit(buf, rng);
            uint32_t del = 5 + (rng() % 16);
            uint32_t start = m_cursor;
            if (start + del > len) {
                if (len >= del) start = len - del;
                else { start = 0; del = len; }
            }
            m_cursor = start;
            return {{{{start, start + del}}}, {""}};
        } else if (action < 85) {
            // Cursor jump: move to a random valid position
            m_cursor = len > 0 ? rng() % (len + 1) : 0;
            // After jump, do a small type
            int count = 1 + static_cast<int>(rng() % 3);
            std::string text = random_ascii(rng, count);
            uint32_t pos = m_cursor;
            m_cursor = pos + static_cast<uint32_t>(text.size());
            return {{{{pos, pos}}}, {text}};
        } else if (action < 95) {
            // Paste: insert 10-50 chars at cursor
            int count = 10 + static_cast<int>(rng() % 41);
            std::string text = random_ascii(rng, count);
            uint32_t pos = m_cursor;
            m_cursor = pos + static_cast<uint32_t>(text.size());
            return {{{{pos, pos}}}, {text}};
        } else {
            // Select+replace: delete 5-15 chars, insert 5-15 chars
            if (len < 5) return next_edit(buf, rng);
            uint32_t del = 5 + (rng() % 11);
            uint32_t start = m_cursor;
            if (start + del > len) {
                if (len >= del) start = len - del;
                else { start = 0; del = len; }
            }
            int ins = 5 + static_cast<int>(rng() % 11);
            std::string text = random_ascii(rng, ins);
            m_cursor = start + static_cast<uint32_t>(text.size());
            return {{{{start, start + del}}}, {text}};
        }
    }

private:
    uint32_t m_cursor = 0;

    static std::string random_ascii(std::mt19937& rng, int count) {
        std::string result(count, ' ');
        for (int i = 0; i < count; ++i)
            result[i] = static_cast<char>('a' + (rng() % 26));
        return result;
    }
};

} // namespace CollabText::Crdt
