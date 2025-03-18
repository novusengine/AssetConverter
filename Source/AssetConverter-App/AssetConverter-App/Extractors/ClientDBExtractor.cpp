#include "ClientDBExtractor.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/ServiceLocator.h"

#include <Base/Container/StringTable.h>
#include <Base/Math/Geometry.h>
#include <Base/Memory/FileWriter.h>
#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <FileFormat/Shared.h>
#include <FileFormat/Warcraft/DB2/DB2Definitions.h>
#include <FileFormat/Warcraft/DB2/Wdc3.h>
#include <FileFormat/Warcraft/Parsers/Wdc3Parser.h>
#include <FileFormat/Novus/ClientDB/ClientDB.h>
#include <FileFormat/Novus/Model/ComplexModel.h>

#include <Meta/Generated/ClientDB.h>

#include <filesystem>
namespace fs = std::filesystem;

using namespace ClientDB;

std::vector<ClientDBExtractor::ExtractionEntry> ClientDBExtractor::_extractionEntries =
{
    { "ModelFileData",                      "A collection of Model File Data",                  ClientDBExtractor::ExtractModelFileData },
    { "TextureFileData",                    "A collection of Texture File Data",                ClientDBExtractor::ExtractTextureFileData },
    { "Map",				                "A collection of all maps",				            ClientDBExtractor::ExtractMap },
    { "LiquidObject",		                "A collection of liquid objects",		            ClientDBExtractor::ExtractLiquidObject },
    { "LiquidType",			                "A collection of liquid types",			            ClientDBExtractor::ExtractLiquidType },
    { "LiquidMaterial",		                "A collection of liquid materials",		            ClientDBExtractor::ExtractLiquidMaterial },
    { "CinematicCamera",	                "A collection of cinematic cameras",	            ClientDBExtractor::ExtractCinematicCamera },
    { "CinematicSequences",	                "A collection of cinematic sequences",	            ClientDBExtractor::ExtractCinematicSequence },
    { "AnimationData",	                    "A collection of Animation Data",	                ClientDBExtractor::ExtractAnimationData },
    { "CreatureModelData",                  "A collection of Creature Model Data",              ClientDBExtractor::ExtractCreatureModelData },
    { "CreatureDisplayInfo",                "A collection of Creature Display Info Data",	    ClientDBExtractor::ExtractCreatureDisplayInfo },
    { "CreatureDisplayInfoExtra",           "A collection of Creature Display Info Extra Data", ClientDBExtractor::ExtractCreatureDisplayInfoExtra },
    { "ItemDisplayInfoMaterialRes",         "A collection of Item Display Material Data",       ClientDBExtractor::ExtractItemDisplayMaterialResources },
    { "ItemDisplayInfoModelMatRes",         "A collection of Item Display Material Data",       ClientDBExtractor::ExtractItemDisplayModelMaterialResources },
    { "ItemDisplayInfo",                    "A collection of Item Display Data",                ClientDBExtractor::ExtractItemDisplayInfo },
    { "Light",                              "A collection of Light Data",                       ClientDBExtractor::ExtractLight },
    { "LightParams",                        "A collection of Light Parameter Data",             ClientDBExtractor::ExtractLightParams },
    { "LightData",                          "A collection of Light Data Data",                  ClientDBExtractor::ExtractLightData },
    { "LightSkybox",                        "A collection of Light Skybox Data",                ClientDBExtractor::ExtractLightSkybox }
};

ClientDB::Data ClientDBExtractor::modelFileDataStorage;
ClientDB::Data ClientDBExtractor::textureFileDataStorage;
ClientDB::Data ClientDBExtractor::mapStorage;
ClientDB::Data ClientDBExtractor::liquidObjectStorage;
ClientDB::Data ClientDBExtractor::liquidTypeStorage;
ClientDB::Data ClientDBExtractor::liquidMaterialStorage;
ClientDB::Data ClientDBExtractor::cinematicCameraStorage;
ClientDB::Data ClientDBExtractor::cinematicSequenceStorage;
ClientDB::Data ClientDBExtractor::animationDataStorage;
ClientDB::Data ClientDBExtractor::creatureModelDataStorage;
ClientDB::Data ClientDBExtractor::creatureDisplayInfoStorage;
ClientDB::Data ClientDBExtractor::creatureDisplayInfoExtraStorage;
ClientDB::Data ClientDBExtractor::itemDisplayMaterialResourcesStorage;
ClientDB::Data ClientDBExtractor::itemDisplayModelMaterialResourcesStorage;
ClientDB::Data ClientDBExtractor::itemDisplayInfoStorage;
ClientDB::Data ClientDBExtractor::lightStorage;
ClientDB::Data ClientDBExtractor::lightParamsStorage;
ClientDB::Data ClientDBExtractor::lightDataStorage;
ClientDB::Data ClientDBExtractor::lightSkyboxStorage;

robin_hood::unordered_map<u32, std::vector<u32>> ClientDBExtractor::modelResourcesIDToModelFileDataEntry;
robin_hood::unordered_map<u32, std::vector<u32>> ClientDBExtractor::materialResourcesIDToTextureFileDataEntry;

void ClientDBExtractor::Process()
{
    for (u32 i = 0; i < _extractionEntries.size(); i++)
    {
        const ExtractionEntry& entry = _extractionEntries[i];
    
        if (entry.function(entry.name))
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
void RepopulateFromCopyTable(const DB2::WDC3::Layout& db2, ClientDB::Data& storage)
{
    u32 numSections = static_cast<u32>(db2.sections.size());

    for (u32 i = 0; i < numSections; i++)
    {
        const DB2::WDC3::Layout::SectionHeader& sectionHeader = db2.sectionHeaders[i];
        const DB2::WDC3::Layout::Section& section = db2.sections[i];

        if (sectionHeader.copyTableCount == 0)
            continue;

        u32 numNewElements = sectionHeader.copyTableCount;
        storage.Reserve(numNewElements);

        for (u32 j = 0; j < sectionHeader.copyTableCount; j++)
        {
            const DB2::WDC3::Layout::CopyTableEntry& copyTableEntry = section.copyTable[j];

            bool didOverride = false;
            storage.Clone<T>(copyTableEntry.oldRowID, copyTableEntry.newRowID, didOverride);
        }
    }
}

std::string GetFilePathForDB2ByName(const std::string& name)
{
    std::string result = "dbfilesclient/" + name + ".db2";
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);

    return result;
}

bool ClientDBExtractor::ExtractModelFileData(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    modelFileDataStorage.Initialize<Generated::ModelFileDataRecord>();
    modelFileDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::ModelFileDataRecord modelFileData;
        u32 modelFileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        modelFileData.flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);
        modelFileData.modelResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        
        fs::path filePath = "";
        if (cascLoader->InCascAndListFile(modelFileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(modelFileID);
            filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
        }

        modelFileData.model = modelFileDataStorage.AddString(filePath.string());

        auto& modelFileDataEntries = modelResourcesIDToModelFileDataEntry[modelFileData.modelResourcesID];
        modelFileDataEntries.push_back(modelFileID);

        modelFileDataStorage.Replace(db2RecordIndex + 1, modelFileData);
    }

    RepopulateFromCopyTable<Generated::ModelFileDataRecord>(layout, modelFileDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!modelFileDataStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractTextureFileData(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    textureFileDataStorage.Initialize<Generated::TextureFileDataRecord>();
    textureFileDataStorage.Reserve(header.recordCount);
    materialResourcesIDToTextureFileDataEntry.reserve(header.recordCount * 2);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::TextureFileDataRecord textureFileData;
        u32 id = db2RecordIndex + 1;

        u32 textureFileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        textureFileData.materialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);

        fs::path filePath = "";
        if (cascLoader->InCascAndListFile(textureFileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(textureFileID);
            filePath = fs::path(fileStr).replace_extension("dds");
        }

        textureFileData.texture = textureFileDataStorage.AddString(filePath.string());

        auto& textureFileDataIDs = materialResourcesIDToTextureFileDataEntry[textureFileData.materialResourcesID];
        textureFileDataIDs.push_back(id);

        textureFileDataStorage.Replace(id, textureFileData);
    }

    RepopulateFromCopyTable<Generated::TextureFileDataRecord>(layout, textureFileDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!textureFileDataStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractMap(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    mapStorage.Initialize<Generated::MapRecord>();
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
            Generated::MapRecord map = { };

            std::string internalName = GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 0);;
            std::string mapName = GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 1);

            map.nameInternal = mapStorage.AddString(internalName);
            map.name = mapStorage.AddString(mapName);

            const u8 instanceType = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 7);
            map.instanceType = instanceType;

            const u32* flags = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 22);
            map.flags = flags[0];

            const u8 expansion = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 8);
            map.expansionID = expansion;

            const u8 maxPlayers = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 18);
            map.maxPlayers = maxPlayers;

            mapStorage.Replace(recordID, map);
        }
    }

    RepopulateFromCopyTable<Generated::MapRecord>(layout, mapStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!mapStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractLiquidObject(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    liquidObjectStorage.Initialize<Generated::LiquidObjectRecord>();
    liquidObjectStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::LiquidObjectRecord liquidObject;
        liquidObject.liquidTypeID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
        liquidObject.fishable = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 3);

        liquidObjectStorage.Replace(recordID, liquidObject);
    }

    RepopulateFromCopyTable<Generated::LiquidObjectRecord>(layout, liquidObjectStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!liquidObjectStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractLiquidType(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    liquidTypeStorage.Initialize<Generated::LiquidTypeRecord>();
    liquidTypeStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::LiquidTypeRecord liquidType;
        liquidType.name = liquidTypeStorage.AddString(GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 0));

        for (u32 textureIndex = 0; textureIndex < 6; textureIndex++)
        {
            liquidType.textures[textureIndex] = liquidTypeStorage.AddString(GetStringFromArrRecordIndex(layout, db2Parser, db2RecordIndex, 1, textureIndex));
        }

        liquidType.flags = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
        liquidType.soundBank = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 3);
        liquidType.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
        liquidType.maxDarkenDepth = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 6);
        liquidType.fogDarkenIntensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 7);
        liquidType.ambDarkenIntensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 8);
        liquidType.dirDarkenIntensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 9);
        liquidType.lightID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 10);
        liquidType.particleScale = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 11);
        liquidType.particleMovement = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 12);
        liquidType.particleTextureSlot = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 13);
        liquidType.materialID = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 14);
        liquidType.minimapColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 15);

        const u8* frameCountTextures = db2Parser.GetFieldPtr<u8>(layout, sectionID, recordID, recordData, 16);
        memcpy(&liquidType.frameCounts[0], frameCountTextures, sizeof(u8) * 6);

        const f32* unkFloats = db2Parser.GetFieldPtr<f32>(layout, sectionID, recordID, recordData, 18);
        memcpy(&liquidType.unkFloats[0], unkFloats, sizeof(f32) * 16);

        const u32* unkInts = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 19);
        memcpy(&liquidType.unkInts[0], unkInts, sizeof(i32) * 4);

        liquidTypeStorage.Replace(recordID, liquidType);
    }

    RepopulateFromCopyTable<Generated::LiquidTypeRecord>(layout, liquidTypeStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!liquidTypeStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractLiquidMaterial(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    liquidMaterialStorage.Initialize<Generated::LiquidMaterialRecord>();
    liquidMaterialStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::LiquidMaterialRecord liquidMaterial;
        liquidMaterial.flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 0);
        liquidMaterial.liquidVertexFormat = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);

        liquidMaterialStorage.Replace(recordID, liquidMaterial);
    }

    RepopulateFromCopyTable<Generated::LiquidMaterialRecord>(layout, liquidMaterialStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!liquidMaterialStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractCinematicCamera(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    cinematicCameraStorage.Initialize<Generated::CinematicCameraRecord>();
    cinematicCameraStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::CinematicCameraRecord cinematicCamera;
        const vec3* endPosition = db2Parser.GetFieldPtr<vec3>(layout, sectionID, recordID, recordData, 0);
        cinematicCamera.endPosition = CoordinateSpaces::CinematicCameraPosToNovus(*endPosition);
        cinematicCamera.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        cinematicCamera.rotation = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 2);

        fs::path filePath = "";
        u32 fileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        if (cascLoader->InCascAndListFile(fileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(fileID);
            filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
        }
        cinematicCamera.model = cinematicCameraStorage.AddString(filePath.string());

        cinematicCameraStorage.Replace(recordID, cinematicCamera);
    }

    RepopulateFromCopyTable<Generated::CinematicCameraRecord>(layout, cinematicCameraStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!cinematicCameraStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractCinematicSequence(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    cinematicSequenceStorage.Initialize<Generated::CinematicSequenceRecord>();
    cinematicSequenceStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::CinematicSequenceRecord cinematicSequence;
        const u16* cameraIDs = db2Parser.GetFieldPtr<u16>(layout, sectionID, recordID, recordData, 1);
        cinematicSequence.cameraID = cameraIDs[0];

        cinematicSequenceStorage.Replace(recordID, cinematicSequence);
    }

    RepopulateFromCopyTable<Generated::CinematicSequenceRecord>(layout, cinematicSequenceStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!cinematicSequenceStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractAnimationData(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    animationDataStorage.Initialize<Generated::AnimationDataRecord>();
    animationDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::AnimationDataRecord animationData;
        animationData.fallback = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 0);
        animationData.behaviorTier = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);
        animationData.behaviorID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);

        const u32* flags = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 3);
        animationData.flags = flags[0] | static_cast<u64>(flags[1]) << 32;

        animationDataStorage.Replace(recordID, animationData);
    }

    RepopulateFromCopyTable<Generated::AnimationDataRecord>(layout, animationDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!animationDataStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractCreatureModelData(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    creatureModelDataStorage.Initialize<Generated::CreatureModelDataRecord>();
    creatureModelDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::CreatureModelDataRecord creatureModelData;
        const Geometry::AABoundingBox* boundingBox = db2Parser.GetFieldPtr<Geometry::AABoundingBox>(layout, sectionID, recordID, recordData, 0);
        creatureModelData.boxMin = boundingBox->center;
        creatureModelData.boxMax = boundingBox->extents;

        creatureModelData.flags = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);

        u32 fileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);
        creatureModelData.bloodID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        creatureModelData.footprintTextureID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
        creatureModelData.footprintTextureLength = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 5);
        creatureModelData.footprintTextureWidth = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 6);
        creatureModelData.footprintParticleScale = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 7);
        creatureModelData.footstepCameraEffectID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 9);
        creatureModelData.deathThudCameraEffectID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 10);
        creatureModelData.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 11);
        creatureModelData.sizeClass = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 12);
        creatureModelData.collisionBox.x = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 13);
        creatureModelData.collisionBox.y = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 14);
        creatureModelData.mountHeight = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 23);

        fs::path filePath = "";
        if (cascLoader->InCascAndListFile(fileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(fileID);
            filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
        }

        creatureModelData.model = creatureModelDataStorage.AddString(filePath.string());

        creatureModelDataStorage.Replace(recordID, creatureModelData);
    }

    RepopulateFromCopyTable<Generated::CreatureModelDataRecord>(layout, creatureModelDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!creatureModelDataStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractCreatureDisplayInfo(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    creatureDisplayInfoStorage.Initialize<Generated::CreatureDisplayInfoRecord>();
    creatureDisplayInfoStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::CreatureDisplayInfoRecord creatureDisplayInfo;
        recordID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        creatureDisplayInfo.modelID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 1);
        creatureDisplayInfo.soundID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
        creatureDisplayInfo.sizeClass = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 3);
        creatureDisplayInfo.creatureModelScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 4);
        creatureDisplayInfo.creatureModelAlpha = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 5);
        creatureDisplayInfo.bloodID = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 6);
        creatureDisplayInfo.extendedDisplayInfoID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 7);
        creatureDisplayInfo.npcSoundID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 8);
        creatureDisplayInfo.flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 14);
        creatureDisplayInfo.creaturePetScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 17);
        creatureDisplayInfo.unarmedWeaponType = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 18);
        creatureDisplayInfo.gender = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 21);

        const u32* textureVariationFileIDs = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 25);

        fs::path filePath = "";

        for (u32 textureVariantIndex = 0; textureVariantIndex < 4; textureVariantIndex++)
        {
            filePath.clear();

            u32 textureFileID = textureVariationFileIDs[textureVariantIndex];
            if (textureFileID > 0 && cascLoader->InCascAndListFile(textureFileID))
            {
                const std::string& fileStr = cascLoader->GetFilePathFromListFileID(textureFileID);
                filePath = fs::path(fileStr).replace_extension("dds");
                creatureDisplayInfo.textureVariations[textureVariantIndex] = creatureDisplayInfoStorage.AddString(filePath.string());
            }

            creatureDisplayInfo.textureVariations[textureVariantIndex] = creatureDisplayInfoStorage.AddString(filePath.string());
        }

        creatureDisplayInfoStorage.Replace(recordID, creatureDisplayInfo);
    }

    RepopulateFromCopyTable<Generated::CreatureDisplayInfoRecord>(layout, creatureDisplayInfoStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!creatureDisplayInfoStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractCreatureDisplayInfoExtra(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    creatureDisplayInfoExtraStorage.Initialize<Generated::CreatureDisplayInfoExtraRecord>();
    creatureDisplayInfoExtraStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::CreatureDisplayInfoExtraRecord creatureDisplayInfoExtra;
        recordID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        creatureDisplayInfoExtra.raceID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 1);
        creatureDisplayInfoExtra.gender = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 2) + 1;
        creatureDisplayInfoExtra.classID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 3);
        creatureDisplayInfoExtra.skinID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 4);
        creatureDisplayInfoExtra.faceID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 5);
        creatureDisplayInfoExtra.hairStyleID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 6);
        creatureDisplayInfoExtra.hairColorID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 7);
        creatureDisplayInfoExtra.facialHairID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 8);
        creatureDisplayInfoExtra.flags = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 9);

        fs::path filePath = "";
        u32 bakedMaterialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 10);
        if (materialResourcesIDToTextureFileDataEntry.contains(bakedMaterialResourcesID))
        {
            u32 textureFileDataID = materialResourcesIDToTextureFileDataEntry[bakedMaterialResourcesID][0];
            
            auto& textureFileData = textureFileDataStorage.Get<Generated::TextureFileDataRecord>(textureFileDataID);
            filePath = textureFileDataStorage.GetString(textureFileData.texture);
        }
        creatureDisplayInfoExtra.bakedTexture = creatureDisplayInfoExtraStorage.AddString(filePath.string());

        bool didOverride = false;
        creatureDisplayInfoExtraStorage.Replace(recordID, creatureDisplayInfoExtra, didOverride);
    }

    RepopulateFromCopyTable<Generated::CreatureDisplayInfoExtraRecord>(layout, creatureDisplayInfoExtraStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!creatureDisplayInfoExtraStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractItemDisplayMaterialResources(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    itemDisplayMaterialResourcesStorage.Initialize<Generated::ItemDisplayInfoMaterialResourceRecord>();
    itemDisplayMaterialResourcesStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::ItemDisplayInfoMaterialResourceRecord itemDisplayMaterialResource;
        itemDisplayMaterialResource.displayInfoID = 0;
        u8 componentSection = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 0);
        itemDisplayMaterialResource.materialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);

        switch (componentSection)
        {
            case 0: // ArmUpper
            {
                componentSection = 5;
                break;
            }
            case 1: // ArmLower
            {
                componentSection = 6;
                break;
            }
            case 2: // Hand
            {
                componentSection = 7;
                break;
            }
            case 3: // TorsoUpper
            {
                componentSection = 3;
                break;
            }
            case 4: // TorsoLower
            {
                componentSection = 4;
                break;
            }
            case 5: // LegUpper
            {
                componentSection = 8;
                break;
            }
            case 6: // LegLower
            {
                componentSection = 9;
                break;
            }
            case 7: // Foot
            {
                componentSection = 10;
                break;
            }
            case 9: // ScalpUpper
            {
                componentSection = 1;
                break;
            }
            case 10: // ScalpLower
            {
                componentSection = 2;
                break;
            }

            default:
            {
                componentSection = 255;
            }
        }

        itemDisplayMaterialResource.componentSection = componentSection;
        itemDisplayMaterialResourcesStorage.Replace(recordID, itemDisplayMaterialResource);
    }

    for (u32 i = 0; i < layout.sections[0].relationshipMap.entriesCount; i++)
    {
        DB2::WDC3::Layout::RelationshipMapEntry* relationshipEntry = layout.sections[0].relationshipMap.entries + i;

        u32 rowID = *(layout.sections[0].idListData + relationshipEntry->recordIndex);
        auto& itemDisplayMaterialResource = itemDisplayMaterialResourcesStorage.Get<Generated::ItemDisplayInfoMaterialResourceRecord>(rowID);
        itemDisplayMaterialResource.displayInfoID = relationshipEntry->foreignID;
    }

    RepopulateFromCopyTable<Generated::ItemDisplayInfoMaterialResourceRecord>(layout, itemDisplayMaterialResourcesStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!itemDisplayMaterialResourcesStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractItemDisplayModelMaterialResources(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    itemDisplayModelMaterialResourcesStorage.Initialize<Generated::ItemDisplayInfoModelMaterialResourceRecord>();
    itemDisplayModelMaterialResourcesStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::ItemDisplayInfoModelMaterialResourceRecord itemDisplayModelMaterialResource;
        itemDisplayModelMaterialResource.displayInfoID = 0;
        itemDisplayModelMaterialResource.modelIndex = static_cast<u8>(db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2));
        itemDisplayModelMaterialResource.textureType = static_cast<u8>(db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1));
        itemDisplayModelMaterialResource.materialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);

        itemDisplayModelMaterialResourcesStorage.Replace(recordID, itemDisplayModelMaterialResource);
    }

    for (u32 i = 0; i < layout.sections[0].relationshipMap.entriesCount; i++)
    {
        DB2::WDC3::Layout::RelationshipMapEntry* relationshipEntry = layout.sections[0].relationshipMap.entries + i;

        u32 rowID = *(layout.sections[0].idListData + relationshipEntry->recordIndex);
        auto& itemDisplayModelMaterialResource = itemDisplayModelMaterialResourcesStorage.Get<Generated::ItemDisplayInfoModelMaterialResourceRecord>(rowID);
        itemDisplayModelMaterialResource.displayInfoID = relationshipEntry->foreignID;
    }

    RepopulateFromCopyTable<Generated::ItemDisplayInfoModelMaterialResourceRecord>(layout, itemDisplayModelMaterialResourcesStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!itemDisplayModelMaterialResourcesStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractItemDisplayInfo(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    itemDisplayInfoStorage.Initialize<Generated::ItemDisplayInfoRecord>();
    itemDisplayInfoStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::ItemDisplayInfoRecord itemDisplayInfo;

        itemDisplayInfo.itemRangedDisplayInfoID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        itemDisplayInfo.flags = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 9);

        // modelResourcesID
        const u32* modelResourceIDs = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 10);
        memcpy(&itemDisplayInfo.modelResourcesID[0], modelResourceIDs, 2 * sizeof(u32));

        // materialResourcesID
        const u32* materialResourcesIDs = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 11);
        memcpy(&itemDisplayInfo.modelMaterialResourcesID[0], materialResourcesIDs, 2 * sizeof(u32));

        // goesetGroup
        const u32* goesetGroups = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 13);
        for (u32 geosetGroupIndex = 0; geosetGroupIndex < 6; geosetGroupIndex++)
        {
            itemDisplayInfo.modelGeosetGroups[geosetGroupIndex] = static_cast<u8>(goesetGroups[geosetGroupIndex]);
        }

        // geosetHelmetVis
        const u32* geosetHelmetVis = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 15);
        itemDisplayInfo.modelGeosetVisIDs[0] = static_cast<u16>(geosetHelmetVis[0]);
        itemDisplayInfo.modelGeosetVisIDs[1] = static_cast<u16>(geosetHelmetVis[1]);

        itemDisplayInfoStorage.Replace(recordID, itemDisplayInfo);
    }

    RepopulateFromCopyTable<Generated::ItemDisplayInfoRecord>(layout, itemDisplayInfoStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!itemDisplayInfoStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractLight(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    lightStorage.Initialize<Generated::LightRecord>();
    lightStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::LightRecord light;
        light.mapID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 3);

        vec3 position = *db2Parser.GetFieldPtr<vec3>(layout, sectionID, recordID, recordData, 0);
        light.position = CoordinateSpaces::TerrainPosToNovus(position);
        light.fallOff.x = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 1);
        light.fallOff.y = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 2);

        const u16* lightParamIDs = db2Parser.GetFieldPtr<u16>(layout, sectionID, recordID, recordData, 4);
        memcpy(&light.paramIDs[0], lightParamIDs, 8 * sizeof(u16));

        lightStorage.Replace(recordID, light);
    }

    RepopulateFromCopyTable<Generated::LightRecord>(layout, lightStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractLightParams(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    lightParamsStorage.Initialize<Generated::LightParamRecord>();
    lightParamsStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::LightParamRecord lightParam;
        recordID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        bool highlightSky = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 2);
        lightParam.flags = 1 << 0 * highlightSky;
        lightParam.lightSkyboxID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 3);
        lightParam.glow = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 5);
        lightParam.riverShallowAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 6);
        lightParam.riverDeepAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 7);
        lightParam.oceanShallowAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 8);
        lightParam.oceanDeepAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 9);

        lightParamsStorage.Replace(db2RecordIndex, lightParam);
    }

    RepopulateFromCopyTable<Generated::LightParamRecord>(layout, lightParamsStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightParamsStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractLightData(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    lightDataStorage.Initialize<Generated::LightDataRecord>();
    lightDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::LightDataRecord lightData;
        lightData.lightParamID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 0);
        u16 timestamp = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 1);
        lightData.timestamp = static_cast<u32>(timestamp) * 30;
        lightData.diffuseColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);
        lightData.ambientColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        lightData.skyTopColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
        lightData.skyMiddleColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 5);
        lightData.skyBand1Color = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 6);
        lightData.skyBand2Color = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 7);
        lightData.skySmogColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 8);
        lightData.skyFogColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 9);
        lightData.sunColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 10);
        lightData.sunFogColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 38);
        lightData.sunFogStrength = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 39);
        lightData.sunFogAngle = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 29);
        lightData.cloudSunColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 11);
        lightData.cloudEmissiveColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 12);
        lightData.cloudLayer1AmbientColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 13);
        lightData.cloudLayer2AmbientColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 14);
        lightData.oceanShallowColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 15);
        lightData.oceanDeepColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 16);
        lightData.riverShallowColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 17);
        lightData.riverDeepColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 18);
        lightData.shadowColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 19);
        lightData.fogEnd = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 20) / 36;
        lightData.fogScaler = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 21);
        lightData.fogDensity = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 22);
        lightData.cloudDensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 30);
        lightData.fogHeightColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        lightData.fogEndColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 35);
        lightData.fogEndHeightColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 41);

        lightDataStorage.Replace(recordID, lightData);
    }

    RepopulateFromCopyTable<Generated::LightDataRecord>(layout, lightDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightDataStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractLightSkybox(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    lightSkyboxStorage.Initialize<Generated::LightSkyboxRecord>();
    lightSkyboxStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Generated::LightSkyboxRecord lightSkybox;

        fs::path filePath = "";

        std::string skyboxName = GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 0);
        std::transform(skyboxName.begin(), skyboxName.end(), skyboxName.begin(), ::tolower);

        u8 flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);
        u32 fileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);

        if ((flags & 0x2) == 0 || fileID > 0)
        {
            if (fileID == 0)
            {
                filePath = fs::path(skyboxName);
            }
            else
            {
                if (cascLoader->InCascAndListFile(fileID))
                {
                    const std::string& fileStr = cascLoader->GetFilePathFromListFileID(fileID);
                    filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
                }
            }

        }

        lightSkybox.model = lightSkyboxStorage.AddString(filePath.string());
        lightSkybox.name = lightSkyboxStorage.AddString(fs::path(skyboxName).filename().replace_extension("").string());
        lightSkyboxStorage.Replace(recordID, lightSkybox);
    }

    RepopulateFromCopyTable<Generated::LightSkyboxRecord>(layout, lightSkyboxStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightSkyboxStorage.Save(path))
        return false;

    return true;
}