cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CONFIGURATION)
  set(CONFIGURATION Release)
endif()
if(NOT CONFIGURATION MATCHES "^(Debug|Release)$")
  message(FATAL_ERROR "CONFIGURATION must be Debug or Release")
endif()

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${REPOSITORY_ROOT}/cmake/runtime_profiles.cmake")

if(DEFINED ASSET_MANIFEST AND NOT ASSET_MANIFEST STREQUAL "")
  get_filename_component(ASSET_MANIFEST "${ASSET_MANIFEST}" ABSOLUTE)
  set(ASSET_MANIFESTS "${ASSET_MANIFEST}")
else()
  file(GLOB ASSET_MANIFESTS LIST_DIRECTORIES false
    "${REPOSITORY_ROOT}/assets/runtime/*/manifest.json")
  list(SORT ASSET_MANIFESTS)
endif()
if(NOT ASSET_MANIFESTS)
  message(FATAL_ERROR "No bidirectional asset manifests were found under assets/runtime")
endif()

file(READ "${REPOSITORY_ROOT}/vcpkg.json" RELEASE_MANIFEST_JSON)
string(JSON RELEASE_VERSION GET "${RELEASE_MANIFEST_JSON}" version-string)
file(READ "${REPOSITORY_ROOT}/CMakeLists.txt" ROOT_CMAKE_TEXT)
string(REGEX REPLACE "-.*$" "" RELEASE_NUMERIC_VERSION "${RELEASE_VERSION}")
string(FIND "${ROOT_CMAKE_TEXT}"
  "project(SkyrimRuntimeSwapper VERSION ${RELEASE_NUMERIC_VERSION} "
  ROOT_NUMERIC_VERSION_POSITION)
string(FIND "${ROOT_CMAKE_TEXT}"
  "set(SKYRIM_RUNTIME_RELEASE_VERSION \"${RELEASE_VERSION}\")"
  ROOT_RELEASE_VERSION_POSITION)
if(ROOT_NUMERIC_VERSION_POSITION EQUAL -1 OR ROOT_RELEASE_VERSION_POSITION EQUAL -1)
  message(FATAL_ERROR "The CMake project version does not match ${RELEASE_VERSION}")
endif()

string(TIMESTAMP BUILD_ID "%Y%m%d-%H%M%S")
set(RELEASE_ROOT "${REPOSITORY_ROOT}/dist/builds/${RELEASE_VERSION}/${BUILD_ID}")
set(VORTEX_OVERRIDE_FILE
  "${REPOSITORY_ROOT}/assets/vortex_override_instructions.json")
if(NOT EXISTS "${VORTEX_OVERRIDE_FILE}")
  message(FATAL_ERROR
    "Missing Vortex override instructions: ${VORTEX_OVERRIDE_FILE}")
endif()
get_filename_component(CMAKE_BIN_DIRECTORY "${CMAKE_COMMAND}" DIRECTORY)
find_program(CTEST_COMMAND ctest HINTS "${CMAKE_BIN_DIRECTORY}" REQUIRED)

foreach(ASSET_MANIFEST IN LISTS ASSET_MANIFESTS)
  if(NOT EXISTS "${ASSET_MANIFEST}")
    message(FATAL_ERROR "The bidirectional asset manifest was not found: ${ASSET_MANIFEST}")
  endif()
  get_filename_component(ASSET_ROOT "${ASSET_MANIFEST}" DIRECTORY)
  file(READ "${ASSET_MANIFEST}" ASSET_JSON)
  string(JSON PATCH_FORMAT GET "${ASSET_JSON}" format)
  string(JSON PATCH_ALGORITHM GET "${ASSET_JSON}" algorithm)
  string(JSON HDIFF_VERSION GET "${ASSET_JSON}" hdiffPatchVersion)
  string(JSON SOURCE_VERSION GET "${ASSET_JSON}" sourceVersion)
  string(JSON TARGET_VERSION GET "${ASSET_JSON}" targetVersion)
  if(NOT PATCH_FORMAT EQUAL 3 OR NOT PATCH_ALGORITHM STREQUAL "hdiffpatch-hdiffw26-zstd")
    message(FATAL_ERROR "The asset catalog must use format 3 HDIFFW26 Zstandard patches")
  endif()
  if(NOT SOURCE_VERSION STREQUAL "1.7.104" OR
     NOT TARGET_VERSION MATCHES "^(1\.6\.1170|1\.5\.97)$")
    message(FATAL_ERROR
      "The asset catalog must describe Skyrim 1.7.104 <-> 1.6.1170/1.5.97")
  endif()
  if(NOT HDIFF_VERSION STREQUAL "5.1.3")
    message(FATAL_ERROR "The native patch library requires HDiffPatch 5.1.3 assets")
  endif()

  foreach(PROFILE bobw boaw)
    runtime_profile_files("${PROFILE}" "${TARGET_VERSION}" PROFILE_FILES)
    runtime_profile_name("${PROFILE}" PROFILE_NAME)
    if(PROFILE STREQUAL "bobw")
      set(PACKAGE_PROFILE "BoBW")
    else()
      set(PACKAGE_PROFILE "BoAW")
    endif()
    set(SLUG "${PACKAGE_PROFILE}-${SOURCE_VERSION}-to-${TARGET_VERSION}")
    set(BUILD_ROOT "${REPOSITORY_ROOT}/build/${TARGET_VERSION}/${PROFILE}")
    set(BINARY_ROOT "${BUILD_ROOT}/${CONFIGURATION}")
    set(OUTPUT_ROOT "${RELEASE_ROOT}/SRS-v${RELEASE_VERSION}-${SLUG}")
    set(ARCHIVE_PATH "${OUTPUT_ROOT}.zip")

    set(CURRENT_NATIVE_SIDECAR "")
    if(DEFINED NATIVE_SIDECAR_ROOT AND NOT NATIVE_SIDECAR_ROOT STREQUAL "")
      get_filename_component(NATIVE_SIDECAR_ROOT "${NATIVE_SIDECAR_ROOT}" ABSOLUTE)
      set(CURRENT_NATIVE_SIDECAR
        "${NATIVE_SIDECAR_ROOT}/${TARGET_VERSION}/${PROFILE}/SkyrimRuntimeSwapper.Native")
      if(NOT EXISTS "${CURRENT_NATIVE_SIDECAR}")
        message(FATAL_ERROR
          "Missing profile-specific native sidecar: ${CURRENT_NATIVE_SIDECAR}")
      endif()
    elseif(DEFINED NATIVE_SIDECAR AND NOT NATIVE_SIDECAR STREQUAL "")
      message(FATAL_ERROR
        "A single NATIVE_SIDECAR cannot safely serve multiple patch profiles. Use NATIVE_SIDECAR_ROOT/<target>/<profile>/SkyrimRuntimeSwapper.Native")
    endif()

    file(REMOVE_RECURSE "${OUTPUT_ROOT}")
    file(REMOVE "${ARCHIVE_PATH}")
    message(STATUS "Building ${PROFILE_NAME} for Skyrim ${TARGET_VERSION}")

    set(CONFIGURE_ARGUMENTS
      -S "${REPOSITORY_ROOT}"
      -B "${BUILD_ROOT}"
      -A x64
      "-DSKYRIM_RUNTIME_PATCH_MANIFEST=${ASSET_MANIFEST}"
      "-DSKYRIM_RUNTIME_BUILD_PROFILE=${PROFILE}"
    )
    if(NOT CURRENT_NATIVE_SIDECAR STREQUAL "")
      list(APPEND CONFIGURE_ARGUMENTS
        "-DSKYRIM_RUNTIME_POSIX_SIDECAR=${CURRENT_NATIVE_SIDECAR}")
    endif()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" ${CONFIGURE_ARGUMENTS}
      RESULT_VARIABLE CONFIGURE_RESULT
    )
    if(NOT CONFIGURE_RESULT EQUAL 0)
      message(FATAL_ERROR "CMake configuration failed for ${PROFILE_NAME} ${TARGET_VERSION}")
    endif()

    execute_process(
      COMMAND "${CMAKE_COMMAND}" --build "${BUILD_ROOT}" --config "${CONFIGURATION}" --parallel
      RESULT_VARIABLE BUILD_RESULT
    )
    if(NOT BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "Compilation failed for ${PROFILE_NAME} ${TARGET_VERSION}")
    endif()

    execute_process(
      COMMAND "${CTEST_COMMAND}" --test-dir "${BUILD_ROOT}" -C "${CONFIGURATION}"
        --output-on-failure
      RESULT_VARIABLE TEST_RESULT
    )
    if(NOT TEST_RESULT EQUAL 0)
      message(FATAL_ERROR "Tests failed for ${PROFILE_NAME} ${TARGET_VERSION}")
    endif()

    foreach(BINARY_NAME version.dll SkyrimRuntimeSwapper.exe)
      if(NOT EXISTS "${BINARY_ROOT}/${BINARY_NAME}")
        message(FATAL_ERROR "Missing release binary: ${BINARY_ROOT}/${BINARY_NAME}")
      endif()
    endforeach()

    set(PATCH_OUTPUT "${OUTPUT_ROOT}/RuntimeSwap/patches")
    file(MAKE_DIRECTORY "${PATCH_OUTPUT}")
    file(COPY_FILE "${BINARY_ROOT}/version.dll" "${OUTPUT_ROOT}/version.dll")
    file(COPY_FILE "${BINARY_ROOT}/SkyrimRuntimeSwapper.exe"
      "${OUTPUT_ROOT}/SkyrimRuntimeSwapper.exe")
    set(PACKAGED_NATIVE_SIDECAR "")
    if(EXISTS "${BINARY_ROOT}/SkyrimRuntimeSwapper.Native")
      file(COPY_FILE "${BINARY_ROOT}/SkyrimRuntimeSwapper.Native"
        "${OUTPUT_ROOT}/SkyrimRuntimeSwapper.Native")
      set(PACKAGED_NATIVE_SIDECAR SkyrimRuntimeSwapper.Native)
    endif()
    file(COPY_FILE "${VORTEX_OVERRIDE_FILE}"
      "${OUTPUT_ROOT}/vortex_override_instructions.json")

    set(SELECTED_ENTRIES "")
    set(FOUND_PROFILE_FILES "")
    string(JSON ASSET_FILE_COUNT LENGTH "${ASSET_JSON}" files)
    math(EXPR ASSET_FILE_LAST "${ASSET_FILE_COUNT} - 1")
    foreach(INDEX RANGE 0 ${ASSET_FILE_LAST})
      string(JSON RELATIVE_FILE GET "${ASSET_JSON}" files ${INDEX} path)
      if(NOT RELATIVE_FILE IN_LIST PROFILE_FILES)
        continue()
      endif()
      list(APPEND FOUND_PROFILE_FILES "${RELATIVE_FILE}")
      string(JSON ENTRY_JSON GET "${ASSET_JSON}" files ${INDEX})
      if(NOT SELECTED_ENTRIES STREQUAL "")
        string(APPEND SELECTED_ENTRIES ",\n")
      endif()
      string(APPEND SELECTED_ENTRIES "    ${ENTRY_JSON}")

      foreach(DIRECTION forward reverse)
        string(JSON PATCH_NAME GET "${ASSET_JSON}" files ${INDEX} ${DIRECTION}Patch)
        string(JSON EXPECTED_PATCH_HASH GET "${ASSET_JSON}" files ${INDEX}
          ${DIRECTION}PatchSha256)
        set(PATCH_SOURCE "${ASSET_ROOT}/${PATCH_NAME}")
        if(NOT EXISTS "${PATCH_SOURCE}")
          message(FATAL_ERROR "Missing ${PROFILE_NAME} patch asset: ${PATCH_SOURCE}")
        endif()
        file(SHA256 "${PATCH_SOURCE}" ACTUAL_PATCH_HASH)
        if(NOT ACTUAL_PATCH_HASH STREQUAL EXPECTED_PATCH_HASH)
          message(FATAL_ERROR "Patch hash mismatch: ${PATCH_SOURCE}")
        endif()
        get_filename_component(PATCH_DIRECTORY "${PATCH_OUTPUT}/${PATCH_NAME}" DIRECTORY)
        file(MAKE_DIRECTORY "${PATCH_DIRECTORY}")
        file(COPY_FILE "${PATCH_SOURCE}" "${PATCH_OUTPUT}/${PATCH_NAME}")
      endforeach()
    endforeach()

    foreach(REQUIRED_FILE IN LISTS PROFILE_FILES)
      if(NOT REQUIRED_FILE IN_LIST FOUND_PROFILE_FILES)
        message(FATAL_ERROR
          "The asset catalog is missing required ${PROFILE_NAME} file: ${REQUIRED_FILE}")
      endif()
    endforeach()

    string(JSON GAME_ID GET "${ASSET_JSON}" gameId)
    string(JSON APP_ID GET "${ASSET_JSON}" appId)
    string(JSON SOURCE_MANIFESTS GET "${ASSET_JSON}" sourceManifests)
    string(JSON TARGET_MANIFESTS GET "${ASSET_JSON}" targetManifests)
    set(DATA_BASELINE_FIELDS "")
    if(TARGET_VERSION STREQUAL "1.5.97")
      string(JSON DATA_BASELINE_VERSION GET "${ASSET_JSON}" dataBaselineVersion)
      string(JSON DATA_BASELINE_MANIFESTS GET "${ASSET_JSON}" dataBaselineManifests)
      set(DATA_BASELINE_FIELDS
        "  \"dataBaselineVersion\": \"${DATA_BASELINE_VERSION}\",\n  \"dataBaselineManifests\": ${DATA_BASELINE_MANIFESTS},\n")
    endif()
    set(RELEASE_MANIFEST
"{
  \"format\": 3,
  \"algorithm\": \"${PATCH_ALGORITHM}\",
  \"hdiffPatchVersion\": \"${HDIFF_VERSION}\",
  \"variant\": \"${PROFILE}\",
  \"gameId\": \"${GAME_ID}\",
  \"appId\": \"${APP_ID}\",
  \"sourceVersion\": \"${SOURCE_VERSION}\",
  \"targetVersion\": \"${TARGET_VERSION}\",
${DATA_BASELINE_FIELDS}
  \"sourceManifests\": ${SOURCE_MANIFESTS},
  \"targetManifests\": ${TARGET_MANIFESTS},
  \"thirdParty\": {
    \"name\": \"HDiffPatch\",
    \"version\": \"${HDIFF_VERSION}\",
    \"copyright\": \"Copyright (c) 2012-2025 HouSisong\",
    \"license\": \"MIT\",
    \"linkage\": \"statically linked into SkyrimRuntimeSwapper.exe\",
    \"notice\": \"Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, to deal in the Software without restriction. The copyright and permission notices must be included in all copies or substantial portions. The software is provided as is, without warranty of any kind.\"
  },
  \"files\": [
${SELECTED_ENTRIES}
  ]
}
")
    file(WRITE "${OUTPUT_ROOT}/RuntimeSwap/manifest.json" "${RELEASE_MANIFEST}")

    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar cf "${ARCHIVE_PATH}" --format=zip
        version.dll SkyrimRuntimeSwapper.exe ${PACKAGED_NATIVE_SIDECAR} RuntimeSwap
        vortex_override_instructions.json
      WORKING_DIRECTORY "${OUTPUT_ROOT}"
      RESULT_VARIABLE ARCHIVE_RESULT
    )
    if(NOT ARCHIVE_RESULT EQUAL 0 OR NOT EXISTS "${ARCHIVE_PATH}")
      message(FATAL_ERROR "Archive creation failed for ${PROFILE_NAME} ${TARGET_VERSION}")
    endif()
    file(SHA256 "${ARCHIVE_PATH}" ARCHIVE_HASH)
    file(SIZE "${ARCHIVE_PATH}" ARCHIVE_SIZE)
    message(STATUS "Created ${ARCHIVE_PATH}")
    message(STATUS "SHA-256 ${ARCHIVE_HASH} (${ARCHIVE_SIZE} bytes)")
  endforeach()
endforeach()
