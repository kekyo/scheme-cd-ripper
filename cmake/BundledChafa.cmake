include(ExternalProject)

set(CHAFA_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps/chafa")
if(NOT EXISTS "${CHAFA_SOURCE_DIR}/configure.ac")
    message(FATAL_ERROR "Chafa sources are missing. Run: git submodule update --init --recursive")
endif()
foreach(tool autoconf automake libtoolize make)
    find_program(CHAFA_${tool}_EXE NAMES ${tool})
    if(NOT CHAFA_${tool}_EXE)
        message(FATAL_ERROR "Missing Chafa build tool: ${tool}. Install autoconf, automake and libtool.")
    endif()
endforeach()
pkg_check_modules(CHAFA_GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.26)

set(CHAFA_SDK_DIR "${CMAKE_BINARY_DIR}/chafa/sdk")
file(MAKE_DIRECTORY "${CHAFA_SDK_DIR}/include/chafa" "${CHAFA_SDK_DIR}/lib/chafa/include")
string(TOUPPER "${CMAKE_BUILD_TYPE}" CHAFA_BUILD_TYPE)
# Chafa 1.14.2 contains unused optional helpers, tables and macros. Keep this
# exception local to the unmodified dependency; cdrip's warnings stay enabled.
set(CHAFA_CFLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${CHAFA_BUILD_TYPE}} -Wno-unused-function -Wno-unused-variable -Wno-unused-const-variable -Wno-unused-macros")

ExternalProject_Add(chafa_build
    PREFIX "${CMAKE_BINARY_DIR}/chafa"
    SOURCE_DIR "${CMAKE_BINARY_DIR}/chafa/source"
    BINARY_DIR "${CMAKE_BINARY_DIR}/chafa/build"
    INSTALL_DIR "${CHAFA_SDK_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        "CC=${CMAKE_C_COMPILER}" "CFLAGS=${CHAFA_CFLAGS}"
        "AR=${CMAKE_AR}" "RANLIB=${CMAKE_RANLIB}"
        "PKG_CONFIG=${PKG_CONFIG_EXECUTABLE}"
        <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --libdir=<INSTALL_DIR>/lib
        --without-tools --disable-man --disable-static --enable-shared
    BUILD_BYPRODUCTS "${CHAFA_SDK_DIR}/lib/libchafa.so"
)

# Bootstrap a copy, so neither generated files nor builds modify the submodule.
file(GLOB_RECURSE CHAFA_INPUTS CONFIGURE_DEPENDS "${CHAFA_SOURCE_DIR}/*")
ExternalProject_Add_Step(chafa_build bootstrap
    COMMAND ${CMAKE_COMMAND}
        "-DCHAFA_SOURCE_DIR=${CHAFA_SOURCE_DIR}"
        "-DCHAFA_COPY_DIR=${CMAKE_BINARY_DIR}/chafa/source"
        -P "${CMAKE_CURRENT_LIST_DIR}/PrepareChafa.cmake"
    DEPENDEES download
    DEPENDERS configure
    DEPENDS ${CHAFA_INPUTS} "${CMAKE_CURRENT_LIST_DIR}/PrepareChafa.cmake"
)

add_library(cdrip_chafa SHARED IMPORTED)
set_target_properties(cdrip_chafa PROPERTIES
    IMPORTED_LOCATION "${CHAFA_SDK_DIR}/lib/libchafa.so"
    INTERFACE_INCLUDE_DIRECTORIES "${CHAFA_SDK_DIR}/include/chafa;${CHAFA_SDK_DIR}/lib/chafa/include"
    INTERFACE_LINK_LIBRARIES PkgConfig::CHAFA_GLIB
)
add_dependencies(cdrip_chafa chafa_build)

install(DIRECTORY "${CHAFA_SDK_DIR}/lib/" DESTINATION lib/cdrip
    COMPONENT cli FILES_MATCHING PATTERN "libchafa.so.*"
    PATTERN chafa EXCLUDE PATTERN pkgconfig EXCLUDE)
install(FILES "${CHAFA_SOURCE_DIR}/COPYING" "${CHAFA_SOURCE_DIR}/COPYING.LESSER"
    DESTINATION share/doc/cdrip/chafa COMPONENT cli)
install(FILES
    "${CHAFA_SOURCE_DIR}/chafa/internal/smolscale/COPYING"
    "${CHAFA_SOURCE_DIR}/chafa/internal/smolscale/COPYING.LGPLv3"
    "${CHAFA_SOURCE_DIR}/chafa/internal/smolscale/COPYING.BSD-4"
    DESTINATION share/doc/cdrip/chafa/smolscale COMPONENT cli)
install(FILES "${CMAKE_SOURCE_DIR}/THIRD_PARTY.md" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_ja.md"
    "${CMAKE_SOURCE_DIR}/README.md" "${CMAKE_SOURCE_DIR}/README_ja.md"
    DESTINATION share/doc/cdrip COMPONENT cli)
