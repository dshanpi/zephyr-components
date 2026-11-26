# Copyright (c) 2026 Yuzuki Tsuru
# SPDX-License-Identifier: GPL-3.0-or-later

# Zephyr module of the iOS screen mirror: the AirPlay receiver library of RPiPlay (git
# submodule) with its OpenSSL, Bonjour and libplist parts replaced by mbedTLS, the Zephyr
# DNS-SD responder and a small binary plist reader/writer.

if(NOT CONFIG_IOS_MIRROR)
  return()
endif()

set(RP ${CMAKE_CURRENT_LIST_DIR}/rpiplay/lib)

zephyr_library_named(ios_screen_mirror)
zephyr_library_sources(
  src/airplay.c
  src/ios_crypto.c
  src/ios_dnssd.c
  src/ed25519_ops.c
  src/plist.c
  src/ios_posix.c
)

# The announced display size is a literal in the library: raop.c and the header it includes are
# copied with the configured size.
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/raop_patched)
file(READ ${RP}/raop_handlers.h RAOP_HANDLERS)
string(REPLACE "plist_new_uint(1920)" "plist_new_uint(${CONFIG_IOS_MIRROR_WIDTH})"
       RAOP_HANDLERS "${RAOP_HANDLERS}")
string(REPLACE "plist_new_uint(1080)" "plist_new_uint(${CONFIG_IOS_MIRROR_HEIGHT})"
       RAOP_HANDLERS "${RAOP_HANDLERS}")
file(WRITE ${GEN}/raop_handlers.h "${RAOP_HANDLERS}")
file(READ ${RP}/raop.c RAOP_C)
string(REPLACE
  "raop_rtp_mirror_destroy(conn->raop_rtp_mirror);\n    }\n\n    conn->raop->callbacks.video_flush(conn->raop->callbacks.cls);"
  "raop_rtp_mirror_destroy(conn->raop_rtp_mirror);\n        conn->raop->callbacks.video_flush(conn->raop->callbacks.cls);\n    }"
  RAOP_C "${RAOP_C}")
file(WRITE ${GEN}/raop.c "${RAOP_C}")
file(READ ${RP}/raop_rtp_mirror.c RAOP_MIRROR_C)
string(REPLACE
  "logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, \"raop_rtp_mirror width_source = %f height_source = %f width = %f height = %f\",\n                           width_source, height_source, width, height);"
  "logger_log(raop_rtp_mirror->logger, LOGGER_INFO, \"raop_rtp_mirror src=%dx%d dst=%dx%d\", (int)width_source, (int)height_source, (int)width, (int)height);"
  RAOP_MIRROR_C "${RAOP_MIRROR_C}")
string(REPLACE
  "h264_data.frame_type = 0;\n                    h264_data.pts = 0;"
  "h264_data.frame_type = 0;\n                    h264_data.pts = 0;\n                    h264_data.n_gop_index = (int)width_source;\n                    h264_data.n_frame_poc = (int)height_source;\n                    h264_data.n_time_stamp = (((unsigned int)(int)width & 0xffffU) << 16) | ((unsigned int)(int)height & 0xffffU);"
  RAOP_MIRROR_C "${RAOP_MIRROR_C}")
file(WRITE ${GEN}/raop_rtp_mirror.c "${RAOP_MIRROR_C}")

set(RP_SOURCES
  ${RP}/byteutils.c
  ${RP}/fairplay_playfair.c
  ${RP}/http_request.c
  ${RP}/http_response.c
  ${RP}/httpd.c
  ${RP}/logger.c
  ${RP}/mirror_buffer.c
  ${RP}/netutils.c
  ${RP}/pairing.c
  ${GEN}/raop.c
  ${RP}/raop_buffer.c
  ${RP}/raop_ntp.c
  ${RP}/raop_rtp.c
  ${GEN}/raop_rtp_mirror.c
  ${RP}/utils.c
  ${RP}/playfair/hand_garble.c
  ${RP}/playfair/modified_md5.c
  ${RP}/playfair/omg_hax.c
  ${RP}/playfair/playfair.c
  ${RP}/playfair/sap_hash.c
  ${RP}/llhttp/api.c
  ${RP}/llhttp/http.c
  ${RP}/llhttp/llhttp.c
)
zephyr_library_sources(${RP_SOURCES})
# the library is not ours: its warnings are not ours to fix
set_source_files_properties(${RP_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-w;-include;${CMAKE_CURRENT_LIST_DIR}/shim/ios_posix.h")

zephyr_library_include_directories(${RP} ${RP}/playfair ${RP}/llhttp src shim)
zephyr_include_directories(include)
zephyr_library_link_libraries(mbedTLS m)
