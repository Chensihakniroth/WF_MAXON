#include "../src/damage_policy.h"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using wfmaxon::EvaluateDamageOverride;

    assert(!EvaluateDamageOverride(false, false, 10.0f, false).apply);
    assert(EvaluateDamageOverride(true, false, 10.0f, false).apply);
    assert(EvaluateDamageOverride(true, false, 10.0f, false).value == 1.0e9f);
    assert(!EvaluateDamageOverride(true, false, 0.0f, false).apply);
    assert(!EvaluateDamageOverride(true, false, -1.0f, false).apply);
    assert(!EvaluateDamageOverride(true, false,
                                  std::numeric_limits<float>::infinity(), false).apply);
    assert(!EvaluateDamageOverride(true, true, 10.0f, false).apply);
    assert(EvaluateDamageOverride(true, true, 10.0f, true).apply);
    return 0;
}
