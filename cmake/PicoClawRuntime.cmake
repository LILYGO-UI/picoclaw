include_guard(GLOBAL)

set(_PICOCLAW_TARGET_SUPPORTED OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_PICOCLAW_TARGET_SUPPORTED ON)
endif()
option(LILYGO_PICOCLAW_BUNDLE_RUNTIME
    "Bundle the Linux ARM64 PicoClaw runtime in the application package"
    ${_PICOCLAW_TARGET_SUPPORTED})
set(LILYGO_PICOCLAW_RUNTIME_URL
    "https://github.com/sipeed/picoclaw/releases/download/nightly/picoclaw_Linux_arm64.tar.gz"
    CACHE STRING "URL of the bundled PicoClaw Linux ARM64 archive")
set(LILYGO_PICOCLAW_RUNTIME_ARCHIVE "" CACHE FILEPATH
    "Use a local PicoClaw archive instead of downloading it")
set(LILYGO_PICOCLAW_RUNTIME_SHA256 "" CACHE STRING
    "Optional expected SHA-256 of the PicoClaw archive")

if(NOT LILYGO_PICOCLAW_BUNDLE_RUNTIME)
    return()
endif()
if(NOT _PICOCLAW_TARGET_SUPPORTED)
    message(FATAL_ERROR
        "The bundled PicoClaw runtime requires a Linux ARM64 target; "
        "disable LILYGO_PICOCLAW_BUNDLE_RUNTIME for this target")
endif()

function(_lilygo_ui_bundle_picoclaw)
    string(TOLOWER "${LILYGO_PICOCLAW_RUNTIME_SHA256}" _expected_sha256)
    if(NOT _expected_sha256 STREQUAL "")
        string(LENGTH "${_expected_sha256}" _hash_length)
        if(NOT _hash_length EQUAL 64 OR
           NOT _expected_sha256 MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR
                "LILYGO_PICOCLAW_RUNTIME_SHA256 must contain 64 hexadecimal characters")
        endif()
    endif()

    set(_runtime_dir "${CMAKE_CURRENT_BINARY_DIR}/picoclaw-runtime")
    file(MAKE_DIRECTORY "${_runtime_dir}")
    file(LOCK "${_runtime_dir}/configure.lock" GUARD FUNCTION TIMEOUT 360
        RESULT_VARIABLE _lock_result)
    if(NOT _lock_result STREQUAL "0")
        message(FATAL_ERROR "Could not lock the PicoClaw runtime cache: ${_lock_result}")
    endif()

    if(LILYGO_PICOCLAW_RUNTIME_ARCHIVE)
        get_filename_component(_archive "${LILYGO_PICOCLAW_RUNTIME_ARCHIVE}"
            ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_archive}" OR IS_DIRECTORY "${_archive}")
            message(FATAL_ERROR "PicoClaw runtime archive does not exist: ${_archive}")
        endif()
        get_filename_component(_archive_name "${_archive}" NAME)
        set(_source "Local archive: ${_archive_name}")
    else()
        if(LILYGO_PICOCLAW_RUNTIME_URL STREQUAL "")
            message(FATAL_ERROR "LILYGO_PICOCLAW_RUNTIME_URL cannot be empty")
        endif()
        # A nightly URL is mutable. Reuse the exact archive in this build tree
        # until its cache is removed, or the URL/expected checksum is changed.
        string(SHA256 _download_key
            "${LILYGO_PICOCLAW_RUNTIME_URL}\n${_expected_sha256}")
        set(_download_dir "${_runtime_dir}/downloads/${_download_key}")
        set(_archive "${_download_dir}/picoclaw_Linux_arm64.tar.gz")
        file(MAKE_DIRECTORY "${_download_dir}")
        if(NOT EXISTS "${_archive}")
            message(STATUS "Downloading bundled PicoClaw: ${LILYGO_PICOCLAW_RUNTIME_URL}")
            file(DOWNLOAD "${LILYGO_PICOCLAW_RUNTIME_URL}" "${_archive}.download"
                TLS_VERIFY ON TIMEOUT 300 INACTIVITY_TIMEOUT 60 SHOW_PROGRESS
                STATUS _download_status)
            list(GET _download_status 0 _download_code)
            list(GET _download_status 1 _download_message)
            if(NOT _download_code EQUAL 0)
                file(REMOVE "${_archive}.download")
                message(FATAL_ERROR
                    "Could not download the bundled PicoClaw runtime: ${_download_message}\n"
                    "Use LILYGO_PICOCLAW_RUNTIME_ARCHIVE for an existing local archive.")
            endif()
            file(RENAME "${_archive}.download" "${_archive}")
        endif()
        set(_source "Download URL: ${LILYGO_PICOCLAW_RUNTIME_URL}")
    endif()

    file(SHA256 "${_archive}" _archive_sha256)
    if(NOT _expected_sha256 STREQUAL "" AND
       NOT _archive_sha256 STREQUAL _expected_sha256)
        message(FATAL_ERROR
            "PicoClaw runtime archive checksum mismatch:\n"
            "  Archive:  ${_archive}\n"
            "  Expected: ${_expected_sha256}\n"
            "  Actual:   ${_archive_sha256}\n"
            "Replace the archive or remove the cached download before retrying.")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_archive}")
    message(STATUS "Bundled PicoClaw archive SHA-256: ${_archive_sha256}")

    set(_extracted "${_runtime_dir}/extracted")
    file(REMOVE_RECURSE "${_extracted}")
    file(MAKE_DIRECTORY "${_extracted}")
    # The official release has these files at its root. Extract only the core
    # runtime and its documentation; this application does not need the web launcher.
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_extracted}"
        PATTERNS picoclaw LICENSE README.md)
    foreach(_required IN ITEMS picoclaw LICENSE README.md)
        if(NOT EXISTS "${_extracted}/${_required}" OR
           IS_DIRECTORY "${_extracted}/${_required}" OR
           IS_SYMLINK "${_extracted}/${_required}")
            message(FATAL_ERROR
                "PicoClaw runtime archive must contain a regular ${_required} file at its root")
        endif()
    endforeach()

    # Do not run a target executable on the build host. Verify its ELF header:
    # 64-bit little endian, current ELF version, System V/Linux ABI, executable
    # or PIE, and EM_AARCH64 (183). A different architecture must fail packaging.
    set(_binary "${_extracted}/picoclaw")
    file(SIZE "${_binary}" _binary_size)
    file(READ "${_binary}" _elf_header LIMIT 24 HEX)
    set(_elf_valid OFF)
    if(NOT _binary_size LESS 64)
        string(SUBSTRING "${_elf_header}" 0 14 _elf_ident)
        string(SUBSTRING "${_elf_header}" 14 2 _elf_abi)
        string(SUBSTRING "${_elf_header}" 32 4 _elf_type)
        string(SUBSTRING "${_elf_header}" 36 4 _elf_machine)
        string(SUBSTRING "${_elf_header}" 40 8 _elf_version)
        if(_elf_ident STREQUAL "7f454c46020101" AND
           _elf_abi MATCHES "^(00|03)$" AND
           _elf_type MATCHES "^(0200|0300)$" AND
           _elf_machine STREQUAL "b700" AND
           _elf_version STREQUAL "01000000")
            set(_elf_valid ON)
        endif()
    endif()
    if(NOT _elf_valid)
        message(FATAL_ERROR
            "PicoClaw runtime is not a Linux ARM64 ELF executable: ${_binary}")
    endif()

    file(SHA256 "${_binary}" _binary_sha256)
    set(_provenance "${_runtime_dir}/runtime-source.txt")
    file(WRITE "${_provenance}"
        "Bundled PicoClaw runtime\n"
        "Upstream: https://github.com/sipeed/picoclaw\n"
        "Target: Linux arm64\n"
        "${_source}\n"
        "Archive SHA-256: ${_archive_sha256}\n"
        "Executable SHA-256: ${_binary_sha256}\n"
        "The nightly URL is mutable; these hashes identify the packaged snapshot.\n")
    install(PROGRAMS "${_binary}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/${LILYGO_UI_PACKAGE_NAME}"
        COMPONENT app)
    install(FILES "${_extracted}/LICENSE" "${_extracted}/README.md" "${_provenance}"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/${LILYGO_UI_PACKAGE_NAME}/picoclaw"
        COMPONENT app)
endfunction()

_lilygo_ui_bundle_picoclaw()
