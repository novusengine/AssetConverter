#pragma once
#include <Base/Memory/Bytebuffer.h>

#include <Filesystem/Config.h>
#include <Filesystem/Core/File.h>
#include <Filesystem/Core/Manifest.h>
#include <Filesystem/Core/Root.h>

#include <enkiTS/TaskScheduler.h>
#include <json/json.hpp>
#include <robinhood/robinhood.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>

struct Runtime;
struct PactManifestInfo
{
public:
    PactManifestInfo() { }

    bool Initialize(const std::filesystem::path& manifestPath, const std::filesystem::path& manifestDataPath, size_t entryCount);
    bool AddFile(Runtime* runtime, const std::string& path, std::shared_ptr<Bytebuffer>& data, PACT::PactFileID* fileID = nullptr);
    bool AddFile(Runtime* runtime, const std::string& path, std::vector<u8>& data, PACT::PactFileID* fileID = nullptr);

    bool Finalize();

    size_t GetSerializedSize() const;
    const std::filesystem::path& GetPath() const { return path; }
    const std::filesystem::path& GetDataPath() const { return dataPath; }

public:
    std::filesystem::path path;
    std::filesystem::path dataPath;

    std::mutex addFileMutex;
    size_t reservedBytes = 0;
    size_t writtenData = 0;
    PACT::PactManifest manifest;
    std::ofstream dataWriter;
};

struct ManifestPool
{
public:
    void Initialize();
    bool Finalize();

    PactManifestInfo& GetManifestForFile(Runtime* runtime, size_t fileSize);

private:
    static constexpr u64 MAX_MANIFEST_SIZE = 1 * 1024 * 1024 * 1024; // 1 GB

    std::mutex _rolloverMutex;

    size_t _currentManifestIndex = 0;
    std::vector<std::unique_ptr<PactManifestInfo>> _manifests;
};

struct PactInfo
{
public:
    bool GetFileID(u64 hash, PACT::PactFileID& out)
    {
        std::shared_lock lock(_fileIDMutex);
        out = 0;

        auto itr = _fileHashToID.find(hash);
        if (itr == _fileHashToID.end())
            return false;

        out = itr->second;
        return true;
    }

    bool FileExists(u64 hash)
    {
        std::shared_lock lock(_fileIDMutex);
        return _fileHashToID.contains(hash);
    }

    bool AddFile(u64 hash, PACT::PactFileID& out)
    {
        std::scoped_lock lock(_fileIDMutex);
        out = 0;

        auto itr = _fileHashToID.find(hash);
        if (itr != _fileHashToID.end())
            return false;

        out = _nextFileID++;
        _fileHashToID[hash] = out;
        return true;
    }

    void RemoveFile(u64 hash, PACT::PactFileID fileID)
    {
        std::scoped_lock lock(_fileIDMutex);

        auto itr = _fileHashToID.find(hash);
        if (itr != _fileHashToID.end() && itr->second == fileID)
            _fileHashToID.erase(itr);
    }

    PACT::PactRoot& GetRoot()
    {
        return _root;
    }

    PactManifestInfo& GetManifestForFile(Runtime* runtime, size_t fileSize)
    {
        return _manifestPool.GetManifestForFile(runtime, fileSize);
    }

    void Initialize()
    {
        _manifestPool.Initialize();
    }

    bool Finalize()
    {
        return _manifestPool.Finalize() && !_failed.load(std::memory_order_acquire);
    }

    void MarkFailed()
    {
        _failed.store(true, std::memory_order_release);
    }

public:
    std::shared_mutex _fileIDMutex;

    PACT::PactRoot _root;
    PACT::PactFileID _nextFileID = 1;
    robin_hood::unordered_map<u64, PACT::PactFileID> _fileHashToID;
    ManifestPool _manifestPool;
    std::atomic<bool> _failed = false;
};

struct Runtime
{
public:
    struct Paths
    {
        std::filesystem::path executable;
        std::filesystem::path data;
        std::filesystem::path pactRoot;
        std::filesystem::path pactManifest;
        std::filesystem::path pactData;
        std::filesystem::path navMesh;
    };

public:
    bool isInDebugMode = false;
    Paths paths = {};
    PactInfo pactInfo = {};

    enki::TaskScheduler scheduler;
    nlohmann::ordered_json json = {};
};
