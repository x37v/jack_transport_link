cmake_minimum_required(VERSION 3.17 FATAL_ERROR)

set (PROJECT_VERSION "0.0.1")

project(jack_transport_link
	VERSION ${PROJECT_VERSION}
	LANGUAGES CXX
)
set(CMAKE_CXX_STANDARD 17)
set(PROJECT_APP "jack_transport_link")

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build, options are: Debug Release" FORCE)
endif()

if (UNIX AND NOT APPLE)
	set(LINUX TRUE)
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
  if (LINUX)
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

if (LINUX)
	#setup deb build
	IF(EXISTS "${CMAKE_ROOT}/Modules/CPack.cmake")
		INCLUDE(InstallRequiredSystemLibraries)

		SET(CPACK_GENERATOR "DEB")

		SET(CPACK_PACKAGE_DESCRIPTION "A service that bridges Ableton's Link to and from Jack Transport, allowing applications that use Jack Transport to synchronize their timing with other applications that support Link.")
		SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A service that bridges Ableton's Link to and from Jack Transport.")
    SET(CPACK_PACKAGE_VENDOR "Alex Norman")
		SET(CPACK_PACKAGE_CONTACT "alex@x37v.info")

    SET(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}")

		SET(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}_${CPACK_DEBIAN_PACKAGE_VERSION}")
		SET(CPACK_SOURCE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")

		SET(CPACK_DEBIAN_PACKAGE_DEPENDS "libjack-jackd2-0 (>= 1.9.12)")

		SET(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
		SET(CPACK_DEBIAN_PACKAGE_SECTION "sound")

    #		set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    #			"${CMAKE_CURRENT_SOURCE_DIR}/debian/prerm;${CMAKE_CURRENT_SOURCE_DIR}/debian/postrm;${CMAKE_CURRENT_SOURCE_DIR}/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/debian/conffiles")
    #
		INCLUDE(CPack)
	ENDIF(EXISTS "${CMAKE_ROOT}/Modules/CPack.cmake")
endif()
