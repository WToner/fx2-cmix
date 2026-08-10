#!/bin/bash -x
set -euo pipefail

#SEED="$1"
#UPDATE_LIMIT="$2"

SEED="923"
UPDATE_LIMIT="3000"

# Toolchain resolution. Upstream hardcodes clang-17 / llvm-profdata-17 / upx-ucl,
# which are installed via install_tools/*.sh (those need root). Resolve whatever
# is actually present instead, and let the caller override via the environment:
#   CMIX_CXX=/opt/llvm17/bin/clang++ ./build_and_construct_comp.sh
CMIX_CXX="${CMIX_CXX:-clang++}"
ROOT=$(pwd)

# llvm-profdata must match the clang major version that emitted the profiles,
# otherwise `merge` rejects the raw profile format.
CLANG_MAJOR="$("$CMIX_CXX" -dumpversion | cut -d. -f1)"
if [ -z "${PROFDATA:-}" ]; then
  for cand in "llvm-profdata-$CLANG_MAJOR" \
              "/usr/lib/llvm-$CLANG_MAJOR/bin/llvm-profdata" \
              "$(dirname "$(command -v "$CMIX_CXX")")/llvm-profdata"; do
    if command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ]; then
      PROFDATA="$cand"; break
    fi
  done
fi
if [ -z "${PROFDATA:-}" ]; then
  echo "error: no llvm-profdata matching $CMIX_CXX (major $CLANG_MAJOR)" >&2
  exit 1
fi

if [ -z "${UPX:-}" ]; then
  for cand in upx upx-ucl "$ROOT"/upx-*/upx; do
    if command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ]; then
      UPX="$cand"; break
    fi
  done
fi
if [ -z "${UPX:-}" ]; then
  echo "error: no upx found (tried upx, upx-ucl, ./upx-*/upx)" >&2
  exit 1
fi

# Banner only; llvm-profdata exits non-zero on --version, hence the guards.
"$CMIX_CXX" --version | head -1 || true
"$PROFDATA" show --version 2>&1 | head -2 || true
"$UPX" --version | head -1 || true

CFLAGS_DEFINES="-DSEED=$SEED -DUPDATE_LIMIT=$UPDATE_LIMIT ${EXTRA_DEFINES:-}"

if [ "${SKIP_PGO:-0}" = "1" ]; then
  # Plain -O3 build. Same compressed output, slower binary; useful when the
  # ~15 min instrumented training run is not worth it.
  make CMIX_CXX="$CMIX_CXX" CFLAGS_DEFINES="$CFLAGS_DEFINES" cmix -j
else
  rm -rf pgo_data
  mkdir -p pgo_data

  # building with PGO
  make CMIX_CXX="$CMIX_CXX" CFLAGS_DEFINES="$CFLAGS_DEFINES" prof_gen -j

  ./cmix -c ./prof_input/input ./prof_comp > ./prof_output
  rm ./prof_comp ./prof_output
  "$PROFDATA" merge -output=default.profdata ./pgo_data/*
  mv default.profdata pgo_data/

  make CMIX_CXX="$CMIX_CXX" CFLAGS_DEFINES="$CFLAGS_DEFINES" prof_use -j
fi

[ "${SKIP_UPX:-0}" = "1" ] || "$UPX" -9 cmix

# The self-extracting archive costs two full cmix runs (~10 min) and is only
# needed to measure S1 or to run `cmix -e`. Development builds skip it.
if [ "${SKIP_SELFEXTRACT:-0}" = "1" ]; then
  echo "SKIP_SELFEXTRACT=1: leaving ./cmix as a standalone binary"
  exit 0
fi

# this is a directory where the compressor binary will be placed
DIR=run
mkdir -p ./$DIR
cp ./cmix $DIR/cmix_orig
# git diff > $DIR/patch
# exit
# building a selfextracting binary
pushd $DIR
# creating a compressed version of dictionary
./cmix_orig -c $ROOT/dictionary/english.dic ./comp_dict
# creating a compressed verions of a file with new order
./cmix_orig -c $ROOT/src/readalike_prepr/data/new_article_order ./comp_order
# creating a header with size of the above files
./cmix_orig -h $(wc -c ./comp_dict | awk '{print $1}') $(wc -c ./comp_order | awk '{print $1}') 0

# merging the above files and setting permissions for the final executable file
cat ./cmix_orig ./comp_dict ./comp_order header.dat > ./cmix
chmod +x ./cmix
popd
