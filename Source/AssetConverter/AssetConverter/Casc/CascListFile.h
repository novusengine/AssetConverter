#pragma once
#include <Base/Types.h>

#include <robinhood/robinhood.h>

class Bytebuffer;


struct CascListFile
{
public:
	CascListFile(std::string listPath) : _listPath(listPath) { }

	bool Initialize();

	bool HasFileWithID(u32 fileID) const { return _fileIDToPath.find(fileID) != _fileIDToPath.end(); }
	const std::string& GetFilePathFromID(u32 fileID) const { return _fileIDToPath.at(fileID); }

	bool HasFileWithPath(const std::string& filePath) const { return _filePathToID.find(filePath) != _filePathToID.end(); }
	u32 GetFileIDFromPath(const std::string& filePath) const { return _filePathToID.at(filePath); }

	const std::vector<u32>& GetM2FileIDs() const { return _m2Files; }
	const std::vector<u32>& GetWMOFileIDs() const { return _wmoFiles; }
	const std::vector<u32>& GetBLPFileIDs() const { return _blpFiles; }
	u32 GetNumEntries() const { return static_cast<u32>(_fileIDToPath.size()); }

	const robin_hood::unordered_map<std::string, u32>& GetFilePathToIDMap() const { return _filePathToID; }

private:
	void ParseListFile();

private:
	std::string _listPath = "";
	Bytebuffer* _fileBuffer = nullptr;

	robin_hood::unordered_map<u32, std::string> _fileIDToPath;
	robin_hood::unordered_map<std::string, u32> _filePathToID;

	std::vector<u32> _m2Files;
	std::vector<u32> _wmoFiles;
	std::vector<u32> _blpFiles;
};