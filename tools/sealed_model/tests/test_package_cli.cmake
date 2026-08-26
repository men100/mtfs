cmake_minimum_required(VERSION 3.16)

foreach(required IN ITEMS
        MTFS_TEST_PACKAGE MTFS_VERIFY MTFS_UNSEAL MTFS_TEST_KEY
        MTFS_EXPECTED_PAYLOAD MTFS_TEST_WORK_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "missing required test setting: ${required}")
    endif()
endforeach()

function(run_success label)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${label} failed (${result})\nstdout:\n${output}\nstderr:\n${error}")
    endif()
endfunction()

function(run_failure label)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR
            "${label} unexpectedly succeeded\nstdout:\n${output}\nstderr:\n${error}")
    endif()
endfunction()

file(REMOVE_RECURSE "${MTFS_TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${MTFS_TEST_WORK_DIR}")

set(package "${MTFS_TEST_WORK_DIR}/MTFSTEST.MTF")
set(descriptor "${MTFS_TEST_WORK_DIR}/MTFSTEST.TXT")
set(recovered "${MTFS_TEST_WORK_DIR}/payload.bin")
set(max_package "${MTFS_TEST_WORK_DIR}/MTFSMAX.MTF")
set(max_descriptor "${MTFS_TEST_WORK_DIR}/MTFSMAX.TXT")
set(max_recovered "${MTFS_TEST_WORK_DIR}/max-payload.bin")

run_success("initial package creation"
    "${MTFS_TEST_PACKAGE}"
    --key "${MTFS_TEST_KEY}"
    --package "${package}"
    --descriptor "${descriptor}")
run_failure("default overwrite protection"
    "${MTFS_TEST_PACKAGE}"
    --key "${MTFS_TEST_KEY}"
    --package "${package}"
    --descriptor "${descriptor}")
run_success("explicit package replacement"
    "${MTFS_TEST_PACKAGE}"
    --key "${MTFS_TEST_KEY}"
    --package "${package}"
    --descriptor "${descriptor}"
    --overwrite)

run_success("package verification"
    "${MTFS_VERIFY}"
    --key "${MTFS_TEST_KEY}"
    --input "${package}"
    --target-id 17
    --accelerator-id 34
    --model-format 51
    --max-chunk-size 4096)
run_success("package recovery"
    "${MTFS_UNSEAL}"
    --key "${MTFS_TEST_KEY}"
    --input "${package}"
    --output "${recovered}"
    --target-id 17
    --accelerator-id 34
    --model-format 51)
run_success("known plaintext comparison"
    "${CMAKE_COMMAND}" -E compare_files
    "${recovered}" "${MTFS_EXPECTED_PAYLOAD}")

run_success("maximum metadata package creation"
    "${MTFS_TEST_PACKAGE}"
    --key "${MTFS_TEST_KEY}"
    --package "${max_package}"
    --descriptor "${max_descriptor}"
    --maximum-metadata)
run_success("maximum metadata package verification"
    "${MTFS_VERIFY}"
    --key "${MTFS_TEST_KEY}"
    --input "${max_package}"
    --target-id 17
    --accelerator-id 34
    --model-format 51
    --max-chunk-size 4096)
run_success("maximum metadata package recovery"
    "${MTFS_UNSEAL}"
    --key "${MTFS_TEST_KEY}"
    --input "${max_package}"
    --output "${max_recovered}"
    --target-id 17
    --accelerator-id 34
    --model-format 51)
run_success("maximum metadata plaintext comparison"
    "${CMAKE_COMMAND}" -E compare_files
    "${max_recovered}" "${MTFS_EXPECTED_PAYLOAD}")

file(SIZE "${package}" package_bytes)
if(NOT package_bytes EQUAL 5280)
    message(FATAL_ERROR "unexpected test package size: ${package_bytes}")
endif()

file(SHA256 "${package}" package_sha256)
file(SHA256 "${recovered}" payload_sha256)
file(READ "${descriptor}" descriptor_text)
foreach(expected IN ITEMS
        "MTFS-TEST-v1\n"
        "package=MTFSTEST.MTF\n"
        "package_bytes=5280\n"
        "payload_bytes=5000\n"
        "chunk_bytes=4096\n"
        "chunks=2\n"
        "metadata_bytes=40\n"
        "payload_pattern=(offset*7+3)&0xff\n"
        "package_sha256=${package_sha256}\n"
        "payload_sha256=${payload_sha256}\n"
        "secret_material=none\n")
    string(FIND "${descriptor_text}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "descriptor is missing: ${expected}")
    endif()
endforeach()

file(SIZE "${max_package}" max_package_bytes)
if(NOT max_package_bytes EQUAL 9336)
    message(FATAL_ERROR
        "unexpected maximum metadata package size: ${max_package_bytes}")
endif()
file(READ "${max_descriptor}" max_descriptor_text)
string(FIND "${max_descriptor_text}" "metadata_bytes=4096\n"
    max_metadata_position)
if(max_metadata_position EQUAL -1)
    message(FATAL_ERROR "maximum metadata descriptor is incorrect")
endif()

file(READ "${MTFS_TEST_KEY}" key_hex HEX)
file(READ "${package}" package_hex HEX)
string(FIND "${package_hex}" "${key_hex}" raw_key_position)
if(NOT raw_key_position EQUAL -1)
    message(FATAL_ERROR "raw fleet test key appears in generated package")
endif()
