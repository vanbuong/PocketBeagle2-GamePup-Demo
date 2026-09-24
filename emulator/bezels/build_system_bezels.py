#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build exact-size GamePup system bezel assets from generated artwork."""

from pathlib import Path

try:
    from PIL import Image, ImageEnhance
except ImportError:  # pragma: no cover - Pillow optional on build hosts
    Image = None
    ImageEnhance = None


OUTPUT_SIZE = (320, 20)
SOURCES = {
    "nes": {
        "filename": "nes-system-source.png",
        # The generated source has two wide bands separated by whitespace.
        "top_crop": (0, 166, 1774, 443),
        "bottom_crop": (0, 458, 1774, 735),
    },
    "gbc": {
        "filename": "gbc-system-source.png",
        # Crop each generated band to the final 16:1 band aspect ratio.
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


def render_band(source: "Image.Image", crop: tuple[int, int, int, int]) -> "Image.Image":
    """Crop without distortion, reduce to 320x20, then restore edge clarity."""
    band = source.crop(crop).resize(OUTPUT_SIZE, Image.Resampling.LANCZOS)
    band = ImageEnhance.Contrast(band).enhance(1.08)
    return ImageEnhance.Sharpness(band).enhance(1.35)


def stretch_rgb(source_path: Path, destination_path: Path,
                source_width: int = 128, destination_width: int = 320,
                height: int = 40) -> None:
    """Nearest-neighbor horizontal stretch for hosts without Pillow."""
    pixels = source_path.read_bytes()
    expected_source = source_width * height * 3
    expected_destination = destination_width * height * 3
    if len(pixels) == expected_destination:
        if source_path != destination_path:
            destination_path.write_bytes(pixels)
        return
    if len(pixels) != expected_source:
        raise SystemExit(
            f"{source_path} has {len(pixels)} bytes, "
            f"expected {expected_source} or {expected_destination}")
    out = bytearray(expected_destination)
    for y in range(height):
        for x in range(destination_width):
            source_x = x * source_width // destination_width
            src = (y * source_width + source_x) * 3
            dst = (y * destination_width + x) * 3
            out[dst:dst + 3] = pixels[src:src + 3]
    destination_path.write_bytes(out)


def main() -> None:
    output_dir = Path(__file__).resolve().parent
    if Image is None:
        for system in SOURCES:
            stretch_rgb(output_dir / f"{system}-system.rgb",
                        output_dir / f"{system}-system.rgb")
        print("Stretched existing RGB bezels to 320x40 (Pillow unavailable).")
        return

    for system, details in SOURCES.items():
        source = Image.open(output_dir / details["filename"]).convert("RGB")
        result = Image.new("RGB", (OUTPUT_SIZE[0], OUTPUT_SIZE[1] * 2))
        result.paste(render_band(source, details["top_crop"]), (0, 0))
        result.paste(render_band(source, details["bottom_crop"]),
                     (0, OUTPUT_SIZE[1]))
        result.save(output_dir / f"{system}-system.png", optimize=True)
        (output_dir / f"{system}-system.rgb").write_bytes(result.tobytes())


if __name__ == "__main__":
    main()
