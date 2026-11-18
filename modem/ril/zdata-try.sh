#!/bin/sh
# zdata-try.sh: bring a mobile data context up or down by hand through
# rild-lite, to try the data path before zcell does it by itself
# (docs/s10-5g/MOBILE-DATA.md). Needs the zero-copy zrn-modem.ko and a
# rild-lite that decodes data call answers.
#
#   sudo zdata-try.sh up [APN]     (default APN: internet)
#   sudo zdata-try.sh list
#   sudo zdata-try.sh down [CID]   (default: the one "up" made)
#
# "up" sends SETUP_DATA_CALL (27) with the strings libsec-ril.so reads
# (SecRilAdaptor::CopyDataReqData: the radio technology as it is, not +2 as
# in AOSP; mvno type and match data must not be NULL), reads the answer
# ({cid .. ifname addr .. dns .. gw ..}), sets the address on the interface,
# brings it up with a default route at metric 700 (Wi-Fi stays first) and
# pings through it. DNS is only shown, /etc/resolv.conf is not touched.
# "down" takes the route and address away and sends DEACTIVATE_DATA_CALL (41).
#
# Added for OmonullOS, November 2026.
set -u
ZRIL="python3 $(dirname "$(readlink -f "$0")")/zril.py"
[ -x /root/tools/zril.py ] && ZRIL="python3 /root/tools/zril.py"
STATE=/run/zdata-try
die() { echo "zdata-try: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo)"

case "${1:-}" in
up)
	APN=${2:-internet}
	case "$APN" in *[!A-Za-z0-9._-]*) die "odd APN" ;; esac
	# the data registration's radio technology
	tech=$($ZRIL -t 3 "raw0 21" | sed -n 's/^< 21 .* reg [0-9]* rat \([0-9]*\) .*/\1/p' | tail -1)
	case "$tech" in '' | 0) die "not registered for data" ;; esac
	echo "radio tech $tech, APN $APN"
	# radioTech profile apn user password authType protocol roamingProtocol
	# supportedTypes bearerBitmask modemCognitive mtu mvnoType mvnoMatch roamingAllowed
	ans=$($ZRIL -t 45 "raws 27 $tech 0 $APN \"\" \"\" 0 IP IP 1 0 0 0 \"\" \"\" 1" | grep '^< 27 ')
	echo "$ans"
	case "$ans" in *" e=0 "*) ;; *) die "no good answer" ;; esac
	cid=$(echo "$ans" | sed -n 's/.*{cid \([0-9]*\) active \([0-9]*\) status \([0-9-]*\) .*/\1/p')
	st=$(echo "$ans" | sed -n 's/.*{cid [0-9]* active [0-9]* status \([0-9-]*\) .*/\1/p')
	ifn=$(echo "$ans" | sed -n 's/.*status [0-9-]* retry [0-9-]* [^ ]* \([^ ]*\) addr .*/\1/p')
	addr=$(echo "$ans" | sed -n 's/.* addr \(.*\) dns .*/\1/p')
	dns=$(echo "$ans" | sed -n 's/.* dns \(.*\) gw .*/\1/p')
	[ "$st" = 0 ] || die "status $st (cid $cid)"
	[ -n "$ifn" ] && [ -n "$addr" ] && [ "$addr" != - ] || die "no interface or address in the answer"
	case "$ifn" in rmnet[0-7]) ;; *) die "odd interface $ifn" ;; esac
	a=${addr%% *}
	case "$a" in */*) ;; *) a=$a/32 ;; esac
	echo "cid=$cid ifn=$ifn addr=$a" > "$STATE"
	ip link set "$ifn" up
	ip addr add "$a" dev "$ifn"
	ip route add default dev "$ifn" metric 700
	echo "up: $ifn $a, dns $dns"
	ip -br addr show "$ifn"
	ping -c 4 -W 3 -I "$ifn" 1.1.1.1
	;;
list)
	$ZRIL -t 4 "raw0 57" | grep -E '^< 57 |^! 1010 '
	;;
down)
	cid=${2:-} ifn="" a=""
	[ -r "$STATE" ] && . "$STATE"
	[ -n "${2:-}" ] && cid=$2
	[ -n "$cid" ] || die "which cid?"
	if [ -n "$ifn" ]; then
		ip route del default dev "$ifn" metric 700 2>/dev/null
		[ -n "$a" ] && ip addr del "$a" dev "$ifn" 2>/dev/null
		ip link set "$ifn" down
	fi
	$ZRIL -t 8 "raws 41 $cid 0" | grep -E '^< 41 '
	rm -f "$STATE"
	;;
*)
	sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
	;;
esac
