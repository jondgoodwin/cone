# Makes the JPEG test data: small synthetic pictures saved by Pillow (libjpeg-turbo)
# in every mode the decoder supports, each with a reference decode (Pillow's, the
# oracle) saved as a lossless PNG. Used only to make the files in tests/data.
import sys, os, math, random
from PIL import Image, ImageDraw
import PIL

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def picture(w, h, seed):
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            r = int(127 + 120 * math.sin(x / 7.0 + seed))
            g = int(127 + 120 * math.sin(y / 5.0 + x / 23.0))
            b = int(255 * x / max(1, w - 1))
            px[x, y] = (r, g, b)
    d = ImageDraw.Draw(im)
    d.ellipse([w // 5, h // 5, w // 2, h // 2], fill=(250, 20, 20), outline=(0, 0, 0))
    d.rectangle([w // 2, h // 2, w - 5, h - 4], fill=(10, 200, 30))
    d.line([0, 0, w, h], fill=(255, 255, 255), width=2)
    d.text((3, h // 2), "Ab", fill=(0, 0, 0))
    for _ in range(40):  # speckle: high frequencies
        px[rnd.randrange(w), rnd.randrange(h)] = (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256))
    return im

def save(name, im, **kw):
    path = os.path.join(out, name + ".jpg")
    im.save(path, "JPEG", **kw)
    ref = Image.open(path)
    ref.load()
    ex = ref.getexif()
    ref = ref.convert("RGB")
    ref.save(os.path.join(out, name + ".png"))
    print(name, os.path.getsize(path), ref.size)

im = picture(61, 43, 1)
save("b444", im, quality=85, subsampling=0)
save("b422", im, quality=85, subsampling=1)
save("b420", im, quality=85, subsampling=2)
save("gray", im.convert("L"), quality=85)
save("p444", im, quality=85, subsampling=0, progressive=True)
save("p422", im, quality=85, subsampling=1, progressive=True)
save("p420", im, quality=85, subsampling=2, progressive=True)
save("pgray", im.convert("L"), quality=85, progressive=True)
save("opt420", im, quality=70, subsampling=2, optimize=True)
save("q5", im, quality=5, subsampling=2)
save("q100", im, quality=100, subsampling=0)
big = picture(130, 97, 3)
save("odd420", big, quality=90, subsampling=2)
save("odd422p", big, quality=90, subsampling=1, progressive=True)
try:
    save("rst420", im, quality=85, subsampling=2, restart_marker_blocks=3)
    save("rstp420", im, quality=85, subsampling=2, progressive=True, restart_marker_rows=1)
except Exception as e:
    print("no restart markers:", e)
# EXIF orientation 1..8 on a non-square picture
small = picture(40, 24, 2)
for o in range(2, 9):
    ex = Image.Exif()
    ex[0x0112] = o
    path = os.path.join(out, "orient%d.jpg" % o)
    small.save(path, "JPEG", quality=95, subsampling=0, exif=ex)
    from PIL import ImageOps
    r = Image.open(path)
    r.load()
    r = ImageOps.exif_transpose(r).convert("RGB")
    r.save(os.path.join(out, "orient%d.png" % o))
    print("orient", o, r.size)
cm = Image.new("CMYK", (32, 16))
cpx = cm.load()
for y in range(16):
    for x in range(32):
        cpx[x, y] = (x * 8, y * 16, 128, 20 if x < 16 else 0)
cm.save(os.path.join(out, "cmyk.jpg"), "JPEG", quality=95)
print("pillow", PIL.__version__)
