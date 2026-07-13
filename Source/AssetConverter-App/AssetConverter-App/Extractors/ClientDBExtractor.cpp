#include "ClientDBExtractor.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/ServiceLocator.h"

#include <Base/Container/StringTable.h>
#include <Base/Math/Geometry.h>
#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <FileFormat/Shared.h>
#include <FileFormat/Novus/FileHeader.h>
#include <FileFormat/Warcraft/DB2/DB2Definitions.h>
#include <FileFormat/Warcraft/DB2/Wdc3.h>
#include <FileFormat/Warcraft/Parsers/Wdc3Parser.h>
#include <FileFormat/Novus/ClientDB/ClientDB.h>
#include <FileFormat/Novus/Model/ComplexModel.h>

#include <MetaGen/Shared/ClientDB/ClientDB.h>

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

bool ClientDBExtractor::LoadMapStorage(bool loadLiquidData)
{
    const bool mapLoaded = ExtractMapData("Map", false);
    if (!loadLiquidData)
        return mapLoaded;

    const bool liquidObjectLoaded = ExtractLiquidObjectData("LiquidObject", false);
    const bool liquidTypeLoaded = ExtractLiquidTypeData("LiquidType", false);
    const bool liquidMaterialLoaded = ExtractLiquidMaterialData("LiquidMaterial", false);
    return mapLoaded && liquidObjectLoaded && liquidTypeLoaded && liquidMaterialLoaded;
}

void FixPathExtension(std::string& path)
{
    if (path.length() == 0)
        return;

    if (StringUtils::EndsWith(path, ".mdx"))
    {
        path = "model/" + path.substr(0, path.length() - 4) + Model::FILE_EXTENSION;
        std::transform(path.begin(), path.end(), path.begin(), ::tolower);
    }
    else if (StringUtils::EndsWith(path, ".m2"))
    {
        path = "model/" + path.substr(0, path.length() - 3) + Model::FILE_EXTENSION;
        std::transform(path.begin(), path.end(), path.begin(), ::tolower);
    }
    else if (StringUtils::EndsWith(path, ".blp"))
    {
        path = "texture/" + path.substr(0, path.length() - 4) + ".dds";
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

    modelFileDataStorage.Initialize<MetaGen::Shared::ClientDB::ModelFileDataRecord>();
    modelFileDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::ModelFileDataRecord modelFileData;
        u32 modelFileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        modelFileData.flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);
        modelFileData.modelResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);

        fs::path filePath = "";
        if (cascLoader->InCascAndListFile(modelFileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(modelFileID);
            filePath = fs::path("model") / fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
        }

        modelFileData.model = modelFileDataStorage.AddString(filePath.generic_string());

        auto& modelFileDataEntries = modelResourcesIDToModelFileDataEntry[modelFileData.modelResourcesID];
        modelFileDataEntries.push_back(modelFileID);

        modelFileDataStorage.Replace(db2RecordIndex + 1, modelFileData);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::ModelFileDataRecord>(layout, modelFileDataStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = modelFileDataStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!modelFileDataStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    textureFileDataStorage.Initialize<MetaGen::Shared::ClientDB::TextureFileDataRecord>();
    textureFileDataStorage.Reserve(header.recordCount);
    materialResourcesIDToTextureFileDataEntry.reserve(header.recordCount * 2);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::TextureFileDataRecord textureFileData;
        u32 id = db2RecordIndex + 1;

        u32 textureFileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        textureFileData.materialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);

        fs::path filePath = "";
        if (cascLoader->InCascAndListFile(textureFileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(textureFileID);
            filePath = fs::path("texture") / fs::path(fileStr).replace_extension("dds");
        }

        textureFileData.texture = textureFileDataStorage.AddString(filePath.generic_string());

        auto& textureFileDataIDs = materialResourcesIDToTextureFileDataEntry[textureFileData.materialResourcesID];
        textureFileDataIDs.push_back(id);

        textureFileDataStorage.Replace(id, textureFileData);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::TextureFileDataRecord>(layout, textureFileDataStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = textureFileDataStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!textureFileDataStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
}

bool ClientDBExtractor::ExtractMap(const std::string& name)
{
    return ExtractMapData(name, true);
}

bool ClientDBExtractor::ExtractMapData(const std::string& name, bool writePact)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    mapStorage.Initialize<MetaGen::Shared::ClientDB::MapRecord>();
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
            MetaGen::Shared::ClientDB::MapRecord map = { };

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

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::MapRecord>(layout, mapStorage);

    if (!writePact)
        return true;

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = mapStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!mapStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
}

bool ClientDBExtractor::ExtractLiquidObject(const std::string& name)
{
    return ExtractLiquidObjectData(name, true);
}

bool ClientDBExtractor::ExtractLiquidObjectData(const std::string& name, bool writePact)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    liquidObjectStorage.Initialize<MetaGen::Shared::ClientDB::LiquidObjectRecord>();
    liquidObjectStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::LiquidObjectRecord liquidObject;
        liquidObject.liquidTypeID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
        liquidObject.fishable = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 3);

        liquidObjectStorage.Replace(recordID, liquidObject);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::LiquidObjectRecord>(layout, liquidObjectStorage);

    if (!writePact)
        return true;

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = liquidObjectStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!liquidObjectStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
}

bool ClientDBExtractor::ExtractLiquidType(const std::string& name)
{
    return ExtractLiquidTypeData(name, true);
}

bool ClientDBExtractor::ExtractLiquidTypeData(const std::string& name, bool writePact)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    liquidTypeStorage.Initialize<MetaGen::Shared::ClientDB::LiquidTypeRecord>();
    liquidTypeStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::LiquidTypeRecord liquidType;
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

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::LiquidTypeRecord>(layout, liquidTypeStorage);

    if (!writePact)
        return true;

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = liquidTypeStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!liquidTypeStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
}

bool ClientDBExtractor::ExtractLiquidMaterial(const std::string& name)
{
    return ExtractLiquidMaterialData(name, true);
}

bool ClientDBExtractor::ExtractLiquidMaterialData(const std::string& name, bool writePact)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    liquidMaterialStorage.Initialize<MetaGen::Shared::ClientDB::LiquidMaterialRecord>();
    liquidMaterialStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::LiquidMaterialRecord liquidMaterial;
        liquidMaterial.flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 0);
        liquidMaterial.liquidVertexFormat = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);

        liquidMaterialStorage.Replace(recordID, liquidMaterial);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::LiquidMaterialRecord>(layout, liquidMaterialStorage);

    if (!writePact)
        return true;

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = liquidMaterialStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!liquidMaterialStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    cinematicCameraStorage.Initialize<MetaGen::Shared::ClientDB::CinematicCameraRecord>();
    cinematicCameraStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::CinematicCameraRecord cinematicCamera;
        const vec3* endPosition = db2Parser.GetFieldPtr<vec3>(layout, sectionID, recordID, recordData, 0);
        cinematicCamera.endPosition = CoordinateSpaces::CinematicCameraPosToNovus(*endPosition);
        cinematicCamera.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        cinematicCamera.rotation = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 2);

        fs::path filePath = "";
        u32 fileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        if (cascLoader->InCascAndListFile(fileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(fileID);
            filePath = fs::path("model") / fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
        }
        cinematicCamera.model = cinematicCameraStorage.AddString(filePath.generic_string());

        cinematicCameraStorage.Replace(recordID, cinematicCamera);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::CinematicCameraRecord>(layout, cinematicCameraStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = cinematicCameraStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!cinematicCameraStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    cinematicSequenceStorage.Initialize<MetaGen::Shared::ClientDB::CinematicSequenceRecord>();
    cinematicSequenceStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::CinematicSequenceRecord cinematicSequence;
        const u16* cameraIDs = db2Parser.GetFieldPtr<u16>(layout, sectionID, recordID, recordData, 1);
        cinematicSequence.cameraID = cameraIDs[0];

        cinematicSequenceStorage.Replace(recordID, cinematicSequence);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::CinematicSequenceRecord>(layout, cinematicSequenceStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = cinematicSequenceStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!cinematicSequenceStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    animationDataStorage.Initialize<MetaGen::Shared::ClientDB::AnimationDataRecord>();
    animationDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::AnimationDataRecord animationData;
        animationData.fallback = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 0);
        animationData.behaviorTier = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);
        animationData.behaviorID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);

        const u32* flags = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 3);
        animationData.flags = flags[0] | static_cast<u64>(flags[1]) << 32;

        animationDataStorage.Replace(recordID, animationData);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::AnimationDataRecord>(layout, animationDataStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = animationDataStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!animationDataStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    creatureModelDataStorage.Initialize<MetaGen::Shared::ClientDB::CreatureModelDataRecord>();
    creatureModelDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::CreatureModelDataRecord creatureModelData;
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
            filePath = fs::path("model") / fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
        }

        creatureModelData.model = creatureModelDataStorage.AddString(filePath.generic_string());

        creatureModelDataStorage.Replace(recordID, creatureModelData);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::CreatureModelDataRecord>(layout, creatureModelDataStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = creatureModelDataStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!creatureModelDataStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    creatureDisplayInfoStorage.Initialize<MetaGen::Shared::ClientDB::CreatureDisplayInfoRecord>();
    creatureDisplayInfoStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::CreatureDisplayInfoRecord creatureDisplayInfo;
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
                filePath = fs::path("texture") / fs::path(fileStr).replace_extension("dds");
                creatureDisplayInfo.textureVariations[textureVariantIndex] = creatureDisplayInfoStorage.AddString(filePath.generic_string());
            }

            creatureDisplayInfo.textureVariations[textureVariantIndex] = creatureDisplayInfoStorage.AddString(filePath.generic_string());
        }

        creatureDisplayInfoStorage.Replace(recordID, creatureDisplayInfo);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::CreatureDisplayInfoRecord>(layout, creatureDisplayInfoStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = creatureDisplayInfoStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!creatureDisplayInfoStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    creatureDisplayInfoExtraStorage.Initialize<MetaGen::Shared::ClientDB::CreatureDisplayInfoExtraRecord>();
    creatureDisplayInfoExtraStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::CreatureDisplayInfoExtraRecord creatureDisplayInfoExtra;
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

            auto& textureFileData = textureFileDataStorage.Get<MetaGen::Shared::ClientDB::TextureFileDataRecord>(textureFileDataID);
            filePath = textureFileDataStorage.GetString(textureFileData.texture);
        }
        creatureDisplayInfoExtra.bakedTexture = creatureDisplayInfoExtraStorage.AddString(filePath.generic_string());

        bool didOverride = false;
        creatureDisplayInfoExtraStorage.Replace(recordID, creatureDisplayInfoExtra, didOverride);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::CreatureDisplayInfoExtraRecord>(layout, creatureDisplayInfoExtraStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = creatureDisplayInfoExtraStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!creatureDisplayInfoExtraStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    itemDisplayMaterialResourcesStorage.Initialize<MetaGen::Shared::ClientDB::ItemDisplayInfoMaterialResourceRecord>();
    itemDisplayMaterialResourcesStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::ItemDisplayInfoMaterialResourceRecord itemDisplayMaterialResource;
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
        auto& itemDisplayMaterialResource = itemDisplayMaterialResourcesStorage.Get<MetaGen::Shared::ClientDB::ItemDisplayInfoMaterialResourceRecord>(rowID);
        itemDisplayMaterialResource.displayInfoID = relationshipEntry->foreignID;
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::ItemDisplayInfoMaterialResourceRecord>(layout, itemDisplayMaterialResourcesStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = itemDisplayMaterialResourcesStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!itemDisplayMaterialResourcesStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    itemDisplayModelMaterialResourcesStorage.Initialize<MetaGen::Shared::ClientDB::ItemDisplayInfoModelMaterialResourceRecord>();
    itemDisplayModelMaterialResourcesStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::ItemDisplayInfoModelMaterialResourceRecord itemDisplayModelMaterialResource;
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
        auto& itemDisplayModelMaterialResource = itemDisplayModelMaterialResourcesStorage.Get<MetaGen::Shared::ClientDB::ItemDisplayInfoModelMaterialResourceRecord>(rowID);
        itemDisplayModelMaterialResource.displayInfoID = relationshipEntry->foreignID;
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::ItemDisplayInfoModelMaterialResourceRecord>(layout, itemDisplayModelMaterialResourcesStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = itemDisplayModelMaterialResourcesStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!itemDisplayModelMaterialResourcesStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    itemDisplayInfoStorage.Initialize<MetaGen::Shared::ClientDB::ItemDisplayInfoRecord>();
    itemDisplayInfoStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::ItemDisplayInfoRecord itemDisplayInfo;

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
        for (u32 geosetGroupIndex = 0; geosetGroupIndex < 4; geosetGroupIndex++)
        {
            itemDisplayInfo.modelGeosetGroups[geosetGroupIndex] = static_cast<u8>(goesetGroups[geosetGroupIndex]);
        }

        // geosetHelmetVis
        const u32* geosetHelmetVis = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 15);
        itemDisplayInfo.modelGeosetVisIDs[0] = static_cast<u16>(geosetHelmetVis[0]);
        itemDisplayInfo.modelGeosetVisIDs[1] = static_cast<u16>(geosetHelmetVis[1]);

        itemDisplayInfoStorage.Replace(recordID, itemDisplayInfo);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::ItemDisplayInfoRecord>(layout, itemDisplayInfoStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = itemDisplayInfoStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!itemDisplayInfoStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    lightStorage.Initialize<MetaGen::Shared::ClientDB::LightRecord>();
    lightStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::LightRecord light;
        light.mapID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 3);

        vec3 position = *db2Parser.GetFieldPtr<vec3>(layout, sectionID, recordID, recordData, 0);
        light.position = CoordinateSpaces::TerrainPosToNovus(position);
        light.fallOff.x = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 1);
        light.fallOff.y = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 2);

        const u16* lightParamIDs = db2Parser.GetFieldPtr<u16>(layout, sectionID, recordID, recordData, 4);
        memcpy(&light.paramIDs[0], lightParamIDs, 8 * sizeof(u16));

        lightStorage.Replace(recordID, light);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::LightRecord>(layout, lightStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = lightStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!lightStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    lightParamsStorage.Initialize<MetaGen::Shared::ClientDB::LightParamRecord>();
    lightParamsStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::LightParamRecord lightParam;
        recordID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        bool highlightSky = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 2);
        lightParam.flags = 1 << 0 * highlightSky;
        lightParam.lightSkyboxID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 3);
        lightParam.glow = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 5);
        lightParam.riverAlphas[0] = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 6);
        lightParam.riverAlphas[1] = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 7);
        lightParam.oceanAlphas[0] = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 8);
        lightParam.oceanAlphas[1] = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 9);

        lightParamsStorage.Replace(db2RecordIndex, lightParam);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::LightParamRecord>(layout, lightParamsStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = lightParamsStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!lightParamsStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    lightDataStorage.Initialize<MetaGen::Shared::ClientDB::LightDataRecord>();
    lightDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::LightDataRecord lightData;
        lightData.lightParamID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 0);
        u16 timestamp = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 1);
        lightData.timestamp = static_cast<u32>(timestamp) * 30;
        lightData.diffuseColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);
        lightData.ambientColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        lightData.skyColors[0] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
        lightData.skyColors[1] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 5);
        lightData.skyColors[2] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 6);
        lightData.skyColors[3] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 7);
        lightData.skyColors[4] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 8);
        lightData.skyColors[5] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 9);
        lightData.sunColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 10);
        lightData.sunFogColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 38);
        lightData.sunFogStrength = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 39);
        lightData.sunFogAngle = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 29);
        lightData.cloudColors[0] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 11);
        lightData.cloudColors[1] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 12);
        lightData.cloudColors[2] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 13);
        lightData.cloudColors[3] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 14);
        lightData.oceanColors[0] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 15);
        lightData.oceanColors[1] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 16);
        lightData.riverColors[0] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 17);
        lightData.riverColors[1] = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 18);
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

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::LightDataRecord>(layout, lightDataStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = lightDataStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!lightDataStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
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

    lightSkyboxStorage.Initialize<MetaGen::Shared::ClientDB::LightSkyboxRecord>();
    lightSkyboxStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        MetaGen::Shared::ClientDB::LightSkyboxRecord lightSkybox;

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
                    filePath = fs::path("model") / fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
                }
            }

        }

        lightSkybox.model = lightSkyboxStorage.AddString(filePath.generic_string());
        lightSkybox.name = lightSkyboxStorage.AddString(fs::path(skyboxName).filename().replace_extension("").string());
        lightSkyboxStorage.Replace(recordID, lightSkybox);
    }

    RepopulateFromCopyTable<MetaGen::Shared::ClientDB::LightSkyboxRecord>(layout, lightSkyboxStorage);

    Runtime* runtime = ServiceLocator::GetRuntime();

    size_t size = lightSkyboxStorage.GetSerializedSize();
    std::shared_ptr<Bytebuffer> storageBuffer = Bytebuffer::BorrowRuntime(size);
    if (!lightSkyboxStorage.Save(storageBuffer))
        return false;

    fs::path path = fs::path("clientdb") / name;
    path.replace_extension(ClientDB::FILE_EXTENSION);

    std::string pactPath = path.generic_string();
    StringUtils::ToLower(pactPath);
    auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, storageBuffer->writtenData);
    return manifest.AddFile(runtime, pactPath, storageBuffer);
}
