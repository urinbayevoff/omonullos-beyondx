"""commons-fetch.py CANDS.json OUT N -- download N per series, cover-crop to 1440x3040, thumbs, credits

Modified for OmonullOS, November 2026: Wikimedia only serves thumbnails in
its standard widths now (https://w.wiki/GHai; other widths get HTTP 429), so
the width asked for is the next standard one (or the original). Files that
are already there are kept and not downloaded again (the image ships three
of them, zcommons-fetch adds the rest on the phone), and .credits.tsv keeps
its lines. With ZCOMMONS_STATUS set, that file gets "progress HAVE TOTAL
files" after each one, for Settings' progress bar. Exits 1 when no wallpaper at all is there afterwards.
"""
import io, json, math, os, sys, time, urllib.request, urllib.parse
from PIL import Image, ImageOps
UA = {"User-Agent": "OmonullOS-wallpapers/1.1 (non-commercial phone system)"}
W, H = 1440, 3040
THUMB = (216, 384)
STEPS = (960, 1280, 1920, 3840)	# Wikimedia's standard thumbnail widths
def get(u, js=False):
    for i in range(6):
        try:
            d = urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=120).read()
            return json.loads(d) if js else d
        except Exception as e:
            print("  retry", i, e, file=sys.stderr); time.sleep(10 * (i + 1))
    return None
def thumb_url(title, width):
    u = "https://commons.wikimedia.org/w/api.php?" + urllib.parse.urlencode(dict(
        action="query", format="json", titles=title, prop="imageinfo", iiprop="url", iiurlwidth=width))
    r = get(u, True)
    if not r: return None
    pg = next(iter(r["query"]["pages"].values()))
    return pg["imageinfo"][0].get("thumburl") or pg["imageinfo"][0]["url"]
def cover(im, size):
    k = max(size[0] / im.width, size[1] / im.height)
    im = im.resize((math.ceil(im.width * k), math.ceil(im.height * k)), Image.LANCZOS)
    l = (im.width - size[0]) // 2; t = (im.height - size[1]) // 2
    return im.crop((l, t, l + size[0], t + size[1]))
cands = json.load(open(sys.argv[1])); out = sys.argv[2]; N = int(sys.argv[3])
root = os.path.join(out, "backgrounds", "ztos"); seen = set(); have = 0
# ZCOMMONS_STATUS: where zcommons-fetch wants "progress HAVE TOTAL files"
status = os.environ.get("ZCOMMONS_STATUS"); total = N * len(cands)
def progress():
    if not status: return
    try:
        with open(status + ".tmp", "w") as f: f.write("progress %d %d files\n" % (have, total))
        os.chmod(status + ".tmp", 0o644); os.replace(status + ".tmp", status)
    except OSError: pass
progress()
cred_path = os.path.join(root, ".credits.tsv")
credits = {}
if os.path.exists(cred_path):
    for line in open(cred_path, encoding="utf-8"):
        if line.strip(): credits[line.split("\t", 1)[0]] = line.rstrip("\n")
for series, lst in cands.items():
    dst = os.path.join(root, series); th = os.path.join(root, ".thumbs", series)
    os.makedirs(dst, exist_ok=True); os.makedirs(th, exist_ok=True)
    n = 0
    for c in lst:
        if n >= N: break
        if c["title"] in seen: continue
        seen.add(c["title"])
        name = "%02d.jpg" % (n + 1)
        if os.path.exists(os.path.join(dst, name)):
            n += 1; have += 1; progress(); continue
        k = max(W / c["w"], H / c["h"])
        need = math.ceil(c["w"] * k)
        step = next((s for s in STEPS if s >= need), None)
        time.sleep(1)
        u = thumb_url(c["title"], step) if step and step < c["w"] else c["url"]
        data = get(u) if u else None
        if not data: continue
        try:
            im = ImageOps.exif_transpose(Image.open(io.BytesIO(data))).convert("RGB")
        except Exception as e:
            print("  bad", c["title"], e, file=sys.stderr); continue
        if im.width < W * 0.98 and im.height < H * 0.98: continue
        n += 1; have += 1
        cover(im, (W, H)).save(os.path.join(dst, name), quality=90, optimize=True, progressive=True)
        cover(im, THUMB).save(os.path.join(th, name + ".jpg"), quality=82)
        credits["%s/%s" % (series, name)] = "%s/%s\t%s\t%s\t%s" % (series, name, c["title"], c["license"], c["page"])
        print(series, name, c["title"][:60], flush=True)
        progress()
open(cred_path, "w", encoding="utf-8").write("\n".join(credits[k] for k in sorted(credits)) + "\n")
print("wallpapers:", have, flush=True)
sys.exit(0 if have else 1)
