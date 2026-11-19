file(REMOVE_RECURSE "${CHAFA_COPY_DIR}")
file(MAKE_DIRECTORY "${CHAFA_COPY_DIR}")
file(COPY "${CHAFA_SOURCE_DIR}/" DESTINATION "${CHAFA_COPY_DIR}" PATTERN .git EXCLUDE)
# Upstream intentionally retains AC_PROG_CC_STDC for older C99 toolchains.
# See the comment preceding that macro in Chafa's configure.ac.
execute_process(COMMAND ${CMAKE_COMMAND} -E env NOCONFIGURE=1 WARNINGS=no-obsolete
    sh ./autogen.sh WORKING_DIRECTORY "${CHAFA_COPY_DIR}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Chafa bootstrap failed: ${status}")
endif()
