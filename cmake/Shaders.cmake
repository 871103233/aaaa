# 把 GLSL 编译为 SDL3_gpu 可消费的 Shader 产物。
#
# 为什么需要两种格式：SDL3_gpu 的 Shader 格式与后端强耦合——
#   Vulkan 后端要求 SPIR-V，D3D12 后端要求 DXIL。
# 而 Windows 上 SDL3_gpu 默认选 D3D12，因此只产出 SPIR-V 会在
# SDL_CreateGPUShader 处触发 "Incompatible shader format for GPU backend"。
# 所以这里两种都生成，运行时按设备能力选择（见 triangle_renderer.cpp）。
#
# 工具链（均可由 vcpkg 提供）：
#   glslc       : GLSL     -> SPIR-V   （shaderc 端口）
#   shadercross : SPIR-V   -> DXIL     （sdl3-shadercross 端口）
#
# 缺少 glslc      ：警告并跳过，不阻断配置（运行时会因缺少产物而失败）。
# 缺少 shadercross：仅产出 SPIR-V，Vulkan 路径仍可用，D3D12 不可用。
#
# 用法：
#   add_shader(<target> <glsl-file> [<glsl-file> ...])
#
# 产物输出到 <build>/assets/shaders/<name>.spv 与 <name>.dxil

find_program(VOXEL_GLSLC
    NAMES glslc
    HINTS ENV VULKAN_SDK
    PATH_SUFFIXES Bin bin
)

find_program(VOXEL_SHADERCROSS
    NAMES shadercross
    HINTS ENV VULKAN_SDK
    PATH_SUFFIXES Bin bin
)

set(VOXEL_SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/assets/shaders)
file(MAKE_DIRECTORY ${VOXEL_SHADER_OUT_DIR})

# GLSL 扩展名 -> shadercross 的 stage 名（shadercross 无法从 .vert.spv 推断）
function(voxel_shader_stage GLSL_FILE OUT_VAR)
    get_filename_component(_ext ${GLSL_FILE} LAST_EXT)
    string(TOLOWER ${_ext} _ext)
    if(_ext STREQUAL ".vert")
        set(${OUT_VAR} vertex PARENT_SCOPE)
    elseif(_ext STREQUAL ".frag")
        set(${OUT_VAR} fragment PARENT_SCOPE)
    elseif(_ext STREQUAL ".comp")
        set(${OUT_VAR} compute PARENT_SCOPE)
    else()
        message(FATAL_ERROR "无法识别的 Shader 扩展名（应为 .vert/.frag/.comp）：${GLSL_FILE}")
    endif()
endfunction()

function(add_shader TARGET)
    foreach(GLSL_FILE ${ARGN})
        if(NOT EXISTS ${GLSL_FILE})
            message(FATAL_ERROR "Shader 源文件不存在：${GLSL_FILE}")
        endif()

        get_filename_component(SHADER_NAME ${GLSL_FILE} NAME)
        string(REPLACE "." "_" SHADER_KEY ${SHADER_NAME})
        set(SPV_FILE  ${VOXEL_SHADER_OUT_DIR}/${SHADER_NAME}.spv)
        set(DXIL_FILE ${VOXEL_SHADER_OUT_DIR}/${SHADER_NAME}.dxil)

        if(VOXEL_GLSLC)
            add_custom_command(
                OUTPUT  ${SPV_FILE}
                COMMAND ${VOXEL_GLSLC} --target-env=vulkan1.1 -O ${GLSL_FILE} -o ${SPV_FILE}
                DEPENDS ${GLSL_FILE}
                COMMENT "编译 Shader：${SHADER_NAME} -> SPIR-V"
                VERBATIM
            )
            add_custom_target(${TARGET}_shader_spirv_${SHADER_KEY} DEPENDS ${SPV_FILE})

            if(VOXEL_SHADERCROSS)
                voxel_shader_stage(${GLSL_FILE} SHADER_STAGE)
                add_custom_command(
                    OUTPUT  ${DXIL_FILE}
                    COMMAND ${VOXEL_SHADERCROSS} ${SPV_FILE}
                            --source SPIRV --dest DXIL --stage ${SHADER_STAGE}
                            --output ${DXIL_FILE}
                    DEPENDS ${SPV_FILE}
                    COMMENT "转换 Shader：${SHADER_NAME}.spv -> DXIL（D3D12 后端）"
                    VERBATIM
                )
                add_custom_target(${TARGET}_shader_dxil_${SHADER_KEY} DEPENDS ${DXIL_FILE})
                add_dependencies(${TARGET}_shader_spirv_${SHADER_KEY}
                                 ${TARGET}_shader_dxil_${SHADER_KEY})
            else()
                message(STATUS
                    "未找到 shadercross，仅产出 SPIR-V：${SHADER_NAME}"
                    "（Vulkan 后端可用，D3D12 后端不可用；"
                    "该工具由 vcpkg.json 的 host 依赖 `sdl3-shadercross` 提供，请检查 VCPKG_ROOT 后重新配置）")
            endif()

            add_dependencies(${TARGET} ${TARGET}_shader_spirv_${SHADER_KEY})
        else()
            message(WARNING
                "未找到 glslc，跳过 Shader 编译：${SHADER_NAME}\n"
                "  该工具由 vcpkg.json 的 host 依赖 `shaderc` 提供，请检查 VCPKG_ROOT 后重新配置。\n"
                "  不要依赖手工 PATH 或 Vulkan SDK（见 docs/adr/0002-shader-dual-format-pipeline.md）。")
        endif()
    endforeach()
endfunction()
