cmake_minimum_required(VERSION 3.12)
project(ws_rate_limiter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Prefer static linking of libstdc++ if possible to ease running
option(STATIC_STDLIB "Link libstdc++ statically" OFF)
if(STATIC_STDLIB)
	add_link_options(-static-libstdc++ -static-libgcc)
endif()

find_package(Boost QUIET COMPONENTS system thread)

if(Boost_FOUND)
	add_executable(ws_server
		ws_server.cpp
	)
	target_include_directories(ws_server PRIVATE ${Boost_INCLUDE_DIRS})
	target_link_libraries(ws_server PRIVATE ${Boost_LIBRARIES})
else()
	message(WARNING "Boost not found. Skipping ws_server target. Install Boost (headers + system, thread) to build the WebSocket server.")
endif()

# Optional: build existing tests in the repository
add_executable(tests
	main.cpp
	tests.cpp
	test_runner.cpp
)

# No special libs needed for tests