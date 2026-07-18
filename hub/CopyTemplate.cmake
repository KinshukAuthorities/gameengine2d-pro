if(NOT DEFINED SOURCE_DIR OR NOT DEFINED DESTINATION_DIR)
  message(FATAL_ERROR "SOURCE_DIR and DESTINATION_DIR are required")
endif()
if(NOT IS_DIRECTORY "${SOURCE_DIR}")
  message(FATAL_ERROR "Hub template source is missing: ${SOURCE_DIR}")
endif()
file(REMOVE_RECURSE "${DESTINATION_DIR}")
file(MAKE_DIRECTORY "${DESTINATION_DIR}")
file(COPY "${SOURCE_DIR}/" DESTINATION "${DESTINATION_DIR}"
  PATTERN "build" EXCLUDE
  PATTERN ".git" EXCLUDE
  PATTERN ".vs" EXCLUDE
  PATTERN "__pycache__" EXCLUDE
  PATTERN ".gameengine.lock" EXCLUDE
  PATTERN "crash_reports" EXCLUDE
  PATTERN "export" EXCLUDE
  PATTERN "*.dll" EXCLUDE
  PATTERN "*.pdb" EXCLUDE
  PATTERN "*.obj" EXCLUDE
  PATTERN "*.ilk" EXCLUDE
  PATTERN "*.tmp" EXCLUDE
  PATTERN "*.bak" EXCLUDE
  # Game7's numbered NewScript files are abandoned scratch behaviours. They
  # are not registered by game_scripts.hpp and compiling them in every new
  # sample project wastes hot-reload time.
  PATTERN "scripts/NewScript*.cpp" EXCLUDE
  # Campaign-map authoring utilities are developer-only Python helpers; the
  # generated playable scenes and all runtime source remain in the template.
  PATTERN "tools" EXCLUDE
  # Canonical projects keep imported source packs/provenance for auditability,
  # but templates should contain only the processed, project-ready assets and
  # notices.  This avoids copying archive/cache bulk or visual references that
  # are not part of the licensed shipped game.
  PATTERN "assets/third_party" EXCLUDE
  PATTERN "assets/new_ones" EXCLUDE
  PATTERN "*.DS_Store" EXCLUDE)
