cmake_minimum_required(VERSION 3.5)
set(TARGET_NAME rekordbox)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME} CXX)
set(CMAKE_CXX_STANDARD 17)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/rekordbox_extension.cpp
    src/audio_decode.cpp
    src/analysis.cpp
    src/essentia_analysis.cpp
    src/analyze_table.cpp
    src/waveform.cpp
    src/anlz_writer.cpp
    src/pdb_writer.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# ---- dependencies ---------------------------------------------------------
# FFmpeg (decode) from the system (Homebrew) via pkg-config — avoids a long
# vcpkg source build. `brew install ffmpeg`.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswresample)

# libKeyFinder (key) + aubio (tempo/beats): not in vcpkg -> found from the
# system (Homebrew). Build once: `brew install libkeyfinder aubio`.
find_path(KEYFINDER_INCLUDE keyfinder/keyfinder.h PATHS /opt/homebrew/include /usr/local/include)
find_library(KEYFINDER_LIB keyfinder PATHS /opt/homebrew/lib /usr/local/lib)
find_library(AUBIO_LIB aubio PATHS /opt/homebrew/lib /usr/local/lib)
find_library(EBUR128_LIB ebur128 PATHS /opt/homebrew/lib /usr/local/lib)

# Essentia (rhythm/danceability/descriptors) is OPTIONAL and source-built into
# ../deps/essentia/install (lightweight static: KISS FFT, no ffmpeg/taglib/gaia).
# Build once: see ESSENTIA_INTEGRATION.md, then `make release WITH_ESSENTIA=ON`.
option(WITH_ESSENTIA "Link Essentia for rhythm + high-level descriptors" OFF)
if(WITH_ESSENTIA)
  # NOTE: under DuckDB's extension build, CMAKE_SOURCE_DIR is the *duckdb* root,
  # so anchor on this CMakeLists' own dir (ext-rekordbox) instead.
  set(ESSENTIA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../deps/essentia/install)
  find_library(ESSENTIA_LIB essentia PATHS ${ESSENTIA_ROOT}/lib REQUIRED)
  find_path(ESSENTIA_INC essentia/algorithmfactory.h PATHS ${ESSENTIA_ROOT}/include REQUIRED)
  # Essentia headers pull in Eigen (header-only, from Homebrew).
  find_path(EIGEN_INC eigen3/Eigen/Core PATHS /opt/homebrew/include /usr/local/include REQUIRED)
  add_compile_definitions(HAVE_ESSENTIA)
endif()

set(LINK_LIBS PkgConfig::FFMPEG ${KEYFINDER_LIB} ${AUBIO_LIB} ${EBUR128_LIB})
if(WITH_ESSENTIA)
  list(APPEND LINK_LIBS ${ESSENTIA_LIB})
endif()

foreach(t ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  target_include_directories(${t} PRIVATE ${FFMPEG_INCLUDE_DIRS} ${KEYFINDER_INCLUDE})
  if(WITH_ESSENTIA)
    target_include_directories(${t} PRIVATE ${ESSENTIA_INC} ${EIGEN_INC}/eigen3)
  endif()
  target_link_libraries(${t} ${LINK_LIBS})
endforeach()

install(TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
