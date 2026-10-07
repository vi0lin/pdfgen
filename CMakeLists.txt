cmake_minimum_required(VERSION 3.20)
# C is needed because zlib/libwebp are C projects when built via FetchContent.
set(TNAME "pdfgen")
project(${TNAME} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===========================================================================
# EMBEDDED-ERKENNUNG
# ---------------------------------------------------------------------------
# Wird dieses Projekt per add_subdirectory() von einem groesseren Projekt
# eingebunden (x_bookmark_manager), ist PROJECT_IS_TOP_LEVEL false. Dann:
#   * KEIN CLI-Executable bauen (das braucht der Host nicht und es zieht
#     Abhaengigkeiten nach, die im Cross-Build gar nicht existieren),
#   * KEIN find_package fuer Dinge, die das Eltern-Projekt schon liefert.
# ===========================================================================
if (NOT DEFINED PROJECT_IS_TOP_LEVEL)      # CMake < 3.21
    if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(PROJECT_IS_TOP_LEVEL TRUE)
    else()
        set(PROJECT_IS_TOP_LEVEL FALSE)
    endif()
endif()

option(PDFGEN_WITH_WEBP   "Enable WebP image support (needs libwebp)" ON)
option(PDFGEN_BUILD_CLI   "Build the pdfgen command line tool"
       ${PROJECT_IS_TOP_LEVEL})
option(PDFGEN_FORCE_FETCH "Skip system libraries, always build zlib+libwebp \
from source via FetchContent" OFF)

# CROSS-BUILD: Host-Bibliotheken sind hier IMMER falsch (x86_64 vs aarch64).
# CMAKE_CROSSCOMPILING setzt die NDK-Toolchain automatisch -- damit muss
# niemand mehr von aussen -DPDFGEN_FORCE_FETCH oder Sysroot-Pfade
# hineinreichen.
if (CMAKE_CROSSCOMPILING)
    set(PDFGEN_FORCE_FETCH ON)
    message(STATUS "pdfgen: cross build -> zlib/libwebp aus Quellen")
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# zlib  (system first, FetchContent fallback)
# ---------------------------------------------------------------------------
set(PDFGEN_ZLIB_TARGET "")
if (TARGET ZLIB::ZLIB)                      # vom Eltern-Projekt geliefert
    set(PDFGEN_ZLIB_TARGET ZLIB::ZLIB)
    message(STATUS "pdfgen: nutze zlib des Eltern-Projekts")
elseif (NOT PDFGEN_FORCE_FETCH)
    find_package(ZLIB QUIET)
    if (ZLIB_FOUND)
        set(PDFGEN_ZLIB_TARGET ZLIB::ZLIB)
        message(STATUS "pdfgen: using system zlib (${ZLIB_VERSION_STRING})")
    endif()
endif()
if (NOT PDFGEN_ZLIB_TARGET AND NOT TARGET zlibstatic)
    message(STATUS "pdfgen: building zlib from source (FetchContent)")
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(pdfgen_zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG        v1.3.1
        GIT_SHALLOW    ON
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(pdfgen_zlib)
    target_include_directories(zlibstatic INTERFACE
        "${pdfgen_zlib_SOURCE_DIR}" "${pdfgen_zlib_BINARY_DIR}")
endif()
if (NOT PDFGEN_ZLIB_TARGET AND TARGET zlibstatic)
    set(PDFGEN_ZLIB_TARGET zlibstatic)
endif()

# ---------------------------------------------------------------------------
# libwebp  (CMake config -> pkg-config -> FetchContent)
# ---------------------------------------------------------------------------
set(PDFGEN_WEBP_TARGET "")
if (PDFGEN_WITH_WEBP)
    if (TARGET webp)                        # Eltern-Projekt hat es schon
        set(PDFGEN_WEBP_TARGET webp)
    elseif (NOT PDFGEN_FORCE_FETCH)
        find_package(WebP CONFIG QUIET)
        if (TARGET WebP::webp)
            set(PDFGEN_WEBP_TARGET WebP::webp)
        else()
            find_package(PkgConfig QUIET)
            if (PkgConfig_FOUND)
                pkg_check_modules(PDFGEN_PC_WEBP QUIET IMPORTED_TARGET libwebp)
                if (TARGET PkgConfig::PDFGEN_PC_WEBP)
                    set(PDFGEN_WEBP_TARGET PkgConfig::PDFGEN_PC_WEBP)
                endif()
            endif()
        endif()
    endif()
    if (NOT PDFGEN_WEBP_TARGET)
        message(STATUS "pdfgen: building libwebp from source (FetchContent)")
        foreach (v ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP
                   WEBPINFO WEBPMUX EXTRAS LIBWEBPMUX)
            set(WEBP_BUILD_${v} OFF CACHE BOOL "" FORCE)
        endforeach()
        FetchContent_Declare(pdfgen_libwebp
            GIT_REPOSITORY https://github.com/webmproject/libwebp.git
            GIT_TAG        v1.5.0
            GIT_SHALLOW    ON
            EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(pdfgen_libwebp)
        set(PDFGEN_WEBP_TARGET webp)        # zieht sharpyuv transitiv mit
    endif()
endif()

# ===========================================================================
# libcurl -- DIE STELLE, DIE DEN CROSS-BUILD ZERSTOERT HAT
# ---------------------------------------------------------------------------
# REIHENFOLGE DER SUCHE:
#  1. Hat das Eltern-Projekt bereits ein curl-Target (x_bookmark_manager
#     baut curl fuer Android selbst per FetchContent)? -> DAS nehmen.
#     Es ist fuer aarch64 gebaut und bringt mbedTLS transitiv mit.
#  2. Sonst find_package(CURL) -- aber nur, wenn wir NICHT cross-bauen.
#  3. Sonst: ohne Mailversand bauen (PDFGEN_NO_CURL), statt hart zu
#     scheitern. Ein eingebettetes Modul darf den Host-Build nicht
#     abschiessen, nur weil ein optionales Feature fehlt.
#
# NIEMALS set_target_properties(CURL::libcurl PROPERTIES IMPORTED_LOCATION
# <host-pfad>) -- genau das hat dem Android-Link die x86-.so untergeschoben
# ("libcurl.so is incompatible with aarch64linux"). Ein importiertes Target
# gehoert dem Finder, nicht dem Verbraucher.
# ===========================================================================
set(PDFGEN_CURL_TARGET "")
foreach (cand CURL::libcurl libcurl_static libcurl)
    if (TARGET ${cand})
        set(PDFGEN_CURL_TARGET ${cand})
        message(STATUS "pdfgen: nutze curl-Target des Eltern-Projekts: ${cand}")
        break()
    endif()
endforeach()
if (NOT PDFGEN_CURL_TARGET AND NOT CMAKE_CROSSCOMPILING)
    find_package(CURL QUIET)
    if (TARGET CURL::libcurl)
        set(PDFGEN_CURL_TARGET CURL::libcurl)
        message(STATUS "pdfgen: using system libcurl (${CURL_VERSION_STRING})")
    endif()
endif()
if (NOT PDFGEN_CURL_TARGET)
    if (CMAKE_CROSSCOMPILING)
        # Im Cross-Build ist das FAST IMMER ein Reihenfolge-Fehler und
        # keine bewusste Entscheidung: Das Eltern-Projekt baut curl per
        # FetchContent, aber add_subdirectory(external/pdf) stand VOR
        # diesem Build. CMake liest streng von oben nach unten -- zum
        # Zeitpunkt dieser Zeile existiert das Target dann noch nicht.
        message(WARNING
            "pdfgen: KEIN curl-Target gefunden.\n"
            "  -> pdfgen::mail wird NICHT erzeugt, gmail_send.h ist damit\n"
            "     fuer die App nicht erreichbar.\n"
            "  -> URSACHE ist meist die REIHENFOLGE im Haupt-CMakeLists:\n"
            "     add_subdirectory(external/pdf ...) MUSS NACH\n"
            "     FetchContent_MakeAvailable(curl_android) stehen.")
    else()
        message(STATUS "pdfgen: kein curl gefunden -> Mailversand deaktiviert")
    endif()
endif()

# VERALTET: Zugangsdaten werden nicht mehr einkompiliert, sondern zur
# Laufzeit gesetzt (pdfgen.conf / -c / --sender / Gast-App, siehe
# src/mail_config.h). Die Variablen wirken nur noch auf send_test_mail().
set(PDFGEN_MAIL_FROM "" CACHE STRING "DEPRECATED: only used by send_test_mail()")
set(PDFGEN_MAIL_APP_PASSWORD "" CACHE STRING "DEPRECATED: only used by send_test_mail()")

# ===========================================================================
# TARGETS
# ---------------------------------------------------------------------------
#  pdfgen_core : PDF-Erzeugung (ohne Mail, ohne main.cpp)
#  pdfgen_mail : gmail_send.c -- nur wenn curl da ist
#  pdfgen      : CLI, nur im Top-Level-Build
# ===========================================================================
file(GLOB PDFGEN_CORE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM PDFGEN_CORE_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/src/mail_dispatch.cpp)   # braucht curl -> pdfgen_mail

add_library(pdfgen_core STATIC ${PDFGEN_CORE_SOURCES})
add_library(pdfgen::core ALIAS pdfgen_core)
# PUBLIC = "wer mich linkt, bekommt dieses Include-Verzeichnis automatisch".
# BUILD_INTERFACE = gilt nur im Build-Baum (fuer install() gaebe es eine
# zweite Zeile mit INSTALL_INTERFACE).
target_include_directories(pdfgen_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# PRIVATE = zlib/webp sind reine Implementierungsdetails; ihre Header
# tauchen in KEINEM oeffentlichen pdfgen-Header auf.
target_link_libraries(pdfgen_core PRIVATE ${PDFGEN_ZLIB_TARGET})
if (PDFGEN_WEBP_TARGET)
    target_link_libraries(pdfgen_core PRIVATE ${PDFGEN_WEBP_TARGET})
else()
    target_compile_definitions(pdfgen_core PRIVATE PDFGEN_NO_WEBP)
endif()

# --- Mail-Teil (frueher: my_library) ---------------------------------------
# Eigenes Target statt "my_library mit main.cpp drin": main.cpp gehoert NIE
# in eine Bibliothek (doppeltes main beim Linken, und der Konsument will es
# gar nicht).
if (PDFGEN_CURL_TARGET)
    add_library(pdfgen_mail STATIC src/gmail_send.c src/mail_dispatch.cpp)
    add_library(pdfgen::mail ALIAS pdfgen_mail)
    target_include_directories(pdfgen_mail PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
    # PUBLIC: curl-Header stehen in gmail_send.h? Dann PUBLIC, sonst
    # PRIVATE. Konservativ PRIVATE -- die Link-Abhaengigkeit wandert bei
    # STATIC-Libs ohnehin transitiv zum Endziel.
    target_link_libraries(pdfgen_mail PRIVATE pdfgen::core ${PDFGEN_CURL_TARGET})
    foreach (def FROM_ADDR:PDFGEN_MAIL_FROM APP_PASSWORD:PDFGEN_MAIL_APP_PASSWORD)
        string(REPLACE ":" ";" p "${def}")
        list(GET p 0 macro)
        list(GET p 1 var)
        if (NOT "${${var}}" STREQUAL "")
            target_compile_definitions(pdfgen_mail PRIVATE "${macro}=\"${${var}}\"")
        endif()
    endforeach()
endif()

# --- Was ist am Ende entstanden? (Erfolgskontrolle im Log) -----------------
message(STATUS "pdfgen: Targets -> pdfgen::core"
               "$<$<BOOL:$<TARGET_EXISTS:pdfgen_mail>>:, pdfgen::mail>")
if (TARGET pdfgen_mail)
    message(STATUS "pdfgen: Mailversand AKTIV (pdfgen::mail vorhanden)")
else()
    message(STATUS "pdfgen: Mailversand INAKTIV (pdfgen::mail fehlt)")
endif()

# --- CLI (nur Top-Level) ----------------------------------------------------
if (PDFGEN_BUILD_CLI)
    add_executable(${TNAME} src/main.cpp)
    target_link_libraries(${TNAME} PRIVATE pdfgen::core)
    if (TARGET pdfgen_mail)
        target_link_libraries(${TNAME} PRIVATE pdfgen::mail)
    else()
        target_compile_definitions(${TNAME} PRIVATE PDFGEN_NO_MAIL)
    endif()
    # Beispielkonfiguration neben die Binary legen -- als .example, damit
    # nie echte Zugangsdaten ueberschrieben werden.
    add_custom_command(TARGET ${TNAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/pdfgen.conf.example
                $<TARGET_FILE_DIR:${TNAME}>/pdfgen.conf.example)
endif()
