cmake_minimum_required(VERSION 3.16)
project(mtfs_host_tests C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "microT-FS host tests support Linux and WSL2 only")
endif()

enable_testing()
find_package(Threads REQUIRED)

set(MTFS_HOST_FF_USE_LFN "2" CACHE STRING
    "FatFs LFN mode for the host test executable (0 or 2)")
set(MTFS_HOST_FF_MAX_LFN "64" CACHE STRING
    "FatFs maximum LFN length in UTF-16 code units")
set(MTFS_HOST_FF_CODE_PAGE "437" CACHE STRING
    "FatFs OEM code page for host tests")

message(STATUS
    "microT-FS FatFs: LFN=${MTFS_HOST_FF_USE_LFN} MAX=${MTFS_HOST_FF_MAX_LFN} CODE_PAGE=${MTFS_HOST_FF_CODE_PAGE}")

add_executable(mtfs_host_tests
    main.c
    test_benchmark.c
    test_block_diagnostics.c
    test_media_lifecycle.c
    test_ra_sd_spi_deadline.c
    test_stm32_sdmmc_wait_policy.c
    test_sentinel.c
    test_sealed_reader_fatfs.c
    test_console.c
    test_time_provider.c
    test_wrapped_key_record.c
    ../common/framework/mtfs_test.c
    ../common/benchmarks/mtfs_benchmark.c
    ../common/cases/test_fatfs_concurrent.c
    ../common/cases/test_fatfs_lfn.c
    ../common/cases/test_fatfs_roundtrip.c
    ../common/cases/test_fatfs_timestamp.c
    ../common/console/mtfs_console.c
    ../../src/block/mtfs_block_device.c
    ../../src/block/mtfs_block_diagnostics.c
    ../../src/block/mtfs_block_registry.c
    ../../src/core/mtfs_media.c
    ../../src/core/mtfs_time.c
    ../../src/extensions/security/wrapped_key/mtfs_wrapped_key_record.c
    ../../src/extensions/security/wrapped_key/mtfs_wrapped_key_fatfs.c
    ../../src/fatfs/ff.c
    ../../src/fatfs/ffunicode.c
    ../../src/fatfs/mtfs_diskio.c
    ../../src/fatfs/mtfs_fattime.c
    ../../src/fatfs/mtfs_sealed_reader_fatfs.c
    ../../src/os/host/mtfs_fatfs_mutex_posix.c
    ../../src/os/microtkernel/mtfs_fatfs_mutex.c
    ../../src/ports/host/mtfs_host_block_file.c
    ../../src/ports/ra_fsp/sd_spi/mtfs_ra_sd_spi_deadline.c
    ../../src/ports/stm32_cube/crypto/mtfs_stm32_nor_key_store.c
    ../../src/sentinel/mtfs_sentinel.c
    ../../src/sentinel/mtfs_sentinel_observer.c
)

add_library(mtfs_public_headers_cpp_compile OBJECT test_public_headers.cpp)
target_include_directories(mtfs_public_headers_cpp_compile PRIVATE ../../src)
target_compile_definitions(mtfs_public_headers_cpp_compile PRIVATE
    MTFS_ENABLE_STORAGE_SENTINEL=1)
set_target_properties(mtfs_public_headers_cpp_compile PROPERTIES
    CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
target_compile_options(mtfs_public_headers_cpp_compile PRIVATE
    -Wall -Wextra -Wpedantic)

target_include_directories(mtfs_host_tests PRIVATE
    ../../src
    ../../src/block
    ../../src/core
    ../../src/extensions/security/wrapped_key
    ../../src/fatfs
    ../../src/os/host
    ../../src/os/microtkernel
    ../../src/ports/host
    ../../src/ports/ra_fsp/sd_spi
    ../../src/ports/stm32_cube/crypto
    ../../src/ports/stm32_cube/sdmmc
    ../../src/sentinel
    ../common/console
    ../common/framework
    ../common/benchmarks
    ../common/cases
)

target_compile_definitions(mtfs_host_tests PRIVATE
    MTFS_ENABLE_SEALED_MODEL=1
    MTFS_ENABLE_STORAGE_SENTINEL=1
    MTFS_FF_USE_MKFS=1
    MTFS_FF_USE_LFN=${MTFS_HOST_FF_USE_LFN}
    MTFS_FF_MAX_LFN=${MTFS_HOST_FF_MAX_LFN}
    MTFS_FF_CODE_PAGE=${MTFS_HOST_FF_CODE_PAGE}
    MTFS_FF_FS_NORTC=0
    MTFS_FF_FS_REENTRANT=1
    MTFS_FATFS_MUTEX_ADAPTER=MTFS_FATFS_MUTEX_ADAPTER_POSIX
)
target_link_libraries(mtfs_host_tests PRIVATE Threads::Threads)
set_target_properties(mtfs_host_tests PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)

target_compile_options(mtfs_host_tests PRIVATE -Wall -Wextra -Wpedantic)

add_library(mtfs_diskio_readonly_compile OBJECT
    ../../src/fatfs/mtfs_diskio.c
)
target_include_directories(mtfs_diskio_readonly_compile PRIVATE
    ../../src
    ../../src/block
    ../../src/fatfs
)
target_compile_definitions(mtfs_diskio_readonly_compile PRIVATE
    MTFS_FF_FS_READONLY=1
)
target_compile_options(mtfs_diskio_readonly_compile PRIVATE
    -Wall -Wextra -Wpedantic
)

add_library(mtfs_fatfs_nonreentrant_compile OBJECT
    ../../src/fatfs/ff.c
    ../../src/fatfs/ffunicode.c
    ../../src/fatfs/mtfs_diskio.c
    ../../src/os/host/mtfs_fatfs_mutex_posix.c
    ../../src/os/microtkernel/mtfs_fatfs_mutex.c
)

add_library(mtfs_fattime_disabled_compile OBJECT
    ../../src/fatfs/mtfs_fattime.c
)

add_library(mtfs_diagnostics_disabled_compile OBJECT
    ../../src/block/mtfs_block_device.c
    ../../src/block/mtfs_block_diagnostics.c
    ../../src/core/mtfs_media.c
    ../../src/ports/host/mtfs_host_block_file.c
)
target_include_directories(mtfs_diagnostics_disabled_compile PRIVATE
    ../../src ../../src/block ../../src/core ../../src/ports/host)
target_compile_definitions(mtfs_diagnostics_disabled_compile PRIVATE
    MTFS_ENABLE_DIAGNOSTICS=0)
target_compile_options(mtfs_diagnostics_disabled_compile PRIVATE
    -Wall -Wextra -Wpedantic)

add_library(mtfs_sentinel_disabled_compile OBJECT
    ../../src/sentinel/mtfs_sentinel.c
    ../../src/sentinel/mtfs_sentinel_observer.c
)
target_include_directories(mtfs_sentinel_disabled_compile PRIVATE ../../src)
target_compile_definitions(mtfs_sentinel_disabled_compile PRIVATE
    MTFS_ENABLE_STORAGE_SENTINEL=0)
target_compile_options(mtfs_sentinel_disabled_compile PRIVATE
    -Wall -Wextra -Wpedantic)

# Prove that the optional sealed-model sources and public headers compile
# without OpenSSL, FSP, STM32 HAL, or their include paths when the feature is
# disabled. Vendor headers must remain behind MTFS_ENABLE_SEALED_MODEL guards.
add_library(mtfs_sealed_model_disabled_compile OBJECT
    test_sealed_model_disabled_headers.c
    ../../src/extensions/ai/model_store/mtfs_model_store.c
    ../../src/extensions/security/sealed_blob/mtfs_secure_zero.c
    ../../src/extensions/security/sealed_blob/mtfs_sealed_format.c
    ../../src/extensions/security/sealed_blob/mtfs_sealed_blob.c
    ../../src/fatfs/mtfs_sealed_reader_fatfs.c
    ../../src/extensions/security/wrapped_key/mtfs_wrapped_key_record.c
    ../../src/extensions/security/wrapped_key/mtfs_wrapped_key_fatfs.c
    ../../src/ports/ra_fsp/crypto/mtfs_ra8p1_ospi_key_store.c
    ../../src/ports/stm32_cube/crypto/mtfs_stm32_saes.c
    ../../src/ports/stm32_cube/crypto/mtfs_stm32_saes_provider.c
    ../../src/ports/stm32_cube/crypto/mtfs_stm32_nor_key_store.c
    ../../src/ports/stm32_cube/crypto/mtfs_stm32n6570_nor.c
)
target_compile_definitions(mtfs_sealed_model_disabled_compile PRIVATE
    MTFS_ENABLE_SEALED_MODEL=0)
target_compile_options(mtfs_sealed_model_disabled_compile PRIVATE
    -Wall -Wextra -Wpedantic)
target_include_directories(mtfs_sealed_model_disabled_compile PRIVATE
    ../../src)
target_include_directories(mtfs_fattime_disabled_compile PRIVATE
    ../../src
    ../../src/core
    ../../src/fatfs
)
target_compile_definitions(mtfs_fattime_disabled_compile PRIVATE
    MTFS_FF_FS_NORTC=1
)
target_compile_options(mtfs_fattime_disabled_compile PRIVATE
    -Wall -Wextra -Wpedantic
)
target_include_directories(mtfs_fatfs_nonreentrant_compile PRIVATE
    ../../src
    ../../src/block
    ../../src/fatfs
)
target_compile_definitions(mtfs_fatfs_nonreentrant_compile PRIVATE
    MTFS_FF_FS_REENTRANT=0
    MTFS_FATFS_MUTEX_ADAPTER=MTFS_FATFS_MUTEX_ADAPTER_NONE
)
target_compile_options(mtfs_fatfs_nonreentrant_compile PRIVATE
    -Wall -Wextra -Wpedantic
)

add_test(NAME mtfs_host_roundtrip COMMAND mtfs_host_tests)

add_executable(mtfs_fattime_enabled_test
    test_fattime_symbol.c
    ../../src/core/mtfs_time.c
    ../../src/fatfs/mtfs_fattime.c
)
target_include_directories(mtfs_fattime_enabled_test PRIVATE
    ../../src
    ../../src/core
    ../../src/fatfs
)
target_compile_definitions(mtfs_fattime_enabled_test PRIVATE
    MTFS_FF_FS_NORTC=0
)
set_target_properties(mtfs_fattime_enabled_test PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES
)
target_compile_options(mtfs_fattime_enabled_test PRIVATE
    -Wall -Wextra -Wpedantic
)
add_test(NAME mtfs_fattime_enabled COMMAND mtfs_fattime_enabled_test)

add_executable(mtfs_stm32_saes_provider_bounds_test
    test_stm32_saes_provider_bounds.c
    ../../src/ports/stm32_cube/crypto/mtfs_stm32_saes_provider.c
)
target_include_directories(mtfs_stm32_saes_provider_bounds_test BEFORE PRIVATE
    stubs
    ../../src
    ../../src/extensions/security/sealed_blob
    ../../src/ports/stm32_cube/crypto
)
target_compile_definitions(mtfs_stm32_saes_provider_bounds_test PRIVATE
    MTFS_ENABLE_SEALED_MODEL=1
)
set_target_properties(mtfs_stm32_saes_provider_bounds_test PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES
)
target_compile_options(mtfs_stm32_saes_provider_bounds_test PRIVATE
    -Wall -Wextra -Wpedantic
)
add_test(NAME mtfs_stm32_saes_provider_bounds
    COMMAND mtfs_stm32_saes_provider_bounds_test)

add_executable(mtfs_sentinel_lab_console_test
    test_sentinel_lab_console.c
    ../../apps/sentinel-lab/application/mtfs_sentinel_lab_console.c
)
target_include_directories(mtfs_sentinel_lab_console_test PRIVATE
    ../../apps/sentinel-lab/application
)
set_target_properties(mtfs_sentinel_lab_console_test PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES
)
target_compile_options(mtfs_sentinel_lab_console_test PRIVATE
    -Wall -Wextra -Wpedantic
)
add_test(NAME mtfs_sentinel_lab_console
    COMMAND mtfs_sentinel_lab_console_test)

add_executable(mtfs_sentinel_recorder_test
    test_sentinel_recorder.c
    ../../apps/sentinel-lab/application/mtfs_sentinel_recorder.c
)
target_include_directories(mtfs_sentinel_recorder_test PRIVATE
    ../../apps/sentinel-lab/application
    ../../src
    ../../src/block
    ../../src/core
    ../../src/fatfs
    ../../src/sentinel
)
target_compile_definitions(mtfs_sentinel_recorder_test PRIVATE
    MTFS_ENABLE_DIAGNOSTICS=1
    MTFS_ENABLE_STORAGE_SENTINEL=1
)
set_target_properties(mtfs_sentinel_recorder_test PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES
)
target_compile_options(mtfs_sentinel_recorder_test PRIVATE
    -Wall -Wextra -Wpedantic
)
add_test(NAME mtfs_sentinel_recorder
    COMMAND mtfs_sentinel_recorder_test)
