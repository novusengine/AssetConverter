#include "Runtime.h"
#include "Util/ServiceLocator.h"

#include <Base/Util/DebugHandler.h>

#include <libsodium/core/crypto_hash_sha256.h>
#include <xxhash/xxhash64.h>

#include <algorithm>
#include <filesystem>
#include <set>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace
{
    bool ReplaceFileAtomically(const fs::path& source, const fs::path& destination)
    {
#ifdef _WIN32
        return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::error_code error;
        fs::rename(source, destination, error);
        return !error;
#endif
    }

    void RemoveSupersededLocalFiles(const Runtime::Paths& paths, const std::vector<PACT::PactDigest>& supersededDigests, const std::vector<PACT::PactManifestRef>& retainedRefs)
    {
        std::set<PACT::PactDigest> retainedDigests;
        for (const PACT::PactManifestRef& manifestRef : retainedRefs)
        {
            retainedDigests.insert(manifestRef.digest);
        }

        for (const PACT::PactDigest& digest : supersededDigests)
        {
            if (retainedDigests.contains(digest))
                continue;

            const std::string digestName = PACT::PactDigestToHex(digest);
            const fs::path manifestPath = (paths.pactManifest / digestName).replace_extension(PACT::Config::MANIFEST_EXT);
            const fs::path dataPath = (paths.pactData / digestName).replace_extension(PACT::Config::DATA_EXT);

            for (const fs::path& path : { manifestPath, dataPath })
            {
                std::error_code error;
                if (!fs::remove(path, error) && error)
                {
                    NC_LOG_WARNING("[AssetConverter] Failed to remove superseded local PACT file {0}: {1}", path.string(), error.message());
                }
            }
        }
    }
}

bool PactInfo::Finalize()
{
    if (_failed.load(std::memory_order_acquire))
        return false;

    if (!_manifestPool.Finalize())
        return false;

    auto& manifests = _manifestPool.GetAllManifests();
    std::vector<PACT::PactManifestRef>& manifestRefs = _root.manifestRefs;
    std::erase_if(manifestRefs, [](const PACT::PactManifestRef& manifestRef)
    {
        return manifestRef.origin == PACT::PactManifestOrigin::Local;
    });
    manifestRefs.reserve(manifestRefs.size() + manifests.size());

    Runtime* runtime = ServiceLocator::GetRuntime();

    for (auto& manifest : manifests)
    {
        const std::string digestName = PACT::PactDigestToHex(manifest->digest);
        const fs::path finalManifestPath = fs::absolute(runtime->paths.pactManifest / digestName).replace_extension(PACT::Config::MANIFEST_EXT);
        const fs::path finalDataPath = fs::absolute(runtime->paths.pactData / digestName).replace_extension(PACT::Config::DATA_EXT);

        if (!ReplaceFileAtomically(manifest->dataPath, finalDataPath))
        {
            NC_LOG_CRITICAL("[AssetConverter] Failed to publish manifest data file {0}", finalDataPath.string());
            return false;
        }

        if (!ReplaceFileAtomically(manifest->path, finalManifestPath))
        {
            NC_LOG_CRITICAL("[AssetConverter] Failed to publish manifest file {0}", finalManifestPath.string());
            return false;
        }

        manifest->path = finalManifestPath;
        manifest->dataPath = finalDataPath;

        auto& manifestRef = manifestRefs.emplace_back();
        manifestRef.digest = manifest->digest;
        manifestRef.manifestID = manifest->manifest.header.manifestID;
        manifestRef.priority = manifest->manifest.header.priority;
        manifestRef.storageMask = manifest->manifest.header.flags.storageMask;
        manifestRef.origin = PACT::PactManifestOrigin::Local;
    }

    std::shared_ptr<Bytebuffer> buffer = Bytebuffer::BorrowRuntime(_root.GetSerializedSize());
    if (!_root.Serialize(buffer.get()))
    {
        NC_LOG_CRITICAL("[AssetConverter] Failed to serialize the PACT root");
        return false;
    }

    const fs::path pactRootPath = fs::absolute(runtime->paths.pactRoot / PACT::Config::ROOT_FILE);
    fs::path temporaryRootPath = pactRootPath;
    temporaryRootPath += ".tmp";

    std::error_code removeError;
    fs::remove(temporaryRootPath, removeError);
    if (removeError)
    {
        NC_LOG_CRITICAL("[AssetConverter] Failed to remove stale temporary PACT root {0}: {1}", temporaryRootPath.string(), removeError.message());
        return false;
    }

    std::ofstream rootWriter(temporaryRootPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!rootWriter.is_open())
    {
        NC_LOG_CRITICAL("[AssetConverter] Failed to open the temporary PACT root file {0}", temporaryRootPath.string());
        return false;
    }

    rootWriter.write(reinterpret_cast<const char*>(buffer->GetDataPointer()), static_cast<std::streamsize>(buffer->writtenData));
    rootWriter.flush();
    rootWriter.close();
    if (rootWriter.fail())
    {
        NC_LOG_CRITICAL("[AssetConverter] Failed to write the temporary PACT root file {0}", temporaryRootPath.string());
        return false;
    }

    if (!ReplaceFileAtomically(temporaryRootPath, pactRootPath))
    {
        NC_LOG_CRITICAL("[AssetConverter] Failed to atomically publish the PACT root file {0}", pactRootPath.string());
        return false;
    }

    RemoveSupersededLocalFiles(runtime->paths, supersededLocalDigests, manifestRefs);
    supersededLocalDigests.clear();

    return true;
}

bool PactManifestInfo::Initialize(const u64 manifestID, const std::filesystem::path& manifestPath, const std::filesystem::path& manifestDataPath, size_t entryCount)
{
    path = manifestPath;
    dataPath = manifestDataPath;

    manifest.header.version = PACT::Config::MANIFEST_VERSION;
    manifest.header.manifestID = manifestID;
    manifest.header.flags = { 0 };
    manifest.header.priority = 0;
    manifest.header.sourceType = PACT::PactSourceType::Pack;
    manifest.chunks.reserve(entryCount);
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

    PACT::PactFeatureSet featureSet = runtime->pactInfo._root.featureSet;

    const u32 startChunkIndex = static_cast<u32>(manifest.chunks.size());
    u32 numChunks = 0;
    if (!PACT::PactChunker::SplitFastCDC(data->GetDataPointer(), data->writtenData, featureSet.cdcMinSize, featureSet.cdcAvgSize, featureSet.cdcMaxSize, manifest.chunks, numChunks))
    {
        manifest.chunks.resize(startChunkIndex);
        runtime->pactInfo.RemoveFile(hash, fileID);
        runtime->pactInfo.MarkFailed();
        NC_LOG_ERROR("PactBuilder : Failed to chunk (\"{0}\")", path);
        return false;
    }

    if (numChunks > 0)
    {
        u64 offset = 0;

        for (u32 i = 0; i < numChunks; i++)
        {
            auto& chunk = manifest.chunks[startChunkIndex + i];

            if (chunk.size > data->writtenData - offset)
            {
                runtime->pactInfo.RemoveFile(hash, fileID);
                runtime->pactInfo.MarkFailed();
                NC_LOG_ERROR("PactBuilder : FastCDC produced an invalid chunk size for (\"{0}\")", path);
                return false;
            }

            offset += chunk.size;
        }

        if (offset != data->writtenData)
        {
            runtime->pactInfo.RemoveFile(hash, fileID);
            runtime->pactInfo.MarkFailed();
            NC_LOG_ERROR("PactBuilder : FastCDC chunks do not cover the complete file (\"{0}\")", path);
            return false;
        }
    }

    if (dataSize > 0 && numChunks > 0)
    {
        entry.chunkIndex = startChunkIndex;
        entry.chunkCount = numChunks;

        crypto_hash_sha256(entry.contentDigest.data(), data->GetDataPointer(), data->writtenData);
    }
    else
    {
        entry.dataOffset = 0;
        entry.chunkIndex = 0;
        entry.chunkCount = 0;

        const u8 emptyContent = 0;
        crypto_hash_sha256(entry.contentDigest.data(), &emptyContent, 0);
    }

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

    PACT::PactFeatureSet featureSet = runtime->pactInfo._root.featureSet;

    const u32 startChunkIndex = static_cast<u32>(manifest.chunks.size());
    u32 numChunks = 0;
    if (!PACT::PactChunker::SplitFastCDC(data.data(), data.size(), featureSet.cdcMinSize, featureSet.cdcAvgSize, featureSet.cdcMaxSize, manifest.chunks, numChunks))
    {
        manifest.chunks.resize(startChunkIndex);
        runtime->pactInfo.RemoveFile(hash, fileID);
        runtime->pactInfo.MarkFailed();
        NC_LOG_ERROR("PactBuilder : Failed to chunk (\"{0}\")", path);
        return false;
    }

    if (numChunks > 0)
    {
        u64 offset = 0;

        for (u32 i = 0; i < numChunks; i++)
        {
            auto& chunk = manifest.chunks[startChunkIndex + i];

            if (chunk.size > data.size() - offset)
            {
                runtime->pactInfo.RemoveFile(hash, fileID);
                runtime->pactInfo.MarkFailed();
                NC_LOG_ERROR("PactBuilder : FastCDC produced an invalid chunk size for (\"{0}\")", path);
                return false;
            }

            offset += chunk.size;
        }

        if (offset != data.size())
        {
            runtime->pactInfo.RemoveFile(hash, fileID);
            runtime->pactInfo.MarkFailed();
            NC_LOG_ERROR("PactBuilder : FastCDC chunks do not cover the complete file (\"{0}\")", path);
            return false;
        }
    }

    if (dataSize > 0 && numChunks > 0)
    {
        entry.chunkIndex = startChunkIndex;
        entry.chunkCount = numChunks;

        crypto_hash_sha256(entry.contentDigest.data(), data.data(), data.size());
    }
    else
    {
        entry.dataOffset = 0;
        entry.chunkIndex = 0;
        entry.chunkCount = 0;

        const u8 emptyContent = 0;
        crypto_hash_sha256(entry.contentDigest.data(), &emptyContent, 0);
    }

    if (outFileID)
        *outFileID = fileID;

    return true;
}

bool PactManifestInfo::Finalize()
{
    const size_t size = GetSerializedSize();
    std::shared_ptr<Bytebuffer> buffer = Bytebuffer::BorrowRuntime(size);

    dataWriter.flush();
    const bool dataWriteSucceeded = dataWriter.good();
    dataWriter.close();

    if (!dataWriteSucceeded || dataWriter.fail())
    {
        NC_LOG_ERROR("PactBuilder : Failed to finalize manifest data file (\"{0}\")", dataPath.string());
        return false;
    }

    if (buffer->Serialize(manifest))
    {
        crypto_hash_sha256(digest.data(), buffer->GetDataPointer(), static_cast<unsigned long long>(buffer->writtenData));

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
    return manifest.GetSerializedSize();
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
    NC_ASSERT(fileSize <= PACT::Config::MAX_MANIFEST_DATA_SIZE, "Filesize exceeds the max size for a manifest");

    std::scoped_lock lock(_rolloverMutex);

    if (_manifests.size() > 0)
    {
        PactManifestInfo& manifest = *_manifests[_currentManifestIndex];
        if ((manifest.reservedBytes + fileSize) <= PACT::Config::MAX_MANIFEST_DATA_SIZE)
        {
            manifest.reservedBytes += fileSize;
            return manifest;
        }
    }

    // Create new manifest
    _manifests.push_back(std::make_unique<PactManifestInfo>());
    _currentManifestIndex = _manifests.size() - 1;

    const PACT::PactManifestHandle manifestID = PACT::Config::LOCAL_MANIFEST_ID_START + static_cast<PACT::PactManifestHandle>(_currentManifestIndex);
    const std::string packName = "local_pending_" + std::to_string(manifestID);
    const fs::path nextManifestPath = fs::absolute(runtime->paths.pactManifest / packName).replace_extension(PACT::Config::MANIFEST_EXT);
    const fs::path nextDataPath = fs::absolute(runtime->paths.pactData / packName).replace_extension(PACT::Config::DATA_EXT);

    PactManifestInfo& nextManifest = *_manifests[_currentManifestIndex];
    if (!nextManifest.Initialize(manifestID, nextManifestPath, nextDataPath, 100000))
        runtime->pactInfo.MarkFailed();

    nextManifest.reservedBytes += fileSize;

    return nextManifest;
}
