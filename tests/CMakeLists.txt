find_package(GTest REQUIRED)

add_executable(tests
    unit_tests.cpp
    typed_tests.cpp
    state_machine_tests.cpp
    integration_tests.cpp
    framing_diff_tests.cpp
    threading_tests.cpp
)
target_link_libraries(tests PRIVATE nanofix GTest::gtest GTest::gtest_main)
target_compile_definitions(tests PRIVATE
    NANOFIX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

include(GoogleTest)
gtest_discover_tests(tests)
