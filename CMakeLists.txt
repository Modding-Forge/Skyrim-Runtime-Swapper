cmake_minimum_required(VERSION 3.25)

project(SkyrimRuntimeSwapper VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Skyrim Runtime Swapper is Windows-only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(BZip2 REQUIRED)

set(SKYRIM_RUNTIME_PATCH_MANIFEST "" CACHE FILEPATH "Verified patch manifest to embed in this build")
if(NOT SKYRIM_RUNTIME_PATCH_MANIFEST OR NOT EXISTS "${SKYRIM_RUNTIME_PATCH_MANIFEST}")
  message(FATAL_ERROR "SKYRIM_RUNTIME_PATCH_MANIFEST must point to a generated and verified manifest.json")
endif()

file(READ "${SKYRIM_RUNTIME_PATCH_MANIFEST}" PATCH_MANIFEST_JSON)
string(JSON SOURCE_VERSION GET "${PATCH_MANIFEST_JSON}" sourceVersion)
string(JSON TARGET_VERSION GET "${PATCH_MANIFEST_JSON}" targetVersion)

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
if(NOT PATCH_FILE_COUNT EQUAL 3)
  message(FATAL_ERROR "Exactly three patch files are required, got ${PATCH_FILE_COUNT}")
endif()
math(EXPR PATCH_FILE_LAST "${PATCH_FILE_COUNT} - 1")
set(PATCH_PLAN_ENTRIES "")
foreach(INDEX RANGE 0 ${PATCH_FILE_LAST})
  string(JSON RELATIVE_FILE GET "${PATCH_MANIFEST_JSON}" files ${INDEX} path)
  string(JSON PATCH_FILE GET "${PATCH_MANIFEST_JSON}" files ${INDEX} patch)
  string(JSON SOURCE_SHA256 GET "${PATCH_MANIFEST_JSON}" files ${INDEX} sourceSha256)
  string(JSON TARGET_SHA256 GET "${PATCH_MANIFEST_JSON}" files ${INDEX} targetSha256)
  string(TOLOWER "${SOURCE_SHA256}" SOURCE_SHA256)
  string(TOLOWER "${TARGET_SHA256}" TARGET_SHA256)
  string(REPLACE "/" "\\\\" RELATIVE_FILE "${RELATIVE_FILE}")
  string(APPEND PATCH_PLAN_ENTRIES
    "    PatchPlanEntry{L\"${RELATIVE_FILE}\", L\"${PATCH_FILE}\", \"${SOURCE_SHA256}\", \"${TARGET_SHA256}\"},\n")
endforeach()

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

add_library(runtime_swapper_core STATIC
  src/core/bspatch.cpp
  src/core/downgrade.cpp
  src/core/sha256.cpp
)
target_include_directories(runtime_swapper_core BEFORE PUBLIC "${GENERATED_INCLUDE}" src/include)
target_compile_definitions(runtime_swapper_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(runtime_swapper_core PUBLIC BZip2::BZip2 bcrypt)

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

add_executable(runtime_swapper_helper WIN32 src/helper/main.cpp)
target_include_directories(runtime_swapper_helper BEFORE PRIVATE "${GENERATED_INCLUDE}" src/include)
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

  add_executable(preload_probe src/tests/preload_probe.cpp)
  target_compile_definitions(preload_probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  set_target_properties(preload_probe PROPERTIES OUTPUT_NAME PreloadProbe)
  add_windows_version_resource(preload_probe "PreloadProbe.exe"
    "Skyrim Runtime Swapper Preload Probe" "PreloadProbe" "VFT_APP")
  if(MSVC)
    target_compile_options(preload_probe PRIVATE /W4 /permissive- /EHsc /utf-8 /FS)
  endif()
endif()

install(TARGETS version_proxy runtime_swapper_helper
  RUNTIME DESTINATION .
  LIBRARY DESTINATION .
)
