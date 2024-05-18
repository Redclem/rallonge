cmake_minimum_required(VERSION 3.20)

project(rallonge)


add_executable(rallonge main.cpp server.cpp app_base.cpp client.cpp)
set_property(TARGET rallonge PROPERTY CXX_STANDARD 17)

if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -fpermissive")
else()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
endif()

if(WIN32)

target_link_libraries(rallonge Ws2_32.lib)

endif()
