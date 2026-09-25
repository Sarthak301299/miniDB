add_executable(test_query_engine test_query_engine.cpp)

target_link_libraries(test_query_engine PRIVATE miniDB)

set_target_properties(test_query_engine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    INSTALL_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
)

install(TARGETS test_query_engine
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)