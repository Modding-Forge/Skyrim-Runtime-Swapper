cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CONFIGURATION)
  set(CONFIGURATION Release)
endif()
if(NOT CONFIGURATION MATCHES "^(Debug|Release)$")
  message(FATAL_ERROR "CONFIGURATION must be Debug or Release")
endif()

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(PATCH_ROOT "${REPOSITORY_ROOT}/patches")

file(READ "${REPOSITORY_ROOT}/vcpkg.json" RELEASE_MANIFEST_JSON)
string(JSON RELEASE_VERSION GET "${RELEASE_MANIFEST_JSON}" version-string)
if(RELEASE_VERSION STREQUAL "")
  message(FATAL_ERROR "The package version is missing")
endif()

file(READ "${REPOSITORY_ROOT}/CMakeLists.txt" ROOT_CMAKE_TEXT)
string(FIND "${ROOT_CMAKE_TEXT}" "project(SkyrimRuntimeSwapper VERSION ${RELEASE_VERSION} " ROOT_VERSION_POSITION)
if(ROOT_VERSION_POSITION EQUAL -1)
  message(FATAL_ERROR "The CMake project version does not match ${RELEASE_VERSION}")
endif()

set(TOOLCHAIN_FILE "")
foreach(VCPKG_ENVIRONMENT_ROOT VCPKG_ROOT VCPKG_INSTALLATION_ROOT)
  if(DEFINED ENV{${VCPKG_ENVIRONMENT_ROOT}})
    set(CANDIDATE "$ENV{${VCPKG_ENVIRONMENT_ROOT}}/scripts/buildsystems/vcpkg.cmake")
    if(EXISTS "${CANDIDATE}")
      set(TOOLCHAIN_FILE "${CANDIDATE}")
      break()
    endif()
  endif()
endforeach()

if(TOOLCHAIN_FILE STREQUAL "")
  find_program(VSWHERE_EXECUTABLE vswhere.exe
    HINTS "$ENV{SystemDrive}/Program Files (x86)/Microsoft Visual Studio/Installer")
  if(VSWHERE_EXECUTABLE)
    execute_process(
      COMMAND "${VSWHERE_EXECUTABLE}" -latest -products * -property installationPath
      OUTPUT_VARIABLE VISUAL_STUDIO_ROOT
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE VSWHERE_RESULT
    )
    if(VSWHERE_RESULT EQUAL 0 AND NOT VISUAL_STUDIO_ROOT STREQUAL "")
      set(CANDIDATE "${VISUAL_STUDIO_ROOT}/VC/vcpkg/scripts/buildsystems/vcpkg.cmake")
      if(EXISTS "${CANDIDATE}")
        set(TOOLCHAIN_FILE "${CANDIDATE}")
      endif()
    endif()
  endif()
endif()

if(TOOLCHAIN_FILE STREQUAL "")
  message(FATAL_ERROR "vcpkg was not found. Set VCPKG_ROOT before running build.bat")
endif()

file(GLOB MANIFEST_PATHS LIST_DIRECTORIES false "${PATCH_ROOT}/*/manifest.json")
list(SORT MANIFEST_PATHS)
list(LENGTH MANIFEST_PATHS MANIFEST_COUNT)
if(MANIFEST_COUNT EQUAL 0)
  message(FATAL_ERROR "No patch manifests were found below ${PATCH_ROOT}")
endif()

string(TIMESTAMP BUILD_ID "%Y%m%d-%H%M%S")
set(RELEASE_ROOT "${REPOSITORY_ROOT}/dist/builds/${RELEASE_VERSION}/${BUILD_ID}")

get_filename_component(CMAKE_BIN_DIRECTORY "${CMAKE_COMMAND}" DIRECTORY)
find_program(CTEST_COMMAND ctest HINTS "${CMAKE_BIN_DIRECTORY}" REQUIRED)

foreach(MANIFEST_PATH IN LISTS MANIFEST_PATHS)
  file(READ "${MANIFEST_PATH}" PATCH_MANIFEST_JSON)
  string(JSON SOURCE_VERSION GET "${PATCH_MANIFEST_JSON}" sourceVersion)
  string(JSON TARGET_VERSION GET "${PATCH_MANIFEST_JSON}" targetVersion)
  set(SLUG "${SOURCE_VERSION}-to-${TARGET_VERSION}")
  set(BUILD_ROOT "${REPOSITORY_ROOT}/build/${SLUG}")
  set(BINARY_ROOT "${BUILD_ROOT}/${CONFIGURATION}")
  set(OUTPUT_ROOT "${RELEASE_ROOT}/Skyrim-Runtime-Swapper-v${RELEASE_VERSION}-${SLUG}")
  set(ARCHIVE_PATH "${OUTPUT_ROOT}.zip")

  if(EXISTS "${BUILD_ROOT}/CMakeCache.txt")
    file(STRINGS "${BUILD_ROOT}/CMakeCache.txt" CACHED_HOME_DIRECTORY
      REGEX "^CMAKE_HOME_DIRECTORY:INTERNAL=")
    string(REPLACE "CMAKE_HOME_DIRECTORY:INTERNAL=" "" CACHED_HOME_DIRECTORY
      "${CACHED_HOME_DIRECTORY}")
    cmake_path(CONVERT "${CACHED_HOME_DIRECTORY}" TO_CMAKE_PATH_LIST CACHED_HOME_DIRECTORY NORMALIZE)
    cmake_path(CONVERT "${REPOSITORY_ROOT}" TO_CMAKE_PATH_LIST EXPECTED_HOME_DIRECTORY NORMALIZE)
    if(NOT CACHED_HOME_DIRECTORY STREQUAL EXPECTED_HOME_DIRECTORY)
      message(STATUS "Removing stale build cache for ${SLUG}")
      file(REMOVE_RECURSE "${BUILD_ROOT}")
    endif()
  endif()

  file(REMOVE_RECURSE "${OUTPUT_ROOT}")
  file(REMOVE "${ARCHIVE_PATH}")
  message(STATUS "Building ${SLUG}")

  set(CONFIGURE_ARGUMENTS
    -S "${REPOSITORY_ROOT}"
    -B "${BUILD_ROOT}"
    -A x64
    "-DSKYRIM_RUNTIME_PATCH_MANIFEST=${MANIFEST_PATH}"
  )
  if(NOT EXISTS "${BUILD_ROOT}/CMakeCache.txt")
    list(APPEND CONFIGURE_ARGUMENTS
      "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}"
      -DVCPKG_TARGET_TRIPLET=x64-windows-static
    )
  endif()

  execute_process(COMMAND "${CMAKE_COMMAND}" ${CONFIGURE_ARGUMENTS} RESULT_VARIABLE CONFIGURE_RESULT)
  if(NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR "CMake configuration failed for ${SLUG}")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_ROOT}" --config "${CONFIGURATION}" --parallel
    RESULT_VARIABLE BUILD_RESULT
  )
  if(NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "Compilation failed for ${SLUG}")
  endif()

  execute_process(
    COMMAND "${CTEST_COMMAND}" --test-dir "${BUILD_ROOT}" -C "${CONFIGURATION}" --output-on-failure
    RESULT_VARIABLE TEST_RESULT
  )
  if(NOT TEST_RESULT EQUAL 0)
    message(FATAL_ERROR "Tests failed for ${SLUG}")
  endif()

  foreach(BINARY_NAME version.dll SkyrimRuntimeSwapper.exe)
    if(NOT EXISTS "${BINARY_ROOT}/${BINARY_NAME}")
      message(FATAL_ERROR "Missing release binary: ${BINARY_ROOT}/${BINARY_NAME}")
    endif()
  endforeach()

  file(READ "${BINARY_ROOT}/SkyrimRuntimeSwapper.exe" HELPER_HEX HEX)
  string(FIND "${HELPER_HEX}"
    "520075006e00740069006d00650053007700610070005c007000610074006300680065007300"
    PATCH_PATH_POSITION)
  if(PATCH_PATH_POSITION EQUAL -1)
    message(FATAL_ERROR "The helper does not reference RuntimeSwap\\patches")
  endif()

  set(PATCH_OUTPUT "${OUTPUT_ROOT}/RuntimeSwap/patches")
  file(MAKE_DIRECTORY "${PATCH_OUTPUT}")
  file(COPY_FILE "${BINARY_ROOT}/version.dll" "${OUTPUT_ROOT}/version.dll")
  file(COPY_FILE "${BINARY_ROOT}/SkyrimRuntimeSwapper.exe" "${OUTPUT_ROOT}/SkyrimRuntimeSwapper.exe")

  get_filename_component(MANIFEST_DIRECTORY "${MANIFEST_PATH}" DIRECTORY)
  string(JSON PATCH_FILE_COUNT LENGTH "${PATCH_MANIFEST_JSON}" files)
  if(NOT PATCH_FILE_COUNT EQUAL 3)
    message(FATAL_ERROR "Exactly three patch files are required for ${SLUG}")
  endif()
  math(EXPR PATCH_FILE_LAST "${PATCH_FILE_COUNT} - 1")
  foreach(PATCH_INDEX RANGE 0 ${PATCH_FILE_LAST})
    string(JSON PATCH_NAME GET "${PATCH_MANIFEST_JSON}" files ${PATCH_INDEX} patch)
    string(JSON EXPECTED_PATCH_HASH GET "${PATCH_MANIFEST_JSON}" files ${PATCH_INDEX} patchSha256)
    set(PATCH_SOURCE "${MANIFEST_DIRECTORY}/${PATCH_NAME}")
    if(NOT EXISTS "${PATCH_SOURCE}")
      message(FATAL_ERROR "Missing patch file: ${PATCH_SOURCE}")
    endif()
    file(SHA256 "${PATCH_SOURCE}" ACTUAL_PATCH_HASH)
    if(NOT ACTUAL_PATCH_HASH STREQUAL EXPECTED_PATCH_HASH)
      message(FATAL_ERROR "Patch hash mismatch: ${PATCH_SOURCE}")
    endif()
    file(COPY_FILE "${PATCH_SOURCE}" "${PATCH_OUTPUT}/${PATCH_NAME}")
  endforeach()
  file(COPY_FILE "${MANIFEST_PATH}" "${OUTPUT_ROOT}/RuntimeSwap/manifest.json")

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${ARCHIVE_PATH}" --format=zip
      version.dll SkyrimRuntimeSwapper.exe RuntimeSwap
    WORKING_DIRECTORY "${OUTPUT_ROOT}"
    RESULT_VARIABLE ARCHIVE_RESULT
  )
  if(NOT ARCHIVE_RESULT EQUAL 0 OR NOT EXISTS "${ARCHIVE_PATH}")
    message(FATAL_ERROR "Archive creation failed for ${SLUG}")
  endif()
  file(SHA256 "${ARCHIVE_PATH}" ARCHIVE_HASH)
  file(SIZE "${ARCHIVE_PATH}" ARCHIVE_SIZE)
  message(STATUS "Created ${ARCHIVE_PATH}")
  message(STATUS "SHA-256 ${ARCHIVE_HASH} (${ARCHIVE_SIZE} bytes)")
endforeach()
