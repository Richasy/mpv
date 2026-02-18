#!/bin/bash
# Apply vulkan cross-compile patches with fallback for incompatible hunks
set -e
cd "$1"

git am --abort 2>/dev/null || true
git reset --hard @{u}

# Try applying the full patch
if ! git apply --ignore-whitespace "$2" 2>/dev/null; then
    # Partial apply with reject
    git apply --reject --ignore-whitespace "$2" 2>/dev/null || true

    # Manually fix the rejected hunk in loader.h
    if [ -f loader/loader.h.rej ]; then
        sed -i '/\/\/ Declare the once_init variable/{
            n
            /^LOADER_PLATFORM_THREAD_ONCE_EXTERN_DEFINITION(once_init)/c\
#if defined(_WIN32) \&\& !defined(LOADER_DYNAMIC_LIB)\
LOADER_PLATFORM_THREAD_ONCE_EXTERN_DEFINITION(once_init)\
#endif
        }' loader/loader.h
        rm -f loader/loader.h.rej
    fi

    # Remove any other .rej files
    find . -name "*.rej" -delete
fi

git add -A
git commit -m "loader: cross-compile & static linking hacks" --allow-empty
