#!/usr/bin/env bash
# Builds the Vita graphics libraries gate 11's GPU view links (vita_gl3d.cpp):
# vitaGL (OpenGL on the Vita's GPU) and what it needs -- vitaShaRK (the shader
# compiler front end, which uses the system's libshacccg.suprx at run time),
# SceShaccCgExt and math-neon. Each is fetched at a fixed commit into
# build/vita-deps/src and built into build/vita-deps/prefix; nothing is
# installed into VitaSDK. A library is rebuilt only when its version or flags
# change, so running this every time costs nothing.
#
# The versions and vitaGL's options are the ones the Super Mario Sunshine Vita
# port uses (github.com/JackRuss9/sms-vita, vita/deps.sh), which has run them
# on the console.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${VITASDK:?set VITASDK first}"
export PATH="$VITASDK/bin:$PATH"

D=$PWD/build/vita-deps
P=$D/prefix
JOBS=${JOBS:-$(nproc)}
mkdir -p "$D/src" "$P/lib" "$P/include"

fetch() {   # name url commit
  local name=$1 url=$2 commit=$3 dir=$D/src/$1
  if [[ ! -d $dir/.git ]]; then
    echo "== Downloading $name"
    git clone -q "$url" "$dir"
  fi
  if [[ "$(git -C "$dir" rev-parse HEAD)" != "$commit" ]]; then
    git -C "$dir" fetch -q origin "$commit" 2>/dev/null || git -C "$dir" fetch -q origin
    git -C "$dir" checkout -q --force "$commit"
    git -C "$dir" clean -q -fdx
  fi
}
up_to_date() { [[ "$(cat "$P/.$1" 2>/dev/null || true)" == "$2" ]]; }
done_build() { echo "$2" > "$P/.$1"; }

CC="arm-vita-eabi-gcc -I$P/include"
CXX="arm-vita-eabi-g++ -I$P/include"
AR=arm-vita-eabi-gcc-ar

c=102192366c25fe7b8fec073d665494b6172605c3
fetch SceShaccCgExt https://github.com/bythos14/SceShaccCgExt.git $c
if ! up_to_date SceShaccCgExt "$c"; then
  echo "== Building SceShaccCgExt"
  rm -rf "$D/src/SceShaccCgExt/build"
  cmake -S "$D/src/SceShaccCgExt" -B "$D/src/SceShaccCgExt/build" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" -DCMAKE_INSTALL_PREFIX="$P" \
    -DCMAKE_C_FLAGS="-std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types" \
    -DCMAKE_CXX_FLAGS="-std=gnu++11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types" >/dev/null
  cmake --build "$D/src/SceShaccCgExt/build" -j "$JOBS" >/dev/null
  cmake --install "$D/src/SceShaccCgExt/build" >/dev/null
  done_build SceShaccCgExt "$c"
fi

c=0faab814782c071ff4015527f1ca955ab1ccc470
fetch math-neon https://github.com/Rinnegatamante/math-neon.git $c
if ! up_to_date math-neon "$c"; then
  echo "== Building math-neon"
  make -C "$D/src/math-neon" -j "$JOBS" CC="$CC" AR="$AR" >/dev/null
  cp "$D/src/math-neon/libmathneon.a" "$P/lib/"
  cp "$D/src/math-neon/source/math_neon.h" "$P/include/"
  done_build math-neon "$c"
fi

c=df24065e65098b2d1ac533760109ad4367573f28
fetch vitaShaRK https://github.com/Rinnegatamante/vitaShaRK.git $c
if ! up_to_date vitaShaRK "$c"; then
  echo "== Building vitaShaRK"
  make -C "$D/src/vitaShaRK" -j "$JOBS" CC="$CC" AR="$AR" >/dev/null
  cp "$D/src/vitaShaRK/libvitashark.a" "$P/lib/"
  cp "$D/src/vitaShaRK/source/vitashark.h" "$P/include/"
  done_build vitaShaRK "$c"
fi

# vitaGL: shader compiler messages, compiled shaders cached on the memory card
c=7635e3ceb3c52e34ca2e51d2281628d6c756a35b
flags="HAVE_SHARK_LOG=1 HAVE_SHADER_CACHE=1 NO_DEBUG=1"
fetch vitaGL https://github.com/Rinnegatamante/vitaGL.git $c
if ! up_to_date vitaGL "$c $flags"; then
  echo "== Building vitaGL ($flags)"
  git -C "$D/src/vitaGL" clean -q -fdx
  # shellcheck disable=SC2086
  make -C "$D/src/vitaGL" -j "$JOBS" CC="$CC" CXX="$CXX" AR="$AR" $flags >/dev/null
  cp "$D/src/vitaGL/libvitaGL.a" "$P/lib/"
  cp "$D/src/vitaGL/source/vitaGL.h" "$P/include/"
  done_build vitaGL "$c $flags"
fi
echo "== Vita graphics libraries ready (build/vita-deps/prefix)"
