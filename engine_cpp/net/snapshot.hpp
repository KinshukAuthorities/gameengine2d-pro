#pragma once
#include "../entity.hpp"
#include "../transform_system.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace net {

struct TransformSnapshot {
    std::uint32_t entity_id = 0;
    float x = 0.f, y = 0.f;
    float vx = 0.f, vy = 0.f;
    float angle = 0.f;
};

struct SnapshotFrame {
    std::uint32_t tick = 0;
    std::vector<TransformSnapshot> entities;
};

namespace snap_detail {
inline void append_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((std::uint8_t)((v >> (i * 8)) & 0xFF));
}
inline void append_f32(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t u = 0;
    static_assert(sizeof(float) == sizeof(std::uint32_t), "float must be 32-bit");
    std::memcpy(&u, &v, sizeof(float));
    append_u32(out, u);
}
inline std::uint32_t read_u32(const std::uint8_t* p) {
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}
inline float read_f32(const std::uint8_t* p) {
    std::uint32_t u = read_u32(p);
    float v = 0.f;
    std::memcpy(&v, &u, sizeof(float));
    return v;
}
}

inline std::vector<std::uint8_t> encode_snapshot(const SnapshotFrame& frame) {
    std::vector<std::uint8_t> out;
    out.reserve(12 + frame.entities.size() * 24);
    out.push_back('S'); out.push_back('N'); out.push_back('A'); out.push_back('P');
    out.push_back(1); out.push_back(0); out.push_back(0); out.push_back(0);
    snap_detail::append_u32(out, frame.tick);
    snap_detail::append_u32(out, (std::uint32_t)frame.entities.size());
    for (const auto& e : frame.entities) {
        snap_detail::append_u32(out, e.entity_id);
        snap_detail::append_f32(out, e.x);
        snap_detail::append_f32(out, e.y);
        snap_detail::append_f32(out, e.vx);
        snap_detail::append_f32(out, e.vy);
        snap_detail::append_f32(out, e.angle);
    }
    return out;
}

inline bool decode_snapshot(const std::uint8_t* data, std::size_t len, SnapshotFrame& out) {
    if (!data || len < 12 || !(data[0]=='S' && data[1]=='N' && data[2]=='A' && data[3]=='P')) return false;
    std::size_t off = 8;
    out.tick = snap_detail::read_u32(data + off); off += 4;
    std::uint32_t count = snap_detail::read_u32(data + off); off += 4;
    if (len < off + (std::size_t)count * 24) return false;
    out.entities.clear();
    out.entities.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        TransformSnapshot s;
        s.entity_id = snap_detail::read_u32(data + off); off += 4;
        s.x = snap_detail::read_f32(data + off); off += 4;
        s.y = snap_detail::read_f32(data + off); off += 4;
        s.vx = snap_detail::read_f32(data + off); off += 4;
        s.vy = snap_detail::read_f32(data + off); off += 4;
        s.angle = snap_detail::read_f32(data + off); off += 4;
        out.entities.push_back(s);
    }
    return true;
}

class SnapshotBuffer {
public:
    void clear() { _frames.clear(); }

    void push(SnapshotFrame frame) {
        _frames.push_back(std::move(frame));
        std::sort(_frames.begin(), _frames.end(), [](const SnapshotFrame& a, const SnapshotFrame& b){ return a.tick < b.tick; });
        while (_frames.size() > 3) _frames.erase(_frames.begin());
    }

    bool sample(float alpha, SnapshotFrame& out) const {
        if (_frames.empty()) return false;
        if (_frames.size() == 1) { out = _frames.back(); return true; }
        const auto& a = _frames[_frames.size() - 2];
        const auto& b = _frames.back();
        alpha = std::max(0.f, std::min(1.f, alpha));
        out.tick = (std::uint32_t)((1.f - alpha) * (float)a.tick + alpha * (float)b.tick);
        out.entities.clear();
        out.entities.reserve(std::max(a.entities.size(), b.entities.size()));
        for (const auto& ea : a.entities) {
            auto it = std::find_if(b.entities.begin(), b.entities.end(), [&](const TransformSnapshot& eb){ return eb.entity_id == ea.entity_id; });
            TransformSnapshot s = ea;
            if (it != b.entities.end()) {
                s.x = ea.x + (it->x - ea.x) * alpha;
                s.y = ea.y + (it->y - ea.y) * alpha;
                s.vx = ea.vx + (it->vx - ea.vx) * alpha;
                s.vy = ea.vy + (it->vy - ea.vy) * alpha;
                s.angle = ea.angle + (it->angle - ea.angle) * alpha;
            }
            out.entities.push_back(s);
        }
        for (const auto& eb : b.entities) {
            auto it = std::find_if(out.entities.begin(), out.entities.end(), [&](const TransformSnapshot& s){ return s.entity_id == eb.entity_id; });
            if (it == out.entities.end()) out.entities.push_back(eb);
        }
        return true;
    }

    const std::vector<SnapshotFrame>& frames() const { return _frames; }

private:
    std::vector<SnapshotFrame> _frames;
};

inline SnapshotFrame capture_snapshot(const EntityList& entities) {
    SnapshotFrame frame;
    for (const auto& e : entities) {
        if (!has_component(e, "Transform")) continue;
        TransformSnapshot s;
        s.entity_id = (std::uint32_t)e.value("id", 0);
        s.x = transform::world_x(e);
        s.y = transform::world_y(e);
        auto& t = e["components"]["Transform"];
        s.vx = t.value("velocity_x", 0.f);
        s.vy = t.value("velocity_y", 0.f);
        s.angle = t.value("rotation", 0.f);
        frame.entities.push_back(s);
    }
    return frame;
}

inline void apply_snapshot(EntityList& entities, const SnapshotFrame& frame) {
    for (const auto& s : frame.entities) {
        for (auto& e : entities) {
            if (e.value("id", 0) != (int)s.entity_id) continue;
            if (!has_component(e, "Transform")) continue;
            auto& t = e["components"]["Transform"];
            t["x"] = s.x;
            t["y"] = s.y;
            t["rotation"] = s.angle;
            if (t.contains("velocity_x")) t["velocity_x"] = s.vx;
            if (t.contains("velocity_y")) t["velocity_y"] = s.vy;
            break;
        }
    }
    transform::mark_structure_dirty();
}

} // namespace net