#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <functional>

namespace engine_det {

inline std::uint64_t fnv1a64(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= (std::uint64_t)c;
        h *= 1099511628211ull;
    }
    return h ? h : 0x9E3779B97F4A7C15ull;
}

inline std::uint64_t& _seed_ref() {
    static std::uint64_t seed = 0xC0FFEE123456789ull;
    return seed;
}

inline std::mt19937_64& session_rng() {
    static std::mt19937_64 rng{_seed_ref()};
    return rng;
}

inline void seed_session(std::uint64_t seed) {
    if (seed == 0) seed = 0xC0FFEE123456789ull;
    _seed_ref() = seed;
    session_rng().seed(seed);
}

inline void seed_session_from_string(const std::string& s) {
    seed_session(fnv1a64(s));
}

inline std::uint64_t session_seed() {
    return _seed_ref();
}

inline float uniform_float(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(session_rng());
}

inline int uniform_int(int lo, int hi) {
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(session_rng());
}

inline bool deterministic_mode() {
#ifdef ENGINE_DETERMINISTIC
    return true;
#else
    return false;
#endif
}

} // namespace engine_det
