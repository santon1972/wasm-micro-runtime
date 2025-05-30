# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Add more test files here if needed
set(COMPONENT_MODEL_TEST_SOURCES
    canonical_section_test.cc
    core_type_parsing_test.cc
    core_instance_linking_test.cc
)

# Link with common test utilities and gtest
target_link_libraries(wamr-unit-tests
    ${GTEST_LIBRARIES}
    ${UNIT_TEST_COMMON_SOURCES}
    # Add other specific dependencies if needed, e.g., if canonical section parsing
    # depends on other compiled parts of iwasm not in common sources.
    # For now, assuming wasm_component_loader.c and wasm_component_canonical.c
    # will be part of a broader library target or directly compiled.
    # If they are part of iwasm_core, that should be linked.
    # For simplicity in this example, we assume they are accessible.
)

# Add tests to be run
foreach(test_source ${COMPONENT_MODEL_TEST_SOURCES})
    # Extract the test name from the source file name (e.g., "my_test.cc" -> "my_test")
    string(REPLACE ".cc" "" test_name ${test_source})
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PUBLIC wamr-unit-tests)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
