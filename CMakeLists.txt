cmake_minimum_required(VERSION 3.20)
project(LightGraph VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_DEBUG_POSTFIX d)

# 编译选项
if(MSVC)
    add_compile_options(/W4 /WX- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -O2)
endif()

# 源文件
set(SOURCES
    src/page_manager.cpp
    src/bplus_tree.cpp
    src/graph_store.cpp
    src/serializer.cpp
    src/cypher_parser.cpp
)

# 头文件
set(HEADERS
    include/common.h
    include/page_manager.h
    include/bplus_tree.h
    include/graph_store.h
    include/serializer.h
    include/cypher_parser.h
)

# 静态库
add_library(LightGraph STATIC 
    ${SOURCES} 
    ${HEADERS}
)
target_include_directories(LightGraph PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_subdirectory(cmds)
add_subdirectory(examples)
add_subdirectory(tests)

# Install
include(GNUInstallDirs)
install(TARGETS LightGraph
    EXPORT LightGraphTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
install(TARGETS db_admin cypher_parser
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(EXPORT LightGraphTargets
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lightgraph
    NAMESPACE LightGraph::
)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/LightGraphConfig.cmake
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/LightGraphTargets.cmake\")\n"
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/LightGraphConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LightGraph
)
