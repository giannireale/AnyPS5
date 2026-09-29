#ifndef CODEGEN_X86_STUBBODYBUILDER_HPP
#define CODEGEN_X86_STUBBODYBUILDER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace Codegen {

struct LoweredBody {
    std::vector<std::uint8_t> Bytes;
    std::size_t ReturnBranchOffset;
};

using StubConstant = std::array<std::uint8_t, 16>;

inline constexpr std::uint8_t kStubPrefixPacked = 0x66;
inline constexpr std::uint8_t kStubShiftDword = 0x72;
inline constexpr std::uint8_t kStubShiftQword = 0x73;
inline constexpr std::uint8_t kGprMov = 0x89;
inline constexpr std::uint8_t kGprAdd = 0x01;
inline constexpr std::uint8_t kGprOr = 0x09;
inline constexpr std::uint8_t kGprAnd = 0x21;
inline constexpr std::uint8_t kGprXor = 0x31;

class StubBodyBuilder {
public:
    void Sse(std::uint8_t prefix, std::initializer_list<std::uint8_t> opcode, std::uint8_t dst, std::uint8_t src);
    void ShiftImm(std::uint8_t opcode, std::uint8_t extension, std::uint8_t reg, std::uint8_t imm);
    void RipOperand(std::initializer_list<std::uint8_t> opcode, std::uint8_t reg, const StubConstant& constant);
    void Spill(std::uint8_t reg);
    void Restore(std::uint8_t reg);
    void Raw(std::span<const std::uint8_t> bytes);
    void AdjustStack(std::int32_t delta);
    void PushFlags();
    void PopFlags();
    void StoreXmm(std::uint8_t reg, std::int32_t offset);
    void LoadXmm(std::uint8_t reg, std::int32_t offset);
    void StoreQword(std::uint8_t reg, std::int32_t offset);
    void LoadQword(std::uint8_t reg, std::int32_t offset);
    void StoreDword(std::uint8_t reg, std::int32_t offset);
    void LoadDword(std::uint8_t reg, std::int32_t offset);
    void AddDwordFromStack(std::uint8_t reg, std::int32_t offset);
    void GprBinary(std::uint8_t opcode, std::uint8_t dst, std::uint8_t src);
    void GprNot(std::uint8_t reg);
    void GprRotate(bool left, std::uint8_t reg, std::uint8_t count);
    void GprAddImmediate(std::uint8_t reg, std::uint32_t value);
    void LoadXmmIndirect(std::uint8_t reg, std::uint8_t rexExtension, std::span<const std::uint8_t> address);
    void LoadXmmStackRelative(std::uint8_t reg, std::uint8_t rexExtension, std::uint8_t sib, std::int32_t offset);
    [[nodiscard]] LoweredBody Finish();

private:
    void _stackOperand(std::initializer_list<std::uint8_t> opcode, std::uint8_t prefix, bool wide, std::uint8_t reg, std::int32_t offset);

    struct Fixup {
        std::size_t DisplacementOffset;
        std::size_t InstructionEnd;
        std::size_t ConstantIndex;
    };

    std::vector<std::uint8_t> _bytes;
    std::vector<Fixup> _fixups;
    std::vector<StubConstant> _constants;
};

void EmitSse(std::vector<std::uint8_t>& out, std::uint8_t prefix, std::initializer_list<std::uint8_t> opcode, std::uint8_t dst, std::uint8_t src);
void EmitShiftImm(std::vector<std::uint8_t>& out, std::uint8_t opcode, std::uint8_t extension, std::uint8_t reg, std::uint8_t imm);
void EmitNopFill(std::vector<std::uint8_t>& out, std::size_t count);

}

#endif
