cmake_minimum_required(VERSION 3.16)
project(mtfs_host_tests C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "microT-FS host tests support Linux and WSL2 only")
endif()

enable_testing()
find_package(Threads REQUIRED)

add_executable(mtfs_host_tests
    main.c
    ../common/framework/mtfs_test.c
    ../common/cases/test_fatfs_concurrent.c
    ../common/cases/test_fatfs_roundtrip.c
    ../../src/block/mtfs_block_device.c
    ../../src/block/mtfs_block_registry.c
    ../../src/fatfs/ff.c
    ../../src/fatfs/mtfs_diskio.c
    ../../src/os/host/mtfs_fatfs_mutex_posix.c
    ../../src/os/microtkernel/mtfs_fatfs_mutex.c
    ../../src/ports/host/mtfs_host_block_file.c
)

target_include_directories(mtfs_host_tests PRIVATE
    ../../src
    ../../src/block
    ../../src/fatfs
    ../../src/os/host
    ../../src/os/microtkernel
    ../../src/ports/host
    ../common/framework
    ../common/cases
)

target_compile_definitions(mtfs_host_tests PRIVATE
    MTFS_FF_USE_MKFS=1
    MTFS_FF_FS_NORTC=1
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
