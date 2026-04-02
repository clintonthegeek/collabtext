#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace CollabText {

class CrdtEngine {
public:
    explicit CrdtEngine(uint16_t replica_id = 0);
    ~CrdtEngine();
    CrdtEngine(const CrdtEngine &) = delete;
    CrdtEngine &operator=(const CrdtEngine &) = delete;

    // Text operations. Offsets are UTF-16 code unit positions.
    void insert(int position, const std::string &text);
    void remove(int position, int length);
    std::string text() const;
    int length() const;

    // Undo / redo
    bool undo();
    bool redo();

    // Change notification
    using ChangeCallback = std::function<void()>;
    void setOnChange(ChangeCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace CollabText
