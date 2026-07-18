#pragma once
#include "../entity.hpp"
#include <unordered_map>
#include <deque>
#include <functional>
#include <utility>

namespace net {

struct PlayerInputFrame {
    std::uint32_t frame = 0;
    std::vector<std::uint8_t> bytes;
};

class InputRingBuffer {
public:
    void push(std::uint32_t player_id, PlayerInputFrame frame) {
        auto& ring = _frames[player_id];
        ring.push_back(std::move(frame));
        while (ring.size() > _max_frames) ring.pop_front();
    }

    bool get(std::uint32_t player_id, std::uint32_t frame, PlayerInputFrame& out) const {
        auto it = _frames.find(player_id);
        if (it == _frames.end()) return false;
        for (const auto& f : it->second) {
            if (f.frame == frame) { out = f; return true; }
        }
        return false;
    }

    void clear() { _frames.clear(); }

private:
    std::unordered_map<std::uint32_t, std::deque<PlayerInputFrame>> _frames;
    std::size_t _max_frames = 240;
};

class RollbackController {
public:
    using StepFn = std::function<void(std::uint32_t frame, EntityList& state)>;

    void clear() {
        _snapshots.clear();
        _inputs.clear();
    }

    void capture(std::uint32_t frame, const EntityList& state) {
        _snapshots.push_back({frame, deep_clone(state)});
        while (_snapshots.size() > _max_snapshots) _snapshots.pop_front();
    }

    void push_input(std::uint32_t player_id, PlayerInputFrame input) {
        _inputs.push(player_id, std::move(input));
    }

    const EntityList* snapshot(std::uint32_t frame) const {
        for (auto it = _snapshots.rbegin(); it != _snapshots.rend(); ++it)
            if (it->first == frame) return &it->second;
        return nullptr;
    }

    bool resimulate_from(std::uint32_t from_frame, std::uint32_t to_frame, EntityList& state, StepFn step) const {
        const EntityList* base = snapshot(from_frame);
        if (!base) return false;
        state = deep_clone(*base);
        for (std::uint32_t f = from_frame + 1; f <= to_frame; ++f) {
            step(f, state);
        }
        return true;
    }

private:
    static EntityList deep_clone(const EntityList& src) {
        EntityList out;
        out.reserve(src.size());
        for (const auto& e : src) out.push_back(e.deep_clone());
        return out;
    }

    std::deque<std::pair<std::uint32_t, EntityList>> _snapshots;
    InputRingBuffer _inputs;
    std::size_t _max_snapshots = 30;
};

} // namespace net