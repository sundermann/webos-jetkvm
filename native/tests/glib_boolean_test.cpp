#include <glib.h>

// Compile against the actual SDK overlay under the same strict diagnostics as
// the service. Nested expressions must retain single evaluation and semantics.
constexpr bool boolean_literals() {
    return G_LIKELY(true) == 1 && G_UNLIKELY(false) == 0;
}
static_assert(boolean_literals());
constexpr bool boolean_expression_once() {
    int count = 0;
    const bool result = G_LIKELY(G_UNLIKELY(++count == 1));
    return result && count == 1;
}
static_assert(boolean_expression_once());
