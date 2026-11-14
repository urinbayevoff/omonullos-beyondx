#!/bin/sh
# zota-sign: sign an omonullOS update and publish its release (the owner, on
# the phone, in a terminal: it asks for the sudo password and the key's
# passphrase, and for «ha» before signing).
#
#   sh scripts/zota-sign.sh beyondx-v0.2           GitHub draft release
#   sh scripts/zota-sign.sh --dir DIR              a local release dir (tests)
#
# The CI leaves each release a DRAFT (phones never see drafts) with
# manifest.json and the packages. This shows what the manifest says (version,
# commit, what is new, how many files, what changed since the release before)
# and checks that the commit is the tag's and the packages' sizes match the
# manifest; on «ha» it signs manifest.json with /root/omonullos-ota/*.key.pem,
# checks the signature against the public key in the tree, uploads
# manifest.json.sig and publishes the release.
#
# Added for OmonullOS, November 2026.
set -eu
TREE=$(cd "$(dirname "$0")/.." && pwd)
REPO=${ZOTA_REPO:-urinbayevoff/omonullos-beyondx}
NAME=${ZOTA_KEY_NAME:-owner-2026}
KEY=/root/omonullos-ota/$NAME.key.pem
PUB=$TREE/image/beyondx/ota-keys/$NAME.pem
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
die() { echo "zota-sign: $*" >&2; exit 1; }

DIR= TAG=
case "${1:-}" in
--dir) DIR=${2:?dir}; cp "$DIR/manifest.json" "$W/" ;;
beyondx-v*) TAG=$1 ;;
*) die "usage: zota-sign.sh beyondx-vX.Y | --dir DIR" ;;
esac
[ -f "$PUB" ] || die "no public key $PUB"

if [ -n "$TAG" ]; then
	gh release view "$TAG" -R "$REPO" --json isDraft,tagName,assets > "$W/rel.json" || die "no release $TAG"
	gh release download "$TAG" -R "$REPO" -p manifest.json -D "$W" || die "the release has no manifest.json"
	tagsha=$(gh api "repos/$REPO/commits/$TAG" --jq .sha) || die "cannot read the tag's commit"
fi

python3 - "$W/manifest.json" "${TAG:+$W/rel.json}" "${tagsha:-}" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
rel = json.load(open(sys.argv[2])) if sys.argv[2] else None
tagsha = sys.argv[3]
bad = []
print()
print(f"  versiya:   {m['version']}  ({m.get('tag', '')})")
print(f"  commit:    {m.get('commit', '')[:12]}  sana: {m.get('date', '')}")
print(f"  fayllar:   {len(m['files'])} ta, öçiriladi: {len(m.get('remove', []))} ta")
d = m.get("delta")
print(f"  paket:     {m['package']['name']} {m['package']['size'] >> 20} MB"
      + (f";  delta {d['base']} dan: {d['size'] >> 10} KB" if d else ""))
print(f"  keyin:     {m.get('after', 'reboot')}   min_updater: {m.get('min_updater', 1)}")
print("  nima yangi:")
for line in m.get("notes", "").splitlines():
    print("    " + line)
if m.get("device") != "beyondx":
    bad.append("device is not beyondx")
if rel is not None:
    if not rel.get("isDraft"):
        bad.append("the release is already published")
    if m.get("tag") != rel.get("tagName"):
        bad.append(f"manifest tag {m.get('tag')} != release {rel.get('tagName')}")
    if tagsha and m.get("commit") != tagsha:
        bad.append(f"manifest commit {m.get('commit', '')[:12]} != the tag's {tagsha[:12]}")
    sizes = {a["name"]: a["size"] for a in rel.get("assets", [])}
    for p in [m["package"]] + ([d] if d else []):
        if sizes.get(p["name"]) != p["size"]:
            bad.append(f"{p['name']}: size {sizes.get(p['name'])} != manifest {p['size']}")
    if "manifest.json.sig" in sizes:
        bad.append("the release already has a manifest.json.sig")
if bad:
    print()
    for b in bad:
        print("  XATO: " + b)
    sys.exit(1)
EOF

printf '\n  Imzolab, release oçilsinmi? (ha/yöq): '
read -r ans
[ "$ans" = ha ] || die "not signed"

echo "  sudo paroli, keyin kalit paroli:"
sudo openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$W/manifest.json" -out "$W/manifest.json.sig"
sudo chown "$(id -u):$(id -g)" "$W/manifest.json.sig"
openssl pkeyutl -verify -pubin -inkey "$PUB" -rawin -in "$W/manifest.json" -sigfile "$W/manifest.json.sig" >/dev/null ||
	die "the signature does not verify with $PUB"

if [ -n "$DIR" ]; then
	cp "$W/manifest.json.sig" "$DIR/"
	echo "  imzolandi: $DIR/manifest.json.sig"
else
	gh release upload "$TAG" -R "$REPO" "$W/manifest.json.sig"
	gh release edit "$TAG" -R "$REPO" --draft=false
	echo "  imzolandi va oçildi: https://github.com/$REPO/releases/tag/$TAG"
fi
