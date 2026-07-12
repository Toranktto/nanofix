find_package(benchmark REQUIRED)

set(NANOFIX_BENCH_DATA_DIR "${CMAKE_SOURCE_DIR}/benchmarks/data"
    CACHE PATH "Directory holding .fix datasets for nanofix_benchmarks")
set(NANOFIX_BENCH_MESSAGES 500000
    CACHE STRING "Number of messages in the synthetic dataset for bench_data")
set(NANOFIX_BENCH_REPETITIONS 5
    CACHE STRING "Repetitions passed to Google Benchmark via the `bench` target")
set(NANOFIX_BENCH_MIN_TIME "1s"
    CACHE STRING "--benchmark_min_time argument for the `bench` target")

add_executable(nanofix_benchmarks
    bench_parse.cpp
    bench_parse_hot_iter.cpp
    bench_parse_hot_indexed.cpp
    bench_write.cpp
    bench_primitives.cpp
)
# The overview-table iterator and indexed sweeps live in tiny non-LTO TUs:
# inside the LTO blob, inline budget and whole-binary layout couple them to
# unrelated code (iter loops -18-19% next to the indexed AVX2 bodies; indexed
# sweeps drifting ~2-3% on header changes they never execute).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(bench_parse_hot_iter.cpp bench_parse_hot_indexed.cpp
        PROPERTIES COMPILE_OPTIONS "-fno-lto")
endif()
target_link_libraries(nanofix_benchmarks PRIVATE
    nanofix
    benchmark::benchmark
)
target_compile_definitions(nanofix_benchmarks PRIVATE
    NANOFIX_BENCH_DATA_DIR="${NANOFIX_BENCH_DATA_DIR}"
    NANOFIX_ASSERT_FAILFAST  # a tripped assert aborts instead of skewing timings
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/nanofix-jcc.cmake)
    nanofix_add_jcc_mitigation(nanofix_benchmarks)
endif()
# 32B function/loop alignment damps the layout lottery between binaries
# built from slightly different headers; the upstream suite carries the same.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nanofix_benchmarks PRIVATE
        -falign-functions=32 -falign-loops=32)
endif()

add_custom_target(bench_data
    COMMAND ${CMAKE_COMMAND} -E make_directory ${NANOFIX_BENCH_DATA_DIR}
    COMMAND $<TARGET_FILE:fixgen>
            -o ${NANOFIX_BENCH_DATA_DIR}/synthetic.fix
            -n ${NANOFIX_BENCH_MESSAGES}
    DEPENDS fixgen
    USES_TERMINAL
    COMMENT "Regenerating ${NANOFIX_BENCH_DATA_DIR}/synthetic.fix"
    VERBATIM
)

add_custom_target(bench
    COMMAND $<TARGET_FILE:nanofix_benchmarks>
            --benchmark_min_time=${NANOFIX_BENCH_MIN_TIME}
            --benchmark_repetitions=${NANOFIX_BENCH_REPETITIONS}
            --benchmark_report_aggregates_only=true
            --benchmark_color=false
    DEPENDS nanofix_benchmarks
    USES_TERMINAL
    COMMENT "Running nanofix benchmarks (uses existing ${NANOFIX_BENCH_DATA_DIR}/synthetic.fix)"
)
