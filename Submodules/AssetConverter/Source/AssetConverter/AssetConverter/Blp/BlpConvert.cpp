#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>
#include "BlpConvert.h"
#include "BlpConvertException.h"
#include <cassert>

#include <cuttlefish/Image.h>
#include <cuttlefish/Texture.h>

namespace BLP
{
	namespace _detail
	{
		static const uint32_t alphaLookup1[] = { 0x00, 0xFF };
		static const uint32_t alphaLookup4[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };

		static void rgb565ToRgb8Array(const uint32_t& input, uint8_t* output)
		{
			uint32_t r = (uint32_t)(input & 0x1Fu);
			uint32_t g = (uint32_t)((input >> 5u) & 0x3Fu);
			uint32_t b = (uint32_t)((input >> 11u) & 0x1Fu);

			r = (r << 3u) | (r >> 2u);
			g = (g << 2u) | (g >> 4u);
			b = (b << 3u) | (b >> 2u);

			output[0] = (uint8_t)b;
			output[1] = (uint8_t)g;
			output[2] = (uint8_t)r;
		}
	}

	void BlpConvert::ConvertBLP(unsigned char* inputBytes, std::size_t size, const std::string& outputPath, bool generateMipmaps)
	{
		ByteStream stream(inputBytes, size);
		BlpHeader header = stream.read<BlpHeader>();

		// Sanity Check : Ensure the stream contains a proper BLP header
		if (header.signature != static_cast<int>('BLP2') && header.version != 1)
			return;

		cuttlefish::Texture::Format textureFormat = cuttlefish::Texture::Format::R32G32B32;
		Format format = GetFormat(header);

		// TODO : Pursche needs to figure out how to read BC5.
		if (format == Format::BC5)
			return;

		// Santiy Check : Ensure we don't try to read BLP files with no content.
		if (header.sizes[0] == 0)
			return;

		std::vector<uint32_t> imageData;
		LoadFirstLayer(header, stream, imageData);

		if (format == Format::BC1)
		{
			if (header.alphaDepth == 1)
			{
				textureFormat = cuttlefish::Texture::Format::BC1_RGBA;
			}
			else
			{
				textureFormat = cuttlefish::Texture::Format::BC1_RGB;
			}
		}
		else if (format == Format::BC2)
		{
			textureFormat = cuttlefish::Texture::Format::BC2;
		}
		else if (format == Format::BC3)
		{
			textureFormat = cuttlefish::Texture::Format::BC3;
		}
		else
		{
			if (header.alphaDepth > 0)
			{
				textureFormat = cuttlefish::Texture::Format::BC3;
			}
			else
			{
				textureFormat = cuttlefish::Texture::Format::BC1_RGB;
			}
		}

		cuttlefish::Image image;
		if (!image.initialize(cuttlefish::Image::Format::RGBA8, header.width, header.height))
			return;

		for (int y = 0; y < header.height; y++)
		{
			for (int x = 0; x < header.width; x++)
			{
				int pixelID = x + (y * header.width);
				uint32_t pixelColor = imageData[pixelID];

				cuttlefish::ColorRGBAd color;
				color.r = ((pixelColor >> 16) & 0xFF) / 255.0f;
				color.g = ((pixelColor >> 8) & 0xFF) / 255.0f;
				color.b = (pixelColor & 0xFF) / 255.0f;
				color.a = ((pixelColor >> 24) & 0xFF) / 255.0f;

				image.setPixel(y, x, color);
			}
		}

		cuttlefish::Texture texture(cuttlefish::Texture::Dimension::Dim2D, header.width, header.height);
		if (!texture.setImage(image))
			return;

		if (!texture.convert(textureFormat, cuttlefish::Texture::Type::UNorm))
			return;

		texture.save(outputPath.c_str(), cuttlefish::Texture::FileType::DDS);
	}

	cuttlefish::Image::Format GetInputFormat(const InputFormat& inputFormat)
	{
		switch (inputFormat)
		{
			case InputFormat::BGRA_8UB: return cuttlefish::Image::Format::RGBA8;
			case InputFormat::RGBA_32F: return cuttlefish::Image::Format::RGBAF;
			case InputFormat::R_32F:    return cuttlefish::Image::Format::Float;
			default: assert(false);
		};

		return cuttlefish::Image::Format::RGBA8;
	}

	cuttlefish::Texture::Format GetOutputFormat(Format format)
	{
		switch (format)
		{
			case Format::RGB:  return cuttlefish::Texture::Format::R8G8B8;
			case Format::RGBA: return cuttlefish::Texture::Format::R8G8B8A8;
			case Format::BC1:  return cuttlefish::Texture::Format::BC1_RGB;
			case Format::BC2:  return cuttlefish::Texture::Format::BC2;
			case Format::BC3:  return cuttlefish::Texture::Format::BC3;
			default: assert(false);
		}
		return cuttlefish::Texture::Format::R8G8B8;
	}

	void BlpConvert::ConvertRaw(uint32_t width, uint32_t height, uint32_t layers, unsigned char* inputBytes, std::size_t size, InputFormat inputFormat, Format outputFormat, const std::string& outputPath, bool generateMipmaps)
	{
		cuttlefish::Image::Format cuttleFishInputFormat = GetInputFormat(inputFormat);
		cuttlefish::Texture::Format cuttleFishOutputFormat = GetOutputFormat(outputFormat);

		cuttlefish::Texture::Dimension dimension = cuttlefish::Texture::Dimension::Dim2D;
		if (layers != 1)
		{
			dimension = cuttlefish::Texture::Dimension::Dim3D;
		}

		cuttlefish::Image image;
		if (!image.initialize(cuttleFishInputFormat, width, height))
			return;

		cuttlefish::Texture texture(dimension, width, height, layers);

		for (int layer = 0; layer < layers; layer++)
		{
			for (int y = 0; y < height; y++)
			{
				for (int x = 0; x < width; x++)
				{
					int pixelID = (x + (y * width)) + (layer * (height * width));
					uint32_t pixelColor = reinterpret_cast<uint32_t*>(inputBytes)[pixelID];

					cuttlefish::ColorRGBAd color;
					color.r = ((pixelColor >> 16) & 0xFF) / 255.0f;
					color.g = ((pixelColor >> 8) & 0xFF) / 255.0f;
					color.b = (pixelColor & 0xFF) / 255.0f;
					color.a = ((pixelColor >> 24) & 0xFF) / 255.0f;

					image.setPixel(x, y, color);
				}
			}

			if (!texture.setImage(image, 0, layer))
				return;
		}

		if (generateMipmaps)
		{
			texture.generateMipmaps();
		}

		if (!texture.convert(cuttleFishOutputFormat, cuttlefish::Texture::Type::UNorm))
			return;

		texture.save(outputPath.c_str(), cuttlefish::Texture::FileType::DDS);
	}

	void BlpConvert::LoadFirstLayer(const BlpHeader& header, ByteStream& data, std::vector<uint32_t>& imageData) const
	{
		Format format = GetFormat(header);
		if (format == UNKNOWN)
		{
			throw BlpConvertException("Unable to determine format");
		}

		//uint32_t size = header.sizes[0];
		uint32_t offset = header.offsets[0];
		data.setPosition(offset);

		switch (format)
		{
			case RGB:
				ParseUncompressed(header, data, imageData);
				break;

			case RGB_PALETTE:
				ParseUncompressedPalette(header, data, imageData);
				break;

			case BC1:
			case BC2:
			case BC3:
				ParseCompressed(header, data, imageData);
				break;

			default:
				throw BlpConvertException("Unsupported format of BLP");
		}
	}

	Format BlpConvert::GetFormat(const BlpHeader& header) const
	{
		switch (header.compression)
		{
			case 1:
				return RGB_PALETTE;
			case 2:
			{
				switch (header.alphaCompression)
				{
				case 0:
					return BC1;
				case 1:
					return BC2;
				case 7:
					return BC3;
				case 11:
					return BC5;
				default:
					return UNKNOWN;
				}
			}
			case 3:
				return RGB;
			default:
				return UNKNOWN;
		}
	}

	void BlpConvert::ParseUncompressed(const BlpHeader& header, ByteStream& data, std::vector<uint32_t>& imageData) const
	{
		uint32_t rowPitch = header.width;
		uint32_t numRows = header.height;
		uint32_t numEntries = rowPitch * numRows;

		imageData.resize(numEntries);
		data.read(imageData.data(), imageData.size() * sizeof(uint32_t));

		//Reverse byteorder of imageData to go from ARGB (which Blizzard stores) to BGRA (which nvtt expects)
		for (uint32_t i = 0; i < numEntries; i++)
		{
			SwapByteOrder(imageData[i]);
		}
	}

	void BlpConvert::ParseUncompressedPalette(const BlpHeader& header, ByteStream& data, std::vector<uint32_t>& imageData) const
	{
		uint32_t palette[256] = { 0 };
		uint64_t curPosition = (uint64_t)data.getPosition();
		data.setPosition(sizeof(BlpHeader));
		data.read(palette, sizeof(palette));
		data.setPosition(curPosition);

		//Reverse byteorder of palette to go from ARGB (which Blizzard stores) to BGRA (which nvtt expects)
		for (uint32_t i = 0; i < 256; i++)
		{
			SwapByteOrder(palette[i]);
		}

		std::vector<uint8_t> indices(header.sizes[0]);
		data.read(indices.data(), header.sizes[0]);

		if (header.alphaDepth == 8)
		{
			DecompressPaletteFastPath(header, palette, indices, imageData);
		}
		else
		{
			DecompressPaletteARGB8(header, palette, indices, imageData);
		}
	}

	void BlpConvert::DecompressPaletteFastPath(const BlpHeader& header, const uint32_t* const palette, const std::vector<uint8_t>& indices, std::vector<uint32_t>& imageData) const
	{
		uint32_t w = header.width;
		uint32_t h = header.height;
		uint32_t numEntries = w * h;

		imageData.resize(numEntries);

		uint32_t counter = 0u;
		for (uint32_t y = 0u; y < h; ++y)
		{
			for (uint32_t x = 0u; x < w; ++x)
			{
				uint8_t index = indices[counter];
				uint8_t alpha = indices[numEntries + counter];
				uint32_t color = palette[index];
				color = (color & 0x00FFFFFFu) | (((uint32_t)alpha) << 24u);
				imageData[counter++] = color;
			}
		}
	}


	void BlpConvert::DecompressPaletteARGB8(const BlpHeader& header, const uint32_t* palette, const std::vector<uint8_t>& indices, std::vector<uint32_t>& imageData) const
	{
		uint32_t w = header.width;
		uint32_t h = header.height;
		uint32_t numEntries = w * h;

		imageData.resize(numEntries);

		for (uint32_t i = 0u; i < numEntries; ++i)
		{
			uint8_t index = indices[i];
			uint32_t color = palette[index];
			color = (color & 0x00FFFFFFu) | 0xFF000000u;
			imageData[i] = color;
		}

		switch (header.alphaDepth)
		{
			case 0:
				break;

			case 1:
			{
				uint32_t colorIndex = 0u;
				for (uint32_t i = 0u; i < (numEntries / 8u); ++i)
				{
					uint8_t value = indices[i + numEntries];
					for (uint32_t j = 0u; j < 8; ++j, ++colorIndex)
					{
						uint32_t& color = imageData[colorIndex];
						color &= 0x00FFFFFF;
						color |= _detail::alphaLookup1[(((value & (1u << j))) != 0) ? 1 : 0] << 24u;
					}
				}

				if ((numEntries % 8) != 0)
				{
					uint8_t value = indices[numEntries + numEntries / 8];
					for (uint32_t j = 0u; j < (numEntries % 8); ++j, ++colorIndex)
					{
						uint32_t& color = imageData[colorIndex];
						color &= 0x00FFFFFF;
						color |= _detail::alphaLookup1[(((value & (1u << j))) != 0) ? 1 : 0] << 24u;
					}
				}

				break;
			}

			case 4:
			{
				uint32_t colorIndex = 0u;
				for (uint32_t i = 0u; i < (numEntries / 2u); ++i)
				{
					uint8_t value = indices[i + numEntries];
					uint8_t alpha0 = _detail::alphaLookup4[value & 0x0Fu];
					uint8_t alpha1 = _detail::alphaLookup4[value >> 4u];
					uint32_t& color1 = imageData[colorIndex++];
					uint32_t& color2 = imageData[colorIndex++];
					color1 = (color1 & 0x00FFFFFFu) | (alpha0 << 24u);
					color2 = (color2 & 0x00FFFFFFu) | (alpha1 << 24u);
				}

				if ((numEntries % 2) != 0)
				{
					uint8_t value = indices[numEntries + numEntries / 2];
					uint8_t alpha = _detail::alphaLookup4[value & 0x0Fu];
					uint32_t& color = imageData[colorIndex];
					color = (color & 0x00FFFFFFu) | (alpha << 24u);
				}

				break;
			}

			default:
				throw BlpConvertException("Unsupported alpha depth");
		}
	}


	void BlpConvert::ParseCompressed(const BlpHeader& header, ByteStream& data, std::vector<uint32_t>& imageData) const
	{
		Format format = GetFormat(header);

		uint32_t w = header.width;
		uint32_t h = header.height;
		uint32_t numEntries = w * h;

		imageData.resize(numEntries);

		uint32_t numBlocks = ((w + 3u) / 4u) * ((h + 3u) / 4u);
		std::vector<uint32_t> blockData(numBlocks * 16u);
		tConvertFunction converter = GetDxtConvertFunction(format);
		for (uint32_t i = 0u; i < numBlocks; ++i)
		{
			(this->*converter)(data, blockData, std::size_t(i * 16));
		}

		uint32_t count = 0;
		std::vector<uint32_t> rowBuffer(w);
		for (uint32_t y = 0u; y < h; ++y)
		{
			for (uint32_t x = 0u; x < w; ++x)
			{
				uint32_t bx = x / 4u;
				uint32_t by = y / 4u;

				uint32_t ibx = x % 4u;
				uint32_t iby = y % 4u;

				uint32_t blockIndex = by * ((w + 3u) / 4u) + bx;
				uint32_t innerIndex = iby * 4u + ibx;
				imageData[count++] = blockData[blockIndex * 16u + innerIndex];
			}
		}

		//Reverse byteorder of imageData to go from ARGB (which Blizzard stores) to BGRA (which nvtt expects)
		for (uint32_t i = 0; i < numEntries; i++)
		{
			SwapByteOrder(imageData[i]);
		}
	}

	BlpConvert::tConvertFunction BlpConvert::GetDxtConvertFunction(const Format& format) const
	{
		switch (format)
		{
			case BC1: return &BlpConvert::Dxt1GetBlock;
			case BC2: return &BlpConvert::Dxt3GetBlock;
			case BC3: return &BlpConvert::Dxt5GetBlock;
			default: throw BlpConvertException("Unrecognized dxt format");
		}
	}

	void BlpConvert::Dxt1GetBlock(ByteStream& stream, std::vector<uint32_t>& blockData, const size_t& blockOffset) const
	{
		_detail::RgbDataArray colors[4];
		ReadDXTColors(stream, colors, true);

		uint32_t indices = stream.read<uint32_t>();
		for (uint32_t i = 0u; i < 16u; ++i)
		{
			uint8_t idx = (uint8_t)((indices >> (2u * i)) & 3u);
			blockData[blockOffset + i] = colors[idx].data.color;
		}
	}

	void BlpConvert::Dxt3GetBlock(ByteStream& stream, std::vector<uint32_t>& blockData, const size_t& blockOffset) const
	{
		uint8_t alphaValues[16];
		uint64_t alpha = stream.read<uint64_t>();
		for (uint32_t i = 0u; i < 16u; ++i)
		{
			alphaValues[i] = (uint8_t)(((alpha >> (4u * i)) & 0x0Fu) * 17);
		}

		_detail::RgbDataArray colors[4];
		ReadDXTColors(stream, colors, false, true);

		uint32_t indices = stream.read<uint32_t>();
		for (uint32_t i = 0u; i < 16u; ++i)
		{
			uint8_t idx = (uint8_t)((indices >> (2u * i)) & 3u);
			uint32_t alphaVal = (uint32_t)alphaValues[i];
			blockData[blockOffset + i] = (colors[idx].data.color & 0x00FFFFFFu) | (alphaVal << 24u);
		}
	}

	void BlpConvert::Dxt5GetBlock(ByteStream& stream, std::vector<uint32_t>& blockData, const size_t& blockOffset) const
	{
		uint8_t alphaValues[8];
		uint8_t alphaLookup[16];

		uint32_t alpha1 = (uint32_t)stream.read<uint8_t>();
		uint32_t alpha2 = (uint32_t)stream.read<uint8_t>();

		alphaValues[0] = (uint8_t)alpha1;
		alphaValues[1] = (uint8_t)alpha2;

		if (alpha1 > alpha2)
		{
			for (uint32_t i = 0u; i < 6u; ++i)
			{
				alphaValues[i + 2u] = (uint8_t)(((6u - i) * alpha1 + (1u + i) * alpha2) / 7u);
			}
		}
		else
		{
			for (uint32_t i = 0u; i < 4u; ++i)
			{
				alphaValues[i + 2u] = (uint8_t)(((4u - i) * alpha1 + (1u + i) * alpha2) / 5u);
			}

			alphaValues[6] = 0;
			alphaValues[7] = 255;
		}

		uint64_t lookupValue = 0;
		stream.read(&lookupValue, 6);

		for (uint32_t i = 0u; i < 16u; ++i)
		{
			alphaLookup[i] = (uint8_t)((lookupValue >> (i * 3u)) & 7u);
		}

		_detail::RgbDataArray colors[4];
		ReadDXTColors(stream, colors, false);

		uint32_t indices = stream.read<uint32_t>();
		for (uint32_t i = 0u; i < 16u; ++i)
		{
			uint8_t idx = (uint8_t)((indices >> (2u * i)) & 3u);
			uint32_t alphaVal = (uint32_t)alphaValues[alphaLookup[i]];
			blockData[blockOffset + i] = (colors[idx].data.color & 0x00FFFFFFu) | (alphaVal << 24u);
		}
	}

	void BlpConvert::ReadDXTColors(ByteStream& stream, _detail::RgbDataArray* colors, bool preMultipliedAlpha, bool use4Colors) const
	{
		uint16_t color1 = stream.read<uint16_t>();
		uint16_t color2 = stream.read<uint16_t>();

		_detail::rgb565ToRgb8Array(color1, colors[0].data.buffer);
		_detail::rgb565ToRgb8Array(color2, colors[1].data.buffer);

		colors[0].data.buffer[3] = 0xFFu;
		colors[1].data.buffer[3] = 0xFFu;
		colors[2].data.buffer[3] = 0xFFu;
		colors[3].data.buffer[3] = 0xFFu;

		if (use4Colors || color1 > color2)
		{
			for (uint32_t i = 0u; i < 3u; ++i)
			{
				colors[3].data.buffer[i] = (uint8_t)((colors[0].data.buffer[i] + 2u * colors[1].data.buffer[i]) / 3u);
				colors[2].data.buffer[i] = (uint8_t)((2u * colors[0].data.buffer[i] + colors[1].data.buffer[i]) / 3u);
			}
		}
		else
		{
			for (uint32_t i = 0u; i < 3u; ++i)
			{
				colors[2].data.buffer[i] = (uint8_t)((colors[0].data.buffer[i] + colors[1].data.buffer[i]) / 2u);
				colors[3].data.buffer[i] = 0;
			}

			if (preMultipliedAlpha)
			{
				colors[3].data.buffer[3] = 0;
			}
		}
	}

	void BlpConvert::SwapByteOrder(uint32_t& ui) const
	{
		uint8_t* b = reinterpret_cast<uint8_t*>(&ui);

		// RGBA
		uint8_t temp = b[0]; // Temp contains R
		b[0] = b[2]; // BGBA
		b[2] = temp; // BGRA
	}
}
