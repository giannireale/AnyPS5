#include <codegen/x86/StubBodyBuilder.hpp>
#include <codegen/x86/Amd64OnlySubstitutionTable.hpp>
#include <algorithm>
#include <codegen/x86/X64OpcodeConstants.hpp>
#include <codegen/CodegenException.hpp>
#include <limits>

namespace Codegen {

namespace {

using namespace Amd64OnlySubstitutionTable;

constexpr std::uint8_t kPrefixScalar = 0xF3;
constexpr std::uint8_t kRexBase = 0x40;
constexpr std::uint8_t kRexR = 0x04;
constexpr std::uint8_t kRexX = 0x02;
constexpr std::uint8_t kRexB = 0x01;
constexpr std::uint8_t kModRmRegister = 0xC0;
constexpr std::uint8_t kModRmRip = 0x05;
constexpr std::uint8_t kModRmRspBase = 0x04;
constexpr std::uint8_t kSibRsp = 0x24;
constexpr std::uint8_t kShiftDwords = 0x72;
constexpr std::uint8_t kShiftQwords = 0x73;
constexpr std::uint8_t kMovdqu = 0x6F;

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

std::size_t _displacementSize(const std::uint8_t mod, const std::uint8_t rm, const std::uint8_t sib) {
    using namespace X64OpcodeConstants;
    if (mod == ModRmModDisp8)
        return Disp8Size;
    if (mod == ModRmModDisp32 || (mod == ModRmModIndirect && rm == ModRmRmSibPresent && (sib & SibBaseMask) == SibBaseDisp32))
        return Disp32Size;
    return 0;
}

void _shiftImm(std::vector<std::uint8_t>& out, const std::uint8_t opcode, const std::uint8_t extension, const std::uint8_t reg, const std::uint8_t imm) {
    _emit(out, kStubPrefixPacked, 0, reg, {0x0F, opcode}, static_cast<std::uint8_t>(kModRmRegister | (extension << 3) | (reg & 7)));
    out.push_back(imm);
}

}

MemoryOperand DecodeMemoryOperand(const std::uint8_t* data, const std::size_t length, const std::size_t modRmOffset, const std::uint8_t rex, std::vector<std::uint8_t> prefixes) {
    using namespace X64OpcodeConstants;
    if (modRmOffset >= length)
        throw CodegenException("Memory operand truncated before its ModRM byte");
    const auto modrm = data[modRmOffset];
    MemoryOperand operand{std::move(prefixes), static_cast<std::uint8_t>(rex & (kRexX | kRexB)), static_cast<std::uint8_t>((modrm >> ModRmModShift) & ModRmModMask), static_cast<std::uint8_t>(modrm & ModRmRmMask), 0, 0, false};
    if (operand.Mod == ModRmModRegister)
        throw CodegenException("Register operand where a memory operand was expected");
    if (operand.Mod == ModRmModIndirect && operand.Rm == ModRmRmRipRelative)
        throw CodegenException("RIP-relative memory operand cannot move into a stub");
    auto pos = modRmOffset + 1;
    if (operand.Rm == ModRmRmSibPresent) {
        if (pos >= length)
            throw CodegenException("Memory operand truncated before its SIB byte");
        operand.Sib = data[pos++];
        operand.StackBase = (operand.Sib & SibBaseMask) == ModRmRmSibPresent && (rex & kRexB) == 0;
    }
    const auto size = _displacementSize(operand.Mod, operand.Rm, operand.Sib);
    if (pos + size > length)
        throw CodegenException("Memory operand truncated in its displacement");
    if (size == Disp8Size)
        operand.Displacement = static_cast<std::int8_t>(data[pos]);
    for (std::size_t index = 0; size == Disp32Size && index < size; ++index)
        operand.Displacement = static_cast<std::int32_t>(static_cast<std::uint32_t>(operand.Displacement) | (static_cast<std::uint32_t>(data[pos + index]) << (index * 8)));
    return operand;
}

void EmitSse(std::vector<std::uint8_t>& out, const std::uint8_t prefix, const std::initializer_list<std::uint8_t> opcode, const std::uint8_t dst, const std::uint8_t src) {
    _emit(out, prefix, dst, src, opcode, static_cast<std::uint8_t>(kModRmRegister | ((dst & 7) << 3) | (src & 7)));
}

void EmitShiftImm(std::vector<std::uint8_t>& out, const std::uint8_t opcode, const std::uint8_t extension, const std::uint8_t reg, const std::uint8_t imm) {
    _emit(out, kStubPrefixPacked, 0, reg, {0x0F, opcode}, static_cast<std::uint8_t>(kModRmRegister | (extension << 3) | (reg & 7)));
    out.push_back(imm);
}

void EmitShiftImm(std::vector<std::uint8_t>& out, const std::uint8_t extension, const std::uint8_t reg, const std::uint8_t imm) {
    EmitShiftImm(out, kStubShiftQword, extension, reg, imm);
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

void StubBodyBuilder::SsePlain(const std::initializer_list<std::uint8_t> opcode, const std::uint8_t dst, const std::uint8_t src) {
    const auto rex = _rex(dst, src);
    if (rex != kRexBase)
        _bytes.push_back(rex);
    _bytes.insert(_bytes.end(), opcode.begin(), opcode.end());
    _bytes.push_back(static_cast<std::uint8_t>(kModRmRegister | ((dst & 7) << 3) | (src & 7)));
}

void StubBodyBuilder::SseImm(const std::uint8_t prefix, const std::initializer_list<std::uint8_t> opcode, const std::uint8_t dst, const std::uint8_t src, const std::uint8_t imm) {
    EmitSse(_bytes, prefix, opcode, dst, src);
    _bytes.push_back(imm);
}

void StubBodyBuilder::ShiftImm(const std::uint8_t extension, const std::uint8_t reg, const std::uint8_t imm) {
    _shiftImm(_bytes, kShiftQwords, extension, reg, imm);
}

void StubBodyBuilder::ShiftDwordImm(const std::uint8_t extension, const std::uint8_t reg, const std::uint8_t imm) {
    _shiftImm(_bytes, kShiftDwords, extension, reg, imm);
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

void StubBodyBuilder::Load(const std::uint8_t reg, const MemoryOperand& operand) {
    using namespace X64OpcodeConstants;
    const auto displacement = static_cast<std::int64_t>(operand.Displacement) + static_cast<std::int64_t>(operand.StackBase ? _stackDepth : 0);
    if (displacement > std::numeric_limits<std::int32_t>::max())
        throw CodegenException("Stack displacement does not fit after spilling");
    const auto mod = operand.StackBase && _stackDepth != 0 ? ModRmModDisp32 : operand.Mod;
    _bytes.insert(_bytes.end(), operand.Prefixes.begin(), operand.Prefixes.end());
    _bytes.push_back(kPrefixScalar);
    const auto rex = static_cast<std::uint8_t>(kRexBase | ((reg & 8) != 0 ? kRexR : 0) | operand.RexIndexBase);
    if (rex != kRexBase)
        _bytes.push_back(rex);
    _bytes.insert(_bytes.end(), {TwoByteOpcodeEscape, kMovdqu, static_cast<std::uint8_t>((mod << ModRmModShift) | ((reg & 7) << ModRmRegShift) | operand.Rm)});
    if (operand.Rm == ModRmRmSibPresent)
        _bytes.push_back(operand.Sib);
    const auto value = static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
    for (std::size_t index = 0; index < _displacementSize(mod, operand.Rm, operand.Sib); ++index)
        _bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

void StubBodyBuilder::Spill(const std::uint8_t reg) {
    _stackDepth += kRedZoneSpillFrame;
    _bytes.insert(_bytes.end(), kLeaRspBelowRedZone.Bytes, kLeaRspBelowRedZone.Bytes + kLeaRspBelowRedZone.Size);
    _emit(_bytes, kPrefixScalar, reg, 0, {0x0F, 0x7F}, static_cast<std::uint8_t>(((reg & 7) << 3) | kModRmRspBase));
    _bytes.push_back(kSibRsp);
}

void StubBodyBuilder::Restore(const std::uint8_t reg) {
    _emit(_bytes, kPrefixScalar, reg, 0, {0x0F, 0x6F}, static_cast<std::uint8_t>(((reg & 7) << 3) | kModRmRspBase));
    _bytes.push_back(kSibRsp);
    _bytes.insert(_bytes.end(), kLeaRspRestore.Bytes, kLeaRspRestore.Bytes + kLeaRspRestore.Size);
    _stackDepth -= kRedZoneSpillFrame;
}

void StubBodyBuilder::AddRipFixup(const std::size_t bodyOffset, const std::int32_t originalDisplacement, const std::size_t instructionEnd) {
    _ripFixups.push_back({bodyOffset, originalDisplacement, instructionEnd});
}

std::size_t StubBodyBuilder::Size() const {
    return _bytes.size();
}

void StubBodyBuilder::Raw(const std::span<const std::uint8_t> bytes) {
    _bytes.insert(_bytes.end(), bytes.begin(), bytes.end());
}

namespace {

void _appendDword(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    for (int index = 0; index < 4; ++index)
        out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

}

void StubBodyBuilder::AdjustStack(const std::int32_t delta) {
    _bytes.insert(_bytes.end(), {0x48, 0x8D, 0xA4, 0x24});
    _appendDword(_bytes, static_cast<std::uint32_t>(delta));
}

void StubBodyBuilder::PushFlags() {
    _bytes.push_back(0x9C);
}

void StubBodyBuilder::PopFlags() {
    _bytes.push_back(0x9D);
}

void StubBodyBuilder::_stackOperand(const std::initializer_list<std::uint8_t> opcode, const std::uint8_t prefix, const bool wide, const std::uint8_t reg, const std::int32_t offset) {
    if (prefix != 0)
        _bytes.push_back(prefix);
    const auto rex = static_cast<std::uint8_t>((wide ? 0x48 : kRexBase) | ((reg & 8) != 0 ? kRexR : 0));
    if (rex != kRexBase)
        _bytes.push_back(rex);
    _bytes.insert(_bytes.end(), opcode.begin(), opcode.end());
    _bytes.push_back(static_cast<std::uint8_t>(0x80 | ((reg & 7) << 3) | kModRmRspBase));
    _bytes.push_back(kSibRsp);
    _appendDword(_bytes, static_cast<std::uint32_t>(offset));
}

void StubBodyBuilder::StoreXmm(const std::uint8_t reg, const std::int32_t offset) {
    _stackOperand({0x0F, 0x7F}, kPrefixScalar, false, reg, offset);
}

void StubBodyBuilder::LoadXmm(const std::uint8_t reg, const std::int32_t offset) {
    _stackOperand({0x0F, 0x6F}, kPrefixScalar, false, reg, offset);
}

void StubBodyBuilder::StoreQword(const std::uint8_t reg, const std::int32_t offset) {
    _stackOperand({0x89}, 0, true, reg, offset);
}

void StubBodyBuilder::LoadQword(const std::uint8_t reg, const std::int32_t offset) {
    _stackOperand({0x8B}, 0, true, reg, offset);
}

void StubBodyBuilder::StoreDword(const std::uint8_t reg, const std::int32_t offset) {
    _stackOperand({0x89}, 0, false, reg, offset);
}

void StubBodyBuilder::LoadDword(const std::uint8_t reg, const std::int32_t offset) {
    _stackOperand({0x8B}, 0, false, reg, offset);
}

void StubBodyBuilder::AddDwordFromStack(const std::uint8_t reg, const std::int32_t offset) {
    _stackOperand({0x03}, 0, false, reg, offset);
}

void StubBodyBuilder::GprBinary(const std::uint8_t opcode, const std::uint8_t dst, const std::uint8_t src) {
    const auto rex = static_cast<std::uint8_t>(kRexBase | ((src & 8) != 0 ? kRexR : 0) | ((dst & 8) != 0 ? kRexB : 0));
    if (rex != kRexBase)
        _bytes.push_back(rex);
    _bytes.push_back(opcode);
    _bytes.push_back(static_cast<std::uint8_t>(kModRmRegister | ((src & 7) << 3) | (dst & 7)));
}

void StubBodyBuilder::GprNot(const std::uint8_t reg) {
    const auto rex = static_cast<std::uint8_t>(kRexBase | ((reg & 8) != 0 ? kRexB : 0));
    if (rex != kRexBase)
        _bytes.push_back(rex);
    _bytes.push_back(0xF7);
    _bytes.push_back(static_cast<std::uint8_t>(kModRmRegister | (2 << 3) | (reg & 7)));
}

void StubBodyBuilder::GprRotate(const bool left, const std::uint8_t reg, const std::uint8_t count) {
    const auto rex = static_cast<std::uint8_t>(kRexBase | ((reg & 8) != 0 ? kRexB : 0));
    if (rex != kRexBase)
        _bytes.push_back(rex);
    _bytes.push_back(0xC1);
    _bytes.push_back(static_cast<std::uint8_t>(kModRmRegister | ((left ? 0 : 1) << 3) | (reg & 7)));
    _bytes.push_back(count);
}

void StubBodyBuilder::GprAddImmediate(const std::uint8_t reg, const std::uint32_t value) {
    const auto rex = static_cast<std::uint8_t>(kRexBase | ((reg & 8) != 0 ? kRexB : 0));
    if (rex != kRexBase)
        _bytes.push_back(rex);
    _bytes.push_back(0x81);
    _bytes.push_back(static_cast<std::uint8_t>(kModRmRegister | (reg & 7)));
    _appendDword(_bytes, value);
}

void StubBodyBuilder::LoadXmmIndirect(const std::uint8_t reg, const std::uint8_t rexExtension, const std::span<const std::uint8_t> address) {
    _bytes.push_back(kPrefixScalar);
    const auto rex = static_cast<std::uint8_t>(kRexBase | rexExtension | ((reg & 8) != 0 ? kRexR : 0));
    if (rex != kRexBase)
        _bytes.push_back(rex);
    _bytes.insert(_bytes.end(), {0x0F, 0x6F});
    _bytes.push_back(static_cast<std::uint8_t>((address[0] & ~0x38) | ((reg & 7) << 3)));
    _bytes.insert(_bytes.end(), address.begin() + 1, address.end());
}

void StubBodyBuilder::LoadXmmStackRelative(const std::uint8_t reg, const std::uint8_t rexExtension, const std::uint8_t sib, const std::int32_t offset) {
    _bytes.push_back(kPrefixScalar);
    const auto rex = static_cast<std::uint8_t>(kRexBase | rexExtension | ((reg & 8) != 0 ? kRexR : 0));
    if (rex != kRexBase)
        _bytes.push_back(rex);
    _bytes.insert(_bytes.end(), {0x0F, 0x6F});
    _bytes.push_back(static_cast<std::uint8_t>(0x80 | ((reg & 7) << 3) | kModRmRspBase));
    _bytes.push_back(sib);
    _appendDword(_bytes, static_cast<std::uint32_t>(offset));
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
    return {std::move(_bytes), returnBranchOffset, 0, std::move(_ripFixups)};
}

}
