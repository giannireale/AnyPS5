#include <algorithm>
#include <codegen/x86/IAmd64OnlyInstructionMatcher.hpp>
#include <codegen/x86/DecodedInstruction.hpp>
#include <codegen/x86/Amd64OnlySubstitutionTable.hpp>
#include <codegen/x86/Sse4aLowering.hpp>
#include <codegen/x86/Sse4aOperands.hpp>
#include <codegen/x86/ShaNiLowering.hpp>
#include <codegen/x86/SystemInstructionLowering.hpp>
#include <codegen/x86/ClzeroLowering.hpp>
#include <codegen/x86/ClzeroOperands.hpp>
#include <codegen/x86/ReciprocalLowering.hpp>
#include <codegen/x86/ReciprocalOperands.hpp>
#include <codegen/x86/Sha256Lowering.hpp>
#include <codegen/x86/Sha256Operands.hpp>
#include <codegen/x86/Sha1Lowering.hpp>
#include <codegen/x86/Sha1Operands.hpp>
#include <codegen/x86/StubBodyBuilder.hpp>
#include <codegen/x86/X64OpcodeConstants.hpp>
#include <codegen/CodegenException.hpp>
#include <memory>
#include <span>
#include <vector>

namespace Codegen {

namespace {

using namespace Amd64OnlySubstitutionTable;

constexpr std::size_t kMaxInstructionLength = 15;

Amd64OnlyMatch _unsupported(const Entry& entry, const std::size_t length) {
    return Amd64OnlyMatch{entry.Name, length, Amd64OnlyLowering::Unsupported, {}, {}, 0};
}

const Entry& _sse4aEntry(const Sse4aOperands& operands) {
    if (operands.RegisterForm)
        return operands.Insertq ? kInsertqRegisterForm : kExtrqRegisterForm;
    return operands.Insertq ? kInsertq : kExtrq;
}

const Entry& _reciprocalEntry(const ReciprocalOperands& operands) {
    return operands.Operation == ReciprocalOperation::ReciprocalSquareRoot ? kVrsqrtps : kVrcpps;
}

const Entry& _sha256Entry(const Sha256Operands& operands) {
    switch (operands.Operation) {
    case Sha256Operation::Rnds2:
        return kSha256rnds2;
    case Sha256Operation::Msg1:
        return kSha256msg1;
    case Sha256Operation::Msg2:
        return kSha256msg2;
    }
    return kSha256rnds2;
}

const Entry& _sha1Entry(const Sha1Operands& operands) {
    switch (operands.Operation) {
    case Sha1Operation::Rnds4:
        return kSha1rnds4;
    case Sha1Operation::Nexte:
        return kSha1nexte;
    case Sha1Operation::Msg1:
        return kSha1msg1;
    case Sha1Operation::Msg2:
        return kSha1msg2;
    }
    return kSha1rnds4;
}

Amd64OnlyMatch _inPlace(const Entry& entry, const std::size_t length, std::vector<std::uint8_t> replacement) {
    while (replacement.size() < length) {
        const auto& nop = kNops[std::min<std::size_t>(length - replacement.size(), std::size(kNops)) - 1];
        replacement.insert(replacement.end(), nop.Bytes, nop.Bytes + nop.Size);
    }
    return Amd64OnlyMatch{entry.Name, length, Amd64OnlyLowering::InPlace, std::move(replacement), {}, 0};
}

bool _isClzeroOpcode(const DecodedInstruction& instr) {
    const auto pos = instr.OpcodeOffset();
    return pos + 2 < instr.Length && instr.Data[pos] == X64OpcodeConstants::TwoByteOpcodeEscape && instr.Data[pos + 1] == X64OpcodeConstants::TwoByteGrp7 && instr.Data[pos + 2] == 0xFC;
}

bool _validWait(const DecodedInstruction& instr) {
    const auto opcode = instr.Data + instr.OpcodeOffset();
    return instr.Length <= kMaxInstructionLength && std::find(instr.Data, opcode, X64OpcodeConstants::PrefixLock) == opcode;
}

class Amd64OnlyInstructionMatcher : public IAmd64OnlyInstructionMatcher {
public:
    [[nodiscard]] std::optional<Amd64OnlyMatch> Match(
        const std::uint8_t* data,
        std::size_t length,
        std::span<const std::uint8_t> trailing = {}
    ) const override;

    [[nodiscard]] std::optional<Amd64OnlyMatch> MatchSequence(
        std::span<const std::span<const std::uint8_t>> instructions,
        std::span<const std::uint8_t> trailing
    ) const override;

private:
    [[nodiscard]] std::optional<Amd64OnlyMatch> _matchMixedSequence(std::span<const std::span<const std::uint8_t>> instructions, std::span<const std::uint8_t> trailing) const;
    Sse4aLowering _lowering;
    SystemInstructionLowering _systemLowering;
    ShaNiLowering _shaLowering;

    [[nodiscard]] Amd64OnlyMatch _matchShaNi(const DecodedInstruction& instr, std::span<const std::uint8_t> trailing) const;
    [[nodiscard]] static const Entry& _shaEntry(const ShaNiOperands& operands);

    [[nodiscard]] Amd64OnlyMatch _matchSystem(const DecodedInstruction& instr, const Entry& entry, SystemInstruction instruction, std::span<const std::uint8_t> trailing) const;
    [[nodiscard]] static std::optional<SystemInstruction> _systemInstruction(const DecodedInstruction& instr);

    [[nodiscard]] Amd64OnlyMatch _matchMovnts(const DecodedInstruction& instr, const Entry& entry) const;
    [[nodiscard]] Amd64OnlyMatch _matchSse4a(const DecodedInstruction& instr, const Entry& entry, const Entry& registerFormEntry, std::span<const std::uint8_t> trailing) const;
    Sha256Lowering _sha256Lowering;
    Sha1Lowering _sha1Lowering;
    ClzeroLowering _clzeroLowering;
    ReciprocalLowering _reciprocalLowering;

    [[nodiscard]] Amd64OnlyMatch _matchSha256(const DecodedInstruction& instr, std::span<const std::uint8_t> trailing) const;
    [[nodiscard]] Amd64OnlyMatch _matchSha1(const DecodedInstruction& instr, std::span<const std::uint8_t> trailing) const;
    [[nodiscard]] Amd64OnlyMatch _matchClzero(const DecodedInstruction& instr, std::span<const std::uint8_t> trailing) const;
};

Amd64OnlyMatch Amd64OnlyInstructionMatcher::_matchMovnts(const DecodedInstruction& instr, const Entry& entry) const {
    using namespace X64OpcodeConstants;
    const auto opcodeOffset = instr.OpcodeOffset();
    if (opcodeOffset + 2 >= instr.Length)
        throw CodegenException("MOVNTSS/MOVNTSD truncated before its ModRM byte");
    const auto modrm = instr.Data[opcodeOffset + 2];
    if (((modrm >> ModRmModShift) & ModRmModMask) == ModRmModRegister)
        throw CodegenException("MOVNTSS/MOVNTSD with a register operand");
    std::vector<std::uint8_t> replacement(instr.Data, instr.Data + instr.Length);
    replacement[opcodeOffset + 1] = kMovsStoreOpcode;
    return Amd64OnlyMatch{entry.Name, instr.Length, Amd64OnlyLowering::InPlace, std::move(replacement), {}, 0};
}

const Entry& Amd64OnlyInstructionMatcher::_shaEntry(const ShaNiOperands& operands) {
    if (operands.ThreeByte3A)
        return kSha1Rnds4;
    switch (operands.Opcode) {
    case Codegen::kShaOpSha1Nexte:
        return kSha1Nexte;
    case Codegen::kShaOpSha1Msg1:
        return kSha1Msg1;
    case Codegen::kShaOpSha1Msg2:
        return kSha1Msg2;
    case Codegen::kShaOpSha256Rnds2:
        return kSha256Rnds2;
    case Codegen::kShaOpSha256Msg1:
        return kSha256Msg1;
    default:
        return kSha256Msg2;
    }
}

Amd64OnlyMatch Amd64OnlyInstructionMatcher::_matchShaNi(const DecodedInstruction& instr, const std::span<const std::uint8_t> trailing) const {
    const auto operands = DecodeShaNi(instr.Data, instr.Length);
    const auto& entry = _shaEntry(operands);
    if (!_shaLowering.CanLower(operands))
        return _unsupported(entry, instr.Length);
    auto body = _shaLowering.LowerOutOfLine(std::span<const ShaNiOperands>(&operands, 1), trailing);
    return Amd64OnlyMatch{entry.Name, instr.Length, Amd64OnlyLowering::Trampoline, {}, std::move(body.Bytes), body.ReturnBranchOffset, body.TrailingOffset, std::move(body.RipFixups)};
}

std::optional<SystemInstruction> Amd64OnlyInstructionMatcher::_systemInstruction(const DecodedInstruction& instr) {
    if (instr.IsMonitorx())
        return SystemInstruction::Monitorx;
    if (instr.IsMwaitx())
        return SystemInstruction::Mwaitx;
    if (instr.IsClzero()) {
        const auto operands = DecodeClzero(instr.Data, instr.Length);
        return operands.AddressSize32 ? SystemInstruction::Clzero32 : SystemInstruction::Clzero;
    }
    if (instr.IsMcommit())
        return SystemInstruction::Mcommit;
    return std::nullopt;
}

Amd64OnlyMatch Amd64OnlyInstructionMatcher::_matchSystem(const DecodedInstruction& instr, const Entry& entry, const SystemInstruction instruction, const std::span<const std::uint8_t> trailing) const {
    if (instruction != SystemInstruction::Clzero && instruction != SystemInstruction::Clzero32 && trailing.empty())
        return Amd64OnlyMatch{entry.Name, instr.Length, Amd64OnlyLowering::InPlace, _systemLowering.LowerInPlace(instruction, instr.Length), {}, 0};
    auto body = _systemLowering.LowerOutOfLine(std::span<const SystemInstruction>(&instruction, 1), trailing);
    return Amd64OnlyMatch{entry.Name, instr.Length, Amd64OnlyLowering::Trampoline, {}, std::move(body.Bytes), body.ReturnBranchOffset, body.TrailingOffset, std::move(body.RipFixups)};
}

Amd64OnlyMatch Amd64OnlyInstructionMatcher::_matchSse4a(const DecodedInstruction& instr, const Entry& entry, const Entry& registerFormEntry, std::span<const std::uint8_t> trailing) const {
    const auto operands = DecodeSse4a(instr.Data, instr.Length);
    if (!operands.RegisterForm && trailing.empty()) {
        if (auto inPlace = _lowering.LowerInPlace(operands, instr.Length))
            return Amd64OnlyMatch{entry.Name, instr.Length, Amd64OnlyLowering::InPlace, std::move(*inPlace), {}, 0};
    }
    auto body = _lowering.LowerOutOfLine(operands, trailing);
    const auto& name = operands.RegisterForm ? registerFormEntry.Name : entry.Name;
    return Amd64OnlyMatch{name, instr.Length, Amd64OnlyLowering::Trampoline, {}, std::move(body.Bytes), body.ReturnBranchOffset, body.TrailingOffset, std::move(body.RipFixups)};
}

Amd64OnlyMatch Amd64OnlyInstructionMatcher::_matchSha256(const DecodedInstruction& instr, std::span<const std::uint8_t> trailing) const {
    const auto operands = DecodeSha256(instr.Data, instr.Length);
    auto body = _sha256Lowering.LowerOutOfLine(operands, trailing);
    return Amd64OnlyMatch{_sha256Entry(operands).Name, instr.Length, Amd64OnlyLowering::Trampoline, {}, std::move(body.Bytes), body.ReturnBranchOffset};
}

Amd64OnlyMatch Amd64OnlyInstructionMatcher::_matchSha1(const DecodedInstruction& instr, std::span<const std::uint8_t> trailing) const {
    const auto operands = DecodeSha1(instr.Data, instr.Length);
    auto body = _sha1Lowering.LowerOutOfLine(operands, trailing);
    return Amd64OnlyMatch{_sha1Entry(operands).Name, instr.Length, Amd64OnlyLowering::Trampoline, {}, std::move(body.Bytes), body.ReturnBranchOffset};
}

Amd64OnlyMatch Amd64OnlyInstructionMatcher::_matchClzero(const DecodedInstruction& instr, std::span<const std::uint8_t> trailing) const {
    auto body = _clzeroLowering.LowerOutOfLine(DecodeClzero(instr.Data, instr.Length), trailing);
    return Amd64OnlyMatch{kClzero.Name, instr.Length, Amd64OnlyLowering::Trampoline, {}, std::move(body.Bytes), body.ReturnBranchOffset};
}

std::optional<Amd64OnlyMatch> Amd64OnlyInstructionMatcher::MatchSequence(
    std::span<const std::span<const std::uint8_t>> instructions,
    std::span<const std::uint8_t> trailing
) const {
    if (instructions.empty())
        return std::nullopt;
    std::vector<ShaNiOperands> shaSequence;
    for (const auto& instruction : instructions) {
        const DecodedInstruction instr{instruction.data(), instruction.size()};
        if (!instr.IsShaNi()) {
            shaSequence.clear();
            break;
        }
        const auto operands = DecodeShaNi(instr.Data, instr.Length);
        if (!_shaLowering.CanLower(operands))
            return std::nullopt;
        shaSequence.push_back(operands);
    }
    if (!shaSequence.empty()) {
        auto shaBody = _shaLowering.LowerOutOfLine(shaSequence, trailing);
        return Amd64OnlyMatch{_shaEntry(shaSequence.front()).Name, instructions.front().size(), Amd64OnlyLowering::Trampoline, {}, std::move(shaBody.Bytes), shaBody.ReturnBranchOffset, shaBody.TrailingOffset, std::move(shaBody.RipFixups)};
    }
    std::vector<SystemInstruction> systemSequence;
    for (const auto& instruction : instructions) {
        const DecodedInstruction instr{instruction.data(), instruction.size()};
        const auto system = _systemInstruction(instr);
        if (!system.has_value()) {
            systemSequence.clear();
            break;
        }
        systemSequence.push_back(*system);
    }
    if (!systemSequence.empty()) {
        const auto& entry = (systemSequence.front() == SystemInstruction::Clzero || systemSequence.front() == SystemInstruction::Clzero32) ? kClzero : (systemSequence.front() == SystemInstruction::Monitorx ? kMonitorx : (systemSequence.front() == SystemInstruction::Mwaitx ? kMwaitx : kMcommit));
        auto systemBody = _systemLowering.LowerOutOfLine(systemSequence, trailing);
        return Amd64OnlyMatch{entry.Name, instructions.front().size(), Amd64OnlyLowering::Trampoline, {}, std::move(systemBody.Bytes), systemBody.ReturnBranchOffset, systemBody.TrailingOffset, std::move(systemBody.RipFixups)};
    }
    std::vector<Sse4aOperands> sequence;
    for (const auto& instruction : instructions) {
        const DecodedInstruction instr{instruction.data(), instruction.size()};
        if (!instr.IsExtrq() && !instr.IsInsertq())
            return _matchMixedSequence(instructions, trailing);
        sequence.push_back(DecodeSse4a(instr.Data, instr.Length));
    }
    const auto& first = sequence.front();
    const auto& name = first.RegisterForm ? (first.Insertq ? kInsertqRegisterForm.Name : kExtrqRegisterForm.Name) : (first.Insertq ? kInsertq.Name : kExtrq.Name);
    auto body = _lowering.LowerOutOfLine(std::span<const Sse4aOperands>(sequence), trailing);
    return Amd64OnlyMatch{name, instructions.front().size(), Amd64OnlyLowering::Trampoline, {}, std::move(body.Bytes), body.ReturnBranchOffset, body.TrailingOffset, std::move(body.RipFixups)};
}

std::optional<Amd64OnlyMatch> Amd64OnlyInstructionMatcher::_matchMixedSequence(
    std::span<const std::span<const std::uint8_t>> instructions,
    std::span<const std::uint8_t> trailing
) const {
    if (instructions.empty())
        return std::nullopt;
    StubBodyBuilder body;
    const char* name = nullptr;
    for (const auto& instruction : instructions) {
        const DecodedInstruction instr{instruction.data(), instruction.size()};
        if (instr.IsExtrq() || instr.IsInsertq()) {
            const auto operands = DecodeSse4a(instr.Data, instr.Length);
            if (name == nullptr)
                name = _sse4aEntry(operands).Name;
            _lowering.EmitOutOfLine(body, operands);
        } else if (instr.IsSha256()) {
            const auto operands = DecodeSha256(instr.Data, instr.Length);
            if (name == nullptr)
                name = _sha256Entry(operands).Name;
            _sha256Lowering.EmitOutOfLine(body, operands);
        } else if (instr.IsSha1()) {
            const auto operands = DecodeSha1(instr.Data, instr.Length);
            if (name == nullptr)
                name = _sha1Entry(operands).Name;
            _sha1Lowering.EmitOutOfLine(body, operands);
        } else if (instr.IsClzero()) {
            const auto operands = DecodeClzero(instr.Data, instr.Length);
            if (name == nullptr)
                name = kClzero.Name;
            _clzeroLowering.EmitOutOfLine(body, operands);
        } else if (const auto reciprocal = DecodeVexReciprocal(instr.Data, instr.Length)) {
            if (name == nullptr)
                name = _reciprocalEntry(*reciprocal).Name;
            _reciprocalLowering.EmitOutOfLine(body, *reciprocal);
        } else {
            return std::nullopt;
        }
    }
    body.Raw(trailing);
    auto lowered = body.Finish();
    const bool optional = DecodeVexReciprocal(instructions.front().data(), instructions.front().size()).has_value();
    return Amd64OnlyMatch{name, instructions.front().size(), Amd64OnlyLowering::Trampoline, {}, std::move(lowered.Bytes), lowered.ReturnBranchOffset, lowered.TrailingOffset, std::move(lowered.RipFixups), optional};
}

std::optional<Amd64OnlyMatch> Amd64OnlyInstructionMatcher::Match(
    const std::uint8_t* data,
    std::size_t length,
    std::span<const std::uint8_t> trailing
) const {
    const DecodedInstruction instr{data, length};

    if (instr.IsShaNi())
        return _matchShaNi(instr, trailing);

    if (instr.IsMovntss())
        return _matchMovnts(instr, kMovntss);

    if (instr.IsMovntsd())
        return _matchMovnts(instr, kMovntsd);

    if (instr.IsExtrq())
        return _matchSse4a(instr, kExtrq, kExtrqRegisterForm, trailing);

    if (instr.IsInsertq())
        return _matchSse4a(instr, kInsertq, kInsertqRegisterForm, trailing);

    if (instr.IsMonitorx())
        return _matchSystem(instr, kMonitorx, SystemInstruction::Monitorx, trailing);

    if (instr.IsMwaitx())
        return _matchSystem(instr, kMwaitx, SystemInstruction::Mwaitx, trailing);

    if (const auto reciprocal = DecodeVexReciprocal(data, length)) {
        auto body = _reciprocalLowering.LowerOutOfLine(*reciprocal, trailing);
        return Amd64OnlyMatch{_reciprocalEntry(*reciprocal).Name, length, Amd64OnlyLowering::Trampoline, {}, std::move(body.Bytes), body.ReturnBranchOffset, body.TrailingOffset, std::move(body.RipFixups), true};
    }

    if (instr.IsClzero())
        return _matchSystem(instr, kClzero, *_systemInstruction(instr), trailing);

    if (instr.IsRdpru())
        return _unsupported(kRdpru, length);

    if (instr.IsMcommit())
        return _matchSystem(instr, kMcommit, SystemInstruction::Mcommit, trailing);

    return std::nullopt;
}

}

std::unique_ptr<IAmd64OnlyInstructionMatcher> MakeAmd64OnlyInstructionMatcher() {
    return std::make_unique<Amd64OnlyInstructionMatcher>();
}

}
