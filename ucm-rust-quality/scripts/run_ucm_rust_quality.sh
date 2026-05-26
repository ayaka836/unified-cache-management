#!/usr/bin/env bash

set -euo pipefail

repo_root() {
    local root
    root="$(git rev-parse --show-toplevel 2> /dev/null || pwd)"
    cd "$root" && pwd
}

has_cargo_manifest() {
    find "$1" \
        \( -path '*/.git' -o -path '*/target' -o -path '*/build' -o -path '*/dist' \) -prune \
        -o -type f -name 'Cargo.toml' -print -quit | grep -q .
}

main() {
    local root
    root="$(repo_root)"
    cd "$root"

    if [[ -x rust-format.sh || -f rust-format.sh ]]; then
        bash rust-format.sh --check
    else
        echo "rust-format.sh not found; skipping repository Rust format check."
    fi

    if ! has_cargo_manifest "$root"; then
        echo "No Cargo.toml found; skipping cargo check/test/clippy."
        return
    fi

    cargo check --all-targets --all-features
    cargo test --all-targets --all-features
    cargo clippy --all-targets --all-features -- -D warnings
}

main "$@"
