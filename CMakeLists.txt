cmake_minimum_required(VERSION 3.24)
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Minimum macOS version")  # keep in sync with packaging/Info.plist.in
endif()
project(opcan VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui GIT_TAG v1.93.0-docking GIT_SHALLOW ON SOURCE_SUBDIR none)
FetchContent_Declare(chesslib GIT_REPOSITORY https://github.com/Disservin/chess-library GIT_TAG 3af102c53752472c208d48921ece705b1f0d1626 SOURCE_SUBDIR none)
FetchContent_Declare(subprocess GIT_REPOSITORY https://github.com/sheredom/subprocess.h GIT_TAG dd28a9cca5eb6783de8bf7879599e04197472fe0 SOURCE_SUBDIR none)
FetchContent_Declare(lunasvg GIT_REPOSITORY https://github.com/sammycage/lunasvg GIT_TAG v3.5.0 GIT_SHALLOW ON)
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lunasvg)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5 SOURCE_SUBDIR none)
FetchContent_Declare(sqlite URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_Declare(nfd GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended GIT_TAG v1.4.1 GIT_SHALLOW ON)
set(NFD_PORTAL ON CACHE BOOL "" FORCE)  # xdg portal on Linux instead of GTK
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio GIT_TAG 0.11.25 GIT_SHALLOW ON SOURCE_SUBDIR none)
FetchContent_MakeAvailable(imgui chesslib subprocess stb sqlite json nfd miniaudio)

# llama.cpp for the local AI coach: static, CPU always; Metal on macOS, Vulkan optional (iGPUs included)
find_package(Vulkan QUIET COMPONENTS glslc)
set(OPCAN_VULKAN_DEFAULT OFF)
if(NOT APPLE AND Vulkan_FOUND AND Vulkan_glslc_FOUND)
  set(OPCAN_VULKAN_DEFAULT ON)
endif()
option(OPCAN_VULKAN "AI coach: Vulkan GPU backend for llama.cpp" ${OPCAN_VULKAN_DEFAULT})
set(GGML_VULKAN ${OPCAN_VULKAN} CACHE BOOL "" FORCE)
if(APPLE)
  set(GGML_METAL ON CACHE BOOL "" FORCE)
  set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
endif()
foreach(o LLAMA_BUILD_COMMON LLAMA_BUILD_TESTS LLAMA_BUILD_TOOLS LLAMA_BUILD_EXAMPLES LLAMA_BUILD_SERVER LLAMA_BUILD_APP
          LLAMA_OPENSSL LLAMA_CURL GGML_BUILD_TESTS GGML_BUILD_EXAMPLES)
  set(${o} OFF CACHE BOOL "" FORCE)
endforeach()
if(MINGW)
  # portable Windows build (packaging/build-windows.sh): x86-64-v2 baseline (SSE4.2/POPCNT, any CPU that runs
  # Windows 11). Per-CPU variants (GGML_CPU_ALL_VARIANTS) need GGML_BACKEND_DL, i.e. ggml as DLLs. No OpenMP: no libgomp.
  set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  set(GGML_SSE42 ON CACHE BOOL "" FORCE)
  set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(llama GIT_REPOSITORY https://github.com/ggml-org/llama.cpp GIT_TAG b11850 GIT_SHALLOW ON)  # b11850+: builds with GCC 14 (std::find include)
FetchContent_MakeAvailable(llama)

add_library(sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)

# opcan_ prefix: llama.cpp vendors its own "miniaudio" target
add_library(opcan_miniaudio STATIC src/miniaudio_impl.c)
target_include_directories(opcan_miniaudio PUBLIC ${miniaudio_SOURCE_DIR})
target_link_libraries(opcan_miniaudio PUBLIC ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
  target_link_libraries(opcan_miniaudio PUBLIC m)
endif()

# ponytail: system glfw on Linux (nix), fetched elsewhere
find_package(glfw3 3.4 QUIET)
if(NOT glfw3_FOUND)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw GIT_TAG 3.4 GIT_SHALLOW ON)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glfw)
endif()
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# core = everything but the window; shared by the app and the self-checks
add_library(opcan_core STATIC src/engine.cpp src/review.cpp src/game.cpp src/openings.cpp src/store.cpp src/stats.cpp src/sync.cpp src/coach.cpp src/play.cpp src/tablebase.cpp src/explorer.cpp src/setup.cpp src/train.cpp src/vision.cpp)
target_include_directories(opcan_core PUBLIC src ${chesslib_SOURCE_DIR}/include ${subprocess_SOURCE_DIR})
target_link_libraries(opcan_core PUBLIC Threads::Threads sqlite3 llama nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})

add_executable(opcan src/main.cpp src/devscript.cpp src/app.cpp src/library.cpp src/coach_ui.cpp src/play_ui.cpp src/stats_ui.cpp src/explorer_ui.cpp src/editor_ui.cpp src/settings_ui.cpp src/tour_ui.cpp src/train_ui.cpp src/vision_ui.cpp src/board.cpp src/theme.cpp src/audio.cpp)
target_link_libraries(opcan PRIVATE opcan_core imgui lunasvg::lunasvg nfd::nfd opcan_miniaudio)
target_include_directories(opcan PRIVATE ${stb_SOURCE_DIR})

add_executable(opcan_check tests/check.cpp tests/check_core.cpp tests/check_review.cpp tests/check_store.cpp tests/check_stats.cpp tests/check_coach.cpp tests/check_play.cpp tests/check_explorer.cpp tests/check_robust.cpp tests/check_train.cpp tests/check_vision.cpp)
target_link_libraries(opcan_check PRIVATE opcan_core)
target_compile_definitions(opcan_check PRIVATE OPCAN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# dev tool: AI coach accuracy harness (tests/coach_eval.cpp); not built by default
add_executable(opcan_coach_eval EXCLUDE_FROM_ALL tests/coach_eval.cpp)
target_link_libraries(opcan_coach_eval PRIVATE opcan_core)
target_compile_definitions(opcan_coach_eval PRIVATE OPCAN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# dev tool: parser fuzzer (tests/fuzz.cpp); build with -fsanitize=address,undefined; not built by default
add_executable(opcan_fuzz EXCLUDE_FROM_ALL tests/fuzz.cpp)
target_link_libraries(opcan_fuzz PRIVATE opcan_core)

# assets live next to the binary (macOS: in the .app's Resources); see resDir() in src/engine.cpp
if(APPLE)
  set(OPCAN_RES_DIR $<TARGET_BUNDLE_CONTENT_DIR:opcan>/Resources)
else()
  set(OPCAN_RES_DIR $<TARGET_FILE_DIR:opcan>)
endif()
add_custom_command(TARGET opcan POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets ${OPCAN_RES_DIR}/assets)

include(packaging/packaging.cmake)
