#!/usr/bin/env bash
# Fetch the game code for an Android build from one link, and leave a usable generated/ in the current directory.
#
# Why this is a script and not a step inside the workflow: two jobs need it (the arm64 compile and the APK), and the
# same thing is handy locally when you have the file on some host and do not want to move it by hand:
#
#   tools/android/fetch_game_code.sh "https://huggingface.co/<user>/<repo>/resolve/main/shadow.iso"
#   tools/android/fetch_game_code.sh "./shadow.iso"          # a local path works too
#
# What the link may be, in the order the checks run:
#   * a web page (HTML)                     -> refused, with the "use the direct link" hint (Hugging Face /blob/ pages)
#   * a .tar.gz / .tgz / .tar.xz / .zip      -> unpacked; it may hold generated/, a disc folder, main.dol or an ISO
#   * an .iso / .gcm image                   -> only sys/main.dol is taken out of it (5 MB of 1.4 GB)
#   * a Dolphin-compressed image (.rvz/.wia/.gcz) -> refused: convert to ISO once, Dolphin's tool is not here
#   * anything else                          -> treated as main.dol
#
# generated/ already in the archive is used as is (fastest). Otherwise tools/gen.py is run on the DOL, which is checked
# against the SHA-1 of the USA release first, so a PAL or damaged dump stops here instead of producing an APK that
# crashes on the phone.
#
# Exit status: 0 when generated/files.cmake exists afterwards, 1 with a ::error:: line explaining why otherwise.
set -uo pipefail

url=${1:-}
if [ -z "$url" ]; then
  echo "usage: fetch_game_code.sh <link or local path>" >&2
  exit 2
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work=${SHADOW_FETCH_DIR:-/tmp/shadow-gamecode}
rm -rf "$work" && mkdir -p "$work"
blob="$work/download.bin"

# --- 1. get the bytes -----------------------------------------------------------------------------------------------
case "$url" in
  http://*|https://*) curl -fsSL --retry 3 --retry-delay 2 -o "$blob" "$url" ;;
  *) cp "$url" "$blob" ;;
esac
if [ ! -s "$blob" ]; then
  echo "::error ::nothing was downloaded from that link (empty response)"
  exit 1
fi
echo "downloaded $(stat -c%s "$blob" 2>/dev/null || stat -f%z "$blob") bytes"

# A Hugging Face *file page* (…/blob/main/…) answers with HTML, not with the file. Say that plainly.
if head -c 512 "$blob" | grep -qiE '<!doctype html|<html'; then
  echo "::error ::that link is a web page, not a file. On Hugging Face use the direct link: the Download /"
  echo "::error ::raw button, or .../resolve/main/<file name> in place of .../blob/main/<file name>"
  exit 1
fi

# --- 2. unpack it if it is an archive -------------------------------------------------------------------------------
src="$work/content"
mkdir -p "$src"
if tar -tzf "$blob" >/dev/null 2>&1; then
  echo "archive: tar.gz";  tar -xzf "$blob" -C "$src"
elif tar -tJf "$blob" >/dev/null 2>&1; then
  echo "archive: tar.xz";  tar -xJf "$blob" -C "$src"
elif unzip -tq "$blob" >/dev/null 2>&1; then
  echo "archive: zip";     unzip -q "$blob" -d "$src"
else
  # Not an archive. Compressed disc images are recognised first, so the message is the real one instead of
  # "this main.dol is not the USA release".
  kind=$(python3 - "$blob" <<'PY'
import sys
head = open(sys.argv[1], 'rb').read(0x440)
if head[:4] in (b'RVZ\x01', b'WIA\x01') or head[:4] == b'\x01\xc0\x0b\xb1':
    print('compressed')
elif len(head) >= 0x440 and head[0x1C:0x20] == b'\xc2\x33\x9f\x3d':
    print('iso')
else:
    print('file')
PY
)
  case "$kind" in
    compressed)
      echo "::error ::that is a Dolphin-compressed image (RVZ/WIA/GCZ). Convert it once - in Dolphin,"
      echo "::error ::right-click the game > Convert File... > Format: ISO - and link the ISO instead."
      exit 1
      ;;
    iso)
      echo "disc image: an ISO/GCM, so only the DOL inside it is taken"
      cp "$blob" "$src/game.iso"
      ;;
    *)
      echo "single file: treating it as main.dol"
      mkdir -p "$src/sys" && cp "$blob" "$src/sys/main.dol"
      ;;
  esac
fi

# --- 3. find what was uploaded --------------------------------------------------------------------------------------
gen_cmake=$(find "$src" -maxdepth 6 -path '*/generated/files.cmake' | head -1 || true)
dol=$(find "$src" -maxdepth 6 -name main.dol | head -1 || true)
iso=$(find "$src" -maxdepth 3 \( -iname '*.iso' -o -iname '*.gcm' \) | head -1 || true)

# An ISO is the friendliest thing to upload (it is what the phone wants too), but this build needs one file out of it.
if [ -z "$dol" ] && [ -n "$iso" ]; then
  echo "extracting sys/main.dol from $iso"
  python3 "$root/tools/game_import/gameimport.py" extract "$iso" "$work/disc"
  dol="$work/disc/sys/main.dol"
fi

# --- 4. produce generated/ -------------------------------------------------------------------------------------------
cd "$root" || exit 1
if [ -n "$gen_cmake" ]; then
  echo "found recompiled game code in $(dirname "$(dirname "$gen_cmake")")"
  mkdir -p generated && cp -a "$(dirname "$gen_cmake")/." generated/
elif [ -n "$dol" ] && [ -f "$dol" ]; then
  echo "found $dol - recompiling it with tools/gen.py"
  # gen.py reads <disc>/sys/main.dol, so the DOL is put in that shape in one place, wherever it came from.
  mkdir -p "$work/disc/sys"
  if [ "$(cd "$(dirname "$dol")" && pwd)/$(basename "$dol")" != "$(cd "$work/disc/sys" && pwd)/main.dol" ]; then
    cp "$dol" "$work/disc/sys/main.dol"
  fi
  digest=$(python3 -c "
import hashlib, sys
print(hashlib.sha1(open('$work/disc/sys/main.dol', 'rb').read()).hexdigest())
")
  echo "main.dol sha1: $digest"
  if [ "$digest" != "118ef49ebd45b371b6f514e0e1b6d1ba7cf12904" ]; then
    echo "::error ::that main.dol is not the USA release (GUPE8P rev 0) - the game would not run on the phone"
    exit 1
  fi
  SHADOW_DISC="$work/disc" python3 tools/gen.py || exit 1
else
  echo "::error ::the link has neither generated/files.cmake nor a disc image / main.dol in it"
  find "$src" -maxdepth 3 | head -50
  exit 1
fi

if [ ! -f generated/files.cmake ]; then
  echo "::error ::no generated/files.cmake was produced"
  exit 1
fi
echo "game code source files: $(grep -c '\.cpp' generated/files.cmake)"
