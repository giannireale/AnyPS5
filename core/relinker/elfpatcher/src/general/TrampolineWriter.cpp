#include <elfpatcher/general/TrampolineWriter.hpp>
#include <codegen/x86/Amd64OnlySubstitutionTable.hpp>
#include <domain/Types.hpp>
#include <io/BufferUtils.hpp>
#include <algorithm>
#include <limits>

namespace Elfpatcher {

void AppendTrampoline(std::vector<std::uint8_t>& bytes, const Codegen::TrampolineSite& site, const std::uint64_t imageEnd, const std::function<std::uint64_t(std::uint64_t)>& addressOfOffset) {
    using namespace Codegen::Amd64OnlySubstitutionTable;
    if (site.Length < kJmpRel32.Size || site.OriginalBytes.size() != site.Length || site.Body.size() < kJmpRel32.Size || site.ReturnBranchOffset > site.Body.size() - kJmpRel32.Size || site.Body[site.ReturnBranchOffset] != kJmpRel32.Bytes[0])
        throw Domain::RelinkerException("Invalid AMD-only trampoline site", site.Offset);
    if (site.Offset > imageEnd || site.Length > imageEnd - site.Offset)
        throw Domain::RelinkerException("AMD-only instruction is outside the original image", site.Offset);
    if (!std::equal(site.OriginalBytes.begin(), site.OriginalBytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(site.Offset)))
        throw Domain::RelinkerException("AMD-only site bytes changed before patching", site.Offset);

    bytes.resize(Io::AlignUp(bytes.size(), kStubAlignment), kTrapFill);
    const auto bodyOffset = static_cast<std::uint64_t>(bytes.size());
    const auto bodyAddress = addressOfOffset(bodyOffset);
    bytes.insert(bytes.end(), site.Body.begin(), site.Body.end());

    const auto inRange = [](const std::int64_t displacement) {
        return displacement >= std::numeric_limits<std::int32_t>::min() && displacement <= std::numeric_limits<std::int32_t>::max();
    };
    const auto returnDisplacement = static_cast<std::int64_t>(site.Address + site.Length) - static_cast<std::int64_t>(bodyAddress + site.ReturnBranchOffset + kJmpRel32.Size);
    const auto jumpDisplacement = static_cast<std::int64_t>(bodyAddress) - static_cast<std::int64_t>(site.Address + kJmpRel32.Size);
    if (!inRange(returnDisplacement) || !inRange(jumpDisplacement))
        throw Domain::RelinkerException("AMD-only stub exceeds rel32 range", site.Offset);
    Io::WriteU32(bytes, static_cast<std::size_t>(bodyOffset + site.ReturnBranchOffset + 1), static_cast<std::uint32_t>(returnDisplacement));

    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(site.Offset), site.Length, kNop1.Bytes[0]);
    bytes[static_cast<std::size_t>(site.Offset)] = kJmpRel32.Bytes[0];
    Io::WriteU32(bytes, static_cast<std::size_t>(site.Offset + 1), static_cast<std::uint32_t>(jumpDisplacement));
}

}
