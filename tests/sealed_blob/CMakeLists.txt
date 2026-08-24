cmake_minimum_required(VERSION 3.16)
project(mtfs_sealed_blob_tests C CXX)

enable_testing()
find_package(OpenSSL REQUIRED)

set(MTFS_SEALED_CORE
    ../../src/extensions/security/sealed_blob/mtfs_secure_zero.c
    ../../src/extensions/security/sealed_blob/mtfs_sealed_format.c
    ../../src/extensions/security/sealed_blob/mtfs_sealed_blob.c
    ../../src/extensions/ai/model_store/mtfs_model_store.c)

add_library(mtfs_sealed_blob_core STATIC ${MTFS_SEALED_CORE})
target_compile_definitions(mtfs_sealed_blob_core PUBLIC MTFS_ENABLE_SEALED_MODEL=1)
target_include_directories(mtfs_sealed_blob_core PUBLIC
    ../../src ../../src/extensions/security/sealed_blob
    ../../src/extensions/ai/model_store)
target_compile_options(mtfs_sealed_blob_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(mtfs_sealed_blob_core PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)

add_executable(mtfs_sealed_blob_tests test_sealed_blob.c)
target_link_libraries(mtfs_sealed_blob_tests PRIVATE mtfs_sealed_blob_core OpenSSL::Crypto)
target_compile_options(mtfs_sealed_blob_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(mtfs_sealed_blob_tests PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
add_test(NAME mtfs_sealed_blob_core COMMAND mtfs_sealed_blob_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/sealed_model/tests/vectors/golden_package.mtfs
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/sealed_model/tests/vectors/golden_payload.bin
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/sealed_model/tests/vectors/fleet_test.key)

add_library(mtfs_sealed_disabled_compile OBJECT ${MTFS_SEALED_CORE} disabled_headers.c)
target_compile_definitions(mtfs_sealed_disabled_compile PRIVATE MTFS_ENABLE_SEALED_MODEL=0)
target_include_directories(mtfs_sealed_disabled_compile PRIVATE ../../src)
target_compile_options(mtfs_sealed_disabled_compile PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(mtfs_sealed_headers_cpp_compile OBJECT enabled_headers.cpp
    ../../src/fatfs/mtfs_sealed_reader_fatfs.c)
target_compile_definitions(mtfs_sealed_headers_cpp_compile PRIVATE MTFS_ENABLE_SEALED_MODEL=1)
target_include_directories(mtfs_sealed_headers_cpp_compile PRIVATE ../../src)
target_compile_options(mtfs_sealed_headers_cpp_compile PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(mtfs_sealed_headers_cpp_compile PROPERTIES
    CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)

add_library(mtfs_sealed_headers_c_compile OBJECT enabled_headers.c
    ../../src/fatfs/mtfs_sealed_reader_fatfs.c)
target_compile_definitions(mtfs_sealed_headers_c_compile PRIVATE MTFS_ENABLE_SEALED_MODEL=1)
target_include_directories(mtfs_sealed_headers_c_compile PRIVATE ../../src)
target_compile_options(mtfs_sealed_headers_c_compile PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(mtfs_sealed_headers_c_compile PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)

option(MTFS_BUILD_FUZZER "Build the libFuzzer sealed-blob harness" OFF)
if(MTFS_BUILD_FUZZER)
    set(MTFS_SEALED_FUZZ_CORE mtfs_sealed_blob_core)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        add_library(mtfs_sealed_blob_fuzz_core STATIC ${MTFS_SEALED_CORE})
        target_compile_definitions(mtfs_sealed_blob_fuzz_core PUBLIC
            MTFS_ENABLE_SEALED_MODEL=1)
        target_include_directories(mtfs_sealed_blob_fuzz_core PUBLIC
            ../../src ../../src/extensions/security/sealed_blob
            ../../src/extensions/ai/model_store)
        target_compile_options(mtfs_sealed_blob_fuzz_core PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
        set_target_properties(mtfs_sealed_blob_fuzz_core PROPERTIES
            C_STANDARD 99 C_STANDARD_REQUIRED YES)
        set(MTFS_SEALED_FUZZ_CORE mtfs_sealed_blob_fuzz_core)
        add_executable(mtfs_sealed_blob_fuzz fuzz_sealed_blob.c)
    else()
        add_executable(mtfs_sealed_blob_fuzz fuzz_sealed_blob.c fuzz_standalone.c)
        target_compile_definitions(mtfs_sealed_blob_fuzz PRIVATE MTFS_FUZZ_STANDALONE=1)
    endif()
    target_link_libraries(mtfs_sealed_blob_fuzz PRIVATE ${MTFS_SEALED_FUZZ_CORE})
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(mtfs_sealed_blob_fuzz PRIVATE
            -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(mtfs_sealed_blob_fuzz PRIVATE
            -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    endif()
endif()
