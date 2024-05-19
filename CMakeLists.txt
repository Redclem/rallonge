cmake_minimum_required(VERSION 3.20)

project(rallonge)

set(CMAKE_EXPORT_COMPILE_COMMANDS True)

add_executable(rallonge main.cpp server.cpp app_base.cpp client.cpp)
set_property(TARGET rallonge PROPERTY CXX_STANDARD 20)

if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
else()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
endif()

if(WIN32)

target_link_libraries(rallonge Ws2_32.lib)

endif()

if(${CMAKE_BUILD_TYPE} STREQUAL "Release")
# Optimisation
if(WIN32)
	set("${CMAKE_CXX_FLAGS} /O2 /Oy")
else()
	set("${CMAKE_CXX_FLAGS} -O3")
endif()

endif()
