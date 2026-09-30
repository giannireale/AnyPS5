#include <codegen/IAmd64OnlyConverter.hpp>
#include <codegen/IInstructionScanner.hpp>
#include <codegen/CodegenException.hpp>
#include <cstring>
#include <codegen/x86/Amd64OnlySubstitutionTable.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <codegen/x86/X64InstructionRewriter.hpp>
#include <codegen/x86/IAmd64OnlyInstructionMatcher.hpp>
#include <algorithm>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace Codegen {

namespace {

std::vector<std::uint8_t> _widenBranch(const DecodedInstructionInfo& info, const Domain::FileByteOffset offset) {
    std::vector<std::uint8_t> widened;
    if (info.FlowKind == ControlFlowKind::UnconditionalJump) {
        widened.push_back(0xE9);
    } else if (info.FlowKind == ControlFlowKind::Call) {
        widened.push_back(0xE8);
    } else if (info.FlowKind == ControlFlowKind::ConditionalBranch && !info.IsTwoByteOpcode && info.Opcode >= 0xE0 && info.Opcode <= 0xE3) {
        widened.push_back(info.Opcode);
        widened.push_back(0x02);
        widened.push_back(0xEB);
        widened.push_back(0x05);
        widened.push_back(0xE9);
    } else if (info.FlowKind == ControlFlowKind::ConditionalBranch) {
        const bool shortForm = !info.IsTwoByteOpcode;
        if (shortForm && (info.Opcode < 0x70 || info.Opcode > 0x7F))
            throw CodegenException("Relocated conditional branch has no 32 bit form", offset);
        if (!shortForm && (info.Opcode < 0x80 || info.Opcode > 0x8F))
            throw CodegenException("Relocated conditional branch has no 32 bit form", offset);
        widened.push_back(0x0F);
        widened.push_back(static_cast<std::uint8_t>(0x80 | (info.Opcode & 0x0F)));
    } else {
        throw CodegenException("AMD-only instruction too short for a jump is followed by an instruction that cannot move", offset);
    }
    widened.insert(widened.end(), 4, 0);
    return widened;
}


template<typename TOperation>
auto _atFileOffset(const Domain::FileByteOffset base, const TOperation& operation) {
    try {
        return operation();
    } catch (const CodegenException& e) {
        throw CodegenException(e.what(), base + e.FailureOffset);
    }
}

class Amd64OnlyConverter : public IAmd64OnlyConverter {
public:
    [[nodiscard]] ConvertResult Convert(
        std::vector<std::uint8_t> fileBytes,
        const std::vector<Domain::ProgramHeader>& codeSegments
    ) const override;

private:
    struct Pending {
        std::size_t Index;
        InstructionMatch Instruction;
        Amd64OnlyMatch Substitution;
    };

    X64InstructionRewriter _rewriter;
    std::unique_ptr<IAmd64OnlyInstructionMatcher> _matcher = MakeAmd64OnlyInstructionMatcher();
    std::unique_ptr<IInstructionScanner> _scanner = MakeInstructionScanner();

    void _convertSegment(
        std::vector<std::uint8_t>& fileBytes,
        const Domain::ProgramHeader& ph,
        ConvertResult& result
    ) const;

    [[nodiscard]] static std::map<std::uint64_t, std::uint64_t> _collectBranchTargets(
        const std::vector<std::uint8_t>& seg,
        const std::vector<InstructionMatch>& matches,
        const Domain::ProgramHeader& ph
    );
};

std::map<std::uint64_t, std::uint64_t> Amd64OnlyConverter::_collectBranchTargets(
    const std::vector<std::uint8_t>& seg,
    const std::vector<InstructionMatch>& matches,
    const Domain::ProgramHeader& ph
) {
    const X64InstructionDecoder decoder;
    std::map<std::uint64_t, std::uint64_t> targets;
    for (const auto& match : matches) {
        const auto info = decoder.DecodeInstruction(seg.data() + match.Offset, match.Length);
        if (!info.HasBranchTarget || info.HasRipRelativeDisp)
            continue;
        const auto target = static_cast<std::int64_t>(ph.MappedAddress + match.Offset + info.Length) + info.BranchDisp;
        if (target >= 0)
            targets.emplace(static_cast<std::uint64_t>(target), ph.MappedAddress + match.Offset);
    }
    return targets;
}

void Amd64OnlyConverter::_convertSegment(
    std::vector<std::uint8_t>& fileBytes,
    const Domain::ProgramHeader& ph,
    ConvertResult& result
) const {
    const auto segOffset = static_cast<std::size_t>(ph.Offset);
    const auto segSize = static_cast<std::size_t>(ph.FileSize);
    if (segOffset > fileBytes.size() || segSize > fileBytes.size() - segOffset)
        throw CodegenException("Code segment exceeds the file", ph.Offset);

    std::vector<std::uint8_t> seg(
        fileBytes.begin() + static_cast<std::ptrdiff_t>(segOffset),
        fileBytes.begin() + static_cast<std::ptrdiff_t>(segOffset + segSize)
    );

    const auto matches = _atFileOffset(ph.Offset, [&] { return _scanner->ScanCodeSection(seg, 0, seg.size()); });

    std::vector<Pending> pending;
    for (std::size_t index = 0; index < matches.size(); ++index) {
        const auto& match = matches[index];
        auto substitution = _atFileOffset(ph.Offset + match.Offset, [&] { return _matcher->Match(seg.data() + match.Offset, match.Length); });
        if (substitution.has_value())
            pending.push_back({index, match, std::move(*substitution)});
    }
    if (pending.empty())
        return;

    const bool needsBranchTargets = std::any_of(pending.begin(), pending.end(), [](const Pending& item) {
        return item.Substitution.Lowering == Amd64OnlyLowering::Trampoline;
    });
    const auto branchTargets = needsBranchTargets ? _collectBranchTargets(seg, matches, ph) : std::map<std::uint64_t, std::uint64_t>{};

    std::set<std::size_t> consumed;
    for (const auto& item : pending) {
        if (consumed.contains(item.Index))
            continue;
        const auto& match = item.Instruction;
        const auto& substitution = item.Substitution;
        const auto fileOffset = static_cast<Domain::FileByteOffset>(ph.Offset + match.Offset);
        const auto address = static_cast<Domain::VirtualAddress>(ph.MappedAddress + match.Offset);
        std::size_t replacementLength = 0;

        switch (substitution.Lowering) {
        case Amd64OnlyLowering::InPlace: {
            if (substitution.ReplacementBytes.size() != match.Length)
                throw CodegenException("Intel substitution changes the instruction length", fileOffset);
            seg = _atFileOffset(ph.Offset, [&] { return _rewriter.Rewrite(seg, {match.Offset, substitution.ReplacementBytes}).Bytes; });
            replacementLength = substitution.ReplacementBytes.size();
            ++result.ReplacedCount;
            break;
        }
        case Amd64OnlyLowering::Trampoline: {
            std::size_t siteLength = match.Length;
            auto stub = substitution;
            std::vector<TrampolineFixup> fixups;
            if (siteLength < Amd64OnlySubstitutionTable::kJmpRel32.Size) {
                const X64InstructionDecoder decoder;
                std::vector<std::span<const std::uint8_t>> sequence{std::span<const std::uint8_t>(seg.data() + match.Offset, match.Length)};
                std::vector<std::uint8_t> trailingBytes;
                for (auto next = item.Index + 1; siteLength < Amd64OnlySubstitutionTable::kJmpRel32.Size; ++next) {
                    if (next >= matches.size() || matches[next].Offset != match.Offset + siteLength)
                        throw CodegenException("AMD-only instruction too short for a jump is at the end of the segment, so there is nothing to absorb into the stub", fileOffset);
                    const auto& following = matches[next];
                    const std::span<const std::uint8_t> bytes(seg.data() + following.Offset, following.Length);
                    const auto info = decoder.DecodeInstruction(bytes.data(), bytes.size());
                    const bool amdOnly = _matcher->Match(bytes.data(), bytes.size()).has_value();
                    const auto followingAddress = static_cast<Domain::VirtualAddress>(ph.MappedAddress + following.Offset);
                    if (amdOnly && trailingBytes.empty()) {
                        sequence.push_back(bytes);
                        consumed.insert(next);
                    } else if (amdOnly) {
                        throw CodegenException("AMD-only instruction too short for a jump is followed by an instruction that cannot move", ph.Offset + following.Offset);
                    } else if (info.HasBranchTarget) {
                        const auto target = static_cast<Domain::VirtualAddress>(followingAddress + following.Length + info.BranchDisp);
                        const auto widened = _widenBranch(info, ph.Offset + following.Offset);
                        trailingBytes.insert(trailingBytes.end(), widened.begin(), widened.end());
                        fixups.push_back({trailingBytes.size() - 4, target});
                    } else if (info.FlowKind == ControlFlowKind::Sequential || info.FlowKind == ControlFlowKind::Return ||
                               info.FlowKind == ControlFlowKind::Trap || info.FlowKind == ControlFlowKind::IndirectJump ||
                               info.FlowKind == ControlFlowKind::IndirectCall) {
                        if (info.HasRipRelativeDisp) {
                            if (info.RipRelativeDispOffset + 4 > following.Length)
                                throw CodegenException("Relocated instruction has a truncated RIP-relative displacement", ph.Offset + following.Offset);
                            std::int32_t displacement = 0;
                            std::memcpy(&displacement, bytes.data() + info.RipRelativeDispOffset, sizeof(displacement));
                            fixups.push_back({trailingBytes.size() + info.RipRelativeDispOffset, static_cast<Domain::VirtualAddress>(followingAddress + following.Length + displacement)});
                        }
                        trailingBytes.insert(trailingBytes.end(), bytes.begin(), bytes.end());
                    } else {
                        throw CodegenException("AMD-only instruction too short for a jump is followed by an instruction that cannot move", ph.Offset + following.Offset);
                    }
                    siteLength += following.Length;
                }
                const std::span<const std::uint8_t> trailing(trailingBytes);
                auto relocated = _atFileOffset(fileOffset, [&] { return _matcher->MatchSequence(sequence, trailing); });
                if (!relocated.has_value() || relocated->Lowering != Amd64OnlyLowering::Trampoline)
                    throw CodegenException("AMD-only instruction sequence has no out-of-line lowering", fileOffset);
                stub = std::move(*relocated);
                for (auto& fixup : fixups)
                    fixup.BodyOffset += stub.TrailingOffset;
            }
            for (const auto& pending : stub.RipFixups)
                fixups.push_back({pending.BodyOffset, static_cast<Domain::VirtualAddress>(address + pending.InstructionEnd + pending.OriginalDisplacement)});
            const auto hit = branchTargets.upper_bound(address);
            if (hit != branchTargets.end() && hit->first < address + siteLength) {
                std::ostringstream message;
                message << "Branch at 0x" << std::hex << hit->second << " enters the AMD-only site at 0x" << address
                        << " (target 0x" << hit->first << "), so the site cannot be replaced by a jump";
                throw CodegenException(message.str(), ph.Offset + (hit->first - ph.MappedAddress));
            }
            const auto begin = seg.begin() + static_cast<std::ptrdiff_t>(match.Offset);
            result.Trampolines.push_back({
                fileOffset,
                address,
                siteLength,
                std::vector<std::uint8_t>(begin, begin + static_cast<std::ptrdiff_t>(siteLength)),
                stub.StubBody,
                stub.ReturnBranchOffset,
                std::move(fixups)
            });
            replacementLength = stub.StubBody.size();
            break;
        }
        case Amd64OnlyLowering::Unsupported:
            throw CodegenException("AMD-only instruction without Intel lowering: " + substitution.InstructionName +
                (substitution.InstructionName == "RDPRU" ? " reads AMD performance counters that no Intel processor exposes, so no lowering can preserve its result" : ""), fileOffset);
        }

        result.Reports.push_back({substitution.InstructionName, fileOffset, match.Length, replacementLength, substitution.Lowering});
    }

    if (seg.size() != segSize)
        throw CodegenException("Code segment size changed during Intel conversion", ph.Offset);
    std::copy(seg.begin(), seg.end(), fileBytes.begin() + static_cast<std::ptrdiff_t>(segOffset));
}

ConvertResult Amd64OnlyConverter::Convert(
    std::vector<std::uint8_t> fileBytes,
    const std::vector<Domain::ProgramHeader>& codeSegments
) const {
    ConvertResult result{{}, 0, {}, {}};
    for (const auto& ph : codeSegments)
        _convertSegment(fileBytes, ph, result);
    result.Bytes = std::move(fileBytes);
    return result;
}

}

std::unique_ptr<IAmd64OnlyConverter> MakeAmd64OnlyConverter() {
    return std::make_unique<Amd64OnlyConverter>();
}

}
