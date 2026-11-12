"""fetch.py CANDS.json OUT N -- download N per series, cover-crop to 1440x3040, thumbs, credits"""
import io, json, math, os, sys, time, urllib.request, urllib.parse
from PIL import Image, ImageOps
UA = {"User-Agent": "OmonullOS-wallpapers/1.0 (private, non-commercial phone build)"}
W, H = 1440, 3040
THUMB = (216, 384)
def get(u, js=False):
    for i in range(8):
        try:
            d = urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=180).read()
            return json.loads(d) if js else d
        except Exception as e:
            print("  retry", i, e, file=sys.stderr); time.sleep(10 * (i + 1))
    return None
def thumb_url(title, width):
    u = "https://commons.wikimedia.org/w/api.php?" + urllib.parse.urlencode(dict(
        action="query", format="json", titles=title, prop="imageinfo", iiprop="url", iiurlwidth=width))
    r = get(u, True)
    pg = next(iter(r["query"]["pages"].values()))
    return pg["imageinfo"][0].get("thumburl") or pg["imageinfo"][0]["url"]
def cover(im, size):
    k = max(size[0] / im.width, size[1] / im.height)
    im = im.resize((math.ceil(im.width * k), math.ceil(im.height * k)), Image.LANCZOS)
    l = (im.width - size[0]) // 2; t = (im.height - size[1]) // 2
    return im.crop((l, t, l + size[0], t + size[1]))
cands = json.load(open(sys.argv[1])); out = sys.argv[2]; N = int(sys.argv[3])
root = os.path.join(out, "backgrounds", "ztos"); seen = set(); credits = []
for series, lst in cands.items():
    dst = os.path.join(root, series); th = os.path.join(root, ".thumbs", series)
    os.makedirs(dst, exist_ok=True); os.makedirs(th, exist_ok=True)
    n = 0
    for c in lst:
        if n >= N: break
        if c["title"] in seen: continue
        seen.add(c["title"])
        k = max(W / c["w"], H / c["h"])
        need = math.ceil(c["w"] * k)
        time.sleep(1)
        u = thumb_url(c["title"], need) if need < c["w"] else c["url"]
        data = get(u)
        if not data: continue
        try:
            im = ImageOps.exif_transpose(Image.open(io.BytesIO(data))).convert("RGB")
        except Exception as e:
            print("  bad", c["title"], e, file=sys.stderr); continue
        if im.width < W * 0.98 and im.height < H * 0.98: continue
        n += 1; name = "%02d.jpg" % n
        cover(im, (W, H)).save(os.path.join(dst, name), quality=90, optimize=True, progressive=True)
        cover(im, THUMB).save(os.path.join(th, name + ".jpg"), quality=82)
        credits.append("%s/%s\t%s\t%s\t%s" % (series, name, c["title"], c["license"], c["page"]))
        print(series, name, c["title"][:60], flush=True)
open(os.path.join(root, ".credits.tsv"), "w").write("\n".join(credits) + "\n")
