function(runtime_profile_files PROFILE TARGET_VERSION OUTPUT_VARIABLE)
  set(BOBW_FILES
    "SkyrimSE.exe"
    "SkyrimSELauncher.exe"
    "Data/Skyrim - Shaders.bsa"
  )
  if(TARGET_VERSION STREQUAL "1.5.97")
    list(APPEND BOBW_FILES
      "binkw64.dll"
      "steam_api64.dll"
    )
  elseif(NOT TARGET_VERSION STREQUAL "1.6.1170")
    message(FATAL_ERROR "Unsupported target runtime: ${TARGET_VERSION}")
  endif()
  if(PROFILE STREQUAL "bobw")
    set(PROFILE_FILES ${BOBW_FILES})
  elseif(PROFILE STREQUAL "boaw")
    set(PROFILE_FILES
      ${BOBW_FILES}
      "Data/Skyrim - Interface.bsa"
      "Data/Skyrim.esm"
      "Data/Update.esm"
      "Data/Dawnguard.esm"
      "Data/HearthFires.esm"
      "Data/Dragonborn.esm"
    )
  else()
    message(FATAL_ERROR "Unknown runtime profile: ${PROFILE}")
  endif()
  set(${OUTPUT_VARIABLE} ${PROFILE_FILES} PARENT_SCOPE)
endfunction()

function(runtime_profile_name PROFILE OUTPUT_VARIABLE)
  if(PROFILE STREQUAL "bobw")
    set(PROFILE_NAME "Best-of-Both-Worlds")
  elseif(PROFILE STREQUAL "boaw")
    set(PROFILE_NAME "Best-of-All-Worlds")
  else()
    message(FATAL_ERROR "Unknown runtime profile: ${PROFILE}")
  endif()
  set(${OUTPUT_VARIABLE} "${PROFILE_NAME}" PARENT_SCOPE)
endfunction()
