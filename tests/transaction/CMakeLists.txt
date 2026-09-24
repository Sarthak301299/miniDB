add_executable(test_transaction_cases test_transaction_cases.cpp)

target_link_libraries(test_transaction_cases PRIVATE miniDB)

set_target_properties(test_transaction_cases PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    INSTALL_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
)

install(TARGETS test_transaction_cases
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)