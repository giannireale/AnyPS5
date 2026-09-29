#include <codegen/x86/StubBodyBuilder.hpp>
#include <codegen/x86/Amd64OnlySubstitutionTable.hpp>
#include <algorithm>

namespace Codegen {

namespace {

using namespace Amd64OnlySubstitutionTable;

constexpr std::uint8_t kPrefixScalar = 0xF3;
constexpr std::uint8_t kRexBase = 0x40;
constexpr std::uint8_t kRexR = 0x04;
constexpr std::uint8_t kRexB = 0x01;
constexpr std::uint8_t kModRmRegister = 0xC0;
constexpr std::uint8_t kModRmRip = 0x05;
constexpr std::uint8_t kModRmRspBase = 0x04;
constexpr std::uint8_t kSibRsp = 0x24;

std::uint8_t _rex(const std::uint8_t reg, const std::uint8_t rm) {
    return static_cast<std::uint8_t>(kRexBase | ((reg & 8) != 0 ? kRexR : 0) | ((rm & 8) != 0 ? kRexB : 0));
}

void _emit(std::vector<std::uint8_t>& out, const std::uint8_t prefix, const std::uint8_t reg, const std::uint8_t rm, const std::initializer_list<std::uint8_t> opcode, const std::uint8_t modrm) {
    out.push_back(prefix);
    const auto rex = _rex(reg, rm);
    if (rex != kRexBase)
        out.push_back(rex);
    out.insert(out.end(), opcode.begin(), opcode.end());
    out.push_back(modrm);
}

}

void EmitSse(std::vector<std::uint8_t>& out, const std::uint8_t prefix, const std::initializer_list<std::uint8_t> opcode, const std::uint8_t dst, const std::uint8_t src) {
    _emit(out, prefix, dst, src, opcode, static_cast<std::uint8_t>(kModRmRegister | ((dst & 7) << 3) | (src & 7)));
}

void EmitShiftImm(std::vector<std::uint8_t>& out, const std::uint8_t opcode, const std::uint8_t extension, const std::uint8_t reg, const std::uint8_t imm) {
    _emit(out, kStubPrefixPacked, 0, reg, {0x0F, opcode}, static_cast<std::uint8_t>(kModRmRegister | (extension << 3) | (reg & 7)));
    out.push_back(imm);
}

void EmitNopFill(std::vector<std::uint8_t>& out, std::size_t count) {
    while (count > 0) {
        const auto& nop = kNops[std::min<std::size_t>(count, std::size(kNops)) - 1];
        out.insert(out.end(), nop.Bytes, nop.Bytes + nop.Size);
        count -= nop.Size;
    }
}

void StubBodyBuilder::Sse(const std::uint8_t prefix, const std::initializer_list<std::uint8_t> opcode, const std::uint8_t dst, const std::uint8_t src) {
    EmitSse(_bytes, prefix, opcode, dst, src);
}

void StubBodyBuilder::ShiftImm(const std::uint8_t opcode, const std::uint8_t extension, const std::uint8_t reg, const std::uint8_t imm) {
    EmitShiftImm(_bytes, opcode, extension, reg, imm);
}

void StubBodyBuilder::RipOperand(const std::initializer_list<std::uint8_t> opcode, const std::uint8_t reg, const StubConstant& constant) {
    _emit(_bytes, kStubPrefixPacked, reg, 0, opcode, static_cast<std::uint8_t>(((reg & 7) << 3) | kModRmRip));
    const auto existing = std::find(_constants.begin(), _constants.end(), constant);
    const auto index = static_cast<std::size_t>(existing - _constants.begin());
    _fixups.push_back({_bytes.size(), _bytes.size() + 4, index});
    _bytes.insert(_bytes.end(), 4, 0);
    if (existing == _constants.end())
        _constants.push_back(constant);
}

void StubBodyBuilder::Spill(const std::uint8_t reg) {
    _bytes.insert(_bytes.end(), kLeaRspBelowRedZone.Bytes, kLeaRspBelowRedZone.Bytes + kLeaRspBelowRedZone.Size);
    _emit(_bytes, kPrefixScalar, reg, 0, {0x0F, 0x7F}, static_cast<std::uint8_t>(((reg & 7) << 3) | kModRmRspBase));
    _bytes.push_back(kSibRsp);
}

void StubBodyBuilder::Restore(const std::uint8_t reg) {
    _emit(_bytes, kPrefixScalar, reg, 0, {0x0F, 0x6F}, static_cast<std::uint8_t>(((reg & 7) << 3) | kModRmRspBase));
    _bytes.push_back(kSibRsp);
    _bytes.insert(_bytes.end(), kLeaRspRestore.Bytes, kLeaRspRestore.Bytes + kLeaRspRestore.Size);
}

void StubBodyBuilder::Raw(const std::span<const std::uint8_t> bytes) {
    _bytes.insert(_bytes.end(), bytes.begin(), bytes.end());
}

LoweredBody StubBodyBuilder::Finish() {
    const auto returnBranchOffset = _bytes.size();
    _bytes.insert(_bytes.end(), kJmpRel32.Bytes, kJmpRel32.Bytes + kJmpRel32.Size);
    std::vector<std::size_t> constantOffsets;
    for (const auto& constant : _constants) {
        while (_bytes.size() % kStubAlignment != 0)
            _bytes.push_back(kTrapFill);
        constantOffsets.push_back(_bytes.size());
        _bytes.insert(_bytes.end(), constant.begin(), constant.end());
    }
    for (const auto& fixup : _fixups) {
        const auto displacement = static_cast<std::int64_t>(constantOffsets[fixup.ConstantIndex]) - static_cast<std::int64_t>(fixup.InstructionEnd);
        const auto value = static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
        for (std::size_t index = 0; index < 4; ++index)
            _bytes[fixup.DisplacementOffset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
    return {std::move(_bytes), returnBranchOffset};
}

}
