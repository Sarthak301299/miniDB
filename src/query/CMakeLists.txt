target_sources(miniDB PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/value.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/catalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/executor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/engine.cpp
)

target_include_directories(miniDB PUBLIC ${CMAKE_SOURCE_DIR}/include/query)