# appends the 4 byte "ULTR" signature that Ultrahand uses to identify
# libultrahand overlays. usage: cmake -DFILE=<path.ovl> -P append_ultr_signature.cmake
if (NOT DEFINED FILE)
    message(FATAL_ERROR "FILE not set")
endif()

file(APPEND "${FILE}" "ULTR")
