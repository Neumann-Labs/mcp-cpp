# Contributing

Thanks for your interest in mcp-cpp. This project is in active development; the
quickest path to a successful contribution is the same as everywhere else:

1. **Read the spec.** This SDK is a faithful implementation of the Model
   Context Protocol revision **2025-11-25**, defined by the TypeScript
   schema in
   [modelcontextprotocol/modelcontextprotocol](https://github.com/modelcontextprotocol/modelcontextprotocol/tree/main/schema/2025-11-25).
   Wire field names, method names, and behaviors track that schema verbatim.
2. **Open an issue first** for non-trivial changes so the design can be
   discussed before code is written.
3. **Add tests.** Every public-facing change should ship with unit or
   integration tests in `tests/`. Round-trip JSON tests for new types,
   end-to-end tests for new request/response pairs, paired with an
   in-memory transport when no real network is needed.
4. **Build & test under the sanitizer matrix locally** before opening a PR:

   ```bash
   cmake -B build -G Ninja
   cmake --build build && ctest --test-dir build --output-on-failure

   cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_ENABLE_ASAN=ON
   cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

   cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_ENABLE_TSAN=ON
   cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
   ```

   CI runs all three on every PR; matching them locally avoids surprises.

## Style

- C++20 baseline. `snake_case` for functions / variables / methods,
  `PascalCase` for types, trailing-underscore for private members
  (`foo_`). JSON wire keys are the spec's camelCase exactly; the
  C++-side accessors are snake_case.
- Prefer composition over inheritance. The codebase has exactly one
  inheritance edge (the `Transport` interface) and we want to keep
  that count low.
- `[[nodiscard]]` on result-returning APIs. `noexcept` where the
  function actually cannot throw.
- No raw owning pointers. `unique_ptr` for unique ownership,
  `shared_ptr` only when ownership genuinely is shared.
- Comments explain *why*, not *what* — if the reader can derive the
  *what* from the code, leave it.
- Format with the project's `.clang-format` (TODO: add one) before
  submitting.

## Commit messages

Use conventional-style prefixes (`feat:`, `fix:`, `refactor:`, `chore:`,
`docs:`, `test:`) followed by a one-line summary, a blank line, and a
body that explains the *why* and any non-obvious *how*. Reference
issues with `Fixes #N` / `Closes #N` when applicable.
