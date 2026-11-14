cmake_minimum_required(VERSION 3.20)
project(oscairn_va VERSION 0.7.0 LANGUAGES C CXX)

# The binary's name (the release stem is NAME in tools/release/release.conf).
set(VA_NAME oscairn-va)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The DSP core: a plain C++ library with no plugin API, OS, threads, files or heap (core/vcore.h).
add_library(vcore STATIC core/core.cpp core/laws.cpp core/patch.cpp)
target_include_directories(vcore PUBLIC core)
# The kernels' code layout (core/core.cpp VC_LAYOUT, docs/cost.md): 0 shared control step and filters (smallest code, the
# default), 1 the hot plans inlined, 2 everything inlined.
set(VA_KERNEL_LAYOUT 0 CACHE STRING "Kernel code layout: 0 shared, 1 hot plans inlined, 2 all inlined")
set_property(CACHE VA_KERNEL_LAYOUT PROPERTY STRINGS 0 1 2)
target_compile_definitions(vcore PRIVATE VC_LAYOUT=${VA_KERNEL_LAYOUT})
if(MSVC)
    target_compile_options(vcore PRIVATE /W4)
else()
    target_compile_options(vcore PRIVATE -Wall -Wextra)
endif()

add_library(clap_headers INTERFACE)
target_include_directories(clap_headers INTERFACE third_party/clap/include third_party/oscairn-clap/include)

add_library(oscairn_va MODULE src/plugin.cpp src/core_engine.cpp)
target_link_libraries(oscairn_va PRIVATE vcore)
target_link_libraries(oscairn_va PRIVATE clap_headers)
if(NOT MSVC)
    target_compile_options(oscairn_va PRIVATE -Wall -Wextra)
endif()
set_target_properties(oscairn_va PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME ${VA_NAME}
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT APPLE AND NOT MSVC)
    target_compile_options(oscairn_va PRIVATE -march=x86-64-v2)  # the baseline of the device images, at most
endif()
install(TARGETS oscairn_va LIBRARY DESTINATION lib/clap)

# The core's own tests link the core (no plugin): laws, plans, voices, cross-build statistics.
enable_testing()
add_executable(vcore_tests tests/core/test_core.cpp)
target_link_libraries(vcore_tests PRIVATE vcore)
add_test(NAME core COMMAND vcore_tests)
if(WIN32)
    return()  # shortcut: the ABI tests (dlopen) and the bench (POSIX clocks) are not ported; on Windows only the core's tests build
endif()
# The bench, cost and render driver (tools/vcbench.cpp; tools/cost/cg.sh runs it under callgrind).
add_executable(vcbench tools/vcbench.cpp)
target_link_libraries(vcbench PRIVATE vcore)
# The hosted cost driver (tools/hostbench.cpp): the built .clap through the CLAP ABI, as the host drives it.
add_executable(hostbench tools/hostbench.cpp)
target_link_libraries(hostbench PRIVATE clap_headers ${CMAKE_DL_LIBS})

# The tests see only the built .clap, through the CLAP ABI (dlopen); they link no plugin code.
add_executable(va_tests tests/test_main.cpp tests/alloc_guard.cpp tests/test_notes.cpp tests/test_budget.cpp
    tests/test_state.cpp tests/test_contexts.cpp tests/test_alloc.cpp tests/test_threads.cpp
    tests/test_fuzz.cpp tests/test_controls.cpp)
target_link_libraries(va_tests PRIVATE clap_headers ${CMAKE_DL_LIBS})
if(APPLE)
    # Counts allocations in every image of the process, the plugin's included (alloc_guard.cpp).
    add_library(va_alloc_interpose SHARED tests/alloc_interpose.c)
    target_link_libraries(va_tests PRIVATE va_alloc_interpose)
endif()
add_dependencies(va_tests oscairn_va)
add_test(NAME abi COMMAND va_tests $<TARGET_FILE:oscairn_va>)
