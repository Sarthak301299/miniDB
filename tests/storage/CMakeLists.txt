add_executable(failure_handling failure_handling.cpp)

target_link_libraries(failure_handling PRIVATE miniDB)

set_target_properties(failure_handling PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    INSTALL_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
)

install(TARGETS failure_handling
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)