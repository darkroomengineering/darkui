#!/bin/bash
# Fetch box-art thumbnails for a darkUI ROMs tree from the libretro-thumbnails
# GitHub org and pre-size them for the on-device preview pane (no runtime
# scaling exists on-device, so art must already be small).
#
# darkUI's launcher looks for art at:
#   <system-dir>/.res/<full rom filename including extension>.png
# e.g. Roms/Game Boy Advance (GBA)/Metroid Fusion (USA).gba
#   -> Roms/Game Boy Advance (GBA)/.res/Metroid Fusion (USA).gba.png
#
# Usage: scripts/fetch-boxart.sh [--force] [--dry-run] <roms-dir>
#
#   <roms-dir>   Directory shaped like a card's Roms/ folder: subdirs named
#                "<System Name> (TAG)" containing ROM files.
#   --force      Re-fetch art that already exists in .res/.
#   --dry-run    Print planned actions; don't hit the network for downloads
#                and don't write any files (existence checks still use
#                lightweight HEAD requests).
#
# Matching:
#   1. Direct: try Named_Boxarts/<rom-title-minus-extension>.png verbatim
#      (URL-encoded, "&" -> "_" per libretro naming convention).
#   2. Fuzzy: on a 404, fetch that system's full Named_Boxarts file listing
#      once per run (GitHub API tree endpoint, cached in a temp dir) and
#      match by title (the part of the filename before the first "("),
#      case-insensitively. Among same-title candidates, prefer one whose
#      region tag (content of the first parenthesised group) matches the
#      ROM's region tag; otherwise take the first candidate alphabetically.
#      This is a simple heuristic, not a scored fuzzy match -- see
#      "Known limitations" in the PR description / commit message.
#
# Rate limits: the GitHub API tree call is unauthenticated (60 req/hr) but is
# only made once per system per run (at most 7 systems are mapped below), so
# it comfortably fits. Direct raw.githubusercontent.com downloads are not
# subject to that limit.

set -uo pipefail

# ---------------------------------------------------------------------------
# Args
# ---------------------------------------------------------------------------

FORCE=0
DRY_RUN=0
ROMS_DIR=""

for arg in "$@"; do
	case "$arg" in
	--force)
		FORCE=1
		;;
	--dry-run)
		DRY_RUN=1
		;;
	-h | --help)
		sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	-*)
		echo "error: unknown flag: $arg" >&2
		exit 1
		;;
	*)
		ROMS_DIR="$arg"
		;;
	esac
done

if [ -z "$ROMS_DIR" ]; then
	echo "usage: scripts/fetch-boxart.sh [--force] [--dry-run] <roms-dir>" >&2
	exit 1
fi

if [ ! -d "$ROMS_DIR" ]; then
	echo "error: $ROMS_DIR is not a directory" >&2
	exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
	echo "error: curl not found on PATH" >&2
	exit 1
fi

if ! command -v sips >/dev/null 2>&1; then
	echo "error: sips not found (this script is macOS-only)" >&2
	exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "error: python3 not found on PATH" >&2
	exit 1
fi

# ---------------------------------------------------------------------------
# System tag -> libretro-thumbnails repo map
#
# NOTE: macOS ships bash 3.2 as /bin/bash (last GPLv2 release; Apple won't
# ship GPLv3). bash 3.2 has no associative arrays (`declare -A`, bash 4+
# only), so this is deliberately a case statement instead of a map literal --
# keep it that way, don't "simplify" it back to declare -A.
# ---------------------------------------------------------------------------

tag_to_repo() {
	case "$1" in
	GB) echo "Nintendo_-_Game_Boy" ;;
	GBC) echo "Nintendo_-_Game_Boy_Color" ;;
	GBA) echo "Nintendo_-_Game_Boy_Advance" ;;
	FC) echo "Nintendo_-_Nintendo_Entertainment_System" ;;
	SFC) echo "Nintendo_-_Super_Nintendo_Entertainment_System" ;;
	MD) echo "Sega_-_Mega_Drive_-_Genesis" ;;
	PS) echo "Sony_-_PlayStation" ;;
	*) echo "" ;;
	esac
}

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# URL-encode a string for use in a raw.githubusercontent.com path segment.
url_encode() {
	python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$1"
}

# Fetch (and cache for the run) the Named_Boxarts file index for a repo.
# Prints the path to a newline-delimited file of titles (without the
# "Named_Boxarts/" prefix or ".png" suffix), or nothing on failure.
fetch_index() {
	local repo="$1"
	local cache="$WORK_DIR/index-${repo}.txt"

	if [ -f "$cache" ]; then
		echo "$cache"
		return 0
	fi

	local api_url="https://api.github.com/repos/libretro-thumbnails/${repo}/git/trees/master?recursive=1"
	local raw_json="$WORK_DIR/tree-${repo}.json"

	if ! curl -sf -H "Accept: application/vnd.github+json" -o "$raw_json" "$api_url"; then
		echo "warn: could not fetch file index for $repo (rate-limited or network error)" >&2
		return 1
	fi

	python3 - "$raw_json" <<-'PYEOF' >"$cache"
		import json, sys
		with open(sys.argv[1]) as f:
		    data = json.load(f)
		for entry in data.get("tree", []):
		    path = entry.get("path", "")
		    if path.startswith("Named_Boxarts/") and path.endswith(".png"):
		        print(path[len("Named_Boxarts/"):-len(".png")])
	PYEOF

	if [ ! -s "$cache" ]; then
		echo "warn: empty file index for $repo" >&2
		return 1
	fi

	echo "$cache"
}

# Fuzzy-match a rom title/region against a cached index file.
# Prints the matched title (without .png) on stdout, nothing if no match.
fuzzy_match() {
	local title="$1"
	local region="$2"
	local index_file="$3"

	python3 - "$title" "$region" "$index_file" <<-'PYEOF'
		import sys

		title, region, index_file = sys.argv[1], sys.argv[2], sys.argv[3]

		def split_title_region(name):
		    paren = name.find("(")
		    if paren == -1:
		        return name.strip(), ""
		    t = name[:paren].strip()
		    close = name.find(")", paren)
		    r = name[paren + 1:close] if close != -1 else ""
		    return t, r

		def norm(s):
		    return s.replace("&", "_").strip().lower()

		with open(index_file) as f:
		    candidates = [line.rstrip("\n") for line in f if line.strip()]

		matches = []
		for c in candidates:
		    c_title, c_region = split_title_region(c)
		    if norm(c_title) == norm(title):
		        matches.append((c, c_region))

		if not matches:
		    sys.exit(0)

		matches.sort(key=lambda m: m[0].lower())

		region_tokens = {t.strip().lower() for t in region.split(",") if t.strip()}
		if region_tokens:
		    for c, c_region in matches:
		        c_tokens = {t.strip().lower() for t in c_region.split(",") if t.strip()}
		        if region_tokens & c_tokens:
		            print(c)
		            sys.exit(0)

		print(matches[0][0])
	PYEOF
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

# Parallel indexed arrays instead of an associative array, for the same
# bash-3.2-on-macOS reason as tag_to_repo() above.
SUMMARY_NAMES=()
SUMMARY_FETCHED=()
SUMMARY_SKIPPED=()
SUMMARY_MISSED=()
MISS_LINES=()

shopt -s nullglob

for system_dir in "$ROMS_DIR"/*/; do
	system_dir="${system_dir%/}"
	system_name="$(basename "$system_dir")"

	if [[ "$system_name" == .* ]]; then
		continue
	fi

	if [[ ! "$system_name" =~ \(([A-Za-z0-9]+)\)[[:space:]]*$ ]]; then
		echo "note: skipping \"$system_name\" (no (TAG) suffix found)"
		continue
	fi
	tag="${BASH_REMATCH[1]}"

	repo="$(tag_to_repo "$tag")"
	if [ -z "$repo" ]; then
		echo "note: skipping \"$system_name\" (unmapped tag: $tag)"
		continue
	fi

	sys_fetched=0
	sys_skipped=0
	sys_missed=0

	res_dir="$system_dir/.res"

	for rom_path in "$system_dir"/*; do
		[ -f "$rom_path" ] || continue
		rom_file="$(basename "$rom_path")"

		if [[ "$rom_file" == .* ]]; then
			continue
		fi

		art_path="$res_dir/${rom_file}.png"
		base="${rom_file%.*}"

		if [ -f "$art_path" ] && [ "$FORCE" -eq 0 ]; then
			sys_skipped=$((sys_skipped + 1))
			continue
		fi

		# libretro-thumbnails filenames use "_" where the title has "&"
		direct_name="${base//&/_}"
		encoded_direct="$(url_encode "$direct_name")"
		direct_url="https://raw.githubusercontent.com/libretro-thumbnails/${repo}/master/Named_Boxarts/${encoded_direct}.png"

		matched_name=""
		match_kind=""

		if [ "$DRY_RUN" -eq 1 ]; then
			if curl -sf -o /dev/null -I "$direct_url"; then
				matched_name="$direct_name"
				match_kind="direct"
			fi
		else
			tmp_png="$WORK_DIR/dl.png"
			if curl -sf -o "$tmp_png" "$direct_url"; then
				matched_name="$direct_name"
				match_kind="direct"
			fi
		fi

		if [ -z "$matched_name" ]; then
			index_file="$(fetch_index "$repo")"
			if [ -n "$index_file" ]; then
				title="${base%%(*}"
				title="$(echo "$title" | sed -E 's/[[:space:]]+$//')"
				region=""
				region_re='\(([^)]*)\)'
				if [[ "$base" =~ $region_re ]]; then
					region="${BASH_REMATCH[1]}"
				fi
				fuzzy_hit="$(fuzzy_match "$title" "$region" "$index_file")"
				if [ -n "$fuzzy_hit" ]; then
					encoded_fuzzy="$(url_encode "$fuzzy_hit")"
					fuzzy_url="https://raw.githubusercontent.com/libretro-thumbnails/${repo}/master/Named_Boxarts/${encoded_fuzzy}.png"
					if [ "$DRY_RUN" -eq 1 ]; then
						if curl -sf -o /dev/null -I "$fuzzy_url"; then
							matched_name="$fuzzy_hit"
							match_kind="fuzzy"
						fi
					else
						tmp_png="$WORK_DIR/dl.png"
						if curl -sf -o "$tmp_png" "$fuzzy_url"; then
							matched_name="$fuzzy_hit"
							match_kind="fuzzy"
						fi
					fi
				fi
			fi
		fi

		if [ -z "$matched_name" ]; then
			echo "MISS: $system_name / $rom_file"
			MISS_LINES+=("$system_name / $rom_file")
			sys_missed=$((sys_missed + 1))
			continue
		fi

		if [ "$DRY_RUN" -eq 1 ]; then
			echo "[dry-run] [$match_kind] $system_name / $rom_file -> ${matched_name}.png"
			sys_fetched=$((sys_fetched + 1))
			continue
		fi

		width="$(sips -g pixelWidth "$tmp_png" 2>/dev/null | awk '/pixelWidth:/{print $2}')"
		height="$(sips -g pixelHeight "$tmp_png" 2>/dev/null | awk '/pixelHeight:/{print $2}')"
		max_dim=0
		if [ -n "$width" ] && [ -n "$height" ]; then
			max_dim=$((width > height ? width : height))
		fi

		if [ "$max_dim" -gt 360 ]; then
			sips -Z 360 "$tmp_png" >/dev/null 2>&1
		fi

		mkdir -p "$res_dir"
		cp "$tmp_png" "$art_path"

		echo "fetched: [$match_kind] $system_name / $rom_file -> ${matched_name}.png"
		sys_fetched=$((sys_fetched + 1))
	done

	SUMMARY_NAMES+=("$system_name")
	SUMMARY_FETCHED+=("$sys_fetched")
	SUMMARY_SKIPPED+=("$sys_skipped")
	SUMMARY_MISSED+=("$sys_missed")
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo
echo "== Summary =="
total_fetched=0
total_skipped=0
total_missed=0
i=0
while [ "$i" -lt "${#SUMMARY_NAMES[@]}" ]; do
	system_name="${SUMMARY_NAMES[$i]}"
	f="${SUMMARY_FETCHED[$i]}"
	s="${SUMMARY_SKIPPED[$i]}"
	m="${SUMMARY_MISSED[$i]}"
	total_fetched=$((total_fetched + f))
	total_skipped=$((total_skipped + s))
	total_missed=$((total_missed + m))
	echo "$system_name: fetched=$f skipped-existing=$s missed=$m"
	i=$((i + 1))
done
echo "TOTAL: fetched=$total_fetched skipped-existing=$total_skipped missed=$total_missed"

if [ "${#MISS_LINES[@]}" -gt 0 ]; then
	echo
	echo "Missed art:"
	for line in "${MISS_LINES[@]}"; do
		echo "  - $line"
	done
fi
