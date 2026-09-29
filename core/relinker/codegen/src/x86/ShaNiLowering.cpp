#include <codegen/x86/ShaNiLowering.hpp>
#include <codegen/CodegenException.hpp>
#include <array>

namespace Codegen {

namespace {

constexpr std::uint8_t kRexFirst = 0x40;
constexpr std::uint8_t kRexLast = 0x4F;
constexpr std::uint8_t kRexR = 0x04;
constexpr std::uint8_t kRexB = 0x01;
constexpr std::uint8_t kEscape = 0x0F;
constexpr std::uint8_t kEscape38 = 0x38;
constexpr std::uint8_t kEscape3A = 0x3A;
constexpr std::uint8_t kModRmModShift = 6;
constexpr std::uint8_t kModRmRegisterMod = 3;
constexpr std::uint8_t kMovdqa[] = {0x0F, 0x6F};
constexpr std::uint8_t kPor[] = {0x0F, 0xEB};
constexpr std::uint8_t kPand[] = {0x0F, 0xDB};
constexpr std::uint8_t kPaddd[] = {0x0F, 0xFE};
constexpr std::uint8_t kShiftLeftDword = 6;
constexpr std::uint8_t kShiftRightDword = 2;
constexpr std::uint8_t kRotateLeft = 30;
constexpr std::uint8_t kRotateRight = 2;
constexpr std::size_t kHighLaneOffset = 12;

std::array<std::uint8_t, 2> _scratch(const ShaNiOperands& operands) {
    std::array<std::uint8_t, 2> scratch{};
    for (std::uint8_t reg = 0, found = 0; found < scratch.size(); ++reg)
        if (reg != operands.Destination && reg != operands.Source) scratch[found++] = reg;
    return scratch;
}

void _emitSha1Nexte(StubBodyBuilder& body, const ShaNiOperands& operands) {
    const auto scratch = _scratch(operands);
    StubConstant highLane{};
    for (std::size_t index = kHighLaneOffset; index < highLane.size(); ++index)
        highLane[index] = 0xFF;
    body.Spill(scratch[0]);
    body.Spill(scratch[1]);
    body.Sse(kStubPrefixPacked, {kMovdqa[0], kMovdqa[1]}, scratch[0], operands.Destination);
    body.ShiftImm(kStubShiftDword, kShiftLeftDword, scratch[0], kRotateLeft);
    body.Sse(kStubPrefixPacked, {kMovdqa[0], kMovdqa[1]}, scratch[1], operands.Destination);
    body.ShiftImm(kStubShiftDword, kShiftRightDword, scratch[1], kRotateRight);
    body.Sse(kStubPrefixPacked, {kPor[0], kPor[1]}, scratch[0], scratch[1]);
    body.RipOperand({kPand[0], kPand[1]}, scratch[0], highLane);
    body.Sse(kStubPrefixPacked, {kMovdqa[0], kMovdqa[1]}, operands.Destination, operands.Source);
    body.Sse(kStubPrefixPacked, {kPaddd[0], kPaddd[1]}, operands.Destination, scratch[0]);
    body.Restore(scratch[1]);
    body.Restore(scratch[0]);
}

}

ShaNiOperands DecodeShaNi(const std::uint8_t* data, const std::size_t length) {
    std::size_t position = 0;
    std::uint8_t rex = 0;
    while (position < length && data[position] >= kRexFirst && data[position] <= kRexLast)
        rex = data[position++];
    if (position + 3 >= length + 1 && position + 3 > length)
        throw CodegenException("SHA-NI instruction truncated before its ModRM byte");
    if (data[position] != kEscape || (data[position + 1] != kEscape38 && data[position + 1] != kEscape3A))
        throw CodegenException("Instruction is not a SHA-NI opcode");
    ShaNiOperands operands;
    operands.ThreeByte3A = data[position + 1] == kEscape3A;
    operands.Opcode = data[position + 2];
    if (position + 3 >= length)
        throw CodegenException("SHA-NI instruction truncated before its ModRM byte");
    const auto modrm = data[position + 3];
    if ((modrm >> kModRmModShift) != kModRmRegisterMod)
        throw CodegenException("SHA-NI instruction with a memory operand has no Intel lowering");
    operands.Destination = static_cast<std::uint8_t>(((modrm >> 3) & 7) | ((rex & kRexR) != 0 ? 8 : 0));
    operands.Source = static_cast<std::uint8_t>((modrm & 7) | ((rex & kRexB) != 0 ? 8 : 0));
    if (operands.ThreeByte3A) {
        if (position + 4 >= length)
            throw CodegenException("SHA1RNDS4 truncated before its immediate");
        operands.Immediate = data[position + 4];
    }
    return operands;
}

bool ShaNiLowering::CanLower(const ShaNiOperands& operands) const {
    return !operands.ThreeByte3A && operands.Opcode == kShaOpSha1Nexte;
}

LoweredBody ShaNiLowering::LowerOutOfLine(const std::span<const ShaNiOperands> sequence, const std::span<const std::uint8_t> trailing) const {
    StubBodyBuilder body;
    for (const auto& operands : sequence) {
        if (!CanLower(operands))
            throw CodegenException("SHA-NI opcode has no Intel lowering");
        _emitSha1Nexte(body, operands);
    }
    body.Raw(trailing);
    return body.Finish();
}

}
