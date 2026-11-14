#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runtime="$root/build-kosmickrisp/runtime/lib"
mkdir -p "$root/build-kosmickrisp/tests"
cc -std=c11 -O2 -Wall -Wextra -Werror \
    -I"$root/subprojects/vulkan-headers/include" \
    "$root/src/kosmickrisp/tests/sync_poll.c" \
    -L"$runtime" -lvulkan -Wl,-rpath,"$runtime" \
    -o "$root/build-kosmickrisp/tests/sync_poll"
VK_DRIVER_FILES=${VK_DRIVER_FILES:-$runtime/kosmickrisp_icd.json} \
    "$root/build-kosmickrisp/tests/sync_poll"
