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
#include <FileFormat/Novus/Model/ComplexModel.h>

#include <filesystem>
namespace fs = std::filesystem;

using namespace ClientDB;

std::vector<ClientDBExtractor::ExtractionEntry> ClientDBExtractor::_extractionEntries =
{
    { "Map.db2",				"A collection of all maps",				ClientDBExtractor::ExtractMap },
    { "LiquidObject.db2",		"A collection of liquid objects",		ClientDBExtractor::ExtractLiquidObject },
    { "LiquidType.db2",			"A collection of liquid types",			ClientDBExtractor::ExtractLiquidType },
    { "LiquidMaterial.db2",		"A collection of liquid materials",		ClientDBExtractor::ExtractLiquidMaterial },
    { "CinematicCamera.db2",	"A collection of cinematic cameras",	ClientDBExtractor::ExtractCinematicCamera },
    { "CinematicSequences.db2",	"A collection of cinematic sequences",	ClientDBExtractor::ExtractCinematicSequence }
};

ClientDB::Storage<ClientDB::Definitions::Map> ClientDBExtractor::mapStorage("Map");
ClientDB::Storage<ClientDB::Definitions::LiquidObject> ClientDBExtractor::liquidObjectStorage("LiquidObject");
ClientDB::Storage<ClientDB::Definitions::LiquidType> ClientDBExtractor::liquidTypeStorage("LiquidType");
ClientDB::Storage<ClientDB::Definitions::LiquidMaterial> ClientDBExtractor::liquidMaterialStorage("LiquidMaterial");
ClientDB::Storage<ClientDB::Definitions::CinematicCamera> ClientDBExtractor::cinematicCameraStorage("CinematicCamera");
ClientDB::Storage<ClientDB::Definitions::CinematicSequence> ClientDBExtractor::cinematicSequenceStorage("CinematicSequences");

void ClientDBExtractor::Process()
{
    for (u32 i = 0; i < _extractionEntries.size(); i++)
    {
        const ExtractionEntry& entry = _extractionEntries[i];

        if (entry.function())
        {
            NC_LOG_INFO("[ClientDBExtractor] Extracted (\"{0}\" : \"{1}\")", entry.name, entry.description);
        }
        else
        {
            NC_LOG_WARNING("[ClientDBExtractor] Failed to extract (\"{0}\" : \"{1}\")", entry.name, entry.description);
        }
    }
}

void FixPathExtension(std::string& path)
{
    if (path.length() == 0)
        return;

    if (StringUtils::EndsWith(path, ".mdx"))
    {
        path = path.substr(0, path.length() - 4) + Model::FILE_EXTENSION;
    }
    else if (StringUtils::EndsWith(path, ".m2"))
    {
        path = path.substr(0, path.length() - 3) + Model::FILE_EXTENSION;
    }
    else if (StringUtils::EndsWith(path, ".blp"))
    {
        path = path.substr(0, path.length() - 4) + ".dds";
        std::transform(path.begin(), path.end(), path.begin(), ::tolower);
    }
}

std::string GetStringFromRecordIndex(DB2::WDC3::Layout& layout, DB2::WDC3::Parser& db2Parser, u32 recordIndex, u32 fieldIndex)
{
    std::string value = db2Parser.GetString(layout, recordIndex, fieldIndex);

    FixPathExtension(value);

    std::replace(value.begin(), value.end(), '\\', '/');

    return value;
}
std::string GetStringFromArrRecordIndex(DB2::WDC3::Layout& layout, DB2::WDC3::Parser& db2Parser, u32 recordIndex, u32 fieldIndex, u32 arrIndex)
{
    std::string value = db2Parser.GetStringInArr(layout, recordIndex, fieldIndex, arrIndex);

    FixPathExtension(value);

    std::replace(value.begin(), value.end(), '\\', '/');

    return value;
}

template <typename T>
void RepopulateFromCopyTable(const DB2::WDC3::Layout& db2, Storage<T>& storage)
{
    u32 numSections = static_cast<u32>(db2.sections.size());

    for (u32 i = 0; i < numSections; i++)
    {
        const DB2::WDC3::Layout::SectionHeader& sectionHeader = db2.sectionHeaders[i];
        const DB2::WDC3::Layout::Section& section = db2.sections[i];

        if (sectionHeader.copyTableCount == 0)
            continue;

        u32 numElements = storage.Count();
        u32 numNewElements = sectionHeader.copyTableCount;

        storage.Reserve(numElements + numNewElements);

        for (u32 j = 0; j < sectionHeader.copyTableCount; j++)
        {
            const DB2::WDC3::Layout::CopyTableEntry& copyTableEntry = section.copyTable[j];
            storage.CopyRow(copyTableEntry.oldRowID, copyTableEntry.newRowID);
        }
    }
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
    mapStorage.Reserve(header.recordCount);

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

        bool hasWDTFile = fileID > 0 && cascLoader->InCascAndListFile(fileID);
        if (hasWDTFile)
        {
            Definitions::Map map = { };

            map.id = recordID;
            map.name = GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 1);
            map.internalName = GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 0);

            const u8 instanceType = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 7);
            map.instanceType = instanceType;

            const u32* flags = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 21);
            map.flags = flags[0];

            const u8 expansion = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 8);
            map.expansion = expansion;

            const u8 maxPlayers = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 18);
            map.maxPlayers = maxPlayers;

            mapStorage.AddRow(map);
        }
    }

    RepopulateFromCopyTable(layout, mapStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / mapStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!mapStorage.Save(path))
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
    liquidObjectStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LiquidObject liquidObject;
        liquidObject.id = recordID;
        liquidObject.flowDirection = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 0);
        liquidObject.flowSpeed = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 1);
        liquidObject.liquidTypeID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
        liquidObject.fishable = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 3);
        liquidObject.reflection = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 4);

        liquidObjectStorage.AddRow(liquidObject);
    }

    RepopulateFromCopyTable(layout, liquidObjectStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / liquidObjectStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!liquidObjectStorage.Save(path))
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
    liquidTypeStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LiquidType liquidType;

        liquidType.id = recordID;
        liquidType.name = GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 0);

        for (u32 i = 0; i < 6; i++)
            liquidType.textures[i] = GetStringFromArrRecordIndex(layout, db2Parser, db2RecordIndex, 1, i);

        liquidType.flags = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
        liquidType.soundBank = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 3);
        liquidType.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
        liquidType.spellID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 5);
        liquidType.maxDarkenDepth = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 6);
        liquidType.fogDarkenIntensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 7);
        liquidType.ambDarkenIntensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 8);
        liquidType.dirDarkenIntensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 9);
        liquidType.lightID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 10);
        liquidType.particleScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 11);
        liquidType.particleMovement = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 12);
        liquidType.particleTexSlots = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 13);
        liquidType.materialID = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 14);
        liquidType.minimapStaticCol = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 15);

        const u8* frameCountTextures = db2Parser.GetFieldPtr<u8>(layout, sectionID, recordID, recordData, 16);
        for (u32 i = 0; i < 6; i++)
            liquidType.frameCountTextures[i] = frameCountTextures[i];

        const u32* colors = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 17);
        for (u32 i = 0; i < 2; i++)
            liquidType.colors[i] = colors[i];

        const f32* unkFloats = db2Parser.GetFieldPtr<f32>(layout, sectionID, recordID, recordData, 18);
        for (u32 i = 0; i < 16; i++)
            liquidType.unkFloats[i] = unkFloats[i];

        const u32* unkInts = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 19);
        for (u32 i = 0; i < 4; i++)
            liquidType.unkInts[i] = unkInts[i];

        const u32* coefficients = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 20);
        for (u32 i = 0; i < 4; i++)
            liquidType.coefficients[i] = coefficients[i];

        liquidTypeStorage.AddRow(liquidType);
    }

    RepopulateFromCopyTable(layout, liquidTypeStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / liquidTypeStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!liquidTypeStorage.Save(path))
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

    liquidMaterialStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LiquidMaterial liquidMaterial;
        liquidMaterial.id = recordID;
        liquidMaterial.flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 0);
        liquidMaterial.liquidVertexFormat = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);

        liquidMaterialStorage.AddRow(liquidMaterial);
    }

    RepopulateFromCopyTable(layout, liquidMaterialStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / liquidMaterialStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!liquidMaterialStorage.Save(path))
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

    cinematicCameraStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CinematicCamera cinematicCamera;
        cinematicCamera.id = recordID;

        const f32* endPosition = db2Parser.GetFieldPtr<f32>(layout, sectionID, recordID, recordData, 0);
        cinematicCamera.endPosition = CoordinateSpaces::CinematicCameraPosToNovus(vec3(endPosition[0], endPosition[1], endPosition[2]));
        cinematicCamera.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        cinematicCamera.rotation = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 2);
        cinematicCamera.cameraPath = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);

        u32 fileID = cinematicCamera.cameraPath;
        if (cascLoader->InCascAndListFile(fileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(cinematicCamera.cameraPath);

            fs::path filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
            u32 nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());

            cinematicCamera.cameraPath = nameHash;
        }
        else
        {
            cinematicCamera.cameraPath = std::numeric_limits<u32>::max();
        }

        cinematicCameraStorage.AddRow(cinematicCamera);
    }

    RepopulateFromCopyTable(layout, cinematicCameraStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / cinematicCameraStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!cinematicCameraStorage.Save(path))
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

    cinematicSequenceStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CinematicSequence cinematicSequence;
        cinematicSequence.id = recordID;
        cinematicSequence.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);

        const u16* cameraIDs = db2Parser.GetFieldPtr<u16>(layout, sectionID, recordID, recordData, 1);
        memcpy(&cinematicSequence.cameraIDs[0], cameraIDs, 8 * sizeof(u16));

        cinematicSequenceStorage.AddRow(cinematicSequence);
    }

    RepopulateFromCopyTable(layout, cinematicSequenceStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / cinematicSequenceStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!cinematicSequenceStorage.Save(path))
        return false;

    return true;
}
