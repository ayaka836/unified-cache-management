---
name: ucm-rust-quality
description: Use when Codex works on UCM Rust refactoring quality standards, Rust formatting and lint gates, Rust coding guideline review, API design, crate and module structure, error handling, unsafe code, C/C++ FFI boundaries, concurrency, dependencies, tests, or architecture robustness for Rust changes in unified-cache-management.
---

# UCM Rust Quality

Use this skill to review, maintain, or apply Rust quality standards for the UCM
Rust refactoring work.

## Core Workflow

1. Locate the UCM repository root. Prefer the current working directory when it
   is already inside `unified-cache-management`.
2. Read `references/rust-quality-standard.md` when the user asks about coding
   standards, review criteria, unsafe/FFI rules, API design, or architectural
   robustness.
3. For formatting tasks, use the repository script:

```bash
bash rust-format.sh --check
bash rust-format.sh --fix
```

4. For broader Rust quality checks, use `scripts/run_ucm_rust_quality.sh` from
   this skill when the repository has Rust tooling available.
5. Report findings in review style: issues first, ordered by severity, with file
   and line references when possible. Include passed checks and skipped checks
   briefly at the end.

## Review Focus

- Formatting must follow `rustfmt.toml` and `rust-format.sh`.
- Public APIs should follow the Rust API Guidelines.
- Crate boundaries should match UCM product ownership, not simply mirror legacy
  C++ directories.
- Raw FFI bindings, safe wrappers, and business logic should stay in separate
  layers.
- Recoverable failures should use `Result<T, E>` and preserve useful context.
- `unwrap`, `expect`, and `panic!` should be absent from production paths unless
  justified by an invariant violation.
- Unsafe code must be isolated and documented with `// SAFETY:` comments or
  `# Safety` docs.
- Rust panics must not cross C/C++ FFI boundaries.
- Shared cache state should document consistency, eviction, shutdown,
  cancellation, ordering, and locking behavior.
- Hot-path changes should avoid unnecessary allocation and cloning and should be
  backed by benchmarks when performance is part of the claim.
- New dependencies should be maintained, license-compatible, production-suitable,
  and justified.

## Bundled Resources

- `references/rust-quality-standard.md`: UCM Rust quality standard integrating
  Rust Style Guide, Rust API Guidelines, Microsoft Pragmatic Rust Guidelines,
  ANSSI Secure Rust Guidelines, and Rust for Linux Coding Guidelines.
- `scripts/run_ucm_rust_quality.sh`: Runs repository Rust quality checks,
  including formatting and Cargo checks when a Cargo manifest exists.

## Output Style

When reviewing code, lead with concrete issues. Use concise explanations and
avoid restating the full standard unless the user asks for it. When no issue is
found, say so clearly and mention any checks that could not be run.
