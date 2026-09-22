# Khronos API headers for offline / TI-PowerVR builds

These are the public Khronos EGL and OpenGL ES 2 headers, vendored so the
GamePup apps can compile without Debian's Mesa `libegl-dev` / `libgles-dev`
packages. Those Mesa -dev packages pull in `libegl1`, which conflicts with
TI's `libegl-mesa0-pvr` from `ti-img-rogue-umlibs-am62`.

Sources (upstream SPDX Apache-2.0):

- https://github.com/KhronosGroup/EGL-Registry
- https://github.com/KhronosGroup/OpenGL-Registry

At link/runtime, use whichever `libEGL` / `libGLESv2` the board provides
(TI PVR on PocketBeagle 2 Debian IoT after `install-gpu.sh`, or Mesa when
GPU support is not installed).
