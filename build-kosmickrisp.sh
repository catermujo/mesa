#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build="$root/build-kosmickrisp"
tools="$build/tools"
runtime="$build/runtime"
venv="$build/venv"
jobs=${JOBS:-8}
llvm_config=${LLVM_CONFIG:-llvm-config}

if [ ! -x "$venv/bin/python" ]; then
    uv venv --python python3 "$venv"
fi
uv pip install --python "$venv/bin/python" -r "$root/requirements-kosmickrisp.txt"
export PATH="$venv/bin:$PATH"
export MACOSX_DEPLOYMENT_TARGET=26.0
export PKG_CONFIG_PATH="$tools/lib/pkgconfig:${VULKAN_SDK:+$VULKAN_SDK/lib/pkgconfig:}${PKG_CONFIG_PATH:-}"

cmake -S "$root/subprojects/spirv-llvm-translator" -B "$build/translator" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR="$("$llvm_config" --cmakedir)" \
    -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR="$root/subprojects/spirv-headers" \
    -DLLVM_SPIRV_INCLUDE_TESTS=OFF \
    -DLLVM_SPIRV_ENABLE_LIBSPIRV_DIS=OFF \
    -DCMAKE_INSTALL_PREFIX="$tools"
cmake --build "$build/translator" --target LLVMSPIRVLib --parallel "$jobs"
cmake --install "$build/translator" --component LLVMSPIRVLib
cmake --install "$build/translator" --component Unspecified

cmake -S "$root/subprojects/vulkan-headers" -B "$build/headers" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$tools" -DVULKAN_HEADERS_ENABLE_TESTS=OFF
cmake --install "$build/headers"

cmake -S "$root/subprojects/vulkan-loader" -B "$build/loader" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -DCMAKE_PREFIX_PATH="$tools" -DCMAKE_INSTALL_PREFIX="$runtime" \
    -DBUILD_TESTS=OFF -DUPDATE_DEPS=OFF
cmake --build "$build/loader" --target install --parallel "$jobs"

set --
if [ -f "$build/mesa/meson-private/coredata.dat" ]; then
    set -- --reconfigure
fi
meson setup "$@" "$build/mesa" "$root" \
    --buildtype=release --prefix="$runtime" --prefer-static --wrap-mode=nofallback \
    -Dplatforms=macos -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= \
    -Dopengl=false -Dzstd=disabled -Dbuild-tests=false \
    -Dc_args=-mmacosx-version-min=26.0 \
    -Dcpp_args=-mmacosx-version-min=26.0 \
    -Dobjc_args=-mmacosx-version-min=26.0
meson compile -C "$build/mesa" -j "$jobs"
meson install -C "$build/mesa" --tags runtime

"$venv/bin/python" - "$runtime" <<'PY'
import json
import sys
from pathlib import Path

runtime = Path(sys.argv[1])
manifest = next((runtime / "share/vulkan/icd.d").glob("kosmickrisp_mesa_icd.*.json"))
data = json.loads(manifest.read_text())
data["ICD"]["library_path"] = "./libvulkan_kosmickrisp.dylib"
(runtime / "lib/kosmickrisp_icd.json").write_text(json.dumps(data, indent=2) + "\n")
PY
