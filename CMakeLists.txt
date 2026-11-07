cmake_minimum_required(VERSION 3.16)
project(quill VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -O2 instead of CMake's default -O3 for Release: noticeably faster to compile on old CPUs.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  string(REPLACE "-O3" "-O2" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  string(REPLACE "-O3" "-O2" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
endif()

# MinGW/MSYS2: CMake turns the include dir of imported libraries (SDL2, curl) into
# "-isystem <prefix>/include". That is also the compiler's own C header dir, and listing it
# explicitly moves it ahead of libstdc++'s dirs, which breaks "#include_next <stdlib.h>"
# ("fatal error: stdlib.h: No such file or directory"). Plain -I is ignored by GCC when the
# directory is already a system directory, so the search order stays intact.
if(MINGW)
  set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
endif()

option(QUILL_BUILD_TESTS "Build the unit tests" ON)
option(QUILL_WEBP "Decode WebP images (needs libwebp)" ON)
option(QUILL_WOFF2 "Decode WOFF2 web fonts (needs Brotli's decoder library)" ON)
option(QUILL_ENGINE_TOOLS "Build the engine dev tools (enginerender, enginedump, engine_smoke)" OFF)
option(QUILL_HARDENING "Enable stack protector / RELRO on GCC and Clang" ON)

# ---------------------------------------------------------------------------
# Dependencies
#   SDL2    window + input + blitting a software framebuffer
#   libcurl networking, TLS (system TLS backend), DNS-over-HTTPS
#   NetSurf libdom/libcss/hubbub (vendored in third_party/netsurf) parse and style HTML; Quill's own
#   engine (src/engine) builds boxes, lays out and paints
# stb_truetype / stb_image / stb_image_write are vendored single headers.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
  set(QUILL_SDL2_TARGET SDL2::SDL2)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2PC REQUIRED IMPORTED_TARGET sdl2)
  set(QUILL_SDL2_TARGET PkgConfig::SDL2PC)
endif()

# libwebp is optional: with it WebP images work (and Quill tells servers it accepts WebP).
set(QUILL_WEBP_TARGET "")
if(QUILL_WEBP)
  find_package(WebP CONFIG QUIET)
  if(TARGET WebP::webp)
    set(QUILL_WEBP_TARGET WebP::webp)
  else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(WEBPPC QUIET IMPORTED_TARGET libwebp)
      if(WEBPPC_FOUND)
        set(QUILL_WEBP_TARGET PkgConfig::WEBPPC)
      endif()
    endif()
  endif()
  if(QUILL_WEBP_TARGET)
    message(STATUS "WebP support: enabled (${QUILL_WEBP_TARGET})")
  else()
    message(WARNING "libwebp not found: WebP images will not display. Install it (Gentoo: media-libs/libwebp; "
                    "MSYS2: mingw-w64-ucrt-x86_64-libwebp) or pass -DQUILL_WEBP=OFF to silence this.")
  endif()
endif()

# Brotli (decoder) is optional: with it WOFF2 web fonts work, which is what most sites send. WOFF and TrueType
# fonts work without it.
set(QUILL_BROTLI_TARGET "")
if(QUILL_WOFF2)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(BROTLIPC QUIET IMPORTED_TARGET libbrotlidec)
    if(BROTLIPC_FOUND)
      set(QUILL_BROTLI_TARGET PkgConfig::BROTLIPC)
    endif()
  endif()
  if(NOT QUILL_BROTLI_TARGET)
    find_path(BROTLI_INCLUDE_DIR brotli/decode.h)
    find_library(BROTLIDEC_LIBRARY NAMES brotlidec brotlidec-static)
    find_library(BROTLICOMMON_LIBRARY NAMES brotlicommon brotlicommon-static)
    if(BROTLI_INCLUDE_DIR AND BROTLIDEC_LIBRARY)
      add_library(quill_brotli INTERFACE)
      target_include_directories(quill_brotli INTERFACE ${BROTLI_INCLUDE_DIR})
      target_link_libraries(quill_brotli INTERFACE ${BROTLIDEC_LIBRARY})
      if(BROTLICOMMON_LIBRARY)
        target_link_libraries(quill_brotli INTERFACE ${BROTLICOMMON_LIBRARY})
      endif()
      set(QUILL_BROTLI_TARGET quill_brotli)
    endif()
  endif()
  if(QUILL_BROTLI_TARGET)
    message(STATUS "WOFF2 fonts: enabled (${QUILL_BROTLI_TARGET})")
  else()
    message(WARNING "Brotli decoder not found: WOFF2 web fonts will be skipped (WOFF / TrueType still work). Install it "
                    "(Gentoo: app-arch/brotli; MSYS2: mingw-w64-ucrt-x86_64-brotli) or pass -DQUILL_WOFF2=OFF.")
  endif()
endif()

set(BUILD_SHARED_LIBS OFF)

# ---------------------------------------------------------------------------
# Base library: networking, images, fonts, framebuffer (no page logic)
# ---------------------------------------------------------------------------
add_library(quillbase STATIC
  src/url.cpp
  src/policy.cpp
  src/hsts.cpp
  src/settings.cpp
  src/input.cpp
  src/net.cpp
  src/reqheaders.cpp
  src/httpcache.cpp
  src/certpin.cpp
  src/cookies.cpp
  src/image.cpp
  src/svg.cpp
  src/framebuffer.cpp
  src/font.cpp
  src/fontsan.cpp
  src/diskcache.cpp
  src/stb_impl.cpp)
target_include_directories(quillbase PUBLIC src)
target_include_directories(quillbase PRIVATE third_party/stb third_party/nanosvg)
if(QUILL_WEBP_TARGET)
  target_link_libraries(quillbase PUBLIC ${QUILL_WEBP_TARGET})
  target_compile_definitions(quillbase PRIVATE QUILL_HAVE_WEBP=1)
endif()
if(QUILL_BROTLI_TARGET)
  target_link_libraries(quillbase PUBLIC ${QUILL_BROTLI_TARGET})
  target_compile_definitions(quillbase PRIVATE QUILL_HAVE_BROTLI=1)
endif()
target_link_libraries(quillbase PUBLIC CURL::libcurl Threads::Threads)
if(WIN32)
  target_link_libraries(quillbase PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# New engine: vendored NetSurf libraries (MIT), compiled directly. All generated tables are
# committed (see third_party/netsurf/VERSIONS), so no Perl or gperf is needed to build.
# ---------------------------------------------------------------------------
set(NS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/netsurf")
function(quill_nslib name)
  cmake_parse_arguments(A "" "" "DIRS;INCS;DEPS" ${ARGN})
  set(srcs "")
  foreach(d ${A_DIRS})
    file(GLOB_RECURSE g CONFIGURE_DEPENDS "${NS}/${d}/*.c")
    list(APPEND srcs ${g})
  endforeach()
  add_library(${name} STATIC ${srcs})
  target_include_directories(${name} PUBLIC ${A_INCS})
  # iconv input filter off: UTF-8/ASCII/8859/ext8/UTF-16 only, no extra dependency
  target_compile_definitions(${name} PRIVATE _DEFAULT_SOURCE _BSD_SOURCE WITHOUT_ICONV_FILTER
    "_ALIGNED=__attribute__((aligned))" STMTEXPR=1)
  set_target_properties(${name} PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
  target_compile_options(${name} PRIVATE -w)
  if(A_DEPS)
    target_link_libraries(${name} PUBLIC ${A_DEPS})
  endif()
endfunction()
quill_nslib(ns_wapcaplet DIRS libwapcaplet/src INCS "${NS}/libwapcaplet/include")
quill_nslib(ns_parserutils DIRS libparserutils/src INCS "${NS}/libparserutils/include" "${NS}/libparserutils/src")
quill_nslib(ns_hubbub DIRS hubbub/src INCS "${NS}/hubbub/include" "${NS}/hubbub/src" DEPS ns_wapcaplet ns_parserutils)
quill_nslib(ns_css DIRS libcss/src INCS "${NS}/libcss/include" "${NS}/libcss/src" DEPS ns_wapcaplet ns_parserutils)
quill_nslib(ns_dom DIRS libdom/src libdom/bindings/hubbub
            INCS "${NS}/libdom/include" "${NS}/libdom/src" "${NS}/libdom" DEPS ns_wapcaplet ns_parserutils ns_hubbub)
add_library(quillengine INTERFACE)
target_link_libraries(quillengine INTERFACE ns_dom ns_css ns_hubbub ns_parserutils ns_wapcaplet)
# libcss headers use C99 'restrict'; libdom headers need the 'namespace' patch noted in VERSIONS
target_compile_definitions(quillengine INTERFACE restrict=__restrict)

# Engine: styled boxes, layout, paint
add_library(quillenginecore STATIC src/engine/document.cpp src/engine/hints.cpp src/engine/cssvars.cpp src/engine/csslex.cpp src/engine/elemcss.cpp
  src/engine/boxbuilder.cpp src/engine/grid.cpp src/engine/layout.cpp src/engine/paint.cpp src/engine/selection.cpp)
target_include_directories(quillenginecore PUBLIC src src/engine)
target_link_libraries(quillenginecore PUBLIC quillengine quillbase)

# Core library: pages, forms, browser logic (everything except the SDL window)
add_library(quillcore STATIC src/forms.cpp src/page.cpp src/browser.cpp src/chrome.cpp src/filepick.cpp src/history.cpp)
target_link_libraries(quillcore PUBLIC quillenginecore)
if(WIN32)
  target_link_libraries(quillcore PUBLIC comdlg32)
endif()

add_executable(quill src/main.cpp)
target_link_libraries(quill PRIVATE quillcore ${QUILL_SDL2_TARGET})

if(QUILL_BUILD_TESTS)
  enable_testing()
  add_executable(quill_tests tests/test_core.cpp)
  target_link_libraries(quill_tests PRIVATE quillcore)
  target_compile_definitions(quill_tests PRIVATE QUILL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME core COMMAND quill_tests)
  if(NOT WIN32) # real libcurl against a little HTTP server on 127.0.0.1
    add_executable(net_tests tests/net/net_tests.cpp)
    target_link_libraries(net_tests PRIVATE quillcore)
    add_test(NAME net COMMAND net_tests)
  endif()
  add_executable(engine_tests tests/engine/engine_tests.cpp)
  target_link_libraries(engine_tests PRIVATE quillenginecore)
  add_test(NAME engine COMMAND engine_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/engine/cases)
  # Browser behaviour (http warning rule, :visited, framesets) against an in-memory site: fake_net.cpp replaces
  # net.cpp, so these run without network access.
  add_executable(browser_tests tests/browser/browser_tests.cpp tests/browser/fake_net.cpp src/browser.cpp src/page.cpp
    src/forms.cpp src/policy.cpp src/hsts.cpp src/settings.cpp src/input.cpp src/certpin.cpp src/cookies.cpp src/diskcache.cpp src/history.cpp src/reqheaders.cpp src/httpcache.cpp)
  target_include_directories(browser_tests PRIVATE tests/browser)
  target_compile_definitions(browser_tests PRIVATE QUILL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(browser_tests PRIVATE quillenginecore)
  add_test(NAME browser COMMAND browser_tests)
endif()

if(QUILL_ENGINE_TOOLS)
  add_executable(engine_smoke src/engine/smoke.c)
  target_link_libraries(engine_smoke PRIVATE quillengine)
  add_executable(enginedump src/engine/enginedump.cpp)
  target_link_libraries(enginedump PRIVATE quillenginecore)
  add_executable(enginerender src/engine/enginerender.cpp)
  target_link_libraries(enginerender PRIVATE quillenginecore)
endif()

# ---------------------------------------------------------------------------
# Flags: -O2 and no LTO keeps compile times short on old hardware.
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(t quillbase quillcore quill)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endforeach()
  if(QUILL_HARDENING AND NOT WIN32)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-fstack-protector-strong HAVE_SSP)
    if(HAVE_SSP)
      target_compile_options(quillbase PRIVATE -fstack-protector-strong)
      target_compile_options(quillcore PRIVATE -fstack-protector-strong)
      target_compile_options(quill PRIVATE -fstack-protector-strong)
    endif()
    if(NOT APPLE)
      target_link_options(quill PRIVATE -Wl,-z,relro,-z,now)
    endif()
  endif()
endif()

install(TARGETS quill RUNTIME DESTINATION bin)
