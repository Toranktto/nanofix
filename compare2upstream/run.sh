#!/usr/bin/env bash
# Bench N configurations and render comparison tables.
# Env: NANOFIX_UPSTREAM_URL, NANOFIX_UPSTREAM_REF, NANOFIX_BENCH_MIN_TIME (1s),
#      NANOFIX_BENCH_REPETITIONS (5),
#      COMPARE2UPSTREAM_DIR (default: <repo>/compare2upstream),
#      PYTHON (default: python3), CONAN (default: conan).

set -euo pipefail

ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)

BASE=${COMPARE2UPSTREAM_DIR:-${ROOT}/compare2upstream}
BUILD=${BASE}/build
UPSTREAM_SRC=${BASE}/upstream-src
RESULTS_DIR=${BASE}/results
CONAN_DIR=${BUILD}/conan-base

PYTHON_BIN=${PYTHON:-python3}
CONAN_BIN=${CONAN:-conan}

UPSTREAM_URL=${NANOFIX_UPSTREAM_URL:-https://github.com/jamesdbrock/hffix.git}
UPSTREAM_REF=${NANOFIX_UPSTREAM_REF:-}
MIN_TIME=${NANOFIX_BENCH_MIN_TIME:-1s}
REPS=${NANOFIX_BENCH_REPETITIONS:-5}
DATA_DIR=${ROOT}/benchmarks/data
mkdir -p "${BUILD}" "${RESULTS_DIR}"

if ! find "${CONAN_DIR}" -name conan_toolchain.cmake -print -quit 2> /dev/null | grep -q .; then
    if ! command -v "${CONAN_BIN}" > /dev/null; then
        echo "error: '${CONAN_BIN}' not in PATH; install Conan 2, set CONAN=<path>, or set COMPARE2UPSTREAM_DIR to a tree that already contains a conan-base toolchain" >&2
        exit 1
    fi
    echo ">> running conan install -> ${CONAN_DIR}" >&2
    "${CONAN_BIN}" install "${ROOT}" \
        --output-folder "${CONAN_DIR}" \
        --build=missing \
        --settings=build_type=Release > /dev/null
fi

# conan's cmake_layout nests the toolchain under build/<cfg>/generators; resolve
# wherever it landed instead of assuming a flat conan-base/conan_toolchain.cmake.
CONAN_TC=$(find "${CONAN_DIR}" -name conan_toolchain.cmake -print -quit 2> /dev/null)
[[ -f ${CONAN_TC} ]] || {
    echo "error: conan_toolchain.cmake not found under ${CONAN_DIR}" >&2
    exit 1
}

if [[ ! -d ${UPSTREAM_SRC}/.git ]]; then
    echo ">> cloning ${UPSTREAM_URL} -> ${UPSTREAM_SRC}" >&2
    rm -rf "${UPSTREAM_SRC}"
    if [[ -n ${UPSTREAM_REF} ]]; then
        git clone --depth 1 --branch "${UPSTREAM_REF}" "${UPSTREAM_URL}" "${UPSTREAM_SRC}"
    else
        git clone --depth 1 "${UPSTREAM_URL}" "${UPSTREAM_SRC}"
    fi
fi
UPSTREAM_INC=${UPSTREAM_SRC}/include
[[ -f ${UPSTREAM_INC}/hffix.hpp ]] || { echo "error: ${UPSTREAM_INC}/hffix.hpp missing" >&2; exit 1; }

# Schema: label|source_dir|exec_relpath|extra_cmake_args
CONFIGS=(
    "upstream|${BASE}/upstream-benchmarks|upstream_benchmarks|-DNANOFIX_UPSTREAM_INCLUDE_DIR=${UPSTREAM_INC}"
    "fork|${ROOT}|benchmarks/nanofix_benchmarks|"
    "fork-no-simd|${ROOT}|benchmarks/nanofix_benchmarks|-DNANOFIX_DISABLE_SIMD=ON"
)

EXE_SUFFIX=""
[[ ${OS:-} == Windows_NT ]] && EXE_SUFFIX=.exe

ensure_configured() {
    local build_dir=$1
    local source_dir=$2
    local extra=$3
    if [[ ! -f ${build_dir}/CMakeCache.txt ]]; then
        # shellcheck disable=SC2086
        cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="${CONAN_TC}" \
            -DCMAKE_BUILD_TYPE=Release \
            ${extra} > /dev/null
    fi
}

if ! ls "${DATA_DIR}"/*.fix > /dev/null 2>&1; then
    echo ">> generating dataset (.fix)" >&2
    ensure_configured "${BUILD}/fork" "${ROOT}" ""
    cmake --build "${BUILD}/fork" --target bench_data >&2
fi

BENCH_FILTER='^(BM_WriteNewOrder|BM_Write_TailLatency|BM_Parse_(Sequential|Random)_(Iter|Indexed)|BM_Parse_TailLatency_(Sequential|Random)_(Iter|Indexed)|BM_Parse_FindN_(Iter|Indexed))(/.*)?$'

BENCH_ARGS=(
    --benchmark_min_time="${MIN_TIME}"
    --benchmark_repetitions="${REPS}"
    --benchmark_report_aggregates_only=true
    --benchmark_enable_random_interleaving=true
    --benchmark_filter="${BENCH_FILTER}"
    --benchmark_format=json
)

PIN=()
if [[ -n ${NANOFIX_BENCH_CPU:-} ]]; then
    if [[ $(uname -s) == Linux ]] && command -v taskset > /dev/null; then
        PIN=(taskset -c "${NANOFIX_BENCH_CPU}")
        echo ">> pinning benchmarks to cpu ${NANOFIX_BENCH_CPU} (taskset)" >&2
    else
        echo ">> NANOFIX_BENCH_CPU set but taskset/Linux unavailable; running unpinned" >&2
    fi
fi

RENDER_ARGS=()
for cfg in "${CONFIGS[@]}"; do
    IFS='|' read -r label source_dir exec_relpath extra <<< "${cfg}"
    build_dir="${BUILD}/${label}"
    target="${exec_relpath##*/}"
    json="${RESULTS_DIR}/${label}.json"

    echo ">> ${label}" >&2
    ensure_configured "${build_dir}" "${source_dir}" "${extra}"
    cmake --build "${build_dir}" --target "${target}" > /dev/null
    ${PIN[@]+"${PIN[@]}"} "${build_dir}/${exec_relpath}${EXE_SUFFIX}" "${BENCH_ARGS[@]}" > "${json}"
    RENDER_ARGS+=("${label}=${json}")
done

PINNED_NOTE="unpinned"
[[ ${#PIN[@]} -gt 0 ]] && PINNED_NOTE="pinned to cpu ${NANOFIX_BENCH_CPU}"
HEADER="# Benchmark overview — $(uname -m)

> $(uname -sr), $(uname -m), ${PINNED_NOTE}. Generated $(date +%Y-%m-%d) by
> \`compare2upstream/run.sh\` (\`min_time=${MIN_TIME}\`,
> \`repetitions=${REPS}\`; cells are means over repetitions). \`fork\` is
> nanofix; \`fork-no-simd\` is the same code with \`NANOFIX_DISABLE_SIMD\`;
> \`upstream\` is jamesdbrock/hffix."

# Tables go to stdout; everything else this script prints is on stderr, so
# the committed snapshot is `compare2upstream/run.sh > X86_64.md`.
"${PYTHON_BIN}" "${BASE}/render.py" "${RENDER_ARGS[@]}" \
    --header "${HEADER}"
