# bbox

A lightweight, non-compositing X11 window manager optimized for NVIDIA and high-performance setups.

## Features

*   **XCB-based**: Built on top of the X C Binding library for maximum efficiency and async event handling.
*   **Event-driven**: Uses `epoll` with event coalescing to handle high-frequency inputs without jitter.
*   **Container Management**: Supports window organization via containers.
*   **Advanced Multi-Monitor**: Strong RandR 1.5 support for correct handling of mixed-refresh rate setups.
*   **Signal Handling**: Graceful shutdown and runtime diagnostics via signals (SIGUSR1).
*   **Logging**: Integrated logging system.

## Installation / Usage

**Prerequisites:**

*   Meson Build System
*   Ninja
*   XCB libraries (`libxcb`, `xcb-xkb`, `xcb-randr`, etc.)
*   C11 compiler (GCC/Clang)

**Building:**

```bash
meson setup build
meson compile -C build
```

**Running:**

```bash
./build/bbox
```
