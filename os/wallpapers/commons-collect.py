"""collect.py OUT.json -- candidate wallpapers from Wikimedia Commons featured/quality categories"""
import json, sys, time, urllib.request, urllib.parse
UA = {"User-Agent": "OmonullOS-wallpapers/1.0 (private, non-commercial phone build)"}
SERIES = [
    ("O'zbekiston", ["Quality images of Uzbekistan", "Featured pictures of Uzbekistan", "Quality images of Samarkand", "Quality images of Bukhara", "Quality images of Tashkent", "Registan", "Itchan Kala"]),
    ("Tog'lar", ["Featured pictures of mountains"]),
    ("Manzaralar", ["Featured pictures of landscapes"]),
    ("Ko'llar", ["Featured pictures of lakes"]),
    ("Sharsharalar", ["Featured pictures of waterfalls"]),
    ("Qirg'oqlar", ["Featured pictures of beaches"]),
    ("Quyosh botishi", ["Featured pictures of sunsets"]),
    ("O'rmonlar", ["Featured pictures of forests"]),
    ("Gullar", ["Featured pictures of flowers"]),
    ("Koinot", ["Featured pictures of astronomy"]),
    ("Shaharlar", ["Featured pictures of cityscapes"]),
]
W, H = 1440, 3040
def api(**p):
    p.update(format="json", action="query")
    u = "https://commons.wikimedia.org/w/api.php?" + urllib.parse.urlencode(p)
    time.sleep(1)
    for i in range(8):
        try:
            return json.load(urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=90))
        except Exception as e:
            time.sleep(10 * (i + 1))
    raise SystemExit("api failed")
out = {}
for series, cats in SERIES:
    cands = []
    for cat in cats:
        cont = {}
        while True:
            r = api(generator="categorymembers", gcmtitle="Category:" + cat, gcmtype="file", gcmlimit=50,
                    prop="imageinfo", iiprop="size|url|mime|extmetadata",
                    iiextmetadatafilter="Artist|LicenseShortName", **cont)
            for pg in r.get("query", {}).get("pages", {}).values():
                ii = pg.get("imageinfo", [{}])[0]
                if ii.get("mime") != "image/jpeg":
                    continue
                w, h = ii["width"], ii["height"]
                if w < W or h < H:
                    continue
                md = ii.get("extmetadata", {})
                cands.append(dict(title=pg["title"], w=w, h=h, url=ii["url"], page=ii.get("descriptionurl"),
                                  artist=md.get("Artist", {}).get("value", ""),
                                  license=md.get("LicenseShortName", {}).get("value", "")))
            if "continue" not in r:
                break
            cont = {k: v for k, v in r["continue"].items()}
    # portrait first (closest to 9:19), then landscapes that lose the least when cropped
    cands.sort(key=lambda c: c["w"] / c["h"])
    out[series] = cands
    print(len(cands), series, "portrait:", sum(c["h"] > c["w"] for c in cands))
json.dump(out, open(sys.argv[1], "w"), ensure_ascii=False, indent=1)
