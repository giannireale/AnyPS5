#include "Decoder/Jpeg.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

static void Require(bool value) { if (!value) std::abort(); }

template<typename TFunction>
static bool ThrowsInvalidArgument(TFunction function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

static bool IsJpeg(const std::vector<std::uint8_t>& data) {
    return data.size() > 4 && data[0] == 0xFF && data[1] == 0xD8 && data[data.size() - 2] == 0xFF && data.back() == 0xD9;
}

static int Difference(std::uint8_t left, std::uint8_t right) {
    return left > right ? left - right : right - left;
}

static std::vector<std::uint8_t> FrameHeader(const std::vector<std::uint8_t>& jpeg) {
    std::size_t cursor = 2;
    while (cursor + 4 <= jpeg.size()) {
        Require(jpeg[cursor] == 0xFF);
        const auto marker = jpeg[cursor + 1];
        const std::size_t length = (static_cast<std::size_t>(jpeg[cursor + 2]) << 8) | jpeg[cursor + 3];
        if (marker == 0xC0) return {jpeg.begin() + static_cast<std::ptrdiff_t>(cursor + 4), jpeg.begin() + static_cast<std::ptrdiff_t>(cursor + 2 + length)};
        cursor += 2 + length;
    }
    std::abort();
}

static int MaximumDifference(const std::vector<std::uint8_t>& left, const std::vector<std::uint8_t>& right) {
    Require(left.size() == right.size());
    int worst = 0;
    for (std::size_t index = 0; index < left.size(); ++index)
        worst = worst > Difference(left[index], right[index]) ? worst : Difference(left[index], right[index]);
    return worst;
}

static void CheckSampling() {
    constexpr std::uint32_t width = 61;
    constexpr std::uint32_t height = 37;
    std::vector<std::uint8_t> rgb(width * height * 3);
    std::vector<std::uint8_t> gray(width * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto index = y * width + x;
            rgb[index * 3 + 0] = static_cast<std::uint8_t>((x * 4) & 0xFF);
            rgb[index * 3 + 1] = static_cast<std::uint8_t>((y * 6) & 0xFF);
            rgb[index * 3 + 2] = static_cast<std::uint8_t>((x * 3 + y * 5) & 0xFF);
            gray[index] = static_cast<std::uint8_t>((x * 7 + y * 5) & 0xFF);
        }
    }

    const auto grayscale = Decoder::Jpeg::Encode(gray, width, height, 1, 90, Decoder::Jpeg::Sampling::Full);
    Require(IsJpeg(grayscale));
    const auto grayscaleHeader = FrameHeader(grayscale);
    Require(grayscaleHeader[5] == 1);
    Require(grayscaleHeader[7] == 0x11);
    const auto decodedGrayscale = Decoder::Jpeg::Decode(grayscale);
    Require(decodedGrayscale.has_value() && decodedGrayscale->channels == 1);
    Require(MaximumDifference(decodedGrayscale->pixels, gray) < 24);

    const auto full = Decoder::Jpeg::Encode(rgb, width, height, 3, 90, Decoder::Jpeg::Sampling::Full);
    const auto half = Decoder::Jpeg::Encode(rgb, width, height, 3, 90, Decoder::Jpeg::Sampling::Ycc422);
    const auto quarter = Decoder::Jpeg::Encode(rgb, width, height, 3, 90, Decoder::Jpeg::Sampling::Ycc420);
    Require(IsJpeg(full) && IsJpeg(half) && IsJpeg(quarter));
    Require(FrameHeader(full)[5] == 3 && FrameHeader(full)[7] == 0x11);
    Require(FrameHeader(half)[7] == 0x21);
    Require(FrameHeader(quarter)[7] == 0x22);
    Require(FrameHeader(half)[10] == 0x11 && FrameHeader(half)[13] == 0x11);

    const auto decodedFull = Decoder::Jpeg::Decode(full);
    const auto decodedHalf = Decoder::Jpeg::Decode(half);
    const auto decodedQuarter = Decoder::Jpeg::Decode(quarter);
    Require(decodedFull.has_value() && decodedHalf.has_value() && decodedQuarter.has_value());
    Require(decodedFull->channels == 3 && decodedHalf->channels == 3 && decodedQuarter->channels == 3);
    Require(decodedFull->width == width && decodedHalf->height == height);
    Require(MaximumDifference(decodedFull->pixels, rgb) <= MaximumDifference(decodedHalf->pixels, rgb));
    Require(MaximumDifference(decodedHalf->pixels, rgb) <= MaximumDifference(decodedQuarter->pixels, rgb));
    Require(quarter.size() < half.size() && half.size() < full.size());
}

int main() {
    CheckSampling();
    constexpr std::uint32_t width = 32;
    constexpr std::uint32_t height = 24;

    std::vector<std::uint8_t> rgb(width * height * 3);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t* pixel = &rgb[(y * width + x) * 3];
            pixel[0] = static_cast<std::uint8_t>(x * 8);
            pixel[1] = static_cast<std::uint8_t>(y * 10);
            pixel[2] = 128;
        }
    }

    const std::vector<std::uint8_t> rgbJpeg = Decoder::Jpeg::Encode(rgb, width, height, 3, 90);
    Require(IsJpeg(rgbJpeg));
    const auto rgbImage = Decoder::Jpeg::Decode(rgbJpeg);
    Require(rgbImage.has_value());
    Require(rgbImage->width == width && rgbImage->height == height && rgbImage->channels == 3);
    Require(rgbImage->pixels.size() == rgb.size());
    long totalError = 0;
    for (std::size_t i = 0; i < rgb.size(); ++i) totalError += Difference(rgb[i], rgbImage->pixels[i]);
    Require(totalError / static_cast<long>(rgb.size()) < 4);

    std::vector<std::uint8_t> gray(width * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) gray[y * width + x] = static_cast<std::uint8_t>((x + y) * 4);
    }
    const std::vector<std::uint8_t> grayJpeg = Decoder::Jpeg::Encode(gray, width, height, 1, 90);
    Require(IsJpeg(grayJpeg));
    const auto grayImage = Decoder::Jpeg::Decode(grayJpeg);
    Require(grayImage.has_value());
    Require(grayImage->width == width && grayImage->height == height);
    totalError = 0;
    for (std::size_t i = 0; i < gray.size(); ++i) totalError += Difference(gray[i], grayImage->pixels[i * grayImage->channels]);
    Require(totalError / static_cast<long>(gray.size()) < 4);

    Require(ThrowsInvalidArgument([&] { Decoder::Jpeg::Encode(rgb, width, height, 4, 90); }));
    Require(ThrowsInvalidArgument([&] { Decoder::Jpeg::Encode(rgb, width, height, 3, 0); }));
    Require(ThrowsInvalidArgument([&] { Decoder::Jpeg::Encode(rgb, width, height, 3, 101); }));
    Require(ThrowsInvalidArgument([&] { Decoder::Jpeg::Encode(rgb, 0, height, 3, 90); }));
    Require(ThrowsInvalidArgument([&] { Decoder::Jpeg::Encode(rgb, 0x10000, 1, 3, 90); }));
    Require(ThrowsInvalidArgument([&] { Decoder::Jpeg::Encode(rgb, width, height + 1, 3, 90); }));

    Require(!Decoder::Jpeg::Decode({}).has_value());
    const std::vector<std::uint8_t> garbage{1, 2, 3, 4, 5, 6, 7, 8};
    Require(!Decoder::Jpeg::Decode(garbage).has_value());

    return 0;
}
