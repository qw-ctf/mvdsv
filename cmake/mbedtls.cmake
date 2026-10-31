# Mbed TLS for the server, fetched here and built as static libraries: the
# 3.6 LTS release, whose API libdatachannel uses.
#
# Gives MbedTLS::MbedTLS (TLS, X.509 and crypto), which the server links, and
# which libdatachannel takes instead of looking for Mbed TLS itself.
#
# Before the first include, add the configuration options (MBEDTLS_...) the
# includer needs on top of Mbed TLS's defaults to MVDSV_MBEDTLS_OPTIONS: they
# go into a generated MBEDTLS_USER_CONFIG_FILE. MVDSV_MBEDTLS_OPTIONS_USED
# then says which ones it was built with.

include_guard(GLOBAL)

set(MVDSV_MBEDTLS_VERSION 3.6.7)
set(MVDSV_MBEDTLS_SHA256 a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)

list(REMOVE_DUPLICATES MVDSV_MBEDTLS_OPTIONS)
set(MVDSV_MBEDTLS_OPTIONS_USED ${MVDSV_MBEDTLS_OPTIONS})

function(mvdsv_fetch_mbedtls)
	include(FetchContent)

	# for the fetched project only, in this function's scope
	set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # plain variables override its option()s
	set(BUILD_SHARED_LIBS OFF)

	set(exclude)
	if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
		set(exclude EXCLUDE_FROM_ALL) # build only what is linked
	endif()

	set(config "${CMAKE_BINARY_DIR}/mbedtls_user_config.h")
	set(text "/* Written by cmake/mbedtls.cmake: options mvdsv needs on top of Mbed TLS's defaults. */\n")
	foreach(option IN LISTS MVDSV_MBEDTLS_OPTIONS)
		string(APPEND text "#ifndef ${option}\n#define ${option}\n#endif\n")
	endforeach()
	file(CONFIGURE OUTPUT "${config}" CONTENT "${text}" @ONLY) # rewritten only when it changes

	set(ENABLE_PROGRAMS OFF)
	set(ENABLE_TESTING OFF)
	set(MBEDTLS_FATAL_WARNINGS OFF)
	set(USE_STATIC_MBEDTLS_LIBRARY ON)
	set(USE_SHARED_MBEDTLS_LIBRARY OFF)
	# Keep its export set even as a subproject: libdatachannel's install(EXPORT)
	# refers to the mbedtls target and fails to generate without it.
	set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF)
	set(MBEDTLS_USER_CONFIG_FILE "${config}" CACHE FILEPATH "Mbed TLS user config file (appended to default)." FORCE)

	# the release tarball: it carries the generated sources, so no Python is needed
	FetchContent_Declare(mbedtls
		URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MVDSV_MBEDTLS_VERSION}/mbedtls-${MVDSV_MBEDTLS_VERSION}.tar.bz2"
		URL_HASH SHA256=${MVDSV_MBEDTLS_SHA256}
		${exclude})
	FetchContent_MakeAvailable(mbedtls)
endfunction()

mvdsv_fetch_mbedtls()
add_library(MbedTLS::MbedTLS ALIAS mbedtls)
message(STATUS "Mbed TLS: building ${MVDSV_MBEDTLS_VERSION}")
