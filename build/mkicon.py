"""从主 PNG 生成桌面图标、原生窗口像素数据和可选的 Windows 图标。"""

from argparse import ArgumentParser
from pathlib import Path

from PIL import Image


SIZES = (16, 24, 32, 48, 64, 128, 256)
MASTER = Path(__file__).resolve().parents[1] / "assets" / "shiying.png"


def write_header(path, icons):
    with path.open("w", encoding="utf-8", newline="\n") as header:
        header.write("/* 自动生成：build/mkicon.py */\n")
        for size in (32, 48, 64):
            values = []
            rgba = icons[size].tobytes()
            for r, g, b, alpha in zip(rgba[::4], rgba[1::4], rgba[2::4], rgba[3::4]):
                pixel = (alpha << 24) | (r * alpha // 255 << 16) | (g * alpha // 255 << 8) | (b * alpha // 255)
                values.append(f"0x{pixel:08x}u")
            header.write(f"static const unsigned int ICON{size}[{size * size}] = {{\n")
            for offset in range(0, len(values), 8):
                header.write("  " + ",".join(values[offset:offset + 8]) + ",\n")
            header.write("};\n")


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="PNG 和 C 头文件输出目录")
    parser.add_argument("--source", type=Path, default=MASTER, help="替换主 PNG")
    parser.add_argument("--ico", type=Path, help="Windows ICO 输出路径")
    args = parser.parse_args()

    with Image.open(args.source) as original:
        if original.width != original.height or original.width < max(SIZES):
            parser.error("主 PNG 必须为边长至少 256 像素的正方形")
        master = original.convert("RGBA")

    args.directory.mkdir(parents=True, exist_ok=True)
    icons = {size: master.resize((size, size), Image.Resampling.LANCZOS) for size in SIZES}
    for size, icon in icons.items():
        icon.save(args.directory / f"shiying-{size}.png")
    png = args.directory / "shiying.png"
    if png.resolve() != args.source.resolve():
        icons[256].save(png)
    write_header(args.directory / "icon_data.h", icons)

    if args.ico:
        args.ico.parent.mkdir(parents=True, exist_ok=True)
        master.save(args.ico, format="ICO", sizes=[(size, size) for size in SIZES])


if __name__ == "__main__":
    main()
