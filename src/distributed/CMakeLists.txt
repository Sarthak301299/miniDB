target_sources(miniDB PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/network_utils.cpp
)

target_include_directories(miniDB PUBLIC ${CMAKE_SOURCE_DIR}/include/distributed)

add_executable(launcher launcher_main.cpp)
target_sources(launcher PRIVATE
launcher.cpp
network_utils.cpp)
target_include_directories(launcher PUBLIC ${CMAKE_SOURCE_DIR}/include/distributed)

set_target_properties(launcher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    INSTALL_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
)

install(TARGETS launcher
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

add_executable(server server_main.cpp)
target_sources(server PRIVATE
db_server.cpp
network_utils.cpp)
target_include_directories(launcher PUBLIC ${CMAKE_SOURCE_DIR}/include/distributed)
target_link_libraries(server PRIVATE miniDB)

set_target_properties(server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    INSTALL_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
)

install(TARGETS server
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)