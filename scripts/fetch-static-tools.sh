#!/bin/bash
# fetch-static-tools.sh - Download static musl binaries for ChocoOS
# Run from repo root: ./scripts/fetch-static-tools.sh

set -e

TOOLS_DIR="$(dirname "$0")/../build/user"
mkdir -p "$TOOLS_DIR"

echo "=== Fetching static musl binaries for ChocoOS ==="
cd "$TOOLS_DIR"

# Function to download and extract
fetch() {
    local name="$1"
    local url="$2"
    local binary_path="$3"
    local dest="$4"

    echo "Fetching $name..."
    if curl -L -f -s "$url" -o "/tmp/${name}.tar.gz"; then
        tar -xzf "/tmp/${name}.tar.gz" -C /tmp
        if [ -f "/tmp/${binary_path}" ]; then
            cp "/tmp/${binary_path}" "${dest}"
            chmod +x "${dest}"
            echo "  ✓ ${dest} installed"
        else
            echo "  ✗ Binary not found at /tmp/${binary_path}"
            return 1
        fi
    else
        echo "  ✗ Failed to download $name"
        return 1
    fi
}

# 1. micro - Modern terminal editor (~3MB static)
# From: https://github.com/zyedidia/micro/releases
fetch "micro" \
    "https://github.com/zyedidia/micro/releases/download/v2.0.12/micro-2.0.12-static-linux64.tar.gz" \
    "micro-2.0.12/micro" \
    "micro"

# 2. lua 5.4 - Lightweight scripting (~200KB static)
# From musl.cc
fetch "lua" \
    "https://musl.cc/lua-5.4.6.tar.gz" \
    "lua-5.4.6/bin/lua" \
    "lua"

# 3. micropython - Python 3 implementation (~1MB static)
# From micropython.org static builds
fetch "micropython" \
    "https://micropython.org/resources/firmware/micropython-v1.23.0-linux-x86_64.tar.gz" \
    "micropython" \
    "micropython"

# Also try to get luac (Lua compiler) if available
if [ -f "/tmp/lua-5.4.6/bin/luac" ]; then
    cp "/tmp/lua-5.4.6/bin/luac" "luac"
    chmod +x luac
    echo "  ✓ luac installed"
fi

echo ""
echo "=== Installed tools ==="
ls -lh micro lua micropython 2>/dev/null || true

# Verify they're static
echo ""
echo "=== Verification ==="
for tool in micro lua micropython; do
    if [ -f "$tool" ]; then
        if file "$tool" | grep -q "statically linked"; then
            echo "  ✓ $tool: static"
        else
            echo "  ⚠ $tool: NOT static"
            file "$tool"
        fi
    fi
done