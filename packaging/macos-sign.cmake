# CPack pre-build script (macOS): ad-hoc sign the staged opcan.app so Apple Silicon will run it
# (not notarized: first launch needs right-click > Open, or `xattr -dr com.apple.quarantine opcan.app`).
file(GLOB_RECURSE _plists LIST_DIRECTORIES false "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/*opcan.app/Contents/Info.plist")
foreach(_p ${_plists})
  get_filename_component(_contents ${_p} DIRECTORY)
  get_filename_component(_app ${_contents} DIRECTORY)
  execute_process(COMMAND codesign --force --deep --sign - ${_app} COMMAND_ERROR_IS_FATAL ANY)
  message(STATUS "ad-hoc signed ${_app}")
endforeach()
