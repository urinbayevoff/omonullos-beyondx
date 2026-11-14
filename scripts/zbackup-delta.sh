#!/bin/sh
# zbackup-delta: what changed on the phone since a full backup, as one
# encrypted archive (to put the phone back after a re-flash: the full
# backup first, then this on top).
#
#   sudo sh scripts/zbackup-delta.sh --list [SINCE]   sizes only, nothing written
#   sudo sh scripts/zbackup-delta.sh [SINCE]          the archive (asks a passphrase)
#   sudo sh scripts/zbackup-delta.sh --restore FILE   put it back (asks the passphrase)
#
# SINCE defaults to 2026-11-13 00:00 (the 16 GB USERDATA backup on the
# Redmi). Every file of the root filesystem changed since then goes in,
# except what can be fetched again or must never leave the phone:
#   caches, logs, the image work files, repo clones that are on GitHub,
#   /var/lib/zmodem (EFS copies: IMEI) and /var/lib/zfield (field logs).
# The archive is tar + zstd, encrypted with age (passphrase): it holds the
# sudo password file, the Telegram session and GitHub's token, so it is
# safe to keep in Telegram or on an iPad only because of that passphrase.
# Files deleted since SINCE are not recorded.
#
# Added for OmonullOS, November 2026.
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
AGE=$(command -v age || echo /usr/local/bin/age)
H=/home/alarm
OUTDIR=$H/zbackup
EXCLUDE="/proc /sys /dev /run /tmp /var/tmp /var/cache /var/log /swapfile
$H/.cache $H/zbackup $H/omonullos-image-work $H/omonullos-public $H/zrestore-1114
/var/lib/zmodem /var/lib/zfield /var/lib/systemd/coredump /root/.cache"

case "${1:-}" in
--restore)
	f=${2:?archive}
	echo "  $f qaytariladi (/ ustiga). Parolni yozing:"
	"$AGE" -d "$f" | zstd -dc | tar -xpf - -C / --numeric-owner --xattrs --acls
	echo "  Qaytarildi. Telefonni qayta yoqing."
	exit 0 ;;
esac
LIST=0
[ "${1:-}" = --list ] && { LIST=1; shift; }
SINCE=${1:-2026-11-13 00:00}

prune=""
for p in $EXCLUDE; do prune="$prune -path $p -prune -o"; done
L=$(mktemp)
trap 'rm -f "$L"' EXIT
# shellcheck disable=SC2086
find / -xdev $prune \( -type f -o -type l \) -newermt "$SINCE" -print0 > "$L"
n=$(tr -cd '\0' < "$L" | wc -c)
bytes=$(xargs -0 -r stat -c %s < "$L" 2>/dev/null | awk '{s+=$1} END {print s+0}')
echo "  $SINCE dan beri: $n ta fayl, $((bytes / 1048576)) MB (siqilmagan)"
if [ "$LIST" = 1 ]; then
	xargs -0 -r stat -c '%s %n' < "$L" 2>/dev/null |
		awk '{split($2, a, "/"); k = "/" a[2] "/" a[3] "/" a[4]; s[k] += $1}
		     END {for (k in s) printf "%8.1f MB  %s\n", s[k] / 1048576, k}' | sort -rn | head -25
	exit 0
fi

mkdir -p "$OUTDIR"
out=$OUTDIR/delta-$(date +%Y%m%d-%H%M).tar.zst.age
cat <<'EOF'

  Zaxira arxivi shifrlanadi. Parol yozing (ikki marta), qoğozga ham yozib
  qöying: usiz arxivni oçib bölmaydi. Parol ekranda körinmaydi.

EOF
tar -cpf - --null -T "$L" --numeric-owner --xattrs --acls --ignore-failed-read 2>/dev/null |
	zstd -q -T0 -6 | "$AGE" -p -o "$out"
chown alarm: "$out"
sha256sum "$out" | sed "s|$OUTDIR/||" > "$out.sha256"
chown alarm: "$out.sha256"
echo
echo "  TAYYOR: $out ($(du -h "$out" | cut -f1))"
echo "  (Claudega «tayyor» deng: Telegramga va iPad uçun yuboradi.)"
