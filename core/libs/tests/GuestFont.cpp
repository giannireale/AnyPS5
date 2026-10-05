#include "prx/libSceFont/include/FontTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <vector>

extern "C" {
int APS5_VABI sceFontMemoryInit(FontMemory*, void*, std::uint32_t, const FontMemoryInterface*, void*, FontMemoryDestroyFunction, void*);
int APS5_VABI sceFontMemoryTerm(FontMemory*);
int APS5_VABI sceFontCreateLibrary(const FontMemory*, const void*, FontLibrary*);
int APS5_VABI sceFontDestroyLibrary(FontLibrary*);
int APS5_VABI sceFontCreateRenderer(const FontMemory*, const void*, FontRenderer*);
int APS5_VABI sceFontDestroyRenderer(FontRenderer*);
int APS5_VABI sceFontSupportSystemFonts(FontLibrary);
int APS5_VABI sceFontSupportExternalFonts(FontLibrary, std::uint32_t, std::uint32_t);
int APS5_VABI sceFontOpenFontSet(FontLibrary, std::uint32_t, std::uint32_t, const FontOpenDetail*, FontHandle*);
int APS5_VABI sceFontOpenFontMemory(FontLibrary, const void*, std::uint32_t, const FontOpenDetail*, FontHandle*);
int APS5_VABI sceFontCloseFont(FontHandle);
int APS5_VABI sceFontSetResolutionDpi(FontHandle, std::uint32_t, std::uint32_t);
int APS5_VABI sceFontGetResolutionDpi(FontHandle, std::uint32_t*, std::uint32_t*);
const void* APS5_VABI sceFontSelectLibraryFt(int);
const void* APS5_VABI sceFontSelectRendererFt(int);
}

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "Font check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

static int allocations = 0;
static void* APS5_VABI Allocate(void*, std::uint32_t size) {
    ++allocations;
    return std::malloc(size);
}
static void APS5_VABI Release(void*, void* pointer) {
    if (pointer) --allocations;
    std::free(pointer);
}

static void Put16(std::vector<unsigned char>& out, int value) {
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    out.push_back(static_cast<unsigned char>(value & 0xFF));
}

static std::vector<unsigned char> Words(std::initializer_list<int> values) {
    std::vector<unsigned char> out;
    for (const int value : values) Put16(out, value);
    return out;
}

static std::vector<unsigned char> EmptyGlyphFont() {
    const std::vector<std::pair<const char*, std::vector<unsigned char>>> tables = {
        {"glyf", Words({0, 0})},
        {"head", Words({1, 0, 1, 0, 0, 0, 0x5F0F, 0x3CF5, 0, 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 2, 0, 0})},
        {"hhea", Words({1, 0, 800, -200, 0, 500, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1})},
        {"hmtx", Words({500, 0})},
        {"loca", Words({0, 0})},
        {"maxp", Words({1, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0})},
    };
    std::vector<unsigned char> font = Words({1, 0, static_cast<int>(tables.size()), 64, 2, 32});
    int offset = 12 + 16 * static_cast<int>(tables.size());
    for (const auto& table : tables) {
        font.insert(font.end(), table.first, table.first + 4);
        for (const int value : {0, 0, offset >> 16, offset & 0xFFFF, 0, static_cast<int>(table.second.size())}) Put16(font, value);
        offset += static_cast<int>((table.second.size() + 3) & ~std::size_t{3});
    }
    for (const auto& table : tables) {
        font.insert(font.end(), table.second.begin(), table.second.end());
        font.resize((font.size() + 3) & ~std::size_t{3});
    }
    return font;
}

int main() {
    constexpr std::uint32_t SystemFontSet = 0x18070043u;
    const FontMemoryInterface iface{Allocate, Release, nullptr, nullptr, nullptr, nullptr};
    FontMemory memory{};
    Require(sceFontMemoryInit(&memory, nullptr, 0, &iface, nullptr, nullptr, nullptr) == SCE_FONT_OK);
    Require(sceFontSelectLibraryFt(0) != nullptr && sceFontSelectLibraryFt(1) == nullptr);
    Require(sceFontSelectRendererFt(0) != nullptr && sceFontSelectRendererFt(1) == nullptr);

    FontLibrary library = nullptr;
    Require(sceFontCreateLibrary(&memory, nullptr, &library) == SCE_FONT_ERROR_INVALID_PARAMETER && library == nullptr);
    Require(sceFontCreateLibrary(&memory, sceFontSelectLibraryFt(0), &library) == SCE_FONT_OK && library != nullptr);

    FontHandle font = reinterpret_cast<FontHandle>(&memory);
    Require(sceFontOpenFontSet(library, SystemFontSet, 1, nullptr, &font) == SCE_FONT_ERROR_NO_SUPPORT_FUNCTION && font == nullptr);
    Require(sceFontSupportSystemFonts(library) == SCE_FONT_OK);
    Require(sceFontOpenFontSet(library, SystemFontSet, 1, nullptr, &font) == SCE_FONT_ERROR_FONT_OPEN_FAILED && font == nullptr);
    Require(sceFontOpenFontSet(library, 0x12345678u, 1, nullptr, &font) == SCE_FONT_ERROR_NO_SUPPORT_FONTSET);
    Require(sceFontOpenFontSet(library, SystemFontSet, 7, nullptr, &font) == SCE_FONT_ERROR_INVALID_PARAMETER);
    Require(sceFontOpenFontSet(nullptr, SystemFontSet, 1, nullptr, &font) == SCE_FONT_ERROR_INVALID_LIBRARY);
    const unsigned char notAFont[64] = {1, 2, 3, 4};
    Require(sceFontOpenFontMemory(library, notAFont, sizeof(notAFont), nullptr, &font) == SCE_FONT_ERROR_NO_SUPPORT_FUNCTION && font == nullptr);
    Require(sceFontSupportExternalFonts(library, 4, 0x52) == SCE_FONT_OK);
    Require(sceFontSupportExternalFonts(library, 4, 0x52) == SCE_FONT_ERROR_ALREADY_SPECIFIED);
    Require(sceFontOpenFontMemory(library, nullptr, 0, nullptr, &font) == SCE_FONT_ERROR_INVALID_PARAMETER && font == nullptr);
    Require(sceFontOpenFontMemory(library, notAFont, sizeof(notAFont), nullptr, &font) == SCE_FONT_ERROR_NO_SUPPORT_FORMAT && font == nullptr);

    const std::vector<unsigned char> fontData = EmptyGlyphFont();
    Require(sceFontOpenFontMemory(library, fontData.data(), static_cast<std::uint32_t>(fontData.size()), nullptr, &font) == SCE_FONT_OK && font != nullptr);
    std::uint32_t hDpi = 1;
    std::uint32_t vDpi = 1;
    Require(sceFontGetResolutionDpi(font, &hDpi, &vDpi) == SCE_FONT_OK && hDpi == 72 && vDpi == 72);
    Require(sceFontSetResolutionDpi(font, 96, 144) == SCE_FONT_OK);
    Require(sceFontGetResolutionDpi(font, &hDpi, &vDpi) == SCE_FONT_OK && hDpi == 96 && vDpi == 144);
    hDpi = 1;
    vDpi = 1;
    Require(sceFontGetResolutionDpi(font, &hDpi, nullptr) == SCE_FONT_OK && hDpi == 96);
    Require(sceFontGetResolutionDpi(font, nullptr, &vDpi) == SCE_FONT_OK && vDpi == 144);
    Require(sceFontGetResolutionDpi(font, nullptr, nullptr) == SCE_FONT_ERROR_INVALID_PARAMETER);
    Require(sceFontSetResolutionDpi(font, 0, 300) == SCE_FONT_OK);
    Require(sceFontGetResolutionDpi(font, &hDpi, &vDpi) == SCE_FONT_OK && hDpi == 72 && vDpi == 300);
    Require(sceFontGetResolutionDpi(nullptr, &hDpi, &vDpi) == SCE_FONT_ERROR_INVALID_FONT_HANDLE && hDpi == 0 && vDpi == 0);
    FontHandleOpaque unopened{};
    hDpi = 1;
    vDpi = 1;
    Require(sceFontGetResolutionDpi(&unopened, &hDpi, &vDpi) == SCE_FONT_ERROR_INVALID_FONT_HANDLE && hDpi == 0 && vDpi == 0);
    Require(sceFontCloseFont(font) == SCE_FONT_OK);

    FontRenderer renderer = nullptr;
    Require(sceFontCreateRenderer(&memory, sceFontSelectRendererFt(0), &renderer) == SCE_FONT_OK && renderer != nullptr);
    Require(sceFontDestroyRenderer(&renderer) == SCE_FONT_OK && renderer == nullptr);
    Require(sceFontDestroyRenderer(&renderer) == SCE_FONT_ERROR_INVALID_RENDERER);

    Require(sceFontDestroyLibrary(&library) == SCE_FONT_OK && library == nullptr);
    Require(sceFontDestroyLibrary(&library) == SCE_FONT_ERROR_INVALID_LIBRARY);
    Require(allocations == 0);
    Require(sceFontMemoryTerm(&memory) == SCE_FONT_OK);
    Require(sceFontMemoryTerm(&memory) == SCE_FONT_ERROR_INVALID_MEMORY);
}
