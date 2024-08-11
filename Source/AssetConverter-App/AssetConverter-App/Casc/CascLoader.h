#pragma once
#include "CascListFile.h"

#include <Base/Types.h>
#include <Base/Memory/Bytebuffer.h>

#include <Casc/CascLib.h>

class CascLoader
{
public:
    enum class Result
    {
        Success,
        AlreadyInitialized,
        MissingCasc,
        MissingListFile,
        MissingLocale
    };

public:
    CascLoader(const std::string& listPath, const std::string& locale) : _listFile(listPath), _locale(locale) { }
    ~CascLoader() { }

    CascLoader::Result Load();
    void Close();

    std::shared_ptr<Bytebuffer> GetFileByID(u32 fileID);
    std::shared_ptr<Bytebuffer> GetFilePartialByID(u32 fileID, u32 size);
    std::shared_ptr<Bytebuffer> GetFileByPath(std::string filePath);
    std::shared_ptr<Bytebuffer> GetFileByListFilePath(const std::string& filePath);
    bool FileExistsInCasc(u32 fileID);
    bool ListFileContainsID(u32 fileID) { return _listFile.HasFileWithID(fileID); }
    bool InCascAndListFile(u32 fileID) { return FileExistsInCasc(fileID) && ListFileContainsID(fileID); }

    const std::string& GetFilePathFromListFileID(u32 fileID)
    {
        return _listFile.GetFilePathFromID(fileID);
    }

    bool ListFileContainsPath(const std::string& filePath) { return _listFile.HasFileWithPath(filePath); }
    u32 GetFileIDFromListFilePath(const std::string& filePath)
    {
        if (!ListFileContainsPath(filePath))
            return 0;

        return _listFile.GetFileIDFromPath(filePath);
    }

    const CascListFile& GetListFile() { return _listFile; }

private:
    static bool LoadingCallback(void* ptrUserParam, CASC_PROGRESS_MSG message, LPCSTR szObject, DWORD currentValue, DWORD totalValue);
    std::shared_ptr<Bytebuffer> GetFileByHandle(void* handle);
    std::shared_ptr<Bytebuffer> GetFilePartialByHandle(void* handle, u32 size);

private:
    void* _storageHandle = nullptr;
    CascListFile _listFile;
    std::string _locale;
    static bool _isLoadingIndexFiles;
};