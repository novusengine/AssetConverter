#include "TextureExtractor.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Blp/BlpConvert.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/ServiceLocator.h"

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
    
        if (!cascLoader->InCascAndListFile(itr.second))
            continue;
    
        std::string pathStr = itr.first;
        std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::tolower);
    
        fs::path outputPath = fs::path("texture") / pathStr;
        outputPath.replace_extension("dds");

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
    NC_LOG_INFO("[Texture Extractor] Processing {0} files", numFiles);

    enki::TaskSet convertTexturesTask(numFiles, [&](enki::TaskSetPartition range, uint32_t threadNum)
    {
        std::vector<u8> outBytes;

        for (u32 i = range.start; i < range.end; i++)
        {
            const FileListEntry& fileListEntry = fileList[i];

            std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByID(fileListEntry.fileID);
            if (!buffer)
            {
                runtime->pactInfo.MarkFailed();
                NC_LOG_ERROR("[Texture Extractor] Failed to load {0} from CASC", fileListEntry.path);
            }
            else
            {
                bool generateMips = !fileListEntry.flags.isInterfaceFile;
                bool useCompression = fileListEntry.flags.useCompression;

                outBytes.clear();
                outBytes.reserve(buffer->writtenData);
                if (blpConvert.ConvertBLPToBuffer(buffer->GetDataPointer(), buffer->writtenData, outBytes, generateMips, useCompression, ivec2(256, 256)))
                {
                    std::string textureName = fileListEntry.path;
                    std::transform(textureName.begin(), textureName.end(), textureName.begin(), ::tolower);
                    std::replace(textureName.begin(), textureName.end(), '\\', '/');

                    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, outBytes.size());
                    if (!manifest.AddFile(runtime, textureName, outBytes))
                    {
                        NC_LOG_WARNING("[Texture Extractor] Failed to add {0} to PACT storage", textureName);
                    }
                }
                else
                {
                    // runtime->pactInfo.MarkFailed();
                    NC_LOG_ERROR("[Texture Extractor] Failed to convert {0}", fileListEntry.path);
                }
            }

            const u32 processedFiles = ++numFilesConverted;
            f32 progress = (static_cast<f32>(processedFiles) / static_cast<f32>(numFiles)) * 10.0f;
            u32 bitToCheck = static_cast<u32>(progress);
            u32 bitMask = 1u << bitToCheck;

            bool reportStatus = (progressFlags & bitMask) == 0;
            if (reportStatus)
            {
                progressFlags |= bitMask;
                NC_LOG_INFO("[Texture Extractor] Progress Status ({0:.0f}% / 100%)", progress * 10.0f);
            }
        }
    });

    runtime->scheduler.AddTaskSetToPipe(&convertTexturesTask);
    runtime->scheduler.WaitforTask(&convertTexturesTask);
}
