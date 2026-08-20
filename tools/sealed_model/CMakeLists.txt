cmake_minimum_required(VERSION 3.16)
project(mtfs_sealed_model_host VERSION 1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "sealed model host tools support Linux and WSL2 only")
endif()

option(MTFS_SEALED_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MTFS_SEALED_WARNINGS_AS_ERRORS "Treat host-tool warnings as errors" ON)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

add_library(mtfs_sealed_host STATIC
    src/mtfs_sealed_host.cpp
)
target_include_directories(mtfs_sealed_host PUBLIC include)
target_link_libraries(mtfs_sealed_host PUBLIC OpenSSL::Crypto)
target_compile_features(mtfs_sealed_host PUBLIC cxx_std_17)

function(mtfs_sealed_set_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(MTFS_SEALED_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(MTFS_SEALED_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

mtfs_sealed_set_warnings(mtfs_sealed_host)

foreach(tool IN ITEMS keygen seal verify unseal)
    add_executable(mtfs-${tool} src/mtfs_${tool}.cpp)
    target_link_libraries(mtfs-${tool} PRIVATE mtfs_sealed_host)
    mtfs_sealed_set_warnings(mtfs-${tool})
endforeach()

add_executable(mtfs-test-package src/mtfs_test_package.cpp)
target_link_libraries(mtfs-test-package PRIVATE mtfs_sealed_host)
mtfs_sealed_set_warnings(mtfs-test-package)

include(CTest)
if(BUILD_TESTING)
    add_executable(mtfs_sealed_tests tests/test_sealed_model.cpp)
    target_link_libraries(mtfs_sealed_tests PRIVATE mtfs_sealed_host)
    target_compile_definitions(mtfs_sealed_tests PRIVATE
        MTFS_SEALED_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
    mtfs_sealed_set_warnings(mtfs_sealed_tests)
    add_test(NAME mtfs_sealed_model COMMAND mtfs_sealed_tests)
endif()

message(STATUS "microT-FS sealed-model Host crypto: OpenSSL ${OPENSSL_VERSION}")
