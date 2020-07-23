cmake_minimum_required(VERSION 3.5)

SET(BINARY_NAME portlink)
project (${BINARY_NAME} LANGUAGES C)

find_library(IOKit IOKit)
find_library(CoreFoundation CoreFoundation)

include_directories("${CoreFoundation}/Headers")
include_directories("${IOKit}/Headers")

SET(EXTRA_LIBS ${IOKit} ${CoreFoundation})

IF( CMAKE_BUILD_TYPE MATCHES Debug )
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -DDEBUG -O0 -g3 -ggdb")
    message("Debug build enabled")
ELSE()
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O3")
    message("Release build enabled")
ENDIF()

file(GLOB_RECURSE SOURCES RELATIVE ${PROJECT_SOURCE_DIR} "src/*.c")
add_executable(${BINARY_NAME} ${SOURCES})
target_link_libraries(${BINARY_NAME} ${EXTRA_LIBS})
