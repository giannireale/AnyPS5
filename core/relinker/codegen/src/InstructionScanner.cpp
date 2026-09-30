#include <codegen/IInstructionScanner.hpp>
#include <codegen/CodegenException.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <algorithm>
#include <array>
#include <memory>
#include <string>

namespace Codegen {

namespace {

constexpr std::size_t MaxInstructionLength = 15;
constexpr std::uint8_t PaddingOpcode = 0x90;

bool _isCutByTheEnd(const X64InstructionDecoder& decoder, const std::uint8_t* cursor, const std::size_t available) {
    std::array<std::uint8_t, MaxInstructionLength * 2> padded{};
    padded.fill(PaddingOpcode);
    std::copy(cursor, cursor + available, padded.begin());
    try {
        return decoder.Decode(padded.data(), padded.size()) > available;
    } catch (const CodegenException&) {
        return false;
    }
}

}

class InstructionScanner : public IInstructionScanner {
public:
    [[nodiscard]] std::vector<InstructionMatch> ScanCodeSection(
        const std::vector<std::uint8_t>& codeSection,
        Domain::FileByteOffset codeSectionOffset,
        Domain::FileByteOffset codeSectionSize
    ) const override;
};

std::vector<InstructionMatch> InstructionScanner::ScanCodeSection(
    const std::vector<std::uint8_t>& codeSection,
    const Domain::FileByteOffset codeSectionOffset,
    const Domain::FileByteOffset codeSectionSize
) const {
    const std::size_t limit = std::min(codeSection.size(), static_cast<std::size_t>(codeSectionSize));

    const X64InstructionDecoder decoder;
    std::vector<InstructionMatch> matches;
    std::size_t i = 0;

    while (i < limit) {
        const std::uint8_t* cursor = codeSection.data() + i;
        const std::size_t available = limit - i;

        std::size_t length = 0;
        try {
            length = decoder.Decode(cursor, available);
        } catch (const CodegenException& e) {
            if (available < MaxInstructionLength && _isCutByTheEnd(decoder, cursor, available))
                break;
            throw CodegenException(std::string("Cannot decode instruction: ") + e.what(), codeSectionOffset + i);
        }

        matches.push_back({codeSectionOffset + i, length});

        i += length;
    }

    return matches;
}

std::unique_ptr<IInstructionScanner> MakeInstructionScanner() {
    return std::make_unique<InstructionScanner>();
}

}
