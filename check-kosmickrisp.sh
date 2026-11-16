#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runtime="$root/build-kosmickrisp/runtime/lib"
if [ "$#" -ne 0 ] && [ "$#" -ne 1 ] && [ "$#" -ne 3 ]; then
    echo "usage: sh check-kosmickrisp.sh [contact.spirv [vertex.spirv fragment.spirv]]" >&2
    exit 2
fi
export VK_DRIVER_FILES=${VK_DRIVER_FILES:-$runtime/kosmickrisp_icd.json}
mkdir -p "$root/build-kosmickrisp/tests"
cc -std=c11 -O2 -Wall -Wextra -Werror \
    -I"$root/subprojects/vulkan-headers/include" \
    "$root/src/kosmickrisp/tests/sync_poll.c" \
    -L"$runtime" -lvulkan -Wl,-rpath,"$runtime" \
    -o "$root/build-kosmickrisp/tests/sync_poll"
"$root/build-kosmickrisp/tests/sync_poll"

# Exercise the Cocoa presentation path as well as offscreen queue completion.
cc -std=c11 -O2 -Wall -Wextra -Werror \
    -I"$root/subprojects/vulkan-headers/include" \
    "$root/src/kosmickrisp/tests/drawable_acquire.m" \
    -L"$runtime" -lvulkan -Wl,-rpath,"$runtime" \
    -framework Cocoa -framework QuartzCore -framework Metal \
    -o "$root/build-kosmickrisp/tests/drawable_acquire"
"$root/build-kosmickrisp/tests/drawable_acquire"

if [ "$#" -ge 1 ]; then
    cc -std=c11 -O2 -Wall -Wextra -Werror \
        -I"$root/subprojects/vulkan-headers/include" \
        "$root/src/kosmickrisp/tests/vizza_contact.c" \
        -L"$runtime" -lvulkan -Wl,-rpath,"$runtime" \
        -o "$root/build-kosmickrisp/tests/vizza_contact"
    "$root/build-kosmickrisp/tests/vizza_contact" "$1" 0
    "$root/build-kosmickrisp/tests/vizza_contact" "$1" 1
fi

if [ "$#" -eq 3 ]; then
    cc -std=c11 -O2 -Wall -Wextra -Werror \
        -I"$root/subprojects/vulkan-headers/include" \
        "$root/src/kosmickrisp/tests/timestamp_graphics.c" \
        -L"$runtime" -lvulkan -Wl,-rpath,"$runtime" \
        -o "$root/build-kosmickrisp/tests/timestamp_graphics"
    "$root/build-kosmickrisp/tests/timestamp_graphics" "$2" "$3"
fi
