# Like webos-userland, supply exported symbols at link time without shipping LG binaries.
function(webos_link_stub name soversion)
    add_library(webos_${name} SHARED dummy.c)
    set(exports "${CMAKE_CURRENT_SOURCE_DIR}/${name}.lds")
    target_link_options(webos_${name} PRIVATE -nostdlib "LINKER:--no-undefined" "${exports}")
    set_property(TARGET webos_${name} APPEND PROPERTY LINK_DEPENDS "${exports}")
    set_target_properties(
        webos_${name}
        PROPERTIES
            OUTPUT_NAME "${name}"
            SOVERSION "${soversion}"
            VERSION "${soversion}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
endfunction()

webos_link_stub(nyx 7)
webos_link_stub(im 1)
