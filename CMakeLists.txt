cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBILBC_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_ilbc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ilbc_stubs.c
)

SET(LIBILBC_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libilbc.a
)

SET(LIBILBC_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libilbc
        "${LIBILBC_SRC}"
        "${LIBILBC_LIBS}"
        []
        ""
        "${LIBILBC_INC}"
        ""
        "1")
