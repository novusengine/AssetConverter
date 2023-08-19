#include "ClientDBExtractor.h"
#include "AssetConverter/Runtime.h"
#include "AssetConverter/Casc/CascLoader.h"
#include "AssetConverter/Util/ServiceLocator.h"

#include <Base/Container/StringTable.h>
#include <Base/Memory/FileWriter.h>
#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <FileFormat/Shared.h>
#include <FileFormat/Warcraft/DB2/DB2Definitions.h>
#include <FileFormat/Warcraft/DB2/Wdc3.h>
#include <FileFormat/Warcraft/Parsers/Wdc3Parser.h>
#include <FileFormat/Novus/ClientDB/ClientDB.h>
#include <FileFormat/Novus/ClientDB/Definitions.h>

#include <filesystem>
namespace fs = std::filesystem;

std::vector<ClientDBExtractor::ExtractionEntry> ClientDBExtractor::_extractionEntries =
{
	{ "Map.db2", "A collection of all maps", ClientDBExtractor::ExtractMap },
	{ "LiquidObject.db2", "A collection of liquid objects", ClientDBExtractor::ExtractLiquidObject },
	{ "LiquidType.db2", "A collection of liquid types", ClientDBExtractor::ExtractLiquidType },
	{ "LiquidMaterial.db2", "A collection of liquid materials", ClientDBExtractor::ExtractLiquidMaterial },
	{ "CinematicCamera.db2", "A collection of cinematic cameras", ClientDBExtractor::ExtractCinematicCamera },
	{ "CinematicSequences.db2", "A collection of cinematic sequences", ClientDBExtractor::ExtractCinematicSequence }
};

Client::ClientDB<Client::Definitions::Map> ClientDBExtractor::maps;
Client::ClientDB<Client::Definitions::LiquidObject> ClientDBExtractor::liquidObjects;
Client::ClientDB<Client::Definitions::LiquidType> ClientDBExtractor::liquidTypes;
Client::ClientDB<Client::Definitions::LiquidMaterial> ClientDBExtractor::liquidMaterials;
Client::ClientDB<Client::Definitions::CinematicCamera> ClientDBExtractor::cinematicCameras;
Client::ClientDB<Client::Definitions::CinematicSequence> ClientDBExtractor::cinematicSequences;

void ClientDBExtractor::Process()
{
	for (u32 i = 0; i < _extractionEntries.size(); i++)
	{
		const ExtractionEntry& entry = _extractionEntries[i];

		if (entry.function())
		{
			DebugHandler::Print("[ClientDBExtractor] Extracted (\"{0}\" : \"{1}\")", entry.name, entry.description);
		}
		else
		{
			DebugHandler::PrintWarning("[ClientDBExtractor] Failed to extract (\"{0}\" : \"{1}\")", entry.name, entry.description);
		}
	}
}

void FixPathExtension(std::string& path)
{
	if (StringUtils::EndsWith(path, ".mdx"))
	{
		path = path.substr(0, path.length() - 4) + ".complexmodel";
	}
	else if (StringUtils::EndsWith(path, ".m2"))
	{
		path = path.substr(0, path.length() - 3) + ".complexmodel";
	}
	else if (StringUtils::EndsWith(path, ".blp"))
	{
		path = path.substr(0, path.length() - 4) + ".dds";
		std::transform(path.begin(), path.end(), path.begin(), ::tolower);
	}
}

u32 GetStringIndexFromRecordIndex(DB2::WDC3::Layout& layout, DB2::WDC3::Parser& db2Parser, u32 recordIndex, u32 fieldIndex, StringTable& stringTable)
{
	std::string value = db2Parser.GetString(layout, recordIndex, fieldIndex);

	if (value.length() == 0)
		return std::numeric_limits<u32>().max();

	FixPathExtension(value);

	return stringTable.AddString(value);
}
u32 GetStringIndexFromArrRecordIndex(DB2::WDC3::Layout& layout, DB2::WDC3::Parser& db2Parser, u32 recordIndex, u32 fieldIndex, u32 arrIndex, StringTable& stringTable)
{
	std::string value = db2Parser.GetStringInArr(layout, recordIndex, fieldIndex, arrIndex);

	if (value.length() == 0)
		return std::numeric_limits<u32>().max();

	FixPathExtension(value);

	return stringTable.AddString(value);
}

bool ClientDBExtractor::ExtractMap()
{
	CascLoader* cascLoader = ServiceLocator::GetCascLoader();

	DB2::WDC3::Layout layout = { };
	DB2::WDC3::Parser db2Parser = { };

	std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/map.db2");
	if (!buffer || !db2Parser.TryParse(buffer, layout))
		return false;

	const DB2::WDC3::Layout::Header& header = layout.header;

	maps.data.clear();
	maps.data.reserve(header.recordCount);
	maps.stringTable.Clear();
	maps.stringTable.Reserve(static_cast<u64>(header.recordCount) * 2);
	maps.idToIndexMap.reserve(header.recordCount);

	for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
	{
		u32 sectionID = 0;
		u32 recordID = 0;
		u8* recordData = nullptr;

		if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
			continue;

		std::string internalName = db2Parser.GetString(layout, db2RecordIndex, 0);
		std::string cascPath = "world/maps/" + internalName + "/" + internalName + ".wdt";
		std::transform(cascPath.begin(), cascPath.end(), cascPath.begin(), ::tolower);

		u32 fileID = cascLoader->GetFileIDFromListFilePath(cascPath);

		bool hasWDTFile = fileID > 0 && cascLoader->FileExistsInCasc(fileID);
		if (hasWDTFile)
		{
			DB::Client::Definitions::Map& map = maps.data.emplace_back();

			map.id = db2Parser.GetRecordIDFromIndex(layout, db2RecordIndex);
			map.name = GetStringIndexFromRecordIndex(layout, db2Parser, db2RecordIndex, 1, maps.stringTable);
			map.internalName = GetStringIndexFromRecordIndex(layout, db2Parser, db2RecordIndex, 0, maps.stringTable);
			map.instanceType = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 7);

			const u32* flags = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 21);
			map.flags = flags[0];

			map.expansion = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 8);
			map.maxPlayers = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 18);

			maps.idToIndexMap[map.id] = static_cast<u32>(maps.data.size()) - 1u;
		}
	}

	db2Parser.RepopulateFromCopyTable(layout, maps.data, maps.idToIndexMap);

	size_t size = maps.GetSerializedSize();
	std::shared_ptr<Bytebuffer> resultBuffer = Bytebuffer::BorrowRuntime(size);

	fs::path path = ServiceLocator::GetRuntime()->paths.clientDB / "Map.cdb";
	FileWriter fileWriter(path, resultBuffer);

	if (!maps.Write(resultBuffer))
		return false;

	if (!fileWriter.Write())
		return false;

	return true;
}

bool ClientDBExtractor::ExtractLiquidObject()
{
	CascLoader* cascLoader = ServiceLocator::GetCascLoader();

	DB2::WDC3::Layout layout = { };
	DB2::WDC3::Parser db2Parser = { };

	std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/liquidobject.db2");
	if (!buffer || !db2Parser.TryParse(buffer, layout))
		return false;

	const DB2::WDC3::Layout::Header& header = layout.header;

	liquidObjects.data.clear();
	liquidObjects.data.reserve(header.recordCount);
	liquidObjects.stringTable.Clear();
	liquidObjects.idToIndexMap.reserve(header.recordCount);

	for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
	{
		u32 sectionID = 0;
		u32 recordID = 0;
		u8* recordData = nullptr;

		if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
			continue;

		DB::Client::Definitions::LiquidObject& liquidObject = liquidObjects.data.emplace_back();

		liquidObject.id = db2Parser.GetRecordIDFromIndex(layout, db2RecordIndex);
		liquidObject.flowDirection = *db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 0);
		liquidObject.flowSpeed = *db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 1);
		liquidObject.liquidTypeID = *db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
		liquidObject.fishable = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 3);
		liquidObject.reflection = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 4);

		liquidObjects.idToIndexMap[liquidObject.id] = static_cast<u32>(liquidObjects.data.size()) - 1u;
	}

	db2Parser.RepopulateFromCopyTable(layout, liquidObjects.data, liquidObjects.idToIndexMap);

	size_t size = liquidObjects.GetSerializedSize();
	std::shared_ptr<Bytebuffer> resultBuffer = Bytebuffer::BorrowRuntime(size);

	fs::path path = ServiceLocator::GetRuntime()->paths.clientDB / "LiquidObject.cdb";
	FileWriter fileWriter(path, resultBuffer);

	if (!liquidObjects.Write(resultBuffer))
		return false;

	if (!fileWriter.Write())
		return false;

	return true;
}
bool ClientDBExtractor::ExtractLiquidType()
{
	CascLoader* cascLoader = ServiceLocator::GetCascLoader();

	DB2::WDC3::Layout layout = { };
	DB2::WDC3::Parser db2Parser = { };

	std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/liquidtype.db2");
	if (!buffer || !db2Parser.TryParse(buffer, layout))
		return false;

	const DB2::WDC3::Layout::Header& header = layout.header;

	liquidTypes.data.clear();
	liquidTypes.data.reserve(header.recordCount);
	liquidTypes.stringTable.Clear();
	liquidTypes.stringTable.Reserve(header.recordCount * 7);
	liquidTypes.idToIndexMap.reserve(header.recordCount);

	for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
	{
		u32 sectionID = 0;
		u32 recordID = 0;
		u8* recordData = nullptr;

		if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
			continue;

		DB::Client::Definitions::LiquidType& liquidType = liquidTypes.data.emplace_back();

		liquidType.id = db2Parser.GetRecordIDFromIndex(layout, db2RecordIndex);
		liquidType.name = GetStringIndexFromRecordIndex(layout, db2Parser, db2RecordIndex, 0, liquidTypes.stringTable);

		for (u32 i = 0; i < 6; i++)
            liquidType.textures[i] = GetStringIndexFromArrRecordIndex(layout, db2Parser, db2RecordIndex, 1, i, liquidTypes.stringTable);

		liquidType.flags = *db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
		liquidType.soundBank = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 3);
		liquidType.soundID = *db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
		liquidType.spellID = *db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 5);
		liquidType.maxDarkenDepth = *db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 6);
		liquidType.fogDarkenIntensity = *db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 7);
		liquidType.ambDarkenIntensity = *db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 8);
		liquidType.dirDarkenIntensity = *db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 9);
		liquidType.lightID = *db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 10);
		liquidType.particleScale = *db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 11);
		liquidType.particleMovement = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 12);
		liquidType.particleTexSlots = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 13);
		liquidType.materialID = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 14);
		liquidType.minimapStaticCol = *db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 15);

		const u8* frameCountTextures = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 16);
		for (u32 i = 0; i < 6; i++)
            liquidType.frameCountTextures[i] = frameCountTextures[i];

		const u32* colors = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 17);
		for (u32 i = 0; i < 2; i++)
			liquidType.colors[i] = colors[i];

		const f32* unkFloats = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 18);
		for (u32 i = 0; i < 16; i++)
			liquidType.unkFloats[i] = unkFloats[i];

		const u32* unkInts = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 19);
		for (u32 i = 0; i < 4; i++)
			liquidType.unkInts[i] = unkInts[i];

		const u32* coefficients = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 20);
		for (u32 i = 0; i < 4; i++)
			liquidType.coefficients[i] = coefficients[i];

		liquidTypes.idToIndexMap[liquidType.id] = static_cast<u32>(liquidTypes.data.size()) - 1u;
	}

	db2Parser.RepopulateFromCopyTable(layout, liquidObjects.data, liquidTypes.idToIndexMap);

	size_t size = liquidTypes.GetSerializedSize();
	std::shared_ptr<Bytebuffer> resultBuffer = Bytebuffer::BorrowRuntime(size);

	fs::path path = ServiceLocator::GetRuntime()->paths.clientDB / "LiquidType.cdb";
	FileWriter fileWriter(path, resultBuffer);

	if (!liquidTypes.Write(resultBuffer))
		return false;

	if (!fileWriter.Write())
		return false;

	return true;
}
bool ClientDBExtractor::ExtractLiquidMaterial()
{
	CascLoader* cascLoader = ServiceLocator::GetCascLoader();

	DB2::WDC3::Layout layout = { };
	DB2::WDC3::Parser db2Parser = { };

	std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/liquidmaterial.db2");
	if (!buffer || !db2Parser.TryParse(buffer, layout))
		return false;

	const DB2::WDC3::Layout::Header& header = layout.header;

	liquidMaterials.data.clear();
	liquidMaterials.data.reserve(header.recordCount);
	liquidMaterials.stringTable.Clear();
	liquidMaterials.idToIndexMap.reserve(header.recordCount);

	for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
	{
		u32 sectionID = 0;
		u32 recordID = 0;
		u8* recordData = nullptr;

		if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
			continue;

		DB::Client::Definitions::LiquidMaterial& liquidMaterial = liquidMaterials.data.emplace_back();

		liquidMaterial.id = db2Parser.GetRecordIDFromIndex(layout, db2RecordIndex);
		liquidMaterial.flags = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 0);
		liquidMaterial.liquidVertexFormat = *db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);

		liquidMaterials.idToIndexMap[liquidMaterial.id] = static_cast<u32>(liquidMaterials.data.size()) - 1u;
	}

	db2Parser.RepopulateFromCopyTable(layout, liquidObjects.data, liquidMaterials.idToIndexMap);

	size_t size = liquidMaterials.GetSerializedSize();
	std::shared_ptr<Bytebuffer> resultBuffer = Bytebuffer::BorrowRuntime(size);

	fs::path path = ServiceLocator::GetRuntime()->paths.clientDB / "LiquidMaterial.cdb";
	FileWriter fileWriter(path, resultBuffer);

	if (!liquidMaterials.Write(resultBuffer))
		return false;

	if (!fileWriter.Write())
		return false;

	return true;
}

bool ClientDBExtractor::ExtractCinematicCamera()
{
	CascLoader* cascLoader = ServiceLocator::GetCascLoader();

	DB2::WDC3::Layout layout = { };
	DB2::WDC3::Parser db2Parser = { };

	std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/cinematiccamera.db2");
	if (!buffer || !db2Parser.TryParse(buffer, layout))
		return false;

	const DB2::WDC3::Layout::Header& header = layout.header;

	cinematicCameras.data.clear();
	cinematicCameras.data.reserve(header.recordCount);
	cinematicCameras.stringTable.Clear();
	cinematicCameras.idToIndexMap.reserve(header.recordCount);

	for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
	{
		u32 sectionID = 0;
		u32 recordID = 0;
		u8* recordData = nullptr;

		if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
			continue;

		DB::Client::Definitions::CinematicCamera& cinematicCamera = cinematicCameras.data.emplace_back();

		cinematicCamera.id = db2Parser.GetRecordIDFromIndex(layout, db2RecordIndex);
		cinematicCamera.endPosition = CoordinateSpaces::CinematicCameraPosToNovus(*db2Parser.GetField<vec3>(layout, sectionID, recordID, recordData, 0));
		cinematicCamera.soundID = *db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
		cinematicCamera.rotation = *db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 2);
		cinematicCamera.cameraPath = *db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);

		u32 fileID = cinematicCamera.cameraPath;
		if (cascLoader->FileExistsInCasc(fileID))
		{
			const std::string& fileStr = cascLoader->GetFilePathFromListFileID(cinematicCamera.cameraPath);

			fs::path filePath = fs::path(fileStr).replace_extension(".complexmodel");
			u32 nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());

			cinematicCamera.cameraPath = nameHash;
		}
		else
		{
			cinematicCamera.cameraPath = std::numeric_limits<u32>::max();
		}

		cinematicCameras.idToIndexMap[cinematicCamera.id] = static_cast<u32>(cinematicCameras.data.size()) - 1u;
	}

	db2Parser.RepopulateFromCopyTable(layout, liquidObjects.data, cinematicCameras.idToIndexMap);

	size_t size = cinematicCameras.GetSerializedSize();
	std::shared_ptr<Bytebuffer> resultBuffer = Bytebuffer::BorrowRuntime(size);

	fs::path path = ServiceLocator::GetRuntime()->paths.clientDB / "CinematicCamera.cdb";
	FileWriter fileWriter(path, resultBuffer);

	if (!cinematicCameras.Write(resultBuffer))
		return false;

	if (!fileWriter.Write())
		return false;

	return true;
}

bool ClientDBExtractor::ExtractCinematicSequence()
{
	CascLoader* cascLoader = ServiceLocator::GetCascLoader();

	DB2::WDC3::Layout layout = { };
	DB2::WDC3::Parser db2Parser = { };

	std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/cinematicsequences.db2");
	if (!buffer || !db2Parser.TryParse(buffer, layout))
		return false;

	const DB2::WDC3::Layout::Header& header = layout.header;

	cinematicSequences.data.clear();
	cinematicSequences.data.reserve(header.recordCount);
	cinematicSequences.stringTable.Clear();
	cinematicSequences.idToIndexMap.reserve(header.recordCount);

	for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
	{
		u32 sectionID = 0;
		u32 recordID = 0;
		u8* recordData = nullptr;

		if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
			continue;

		DB::Client::Definitions::CinematicSequence& cinematicSequence = cinematicSequences.data.emplace_back();

		cinematicSequence.id = db2Parser.GetRecordIDFromIndex(layout, db2RecordIndex);
		cinematicSequence.soundID = *db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);

		const u16* cameraIDs = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 1);
		memcpy(&cinematicSequence.cameraIDs[0], cameraIDs, 8 * sizeof(u16));

		cinematicSequences.idToIndexMap[cinematicSequence.id] = static_cast<u32>(cinematicSequences.data.size()) - 1u;
	}

	db2Parser.RepopulateFromCopyTable(layout, liquidObjects.data, cinematicSequences.idToIndexMap);

	size_t size = cinematicSequences.GetSerializedSize();
	std::shared_ptr<Bytebuffer> resultBuffer = Bytebuffer::BorrowRuntime(size);

	fs::path path = ServiceLocator::GetRuntime()->paths.clientDB / "CinematicSequences.cdb";
	FileWriter fileWriter(path, resultBuffer);

	if (!cinematicSequences.Write(resultBuffer))
		return false;

	if (!fileWriter.Write())
		return false;

	return true;
}
