# libdatachannel for the WebRTC transport (src/sv_webrtc.c), fetched here and
# built as a static library on Mbed TLS (cmake/mbedtls.cmake), with its own
# libjuice, usrsctp and plog.
#
# It is a master commit, for the PEM fields of the C API's rtcConfiguration
# (upstream commit 5b63d8c, in no release yet), with a patch of ours,
# patches/libdatachannel-websocket-ca.patch: the C API's rtcWsConfiguration
# takes the CA certificates to check a wss:// server's with
# (caCertificatePemFile), since Mbed TLS cannot find the system's, and Windows
# then checks with them too.
#
# Include it before cmake/mbedtls.cmake: it needs more of Mbed TLS.

include_guard(GLOBAL)

set(MVDSV_LIBDATACHANNEL_COMMIT bdc5ff28e9d3b863144c94a677ecf5bf043aaf15) # master, 2026-10-07
set(MVDSV_PATCHES ${CMAKE_CURRENT_LIST_DIR}/patches)
set(MVDSV_LIBDATACHANNEL_PATCH ${MVDSV_PATCHES}/libdatachannel-websocket-ca.patch)

# C++17, on Mbed TLS with DTLS-SRTP, which libdatachannel's DTLS transport
# sets up even without media
enable_language(CXX)
if(DEFINED MVDSV_MBEDTLS_OPTIONS_USED AND NOT "MBEDTLS_SSL_DTLS_SRTP" IN_LIST MVDSV_MBEDTLS_OPTIONS_USED)
	message(FATAL_ERROR "cmake/mbedtls.cmake was included before cmake/libdatachannel.cmake, "
		"so its Mbed TLS was configured without MBEDTLS_SSL_DTLS_SRTP; include this one first.")
endif()
list(APPEND MVDSV_MBEDTLS_OPTIONS MBEDTLS_SSL_DTLS_SRTP)
include(${CMAKE_CURRENT_LIST_DIR}/mbedtls.cmake)

find_package(Git REQUIRED)

function(mvdsv_fetch_libdatachannel)
	include(FetchContent)

	# for the fetched project only, in this function's scope
	set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # plain variables override its option()s
	set(BUILD_SHARED_LIBS OFF)
	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)

	set(exclude)
	if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
		set(exclude EXCLUDE_FROM_ALL) # build only what is linked
	endif()

	# data channels and its WebSocket client (for the broker), no media; it
	# finds Mbed TLS as MbedTLS::MbedTLS
	set(USE_MBEDTLS ON)
	set(NO_MEDIA ON)
	set(NO_WEBSOCKET OFF)
	set(NO_EXAMPLES ON)
	set(NO_TESTS ON)
	FetchContent_Declare(libdatachannel
		GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
		GIT_TAG ${MVDSV_LIBDATACHANNEL_COMMIT}
		GIT_SUBMODULES deps/libjuice deps/usrsctp deps/plog
		GIT_CONFIG core.autocrlf=false # LF on every system, as the patch has it
		PATCH_COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} -DPATCH=${MVDSV_LIBDATACHANNEL_PATCH}
			-P ${MVDSV_PATCHES}/apply.cmake
		${exclude})
	FetchContent_MakeAvailable(libdatachannel)

	# a copy given with FETCHCONTENT_SOURCE_DIR_LIBDATACHANNEL is not patched here
	file(READ "${libdatachannel_SOURCE_DIR}/include/rtc/rtc.h" header)
	foreach(field certificatePemFile caCertificatePemFile)
		string(FIND "${header}" "${field}" found)
		if(found EQUAL -1)
			message(FATAL_ERROR "libdatachannel in ${libdatachannel_SOURCE_DIR} has no ${field} in its C API: "
				"it needs commit ${MVDSV_LIBDATACHANNEL_COMMIT} or later with ${MVDSV_LIBDATACHANNEL_PATCH}.")
		endif()
	endforeach()
endfunction()

mvdsv_fetch_libdatachannel()
message(STATUS "libdatachannel: building ${MVDSV_LIBDATACHANNEL_COMMIT}, patched")
