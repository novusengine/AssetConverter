#include "TextureExtractor.h"
#include "AssetConverter/Runtime.h"
#include "AssetConverter/Blp/BlpConvert.h"
#include "AssetConverter/Casc/CascLoader.h"
#include "AssetConverter/Util/ServiceLocator.h"

#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <enkiTS/TaskScheduler.h>

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

		struct Flags
		{
			u8 isInterfaceFile : 1;
			u8 useCompression : 1;
		};

		Flags flags;
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
	
		if (fs::exists(outputPath))
			continue;
	
		fs::create_directories(outputPath.parent_path());
	
		FileListEntry& fileListEntry = fileList.emplace_back();
		fileListEntry.fileID = itr.second;
		fileListEntry.fileName = outputPath.filename().string();
		fileListEntry.path = outputPath.string();
		fileListEntry.flags.isInterfaceFile = StringUtils::BeginsWith(pathStr, "interface");
		fileListEntry.flags.useCompression = !fileListEntry.flags.isInterfaceFile;
	}

	BLP::BlpConvert blpConvert;
	u32 numFiles = static_cast<u32>(fileList.size());
	std::atomic<u32> numFilesConverted = 0;
	std::atomic<u16> progressFlags = 0;
	DebugHandler::Print("[Texture Extractor] Processing {0} files", numFiles);

	enki::TaskSet convertTexturesTask(numFiles, [&](enki::TaskSetPartition range, uint32_t threadNum)
	{
		for (u32 i = range.start; i < range.end; i++)
		{
			const FileListEntry& fileListEntry = fileList[i];

			std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByID(fileListEntry.fileID);
			if (!buffer)
				return;

			bool generateMips = !fileListEntry.flags.isInterfaceFile;
			bool useCompression = fileListEntry.flags.useCompression;
			blpConvert.ConvertBLP(buffer->GetDataPointer(), buffer->writtenData, fileListEntry.path, generateMips, useCompression, ivec2(256, 256));

			f32 progress = (static_cast<f32>(numFilesConverted++) / static_cast<f32>(numFiles - 1)) * 10.0f;
			u32 bitToCheck = static_cast<u32>(progress);
			u32 bitMask = 1u << bitToCheck;

			bool reportStatus = (progressFlags & bitMask) == 0;
			if (reportStatus)
			{
				progressFlags |= bitMask;
				DebugHandler::Print("[Texture Extractor] Progress Status ({0:.0f}% / 100%)", progress * 10.0f);
			}
		}
	});

	runtime->scheduler.AddTaskSetToPipe(&convertTexturesTask);
	runtime->scheduler.WaitforTask(&convertTexturesTask);
}