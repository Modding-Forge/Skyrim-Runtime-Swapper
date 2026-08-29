cmake_minimum_required(VERSION 3.25)

project(SkyrimRuntimeSwapper VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Skyrim Runtime Swapper is Windows-only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(SKYRIM_RUNTIME_PATCH_MANIFEST "" CACHE FILEPATH "Verified patch manifest to embed in this build")
set(SKYRIM_RUNTIME_BUILD_PROFILE "" CACHE STRING "Runtime package profile: bobw or boaw")
if(NOT SKYRIM_RUNTIME_PATCH_MANIFEST OR NOT EXISTS "${SKYRIM_RUNTIME_PATCH_MANIFEST}")
  message(FATAL_ERROR "SKYRIM_RUNTIME_PATCH_MANIFEST must point to a generated and verified manifest.json")
endif()
if(NOT SKYRIM_RUNTIME_BUILD_PROFILE MATCHES "^(bobw|boaw)$")
  message(FATAL_ERROR "SKYRIM_RUNTIME_BUILD_PROFILE must be bobw or boaw")
endif()

file(READ "${SKYRIM_RUNTIME_PATCH_MANIFEST}" PATCH_MANIFEST_JSON)
string(JSON SOURCE_VERSION GET "${PATCH_MANIFEST_JSON}" sourceVersion)
string(JSON TARGET_VERSION GET "${PATCH_MANIFEST_JSON}" targetVersion)
string(JSON PATCH_FORMAT GET "${PATCH_MANIFEST_JSON}" format)
string(JSON PATCH_ALGORITHM GET "${PATCH_MANIFEST_JSON}" algorithm)
string(JSON HDIFFPATCH_VERSION GET "${PATCH_MANIFEST_JSON}" hdiffPatchVersion)
if(NOT PATCH_FORMAT EQUAL 3)
  message(FATAL_ERROR "Patch manifest format 3 is required")
endif()
if(NOT PATCH_ALGORITHM STREQUAL "hdiffpatch-hdiffw26-zstd")
  message(FATAL_ERROR "Unsupported patch algorithm: ${PATCH_ALGORITHM}")
endif()
if(NOT HDIFFPATCH_VERSION STREQUAL "5.1.3")
  message(FATAL_ERROR "The native patch library requires HDiffPatch 5.1.3 assets")
endif()
if(NOT SOURCE_VERSION STREQUAL "1.7.104" OR
   NOT TARGET_VERSION MATCHES "^(1\.6\.1170|1\.5\.97)$")
  message(FATAL_ERROR "Only Skyrim 1.7.104 <-> 1.6.1170/1.5.97 is currently supported")
endif()

include(cmake/runtime_profiles.cmake)
runtime_profile_files("${SKYRIM_RUNTIME_BUILD_PROFILE}" "${TARGET_VERSION}" PROFILE_FILES)

function(split_runtime_version VERSION PREFIX)
  string(REPLACE "." ";" PARTS "${VERSION}")
  list(LENGTH PARTS PART_COUNT)
  if(PART_COUNT LESS 3 OR PART_COUNT GREATER 4)
    message(FATAL_ERROR "Invalid runtime version in manifest: ${VERSION}")
  endif()
  list(GET PARTS 0 ${PREFIX}_MAJOR)
  list(GET PARTS 1 ${PREFIX}_MINOR)
  list(GET PARTS 2 ${PREFIX}_PATCH)
  if(PART_COUNT EQUAL 4)
    list(GET PARTS 3 ${PREFIX}_REVISION)
  else()
    set(${PREFIX}_REVISION 0)
  endif()
  foreach(PART MAJOR MINOR PATCH REVISION)
    set(${PREFIX}_${PART} "${${PREFIX}_${PART}}" PARENT_SCOPE)
  endforeach()
endfunction()

split_runtime_version("${SOURCE_VERSION}" SOURCE)
split_runtime_version("${TARGET_VERSION}" TARGET)

string(JSON PATCH_FILE_COUNT LENGTH "${PATCH_MANIFEST_JSON}" files)
if(PATCH_FILE_COUNT LESS 1)
  message(FATAL_ERROR "At least one patch file is required")
endif()
math(EXPR PATCH_FILE_LAST "${PATCH_FILE_COUNT} - 1")
set(PATCH_PLAN_ENTRIES "")
set(SELECTED_PATCH_FILES "")
foreach(INDEX RANGE 0 ${PATCH_FILE_LAST})
  string(JSON RELATIVE_FILE GET "${PATCH_MANIFEST_JSON}" files ${INDEX} path)
  if(NOT RELATIVE_FILE IN_LIST PROFILE_FILES)
    continue()
  endif()
  string(JSON SOURCE_PRESENT GET "${PATCH_MANIFEST_JSON}" files ${INDEX} sourcePresent)
  string(JSON TARGET_PRESENT GET "${PATCH_MANIFEST_JSON}" files ${INDEX} targetPresent)
  if(NOT TARGET_PRESENT)
    message(FATAL_ERROR "Profile targets must contain every managed file: ${RELATIVE_FILE}")
  endif()
  string(JSON SOURCE_SHA256 GET "${PATCH_MANIFEST_JSON}" files ${INDEX} sourceSha256)
  string(JSON TARGET_SHA256 GET "${PATCH_MANIFEST_JSON}" files ${INDEX} targetSha256)
  string(JSON SOURCE_SIZE GET "${PATCH_MANIFEST_JSON}" files ${INDEX} sourceSize)
  string(JSON TARGET_SIZE GET "${PATCH_MANIFEST_JSON}" files ${INDEX} targetSize)
  string(JSON FORWARD_PATCH GET "${PATCH_MANIFEST_JSON}" files ${INDEX} forwardPatch)
  string(JSON REVERSE_PATCH GET "${PATCH_MANIFEST_JSON}" files ${INDEX} reversePatch)
  string(JSON FORWARD_PATCH_SHA256 GET "${PATCH_MANIFEST_JSON}" files ${INDEX} forwardPatchSha256)
  string(JSON REVERSE_PATCH_SHA256 GET "${PATCH_MANIFEST_JSON}" files ${INDEX} reversePatchSha256)
  string(TOLOWER "${SOURCE_SHA256}" SOURCE_SHA256)
  string(TOLOWER "${TARGET_SHA256}" TARGET_SHA256)
  string(REPLACE "\\" "/" RELATIVE_FILE "${RELATIVE_FILE}")
  string(REPLACE "\\" "/" FORWARD_PATCH_CPP "${FORWARD_PATCH}")
  string(REPLACE "\\" "/" REVERSE_PATCH_CPP "${REVERSE_PATCH}")
  if(SOURCE_PRESENT)
    set(SOURCE_PRESENT_CPP true)
  else()
    set(SOURCE_PRESENT_CPP false)
  endif()
  if(TARGET_PRESENT)
    set(TARGET_PRESENT_CPP true)
  else()
    set(TARGET_PRESENT_CPP false)
  endif()
  string(APPEND PATCH_PLAN_ENTRIES
    "    PatchPlanEntry{\"${RELATIVE_FILE}\", ${SOURCE_PRESENT_CPP}, ${TARGET_PRESENT_CPP}, \"${SOURCE_SHA256}\", \"${TARGET_SHA256}\", ${SOURCE_SIZE}ULL, ${TARGET_SIZE}ULL, \"${FORWARD_PATCH_CPP}\", \"${REVERSE_PATCH_CPP}\", \"${FORWARD_PATCH_SHA256}\", \"${REVERSE_PATCH_SHA256}\"},\n")
  list(APPEND SELECTED_PATCH_FILES "${FORWARD_PATCH}" "${REVERSE_PATCH}")
endforeach()

foreach(REQUIRED_FILE IN LISTS PROFILE_FILES)
  string(REPLACE "\\" "/" REQUIRED_FILE_CPP "${REQUIRED_FILE}")
  string(FIND "${PATCH_PLAN_ENTRIES}" "\"${REQUIRED_FILE_CPP}\"" REQUIRED_FILE_POSITION)
  if(REQUIRED_FILE_POSITION EQUAL -1)
    message(FATAL_ERROR "The asset catalog does not contain required ${SKYRIM_RUNTIME_BUILD_PROFILE} file: ${REQUIRED_FILE}")
  endif()
endforeach()
list(LENGTH PROFILE_FILES PROFILE_FILE_COUNT)

set(GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_INCLUDE}/runtime_swapper")
configure_file(cmake/runtime_version.hpp.in "${GENERATED_INCLUDE}/runtime_swapper/runtime_version.hpp" @ONLY)
configure_file(cmake/patch_plan.hpp.in "${GENERATED_INCLUDE}/runtime_swapper/patch_plan.hpp" @ONLY)

function(add_windows_version_resource TARGET_NAME BINARY_NAME DESCRIPTION INTERNAL_NAME FILE_TYPE)
  set(BINARY_FILENAME "${BINARY_NAME}")
  set(BINARY_DESCRIPTION "${DESCRIPTION}")
  set(BINARY_INTERNAL_NAME "${INTERNAL_NAME}")
  set(BINARY_FILE_TYPE "${FILE_TYPE}")
  set(RESOURCE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/resources")
  file(MAKE_DIRECTORY "${RESOURCE_DIRECTORY}")
  set(RESOURCE_FILE "${RESOURCE_DIRECTORY}/${TARGET_NAME}_version.rc")
  configure_file(cmake/windows_version.rc.in "${RESOURCE_FILE}" @ONLY)
  target_sources(${TARGET_NAME} PRIVATE "${RESOURCE_FILE}")
endfunction()

set(HDIFFPATCH5_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/HDiffPatch5")
set(HDIFFPATCH_ROOT "${HDIFFPATCH5_ROOT}/HDiffPatch")
if(NOT EXISTS "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/patch.c" OR
   NOT EXISTS "${HDIFFPATCH5_ROOT}/zstd/lib/zstd.h" OR
   NOT EXISTS "${HDIFFPATCH5_ROOT}/xxHash/xxh3.h")
  message(FATAL_ERROR
    "HDiffPatch5 is missing. Initialize third_party/HDiffPatch5 recursively before building")
endif()

add_library(hdiffpatch_native STATIC
  src/core/native_hpatch_adapter.c
  "${HDIFFPATCH_ROOT}/hpatchz.c"
  "${HDIFFPATCH_ROOT}/file_for_patch.c"
  "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/patch.c"
  "${HDIFFPATCH_ROOT}/libHDiffPatch/HDiff/private_diff/limit_mem_diff/adler_roll.c"
  "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/_hcache_old_mt.c"
  "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/_hcache_window_old_mt.c"
  "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/_hinput_mt.c"
  "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/_houtput_mt.c"
  "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/_hpatch_mt.c"
  "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/hpatch_mt.c"
  "${HDIFFPATCH_ROOT}/libParallel/parallel_import_c.c"
  "${HDIFFPATCH5_ROOT}/zstd/lib/common/debug.c"
  "${HDIFFPATCH5_ROOT}/zstd/lib/common/entropy_common.c"
  "${HDIFFPATCH5_ROOT}/zstd/lib/common/error_private.c"
  "${HDIFFPATCH5_ROOT}/zstd/lib/common/fse_decompress.c"
  "${HDIFFPATCH5_ROOT}/zstd/lib/common/xxhash.c"
  "${HDIFFPATCH5_ROOT}/zstd/lib/common/zstd_common.c"
  "${HDIFFPATCH5_ROOT}/zstd/lib/decompress/huf_decompress.c"
  "${HDIFFPATCH5_ROOT}/zstd/lib/decompress/zstd_ddict.c"
  "${HDIFFPATCH5_ROOT}/zstd/lib/decompress/zstd_decompress.c"
  "${HDIFFPATCH5_ROOT}/zstd/lib/decompress/zstd_decompress_block.c"
)
target_include_directories(hdiffpatch_native PRIVATE
  "${HDIFFPATCH_ROOT}"
  "${HDIFFPATCH5_ROOT}/zstd/lib"
  "${HDIFFPATCH5_ROOT}/zstd/lib/common"
  "${HDIFFPATCH5_ROOT}/zstd/lib/decompress"
  "${HDIFFPATCH5_ROOT}/xxHash"
)
target_compile_definitions(hdiffpatch_native PRIVATE
  _CRT_SECURE_NO_WARNINGS
  _IS_NEED_MAIN=0
  _IS_NEED_CMDLINE=0
  _IS_NEED_PRINT_LOG=0
  _IS_NEED_PRINT_PROGRESS=0
  _IS_NEED_DIR_DIFF_PATCH=0
  _IS_NEED_WINDOW_DIFF=1
  _IS_NEED_BSDIFF=0
  _IS_NEED_VCDIFF=0
  _IS_NEED_SFX=0
  _IS_NEED_DEFAULT_CompressPlugin=0
  _IS_NEED_DEFAULT_ChecksumPlugin=0
  _CompressPlugin_zstd
  _ChecksumPlugin_xxh128
)
if(MSVC)
  set_target_properties(hdiffpatch_native PROPERTIES VS_GLOBAL_WarningLevel TurnOffAllWarnings)
endif()

add_library(runtime_swapper_core STATIC
  src/core/downgrade.cpp
  src/core/file_operations.cpp
  src/core/hdiff_patch.cpp
  src/core/restore.cpp
  src/core/runtime_backup.cpp
  src/core/runtime_transform.cpp
  src/core/sha256.cpp
  src/core/transaction_journal.cpp
  src/core/windows_transaction_backend.cpp
)
target_include_directories(runtime_swapper_core BEFORE
  PUBLIC "${GENERATED_INCLUDE}" src/include
  PRIVATE src/core
)
target_compile_definitions(runtime_swapper_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(runtime_swapper_core PUBLIC bcrypt PRIVATE hdiffpatch_native)

add_library(version_proxy SHARED
  src/proxy/bootstrap.cpp
  src/proxy/version_proxy.cpp
  src/proxy/version_proxy.def
)
target_include_directories(version_proxy PRIVATE src/include src/proxy)
target_compile_definitions(version_proxy PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(version_proxy PROPERTIES OUTPUT_NAME version PREFIX "")
add_windows_version_resource(version_proxy "version.dll"
  "Skyrim Runtime Swapper Version API Proxy" "version" "VFT_DLL")

add_executable(runtime_swapper_helper WIN32
  src/main.cpp
  src/app/application.cpp
  src/app/command_line.cpp
  src/app/content_catalog.cpp
  src/app/creation_club.cpp
  src/app/diagnostics.cpp
  src/app/fixed_runtime.cpp
  src/app/manual_gui.cpp
  src/app/runtime_version_reader.cpp
  src/app/session.cpp
)
target_include_directories(runtime_swapper_helper BEFORE PRIVATE
  "${GENERATED_INCLUDE}"
  src/include
  src/app
)
target_compile_definitions(runtime_swapper_helper PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(runtime_swapper_helper PRIVATE runtime_swapper_core version shell32 comctl32)
set_target_properties(runtime_swapper_helper PROPERTIES OUTPUT_NAME SkyrimRuntimeSwapper)
add_windows_version_resource(runtime_swapper_helper "SkyrimRuntimeSwapper.exe"
  "Skyrim Runtime Swapper" "SkyrimRuntimeSwapper" "VFT_APP")

add_executable(patch_validator src/tools/patch_validator.cpp)
target_include_directories(patch_validator BEFORE PRIVATE "${GENERATED_INCLUDE}" src/include)
target_compile_definitions(patch_validator PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(patch_validator PRIVATE runtime_swapper_core shell32)
set_target_properties(patch_validator PROPERTIES OUTPUT_NAME SkyrimRuntimePatchValidator)
add_windows_version_resource(patch_validator "SkyrimRuntimePatchValidator.exe"
  "Skyrim Runtime Swapper Patch Validator" "SkyrimRuntimePatchValidator" "VFT_APP")

if(MSVC)
  foreach(target runtime_swapper_core version_proxy runtime_swapper_helper patch_validator)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8 /FS)
  endforeach()
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(runtime_version_tests src/tests/runtime_version_tests.cpp)
  target_include_directories(runtime_version_tests BEFORE PRIVATE "${GENERATED_INCLUDE}" src/include)
  if(MSVC)
    target_compile_options(runtime_version_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  endif()
  add_test(NAME runtime_version_tests COMMAND runtime_version_tests)
  add_windows_version_resource(runtime_version_tests "runtime_version_tests.exe"
    "Skyrim Runtime Swapper Runtime Version Tests" "runtime_version_tests" "VFT_APP")

  add_executable(command_line_tests
    src/tests/command_line_tests.cpp
    src/app/command_line.cpp
  )
  target_include_directories(command_line_tests PRIVATE src/app)
  target_compile_definitions(command_line_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  if(MSVC)
    target_compile_options(command_line_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  endif()
  add_test(NAME command_line_tests COMMAND command_line_tests)
  add_windows_version_resource(command_line_tests "command_line_tests.exe"
    "Skyrim Runtime Swapper Command Line Tests" "command_line_tests" "VFT_APP")

  add_executable(core_storage_tests src/tests/core_storage_tests.cpp)
  target_include_directories(core_storage_tests PRIVATE src/core)
  target_compile_definitions(core_storage_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(core_storage_tests PRIVATE runtime_swapper_core)
  if(MSVC)
    target_compile_options(core_storage_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  endif()
  add_test(NAME core_storage_tests COMMAND core_storage_tests)
  add_windows_version_resource(core_storage_tests "core_storage_tests.exe"
    "Skyrim Runtime Swapper Core Storage Tests" "core_storage_tests" "VFT_APP")

  add_executable(content_catalog_tests
    src/tests/content_catalog_tests.cpp
    src/app/content_catalog.cpp
  )
  target_include_directories(content_catalog_tests PRIVATE src/app)
  target_compile_definitions(content_catalog_tests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(content_catalog_tests PRIVATE runtime_swapper_core)
  if(MSVC)
    target_compile_options(content_catalog_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  endif()
  add_test(NAME content_catalog_tests COMMAND content_catalog_tests)
  add_windows_version_resource(content_catalog_tests "content_catalog_tests.exe"
    "Skyrim Runtime Swapper Content Catalog Tests" "content_catalog_tests" "VFT_APP")

  add_executable(creation_club_tests
    src/tests/creation_club_tests.cpp
    src/app/creation_club.cpp
  )
  target_include_directories(creation_club_tests PRIVATE src/app)
  target_compile_definitions(creation_club_tests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(creation_club_tests PRIVATE runtime_swapper_core)
  if(MSVC)
    target_compile_options(creation_club_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  endif()
  add_test(NAME creation_club_tests COMMAND creation_club_tests)
  add_windows_version_resource(creation_club_tests "creation_club_tests.exe"
    "Skyrim Runtime Swapper Creation Club Tests" "creation_club_tests" "VFT_APP")

  add_executable(fixed_runtime_tests
    src/tests/fixed_runtime_tests.cpp
    src/app/fixed_runtime.cpp
  )
  target_include_directories(fixed_runtime_tests PRIVATE src/app)
  target_compile_definitions(fixed_runtime_tests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(fixed_runtime_tests PRIVATE runtime_swapper_core)
  if(MSVC)
    target_compile_options(fixed_runtime_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  endif()
  add_test(NAME fixed_runtime_tests COMMAND fixed_runtime_tests)
  add_windows_version_resource(fixed_runtime_tests "fixed_runtime_tests.exe"
    "Skyrim Runtime Swapper Fixed Runtime Tests" "fixed_runtime_tests" "VFT_APP")

  add_executable(preload_probe src/tests/preload_probe.cpp)
  target_compile_definitions(preload_probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  set_target_properties(preload_probe PROPERTIES OUTPUT_NAME PreloadProbe)
  add_windows_version_resource(preload_probe "PreloadProbe.exe"
    "Skyrim Runtime Swapper Preload Probe" "PreloadProbe" "VFT_APP")
  if(MSVC)
    target_compile_options(preload_probe PRIVATE /W4 /permissive- /EHsc /utf-8 /FS)
  endif()

  add_executable(runtime_transaction_probe src/tests/runtime_transaction_probe.cpp)
  target_include_directories(runtime_transaction_probe BEFORE PRIVATE
    "${GENERATED_INCLUDE}" src/include)
  target_compile_definitions(runtime_transaction_probe PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(runtime_transaction_probe PRIVATE runtime_swapper_core)
  set_target_properties(runtime_transaction_probe PROPERTIES OUTPUT_NAME RuntimeTransactionProbe)
  add_windows_version_resource(runtime_transaction_probe "RuntimeTransactionProbe.exe"
    "Skyrim Runtime Swapper Transaction Probe" "RuntimeTransactionProbe" "VFT_APP")
  if(MSVC)
    target_compile_options(runtime_transaction_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
  endif()
endif()

install(TARGETS version_proxy runtime_swapper_helper
  RUNTIME DESTINATION .
  LIBRARY DESTINATION .
)
