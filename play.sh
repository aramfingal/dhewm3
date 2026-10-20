#!/bin/sh
# Launches the game built in build/ with the Metal renderer and the game data in
# ~/Projects/doom-3-english. Saves, config and the HD pack stay in
# ~/Library/Application Support/dhewm3. Extra arguments go to the game, e.g.
#   ./play.sh +set fs_game d3xp          (Resurrection of Evil)
#   ./play.sh +map game/mc_underground
cd "$(dirname "$0")" || exit 1
exec ./build/dhewm3.app/Contents/MacOS/dhewm3 \
	+set fs_basepath "$HOME/Projects/doom-3-english" \
	+set r_backend metal \
	+set sys_lang english \
	"$@"
