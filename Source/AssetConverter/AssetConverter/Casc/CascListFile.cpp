#include "CascListFile.h"

#include <Base/Memory/FileReader.h>
#include <Base/Util/StringUtils.h>

#include <filesystem>
namespace fs = std::filesystem;

bool CascListFile::Initialize()
{
	fs::path listFilePath = _listPath;
	if (!fs::exists(listFilePath))
		return false;

	const std::string pathAsString = listFilePath.string();

	FileReader reader(pathAsString);
	if (!reader.Open())
		return false;

	_fileBuffer = new Bytebuffer(nullptr, reader.Length());
	reader.Read(_fileBuffer, _fileBuffer->size);

	ParseListFile();

	return true;
}

void CascListFile::ParseListFile()
{
	char* buffer = reinterpret_cast<char*>(_fileBuffer->GetDataPointer());
	size_t bufferSize = _fileBuffer->size;

	_fileIDToPath.reserve(2000000);
	_filePathToID.reserve(2000000);

	_m2Files.reserve(65535);
	_wmoFiles.reserve(32768);
	_blpFiles.reserve(262144);

	while (_fileBuffer->GetReadSpace())
	{
		u32 fileID = 0;
		std::string filePath = "";

		// Read fileID
		{
			size_t numberStartIndex = _fileBuffer->readData;

			char c = 0;
			for (size_t i = numberStartIndex; i < bufferSize; i++)
			{
				if (!_fileBuffer->Get<char>(c))
					break;

				if (c == ';')
					break;
			}

			size_t numberEndIndex = _fileBuffer->readData - 1;
			std::string numberStr = std::string(&buffer[numberStartIndex], (numberEndIndex - numberStartIndex));
			fileID = std::stoi(numberStr);
		}

		// Read filePath
		{
			size_t pathStartIndex = _fileBuffer->readData;
			u8 numLineEndingSymbols = 0;

			char c = 0;
			for (size_t i = pathStartIndex; i < bufferSize; i++)
			{
				if (!_fileBuffer->Get<char>(c))
					break;

				if (c == '\r' || c == '\n')
				{
					numLineEndingSymbols++;
					break;
                }
			}

			// Skip any \r or \n
			char currentData = *reinterpret_cast<char*>(_fileBuffer->GetReadPointer());
			if (currentData == '\r' || currentData == '\n')
			{
				numLineEndingSymbols++;
				_fileBuffer->SkipRead(1);
			}

			size_t pathEndIndex = _fileBuffer->readData - numLineEndingSymbols;
			size_t strSize = pathEndIndex - pathStartIndex;
			filePath = std::string(&buffer[pathStartIndex], strSize);
		}

		_fileIDToPath[fileID] = filePath;
		_filePathToID[filePath] = fileID;

		if (StringUtils::EndsWith(filePath, ".m2") || StringUtils::EndsWith(filePath, ".mdx"))
		{
			_m2Files.push_back(fileID);
		}
		else if (StringUtils::EndsWith(filePath, ".wmo"))
		{
			_wmoFiles.push_back(fileID);
		}
		else if (StringUtils::EndsWith(filePath, ".blp"))
		{
			_blpFiles.push_back(fileID);
		}
	}
}