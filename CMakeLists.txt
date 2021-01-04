cmake_minimum_required(VERSION 3.17 FATAL_ERROR)

set (PROJECT_VERSION "0.0.1")

project(JackTransportLink
	VERSION ${PROJECT_VERSION}
	LANGUAGES CXX
)
set(CMAKE_CXX_STANDARD 17)
set(PROJECT_APP "jack_transport_link")

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build, options are: Debug Release" FORCE)
endif()

# Download automatically, you can also just copy the conan.cmake file
if(NOT EXISTS "${CMAKE_BINARY_DIR}/conan.cmake")
	message(STATUS "Downloading conan.cmake from https://github.com/conan-io/cmake-conan")
	file(DOWNLOAD "https://raw.githubusercontent.com/conan-io/cmake-conan/master/conan.cmake"
		"${CMAKE_BINARY_DIR}/conan.cmake")
endif()

include(${CMAKE_BINARY_DIR}/conan.cmake)

conan_cmake_run(REQUIRES cpp-optparse/cci.20171104
	BASIC_SETUP
	BUILD missing
)

find_library(JACK_LIB "jack" REQUIRED)

include(3rdparty/link/AbletonLinkConfig.cmake)

include_directories(
  ./src/
	./3rdparty/
	./3rdparty/link/include/
)

if (UNIX)
  # for Linux, BSD, Solaris, Minix
  if (NOT APPLE)
		set(PLATFORM_LIBS "-latomic -lpthread")
	endif()
else()
	message(FATAL_ERROR "platform not supported (yet)")
endif()

add_executable(${PROJECT_APP}
	src/main.cpp
  src/JackTransportLink.cpp
)
target_link_libraries(${PROJECT_APP} ${CONAN_LIBS} ${JACK_LIB} ${PLATFORM_LIBS} Ableton::Link)
install(TARGETS ${PROJECT_APP} DESTINATION bin)
