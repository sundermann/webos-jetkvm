
# CLAUDE.md

## General

- For anything beyond a small, local fix, propose a plan first (what changes, which interfaces are affected, which tests you'll add) and wait for approval.
- Write or update the test first, then the code. Every bug fix gets a regression test.
- Keep each change small and focused. Don't refactor unrelated code in the same change.
- Match the existing style and patterns of the codebase before introducing new ones.
- Before reporting done, build with warnings as errors and run all tests. Say what you ran and the result.
- If hooks report format, lint, build or test failures, treat them as errors and fix them.
- If something is unclear or a requirement conflicts with these rules, ask instead of guessing.
## C++ guidelines

### Language and style
- Standards up to C++26 are allowed. Use a newer feature only if every compiler and standard library the project builds with supports it. If unsure, check the feature-test macro or a compiler support table first.
- Code compiles with zero warnings on every supported compiler.
- Initialize every variable at declaration. Prefer `auto` when the type is obvious from the right-hand side.
- `const` everything that can be; `constexpr` for compile-time values.
- `enum class` over plain enums. `std::array` / `std::vector` over C arrays.
- No C-style casts; use `static_cast` and friends, and avoid `reinterpret_cast`.
- No macros where `constexpr`, templates or inline functions work.
- No undefined behavior: no signed overflow, out-of-bounds access, dangling references or uninitialized reads.
### Modern features to prefer
- `std::format`, `std::print` and `std::println` over iostreams and `printf`.
- Standard algorithms, ranges and views (`std::views::zip`, `std::views::enumerate`, `std::ranges::to`) over hand-written index loops when they're clearer.
- `std::span` and `std::mdspan` for views of contiguous and multidimensional data.
- `std::inplace_vector` for small fixed-capacity collections that shouldn't allocate.
- Deducing `this` instead of duplicated const and non-const overloads.
- `std::to_underlying`, `std::unreachable` and `if consteval` where they fit.
- `_` for intentionally unused variables; `= delete("reason")` to explain deleted functions.
- Contracts (`pre`, `post`, `contract_assert`) for preconditions, postconditions and invariants.
- Static reflection and `std::execution` only when they clearly simplify the code and the project has already adopted them.
### Ownership and resources
- Values by default. `std::unique_ptr` for ownership; `std::shared_ptr` only for genuinely shared ownership.
- No raw `new`/`delete`, no `malloc`/`free`. Every resource (memory, files, locks, handles) is owned by an RAII type.
- Rule of zero: don't write destructors or copy/move operations unless the class directly owns a raw resource. If you write one, handle all five.
- Non-owning access through references, `std::span` and `std::string_view`. Never store a view that can outlive its data.
### Functions and types
- Cheap types by value, others by `const&`; sink parameters by value, then `std::move`.
- `[[nodiscard]]` on functions whose result must not be ignored.
- `explicit` on single-argument constructors and conversion operators.
- `override` on every overriding function; virtual destructors in polymorphic base classes.
- `noexcept` on move operations, swap and functions that genuinely cannot throw.
- Keep functions short and single-purpose. Prefer free functions and small value types over deep class hierarchies.
- No global mutable state and no singletons. Pass dependencies explicitly, e.g. through constructors.
### Errors
- Use one error-handling style consistently, matching the existing code. Don't mix exceptions and error codes.
- Default: `std::expected<T, E>` for failures the caller is expected to handle, exceptions for failures that can't be handled locally, `std::optional` for "no result".
- Violated preconditions are bugs: check them with contracts where supported, otherwise `assert`. Don't throw for them.
- Never swallow errors silently or return magic values like `-1`.
### Concurrency
- Avoid shared mutable state. When it's needed, protect it with a mutex via `std::scoped_lock` and document what the mutex guards.
- Prefer `std::jthread` and higher-level primitives over raw threads and manual synchronization.
- Never use `volatile` for synchronization; use `std::atomic`.
### Headers
- `#pragma once`. Include what you use; forward-declare where possible.
- No `using namespace` in headers. Keep implementation details out of public headers.
### Naming
- `CamelCase` types, `snake_case` functions and variables, `trailing_` private members, `kCamelCase` constants.
- Names say what something is or does; no unexplained abbreviations.
## Never

- Never silence a warning or lint finding to make it pass: no `NOLINT`, no warning-suppression pragmas, no edits to `.clang-tidy`, `.clang-format`, compiler warning flags or CI. Fix the code. If you think a check is wrong, stop and explain why.
- Never delete, skip or weaken a test to make it pass.
- Never add a third-party dependency without asking.
