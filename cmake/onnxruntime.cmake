function(llama_require_onnxruntime)
    if (TARGET onnxruntime::onnxruntime)
        return()
    endif()

    set(_ort_roots)

    foreach(_env_var ONNXRUNTIME_ROOT ONNXRUNTIME_DIR ONNXRUNTIME_PATH)
        if (DEFINED ENV{${_env_var}} AND NOT "$ENV{${_env_var}}" STREQUAL "")
            list(APPEND _ort_roots "$ENV{${_env_var}}")
        endif()
    endforeach()

    if (WIN32)
        file(GLOB _ort_candidates
            "C:/Program Files/Microsoft Visual Studio/*/*/Common7/IDE/CommonExtensions/Microsoft/ModelBuilder/AutoMLService"
            "C:/Program Files (x86)/Microsoft Visual Studio/*/*/Common7/IDE/CommonExtensions/Microsoft/ModelBuilder/AutoMLService"
            "C:/Program Files/Microsoft Visual Studio/*/*/Common7/IDE/Extensions/Microsoft/IntelliCode/win-x64/native"
            "C:/Program Files (x86)/Microsoft Visual Studio/*/*/Common7/IDE/Extensions/Microsoft/IntelliCode/win-x64/native"
            "C:/Program Files/Microsoft Office/root/Office16/WinAppSDK"
            "C:/Program Files (x86)/Microsoft Office/root/Office16/WinAppSDK"
        )
        list(APPEND _ort_roots ${_ort_candidates})
    else()
        list(APPEND _ort_roots
            /usr
            /usr/local
            /opt
            /opt/homebrew
        )
    endif()

    list(REMOVE_DUPLICATES _ort_roots)

    set(_ort_library_hints ${_ort_roots})
    foreach(_root ${_ort_roots})
        list(APPEND _ort_library_hints "${_root}/lib" "${_root}/lib64")
    endforeach()
    list(REMOVE_DUPLICATES _ort_library_hints)

    find_library(ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        HINTS ${_ort_library_hints}
    )
    if (NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR "ONNX Runtime library not found. Set ONNXRUNTIME_ROOT/ONNXRUNTIME_DIR or install ONNX Runtime locally.")
    endif()

    if (WIN32)
        set(_ort_runtime_hints ${_ort_roots})
        if (EXISTS "${PROJECT_SOURCE_DIR}/.tmp_onnxruntime/onnxruntime/capi/onnxruntime.dll")
            set(ONNXRUNTIME_DLL "${PROJECT_SOURCE_DIR}/.tmp_onnxruntime/onnxruntime/capi/onnxruntime.dll" CACHE FILEPATH "onnxruntime.dll" FORCE)
            list(PREPEND _ort_runtime_hints "${PROJECT_SOURCE_DIR}/.tmp_onnxruntime/onnxruntime/capi")
        endif()
        foreach(_root ${_ort_roots})
            list(APPEND _ort_runtime_hints "${_root}/bin")
        endforeach()
        list(REMOVE_DUPLICATES _ort_runtime_hints)

        if (NOT ONNXRUNTIME_DLL)
            find_file(ONNXRUNTIME_DLL
                NAMES onnxruntime.dll
                HINTS ${_ort_runtime_hints}
            )
        endif()
        if (NOT ONNXRUNTIME_DLL)
            message(FATAL_ERROR "onnxruntime.dll not found. Set ONNXRUNTIME_ROOT/ONNXRUNTIME_DIR or install ONNX Runtime locally.")
        endif()
    endif()

    add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
    if (WIN32)
        set_target_properties(onnxruntime::onnxruntime PROPERTIES
            IMPORTED_IMPLIB "${ONNXRUNTIME_LIBRARY}"
            IMPORTED_LOCATION "${ONNXRUNTIME_DLL}"
            INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/vendor"
        )
    else()
        set_target_properties(onnxruntime::onnxruntime PROPERTIES
            IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/vendor"
        )
    endif()
endfunction()

function(llama_stage_onnxruntime target)
    if (WIN32)
        get_target_property(_ort_runtime_dll onnxruntime::onnxruntime IMPORTED_LOCATION)
        if (EXISTS "${_ort_runtime_dll}")
            get_filename_component(_ort_runtime_dir "${_ort_runtime_dll}" DIRECTORY)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_ort_runtime_dll}"
                        "$<TARGET_FILE_DIR:${target}>/onnxruntime.dll"
            )
            if (EXISTS "${_ort_runtime_dir}/onnxruntime_providers_shared.dll")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_ort_runtime_dir}/onnxruntime_providers_shared.dll"
                            "$<TARGET_FILE_DIR:${target}>/onnxruntime_providers_shared.dll"
                )
            endif()
        else()
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_PROPERTY:onnxruntime::onnxruntime,IMPORTED_LOCATION>"
                        "$<TARGET_FILE_DIR:${target}>/onnxruntime.dll"
            )
        endif()
    endif()
endfunction()
