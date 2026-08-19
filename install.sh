#!/bin/sh
# FlaSSH installer. Downloads the latest release binary for this machine's
# architecture and installs it as `fssh`.
#
#   curl -fsSL https://raw.githubusercontent.com/z3r0n3br4instorm/FlaSSH/main/install.sh | sh
#
# Override the destination with INSTALL_DIR=~/.local/bin (no sudo needed).
set -eu

REPO="z3r0n3br4instorm/FlaSSH"
BIN_NAME="fssh"
INSTALL_DIR="${INSTALL_DIR:-/usr/local/bin}"

case "$(uname -s)" in
    Linux)  OS="linux" ;;
    Darwin) OS="macos" ;;
    *)
        echo "No prebuilt binary for $(uname -s)." >&2
        echo "Build from source instead: https://github.com/$REPO" >&2
        exit 1
        ;;
esac

case "$(uname -m)" in
    x86_64 | amd64)  ARCH="x86_64" ;;
    aarch64 | arm64) ARCH="arm64" ;;
    *)
        echo "No prebuilt binary for $(uname -m). Build from source instead:" >&2
        echo "  https://github.com/$REPO" >&2
        exit 1
        ;;
esac

ASSET="flassh-${OS}-${ARCH}"

command -v curl >/dev/null 2>&1 || { echo "curl is required." >&2; exit 1; }

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT INT TERM

echo "Downloading $ASSET (latest release)..."
curl -fsSL --proto '=https' --tlsv1.2 \
    "https://github.com/$REPO/releases/latest/download/$ASSET" -o "$TMP"
chmod +x "$TMP"

SUDO=""
if [ ! -w "$INSTALL_DIR" ]; then
    if command -v sudo >/dev/null 2>&1; then
        echo "$INSTALL_DIR needs elevated permissions; using sudo."
        SUDO="sudo"
    else
        echo "$INSTALL_DIR is not writable and sudo is unavailable." >&2
        echo "Retry with a writable location, e.g.:" >&2
        echo "  INSTALL_DIR=\$HOME/.local/bin sh install.sh" >&2
        exit 1
    fi
fi

$SUDO mkdir -p "$INSTALL_DIR"
$SUDO install -m 755 "$TMP" "$INSTALL_DIR/$BIN_NAME"
echo "Installed $INSTALL_DIR/$BIN_NAME"

# The binary links against libssh at runtime, so flag a missing copy rather
# than letting the user hit a loader error on first run.
if command -v ldd >/dev/null 2>&1; then
    if ldd "$INSTALL_DIR/$BIN_NAME" 2>/dev/null | grep -q "not found"; then
        echo >&2
        echo "Warning: a required shared library is missing (likely libssh):" >&2
        ldd "$INSTALL_DIR/$BIN_NAME" 2>/dev/null | grep "not found" >&2
        echo >&2
        echo "Install it with your package manager:" >&2
        echo "  Debian/Ubuntu: sudo apt-get install libssh-4" >&2
        echo "  Fedora:        sudo dnf install libssh" >&2
        echo "  Arch:          sudo pacman -S libssh" >&2
        echo "  macOS:         brew install libssh" >&2
    fi
fi

case ":${PATH}:" in
    *":$INSTALL_DIR:"*) ;;
    *) echo "Note: $INSTALL_DIR is not in your PATH." >&2 ;;
esac

echo "Run: $BIN_NAME [-i identity_file] <username>@<host>"
