add_executable(benchmark benchmark.cpp)

target_sources(benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/src/distributed/network_utils.cpp
)

target_include_directories(benchmark PUBLIC ${CMAKE_SOURCE_DIR}/include/distributed)

set_target_properties(benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    INSTALL_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
)

install(TARGETS benchmark
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(PROGRAMS run_bench.sh
    DESTINATION bin
 )