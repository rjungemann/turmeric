# cmake/CPM.cmake — bootstrap CPM.cmake if not already present
# This file downloads CPM.cmake on first configure, then includes it.
# CPM.cmake: https://github.com/cpm-cmake/CPM.cmake

set(CPM_VERSION "0.40.2")
set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM.cmake")

if(NOT (EXISTS "${CPM_DOWNLOAD_LOCATION}"))
  # Prefer the vendored copy: avoids a network hop on every fresh build dir,
  # and stays reliable behind proxies / offline. Bump the vendored file to
  # upgrade CPM.
  if(EXISTS "${CMAKE_SOURCE_DIR}/external/CPM.cmake")
    set(CPM_DOWNLOAD_LOCATION "${CMAKE_SOURCE_DIR}/external/CPM.cmake")
  else()
    message(STATUS "Downloading CPM.cmake v${CPM_VERSION}...")
    file(DOWNLOAD
      "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake"
      "${CPM_DOWNLOAD_LOCATION}"
      EXPECTED_HASH SHA256=c8cdc32c03816538ce22781ed72964dc864b2a34a310d3b7104812a5ca2d835d
      STATUS _download_status
    )
    list(GET _download_status 0 _download_code)
    if(NOT _download_code EQUAL 0)
      message(FATAL_ERROR "Could not download CPM.cmake. "
        "Place it at ${CMAKE_SOURCE_DIR}/external/CPM.cmake as a fallback.")
    endif()
  endif()
endif()

include("${CPM_DOWNLOAD_LOCATION}")
