#include "Runtime.h"

#include <Base/Util/DebugHandler.h>

#include <xxhash/xxhash64.h>

#include <filesystem>
namespace fs = std::filesystem;

bool PactManifestInfo::Initialize(const std::filesystem::path& manifestPath, const std::filesystem::path& manifestDataPath, size_t entryCount)
{
    path = manifestPath;
    dataPath = manifestDataPath;

    manifest.header.version = PACT::Config::MANIFEST_VERSION;
    manifest.header.flags = { 0 };
    manifest.header.priority = 0;
    manifest.header.sourceType = PACT::PactSourceType::Pack;
    manifest.header.entryCount = 0;
    manifest.header.reserved = 0;

    manifest.entries.reserve(entryCount);
    dataWriter.open(manifestDataPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!dataWriter.is_open())
    {
        NC_LOG_ERROR("PactBuilder : Failed to open manifest data file (\"{0}\")", manifestDataPath.string());
        return false;
    }

    return true;
}

bool PactManifestInfo::AddFile(Runtime* runtime, const std::string& path, std::shared_ptr<Bytebuffer>& data, PACT::PactFileID* outFileID)
{
    u64 hash = XXHash64::hash(path.c_str(), path.length(), 0);

    PACT::PactFileID fileID = 0;
    if (!runtime->pactInfo.AddFile(hash, fileID))
    {
        runtime->pactInfo.MarkFailed();
        NC_LOG_ERROR("PactBuilder : Duplicate file path (\"{0}\")", path);
        return false;
    }

    std::scoped_lock lock(addFileMutex);
    if (!dataWriter.is_open() || !dataWriter.good())
    {
        runtime->pactInfo.RemoveFile(hash, fileID);
        runtime->pactInfo.MarkFailed();
        NC_LOG_ERROR("PactBuilder : Manifest data file is not writable while adding (\"{0}\")", path);
        return false;
    }

    const size_t dataOffset = writtenData;
    const u32 dataSize = static_cast<u32>(data->writtenData);
    if (dataSize > 0)
    {
        dataWriter.write(reinterpret_cast<const char*>(data->GetDataPointer()), static_cast<std::streamsize>(data->writtenData));
        if (!dataWriter.good())
        {
            runtime->pactInfo.RemoveFile(hash, fileID);
            runtime->pactInfo.MarkFailed();
            NC_LOG_ERROR("PactBuilder : Failed to write manifest data for (\"{0}\")", path);
            return false;
        }

        writtenData += data->writtenData;
    }

    PACT::ManifestEntry& entry = manifest.entries.emplace_back();
    entry.fileID = fileID;
    entry.flags = {};
    entry.pathIndex = manifest.stringTable.AddString(path);
    entry.pathHash = hash;
    entry.dataOffset = dataOffset;
    entry.dataSize = dataSize;
    entry.dataCompressedSize = 0;

    if (outFileID)
        *outFileID = fileID;

    return true;
}

bool PactManifestInfo::AddFile(Runtime* runtime, const std::string& path, std::vector<u8>& data, PACT::PactFileID* outFileID)
{
    u64 hash = XXHash64::hash(path.c_str(), path.length(), 0);

    PACT::PactFileID fileID = 0;
    if (!runtime->pactInfo.AddFile(hash, fileID))
    {
        runtime->pactInfo.MarkFailed();
        NC_LOG_ERROR("PactBuilder : Duplicate file path (\"{0}\")", path);
        return false;
    }

    std::scoped_lock lock(addFileMutex);
    if (!dataWriter.is_open() || !dataWriter.good())
    {
        runtime->pactInfo.RemoveFile(hash, fileID);
        runtime->pactInfo.MarkFailed();
        NC_LOG_ERROR("PactBuilder : Manifest data file is not writable while adding (\"{0}\")", path);
        return false;
    }

    const size_t dataOffset = writtenData;
    const u32 dataSize = static_cast<u32>(data.size());
    if (dataSize > 0)
    {
        dataWriter.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!dataWriter.good())
        {
            runtime->pactInfo.RemoveFile(hash, fileID);
            runtime->pactInfo.MarkFailed();
            NC_LOG_ERROR("PactBuilder : Failed to write manifest data for (\"{0}\")", path);
            return false;
        }

        writtenData += data.size();
    }

    PACT::ManifestEntry& entry = manifest.entries.emplace_back();
    entry.fileID = fileID;
    entry.flags = {};
    entry.pathIndex = manifest.stringTable.AddString(path);
    entry.pathHash = hash;
    entry.dataOffset = dataOffset;
    entry.dataSize = dataSize;
    entry.dataCompressedSize = 0;

    if (outFileID)
        *outFileID = fileID;

    return true;
}

bool PactManifestInfo::Finalize()
{
    dataWriter.flush();
    const bool dataWriteSucceeded = dataWriter.good();
    dataWriter.close();
    if (!dataWriteSucceeded || dataWriter.fail())
    {
        NC_LOG_ERROR("PactBuilder : Failed to finalize manifest data file (\"{0}\")", dataPath.string());
        return false;
    }

    // A reservation can roll over to a new pack before AddFile discovers that
    // the path is a duplicate. Do not serialize an empty entries.data() range;
    // empty pack placeholders are not part of the finalized PACT.
    if (manifest.entries.empty())
    {
        std::error_code error;
        std::filesystem::remove(dataPath, error);
        return !error;
    }

    const size_t size = GetSerializedSize();
    std::shared_ptr<Bytebuffer> buffer = Bytebuffer::BorrowRuntime(size);

    manifest.header.entryCount = static_cast<u32>(manifest.entries.size());

    if (buffer->Serialize(manifest))
    {
        std::ofstream manifestWriter(path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!manifestWriter.is_open())
        {
            NC_LOG_ERROR("PactBuilder : Failed to open or write to manifest file (\"{0}\")", path.string());
            return false;
        }

        manifestWriter.write(reinterpret_cast<const char*>(buffer->GetDataPointer()), static_cast<std::streamsize>(buffer->writtenData));
        manifestWriter.flush();
        const bool manifestWriteSucceeded = manifestWriter.good();
        manifestWriter.close();
        if (!manifestWriteSucceeded || manifestWriter.fail())
        {
            NC_LOG_ERROR("PactBuilder : Failed to write manifest file (\"{0}\")", path.string());
            return false;
        }
    }
    else
    {
        NC_LOG_ERROR("PactBuilder : Failed to serialize manifest file (\"{0}\")", path.string());
        return false;
    }

    return true;
}

size_t PactManifestInfo::GetSerializedSize() const
{
    size_t size = 0;

    size += sizeof(manifest.header);
    size += manifest.entries.size() * sizeof(PACT::ManifestEntry);
    size += sizeof(u32) + manifest.stringTable.GetNumBytes();

    return size;
}

void ManifestPool::Initialize()
{
    _manifests.reserve(64);
}
bool ManifestPool::Finalize()
{
    bool success = true;
    for (auto& manifest : _manifests)
    {
        success &= manifest->Finalize();
    }

    return success;
}
PactManifestInfo& ManifestPool::GetManifestForFile(Runtime* runtime, size_t fileSize)
{
    NC_ASSERT(fileSize <= MAX_MANIFEST_SIZE, "Filesize exceeds the max size for a manifest");

    std::scoped_lock lock(_rolloverMutex);

    if (_manifests.size() > 0)
    {
        PactManifestInfo& manifest = *_manifests[_currentManifestIndex];
        if ((manifest.reservedBytes + fileSize) <= MAX_MANIFEST_SIZE)
        {
            manifest.reservedBytes += fileSize;
            return manifest;
        }
    }

    // Create new manifest
    _manifests.push_back(std::make_unique<PactManifestInfo>());
    _currentManifestIndex = _manifests.size() - 1;

    fs::path nextManifestPath = fs::absolute(runtime->paths.pactManifest / ("pack_" + std::to_string(_currentManifestIndex))).replace_extension(PACT::Config::MANIFEST_EXT);
    fs::path nextDataPath = fs::absolute(runtime->paths.pactData / ("pack_" + std::to_string(_currentManifestIndex))).replace_extension(PACT::Config::DATA_EXT);

    PactManifestInfo& nextManifest = *_manifests[_currentManifestIndex];
    if (!nextManifest.Initialize(nextManifestPath, nextDataPath, 100000))
        runtime->pactInfo.MarkFailed();

    nextManifest.manifest.header.manifestID = _currentManifestIndex;
    nextManifest.reservedBytes += fileSize;

    return nextManifest;
}
