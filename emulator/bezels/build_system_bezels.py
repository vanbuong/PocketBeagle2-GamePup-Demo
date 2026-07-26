#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build exact-size GamePup system bezel assets from generated artwork."""

from pathlib import Path

from PIL import Image, ImageEnhance


OUTPUT_SIZE = (128, 20)
SOURCES = {
    "nes": {
        "filename": "nes-system-source.png",
        # The generated source has two wide bands separated by whitespace.
        "top_crop": (0, 166, 1774, 443),
        "bottom_crop": (0, 458, 1774, 735),
    },
    "gbc": {
        "filename": "gbc-system-source.png",
        # Crop each generated band to the final 6.4:1 band aspect ratio.
        "top_crop": (0, 42, 1983, 352),
        "bottom_crop": (0, 440, 1983, 750),
    },
    "doom": {
        "filename": "doom-system-source.png",
        # Keep the full generated title panels; their bold forms tolerate the
        # small vertical reduction better than cropping either word.
        "top_crop": (3, 86, 1583, 450),
        "bottom_crop": (3, 484, 1583, 850),
    },
    "n64": {
        "filename": "n64-system-source.png",
        # Preserve the generated controller motifs and bold central labels.
        "top_crop": (0, 69, 1774, 346),
        "bottom_crop": (0, 542, 1774, 819),
    },
}


def render_band(source: Image.Image, crop: tuple[int, int, int, int]) -> Image.Image:
    """Crop without distortion, reduce to 128x20, then restore edge clarity."""
    band = source.crop(crop).resize(OUTPUT_SIZE, Image.Resampling.LANCZOS)
    band = ImageEnhance.Contrast(band).enhance(1.08)
    return ImageEnhance.Sharpness(band).enhance(1.35)


def main() -> None:
    output_dir = Path(__file__).resolve().parent
    for system, details in SOURCES.items():
        source = Image.open(output_dir / details["filename"]).convert("RGB")
        result = Image.new("RGB", (128, 40))
        result.paste(render_band(source, details["top_crop"]), (0, 0))
        result.paste(render_band(source, details["bottom_crop"]), (0, 20))
        result.save(output_dir / f"{system}-system.png", optimize=True)
        (output_dir / f"{system}-system.rgb").write_bytes(result.tobytes())


if __name__ == "__main__":
    main()
