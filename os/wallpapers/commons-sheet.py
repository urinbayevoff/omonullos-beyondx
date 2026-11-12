"""sheet.py CANDS.json OUTDIR MAX -- contact sheets of the 9:19 centre crop, numbered"""
import io, json, os, sys, time, urllib.request, urllib.parse
from PIL import Image, ImageDraw
UA = {"User-Agent": "OmonullOS-wallpapers/1.0 (private, non-commercial phone build)"}
TW, TH = 120, 253
def get(u, js=False):
    for i in range(6):
        try:
            d = urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=90).read()
            return json.loads(d) if js else d
        except Exception as e:
            time.sleep(8 * (i + 1))
cands = json.load(open(sys.argv[1])); od = sys.argv[2]; M = int(sys.argv[3]); os.makedirs(od, exist_ok=True)
for si, (series, lst) in enumerate(cands.items()):
    lst = lst[:M]; thumbs = {}
    for i in range(0, len(lst), 40):
        part = lst[i:i + 40]; time.sleep(1)
        r = get("https://commons.wikimedia.org/w/api.php?" + urllib.parse.urlencode(dict(action="query", format="json",
              titles="|".join(c["title"] for c in part), prop="imageinfo", iiprop="url", iiurlheight=TH * 2)), True)
        for pg in r["query"]["pages"].values():
            thumbs[pg["title"]] = pg["imageinfo"][0].get("thumburl")
    cols = 10; rows = (len(lst) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (TW + 4), rows * (TH + 4)), "white"); d = ImageDraw.Draw(sheet)
    for i, c in enumerate(lst):
        u = thumbs.get(c["title"])
        if not u: continue
        time.sleep(0.3); data = get(u)
        if not data: continue
        im = Image.open(io.BytesIO(data)).convert("RGB")
        k = max(TW / im.width, TH / im.height); im = im.resize((int(im.width * k) + 1, int(im.height * k) + 1))
        l = (im.width - TW) // 2; t = (im.height - TH) // 2; im = im.crop((l, t, l + TW, t + TH))
        x, y = (i % cols) * (TW + 4), (i // cols) * (TH + 4); sheet.paste(im, (x, y))
        d.rectangle((x, y, x + 26, y + 16), fill="black"); d.text((x + 3, y + 2), str(i), fill="yellow")
    sheet.save(os.path.join(od, "%02d.jpg" % si), quality=80)
    print(si, series, len(lst), flush=True)
