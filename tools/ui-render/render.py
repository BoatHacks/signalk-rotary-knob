"""Crops the raw screens round, upscales them and builds the overview, and the manual's pictures.

Usage: render.py <build dir> <version text>, or render.py <build dir> manual (every screen in out/, 360 px, in img/manual)."""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

build, version = sys.argv[1], sys.argv[2]
MANUAL = version == 'manual'
SCREENS = [('autopilot', 'Autopilot'), ('anchor', 'Anchor watch'), ('lights', 'Lights'), ('music', 'Music'),
           ('motor', 'Motor (off by default)'), ('usb', 'USB-HID'), ('mode-ring', 'Mode ring'), ('settings', 'Settings'),
           ('drag-alarm', 'Drag alarm'), ('autopilot-night', 'Night palette')]


def screen(name, n):
    im = Image.open(os.path.join(build, 'out', name + '.ppm')).convert('RGB').resize((n, n), Image.LANCZOS)
    mask = Image.new('L', (n * 4, n * 4), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, n * 4 - 1, n * 4 - 1), fill=255)
    im.putalpha(mask.resize((n, n), Image.LANCZOS))
    return im


img = os.path.join(build, 'img')
if MANUAL:
    img = os.path.join(img, 'manual')
    os.makedirs(img, exist_ok=True)
    for f in sorted(os.listdir(os.path.join(build, 'out'))):
        screen(f[:-4], 360).quantize(256, method=Image.Quantize.FASTOCTREE).save(os.path.join(img, f[:-4] + '.png'), optimize=True)
    sys.exit()
os.makedirs(img, exist_ok=True)
font = ImageFont.load_default(24)
sheet = Image.new('RGB', (1720, 850), (43, 45, 49))
d = ImageDraw.Draw(sheet)
for i, (name, label) in enumerate(SCREENS):
    screen(name, 720).save(os.path.join(img, f'rendering-{name}.png'), optimize=True)
    x, y, p = 20 + i % 5 * 340, 20 + i // 5 * 390, screen(name, 320)
    sheet.paste(p, (x, y), p)
    d.text((x + 160, y + 345), label, font=font, fill=(219, 222, 225), anchor='mm')
d.text((860, 820), f'signalk-rotary-knob {version}: renderings of the firmware UI on a PC, not photos. Simulated data.',
       font=font, fill=(150, 153, 160), anchor='mm')
sheet.save(os.path.join(img, 'rendering-overview.png'), optimize=True)
