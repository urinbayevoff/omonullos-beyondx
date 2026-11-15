#!/bin/sh
# ota-github-check: this tree's omonullos-update asks the REAL GitHub for
# updates, as a phone with nothing installed would, both ways: the API, and
# (the API pointed at nothing) the latest release's own files. A scratch
# root, the keys in the tree; nothing of the system is touched.
#
#   scripts/ota-github-check.sh [OWNER/REPO]   default urinbayevoff/omonullos-beyondx
#
# The CI runs it before it builds a release (build-image-beyondx.yml): 0.2,
# 0.2.1 and 0.2.2 were released with an updater whose every check failed
# (GitHub's API answered 415), and only a dir: source had ever been tested.
#
# Added for OmonullOS, November 2026.
set -eu
TREE=$(cd "$(dirname "$0")/.." && pwd)
UPD=${OTA_UPDATER:-$TREE/rootfs/overlay-beyondx/usr/local/sbin/omonullos-update}
REPO=${1:-urinbayevoff/omonullos-beyondx}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/root/usr/local/share/omonullos/ota-keys" "$W/root/var/lib/omonullos-update"
cp "$TREE"/image/beyondx/ota-keys/*.pem "$W/root/usr/local/share/omonullos/ota-keys/"
echo "source=github:$REPO" > "$W/conf"
export OMONULLOS_UPDATE_ROOT="$W/root" OMONULLOS_UPDATE_RUN="$W/run" \
	OMONULLOS_UPDATE_VAR="$W/root/var/lib/omonullos-update" OMONULLOS_UPDATE_LOG="$W/log" \
	OMONULLOS_UPDATE_CONF="$W/conf"
st() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["state"])' "$W/run/status.json"; }
bad=0
# a state that means GitHub answered and a signed release was read
good() { case "$1" in available|needs-updater|kernel|uptodate) return 0 ;; esac; return 1; }

python3 "$UPD" check --quiet || true
s=$(st)
if grep -q "GitHub API failed" "$W/log" || ! good "$s"; then
	echo "FAIL: the API way: $s"; bad=1
else
	echo "ok: the API way: $s"
fi

: > "$W/log"
OMONULLOS_UPDATE_GITHUB_API="https://api.github.com/omonullos-no-such-path" python3 "$UPD" check --quiet || true
s=$(st)
if ! grep -q "GitHub API failed" "$W/log" || ! good "$s"; then
	echo "FAIL: the latest release's files: $s"; bad=1
else
	echo "ok: the latest release's files: $s"
fi
[ "$bad" = 0 ] || { echo "--- log"; cat "$W/log"; exit 1; }
