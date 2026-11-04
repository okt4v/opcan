# App metadata, install layout and CPack installers (included from the top-level CMakeLists.txt).
# Layout (resDir() in src/engine.cpp finds assets/ and engines/ in each):
#   Linux:   bin/opcan, share/opcan/{assets,engines}, share/applications, share/icons
#   Windows: opcan.exe, assets/, engines/ side by side
#   macOS:   opcan.app/Contents/{MacOS/opcan, Resources/{assets,engines,opcan.icns}}
# Only the "opcan" component is ours; dependencies (llama.cpp, glfw, ...) add their own install rules,
# so install with `cmake --install <build> --component opcan` (CPack does this already).
set(OPCAN_STOCKFISH_PATH "" CACHE FILEPATH "Stockfish binary to bundle into the package (see packaging/fetch-stockfish.*)")
set(OPCAN_PKG ${CMAKE_CURRENT_LIST_DIR})

if(WIN32)
  file(TO_CMAKE_PATH "${OPCAN_PKG}/opcan.ico" OPCAN_ICO)
  file(TO_CMAKE_PATH "${OPCAN_PKG}/opcan.manifest" OPCAN_MANIFEST)
  configure_file(${OPCAN_PKG}/opcan.rc.in ${CMAKE_BINARY_DIR}/opcan.rc @ONLY)
  target_sources(opcan PRIVATE ${CMAKE_BINARY_DIR}/opcan.rc)
  if(MSVC)
    target_sources(opcan PRIVATE ${OPCAN_MANIFEST})  # MSVC merges .manifest sources; MinGW gets it via opcan.rc
  endif()
  # GUI app without a console window, keeping the plain main() (MinGW's CRT calls main() for GUI apps too)
  set_target_properties(opcan PROPERTIES WIN32_EXECUTABLE ON)
  if(MSVC)
    target_link_options(opcan PRIVATE /ENTRY:mainCRTStartup)
  endif()
  set(OPCAN_BIN_DEST .)
  set(OPCAN_RES_DEST .)
elseif(APPLE)
  target_sources(opcan PRIVATE ${OPCAN_PKG}/opcan.icns)
  set_source_files_properties(${OPCAN_PKG}/opcan.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(opcan PROPERTIES
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_INFO_PLIST ${OPCAN_PKG}/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME opcan
    MACOSX_BUNDLE_GUI_IDENTIFIER org.opcan.opcan
    MACOSX_BUNDLE_ICON_FILE opcan.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_COPYRIGHT "opcan contributors")
  set(OPCAN_RES_DEST opcan.app/Contents/Resources)
else()
  include(GNUInstallDirs)
  set(OPCAN_BIN_DEST ${CMAKE_INSTALL_BINDIR})
  set(OPCAN_RES_DEST ${CMAKE_INSTALL_DATADIR}/opcan)
  install(FILES ${OPCAN_PKG}/opcan.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT opcan)
  install(FILES ${OPCAN_PKG}/opcan.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps COMPONENT opcan)
endif()

if(APPLE)  # assets are already in the bundle (POST_BUILD copy)
  install(TARGETS opcan BUNDLE DESTINATION . COMPONENT opcan)
else()
  install(TARGETS opcan RUNTIME DESTINATION ${OPCAN_BIN_DEST} COMPONENT opcan)
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${OPCAN_RES_DEST} COMPONENT opcan)
endif()
if(MSVC)  # MSVCP140/VCRUNTIME140 next to opcan.exe: a clean Windows without the VC++ redistributable can't start it
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${OPCAN_BIN_DEST})
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT opcan)
  include(InstallRequiredSystemLibraries)
endif()
foreach(f LICENSE COPYING README.md)
  if(EXISTS ${CMAKE_SOURCE_DIR}/${f})
    install(FILES ${CMAKE_SOURCE_DIR}/${f} DESTINATION ${OPCAN_RES_DEST} COMPONENT opcan)
  endif()
endforeach()
if(OPCAN_STOCKFISH_PATH)
  if(NOT EXISTS ${OPCAN_STOCKFISH_PATH})
    message(FATAL_ERROR "OPCAN_STOCKFISH_PATH=${OPCAN_STOCKFISH_PATH} does not exist")
  endif()
  install(PROGRAMS ${OPCAN_STOCKFISH_PATH} DESTINATION ${OPCAN_RES_DEST}/engines
          RENAME stockfish${CMAKE_EXECUTABLE_SUFFIX} COMPONENT opcan)
  get_filename_component(_sfdir ${OPCAN_STOCKFISH_PATH} DIRECTORY)
  if(EXISTS ${_sfdir}/stockfish-COPYING.txt)  # Stockfish is GPLv3: ship its license with it
    install(FILES ${_sfdir}/stockfish-COPYING.txt DESTINATION ${OPCAN_RES_DEST}/engines COMPONENT opcan)
  endif()
endif()

# CPack: Windows zip, macOS .dmg, Linux .tar.gz (the Nix flake is the primary Linux package). The Windows installer is
# packaging/opcan.nsi (the `installer` target below), not CPack's NSIS one: the in-app updater expects its per-user install.
set(CPACK_PACKAGE_NAME opcan)
set(CPACK_PACKAGE_VENDOR "opcan contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "opcan - open chess analysis")
set(CPACK_PACKAGE_INSTALL_DIRECTORY opcan)
set(CPACK_PACKAGE_EXECUTABLES opcan opcan)
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};opcan;opcan;/")
string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" _plat)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  string(REPLACE ";" "-" _arch "${CMAKE_OSX_ARCHITECTURES}")
  set(_plat darwin-${_arch})
endif()
set(CPACK_PACKAGE_FILE_NAME opcan-${OPCAN_VERSION_STRING}-${_plat})
if(EXISTS ${CMAKE_SOURCE_DIR}/LICENSE)
  set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
endif()
if(WIN32)
  set(CPACK_GENERATOR ZIP)
  # cmake --build <build> --config Release --target installer -> <build>/opcan-<version>-windows-<arch>-setup.exe, made
  # from the install tree (the zip's contents: opcan.exe, assets, engines, LICENSE, MSVC runtime DLLs)
  find_program(OPCAN_MAKENSIS makensis)
  if(OPCAN_MAKENSIS)
    set(_stage ${CMAKE_BINARY_DIR}/installer-stage)
    file(TO_NATIVE_PATH ${_stage} _stage_native)
    add_custom_target(installer
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${_stage}
      COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --component opcan --prefix ${_stage}
      COMMAND ${OPCAN_MAKENSIS} -V2 -DVERSION=${OPCAN_VERSION_STRING} -DNUMVERSION=${PROJECT_VERSION} -DSTAGE=${_stage_native}
              -DOUTFILE=${CMAKE_BINARY_DIR}/${CPACK_PACKAGE_FILE_NAME}-setup.exe ${OPCAN_PKG}/opcan.nsi
      VERBATIM)
    add_dependencies(installer opcan)
  endif()
elseif(APPLE)
  set(CPACK_GENERATOR DragNDrop)
  set(CPACK_DMG_VOLUME_NAME opcan)
  set(CPACK_PRE_BUILD_SCRIPTS ${OPCAN_PKG}/macos-sign.cmake)  # ad-hoc sign the staged .app
else()
  set(CPACK_GENERATOR TGZ)
endif()
include(CPack)
