# Vendors libcurl from source via CMake FetchContent so HTTP/HTTPS support
# (including S3 presigned URLs) works without requiring a system-installed
# curl dev package, and so the plugin doesn't ship an extra DLL alongside it
# into the host process (see CMakeLists.txt's static-CRT-linking rationale --
# same motivation applies here: built static, not shared).
include(FetchContent)

set(BUILD_SHARED_LIBS OFF)
set(BUILD_STATIC_LIBS ON)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

# Only HTTP(S) is needed -- disable every other protocol to keep the
# vendored footprint and attack surface small.
foreach(_proto DICT FILE FTP GOPHER IMAP LDAP LDAPS MQTT POP3 RTSP SMB SMTP TELNET TFTP)
  set(CURL_DISABLE_${_proto} ON CACHE BOOL "" FORCE)
endforeach()

# No compression/cookie-jar/netrc features needed for reading MXF byte
# ranges; skip zlib so Unix builds don't need a system zlib dev package.
# CURL_ZSTD must be forced off too: curl's CMakeLists auto-detects and
# links a system zstd if find_package(zstd) succeeds (e.g. a machine with
# a conda/miniforge environment on its CMAKE_PREFIX_PATH), and that zstd is
# typically a *shared* library -- silently pulling zstd.dll in as an
# undeployed transitive dependency of what's otherwise built fully static
# (see this file's own header comment). Confirmed as the exact cause of
# mxfsource_vs.dll failing to load in a real VapourSynth host
# (GetLastError 126) on a machine with such a zstd package installed.
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_COOKIES ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_NETRC ON CACHE BOOL "" FORCE)

if(WIN32)
  # Native Windows TLS backend -- uses the OS certificate store, no CA
  # bundle to vendor or configure at runtime.
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
else()
  # Relies on a system OpenSSL dev package being available; curl's own
  # CMakeLists calls find_package(OpenSSL) when this is enabled.
  set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(curl
  GIT_REPOSITORY https://github.com/curl/curl.git
  GIT_TAG curl-8_21_0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(curl)

# curl's own CMakeLists (lib/CMakeLists.txt) exposes the built library
# (libcurl_static, since BUILD_STATIC_LIBS/BUILD_SHARED_LIBS are set above)
# under the generic alias `CURL::libcurl` (curl's project() name is the
# uppercase "CURL") -- link against that directly.
