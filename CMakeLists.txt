cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(PROJECT_VERSION "0.0.9")
set(CPACK_DEBIAN_PACKAGE_RELEASE 1) #set to 1 if we inc PROJECT_VERSION, incr if we need to package new with same PROJECT_VERSION

project(jack_transport_link
  VERSION ${PROJECT_VERSION}
  LANGUAGES CXX
  )
set(CMAKE_CXX_STANDARD 17)
set(PROJECT_APP "jack_transport_link")

set(INSTALL_SERVICE_FILE ON CACHE BOOL "Should we install a service file")

set(CONAN_PROFILE "default" CACHE STRING "The profile to use for building conan deps, this is useful for cross compiling")
set(JACK_DIR "" CACHE FILEPATH "optional path to specify location for JACK libs/includes")
mark_as_advanced(JACK_DIR)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build, options are: Debug Release" FORCE)
endif()

if (UNIX AND NOT APPLE)
  set(LINUX TRUE)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/conan.cmake")

#to get find_package to work with conan generated packages
list(APPEND CMAKE_MODULE_PATH ${CMAKE_BINARY_DIR})
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR})

conan_cmake_configure(
	REQUIRES cpp-optparse/cci.20171104
	GENERATORS cmake_find_package)
conan_cmake_install(
	PATH_OR_REFERENCE .
	BUILD missing
	SETTINGS_HOST build_type=${CMAKE_BUILD_TYPE}
	SETTINGS_BUILD build_type=${CMAKE_BUILD_TYPE}
	PROFILE_HOST ${CONAN_PROFILE}
	PROFILE_BUILD default
)

find_package(cpp-optparse REQUIRED)
find_package(PkgConfig REQUIRED)

if (JACK_DIR)
	find_library(JACK_LIB "jack"
		PATHS ${JACK_DIR}/lib/
		NO_DEFAULT_PATH
		NO_CMAKE_FIND_ROOT_PATH
	)
	find_path(JACK_INCLUDE_DIR jack/jack.h REQUIRED
		PATHS ${JACK_DIR}/include/
		NO_DEFAULT_PATH
		NO_CMAKE_FIND_ROOT_PATH
	)
else()
	find_library(JACK_LIB "jack")
	find_path(JACK_INCLUDE_DIR jack/jack.h REQUIRED)
endif()

include(3rdparty/link/AbletonLinkConfig.cmake)

include_directories(
  ./src/
  ./3rdparty/link/include/
  ${cpp-optparse_INCLUDE_DIRS}
	${JACK_INCLUDE_DIR}
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
target_link_libraries(
  ${PROJECT_APP}
  PRIVATE
  ${cpp-optparse_LIBS}
  ${PLATFORM_LIBS}
  Ableton::Link
	${JACK_LIB}
)

install(TARGETS ${PROJECT_APP} DESTINATION bin)

if (LINUX)

  if (INSTALL_SERVICE_FILE)
    install(FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/config/systemd.service.local
      RENAME "${PROJECT_APP}.service"
      DESTINATION lib/systemd/system/
      COMPONENT local
    )

    install(FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/config/systemd.service.usr
      RENAME "${PROJECT_APP}.service"
      DESTINATION lib/systemd/system/
      COMPONENT usr
      EXCLUDE_FROM_ALL
    )
  endif()

  #setup deb build
  IF(EXISTS "${CMAKE_ROOT}/Modules/CPack.cmake")
    INCLUDE(InstallRequiredSystemLibraries)

    SET(CPACK_GENERATOR "DEB")

    SET(CPACK_PACKAGE_DESCRIPTION "A service that bridges Ableton's Link to and from Jack Transport, allowing applications that use Jack Transport to synchronize their timing with other applications that support Link.")
    SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A service that bridges Ableton's Link to and from Jack Transport.")
    SET(CPACK_PACKAGE_VENDOR "Alex Norman")
    SET(CPACK_PACKAGE_CONTACT "alex@x37v.info")
    SET(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/x37v/jack_transport_link")

    SET(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}")

    #https://github.com/Kitware/CMake/blob/70413d2c3501fe229e9b2d46c745aca5c9f2dc4a/Modules/Internal/CPack/CPackDeb.cmake#L451-L463
    if (NOT CPACK_DEBIAN_PACKAGE_ARCHITECTURE)
      find_program(DPKG_CMD dpkg)
      if(NOT DPKG_CMD)
        message(STATUS "CPackDeb: Can not find dpkg in your path, default to i386.")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE i386)
      endif()
      execute_process(COMMAND "${DPKG_CMD}" --print-architecture
        OUTPUT_VARIABLE CPACK_DEBIAN_PACKAGE_ARCHITECTURE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif()

    SET(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}_${CPACK_DEBIAN_PACKAGE_VERSION}-${CPACK_DEBIAN_PACKAGE_RELEASE}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}")
    SET(CPACK_SOURCE_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}_${CPACK_DEBIAN_PACKAGE_VERSION}")

    SET(CPACK_DEBIAN_PACKAGE_DEPENDS "libjack-jackd2-0 (>= 1.9.12)")

    SET(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    SET(CPACK_DEBIAN_PACKAGE_SECTION "sound")

		#remove the local component from the deb as it references /usr/ not /usr/local/
		get_cmake_property(CPACK_COMPONENTS_ALL COMPONENTS)
		message(STATUS "components ${CPACK_COMPONENTS_ALL}")
		list(REMOVE_ITEM CPACK_COMPONENTS_ALL "local")
		SET(CPACK_DEB_COMPONENT_INSTALL ON)
		SET(CPACK_COMPONENTS_GROUPING "ALL_COMPONENTS_IN_ONE")

    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/debian/copyright")
    if (INSTALL_SERVICE_FILE)
      set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA};${CMAKE_CURRENT_SOURCE_DIR}/debian/prerm;${CMAKE_CURRENT_SOURCE_DIR}/debian/postrm;${CMAKE_CURRENT_SOURCE_DIR}/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/debian/conffiles")
    endif()

    INCLUDE(CPack)
  ENDIF(EXISTS "${CMAKE_ROOT}/Modules/CPack.cmake")
endif()
