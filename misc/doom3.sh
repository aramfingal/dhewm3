#!/bin/sh
# Start dhewm3 with one of the installed HD packs.
#
#   misc/doom3.sh [d3hdp|hidef2k|vanilla] [roe] [extra dhewm3 arguments]
#
#   d3hdp    Doom 3 Essential HD Pack v2.0 (default)
#   hidef2k  Hi-Def 2K texture pack for dhewm3 (runs as its own mod, with its own
#            config and savegames)
#   vanilla  the game data as shipped
#   roe      Resurrection of Evil instead of the base game
#
# The packs live in "~/Library/Application Support/dhewm3-packs/<pack>" and are added to the
# game's search path with fs_devpath, so only the chosen one is loaded and the game data
# itself is never changed. DOOM3_DATA overrides where the game data is.

here=$(cd "$(dirname "$0")/.." && pwd)
bin="$here/build/dhewm3.app/Contents/MacOS/dhewm3"
data="${DOOM3_DATA:-$HOME/Projects/doom-3-extracted}"
packs="$HOME/Library/Application Support/dhewm3-packs"

pack=d3hdp
case "$1" in
	d3hdp|hidef2k|vanilla) pack=$1; shift ;;
esac
roe=0
if [ "$1" = "roe" ]; then
	roe=1
	shift
fi

if [ "$pack" != vanilla ] && [ ! -d "$packs/$pack" ]; then
	echo "pack '$pack' is not installed in $packs" >&2
	exit 1
fi

set -- +set fs_basepath "$data" "$@"
case "$pack" in
	d3hdp)
		set -- +set fs_devpath "$packs/d3hdp" "$@"
		[ $roe = 1 ] && set -- +set fs_game d3xp "$@"
		;;
	hidef2k)
		# What the pack's own .bat files and autoexec.cfg set. Its lighting is darker than the
		# game's and it compensates with gamma and brightness. These cvars are saved in the
		# config, so the pack gets a config folder of its own and the other setups keep theirs.
		cfg="$HOME/Library/Application Support/dhewm3/hidef2k-config"
		for g in hidef2k d3xp; do
			if [ ! -f "$cfg/$g/dhewm.cfg" ]; then
				mkdir -p "$cfg/$g"
				# start from the settings and key bindings of the normal setup
				for src in "$HOME/Library/Application Support/dhewm3/$g/dhewm.cfg" "$HOME/Library/Application Support/dhewm3/base/dhewm.cfg"; do
					[ -f "$src" ] && cp "$src" "$cfg/$g/dhewm.cfg" && break
				done
			fi
		done
		set -- +set fs_devpath "$packs/hidef2k" +set fs_configpath "$cfg" \
			+set image_usePrecompressedTextures 2 +set image_useNormalCompression 2 \
			+set r_gamma 1.2 +set r_brightness 1.5 "$@"
		if [ $roe = 1 ]; then
			set -- +set fs_game d3xp +set fs_game_base hidef2k "$@"
		else
			set -- +set fs_game hidef2k "$@"
		fi
		;;
	vanilla)
		[ $roe = 1 ] && set -- +set fs_game d3xp "$@"
		;;
esac

exec "$bin" "$@"
