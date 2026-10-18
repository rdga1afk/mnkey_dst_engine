# #187: gaia-ecs compile-time fix. md_gaia_pch(<target>) makes the target's PCH carry gaia + the registry headers and a warm-up instantiation
# (include/monkey_dust/ecs/gaia_pch.h), built with -fpch-codegen -fpch-instantiate-templates, and compiles that header once more into an object that
# is linked into the target. Every other TU then sees the instantiated gaia functions as available_externally (still inlinable) instead of optimising
# and emitting them again: 51 s -> 24 s for a TU with one query.
# The object is built from its own copy of the PCH (not from the target's PCH file) so that it does not depend on the target's own object phase;
# MdGaiaPchObj.cmake caches it by content (set CCACHE_DISABLE=1 to bypass). clang only: the flags are clang-specific.
option(MD_GAIA_PCH "Compile the gaia instantiations once per target (PCH codegen object), clang only" ON)
option(MD_GAIA_PCH_HOT "Also apply it to the engine lib and the game (their gaia calls stop being inlined: query().each microbench +74 %, #187)" OFF)
set(MD_GAIA_PCH_CACHE "$ENV{HOME}/.cache/md_gaia_pch" CACHE PATH "Content-addressed cache of the gaia PCH codegen objects")

function(md_gaia_pch tgt)
    if(NOT MD_GAIA_PCH OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        return()
    endif()
    set(_hdr "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include/monkey_dust/ecs/gaia_pch.h")
    cmake_path(NORMAL_PATH _hdr)
    target_precompile_headers(${tgt} PRIVATE "${_hdr}")
    target_compile_options(${tgt} PRIVATE -fpch-codegen -fpch-instantiate-templates)

    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/${tgt}_gaia")
    set(_rsp "${_dir}/flags.rsp")
    # The codegen object must be built from the same header set as the target's PCH (the inline functions of EVERY header in it are assumed to be
    # defined in that object): one generated header that includes them all, in order.
    get_target_property(_pchs ${tgt} PRECOMPILE_HEADERS)
    set(_all "")
    foreach(_h IN LISTS _pchs)
        if(_h MATCHES "^<")
            string(APPEND _all "#include ${_h}\n")
        else()
            string(APPEND _all "#include \"${_h}\"\n")
        endif()
    endforeach()
    set(_hdr "${_dir}/pch_all.h")
    file(GENERATE OUTPUT "${_hdr}" CONTENT "${_all}")
    set(_obj "${_dir}/gaia_codegen.o")
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
    separate_arguments(_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}}")
    set(_inc "$<TARGET_PROPERTY:${tgt},INCLUDE_DIRECTORIES>")
    set(_def "$<TARGET_PROPERTY:${tgt},COMPILE_DEFINITIONS>")
    set(_opt "$<TARGET_PROPERTY:${tgt},COMPILE_OPTIONS>")
    file(GENERATE OUTPUT "${_rsp}" CONTENT
"-std=gnu++17
$<JOIN:${_flags},\n>
$<JOIN:${_opt},\n>
$<$<BOOL:${_def}>:-D$<JOIN:${_def},\n-D>>
$<$<BOOL:${_inc}>:-I$<JOIN:${_inc},\n-I>>
-Qunused-arguments
")
    add_custom_command(
        OUTPUT "${_obj}"
        COMMAND ${CMAKE_COMMAND} -DCXX=${CMAKE_CXX_COMPILER} -DRSP=${_rsp} -DHDR=${_hdr} -DWORK=${_dir} -DOBJ=${_obj} -DDEPFILE=${_dir}/gaia_codegen.d
                -DCACHE_DIR=${MD_GAIA_PCH_CACHE} -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MdGaiaPchObj.cmake"
        DEPENDS "${_hdr}" "${_rsp}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MdGaiaPchObj.cmake"
        DEPFILE "${_dir}/gaia_codegen.d"
        VERBATIM
        COMMENT "gaia codegen object for ${tgt}")
    add_custom_target(${tgt}_gaia_codegen DEPENDS "${_obj}")
    add_dependencies(${tgt} ${tgt}_gaia_codegen)
    target_link_libraries(${tgt} PRIVATE "${_obj}")
endfunction()
