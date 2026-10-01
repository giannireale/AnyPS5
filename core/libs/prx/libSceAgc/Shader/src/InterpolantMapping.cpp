#include "prx/libSceAgc/Shader/include/InterpolantMapping.hpp"

#include <cstdio>
#include <stdexcept>
#include <prx/libc/include/General.hpp>

#include "SceShaders.hpp"
#include "prx/libSceAgc/Shader/include/ShaderUtils.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"

namespace {
int CreateInterpolantMapping(ShaderRegister* regs, const Shader* gs, const Shader* ps,
                             bool identityUnused) {
    constexpr auto fn = __func__;
    if (regs == nullptr) {
        throw std::runtime_error(std::string(fn) + ": regs is null");
    }

    if (ps == nullptr || ps->num_input_semantics == 0) {
        for (std::uint32_t i = 0; i < 32; ++i)
            SetInterpolantRegister(regs, i, identityUnused ? i : 0);
        return 0;
    }

    if (ps->num_input_semantics > 32)
        return ShaderRegs::GRAPHICS5_ERROR_INVALID_SHADER_PROGRAM;

    if (ps->num_input_semantics != 0 && ps->input_semantics == nullptr) {
        throw std::runtime_error(std::string(fn) + ": ps->input_semantics is null but num_input_semantics != 0");
    }

    if (gs == nullptr) {
        throw std::runtime_error(std::string(fn) + ": gs is null");
    }

    if (gs->num_output_semantics != 0 && gs->output_semantics == nullptr) {
        throw std::runtime_error(std::string(fn) + ": gs->output_semantics is null but num_output_semantics != 0");
    }

    for (std::uint32_t i = 0; i < ps->num_input_semantics; ++i) {
        const ShaderSemantic& psSemantic = ps->input_semantics[i];
        const ShaderSemantic* gsSemantic = FindOutputSemantic(gs, psSemantic.semantic);
        const std::uint32_t psWord = ShaderSemanticWord(psSemantic);

        std::uint32_t value = ((psWord & 0x00300000u) != 0)
            ? CreateInterpolantF16Value(psWord, gsSemantic)
            : CreateInterpolantNonF16Value(psWord, gsSemantic);

        value = (gsSemantic == nullptr)
            ? CreateInterpolantDefaultValue(value, psWord)
            : CreateInterpolantMappingValue(value, psWord, ShaderSemanticWord(*gsSemantic));

        SetInterpolantRegister(regs, i, value);
    }

    for (std::uint32_t i = ps->num_input_semantics; i < 32; ++i)
        SetInterpolantRegister(regs, i, identityUnused ? i : 0);
    return 0;
}
}

extern "C" {
APS5_EXPORT("HV4j+E0MBHE", sceAgcCreateInterpolantMapping);
int APS5_VABI sceAgcCreateInterpolantMapping(ShaderRegister* regs, const Shader* gs, const Shader* ps) {
    return CreateInterpolantMapping(regs, gs, ps, true);
}

// SDK mapping entry points used by ANIMAL WELL. Its wrappers pass an output
// register array, a GS producer and a PS consumer; unused inputs are disabled.
APS5_EXPORT("dbOlWdppb4o", sceAgcCreateInterpolantMappingSdk);
APS5_EXPORT("vieBRwlh1Lw", sceAgcCreateInterpolantMappingSdk);
int APS5_VABI sceAgcCreateInterpolantMappingSdk(ShaderRegister* regs, const Shader* gs, const Shader* ps) {
    using namespace ShaderRegs;
    // A GS without a PS (the title's first, depth-only pipeline) still gets all 32 interpolants
    // disabled; leaving them unwritten sent stale stack words to the CP as register offsets.
    if (!regs || (gs == nullptr && ps != nullptr) ||
        (gs && (gs->type != static_cast<std::uint8_t>(ShaderBinaryType::Gs) ||
                (gs->num_output_semantics && !gs->output_semantics))) ||
        (ps && (ps->type != static_cast<std::uint8_t>(ShaderBinaryType::Ps) ||
                ps->num_input_semantics > 32 ||
                (ps->num_input_semantics && !ps->input_semantics))))
        return GRAPHICS5_ERROR_INVALID_SHADER_PROGRAM;
    return CreateInterpolantMapping(regs, gs, ps, false);
}

}
