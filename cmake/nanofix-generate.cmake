if(NOT TARGET fixspec-gen AND NOT TARGET nanofix::fixspec-gen)
    set(_nanofix_fixspec_gen_path "")
    foreach(_c IN ITEMS
            "${CMAKE_CURRENT_LIST_DIR}/../../../bin/fixspec-gen"
            "${CMAKE_CURRENT_LIST_DIR}/../../../bin/fixspec-gen.exe"
            "${CMAKE_CURRENT_LIST_DIR}/../../bin/fixspec-gen"
            "${CMAKE_CURRENT_LIST_DIR}/../../bin/fixspec-gen.exe")
        get_filename_component(_c_abs "${_c}" ABSOLUTE)
        if(EXISTS "${_c_abs}")
            set(_nanofix_fixspec_gen_path "${_c_abs}")
            break()
        endif()
    endforeach()
    if(_nanofix_fixspec_gen_path)
        add_executable(nanofix::fixspec-gen IMPORTED)
        set_target_properties(nanofix::fixspec-gen PROPERTIES
            IMPORTED_LOCATION "${_nanofix_fixspec_gen_path}")
    endif()
    unset(_nanofix_fixspec_gen_path)
    unset(_c_abs)
endif()

function(nanofix_generate)
    cmake_parse_arguments(HG "" "TARGET" "SPEC_XML" ${ARGN})
    if(NOT HG_TARGET OR NOT HG_SPEC_XML)
        message(FATAL_ERROR
            "nanofix_generate requires TARGET and SPEC_XML "
            "(one or more QuickFIX-format specs, e.g. fixspec/FIX50SP2.xml fixspec/FIXT11.xml)")
    endif()
    if(NOT TARGET ${HG_TARGET})
        message(FATAL_ERROR
            "nanofix_generate: TARGET '${HG_TARGET}' does not exist; "
            "create it before calling this function")
    endif()
    if(TARGET fixspec-gen)
        set(_gen_cmd "$<TARGET_FILE:fixspec-gen>")
        set(_gen_dep fixspec-gen)
    elseif(TARGET nanofix::fixspec-gen)
        set(_gen_cmd "$<TARGET_FILE:nanofix::fixspec-gen>")
        set(_gen_dep nanofix::fixspec-gen)
    else()
        message(FATAL_ERROR
            "nanofix_generate needs the fixspec-gen target. "
            "Either build nanofix with NANOFIX_BUILD_FIXSPEC_GEN=ON, or "
            "install the Conan package which ships the binary.")
    endif()
    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/nanofix_fields_${HG_TARGET}")
    set(_fields_out "${_out_dir}/nanofix/detail/fields.hpp")
    set(_names_out "${_out_dir}/nanofix/names.hpp")
    set(_gen_target "${HG_TARGET}__nanofix_fields")

    add_custom_command(
        OUTPUT  ${_fields_out} ${_names_out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_out_dir}/nanofix/detail
        COMMAND ${_gen_cmd}
                ${HG_SPEC_XML}
                -d ${_out_dir}/nanofix
        DEPENDS ${_gen_dep} ${HG_SPEC_XML}
        COMMENT "fixspec-gen -> ${_out_dir}/nanofix/{detail/fields,names}.hpp"
        VERBATIM
    )

    add_custom_target(${_gen_target} DEPENDS ${_fields_out} ${_names_out})
    add_dependencies(${HG_TARGET} ${_gen_target})
    target_include_directories(${HG_TARGET} BEFORE PRIVATE ${_out_dir})
endfunction()
