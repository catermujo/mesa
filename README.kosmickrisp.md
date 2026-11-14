# KosmicKrisp for macOS

This experimental fork builds KosmicKrisp and the Vulkan loader for Apple Silicon and macOS 26 or later.
It does not change an application's driver selection.
Git submodules pin the loader, Vulkan headers, SPIR-V headers, and LLVM 22 translator.

## Requirements

Install Xcode, CMake, Ninja, pkg-config, uv, LLVM 22, and the macOS Vulkan SDK.
The build uses the SDK's static SPIRV-Tools libraries through pkg-config.
Set `VULKAN_SDK` to the SDK's `macOS` directory if its pkg-config files are absent from the system paths.
Set `LLVM_CONFIG` to an LLVM 22 `llvm-config` executable if it is absent from `PATH`.

## Build

Initialize the source dependencies:

```sh
git submodule update --init --recursive
```

Build the runtime:

```sh
sh build-kosmickrisp.sh
```

The build creates a persistent Python environment under `build-kosmickrisp/venv`.
It does not install the runtime into system directories.
The runtime files are in `build-kosmickrisp/runtime/lib`:

- `libvulkan.1.dylib`
- `libvulkan_kosmickrisp.dylib`
- `kosmickrisp_icd.json`

The ICD uses a relative library path. Keep these three files together when you copy the runtime.
The runtime includes shared Mesa compiler and Vulkan code, without other Mesa drivers or OpenGL.
LLVM and the translator are build dependencies. The driver does not load them at runtime.

## Local changes

Zero-timeout timeline waits read the shared-event value directly.
Fence status checks avoid a kernel wait and retain the same completion test.
Other waits retain their existing timeout handling.
Metal can use its full CPU compilation budget for independent pipeline builds.

Timestamp queries request precise Metal samples.
Render queries end the current encoder so each query gets a distinct sample.
The renderer preserves attachments and restores graphics state for subsequent draws.
These encoder boundaries add overhead when an application records timestamps.
`MESA_KK_DEBUG=msl` also prints shaders restored from a pipeline cache.

`KK_WORKAROUND_20` changes address arithmetic to avoid a Metal compiler failure.
See `docs/drivers/kosmickrisp/workarounds.rst` for the trigger and tested platform.

## Check

Run the native Vulkan checks after the build:

```sh
sh check-kosmickrisp.sh
```

The checks exercise fence polling, reset, bounded waits, timeline signaling, GPU writes, and distinct timestamps across compute regions.
Set `VK_DRIVER_FILES` to another ICD to compare the same executable with another driver.

Supply VIZZA's compiled `raw_grain_contact.spirv` to compare all 8,192 output forces with a CPU reference:

```sh
sh check-kosmickrisp.sh /path/to/raw_grain_contact.spirv
```

The check runs with buffer robustness off and on.
The input contains zero velocities and equal masses on a fixed grid.
It isolates contact forces from grid construction and frame scheduling.

Compile `src/kosmickrisp/tests/timestamp.slang` with the application's shader workflow.
Use its `vertex_main` and `fragment_main` entries for the vertex and fragment SPIR-V modules.
Supply those modules after the contact shader to check graphics timestamps:

```sh
sh check-kosmickrisp.sh contact.spirv vertex.spirv fragment.spirv
```

That check records repeated timestamps at the same stage within one render pass.
It also checks the rendered pixel after the driver splits the Metal encoder.
