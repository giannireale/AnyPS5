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

constexpr std::uint8_t kPxor[] = {0x0F, 0xEF};
constexpr std::uint8_t kShiftLeftBytes = 7;
constexpr std::uint8_t kShiftRightBytes = 3;
constexpr std::uint8_t kDwordBits = 32;

template <std::size_t TCount>
std::array<std::uint8_t, TCount> _scratchRegisters(const ShaNiOperands& operands, const bool keepXmm0) {
    std::array<std::uint8_t, TCount> scratch{};
    std::uint8_t found = 0;
    for (std::uint8_t reg = 0; found < TCount; ++reg) {
        if (reg == operands.Destination || reg == operands.Source) continue;
        if (keepXmm0 && reg == 0) continue;
        scratch[found++] = reg;
    }
    return scratch;
}

void _move(StubBodyBuilder& body, const std::uint8_t dst, const std::uint8_t src) {
    body.Sse(kStubPrefixPacked, {kMovdqa[0], kMovdqa[1]}, dst, src);
}

void _rotateLeft(StubBodyBuilder& body, const std::uint8_t reg, const std::uint8_t temp, const std::uint8_t count) {
    _move(body, temp, reg);
    body.ShiftImm(kStubShiftDword, kShiftLeftDword, reg, count);
    body.ShiftImm(kStubShiftDword, kShiftRightDword, temp, static_cast<std::uint8_t>(kDwordBits - count));
    body.Sse(kStubPrefixPacked, {kPor[0], kPor[1]}, reg, temp);
}

void _sigma(StubBodyBuilder& body, const std::uint8_t out, const std::uint8_t in, const std::uint8_t tempA, const std::uint8_t tempB, const std::uint8_t rotateA, const std::uint8_t rotateB, const std::uint8_t shift) {
    _move(body, out, in);
    _rotateLeft(body, out, tempA, rotateA);
    _move(body, tempB, in);
    _rotateLeft(body, tempB, tempA, rotateB);
    body.Sse(kStubPrefixPacked, {kPxor[0], kPxor[1]}, out, tempB);
    _move(body, tempB, in);
    body.ShiftImm(kStubShiftDword, kShiftRightDword, tempB, shift);
    body.Sse(kStubPrefixPacked, {kPxor[0], kPxor[1]}, out, tempB);
}

void _emitSha1Nexte(StubBodyBuilder& body, const ShaNiOperands& operands) {
    const auto scratch = _scratchRegisters<2>(operands, false);
    StubConstant highLane{};
    for (std::size_t index = kHighLaneOffset; index < highLane.size(); ++index)
        highLane[index] = 0xFF;
    body.Spill(scratch[0]);
    body.Spill(scratch[1]);
    _move(body, scratch[0], operands.Destination);
    body.ShiftImm(kStubShiftDword, kShiftLeftDword, scratch[0], kRotateLeft);
    _move(body, scratch[1], operands.Destination);
    body.ShiftImm(kStubShiftDword, kShiftRightDword, scratch[1], kRotateRight);
    body.Sse(kStubPrefixPacked, {kPor[0], kPor[1]}, scratch[0], scratch[1]);
    body.RipOperand({kPand[0], kPand[1]}, scratch[0], highLane);
    _move(body, operands.Destination, operands.Source);
    body.Sse(kStubPrefixPacked, {kPaddd[0], kPaddd[1]}, operands.Destination, scratch[0]);
    body.Restore(scratch[1]);
    body.Restore(scratch[0]);
}

void _emitSha1Msg1(StubBodyBuilder& body, const ShaNiOperands& operands) {
    const auto scratch = _scratchRegisters<2>(operands, false);
    body.Spill(scratch[0]);
    body.Spill(scratch[1]);
    _move(body, scratch[0], operands.Destination);
    body.ShiftImm(kStubShiftQword, kShiftLeftBytes, scratch[0], 8);
    _move(body, scratch[1], operands.Source);
    body.ShiftImm(kStubShiftQword, kShiftRightBytes, scratch[1], 8);
    body.Sse(kStubPrefixPacked, {kPor[0], kPor[1]}, scratch[0], scratch[1]);
    body.Sse(kStubPrefixPacked, {kPxor[0], kPxor[1]}, operands.Destination, scratch[0]);
    body.Restore(scratch[1]);
    body.Restore(scratch[0]);
}

void _emitSha1Msg2(StubBodyBuilder& body, const ShaNiOperands& operands) {
    const auto scratch = _scratchRegisters<3>(operands, false);
    for (const auto reg : scratch) body.Spill(reg);
    _move(body, scratch[0], operands.Source);
    body.ShiftImm(kStubShiftQword, kShiftLeftBytes, scratch[0], 4);
    body.Sse(kStubPrefixPacked, {kPxor[0], kPxor[1]}, scratch[0], operands.Destination);
    _rotateLeft(body, scratch[0], scratch[1], 1);
    _move(body, scratch[1], scratch[0]);
    body.ShiftImm(kStubShiftQword, kShiftRightBytes, scratch[1], 12);
    _rotateLeft(body, scratch[1], scratch[2], 1);
    body.Sse(kStubPrefixPacked, {kPxor[0], kPxor[1]}, scratch[0], scratch[1]);
    _move(body, operands.Destination, scratch[0]);
    for (auto index = scratch.size(); index > 0; --index) body.Restore(scratch[index - 1]);
}

void _emitSha256Msg1(StubBodyBuilder& body, const ShaNiOperands& operands) {
    const auto scratch = _scratchRegisters<4>(operands, false);
    for (const auto reg : scratch) body.Spill(reg);
    _move(body, scratch[0], operands.Destination);
    body.ShiftImm(kStubShiftQword, kShiftRightBytes, scratch[0], 4);
    _move(body, scratch[1], operands.Source);
    body.ShiftImm(kStubShiftQword, kShiftLeftBytes, scratch[1], 12);
    body.Sse(kStubPrefixPacked, {kPor[0], kPor[1]}, scratch[0], scratch[1]);
    _sigma(body, scratch[1], scratch[0], scratch[2], scratch[3], 25, 14, 3);
    body.Sse(kStubPrefixPacked, {kPaddd[0], kPaddd[1]}, operands.Destination, scratch[1]);
    for (auto index = scratch.size(); index > 0; --index) body.Restore(scratch[index - 1]);
}

void _emitSha256Msg2(StubBodyBuilder& body, const ShaNiOperands& operands) {
    const auto scratch = _scratchRegisters<4>(operands, false);
    for (const auto reg : scratch) body.Spill(reg);
    _move(body, scratch[0], operands.Source);
    body.ShiftImm(kStubShiftQword, kShiftRightBytes, scratch[0], 8);
    _sigma(body, scratch[1], scratch[0], scratch[2], scratch[3], 15, 13, 10);
    body.Sse(kStubPrefixPacked, {kPaddd[0], kPaddd[1]}, operands.Destination, scratch[1]);
    _move(body, scratch[0], operands.Destination);
    body.ShiftImm(kStubShiftQword, kShiftLeftBytes, scratch[0], 8);
    _sigma(body, scratch[1], scratch[0], scratch[2], scratch[3], 15, 13, 10);
    body.Sse(kStubPrefixPacked, {kPaddd[0], kPaddd[1]}, operands.Destination, scratch[1]);
    for (auto index = scratch.size(); index > 0; --index) body.Restore(scratch[index - 1]);
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
    if (operands.ThreeByte3A)
        return false;
    switch (operands.Opcode) {
    case kShaOpSha1Nexte:
    case kShaOpSha1Msg1:
    case kShaOpSha1Msg2:
    case kShaOpSha256Msg1:
    case kShaOpSha256Msg2:
        return true;
    default:
        return false;
    }
}

LoweredBody ShaNiLowering::LowerOutOfLine(const std::span<const ShaNiOperands> sequence, const std::span<const std::uint8_t> trailing) const {
    StubBodyBuilder body;
    for (const auto& operands : sequence) {
        if (!CanLower(operands))
            throw CodegenException("SHA-NI opcode has no Intel lowering");
        switch (operands.Opcode) {
        case kShaOpSha1Nexte:
            _emitSha1Nexte(body, operands);
            break;
        case kShaOpSha1Msg1:
            _emitSha1Msg1(body, operands);
            break;
        case kShaOpSha1Msg2:
            _emitSha1Msg2(body, operands);
            break;
        case kShaOpSha256Msg1:
            _emitSha256Msg1(body, operands);
            break;
        default:
            _emitSha256Msg2(body, operands);
            break;
        }
    }
    body.Raw(trailing);
    return body.Finish();
}

}
