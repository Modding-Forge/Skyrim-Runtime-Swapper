function(runtime_profile_files PROFILE OUTPUT_VARIABLE)
  set(BOBW_FILES
    "SkyrimSE.exe"
    "SkyrimSELauncher.exe"
    "Data/Skyrim - Shaders.bsa"
  )
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
