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
    { "TextureFileData.db2",            "A collection of Texture File Data",                ClientDBExtractor::ExtractTextureFileData },
    { "Map.db2",				        "A collection of all maps",				            ClientDBExtractor::ExtractMap },
    { "LiquidObject.db2",		        "A collection of liquid objects",		            ClientDBExtractor::ExtractLiquidObject },
    { "LiquidType.db2",			        "A collection of liquid types",			            ClientDBExtractor::ExtractLiquidType },
    { "LiquidMaterial.db2",		        "A collection of liquid materials",		            ClientDBExtractor::ExtractLiquidMaterial },
    { "CinematicCamera.db2",	        "A collection of cinematic cameras",	            ClientDBExtractor::ExtractCinematicCamera },
    { "CinematicSequences.db2",	        "A collection of cinematic sequences",	            ClientDBExtractor::ExtractCinematicSequence },
    { "AnimationData.db2",	            "A collection of Animation Data",	                ClientDBExtractor::ExtractAnimationData },
    { "CreatureDisplayInfo.db2",        "A collection of Creature Display Info Data",	    ClientDBExtractor::ExtractCreatureDisplayInfo },
    { "CreatureDisplayInfoExtra.db2",   "A collection of Creature Display Info Extra Data", ClientDBExtractor::ExtractCreatureDisplayInfoExtra },
    { "CreatureModelData.db2",          "A collection of Creature Model Data",              ClientDBExtractor::ExtractCreatureModelData },
    { "CharSection.db2",                "A collection of Char Section Data",                ClientDBExtractor::ExtractCharSection },
    { "Light.db2",                      "A collection of Light Data",                       ClientDBExtractor::ExtractLight },
    { "LightParams.db2",                "A collection of Light Parameter Data",             ClientDBExtractor::ExtractLightParams },
    { "LightData.db2",                  "A collection of Light Data Data",                  ClientDBExtractor::ExtractLightData },
    { "LightSkybox.db2",                "A collection of Light Skybox Data",                ClientDBExtractor::ExtractLightSkybox }
};

ClientDB::Storage<ClientDB::Definitions::Map> ClientDBExtractor::mapStorage("Map");
ClientDB::Storage<ClientDB::Definitions::LiquidObject> ClientDBExtractor::liquidObjectStorage("LiquidObject");
ClientDB::Storage<ClientDB::Definitions::LiquidType> ClientDBExtractor::liquidTypeStorage("LiquidType");
ClientDB::Storage<ClientDB::Definitions::LiquidMaterial> ClientDBExtractor::liquidMaterialStorage("LiquidMaterial");
ClientDB::Storage<ClientDB::Definitions::CinematicCamera> ClientDBExtractor::cinematicCameraStorage("CinematicCamera");
ClientDB::Storage<ClientDB::Definitions::CinematicSequence> ClientDBExtractor::cinematicSequenceStorage("CinematicSequences");
ClientDB::Storage<ClientDB::Definitions::AnimationData> ClientDBExtractor::animationDataStorage("AnimationData");
ClientDB::Storage<ClientDB::Definitions::CreatureDisplayInfo> ClientDBExtractor::creatureDisplayInfoStorage("CreatureDisplayInfo");
ClientDB::Storage<ClientDB::Definitions::CreatureDisplayInfoExtra> ClientDBExtractor::creatureDisplayInfoExtraStorage("CreatureDisplayInfoExtra");
ClientDB::Storage<ClientDB::Definitions::CreatureModelData> ClientDBExtractor::creatureModelDataStorage("CreatureModelData");
ClientDB::Storage<ClientDB::Definitions::TextureFileData> ClientDBExtractor::textureFileDataStorage("TextureFileData");
ClientDB::Storage<ClientDB::Definitions::CharSection> ClientDBExtractor::charSectionStorage("CharSection");
ClientDB::Storage<ClientDB::Definitions::Light> ClientDBExtractor::lightStorage("Light");
ClientDB::Storage<ClientDB::Definitions::LightParam> ClientDBExtractor::lightParamsStorage("LightParams");
ClientDB::Storage<ClientDB::Definitions::LightData> ClientDBExtractor::lightDataStorage("LightData");
ClientDB::Storage<ClientDB::Definitions::LightSkybox> ClientDBExtractor::lightSkyboxStorage("LightSkybox");

robin_hood::unordered_map<u32, u32> ClientDBExtractor::materialResourcesIDToTextureFileDataEntry;

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

bool ClientDBExtractor::ExtractAnimationData()
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/animationdata.db2");
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    animationDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::AnimationData animationData;
        animationData.id = recordID;
        animationData.fallback = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 0);
        animationData.behaviorTier = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);
        animationData.behaviorID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);

        const u32* flags = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 3);
        memcpy(&animationData.flags[0], flags, 2 * sizeof(u32));

        animationDataStorage.AddRow(animationData);
    }

    RepopulateFromCopyTable(layout, animationDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / animationDataStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!animationDataStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractCreatureDisplayInfo()
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/creaturedisplayinfo.db2");
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    creatureDisplayInfoStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CreatureDisplayInfo creatureDisplayInfo;
        creatureDisplayInfo.id = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        creatureDisplayInfo.modelID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 1);
        creatureDisplayInfo.soundID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
        creatureDisplayInfo.sizeClass = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 3);
        creatureDisplayInfo.creatureModelScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 4);
        creatureDisplayInfo.creatureModelAlpha = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 5);
        creatureDisplayInfo.bloodID = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 6);
        creatureDisplayInfo.extendedDisplayInfoID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 7);
        creatureDisplayInfo.npcSoundID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 8);
        creatureDisplayInfo.particleColorID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 9);
        creatureDisplayInfo.portraitCreatureDisplayInfoID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 10);
        creatureDisplayInfo.portraitTextureFileDataID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 11);
        creatureDisplayInfo.objectEffectPackageID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 12);
        creatureDisplayInfo.animReplacementSetID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 13);
        creatureDisplayInfo.flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 14);
        creatureDisplayInfo.stateSpellVisualKitID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 15);
        creatureDisplayInfo.playerOverrideScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 16);
        creatureDisplayInfo.petInstanceScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 17);
        creatureDisplayInfo.unarmedWeaponType = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 18);
        creatureDisplayInfo.mountPoofSpellVisualKitID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 19);
        creatureDisplayInfo.dissolveEffectID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 20);
        creatureDisplayInfo.gender = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 21);
        creatureDisplayInfo.dissolveOutEffectID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 22);
        creatureDisplayInfo.creatureModelMinLod = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 23);

        const u32* textureVariations = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 24);
        memcpy(&creatureDisplayInfo.textureVariations[0], textureVariations, 4 * sizeof(u32));

        for (i32 i = 0; i < 4; i++)
        {
            i32 textureVariationFileID = creatureDisplayInfo.textureVariations[i];

            if (textureVariationFileID == 0)
                continue;

            if (!cascLoader->InCascAndListFile(textureVariationFileID))
                continue;

            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(textureVariationFileID);

            fs::path filePath = fs::path(fileStr).replace_extension("dds");
            u32 nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());

            creatureDisplayInfo.textureVariations[i] = nameHash;
        }

        creatureDisplayInfoStorage.AddRow(creatureDisplayInfo);
    }

    RepopulateFromCopyTable(layout, creatureDisplayInfoStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / creatureDisplayInfoStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!creatureDisplayInfoStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractCreatureDisplayInfoExtra()
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/creaturedisplayinfoextra.db2");
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    creatureDisplayInfoExtraStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CreatureDisplayInfoExtra creatureDisplayInfoExtra;
        creatureDisplayInfoExtra.id = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        creatureDisplayInfoExtra.displayRaceID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 1);
        creatureDisplayInfoExtra.displaySexID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 2);
        creatureDisplayInfoExtra.displayClassID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 3);
        creatureDisplayInfoExtra.skinID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 4);
        creatureDisplayInfoExtra.faceID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 5);
        creatureDisplayInfoExtra.hairStyleID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 6);
        creatureDisplayInfoExtra.hairColorID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 7);
        creatureDisplayInfoExtra.facialHairID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 8);
        creatureDisplayInfoExtra.flags = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 9);
        creatureDisplayInfoExtra.bakedTextureHash = std::numeric_limits<u32>::max();
        u32 bakedMaterialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 10);

        const u8* customDisplayOptions = db2Parser.GetFieldPtr<u8>(layout, sectionID, recordID, recordData, 12);
        memcpy(&creatureDisplayInfoExtra.customDisplayOptions[0], customDisplayOptions, 3 * sizeof(u8));

        if (materialResourcesIDToTextureFileDataEntry.contains(bakedMaterialResourcesID))
        {
            u32 textureFileDataID = materialResourcesIDToTextureFileDataEntry[bakedMaterialResourcesID];
            
            if (ClientDB::Definitions::TextureFileData* textureFileData = textureFileDataStorage.GetRow(textureFileDataID))
            {
                creatureDisplayInfoExtra.bakedTextureHash = textureFileData->textureHash;
            }
        }

        creatureDisplayInfoExtraStorage.AddRow(creatureDisplayInfoExtra);
    }

    RepopulateFromCopyTable(layout, creatureDisplayInfoExtraStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / creatureDisplayInfoExtraStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!creatureDisplayInfoExtraStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractCreatureModelData()
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/creaturemodeldata.db2");
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    creatureModelDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CreatureModelData creatureModelData;
        creatureModelData.id = recordID;
        creatureModelData.boundingBox = *db2Parser.GetFieldPtr<Geometry::AABoundingBox>(layout, sectionID, recordID, recordData, 0);
        creatureModelData.flags = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        creatureModelData.modelHash = std::numeric_limits<u32>::max();
        u32 fileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);
        creatureModelData.bloodID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        creatureModelData.footprintTextureID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
        creatureModelData.footprintTextureLength = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 5);
        creatureModelData.footprintTextureWidth = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 6);
        creatureModelData.footprintParticleScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 7);
        creatureModelData.foleyMaterialID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 8);
        creatureModelData.footstepCameraEffectID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 9);
        creatureModelData.deathThudCameraEffectID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 10);
        creatureModelData.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 11);
        creatureModelData.sizeClass = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 12);
        creatureModelData.collisionWidth = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 13);
        creatureModelData.collisionHeight = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 14);
        creatureModelData.worldEffectScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 15);
        creatureModelData.creatureGeosetDataID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 16);
        creatureModelData.hoverHeight = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 17);
        creatureModelData.attachedEffectScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 18);
        creatureModelData.modelScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 19);
        creatureModelData.missileCollisionRadius = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 20);
        creatureModelData.missileCollisionPush = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 21);
        creatureModelData.missileCollisionRaise = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 22);
        creatureModelData.mountHeight = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 23);
        creatureModelData.overrideLootEffectScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 24);
        creatureModelData.overrideNameScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 25);
        creatureModelData.overrideSelectionRadius = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 26);
        creatureModelData.tamedPetBaseScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 27);

        if (cascLoader->InCascAndListFile(fileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(fileID);

            fs::path filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
            u32 nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());

            creatureModelData.modelHash = nameHash;
        }

        creatureModelDataStorage.AddRow(creatureModelData);
    }

    RepopulateFromCopyTable(layout, creatureModelDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / creatureModelDataStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!creatureModelDataStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractTextureFileData()
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/texturefiledata.db2");
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    textureFileDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::TextureFileData textureFileData;
        textureFileData.id = db2RecordIndex;
        
        u32 textureFileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        textureFileData.usage = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);
        textureFileData.materialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);

        if (cascLoader->InCascAndListFile(textureFileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(textureFileID);

            fs::path filePath = fs::path(fileStr).replace_extension("dds");
            u32 nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());

            textureFileData.textureHash = nameHash;
        }
        else
        {
            textureFileData.textureHash = std::numeric_limits<u32>::max();
        }

        if (!materialResourcesIDToTextureFileDataEntry.contains(textureFileData.materialResourcesID))
        {
            materialResourcesIDToTextureFileDataEntry[textureFileData.materialResourcesID] = textureFileData.id;
        }

        textureFileDataStorage.AddRow(textureFileData);
    }

    RepopulateFromCopyTable(layout, textureFileDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / textureFileDataStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!textureFileDataStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractCharSection()
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/charsections.db2");
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    charSectionStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CharSection charSection;
        charSection.id = recordID;
        charSection.raceID = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 0);
        charSection.sexID = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);
        charSection.baseSection = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 2);
        charSection.varationIndex = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 3);
        charSection.colorIndex = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 4);
        charSection.flags = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 5);

        const u32* materialResourcesIDs = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 6);

        for (u32 i = 0; i < 3; i++)
        {
            charSection.textureHashes[i] = std::numeric_limits<u32>::max();

            u32 materialResourcesID = materialResourcesIDs[i];
            if (materialResourcesID == 0)
                continue;

            if (!materialResourcesIDToTextureFileDataEntry.contains(materialResourcesID))
                continue;

            u32 textureFileDataID = materialResourcesIDToTextureFileDataEntry[materialResourcesID];
            ClientDB::Definitions::TextureFileData* textureFileData = textureFileDataStorage.GetRow(textureFileDataID);
            if (!textureFileData)
                continue;
        
            charSection.textureHashes[i] = textureFileData->textureHash;
        }

        charSectionStorage.AddRow(charSection);
    }

    RepopulateFromCopyTable(layout, charSectionStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / charSectionStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!charSectionStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractLight()
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/light.db2");
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    lightStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::Light light;
        light.id = recordID;
        light.mapID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 3);

        vec3 position = *db2Parser.GetFieldPtr<vec3>(layout, sectionID, recordID, recordData, 0);
        light.position = CoordinateSpaces::TerrainPosToNovus(position);
        light.fallOff.x = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 1);
        light.fallOff.y = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 2);

        const u16* lightParamIDs = db2Parser.GetFieldPtr<u16>(layout, sectionID, recordID, recordData, 4);
        memcpy(&light.lightParamsID[0], lightParamIDs, 8 * sizeof(u16));
        lightStorage.AddRow(light);
    }

    RepopulateFromCopyTable(layout, lightStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / lightStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractLightParams()
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/lightparams.db2");
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    lightParamsStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LightParam lightParam;
        lightParam.id = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        lightParam.flags.highlightSky = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 2);
        lightParam.lightSkyboxID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 3);
        lightParam.glow = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 5);
        lightParam.waterShallowAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 6);
        lightParam.waterDeepAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 7);
        lightParam.oceanShallowAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 8);
        lightParam.oceanDeepAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 9);
        lightParamsStorage.AddRow(lightParam);
    }

    RepopulateFromCopyTable(layout, lightParamsStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / lightParamsStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightParamsStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractLightData()
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/lightdata.db2");
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    lightDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LightData lightData;
        lightData.id = recordID;
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
        lightData.fogDensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 22);
        lightData.cloudDensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 30);
        lightData.fogHeightColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        lightData.endFogColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 35);
        lightData.endFogHeightColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 41);

        lightDataStorage.AddRow(lightData);
    }

    RepopulateFromCopyTable(layout, lightDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / lightDataStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightDataStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractLightSkybox()
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath("dbfilesclient/lightskybox.db2");
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    lightSkyboxStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LightSkybox lightSkybox;
        lightSkybox.id = recordID;

        u32 nameHash = std::numeric_limits<u32>::max();
        
        u32 fileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);
        if (fileID == 0)
        {
            std::string modelPath = GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 0);

            if (cascLoader->ListFileContainsPath(modelPath))
            {
                if (!modelPath.empty())
                {
                    nameHash = StringUtils::fnv1a_32(modelPath.c_str(), modelPath.size());
                }
            }
        }
        else
        {
            if (cascLoader->InCascAndListFile(fileID))
            {
                const std::string& fileStr = cascLoader->GetFilePathFromListFileID(fileID);

                fs::path filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
                nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());
            }
        }

        lightSkybox.modelHash = nameHash;
        lightSkyboxStorage.AddRow(lightSkybox);
    }

    RepopulateFromCopyTable(layout, lightSkyboxStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / lightSkyboxStorage.GetName()).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightSkyboxStorage.Save(path))
        return false;

    return true;
}
