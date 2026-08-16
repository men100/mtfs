cmake_minimum_required(VERSION 3.16)
project(mtfs_host_tests C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "microT-FS host tests support Linux and WSL2 only")
endif()

enable_testing()
find_package(Threads REQUIRED)

add_executable(mtfs_host_tests
    main.c
    test_benchmark.c
    test_block_diagnostics.c
    test_media_lifecycle.c
    test_ra_sd_spi_deadline.c
    test_rtc_set_app.c
    test_time_provider.c
    ../common/framework/mtfs_test.c
    ../common/benchmarks/mtfs_benchmark.c
    ../common/cases/test_fatfs_concurrent.c
    ../common/cases/test_fatfs_roundtrip.c
    ../common/cases/test_fatfs_timestamp.c
    ../../apps/rtc-set/common/mtfs_rtc_set_app.c
    ../../src/block/mtfs_block_device.c
    ../../src/block/mtfs_block_diagnostics.c
    ../../src/block/mtfs_block_registry.c
    ../../src/core/mtfs_media.c
    ../../src/core/mtfs_time.c
    ../../src/fatfs/ff.c
    ../../src/fatfs/mtfs_diskio.c
    ../../src/fatfs/mtfs_fattime.c
    ../../src/os/host/mtfs_fatfs_mutex_posix.c
    ../../src/os/microtkernel/mtfs_fatfs_mutex.c
    ../../src/ports/host/mtfs_host_block_file.c
    ../../src/ports/ra_fsp/sd_spi/mtfs_ra_sd_spi_deadline.c
)

add_library(mtfs_public_headers_cpp_compile OBJECT test_public_headers.cpp)
target_include_directories(mtfs_public_headers_cpp_compile PRIVATE ../../src)
set_target_properties(mtfs_public_headers_cpp_compile PROPERTIES
    CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
target_compile_options(mtfs_public_headers_cpp_compile PRIVATE
    -Wall -Wextra -Wpedantic)

target_include_directories(mtfs_host_tests PRIVATE
    ../../src
    ../../src/block
    ../../src/core
    ../../src/fatfs
    ../../src/os/host
    ../../src/os/microtkernel
    ../../src/ports/host
    ../../src/ports/ra_fsp/sd_spi
    ../../apps/rtc-set/common
    ../common/framework
    ../common/benchmarks
    ../common/cases
)

target_compile_definitions(mtfs_host_tests PRIVATE
    MTFS_FF_USE_MKFS=1
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
