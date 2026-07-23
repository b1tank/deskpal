#!/usr/bin/env bash
set -euo pipefail

UUID=indicator@deskpal.local
SERVICE=org.deskpal.Indicator
OBJECT=/org/deskpal/Indicator
INTERFACE=org.deskpal.Indicator
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SOURCE="$ROOT/gnome-extension/$UUID"
TARGET="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$UUID"

usage() {
    cat <<'EOF'
Usage: scripts/indicator.sh COMMAND [ARGS]

Commands:
  install                         Install and enable the development extension
  enable                          Enable an installed extension
  uninstall                       Disable and remove the extension
  ping                            Verify the local D-Bus service
  show ID X Y COLOR [LABEL]       Show a logical cursor
  move ID X Y                     Move a logical cursor
  hide ID                         Remove one logical cursor
  clear                           Remove manual cursors (never another process's owned cursors)
  list                            Print logical cursor state
  demo                            Run a two-cursor visual demonstration
EOF
}

call() {
    gdbus call --session --dest "$SERVICE" --object-path "$OBJECT" \
        --method "$INTERFACE.$1" "${@:2}"
}

wait_for_service() {
    local attempts=30
    while (( attempts > 0 )); do
        if call Ping >/dev/null 2>&1; then
            return 0
        fi
        attempts=$((attempts - 1))
        sleep 0.2
    done
    echo "Deskpal indicator did not appear on D-Bus." >&2
    echo "Check: journalctl --user -f -o cat /usr/bin/gnome-shell" >&2
    return 1
}

install_extension() {
    command -v gnome-extensions >/dev/null
    command -v gdbus >/dev/null
    local package_dir package
    package_dir=$(mktemp -d)
    gnome-extensions pack --force --out-dir "$package_dir" "$SOURCE" >/dev/null
    package="$package_dir/$UUID.shell-extension.zip"
    gnome-extensions install --force "$package"
    rm -rf "$package_dir"
    if ! gnome-extensions list | grep -Fxq "$UUID"; then
        echo "Installed $UUID. GNOME Shell has not loaded the new extension yet."
        echo "On GNOME Wayland 42, log out and back in, then run '$0 enable'."
        return 0
    fi
    gnome-extensions enable "$UUID"
    wait_for_service
    echo "Installed and enabled $UUID"
}

case "${1:-}" in
    install)
        install_extension
        ;;
    enable)
        gnome-extensions enable "$UUID"
        wait_for_service
        echo "Enabled $UUID"
        ;;
    uninstall)
        call ClearAll >/dev/null 2>&1 || true
        gnome-extensions disable "$UUID" >/dev/null 2>&1 || true
        rm -rf "$TARGET"
        echo "Removed $UUID"
        ;;
    ping)
        call Ping
        ;;
    show)
        [[ $# -ge 5 ]] || { usage >&2; exit 2; }
        call ShowCursor "$2" "$3" "$4" "$5" "${6:-$2}"
        ;;
    move)
        [[ $# -eq 4 ]] || { usage >&2; exit 2; }
        call MoveCursor "$2" "$3" "$4"
        ;;
    hide)
        [[ $# -eq 2 ]] || { usage >&2; exit 2; }
        call HideCursor "$2"
        ;;
    clear)
        call ClearAll
        ;;
    list)
        call ListCursors
        ;;
    demo)
        wait_for_service
        call ClearAll >/dev/null
        call ShowCursor agent-1 80 100 '#36C5F0' 'agent-1' >/dev/null
        call ShowCursor agent-2 80 180 '#FF9F1C' 'agent-2' >/dev/null
        sleep 0.6
        call MoveCursor agent-1 420 180 >/dev/null
        sleep 0.35
        call MoveCursor agent-2 360 300 >/dev/null
        sleep 0.35
        call MoveCursor agent-1 560 340 >/dev/null
        sleep 0.35
        call MoveCursor agent-2 470 430 >/dev/null
        echo "Two logical cursors are visible. Your physical pointer was not moved."
        echo "Run '$0 clear' when finished."
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
