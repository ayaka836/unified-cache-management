# UCM Rust Quality Standard

UCM adopts widely used Rust community guidelines first, then adds
project-specific rules for cache management, C/C++ interoperability, and
performance-sensitive systems code.

## Adopted Standards

Follow these sources in order:

1. Rust Style Guide: baseline Rust source style, enforced through
   `rustfmt.toml` and `rust-format.sh`.
2. Rust API Guidelines: public API design, naming, trait behavior, conversions,
   documentation, and error types.
3. Microsoft Pragmatic Rust Guidelines: production Rust engineering, panic
   usage, lint exceptions, dependency choices, maintainability, and unsafe
   boundaries.
4. ANSSI Secure Rust Guidelines: security, unsafe code, FFI, dependency hygiene,
   memory safety assumptions, and supply-chain review.
5. Rust for Linux Coding Guidelines: low-level resource ownership, C
   interoperability, and review discipline.

When standards conflict, apply the more specific rule for the code under review.
Use stricter safety and security guidance for unsafe and FFI code.

## Mandatory Tooling

- Use stable Rust unless nightly is explicitly justified.
- Keep required Rust components in repository `rust-toolchain.toml`.
- Use `bash rust-format.sh --fix` to format code and
  `bash rust-format.sh --check` for CI-style checks.
- Once a Cargo workspace exists, include `cargo check`, `cargo test`, and
  `cargo clippy` in quality gates.
- Scope `allow` attributes narrowly and explain non-obvious lint exceptions.

## Formatting And Style

- Follow Rust Style Guide and repository `rustfmt.toml`.
- Do not hand-format code in a way that fights `rustfmt`.
- Keep imports ordered by `rustfmt`.
- Prefer readable code over clever code. Do not hide important domain types when
  explicit annotations improve review.

## API Design

- Public APIs must follow Rust API Guidelines.
- Expose only intentional public surface. Prefer `pub(crate)` for internal
  cross-module visibility.
- Use domain-specific types when values carry invariants such as capacity,
  token count, block size, device id, cache key, or backend type.
- Keep constructors explicit when validation is required.
- Avoid public fields on types that must preserve invariants.
- Implement standard traits when they match type semantics, such as `Debug`,
  `Default`, `Clone`, `Copy`, `From`, `TryFrom`, `AsRef`, or `Deref`.
- Avoid surprising trait behavior.

## Crate And Module Structure

- Crate boundaries should follow UCM product ownership boundaries rather than
  mirror every legacy C++ directory.
- Keep modules purpose-driven and small enough for review.
- Avoid catch-all `utils`, `common`, or `types` modules unless they have a clear
  owner and narrow responsibility.
- Put shared domain types close to the logic that owns their invariants.
- Keep raw FFI bindings, safe wrappers, and business logic in separate layers.
- Do not let examples, benchmarks, or tests become hidden dependencies of
  production crates.

## Error Handling

- Use `Result<T, E>` for recoverable failures.
- Do not encode errors as magic values, nullable pointers, or ambiguous
  booleans.
- Preserve context useful for operators and developers, especially backend type,
  device id, path, cache key, request id, or external API failure code.
- Avoid `unwrap()` and `expect()` in production code. They are acceptable in
  tests when failure means the test setup is invalid.
- Use `?` when the current layer cannot add meaningful recovery behavior.
- Panics are for bugs and violated internal invariants, not normal runtime
  failures.
- Panics must not unwind across FFI boundaries.

## Unsafe Code

- Safe Rust is the default. Any `unsafe` must be necessary, isolated, and easy
  to audit.
- Every `unsafe` block or function must have a `// SAFETY:` comment explaining
  the invariants that make it sound.
- Unsafe functions must include a `# Safety` doc section.
- Prefer small safe abstractions around raw pointers, FFI handles, GPU buffers,
  shared memory, and manually managed allocations.
- Validate ownership, lifetime, alignment, aliasing, initialization, and
  thread-safety assumptions before exposing a safe wrapper.
- Do not use global mutable state unless it is protected by synchronization and
  has a documented lifecycle.
- Cover unsafe code with tests that exercise edge cases and failure paths where
  practical.

## FFI And C/C++ Interoperability

- Keep raw C/C++ ABI definitions in dedicated FFI modules.
- Use `#[repr(C)]` for data structures crossing a C ABI boundary.
- Expose safe Rust wrappers above raw FFI calls.
- Safe wrappers must validate pointers, lengths, ownership transfer, callback
  lifetime, and thread-safety assumptions.
- Do not let Rust panics cross into C/C++ code. Convert failures into explicit
  status values or error objects at the boundary.
- Document ownership rules for buffers, handles, callbacks, and objects shared
  between Rust and C/C++.
- Keep generated bindings reproducible. Document the command, input headers,
  tool version, and review policy.

## Concurrency And Async

- Make ownership explicit for data shared across threads.
- Use `Arc` only when shared ownership is required.
- Prefer message passing, scoped ownership, or narrow synchronization over broad
  shared mutable state.
- Keep lock scopes small. Do not call user callbacks or external blocking APIs
  while holding locks.
- Document shutdown, cancellation, timeout, and ordering behavior for background
  workers.
- Avoid blocking work in async contexts unless it is moved to a dedicated
  blocking executor or thread.
- Shared caches must document consistency expectations, eviction behavior, and
  whether operations are best-effort or strongly ordered.

## Performance-Sensitive Code

- Correctness and clear invariants come first.
- Avoid unnecessary allocation and cloning in hot paths. Use borrowing,
  preallocation, or pooling when profiling shows it matters.
- Use benchmarks for performance-sensitive changes. Include workload shape and
  relevant hardware assumptions in the pull request.
- Do not introduce unsafe optimizations without explaining the measured benefit
  and safety invariants.
- Keep CPU, GPU, and storage backends behind clear interfaces so backend-specific
  optimizations do not leak into generic cache logic.

## Dependencies

- Keep dependencies minimal and justified.
- New dependencies must be actively maintained, license-compatible, and suitable
  for production use.
- Prefer standard library or existing workspace dependencies when they are clear
  and sufficient.
- Avoid adding a dependency for small helpers that can be implemented clearly in
  local code.
- Dependency additions should include a short rationale in the pull request,
  especially for runtime, async, crypto, serialization, FFI, or unsafe-heavy
  crates.

## Testing

- Add unit tests for pure logic and invariant-heavy code.
- Add integration tests for crate-level behavior, public APIs, and cross-module
  contracts.
- Add regression tests for bug fixes.
- Keep tests deterministic.
- Avoid timing-sensitive assertions unless the test explicitly covers scheduling
  or timeout behavior.
- FFI wrappers should include tests for null pointers, invalid lengths,
  ownership transfer, and error propagation where practical.
- Performance changes should include benchmarks or a clear reason why benchmark
  coverage is not practical yet.

## Review Checklist

- `bash rust-format.sh --check` passes.
- New public APIs follow Rust API Guidelines.
- Clippy findings are fixed or narrowly justified.
- Error paths preserve useful context.
- `unwrap()`, `expect()`, and `panic!` are absent from production paths unless
  intentionally justified.
- Unsafe code is isolated and documented.
- FFI boundaries define ownership, lifetimes, layout, and panic behavior.
- Tests cover new behavior and important failure paths.
- New dependencies are justified.
