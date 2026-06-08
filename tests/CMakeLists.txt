# 测试可执行文件
add_executable(test_graphstore test_graphstore.cpp)
target_link_libraries(test_graphstore PRIVATE LightGraph)

add_executable(test_btree test_btree.cpp)
target_link_libraries(test_btree PRIVATE LightGraph)

# Cypher Parser 测试
add_executable(test_cypherparser test_cypherparser.cpp)
target_link_libraries(test_cypherparser PRIVATE LightGraph)

# 性能基准测试
add_executable(test_benchmark test_benchmark.cpp)
target_link_libraries(test_benchmark PRIVATE LightGraph)

# 模糊测试和崩溃恢复测试
add_executable(test_fuzz test_fuzz.cpp)
target_link_libraries(test_fuzz PRIVATE LightGraph)
