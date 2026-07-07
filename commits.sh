#!/bin/bash

show() {
	[ -d "$1" ] || return 0
	pushd "$1" >> /dev/null
	HASH=$(git rev-parse --short=8 HEAD)
	NAME=$(basename $PWD)
	DATE=$(git log -1 --pretty='%ad' --date=format:'%Y-%m-%d')
	REPO=$(git config --get remote.origin.url)
	REPO=$(sed -E "s,(^git@github.com:)|(^https?://github.com/)|(.git$)|(/$),,g" <<<"$REPO")
	popd >> /dev/null

	printf '\055 %-24s%-10s%-12s%s\n' $NAME $HASH $DATE $REPO
}
list() {
	[ -d "$1" ] || return 0
	pushd "$1" >> /dev/null
	for D in ./*/; do
		[ -d "$D" ] || continue
		show "$D"
	done
	popd >> /dev/null
}
rule() {
	echo '----------------------------------------------------------------'	
}
tell() {
	echo $1
	rule
}

cores() {
	echo CORES
	list ./workspace/$1/cores/src
	bump
}

bump() {
	printf '\n'
}

{
	# tell MINUI
	printf '%-26s%-10s%-12s%s\n' MINUI HASH DATE USER/REPO
	rule
	show ./
	bump
	
	tell TOOLCHAINS
	bump
	
	tell LIBRETRO
	show ./workspace/all/minarch/libretro-common
	bump
	
	tell RG35XX
	show ./workspace/rg35xx/other/DinguxCommander
	show ./workspace/rg35xx/other/evtest
	cores rg35xx
	
	tell RG35XXPLUS
	show ./workspace/rg35xxplus/other/dtc
	show ./workspace/rg35xxplus/other/fbset
	show ./workspace/rg35xxplus/other/sdl2
	show ./workspace/rg35xxplus/other/unzip60
	cores rg35xx # just copied from normal rg35xx
	
	tell CHECK
	echo https://github.com/USER/REPO/compare/HASH...HEAD
	bump
} | sed 's/\n/ /g'
