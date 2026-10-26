#pragma once

#include <cmath>

namespace wfmaxon {

struct DamageOverrideDecision {
    bool apply = false;
    float value = 0.0f;
};

inline DamageOverrideDecision EvaluateDamageOverride(bool enabled,
                                                      bool alt_only,
                                                      float original_damage,
                                                      bool right_alt_held) noexcept {
    if (!enabled || (alt_only && !right_alt_held) ||
        !std::isfinite(original_damage) || original_damage <= 0.0f) {
        return {};
    }
    return {true, 1.0e9f};
}

} // namespace wfmaxon
