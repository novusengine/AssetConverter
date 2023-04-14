#include "ComplexModelExtractor.h"
#include "AssetConverter/Runtime.h"
#include "AssetConverter/Casc/CascLoader.h"
#include "AssetConverter/Util/ServiceLocator.h"

#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <FileFormat/Novus/Model/ComplexModel.h>
#include <FileFormat/Warcraft/Shared.h>
#include <FileFormat/Warcraft/M2/M2.h>
#include <FileFormat/Warcraft/Parsers/M2Parser.h>

#include <filesystem>
namespace fs = std::filesystem;

void ComplexModelExtractor::Process()
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
		if (!StringUtils::EndsWith(itr.first, ".m2"))
			continue;

		if (!cascLoader->FileExistsInCasc(itr.second))
			continue;

		std::string pathStr = itr.first;
		std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::tolower);

		fs::path outputPath = (runtime->paths.complexModel / pathStr).replace_extension("complexmodel");
		fs::create_directories(outputPath.parent_path());

		FileListEntry& fileListEntry = fileList.emplace_back();
		fileListEntry.fileID = itr.second;
		fileListEntry.fileName = outputPath.filename().string();
		fileListEntry.path = outputPath.string();
	}

	std::mutex printMutex;
	u32 numFiles = static_cast<u32>(fileList.size());
	u32 numProcessedFiles = 0;
	u16 progressFlags = 0;
	DebugHandler::Print("[ComplexModel Extractor] Processing {0} files", numFiles);

	enki::TaskSet convertM2Task(numFiles, [&runtime, &cascLoader, &fileList, &numProcessedFiles, &progressFlags, &printMutex, numFiles](enki::TaskSetPartition range, uint32_t threadNum)
	{
		M2::Parser m2Parser = {};
		for (u32 index = range.start; index < range.end; index++)
		{
			const FileListEntry& fileListEntry = fileList[index];

			std::shared_ptr<Bytebuffer> rootBuffer = cascLoader->GetFileByID(fileListEntry.fileID);
			if (!rootBuffer || rootBuffer->size == 0 || rootBuffer->writtenData == 0)
				continue;

			M2::Layout m2 = { };
			if (!m2Parser.TryParse(M2::Parser::ParseType::Root, rootBuffer, m2))
				continue;

			std::shared_ptr<Bytebuffer> skinBuffer = cascLoader->GetFileByID(m2.sfid.skinFileIDs[0]);
			if (!skinBuffer || skinBuffer->size == 0 || skinBuffer->writtenData == 0)
				continue;

			if (!m2Parser.TryParse(M2::Parser::ParseType::Skin, skinBuffer, m2))
				continue;

			Model::ComplexModel cmodel = { };
			if (!Model::ComplexModel::FromM2(rootBuffer, skinBuffer, m2, cmodel))
				continue;

			// Post Process
			{
				for (u32 i = 0; i < cmodel.textures.size(); i++)
				{
					Model::ComplexModel::Texture& texture = cmodel.textures[i];

					u32 fileID = texture.textureHash; // This has not been converted to a textureHash yet.
					texture.textureHash = std::numeric_limits<u32>().max(); // Default to invalid

					if (fileID == 0 || fileID == std::numeric_limits<u32>().max())
						continue;

					const std::string& cascFilePath = cascLoader->GetFilePathFromListFileID(fileID);
					if (cascFilePath.size() == 0)
						continue;

					fs::path texturePath = cascFilePath;
					texturePath.replace_extension("dds").make_preferred();

					std::string textureName = texturePath.string();
					std::transform(textureName.begin(), textureName.end(), textureName.begin(), ::tolower);

					texture.textureHash = StringUtils::fnv1a_32(textureName.c_str(), textureName.length());
				}
			}

			bool result = cmodel.Save(fileListEntry.path);
			if (runtime->isInDebugMode)
			{
				if (result)
				{
					DebugHandler::Print("[ComplexModel Extractor] Extracted {0}", fileListEntry.fileName);
				}
				else
				{
					DebugHandler::PrintWarning("[ComplexModel Extractor] Failed to extract {0}", fileListEntry.fileName);
				}
			}

			{
				std::scoped_lock scopedLock(printMutex);
				
				f32 processedFiles = static_cast<f32>(++numProcessedFiles);
				f32 progress = (processedFiles / static_cast<f32>(numFiles - 1)) * 10.0f;
				u32 bitToCheck = static_cast<u32>(progress);
				u32 bitMask = 1 << bitToCheck;
				
				bool reportStatus = (progressFlags & bitMask) == 0;
				if (reportStatus)
				{
					progressFlags |= bitMask;
					DebugHandler::Print("[ComplexModel Extractor] Progress Status ({0:.0f}% / 100%)", progress * 10.0f);
				}
			}
		}
	});

	convertM2Task.m_Priority = enki::TaskPriority::TASK_PRIORITY_HIGH;
	runtime->scheduler.AddTaskSetToPipe(&convertM2Task);
	runtime->scheduler.WaitforTask(&convertM2Task);
}