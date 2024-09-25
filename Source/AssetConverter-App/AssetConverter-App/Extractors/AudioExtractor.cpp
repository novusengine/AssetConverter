#include "AudioExtractor.h"

#include "AssetConverter-App/Sound/AudioConverter.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/ServiceLocator.h"

#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <enkiTS/TaskScheduler.h>

namespace fs = std::filesystem;

// All 8 .ogg audio files in 'Data\Audio\interface\exploration' aren't being extracted with their pcm/frame data.
void AudioExtractor::Process()
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

    std::vector<std::string> paths = { };
    std::vector<FileListEntry> fileList = { };
    fileList.reserve(filePathToIDMap.size());

    for (auto& itr : filePathToIDMap)
    {
        // ".wav" (1 file) and ".wav.ogg" (6 files) are not observed in Classic. The 7 files were added in a later expansion.
        if (!StringUtils::EndsWith(itr.first, ".mp3") && !StringUtils::EndsWith(itr.first, ".ogg") && !StringUtils::EndsWith(itr.first, ".wav") && !StringUtils::EndsWith(itr.first, ".wav.ogg"))
            continue;

        if (!cascLoader->InCascAndListFile(itr.second))
            continue;

        std::string pathStr = itr.first;
        paths.push_back(itr.first);
        std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::tolower);

        fs::path outputPath = (runtime->paths.audio / pathStr).replace_extension("wav");

        if (fs::exists(outputPath))
            continue;

        fs::create_directories(outputPath.parent_path());

        FileListEntry& fileListEntry = fileList.emplace_back();
        fileListEntry.fileID = itr.second;
        fileListEntry.fileName = outputPath.filename().string();
        fileListEntry.path = outputPath.string();
    }

    u32 numFiles = static_cast<u32>(fileList.size());
    std::atomic<u32> numFilesConverted = 0;
    std::atomic<u16> progressFlags = 0;
    NC_LOG_INFO("[Audio Extractor] Processing {0} files", numFiles);

    enki::TaskSet convertAudioTask(numFiles, [&](enki::TaskSetPartition range, uint32_t threadNum)
        {
            for (u32 i = range.start; i < range.end; i++)
            {
                const FileListEntry& fileListEntry = fileList[i];

                std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByID(fileListEntry.fileID);
                if (!buffer)
                    continue;

                ConvertToWAV(buffer, fileListEntry.path);

                f32 progress = (static_cast<f32>(numFilesConverted++) / static_cast<f32>(numFiles - 1)) * 10.0f;
                u32 bitToCheck = static_cast<u32>(progress);
                u32 bitMask = 1u << bitToCheck;

                bool reportStatus = (progressFlags & bitMask) == 0;
                if (reportStatus)
                {
                    progressFlags |= bitMask;
                    NC_LOG_INFO("[Audio Extractor] Progress Status ({0:.0f}% / 100%)", progress * 10.0f);
                }
            }
        });

    runtime->scheduler.AddTaskSetToPipe(&convertAudioTask);
    runtime->scheduler.WaitforTask(&convertAudioTask);
}