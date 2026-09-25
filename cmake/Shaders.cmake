# 把 GLSL 编译为 SPIR-V（SDL3_gpu 的 Vulkan 后端消费）。
#
# 需要 glslc（Vulkan SDK 自带）或等价的 shadercross。
# 若找不到编译器：给出警告并跳过，不阻断配置——但运行时会因缺少 .spv 而失败，
# 因此只适用于"暂时没有 Shader 工具链"的机器。
#
# 用法：
#   add_shader(<target> <glsl-file> [<glsl-file> ...])
#
# 产物输出到 <build>/assets/shaders/<name>.spv

find_program(VOXEL_GLSLC
    NAMES glslc
    HINTS ENV VULKAN_SDK
    PATH_SUFFIXES Bin bin
)

set(VOXEL_SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/assets/shaders)
file(MAKE_DIRECTORY ${VOXEL_SHADER_OUT_DIR})

function(add_shader TARGET)
    foreach(GLSL_FILE ${ARGN})
        if(NOT EXISTS ${GLSL_FILE})
            message(FATAL_ERROR "Shader 源文件不存在：${GLSL_FILE}")
        endif()

        get_filename_component(SHADER_NAME ${GLSL_FILE} NAME)
        string(REPLACE "." "_" SHADER_KEY ${SHADER_NAME})
        set(SPV_FILE ${VOXEL_SHADER_OUT_DIR}/${SHADER_NAME}.spv)

        if(VOXEL_GLSLC)
            add_custom_command(
                OUTPUT  ${SPV_FILE}
                COMMAND ${VOXEL_GLSLC} --target-env=vulkan1.1 -O ${GLSL_FILE} -o ${SPV_FILE}
                DEPENDS ${GLSL_FILE}
                COMMENT "编译 Shader：${SHADER_NAME} -> SPIR-V"
                VERBATIM
            )
            add_custom_target(${TARGET}_shader_${SHADER_KEY} DEPENDS ${SPV_FILE})
            add_dependencies(${TARGET} ${TARGET}_shader_${SHADER_KEY})
        else()
            message(WARNING
                "未找到 glslc，跳过 Shader 编译：${SHADER_NAME}\n"
                "  安装 Vulkan SDK，或设置环境变量 VULKAN_SDK 后重新配置。")
        endif()
    endforeach()
endfunction()
