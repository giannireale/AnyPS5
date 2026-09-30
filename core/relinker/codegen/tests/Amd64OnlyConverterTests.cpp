#include <codegen/IAmd64OnlyConverter.hpp>
#include <codegen/CodegenException.hpp>
#include <codegen/x86/IAmd64OnlyInstructionMatcher.hpp>
#include <codegen/x86/Sse4aLowering.hpp>
#include <codegen/x86/Sse4aOperands.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <elfpatcher/general/EntryStubBuilder.hpp>
#include <elfpatcher/general/ProgramHeaderLayoutBuilder.hpp>
#include <elfpatcher/general/SectionHeaderTableBuilder.hpp>
#include <elfpatcher/general/SegmentFilter.hpp>
#include <elfpatcher/linux/LinuxElfPatcher.hpp>
#include <io/ByteWriter.hpp>
#include <algorithm>
#include <cstring>
#include <cstdint>
#ifdef __linux__
#include <sys/mman.h>
#endif
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void requireFailure(const std::function<void()>& operation, const char* message) {
    try {
        operation();
    } catch (const Codegen::CodegenException&) {
        return;
    } catch (const Domain::RelinkerException&) {
        return;
    }
    throw std::runtime_error(message);
}

Domain::FileByteOffset failureOffset(const std::function<void()>& operation, const char* message) {
    try {
        operation();
    } catch (const Codegen::CodegenException& error) {
        return error.FailureOffset;
    }
    throw std::runtime_error(message);
}

template<typename TValue>
void write(Bytes& bytes, std::size_t offset, TValue value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) throw std::runtime_error("Test fixture write is out of bounds");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<typename TValue>
TValue read(const Bytes& bytes, std::size_t offset) {
    TValue value;
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) throw std::runtime_error("Test fixture read is out of bounds");
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

Bytes withReturn(Bytes body, const std::size_t returnBranchOffset) {
    body.at(returnBranchOffset) = 0xE9;
    for (std::size_t index = 1; index <= 4; ++index) body.at(returnBranchOffset + index) = 0;
    return body;
}

const Bytes kExtrqSite = {0x66, 0x0F, 0x78, 0xC3, 0x08, 0x28};
const Bytes kInsertqSelfSite = {0xF2, 0x0F, 0x78, 0xDB, 0x08, 0x08};
const Bytes kInsertqCrossSite = {0xF2, 0x0F, 0x78, 0xC8, 0x08, 0x00};
const Bytes kInsertqHighSite = {0xF2, 0x44, 0x0F, 0x78, 0xCC, 0x10, 0x10};
const Bytes kInsertqWordSite = {0xF2, 0x0F, 0x78, 0xDC, 0x10, 0x10};

const Bytes kExtrqBody = {
    0x66, 0x0F, 0x38, 0x00, 0x1D, 0x07, 0x00, 0x00, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC,
    0x05, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
const Bytes kInsertqSelfBody = {
    0x66, 0x0F, 0x38, 0x00, 0x1D, 0x07, 0x00, 0x00, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC,
    0x00, 0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
const Bytes kInsertqCrossBody = {
    0x66, 0x0F, 0x6C, 0xC8, 0x66, 0x0F, 0x38, 0x00, 0x0D, 0x13, 0x00, 0x00, 0x00, 0xE9, 0x00, 0x00,
    0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
const Bytes kInsertqHighBody = {
    0x66, 0x44, 0x0F, 0x6C, 0xCC, 0x66, 0x44, 0x0F, 0x38, 0x00, 0x0D, 0x11, 0x00, 0x00, 0x00, 0xE9,
    0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x00, 0x01, 0x08, 0x09, 0x04, 0x05, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
const Bytes kInsertqWordBody = {
    0x66, 0x0F, 0x6C, 0xDC, 0x66, 0x0F, 0x38, 0x00, 0x1D, 0x13, 0x00, 0x00, 0x00, 0xE9, 0x00, 0x00,
    0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x00, 0x01, 0x08, 0x09, 0x04, 0x05, 0x06, 0x07, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};

void decoderLengths() {
    const Codegen::X64InstructionDecoder decoder;
    const std::vector<Bytes> instructions = {
        kExtrqSite, kInsertqSelfSite, kInsertqCrossSite, kInsertqHighSite, kInsertqWordSite,
        {0x66, 0x0F, 0x79, 0xCA}, {0xF2, 0x0F, 0x79, 0xCA}, {0x66, 0x45, 0x0F, 0x79, 0xCA},
        {0xF3, 0x0F, 0xB8, 0xC0}, {0xCD, 0x41}, {0x0F, 0x0D, 0x08}, {0x0F, 0xC0, 0xC1}, {0x0F, 0xC3, 0x07},
        {0x66, 0x0F, 0xC4, 0xC0, 0x01}, {0xC2, 0x08, 0x00}, {0xC8, 0x10, 0x00, 0x00}, {0xF3, 0x0F, 0x2B, 0x07},
        {0xF2, 0x44, 0x0F, 0x2B, 0x4C, 0x24, 0x10}, {0x0F, 0x01, 0xFA}, {0x0F, 0xB9, 0x00},
        {0x41, 0x0F, 0xBB, 0xF7}, {0x0F, 0xBB, 0x47, 0x08},
        {0x66, 0x48, 0x81, 0xC0, 0x11, 0x22, 0x33, 0x44},
        {0x66, 0x48, 0xC7, 0xC0, 0x11, 0x22, 0x33, 0x44},
        {0x66, 0x48, 0x69, 0xC0, 0x11, 0x22, 0x33, 0x44},
        {0x66, 0x48, 0x05, 0x11, 0x22, 0x33, 0x44},
        {0x66, 0x48, 0xA9, 0x11, 0x22, 0x33, 0x44},
        {0x66, 0x48, 0xF7, 0xC0, 0x11, 0x22, 0x33, 0x44},
        {0x66, 0x81, 0xC0, 0x11, 0x22}, {0x66, 0xF7, 0xC0, 0x11, 0x22},
        {0x66, 0x48, 0x68, 0x11, 0x22},
        {0x66, 0x48, 0xB8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}};
    Bytes padded;
    for (const auto& instruction : instructions) {
        padded = instruction;
        padded.insert(padded.end(), 8, 0x90);
        require(decoder.Decode(padded.data(), padded.size()) == instruction.size(), "AMD-only or repaired two-byte opcode was decoded with the wrong length");
    }
    requireFailure([&] { const Bytes bare = {0x0F, 0x78, 0xC3, 0x08, 0x28}; (void)decoder.Decode(bare.data(), bare.size()); }, "0F 78 without an SSE4a prefix was accepted");
    for (const std::uint8_t opcode : {0xA0, 0xA1, 0xA2, 0xA3}) {
        for (const Bytes prefix : {Bytes{}, Bytes{0x66}, Bytes{0x48}, Bytes{0x66, 0x48}, Bytes{0x67}, Bytes{0x66, 0x67, 0x48}}) {
            Bytes instruction = prefix;
            instruction.push_back(opcode);
            instruction.insert(instruction.end(), std::find(prefix.begin(), prefix.end(), 0x67) == prefix.end() ? 8 : 4, 0x90);
            require(decoder.Decode(instruction.data(), instruction.size()) == instruction.size(), "MOFFS address width was decoded incorrectly");
            requireFailure([&] { (void)decoder.Decode(instruction.data(), instruction.size() - 1); }, "Truncated MOFFS address was accepted");
        }
    }
}

void sse4aOperands() {
    const auto check = [](const Bytes& site, const bool insertq, const int dst, const int src, const int length, const int index) {
        const auto operands = Codegen::DecodeSse4a(site.data(), site.size());
        require(operands.Insertq == insertq && !operands.RegisterForm && operands.Destination == dst && operands.Source == src && operands.Length == length && operands.Index == index, "SSE4a operands were decoded incorrectly");
    };
    check(kExtrqSite, false, 3, 3, 8, 40);
    check(kInsertqSelfSite, true, 3, 3, 8, 8);
    check(kInsertqCrossSite, true, 1, 0, 8, 0);
    check(kInsertqHighSite, true, 9, 4, 16, 16);
    check(kInsertqWordSite, true, 3, 4, 16, 16);
    const Bytes fullField = {0xF2, 0x0F, 0x78, 0xC8, 0x00, 0x00};
    require(Codegen::DecodeSse4a(fullField.data(), fullField.size()).Length == 64, "Zero length does not mean 64");
    const Bytes registerForm = {0x66, 0x45, 0x0F, 0x79, 0xCA};
    const auto decoded = Codegen::DecodeSse4a(registerForm.data(), registerForm.size());
    require(decoded.RegisterForm && !decoded.Insertq && decoded.Destination == 9 && decoded.Source == 10, "Register form operands were decoded incorrectly");
    requireFailure([] { const Bytes bytes = {0x66, 0x0F, 0x78, 0xCB, 0x08, 0x28}; (void)Codegen::DecodeSse4a(bytes.data(), bytes.size()); }, "EXTRQ with a non-zero reg field was accepted");
    requireFailure([] { const Bytes bytes = {0xF2, 0x0F, 0x78, 0x1B, 0x08, 0x08}; (void)Codegen::DecodeSse4a(bytes.data(), bytes.size()); }, "SSE4a memory operand was accepted");
    requireFailure([] { const Bytes bytes = {0xF2, 0x0F, 0x78, 0xC8, 0x20, 0x30}; (void)Codegen::DecodeSse4a(bytes.data(), bytes.size()); }, "Field beyond bit 64 was accepted");
}

void matcherSubstitutions() {
    const auto matcher = Codegen::MakeAmd64OnlyInstructionMatcher();
    const auto match = [&](const Bytes& bytes) { return matcher->Match(bytes.data(), bytes.size()); };
    const auto movntss = match({0xF3, 0x0F, 0x2B, 0x07});
    require(movntss && movntss->Lowering == Codegen::Amd64OnlyLowering::InPlace && movntss->ReplacementBytes == Bytes{0xF3, 0x0F, 0x11, 0x07} && movntss->InstructionName == "MOVNTSS", "MOVNTSS was not rewritten to MOVSS");
    const auto movntsd = match({0xF2, 0x44, 0x0F, 0x2B, 0x4C, 0x24, 0x10});
    require(movntsd && movntsd->Lowering == Codegen::Amd64OnlyLowering::InPlace && movntsd->ReplacementBytes == Bytes{0xF2, 0x44, 0x0F, 0x11, 0x4C, 0x24, 0x10} && movntsd->InstructionName == "MOVNTSD", "MOVNTSD was not rewritten to MOVSD");
    requireFailure([&] { (void)match({0xF3, 0x0F, 0x2B, 0xC1}); }, "MOVNTSS with a register operand was accepted");
    const auto monitorx = match({0x0F, 0x01, 0xFA});
    require(monitorx && monitorx->Lowering == Codegen::Amd64OnlyLowering::InPlace && monitorx->ReplacementBytes == Bytes{0x0F, 0x1F, 0x00} && monitorx->InstructionName == "MONITORX", "MONITORX was not rewritten to a three byte NOP");
    const auto mwaitx = match({0x0F, 0x01, 0xFB});
    require(mwaitx && mwaitx->Lowering == Codegen::Amd64OnlyLowering::InPlace && mwaitx->ReplacementBytes == Bytes{0xF3, 0x90, 0x90} && mwaitx->InstructionName == "MWAITX", "MWAITX was not rewritten to PAUSE");
    const auto mcommit = match({0xF3, 0x0F, 0x01, 0xFA});
    require(mcommit && mcommit->Lowering == Codegen::Amd64OnlyLowering::InPlace && mcommit->ReplacementBytes == Bytes{0x0F, 0xAE, 0xF0, 0xF8} && mcommit->InstructionName == "MCOMMIT", "MCOMMIT was not rewritten to MFENCE and CLC");
    const auto clzero = match({0x0F, 0x01, 0xFC});
    require(clzero && clzero->Lowering == Codegen::Amd64OnlyLowering::Trampoline && clzero->InstructionName == "CLZERO", "CLZERO was not lowered through a stub");
    const auto sha1nexte = match({0x0F, 0x38, 0xC8, 0xD9});
    require(sha1nexte && sha1nexte->Lowering == Codegen::Amd64OnlyLowering::Trampoline && sha1nexte->InstructionName == "SHA1NEXTE", "SHA1NEXTE was not lowered through a stub");
    for (const std::uint8_t opcode : {0x0F, 0xC9, 0xCA, 0xCC, 0xCD}) {
        if (opcode == 0x0F) continue;
        const auto lowered = match({0x0F, 0x38, opcode, 0xD9});
        require(lowered && lowered->Lowering == Codegen::Amd64OnlyLowering::Trampoline, "SHA-NI message schedule opcode was not lowered through a stub");
    }
    const auto sha256rnds2 = match({0x0F, 0x38, 0xCB, 0xD9});
    require(sha256rnds2 && sha256rnds2->Lowering == Codegen::Amd64OnlyLowering::Trampoline && sha256rnds2->InstructionName == "SHA256RNDS2", "SHA256RNDS2 was not lowered through a stub");
    const auto sha1rnds4 = match({0x0F, 0x3A, 0xCC, 0xD9, 0x02});
    require(sha1rnds4 && sha1rnds4->Lowering == Codegen::Amd64OnlyLowering::Trampoline && sha1rnds4->InstructionName == "SHA1RNDS4", "SHA1RNDS4 was not lowered through a stub");
    const auto shaMemory = match({0x0F, 0x38, 0xC8, 0x18});
    require(shaMemory && shaMemory->Lowering == Codegen::Amd64OnlyLowering::Trampoline, "SHA1NEXTE with a memory operand was not lowered through a stub");
    const auto shaRip = match({0x0F, 0x38, 0xC8, 0x1D, 0x00, 0x00, 0x00, 0x00});
    require(shaRip && shaRip->Lowering == Codegen::Amd64OnlyLowering::Trampoline && shaRip->RipFixups.size() == 1, "SHA1NEXTE with a RIP-relative operand was not lowered with a relocation");
    require(shaRip->RipFixups[0].InstructionEnd == 8 && shaRip->RipFixups[0].OriginalDisplacement == 0, "The SHA-NI relocation does not describe the original operand");
    const auto rdpru = match({0x0F, 0x01, 0xFD});
    require(rdpru && rdpru->Lowering == Codegen::Amd64OnlyLowering::Unsupported && rdpru->InstructionName == "RDPRU", "RDPRU was not reported as unsupported");
    const auto registerForm = match({0x66, 0x0F, 0x79, 0xCA});
    require(registerForm && registerForm->Lowering == Codegen::Amd64OnlyLowering::Trampoline && registerForm->InstructionName == "EXTRQ register form", "EXTRQ register form was not lowered through a stub");
    const auto insertqRegisterForm = match({0xF2, 0x0F, 0x79, 0xCA});
    require(insertqRegisterForm && insertqRegisterForm->Lowering == Codegen::Amd64OnlyLowering::Trampoline && insertqRegisterForm->InstructionName == "INSERTQ register form", "INSERTQ register form was not lowered through a stub");
    require(!match({0x66, 0x0F, 0x2B, 0x07}) && !match({0x0F, 0x2B, 0x07}) && !match({0x48, 0x8B, 0x05, 0, 0, 0, 0}), "Ordinary instruction was matched");
    const auto stub = match(kInsertqHighSite);
    require(stub && stub->Lowering == Codegen::Amd64OnlyLowering::Trampoline && stub->StubBody == kInsertqHighBody && stub->ReturnBranchOffset == 15 && stub->InstructionName == "INSERTQ", "INSERTQ was not lowered through a stub");
    const auto shiftInPlace = match({0x66, 0x0F, 0x78, 0xC3, 0x18, 0x28});
    require(shiftInPlace && shiftInPlace->Lowering == Codegen::Amd64OnlyLowering::InPlace && shiftInPlace->ReplacementBytes == Bytes{0x66, 0x0F, 0x73, 0xD3, 0x28, 0x90}, "Top-aligned EXTRQ was not lowered in place");
}

void goldenBodies() {
    const Codegen::Sse4aLowering lowering;
    const auto outOfLine = [&](const Bytes& site, const Bytes& expected, const std::size_t returnBranchOffset) {
        const auto operands = Codegen::DecodeSse4a(site.data(), site.size());
        require(!lowering.LowerInPlace(operands, site.size()).has_value(), "Demon's Souls site unexpectedly qualified for an in-place lowering");
        const auto body = lowering.LowerOutOfLine(operands);
        require(body.ReturnBranchOffset == returnBranchOffset, "Stub return branch is at the wrong offset");
        require(body.Bytes == expected, "Stub body differs from the golden encoding");
    };
    outOfLine(kExtrqSite, kExtrqBody, 9);
    outOfLine(kInsertqSelfSite, kInsertqSelfBody, 9);
    outOfLine(kInsertqCrossSite, kInsertqCrossBody, 13);
    outOfLine(kInsertqHighSite, kInsertqHighBody, 15);
    outOfLine(kInsertqWordSite, kInsertqWordBody, 13);
    const auto inPlace = [&](const Bytes& site, const Bytes& expected) {
        const auto operands = Codegen::DecodeSse4a(site.data(), site.size());
        const auto sequence = lowering.LowerInPlace(operands, site.size());
        require(sequence.has_value() && *sequence == expected, "In-place lowering differs from the golden encoding");
    };
    inPlace({0xF2, 0x0F, 0x78, 0xC8, 0x00, 0x00}, {0xF3, 0x0F, 0x7E, 0xC8, 0x66, 0x90});
    inPlace({0x66, 0x0F, 0x78, 0xC3, 0x18, 0x28}, {0x66, 0x0F, 0x73, 0xD3, 0x28, 0x90});
    inPlace({0x66, 0x0F, 0x78, 0xC3, 0x08, 0x00}, {0x66, 0x0F, 0x38, 0x32, 0xDB, 0x90});
    inPlace({0xF2, 0x0F, 0x78, 0xDB, 0x08, 0x00}, {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00});
    inPlace({0xF2, 0x0F, 0x78, 0xC8, 0x20, 0x00}, {0x66, 0x0F, 0x3A, 0x0E, 0xC8, 0x03});
    inPlace({0xF2, 0x45, 0x0F, 0x78, 0xC8, 0x10, 0x00}, {0x66, 0x45, 0x0F, 0x3A, 0x0E, 0xC8, 0x01});
    const auto highRegisters = Codegen::DecodeSse4a(kInsertqHighSite.data(), kInsertqHighSite.size());
    const auto generic = lowering.LowerOutOfLine(Codegen::Sse4aOperands{true, false, 9, 4, 5, 3});
    require(generic.Bytes[0] == 0x48 && generic.Bytes.size() % 16 == 0 && generic.ReturnBranchOffset < generic.Bytes.size(), "Generic INSERTQ body does not start with the red-zone skip");
    (void)highRegisters;
    const auto registerFormBody = lowering.LowerOutOfLine(Codegen::Sse4aOperands{true, true, 1, 2, 0, 0});
    require(registerFormBody.Bytes[0] == 0x48 && registerFormBody.Bytes.size() % 16 == 0 && registerFormBody.ReturnBranchOffset < registerFormBody.Bytes.size(), "INSERTQ register form body does not start with the red-zone skip");
}

Bytes segmentFixture() {
    Bytes file(0x300, 0xCC);
    const Bytes text = {
        0xF3, 0x0F, 0xB8, 0xC0,
        0xCD, 0x41,
        0xEB, 0x07,
        0xF2, 0x44, 0x0F, 0x78, 0xCC, 0x10, 0x10,
        0xF3, 0x0F, 0x2B, 0x07,
        0xC3};
    std::copy(text.begin(), text.end(), file.begin() + 0x200);
    return file;
}

Domain::ProgramHeader segmentHeader(const std::uint64_t size) {
    return {1, 5, 0x200, 0x1000, 0, size, size, 16};
}

void converterSegment() {
    const auto converter = Codegen::MakeAmd64OnlyConverter();
    const auto file = segmentFixture();
    const auto result = converter->Convert(file, {segmentHeader(20)});
    require(result.ReplacedCount == 1 && result.Reports.size() == 2 && result.Trampolines.size() == 1, "Converter did not classify the segment's AMD-only instructions");
    const auto& site = result.Trampolines[0];
    require(site.Offset == 0x208 && site.Address == 0x1008 && site.Length == 7 && site.OriginalBytes == kInsertqHighSite && site.Body == kInsertqHighBody && site.ReturnBranchOffset == 15, "Trampoline site was recorded incorrectly");
    require(result.Reports[0].InstructionName == "INSERTQ" && result.Reports[0].Offset == 0x208 && result.Reports[0].Lowering == Codegen::Amd64OnlyLowering::Trampoline && result.Reports[0].ReplacementLength == 48, "Trampoline report is wrong");
    require(result.Reports[1].InstructionName == "MOVNTSS" && result.Reports[1].Offset == 0x20F && result.Reports[1].Lowering == Codegen::Amd64OnlyLowering::InPlace && result.Reports[1].ReplacementLength == 4, "In-place report is wrong");
    auto expected = file;
    expected[0x211] = 0x11;
    require(result.Bytes == expected, "Converter changed bytes other than the MOVNTSS opcode");
    const auto untouched = converter->Convert(Bytes(0x300, 0x90), {segmentHeader(0x100)});
    require(untouched.ReplacedCount == 0 && untouched.Trampolines.empty() && untouched.Reports.empty() && untouched.Bytes == Bytes(0x300, 0x90), "Segment without AMD-only instructions was changed");
    Bytes addressFixture(0x300, 0x90);
    const Bytes addressText = {0xA1, 0xF3, 0x0F, 0x2B, 0x07, 0x90, 0x90, 0x90, 0x90, 0xF3, 0x0F, 0x2B, 0x07, 0xC3};
    std::copy(addressText.begin(), addressText.end(), addressFixture.begin() + 0x200);
    const auto addressResult = converter->Convert(addressFixture, {segmentHeader(addressText.size())});
    auto expectedAddress = addressFixture;
    expectedAddress[0x20B] = 0x11;
    require(addressResult.Reports.size() == 1 && addressResult.Reports[0].Offset == 0x209 && addressResult.Bytes == expectedAddress, "Converter rewrote instruction-like bytes inside a MOFFS address");
    auto branchInside = file;
    branchInside[0x207] = 0x02;
    requireFailure([&] { (void)converter->Convert(branchInside, {segmentHeader(20)}); }, "Branch into an AMD-only instruction was accepted");
    auto rdpru = file;
    rdpru[0x20F] = 0x0F;
    rdpru[0x210] = 0x01;
    rdpru[0x211] = 0xFD;
    rdpru[0x212] = 0x90;
    requireFailure([&] { (void)converter->Convert(rdpru, {segmentHeader(20)}); }, "RDPRU was silently kept");
    auto monitorx = file;
    monitorx[0x20F] = 0x0F;
    monitorx[0x210] = 0x01;
    monitorx[0x211] = 0xFA;
    monitorx[0x212] = 0x90;
    const auto monitorxResult = converter->Convert(monitorx, {segmentHeader(20)});
    require(monitorxResult.ReplacedCount == 1 && monitorxResult.Bytes[0x20F] == 0x0F && monitorxResult.Bytes[0x210] == 0x1F && monitorxResult.Bytes[0x211] == 0x00, "MONITORX was not replaced in place");
    auto clzero = monitorx;
    clzero[0x211] = 0xFC;
    clzero[0x213] = 0x90;
    const auto clzeroResult = converter->Convert(clzero, {segmentHeader(20)});
    require(clzeroResult.Trampolines.size() == 2 && clzeroResult.Trampolines[1].Offset == 0x20F && clzeroResult.Trampolines[1].Length == 5, "Short CLZERO did not absorb the following instructions");
    require(clzeroResult.Trampolines[1].Body[clzeroResult.Trampolines[1].ReturnBranchOffset] == 0xE9, "CLZERO stub does not end with the return jump");
    auto registerForm = file;
    const Bytes extrqRegister = {0x66, 0x0F, 0x79, 0xCA};
    std::copy(extrqRegister.begin(), extrqRegister.end(), registerForm.begin() + 0x20F);
    const auto absorbedReturn = converter->Convert(registerForm, {segmentHeader(20)});
    require(absorbedReturn.Trampolines.size() == 2 && absorbedReturn.Trampolines[1].Length == 5, "Short EXTRQ did not absorb the following return");
    require(absorbedReturn.Trampolines[1].Body[absorbedReturn.Trampolines[1].ReturnBranchOffset - 1] == 0xC3, "The absorbed return is missing from the stub");
    require(absorbedReturn.Trampolines[1].Fixups.empty(), "A return does not need a relocation");
    registerForm[0x213] = 0x90;
    const auto relocated = converter->Convert(registerForm, {segmentHeader(20)});
    require(relocated.Trampolines.size() == 2, "Short EXTRQ register form was not lowered through a stub");
    const auto& shortSite = relocated.Trampolines[1];
    const Bytes shortOriginal = {0x66, 0x0F, 0x79, 0xCA, 0x90};
    require(shortSite.Offset == 0x20F && shortSite.Length == 5 && shortSite.OriginalBytes == shortOriginal, "Short EXTRQ site did not absorb the following instruction");
    require(shortSite.Body[shortSite.ReturnBranchOffset - 1] == 0x90 && shortSite.Body[shortSite.ReturnBranchOffset] == 0xE9, "Absorbed instruction does not run before the return jump");
    auto branched = file;
    std::copy(extrqRegister.begin(), extrqRegister.end(), branched.begin() + 0x20F);
    branched[0x213] = 0xEB;
    branched[0x214] = 0x00;
    const auto absorbedBranch = converter->Convert(branched, {segmentHeader(22)});
    const auto& branchSite = absorbedBranch.Trampolines[1];
    require(branchSite.Length == 6 && branchSite.Fixups.size() == 1, "Short EXTRQ did not absorb and relocate the following branch");
    require(branchSite.Body[branchSite.Fixups[0].BodyOffset - 1] == 0xE9, "The absorbed short jump was not widened to a 32 bit jump");
    require(branchSite.Fixups[0].Target == 0x1015, "The relocated branch does not keep its original target");
    requireFailure([&] { (void)converter->Convert(file, {segmentHeader(0x200)}); }, "Segment exceeding the file was accepted");
}

void converterFailureOffsets() {
    const auto converter = Codegen::MakeAmd64OnlyConverter();
    const auto file = segmentFixture();
    auto undecodable = file;
    const Bytes bareSse4a = {0x0F, 0x78, 0xC0, 0x00};
    std::copy(bareSse4a.begin(), bareSse4a.end(), undecodable.begin() + 0x20F);
    require(failureOffset([&] { (void)converter->Convert(undecodable, {segmentHeader(20)}); }, "Undecodable instruction was accepted") == 0x20F, "Decoder failure does not carry the file offset");
    auto memoryForm = file;
    memoryForm[0x20C] = 0x08;
    require(failureOffset([&] { (void)converter->Convert(memoryForm, {segmentHeader(20)}); }, "INSERTQ memory form was accepted") == 0x208, "SSE4a operand failure does not carry the file offset");
    auto movntsRegister = file;
    movntsRegister[0x212] = 0xC1;
    require(failureOffset([&] { (void)converter->Convert(movntsRegister, {segmentHeader(20)}); }, "MOVNTSS register form was accepted") == 0x20F, "MOVNTSS failure does not carry the file offset");
    auto rdpru = file;
    const Bytes rdpruBytes = {0x0F, 0x01, 0xFD, 0x90};
    std::copy(rdpruBytes.begin(), rdpruBytes.end(), rdpru.begin() + 0x20F);
    require(failureOffset([&] { (void)converter->Convert(rdpru, {segmentHeader(20)}); }, "RDPRU was accepted") == 0x20F, "Unsupported instruction failure does not carry the file offset");
}

Bytes elfFixture(const Bytes& text) {
    Bytes bytes(0x400);
    bytes[0] = 0x7F;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 2;
    bytes[5] = 1;
    bytes[6] = 1;
    write<std::uint16_t>(bytes, 16, 3);
    write<std::uint16_t>(bytes, 18, 62);
    write<std::uint64_t>(bytes, 24, 0x1000);
    write<std::uint64_t>(bytes, 32, 64);
    write<std::uint16_t>(bytes, 54, 56);
    write<std::uint16_t>(bytes, 56, 6);
    write<std::uint32_t>(bytes, 64, 1);
    write<std::uint32_t>(bytes, 68, 5);
    write<std::uint64_t>(bytes, 72, 0x200);
    write<std::uint64_t>(bytes, 80, 0x1000);
    write<std::uint64_t>(bytes, 96, 0x100);
    write<std::uint64_t>(bytes, 104, 0x100);
    write<std::uint64_t>(bytes, 112, 0x1000);
    write<std::uint32_t>(bytes, 120, 1);
    write<std::uint32_t>(bytes, 124, 6);
    write<std::uint64_t>(bytes, 128, 0x300);
    write<std::uint64_t>(bytes, 136, 0x2000);
    write<std::uint64_t>(bytes, 152, 0x100);
    write<std::uint64_t>(bytes, 160, 0x100);
    write<std::uint64_t>(bytes, 168, 0x1000);
    std::fill(bytes.begin() + 0x200, bytes.begin() + 0x300, 0xCC);
    std::copy(text.begin(), text.end(), bytes.begin() + 0x200);
    return bytes;
}

std::vector<Domain::ProgramHeader> elfHeaders() {
    return {{1, 5, 0x200, 0x1000, 0, 0x100, 0x100, 0x1000}, {1, 6, 0x300, 0x2000, 0, 0x100, 0x100, 0x1000}};
}

void linuxPlacement() {
    const auto source = elfFixture({0xEB, 0x06, 0xF2, 0x0F, 0x78, 0xDB, 0x08, 0x08, 0xC3});
    const auto headers = elfHeaders();
    const auto converted = Codegen::MakeAmd64OnlyConverter()->Convert(source, {headers[0]});
    require(converted.Trampolines.size() == 1 && converted.Bytes == source, "Linux fixture conversion produced unexpected results");
    const auto byteWriter = std::make_shared<Io::ByteWriter>();
    Elfpatcher::Linux::LinuxElfPatcher patcher(
        std::make_shared<Elfpatcher::EntryStubBuilder>(),
        std::make_shared<Elfpatcher::ProgramHeaderLayoutBuilder>(std::make_shared<Elfpatcher::SegmentFilter>(), byteWriter),
        std::make_shared<Elfpatcher::SectionHeaderTableBuilder>(byteWriter),
        byteWriter);
    const auto output = patcher.Patch(converted.Bytes, headers, {}, 0, "$ORIGIN/libs", true, false, converted.Trampolines);
    require(output[0x202] == 0xE9 && output[0x207] == 0x90, "Linux site was not replaced by a jump");
    const auto target = 0x1002 + 5 + static_cast<std::int64_t>(read<std::int32_t>(output, 0x203));
    require(target % 16 == 0 && target > 0x2100, "Linux stub is misaligned or inside the original image");
    const auto phNum = read<std::uint16_t>(output, 56);
    std::uint64_t bodyOffset = 0;
    bool found = false;
    for (std::uint16_t index = 0; index < phNum; ++index) {
        const auto header = 64 + index * 56;
        if (read<std::uint32_t>(output, header) != 1) continue;
        const auto vaddr = read<std::uint64_t>(output, header + 16);
        const auto memSize = read<std::uint64_t>(output, header + 40);
        if (static_cast<std::uint64_t>(target) < vaddr || static_cast<std::uint64_t>(target) >= vaddr + memSize) continue;
        require((read<std::uint32_t>(output, header + 4) & 1) != 0, "Linux stub segment is not executable");
        bodyOffset = read<std::uint64_t>(output, header + 8) + (static_cast<std::uint64_t>(target) - vaddr);
        found = true;
    }
    require(found, "Linux stub is not inside a PT_LOAD segment");
    auto expectedBody = kInsertqSelfBody;
    write<std::int32_t>(expectedBody, 10, static_cast<std::int32_t>(0x1008 - (target + 9 + 5)));
    const Bytes actualBody(output.begin() + static_cast<std::ptrdiff_t>(bodyOffset), output.begin() + static_cast<std::ptrdiff_t>(bodyOffset + expectedBody.size()));
    require(actualBody == expectedBody, "Linux stub body or return branch is wrong");
    auto altered = converted.Bytes;
    altered[0x205] = 0xDC;
    requireFailure([&] { (void)patcher.Patch(altered, headers, {}, 0, "$ORIGIN/libs", true, false, converted.Trampolines); }, "Changed Linux site bytes were accepted");
}

}

#if defined(__linux__) && defined(__x86_64__)
std::uint64_t extrqReference(std::uint64_t value, std::uint64_t control) {
    const auto length = static_cast<unsigned>(control & 0x3f);
    const auto index = static_cast<unsigned>((control >> 8) & 0x3f);
    const auto shifted = value >> index;
    return length == 0 ? shifted : shifted & ((std::uint64_t{1} << length) - 1);
}

std::uint64_t runExtrqStub(const Bytes& site, std::uint64_t destination, std::uint64_t control) {
    const auto matcher = Codegen::MakeAmd64OnlyInstructionMatcher();
    const auto match = matcher->Match(site.data(), site.size());
    require(match && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline, "EXTRQ register form stub was not produced");
    auto body = match->StubBody;
    const auto ret = body.size();
    body.push_back(0xC3);
    const auto displacement = static_cast<std::int32_t>(ret - (match->ReturnBranchOffset + 5));
    std::memcpy(body.data() + match->ReturnBranchOffset + 1, &displacement, sizeof(displacement));
    void* code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(code != MAP_FAILED, "cannot map executable memory for the stub");
    std::memcpy(code, body.data(), body.size());
    alignas(16) std::uint64_t destinationIn[2] = {destination, 0x1122334455667788ull};
    alignas(16) std::uint64_t controlIn[2] = {control, 0};
    alignas(16) std::uint64_t out[2] = {};
    alignas(16) std::uint64_t scratchIn[2] = {0x0123456789abcdefull, 0xfedcba9876543210ull};
    alignas(16) std::uint64_t scratchOut[2] = {};
    const bool same = site[3] == 0xD2;
    asm volatile(
        "movdqu (%[scratch]), %%xmm0\n\t"
        "movdqu (%[dst]), %%xmm2\n\t"
        "movdqu (%[ctl]), %%xmm5\n\t"
        "sub $128, %%rsp\n\t"
        "call *%[code]\n\t"
        "add $128, %%rsp\n\t"
        "movdqu %%xmm2, (%[out])\n\t"
        "movdqu %%xmm0, (%[scratchOut])\n\t"
        :
        : [scratch] "r"(scratchIn), [dst] "r"(same ? controlIn : destinationIn), [ctl] "r"(controlIn), [code] "r"(code), [out] "r"(out), [scratchOut] "r"(scratchOut)
        : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
    munmap(code, 4096);
    require(scratchOut[0] == scratchIn[0] && scratchOut[1] == scratchIn[1], "EXTRQ stub clobbered a scratch register");
    return out[0];
}

std::uint64_t insertqReference(std::uint64_t destination, std::uint64_t value, std::uint64_t control) {
    const auto rawLength = static_cast<unsigned>(control & 0x3f);
    const auto index = static_cast<unsigned>((control >> 8) & 0x3f);
    const auto length = rawLength == 0 ? 64u : rawLength;
    const auto mask = length >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << length) - 1);
    return (destination & ~(mask << index)) | ((value & mask) << index);
}

void runInsertqStub(const Bytes& site, std::uint64_t destination, std::uint64_t value, std::uint64_t control, std::uint64_t* low, std::uint64_t* high) {
    const auto matcher = Codegen::MakeAmd64OnlyInstructionMatcher();
    const auto match = matcher->Match(site.data(), site.size());
    require(match && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline, "INSERTQ register form stub was not produced");
    auto body = match->StubBody;
    const auto ret = body.size();
    body.push_back(0xC3);
    const auto displacement = static_cast<std::int32_t>(ret - (match->ReturnBranchOffset + 5));
    std::memcpy(body.data() + match->ReturnBranchOffset + 1, &displacement, sizeof(displacement));
    void* code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(code != MAP_FAILED, "cannot map executable memory for the stub");
    std::memcpy(code, body.data(), body.size());
    alignas(16) std::uint64_t destinationIn[2] = {destination, 0x1122334455667788ull};
    alignas(16) std::uint64_t sourceIn[2] = {value, control};
    alignas(16) std::uint64_t out[2] = {};
    alignas(16) std::uint64_t scratchIn[6] = {0x0123456789abcdefull, 0xfedcba9876543210ull, 0x00ff00ff00ff00ffull, 0xff00ff00ff00ff00ull, 0xdeadbeefcafebabeull, 0xbaadf00dfeedfaceull};
    alignas(16) std::uint64_t scratchOut[6] = {};
    const bool same = site[3] == 0xD2;
    asm volatile(
        "movdqu (%[scratch]), %%xmm0\n\t"
        "movdqu 16(%[scratch]), %%xmm1\n\t"
        "movdqu 32(%[scratch]), %%xmm3\n\t"
        "movdqu (%[src]), %%xmm5\n\t"
        "movdqu (%[dst]), %%xmm2\n\t"
        "sub $128, %%rsp\n\t"
        "call *%[code]\n\t"
        "add $128, %%rsp\n\t"
        "movdqu %%xmm2, (%[out])\n\t"
        "movdqu %%xmm0, (%[scratchOut])\n\t"
        "movdqu %%xmm1, 16(%[scratchOut])\n\t"
        "movdqu %%xmm3, 32(%[scratchOut])\n\t"
        :
        : [scratch] "r"(scratchIn), [dst] "r"(same ? sourceIn : destinationIn), [src] "r"(sourceIn), [code] "r"(code), [out] "r"(out), [scratchOut] "r"(scratchOut)
        : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
    munmap(code, 4096);
    for (std::size_t index = 0; index < 6; ++index)
        require(scratchOut[index] == scratchIn[index], "INSERTQ stub clobbered a scratch register");
    *low = out[0];
    *high = out[1];
}

std::uint32_t rotateLeft(const std::uint32_t value, const int count) {
    return (value << count) | (value >> (32 - count));
}

std::uint32_t rotateRight(const std::uint32_t value, const int count) {
    return (value >> count) | (value << (32 - count));
}

void shaReference(const std::uint8_t opcode, const std::uint32_t* destination, const std::uint32_t* source, std::uint32_t* out) {
    const auto d = destination;
    const auto s = source;
    switch (opcode) {
    case 0xC8:
        out[0] = s[0];
        out[1] = s[1];
        out[2] = s[2];
        out[3] = s[3] + rotateLeft(d[3], 30);
        return;
    case 0xC9:
        out[0] = d[0] ^ s[2];
        out[1] = d[1] ^ s[3];
        out[2] = d[2] ^ d[0];
        out[3] = d[3] ^ d[1];
        return;
    case 0xCA: {
        const auto w16 = rotateLeft(d[3] ^ s[2], 1);
        out[3] = w16;
        out[2] = rotateLeft(d[2] ^ s[1], 1);
        out[1] = rotateLeft(d[1] ^ s[0], 1);
        out[0] = rotateLeft(d[0] ^ w16, 1);
        return;
    }
    case 0xCC: {
        const auto sigma = [](const std::uint32_t value) { return rotateRight(value, 7) ^ rotateRight(value, 18) ^ (value >> 3); };
        out[0] = d[0] + sigma(d[1]);
        out[1] = d[1] + sigma(d[2]);
        out[2] = d[2] + sigma(d[3]);
        out[3] = d[3] + sigma(s[0]);
        return;
    }
    default: {
        const auto sigma = [](const std::uint32_t value) { return rotateRight(value, 17) ^ rotateRight(value, 19) ^ (value >> 10); };
        out[0] = d[0] + sigma(s[2]);
        out[1] = d[1] + sigma(s[3]);
        out[2] = d[2] + sigma(out[0]);
        out[3] = d[3] + sigma(out[1]);
        return;
    }
    }
}

void sha1Rnds4Reference(const std::uint32_t* destination, const std::uint32_t* source, const std::uint8_t immediate, std::uint32_t* out) {
    static const std::uint32_t constants[4] = {0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xCA62C1D6u};
    const auto selector = immediate & 3u;
    std::uint32_t a = destination[3], b = destination[2], c = destination[1], d = destination[0], e = 0;
    const std::uint32_t w[4] = {source[3], source[2], source[1], source[0]};
    for (int round = 0; round < 4; ++round) {
        std::uint32_t mixed;
        if (selector == 0) mixed = (b & c) | (~b & d);
        else if (selector == 2) mixed = (b & c) | (b & d) | (c & d);
        else mixed = b ^ c ^ d;
        const auto value = mixed + rotateLeft(a, 5) + w[round] + e + constants[selector];
        e = d;
        d = c;
        c = rotateLeft(b, 30);
        b = a;
        a = value;
    }
    out[3] = a;
    out[2] = b;
    out[1] = c;
    out[0] = d;
}

void sha256Rnds2Reference(const std::uint32_t* destination, const std::uint32_t* source, const std::uint32_t* implicit, std::uint32_t* out) {
    std::uint32_t a = source[3], b = source[2], c = destination[3], d = destination[2];
    std::uint32_t e = source[1], f = source[0], g = destination[1], h = destination[0];
    for (int round = 0; round < 2; ++round) {
        const auto choose = (e & f) ^ (~e & g);
        const auto sigma1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const auto sigma0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto value = choose + sigma1 + implicit[round] + h;
        h = g;
        g = f;
        f = e;
        e = d + value;
        d = c;
        c = b;
        b = a;
        a = value + majority + sigma0;
    }
    out[3] = a;
    out[2] = b;
    out[1] = e;
    out[0] = f;
}

void runShaNative(const std::uint8_t opcode, const std::uint32_t* destination, const std::uint32_t* source, std::uint32_t* out) {
    switch (opcode) {
    case 0xC8:
        asm volatile("movdqu (%[s]), %%xmm1\n\t movdqu (%[d]), %%xmm3\n\t .byte 0x0f,0x38,0xc8,0xd9\n\t movdqu %%xmm3, (%[o])"
                     : : [d] "r"(destination), [s] "r"(source), [o] "r"(out) : "xmm1", "xmm3", "memory");
        return;
    case 0xC9:
        asm volatile("movdqu (%[s]), %%xmm1\n\t movdqu (%[d]), %%xmm3\n\t .byte 0x0f,0x38,0xc9,0xd9\n\t movdqu %%xmm3, (%[o])"
                     : : [d] "r"(destination), [s] "r"(source), [o] "r"(out) : "xmm1", "xmm3", "memory");
        return;
    case 0xCA:
        asm volatile("movdqu (%[s]), %%xmm1\n\t movdqu (%[d]), %%xmm3\n\t .byte 0x0f,0x38,0xca,0xd9\n\t movdqu %%xmm3, (%[o])"
                     : : [d] "r"(destination), [s] "r"(source), [o] "r"(out) : "xmm1", "xmm3", "memory");
        return;
    case 0xCC:
        asm volatile("movdqu (%[s]), %%xmm1\n\t movdqu (%[d]), %%xmm3\n\t .byte 0x0f,0x38,0xcc,0xd9\n\t movdqu %%xmm3, (%[o])"
                     : : [d] "r"(destination), [s] "r"(source), [o] "r"(out) : "xmm1", "xmm3", "memory");
        return;
    default:
        asm volatile("movdqu (%[s]), %%xmm1\n\t movdqu (%[d]), %%xmm3\n\t .byte 0x0f,0x38,0xcd,0xd9\n\t movdqu %%xmm3, (%[o])"
                     : : [d] "r"(destination), [s] "r"(source), [o] "r"(out) : "xmm1", "xmm3", "memory");
        return;
    }
}

void runSha256Rnds2Native(const std::uint32_t* destination, const std::uint32_t* source, const std::uint32_t* implicit, std::uint32_t* out) {
    asm volatile("movdqu (%[k]), %%xmm0\n\t movdqu (%[s]), %%xmm1\n\t movdqu (%[d]), %%xmm3\n\t .byte 0x0f,0x38,0xcb,0xd9\n\t movdqu %%xmm3, (%[o])"
                 : : [d] "r"(destination), [s] "r"(source), [k] "r"(implicit), [o] "r"(out) : "xmm0", "xmm1", "xmm3", "memory");
}

void runSha1Rnds4Native(const std::uint32_t* destination, const std::uint32_t* source, const std::uint8_t immediate, std::uint32_t* out) {
    switch (immediate & 3u) {
    case 0:
        asm volatile("movdqu (%[s]), %%xmm1\n\t movdqu (%[d]), %%xmm3\n\t .byte 0x0f,0x3a,0xcc,0xd9,0x00\n\t movdqu %%xmm3, (%[o])"
                     : : [d] "r"(destination), [s] "r"(source), [o] "r"(out) : "xmm1", "xmm3", "memory");
        return;
    case 1:
        asm volatile("movdqu (%[s]), %%xmm1\n\t movdqu (%[d]), %%xmm3\n\t .byte 0x0f,0x3a,0xcc,0xd9,0x01\n\t movdqu %%xmm3, (%[o])"
                     : : [d] "r"(destination), [s] "r"(source), [o] "r"(out) : "xmm1", "xmm3", "memory");
        return;
    case 2:
        asm volatile("movdqu (%[s]), %%xmm1\n\t movdqu (%[d]), %%xmm3\n\t .byte 0x0f,0x3a,0xcc,0xd9,0x02\n\t movdqu %%xmm3, (%[o])"
                     : : [d] "r"(destination), [s] "r"(source), [o] "r"(out) : "xmm1", "xmm3", "memory");
        return;
    default:
        asm volatile("movdqu (%[s]), %%xmm1\n\t movdqu (%[d]), %%xmm3\n\t .byte 0x0f,0x3a,0xcc,0xd9,0x03\n\t movdqu %%xmm3, (%[o])"
                     : : [d] "r"(destination), [s] "r"(source), [o] "r"(out) : "xmm1", "xmm3", "memory");
        return;
    }
}

void shaMemoryExecution() {
    const auto matcher = Codegen::MakeAmd64OnlyInstructionMatcher();
    std::uint32_t seed = 0x1b873593u;
    const auto next = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    const Bytes indirect = {0x0F, 0x38, 0xC9, 0x1F};
    const Bytes scaled = {0x0F, 0x38, 0xCD, 0x5C, 0x8F, 0x08};
    const Bytes stackRelative = {0x0F, 0x38, 0xC8, 0x5C, 0x24, 0x20};
    for (int variant = 0; variant < 3; ++variant) {
        const auto& site = variant == 0 ? indirect : (variant == 1 ? scaled : stackRelative);
        const std::uint8_t opcode = site[2];
        const auto match = matcher->Match(site.data(), site.size());
        require(match && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline, "SHA-NI memory form stub was not produced");
        auto body = match->StubBody;
        const auto ret = body.size();
        body.push_back(0xC3);
        const auto displacement = static_cast<std::int32_t>(ret - (match->ReturnBranchOffset + 5));
        std::memcpy(body.data() + match->ReturnBranchOffset + 1, &displacement, sizeof(displacement));
        void* code = mmap(nullptr, 8192, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        require(code != MAP_FAILED, "cannot map executable memory for the stub");
        std::memcpy(code, body.data(), body.size());
        for (int iteration = 0; iteration < 64; ++iteration) {
            alignas(16) std::uint32_t destination[4];
            alignas(16) std::uint32_t memory[16];
            alignas(16) std::uint32_t out[4] = {};
            for (auto& value : destination) value = next();
            for (auto& value : memory) value = next();
            const std::uint32_t* operand = memory;
            const std::uint64_t index = 2;
            if (variant == 0) {
                asm volatile("movdqu (%[d]), %%xmm3\n\t sub $128, %%rsp\n\t call *%[c]\n\t add $128, %%rsp\n\t movdqu %%xmm3, (%[o])"
                             : : [d] "r"(destination), [c] "r"(code), [o] "r"(out), "D"(memory)
                             : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
            } else if (variant == 1) {
                operand = memory + index;
                asm volatile("movdqu (%[d]), %%xmm3\n\t sub $128, %%rsp\n\t call *%[cd]\n\t add $128, %%rsp\n\t movdqu %%xmm3, (%[o])"
                             : : [d] "r"(destination), [cd] "r"(code), [o] "r"(out), "D"(reinterpret_cast<const std::uint8_t*>(memory) - 8), "c"(index)
                             : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
            } else {
                // The relinker reaches a stub with a jump, this harness with a call: the pushed
                // return address shifts RSP by eight, so the payload is duplicated eight bytes down.
                asm volatile("sub $0x60, %%rsp\n\t movdqu (%[m]), %%xmm4\n\t movdqu %%xmm4, 0x20(%%rsp)\n\t movdqu %%xmm4, 0x18(%%rsp)\n\t"
                             "movdqu (%[d]), %%xmm3\n\t call *%[c]\n\t movdqu %%xmm3, (%[o])\n\t add $0x60, %%rsp"
                             : : [d] "r"(destination), [m] "r"(memory), [c] "r"(code), [o] "r"(out)
                             : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc");
            }
            std::uint32_t expected[4] = {};
            shaReference(opcode, destination, operand, expected);
            require(std::memcmp(out, expected, sizeof(expected)) == 0, "SHA-NI memory form stub does not match the reference model");
            if (__builtin_cpu_supports("sha")) {
                alignas(16) std::uint32_t native[4] = {};
                runShaNative(opcode, destination, operand, native);
                require(std::memcmp(out, native, sizeof(native)) == 0, "SHA-NI memory form stub does not match the hardware instruction");
            }
        }
        munmap(code, 8192);
    }
}

void shaRoundExecution() {
    const auto matcher = Codegen::MakeAmd64OnlyInstructionMatcher();
    std::uint32_t seed = 0x9e3779b9u;
    const auto next = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    for (int variant = 0; variant < 5; ++variant) {
        const bool rounds4 = variant < 4;
        const auto immediate = static_cast<std::uint8_t>(variant & 3);
        const Bytes site = rounds4 ? Bytes{0x0F, 0x3A, 0xCC, 0xD9, immediate} : Bytes{0x0F, 0x38, 0xCB, 0xD9};
        const auto match = matcher->Match(site.data(), site.size());
        require(match && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline, "SHA-NI round stub was not produced");
        auto body = match->StubBody;
        const auto ret = body.size();
        body.push_back(0xC3);
        const auto displacement = static_cast<std::int32_t>(ret - (match->ReturnBranchOffset + 5));
        std::memcpy(body.data() + match->ReturnBranchOffset + 1, &displacement, sizeof(displacement));
        void* code = mmap(nullptr, 8192, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        require(code != MAP_FAILED, "cannot map executable memory for the stub");
        std::memcpy(code, body.data(), body.size());
        for (int iteration = 0; iteration < 128; ++iteration) {
            alignas(16) std::uint32_t destination[4];
            alignas(16) std::uint32_t source[4];
            alignas(16) std::uint32_t implicit[4];
            alignas(16) std::uint32_t out[4] = {};
            for (auto& value : destination) value = next();
            for (auto& value : source) value = next();
            for (auto& value : implicit) value = next();
            std::uint64_t flags = 0;
            asm volatile(
                "movdqu (%[k]), %%xmm0\n\t"
                "movdqu (%[src]), %%xmm1\n\t"
                "movdqu (%[dst]), %%xmm3\n\t"
                "lea -128(%%rsp), %%rsp\n\t"
                "stc\n\t"
                "call *%[code]\n\t"
                "pushfq\n\t"
                "popq %%rdx\n\t"
                "lea 128(%%rsp), %%rsp\n\t"
                "movdqu %%xmm3, (%[out])\n\t"
                "movq %%rdx, %[flagsOut]"
                : [flagsOut] "=m"(flags)
                : [dst] "r"(destination), [src] "r"(source), [k] "r"(implicit), [code] "r"(code), [out] "r"(out)
                : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm3", "memory", "cc");
            std::uint32_t expected[4] = {};
            if (rounds4) sha1Rnds4Reference(destination, source, immediate, expected);
            else sha256Rnds2Reference(destination, source, implicit, expected);
            require(std::memcmp(out, expected, sizeof(expected)) == 0, "SHA-NI round stub does not match the reference model");
            require((flags & 1u) == 1u, "SHA-NI round stub did not preserve the flags");
            if (__builtin_cpu_supports("sha")) {
                alignas(16) std::uint32_t native[4] = {};
                if (rounds4) runSha1Rnds4Native(destination, source, immediate, native);
                else runSha256Rnds2Native(destination, source, implicit, native);
                require(std::memcmp(out, native, sizeof(native)) == 0, "SHA-NI round stub does not match the hardware instruction");
            }
        }
        munmap(code, 8192);
    }
}

void shaNiExecution() {
    const auto matcher = Codegen::MakeAmd64OnlyInstructionMatcher();
    std::uint32_t seed = 0x2545f491u;
    const auto next = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    for (const std::uint8_t opcode : {0xC8, 0xC9, 0xCA, 0xCC, 0xCD}) {
        const Bytes site = {0x0F, 0x38, opcode, 0xD9};
        const auto match = matcher->Match(site.data(), site.size());
        require(match && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline, "SHA-NI stub was not produced");
        auto body = match->StubBody;
        const auto ret = body.size();
        body.push_back(0xC3);
        const auto displacement = static_cast<std::int32_t>(ret - (match->ReturnBranchOffset + 5));
        std::memcpy(body.data() + match->ReturnBranchOffset + 1, &displacement, sizeof(displacement));
        void* code = mmap(nullptr, 8192, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        require(code != MAP_FAILED, "cannot map executable memory for the stub");
        std::memcpy(code, body.data(), body.size());
        for (int iteration = 0; iteration < 128; ++iteration) {
            alignas(16) std::uint32_t destination[4];
            alignas(16) std::uint32_t source[4];
            alignas(16) std::uint32_t out[4] = {};
            alignas(16) std::uint32_t scratchIn[8];
            alignas(16) std::uint32_t scratchOut[8] = {};
            for (auto& value : destination) value = next();
            for (auto& value : source) value = next();
            for (auto& value : scratchIn) value = next();
            asm volatile(
                "movdqu (%[scratch]), %%xmm0\n\t"
                "movdqu 16(%[scratch]), %%xmm2\n\t"
                "movdqu (%[src]), %%xmm1\n\t"
                "movdqu (%[dst]), %%xmm3\n\t"
                "sub $128, %%rsp\n\t"
                "call *%[code]\n\t"
                "add $128, %%rsp\n\t"
                "movdqu %%xmm3, (%[out])\n\t"
                "movdqu %%xmm0, (%[scratchOut])\n\t"
                "movdqu %%xmm2, 16(%[scratchOut])"
                :
                : [scratch] "r"(scratchIn), [dst] "r"(destination), [src] "r"(source), [code] "r"(code), [out] "r"(out), [scratchOut] "r"(scratchOut)
                : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "memory", "cc");
            std::uint32_t expected[4] = {};
            shaReference(opcode, destination, source, expected);
            require(std::memcmp(out, expected, sizeof(expected)) == 0, "SHA-NI stub does not match the reference model");
            require(std::memcmp(scratchOut, scratchIn, sizeof(scratchIn)) == 0, "SHA-NI stub clobbered a scratch register");
            if (__builtin_cpu_supports("sha")) {
                alignas(16) std::uint32_t native[4] = {};
                runShaNative(opcode, destination, source, native);
                require(std::memcmp(out, native, sizeof(native)) == 0, "SHA-NI stub does not match the hardware instruction");
            }
        }
        munmap(code, 8192);
    }
}

void clzeroExecution() {
    const auto matcher = Codegen::MakeAmd64OnlyInstructionMatcher();
    const Bytes site = {0x0F, 0x01, 0xFC};
    const auto match = matcher->Match(site.data(), site.size());
    require(match && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline, "CLZERO stub was not produced");
    auto body = match->StubBody;
    const auto ret = body.size();
    body.push_back(0xC3);
    const auto displacement = static_cast<std::int32_t>(ret - (match->ReturnBranchOffset + 5));
    std::memcpy(body.data() + match->ReturnBranchOffset + 1, &displacement, sizeof(displacement));
    void* code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(code != MAP_FAILED, "cannot map executable memory for the stub");
    std::memcpy(code, body.data(), body.size());
    alignas(64) std::uint8_t buffer[192];
    std::memset(buffer, 0xAA, sizeof(buffer));
    std::uint8_t* const target = buffer + 70;
    std::uint64_t returnedRax = 0;
    std::uint64_t returnedFlags = 0;
    asm volatile(
        "movq %[target], %%rax\n\t"
        "lea -128(%%rsp), %%rsp\n\t"
        "stc\n\t"
        "call *%[code]\n\t"
        "pushfq\n\t"
        "popq %%rdx\n\t"
        "lea 128(%%rsp), %%rsp\n\t"
        "movq %%rax, %[rax]\n\t"
        "movq %%rdx, %[flags]"
        : [rax] "=m"(returnedRax), [flags] "=m"(returnedFlags)
        : [target] "r"(target), [code] "r"(code)
        : "rax", "rdx", "memory", "cc");
    munmap(code, 4096);
    require(returnedRax == reinterpret_cast<std::uint64_t>(target), "CLZERO stub did not preserve RAX");
    require((returnedFlags & 1u) == 1u, "CLZERO stub did not preserve the flags");
    for (std::size_t index = 0; index < sizeof(buffer); ++index) {
        const bool inLine = index >= 64 && index < 128;
        require(buffer[index] == (inLine ? 0x00 : 0xAA), "CLZERO stub zeroed the wrong bytes");
    }
}

void insertqRegisterFormExecution() {
    const Bytes distinct = {0xF2, 0x0F, 0x79, 0xD5};
    const Bytes same = {0xF2, 0x0F, 0x79, 0xD2};
    const std::uint64_t destination = 0x0f1e2d3c4b5a6978ull;
    const std::uint64_t value = 0x9e3779b97f4a7c15ull;
    for (const auto [length, index] : {std::pair{8u, 4u}, {0u, 0u}, {40u, 20u}, {63u, 1u}, {1u, 63u}, {16u, 48u}, {64u, 0u}, {32u, 32u}}) {
        const auto control = (static_cast<std::uint64_t>(length) & 0x3f) | ((static_cast<std::uint64_t>(index) & 0x3f) << 8) | 0xffffc0c0ull;
        std::uint64_t low = 0;
        std::uint64_t high = 0;
        runInsertqStub(distinct, destination, value, control, &low, &high);
        require(low == insertqReference(destination, value, control), "INSERTQ register form stub inserted the wrong field");
        require(high == 0x1122334455667788ull, "INSERTQ register form stub changed the upper quadword of the destination");
        runInsertqStub(same, value, value, control, &low, &high);
        require(low == insertqReference(value, value, control), "INSERTQ register form stub with equal operands inserted the wrong field");
        require(high == control, "INSERTQ register form stub with equal operands changed the upper quadword of the destination");
    }
}

void registerFormExecution() {
    const Bytes distinct = {0x66, 0x0F, 0x79, 0xD5};
    const Bytes same = {0x66, 0x0F, 0x79, 0xD2};
    const std::uint64_t value = 0x9e3779b97f4a7c15ull;
    for (const auto [length, index] : {std::pair{8u, 4u}, {0u, 0u}, {40u, 20u}, {63u, 1u}, {1u, 63u}, {16u, 48u}}) {
        const auto control = static_cast<std::uint64_t>(length) | (static_cast<std::uint64_t>(index) << 8) | 0xffffc000ull;
        require(runExtrqStub(distinct, value, control) == extrqReference(value, control), "EXTRQ register form stub computed the wrong field");
        require(runExtrqStub(same, control, control) == extrqReference(control, control), "EXTRQ register form stub with equal operands computed the wrong field");
    }
}
std::uint32_t sha1nexteLane(const std::uint32_t value) {
    return (value << 30) | (value >> 2);
}

void sha1nexteExecution() {
    const auto matcher = Codegen::MakeAmd64OnlyInstructionMatcher();
    const Bytes site = {0x0F, 0x38, 0xC8, 0xD9};
    const auto match = matcher->Match(site.data(), site.size());
    require(match && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline, "SHA1NEXTE stub was not produced");
    auto body = match->StubBody;
    const auto ret = body.size();
    body.push_back(0xC3);
    const auto displacement = static_cast<std::int32_t>(ret - (match->ReturnBranchOffset + 5));
    std::memcpy(body.data() + match->ReturnBranchOffset + 1, &displacement, sizeof(displacement));
    void* code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(code != MAP_FAILED, "cannot map executable memory for the stub");
    std::memcpy(code, body.data(), body.size());
    std::uint32_t seed = 0x2545f491u;
    const auto next = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    for (int iteration = 0; iteration < 256; ++iteration) {
        alignas(16) std::uint32_t destination[4];
        alignas(16) std::uint32_t source[4];
        alignas(16) std::uint32_t out[4] = {};
        alignas(16) std::uint32_t scratchIn[8];
        alignas(16) std::uint32_t scratchOut[8] = {};
        for (auto& value : destination) value = next();
        for (auto& value : source) value = next();
        for (auto& value : scratchIn) value = next();
        asm volatile(
            "movdqu (%[scratch]), %%xmm0\n\t"
            "movdqu 16(%[scratch]), %%xmm2\n\t"
            "movdqu (%[src]), %%xmm1\n\t"
            "movdqu (%[dst]), %%xmm3\n\t"
            "sub $128, %%rsp\n\t"
            "call *%[code]\n\t"
            "add $128, %%rsp\n\t"
            "movdqu %%xmm3, (%[out])\n\t"
            "movdqu %%xmm0, (%[scratchOut])\n\t"
            "movdqu %%xmm2, 16(%[scratchOut])"
            :
            : [scratch] "r"(scratchIn), [dst] "r"(destination), [src] "r"(source), [code] "r"(code), [out] "r"(out), [scratchOut] "r"(scratchOut)
            : "xmm0", "xmm1", "xmm2", "xmm3", "memory", "cc");
        std::uint32_t expected[4];
        expected[0] = source[0];
        expected[1] = source[1];
        expected[2] = source[2];
        expected[3] = source[3] + sha1nexteLane(destination[3]);
        require(std::memcmp(out, expected, sizeof(expected)) == 0, "SHA1NEXTE stub does not match the reference model");
        require(std::memcmp(scratchOut, scratchIn, sizeof(scratchIn)) == 0, "SHA1NEXTE stub clobbered a scratch register");
        if (__builtin_cpu_supports("sha")) {
            alignas(16) std::uint32_t native[4] = {};
            asm volatile(
                "movdqu (%[src]), %%xmm1\n\t"
                "movdqu (%[dst]), %%xmm3\n\t"
                ".byte 0x0f, 0x38, 0xc8, 0xd9\n\t"
                "movdqu %%xmm3, (%[out])"
                :
                : [dst] "r"(destination), [src] "r"(source), [out] "r"(native)
                : "xmm1", "xmm3", "memory");
            require(std::memcmp(out, native, sizeof(native)) == 0, "SHA1NEXTE stub does not match the hardware instruction");
        }
    }
    munmap(code, 4096);
}

#else
void registerFormExecution() {}
void insertqRegisterFormExecution() {}
void sha1nexteExecution() {}
void clzeroExecution() {}
void shaNiExecution() {}
void shaRoundExecution() {}
void shaMemoryExecution() {}
#endif

int main() {
    try {
        decoderLengths();
        sse4aOperands();
        matcherSubstitutions();
        goldenBodies();
        registerFormExecution();
        insertqRegisterFormExecution();
        clzeroExecution();
        shaNiExecution();
        shaRoundExecution();
        shaMemoryExecution();
        converterSegment();
        converterFailureOffsets();
        linuxPlacement();
        std::cout << "AMD64-only converter tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
