#!/usr/bin/env bash
#
# mayhem/build.sh — build this repo's fuzz harness(es). EDIT per repo.
#
# Runs inside the commit image (mayhem/Dockerfile) as `mayhem` in /mayhem. The base image
# (ghcr.io/savantenvs/base) already exports the build contract — use these, don't redefine:
#   CC, CXX             stock clang / clang++
#   LIB_FUZZING_ENGINE  -fsanitize=fuzzer   (link into each harness that has a LLVMFuzzer entry)
#   SANITIZER_FLAGS     -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
#                       (ASan + UBSan, both set to HALT — so Mayhem catches memory AND UB defects)
#   DEBUG_FLAGS         -g -gdwarf-3   (DWARF debug info — always on for fuzz/standalone builds,
#                       independent of the sanitizer off-switch; DWARF version must be < 4)
#   RUST_DEBUG_FLAGS    -C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3
#                       (thread through RUSTFLAGS on every cargo-fuzz build)
#   GO_DEBUG_FLAGS      -gcflags=all=-N -l
#                       (thread through go build / go-fuzz-build so the linked ELF keeps symbols)
#   SRC                 /mayhem (the repo source)
#
# Contract: build EVERYTHING here — one runnable binary per fuzz harness, AND the project's test
# suite (so mayhem/test.sh only has to RUN it, never compile). Keep it ADDITIVE (build upstream as
# upstream documents; don't edit upstream files). IMPORTANT: build the PROJECT ITSELF with
# $SANITIZER_FLAGS and $DEBUG_FLAGS (not just the harness) so the fuzzed code is instrumented
# AND carries DWARF < 4 symbols — otherwise ASan/UBSan only see the harness, not the library
# you're trying to find bugs in, and backtraces won't resolve project source lines. Build the TEST
# suite with the project's NORMAL flags (a clean, independent build) so test.sh stays an honest
# functional oracle and won't false-fail on benign UB. Leave the test binary/runner where test.sh
# expects it.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

# Build knobs come from the ENVIRONMENT (overridable), with sane defaults — no if-statements,
# just parameter-expansion fallbacks. Default sanitizers are the base's ASan+UBSan (halting); override
# per build via the Dockerfile's `--build-arg SANITIZER_FLAGS="..."`.
# NB: SANITIZER_FLAGS uses `=` (no colon) on purpose — `=` only fills when the var is UNSET, so an
# explicit EMPTY value (`--build-arg SANITIZER_FLAGS=`) is honored and builds with NO sanitizers
# (useful when you want the program's natural crash / full backtrace, not an ASan report). The other
# knobs use `:=` (default on empty too). MAYHEM_JOBS sets build parallelism (falls back to nproc).
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
# DEBUG_FLAGS carries DWARF debug info INDEPENDENTLY of the sanitizer off-switch (so an empty
# SANITIZER_FLAGS still yields DWARF symbols). DWARF MUST be < 4 (Mayhem triage can't read >=4); clang-19's
# plain `-g` emits DWARF-5, so `-gdwarf-3` is explicit. Apply $DEBUG_FLAGS to the fuzz/harness/standalone
# builds (NOT the test/oracle build). Rust/Go carry the same intent via their language flags below.
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${RUST_DEBUG_FLAGS:=-C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3}"
: "${GO_DEBUG_FLAGS:=-gcflags=all=-N -l}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
# COVERAGE_FLAGS: empty by default → no effect on the normal oracle build. Set it via the Dockerfile's
# `--build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping"` to instrument the TEST
# build for source-coverage measurement (how much of the project the test suite actually exercises —
# a quality signal for the oracle; complements the anti-reward-hack sabotage check). APPEND it to the
# test build's compile+link flags in step 3 (NOT the fuzz build); after `test.sh` runs, merge with
# `llvm-profdata` and report with `llvm-cov`. Empty value is honored (`=`, not `:=`).
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS RUST_DEBUG_FLAGS GO_DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

: "${CARGO_HOME:=/opt/toolchains/rust/cargo}"
export CARGO_HOME

# UBSan's float-cast-overflow is relaxed (and only it): tinyexpr's fac() casts NaN to unsigned and
# Matcher result formatting casts out-of-range floats, so it would end every run within seconds
# and hide everything behind it. ASan and the rest of UBSan stay halting.
SANITIZER_FLAGS="$SANITIZER_FLAGS -fno-sanitize=float-cast-overflow"

HARNESSES="fuzz_kseq fuzz_expression fuzz_orf fuzz_align"
ZSTD_TARGETS="simple_decompress stream_decompress block_decompress"

# CMake discards its cache when the compiler is passed by name (clang++) over a cached absolute
# path, and the regenerated tree then lacks the test targets, so always start from clean build dirs.
rm -rf build build-normal

# 1) Sanitized project build. -fsanitize=fuzzer-no-link gives the library libFuzzer coverage
#    edges (without it Mayhem reports edges=0). The AVX2 baseline is pinned instead of
#    -march=native so the binaries do not depend on the build host's CPU.
FUZZ_C_FLAGS="$SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS"
export RUSTFLAGS="-C debuginfo=0 -C force-frame-pointers=yes"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="$FUZZ_C_FLAGS" -DCMAKE_CXX_FLAGS="$FUZZ_C_FLAGS" \
    -DNATIVE_ARCH=0 -DHAVE_AVX2=1 -DAVX512_DISPATCH=OFF -DHAVE_TESTS=0
cmake --build build -j"$MAYHEM_JOBS" --target mmseqs-framework version

# 2) Harnesses: each one twice, against libFuzzer and against the run-once standalone driver.
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o build/standalone_main.o
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o build/lsan_off.o

INCLUDES="-Ilib/block-aligner/c -Ilib/zstd/lib -Ilib/tinyexpr -Ilib/microtar -Ilib/tantan -Ilib/simde -Ilib
 -Ilib/simd -Ilib/alp -Ilib/ksw2 -Ilib/xxhash -Ilib/ips4o -Ibuild/generated -Isrc/alignment -Isrc/clustering
 -Isrc/commons -Isrc/multihit -Isrc/prefiltering -Isrc/linclust -Isrc/taxonomy -Isrc/util -Isrc"
DEFS="-DENABLE_IPS4O=1 -DHAVE_BZLIB=1 -DHAVE_POSIX_FADVISE=1 -DHAVE_POSIX_MADVISE=1 -DHAVE_ZLIB=1 -DOPENMP=1"
CXXFLAGS_H="-O2 -DNDEBUG -fsigned-char -mavx2 -mfma -mcx16 -std=c++1y -fno-exceptions -fopenmp=libomp"
libs_for() {
    local b="$1"
    echo "$b/src/libmmseqs-framework.a $b/src/version/libversion.a -latomic $b/lib/tinyexpr/libtinyexpr.a
 $b/lib/zstd/build/cmake/lib/libzstd.a $b/lib/microtar/libmicrotar.a $b/lib/tantan/libtantan.a
 $b/libblock_aligner_c.a -lutil -lrt -lpthread -lm -ldl -lc -lz -lbz2 -Wl,-rpath,/usr/lib/llvm-19/lib"
}
# tinyexpr's allocator hands out a te_expr smaller than sizeof(te_expr) for leaf nodes (a
# flexible-array idiom), which trips UBSan's object-size check on every successful parse. Rebuild
# that one file with only that check relaxed and link it ahead of the archive.
$CC $FUZZ_C_FLAGS -fno-sanitize=object-size -fPIC -O2 -fsigned-char -Ilib/tinyexpr -c lib/tinyexpr/tinyexpr.c -o build/tinyexpr-relaxed.o
LIBS="build/tinyexpr-relaxed.o $(libs_for build)"

for h in $HARNESSES; do
    # shellcheck disable=SC2086
    $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $CXXFLAGS_H $DEFS $INCLUDES -c "mayhem/harness/$h.cpp" -o "build/$h.o"
    # shellcheck disable=SC2086
    $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $CXXFLAGS_H $LIB_FUZZING_ENGINE "build/$h.o" build/lsan_off.o $LIBS -o "$SRC/$h"
    # shellcheck disable=SC2086
    $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $CXXFLAGS_H build/standalone_main.o "build/$h.o" build/lsan_off.o $LIBS -o "$SRC/$h-standalone"
done

# zstd's in-tree decompress fuzzers, against a decompress-only libzstd built here with the
# pointer-overflow check relaxed: zstd 1.3.8 does `ptr - offset` arithmetic that trips it on
# any malformed block, which would make every input a crash.
mkdir -p build/zstd-fuzz
ZSTD_OBJS=""
for f in lib/zstd/lib/common/*.c lib/zstd/lib/decompress/*.c; do
    o="build/zstd-fuzz/$(basename "$f").o"
    $CC $FUZZ_C_FLAGS -fno-sanitize=pointer-overflow -O2 -Ilib/zstd/lib -Ilib/zstd/lib/common -c "$f" -o "$o"
    ZSTD_OBJS="$ZSTD_OBJS $o"
done
for t in $ZSTD_TARGETS; do
    name="zstd_$t"
    $CC $FUZZ_C_FLAGS -O2 -Ilib/zstd/lib -Ilib/zstd/lib/common -Ilib/zstd/tests/fuzz \
        -c "lib/zstd/tests/fuzz/$t.c" -o "build/zstd-fuzz/$name.o"
    # shellcheck disable=SC2086
    $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "build/zstd-fuzz/$name.o" $ZSTD_OBJS build/lsan_off.o -o "$SRC/$name"
    # shellcheck disable=SC2086
    $CXX $SANITIZER_FLAGS $DEBUG_FLAGS build/standalone_main.o "build/zstd-fuzz/$name.o" $ZSTD_OBJS build/lsan_off.o -o "$SRC/$name-standalone"
done

# 3) Test suite with the project's normal flags (asserts on), separate build dir. RUSTFLAGS from
#    the fuzz build must not leak in.
export RUSTFLAGS=""
cmake -S . -B build-normal -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS" \
    -DNATIVE_ARCH=0 -DHAVE_AVX2=1 -DAVX512_DISPATCH=OFF -DHAVE_TESTS=1
cmake --build build-normal -j"$MAYHEM_JOBS" --target mmseqs-framework version test_tinyexpr test_translate

# shellcheck disable=SC2086
$CXX -g -O0 $COVERAGE_FLAGS $CXXFLAGS_H -UNDEBUG $DEFS ${INCLUDES//-Ibuild\/generated/-Ibuild-normal\/generated} \
    mayhem/selftest.cpp $(libs_for build-normal) -o "$SRC/mayhem_selftest"
