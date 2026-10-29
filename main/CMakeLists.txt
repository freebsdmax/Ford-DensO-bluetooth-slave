set(COMPONENT_SRCS "a2dp-slave.c" "avrc_media.c" "uart_ota_slave.c" "uart_ota_crc.c")
set(COMPONENT_ADD_INCLUDEDIRS "")

register_component()

# Firmware version = git tag (CI must use fetch-depth: 0)
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    OUTPUT_VARIABLE GIT_DESC
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT GIT_DESC)
    set(GIT_DESC "dev")
endif()
message(STATUS "Slave firmware version: ${GIT_DESC}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE SLAVE_FW_VERSION="${GIT_DESC}")
