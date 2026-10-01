# Render the menu / hardware-test screens at 320x240 and 480x272 to PNGs (no hardware).
# Usage: python3 emulator/tests/render_ui.py OUTDIR      (needs Pillow)
import importlib.machinery, importlib.util, sys
from pathlib import Path
from PIL import Image
ROOT = Path(__file__).resolve().parent.parent
OUT = Path(sys.argv[1]); OUT.mkdir(parents=True, exist_ok=True)
def load(path, name):
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(name, loader)
    m = importlib.util.module_from_spec(spec); loader.exec_module(m); return m
def shot(m, label, size):
    cap = {}
    def present(fb, canvas):
        cap['c'] = bytes(memoryview(canvas).cast('B'))
        cap['n'] = len(canvas)
    m.present_canvas = present
    w, h = size
    m._fb_info = {"width": w, "height": h, "stride": w*4, "bpp": 32}
    m.WIDTH, m.HEIGHT = w, h
    return cap
games = [Path(f"/opt/gamepup/games/nes/Game {i:02d}.nes") for i in range(30)]
m = load(str(ROOT / "gamepup-menu"), "menu")
for size in ((320,240),(480,272)):
    cap = shot(m, "menu", size)
    for screen in ("home","settings","second","about","nes"):
        m.draw_menu(0, games, screen, 2)
        w,h = size
        im = Image.frombuffer("RGBA",(w,h),cap['c'],"raw","BGRA",0,1)
        im.convert("RGB").save(OUT/f"menu_{screen}_{w}x{h}.png")
    m.draw_import_screen(0)
    im = Image.frombuffer("RGBA",(w,h),cap['c'],"raw","BGRA",0,1); im.convert("RGB").save(OUT/f"import_{w}x{h}.png")
t = load(str(ROOT / "gamepup-hardware-test"), "hwt")
for size in ((320,240),(480,272)):
    cap = shot(t, "hwt", size); w,h = size
    t.draw_ui(0, {t.KEY_UP}, {t.KEY_TAB}, 440, 8)
    Image.frombuffer("RGBA",(w,h),cap['c'],"raw","BGRA",0,1).convert("RGB").save(OUT/f"hwtest_{w}x{h}.png")
    t.draw_color_test(0, 6)
    Image.frombuffer("RGBA",(w,h),cap['c'],"raw","BGRA",0,1).convert("RGB").save(OUT/f"hwtest_bars_{w}x{h}.png")
print(sorted(p.name for p in OUT.iterdir()))
