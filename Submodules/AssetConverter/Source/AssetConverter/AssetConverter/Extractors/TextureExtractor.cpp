#include "TextureExtractor.h"
#include "AssetConverter/Runtime.h"
#include "AssetConverter/Blp/BlpConvert.h"
#include "AssetConverter/Casc/CascLoader.h"
#include "AssetConverter/Util/ServiceLocator.h"

#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <filesystem>
namespace fs = std::filesystem;

void TextureExtractor::Process()
{
    Runtime* runtime = ServiceLocator::GetRuntime();
    CascLoader* cascLoader = ServiceLocator::GetCascLoader(); 
	
	const CascListFile& listFile = cascLoader->GetListFile();
	const robin_hood::unordered_map<std::string, u32>& filePathToIDMap = listFile.GetFilePathToIDMap();

	struct FileListEntry
	{
		u32 fileID = 0;
		std::string fileName;
		std::string path;
	};

	std::vector<FileListEntry> fileList = { };
	fileList.reserve(filePathToIDMap.size());

	for (auto& itr : filePathToIDMap)
	{
		if (!StringUtils::EndsWith(itr.first, ".blp"))
			continue;

		if (!cascLoader->FileExistsInCasc(itr.second))
			continue;

		std::string pathStr = itr.first;
		std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::tolower);

		fs::path outputPath = (runtime->paths.texture / pathStr).replace_extension("dds");
		fs::create_directories(outputPath.parent_path());

		FileListEntry& fileListEntry = fileList.emplace_back();
		fileListEntry.fileID = itr.second;
		fileListEntry.fileName = outputPath.filename().string();
		fileListEntry.path = outputPath.string();
	}

	u32 numFiles = static_cast<u32>(fileList.size());
	u16 progressFlags = 0;
	DebugHandler::Print("[Texture Extractor] Processing %u files", numFiles);

	BLP::BlpConvert blpConvert;
	for (u32 i = 0; i < numFiles; i++)
	{
		const FileListEntry& fileListEntry = fileList[i];

		std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByID(fileListEntry.fileID);
		if (!buffer)
			continue;

		blpConvert.ConvertBLP(buffer->GetDataPointer(), buffer->writtenData, fileListEntry.path, true);

		f32 progress = (static_cast<f32>(i) / static_cast<f32>(numFiles - 1)) * 10.0f;
		u32 bitToCheck = static_cast<u32>(progress);
		u32 bitMask = 1 << bitToCheck;

		bool reportStatus = (progressFlags & bitMask) == 0;
		if (reportStatus)
		{
			progressFlags |= bitMask;
			DebugHandler::Print("[Texture Extractor] Progress Status ({0:.2f}% / 100%)", progress * 10.0f);
		}
	}
}