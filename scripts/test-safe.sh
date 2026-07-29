#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

run_suites() {
    cd "$ROOT"
    python3 test/indicator_contract.py
    python3 test/shell_bridge.py
    python3 test/broker_contract.py
    python3 test/frame_state.py
    python3 test/e2e_isolation.py
    python3 test/e2e_computer_use.py
    python3 test/e2e_accessibility.py
}

if [[ ${DESKPAL_TEST_LOCK_NAMESPACE_ACTIVE:-} == 1 ]]; then
    run_suites
    exit 0
fi

command -v bwrap >/dev/null || {
    echo "FAIL: safe tests require bubblewrap for a private control-lock namespace" >&2
    exit 1
}

private_runtime=$(mktemp -d -t deskpal-test-runtime-XXXXXX)
cleanup() {
    rm -rf "$private_runtime"
}
trap cleanup EXIT
chmod 700 "$private_runtime"
mkdir -m 700 "$private_runtime/cache"

# Preserve production's canonical lock path and inode validation while hiding
# the host session's /run/user/<uid> in a private mount namespace. Test Deskpal
# processes still arbitrate one shared lock with each other. The checkout and
# /tmp remain writable; the rest of the host filesystem is read-only.
bwrap --die-with-parent \
    --ro-bind / / \
    --bind "$ROOT" "$ROOT" \
    --bind /tmp /tmp \
    --bind "$private_runtime" "/run/user/$(id -u)" \
    --dev-bind /dev /dev \
    --proc /proc \
    --setenv DESKPAL_TEST_LOCK_NAMESPACE_ACTIVE 1 \
    --setenv XDG_CACHE_HOME "$private_runtime/cache" \
    --setenv GIO_USE_VFS local \
    "$0"
