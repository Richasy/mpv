#!/bin/bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "Expected Vulkan source, patch and pinned commit." >&2
    exit 2
fi
cd "$1"
git rev-parse --verify "$3^{commit}" >/dev/null
git reset --hard "$3"
git apply --check --ignore-whitespace "$2"
git apply --ignore-whitespace "$2"

git add -- loader/CMakeLists.txt loader/loader.c loader/loader.h loader/loader.rc.in \
    loader/loader_windows.c loader/vk_loader_platform.h loader/vulkan_own.pc.in
git commit -m "loader: cross-compile & static linking hacks"
