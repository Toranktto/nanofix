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
    bench_write.cpp
    bench_primitives.cpp
)
target_link_libraries(nanofix_benchmarks PRIVATE
    nanofix
    benchmark::benchmark
)
target_compile_definitions(nanofix_benchmarks PRIVATE
    NANOFIX_BENCH_DATA_DIR="${NANOFIX_BENCH_DATA_DIR}"
    NANOFIX_ASSERT_FAILFAST  # a tripped assert aborts instead of skewing timings
)

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
