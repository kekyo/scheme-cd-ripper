cmake_minimum_required(VERSION 3.16)
project(cd_ripper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(SCREW_UP_EXE screw-up)
if(NOT SCREW_UP_EXE)
    message(FATAL_ERROR "screw-up not found; install it (e.g., npm install -g screw-up) or ensure it is on PATH.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CDIO REQUIRED libcdio_paranoia)
pkg_check_modules(CDDB REQUIRED libcddb)
# pkg-config name is lowercase 'flac++'
pkg_check_modules(FLACPP REQUIRED flac++)
pkg_check_modules(GIO REQUIRED gio-2.0)
pkg_check_modules(SOUP REQUIRED libsoup-3.0)
pkg_check_modules(JSONGLIB REQUIRED json-glib-1.0)
pkg_check_modules(PNG REQUIRED libpng)
pkg_check_modules(JPEG REQUIRED libjpeg)
pkg_check_modules(LCMS2 REQUIRED lcms2)

set(VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/version.h)
get_filename_component(VERSION_DIR ${VERSION_HEADER} DIRECTORY)

add_custom_target(version_header ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VERSION_DIR}
    COMMAND /bin/bash -c "set -euo pipefail; \"${SCREW_UP_EXE}\" format -f -i \"${CMAKE_SOURCE_DIR}/version.h.in\" \"${VERSION_HEADER}\" >/dev/null"
    COMMAND /bin/bash -c "set -euo pipefail; sed -i 's/{version}/0.1.0/g; s/{git.commit.hash}/unknown/g' \"${VERSION_HEADER}\""
    BYPRODUCTS ${VERSION_HEADER}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating version header with screw-up format"
    VERBATIM
)

set(COMMON_INCLUDES
    ${CDIO_INCLUDE_DIRS}
    ${CDDB_INCLUDE_DIRS}
    ${FLACPP_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${SOUP_INCLUDE_DIRS}
    ${JSONGLIB_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS}
    ${JPEG_INCLUDE_DIRS}
    ${LCMS2_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VERSION_DIR}
)

set(COMMON_LIB_DIRS
    ${CDIO_LIBRARY_DIRS}
    ${CDDB_LIBRARY_DIRS}
    ${FLACPP_LIBRARY_DIRS}
    ${GIO_LIBRARY_DIRS}
    ${SOUP_LIBRARY_DIRS}
    ${JSONGLIB_LIBRARY_DIRS}
    ${PNG_LIBRARY_DIRS}
    ${JPEG_LIBRARY_DIRS}
    ${LCMS2_LIBRARY_DIRS}
)

set(COMMON_LIBS
    ${CDIO_LIBRARIES}
    ${CDDB_LIBRARIES}
    ${FLACPP_LIBRARIES}
    ${GIO_LIBRARIES}
    ${SOUP_LIBRARIES}
    ${JSONGLIB_LIBRARIES}
    ${PNG_LIBRARIES}
    ${JPEG_LIBRARIES}
    ${LCMS2_LIBRARIES}
)

set(COMMON_CFLAGS
    ${CDIO_CFLAGS_OTHER}
    ${CDDB_CFLAGS_OTHER}
    ${FLACPP_CFLAGS_OTHER}
    ${GIO_CFLAGS_OTHER}
    ${SOUP_CFLAGS_OTHER}
    ${JSONGLIB_CFLAGS_OTHER}
    ${PNG_CFLAGS_OTHER}
    ${JPEG_CFLAGS_OTHER}
    ${LCMS2_CFLAGS_OTHER}
    -Wall
    -Wextra
    -pedantic
)

set(CDRIP_SOURCES
    src/cdrip/config.cpp
    src/cdrip/cddb_entries.cpp
    src/cdrip/flac_metadata.cpp
    src/cdrip/drives.cpp
    src/cdrip/drive_handle.cpp
    src/cdrip/disc_toc.cpp
    src/cdrip/rip.cpp
    src/cdrip/error.cpp
    src/cdrip/cover_art.cpp
    src/cdrip/timestamp.cpp
)

add_library(cdrip_static STATIC ${CDRIP_SOURCES})
set_target_properties(cdrip_static PROPERTIES OUTPUT_NAME cdrip)
target_include_directories(cdrip_static PUBLIC ${COMMON_INCLUDES})
target_link_directories(cdrip_static PRIVATE ${COMMON_LIB_DIRS})
target_compile_options(cdrip_static PRIVATE ${COMMON_CFLAGS})
target_link_libraries(cdrip_static PUBLIC ${COMMON_LIBS})
add_dependencies(cdrip_static version_header)

add_library(cdrip SHARED ${CDRIP_SOURCES})
set_target_properties(cdrip PROPERTIES OUTPUT_NAME cdrip)
target_include_directories(cdrip PUBLIC ${COMMON_INCLUDES})
target_link_directories(cdrip PRIVATE ${COMMON_LIB_DIRS})
target_compile_options(cdrip PRIVATE ${COMMON_CFLAGS})
target_link_libraries(cdrip PUBLIC ${COMMON_LIBS})
add_dependencies(cdrip version_header)

add_executable(cdrip_app src/main.cpp)
set_target_properties(cdrip_app PROPERTIES OUTPUT_NAME cdrip)
target_include_directories(cdrip_app PRIVATE ${COMMON_INCLUDES} ${VERSION_DIR})
target_link_directories(cdrip_app PRIVATE ${COMMON_LIB_DIRS})
target_compile_options(cdrip_app PRIVATE ${COMMON_CFLAGS})
target_link_libraries(cdrip_app PRIVATE cdrip_static)
add_dependencies(cdrip_app version_header)
