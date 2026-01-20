cmake_minimum_required(VERSION 3.14)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(ncmdump_wasm CXX)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(WITH_ZLIB OFF)
set(BUILD_TESTING OFF)

add_subdirectory(taglib)

FILE(GLOB COMMON_HEADERS src/include/*.h)
FILE(GLOB COMMON_SOURCES src/ncmcrypt.cpp src/utils/*.cpp)
FILE(GLOB LIBRARY_HEADERS src/lib/libncmdump.h)
FILE(GLOB LIBRARY_SOURCES src/lib/*.cpp)

# Emscripten 绑定接口的源文件
set(WASM_INTERFACE_SOURCE src/wasm_interface.cpp)

if(EMSCRIPTEN)
    message(STATUS "Configuring WebAssembly build with Emscripten")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")

    add_executable(ncmdump_wasm
        ${COMMON_SOURCES}
        ${LIBRARY_SOURCES}
        ${WASM_INTERFACE_SOURCE}
    )

    target_include_directories(ncmdump_wasm PRIVATE
        src/include
        taglib
        taglib/taglib
        taglib/taglib/toolkit
        taglib/taglib/mpeg/id3v2
    )

    target_link_libraries(ncmdump_wasm tag)

    set_target_properties(ncmdump_wasm PROPERTIES OUTPUT_NAME "ncmdump")

    target_compile_options(ncmdump_wasm PRIVATE
        -frtti
        -fexceptions
    )

    target_link_options(ncmdump_wasm PRIVATE
        --bind
        -fexceptions
        -sMODULARIZE=1
        -sEXPORT_NAME="createNCMDumpModule"
        -sEXPORT_ES6=1
        -sUSE_ES6_IMPORT_META=1
        -sENVIRONMENT=web
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORTED_FUNCTIONS=['_malloc','_free']
        -sEXPORTED_RUNTIME_METHODS=['HEAPU8']
    )

    install(TARGETS ncmdump_wasm
        RUNTIME DESTINATION wasm
        LIBRARY DESTINATION wasm
        ARCHIVE DESTINATION wasm
    )
endif()