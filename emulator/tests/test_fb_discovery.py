#!/usr/bin/env python3
# Framebuffer discovery: pick the GamePup-size /dev/fbN, never the small second panel.
import importlib.machinery, importlib.util, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load(name):
    loader = importlib.machinery.SourceFileLoader(name, str(ROOT / name))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def fake_sysfs(sizes):
    root = Path(tempfile.mkdtemp())
    for index, size in enumerate(sizes):
        node = root / f"fb{index}"
        node.mkdir()
        (node / "virtual_size").write_text(f"{size[0]},{size[1]}\n")
    return root


failures = 0
cases = [
    ([(480, 272)], "/dev/fb0"),
    ([(240, 135), (480, 272)], "/dev/fb1"),            # small panel registered first
    ([(480, 272), (240, 135)], "/dev/fb0"),
    ([(320, 240)], "/dev/fb0"),
    ([(240, 320), (240, 135)], "/dev/fb0"),             # portrait ILI9341
    ([(240, 135)], "/dev/fb0"),                         # nothing supported: fall back
    ([(240, 135)] * 3 + [(480, 272)], "/dev/fb3"),
]
for name in ("gamepup-menu", "gamepup-hardware-test"):
    module = load(name)
    for sizes, expected in cases:
        module.GRAPHICS_SYSFS = fake_sysfs(sizes)
        module._framebuffer_path = None
        got = module.framebuffer_path()
        if got != expected:
            print(f"FAIL {name} {sizes}: got {got}, expected {expected}")
            failures += 1
print("framebuffer discovery: all passed" if not failures else f"{failures} FAILED")
sys.exit(1 if failures else 0)
