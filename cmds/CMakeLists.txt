add_executable(db_admin db_admin.cpp)
target_link_libraries(db_admin PRIVATE LightGraph)

add_executable(cypher_parser cypher_parser.cpp)
target_link_libraries(cypher_parser PRIVATE LightGraph)
