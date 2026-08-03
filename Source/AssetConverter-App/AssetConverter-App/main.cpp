#include "Runtime.h"
#include "Casc/CascLoader.h"
#include "Extractors/ClientDBExtractor.h"
#include "Extractors/MapExtractor.h"
#include "Extractors/MapObjectExtractor.h"
#include "Extractors/ComplexModelExtractor.h"
#include "Extractors/TextureExtractor.h"
#include "Model/ModelV2Allocation.h"
#include "Util/ServiceLocator.h"

#include <Base/Types.h>
#include <Base/Util/JsonUtils.h>
#include <Base/Util/DebugHandler.h>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <quill/Backend.h>

#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;

namespace
{
    bool RemoveGeneratedPactFiles(const fs::path& directory, const std::string& extension)
    {
        std::error_code error;
        if (!fs::exists(directory, error))
        {
            if (error)
            {
                NC_LOG_ERROR("[AssetConverter] Failed to inspect PACT directory {0}: {1}", directory.string(), error.message());
                return false;
            }

            return true;
        }

        for (fs::directory_iterator itr(directory, error); !error && itr != fs::directory_iterator(); itr.increment(error))
        {
            const fs::directory_entry& entry = *itr;
            if (entry.is_regular_file(error) && entry.path().extension() == extension)
                fs::remove(entry.path(), error);

            if (error)
                break;
        }

        if (error)
        {
            NC_LOG_ERROR("[AssetConverter] Failed to clean generated PACT files in {0}: {1}", directory.string(), error.message());
            return false;
        }

        return true;
    }

    bool InitializePact(Runtime* runtime)
    {
        if (!RemoveGeneratedPactFiles(runtime->paths.pactManifest, PACT::Config::MANIFEST_EXT) ||
            !RemoveGeneratedPactFiles(runtime->paths.pactData, PACT::Config::DATA_EXT))
        {
            NC_LOG_CRITICAL("[AssetConverter] Failed to clean generated PACT files; aborting extraction");
            return false;
        }

        std::error_code error;
        fs::create_directories(runtime->paths.pactRoot, error);
        if (!error)
            fs::create_directories(runtime->paths.pactManifest, error);
        if (!error)
            fs::create_directories(runtime->paths.pactData, error);

        if (error)
        {
            NC_LOG_CRITICAL("[AssetConverter] Failed to create PACT directories: {0}", error.message());
            return false;
        }

        PACT::PactRoot& pactRoot = runtime->pactInfo.GetRoot();
        pactRoot.version = PACT::Config::ROOT_VERSION;
        pactRoot.featureSet = {};

        std::shared_ptr<Bytebuffer> buffer = Bytebuffer::Borrow<sizeof(PACT::PactRoot)>();
        if (!buffer->Serialize(pactRoot))
        {
            NC_LOG_CRITICAL("[AssetConverter] Failed to serialize the PACT root");
            return false;
        }

        const fs::path pactRootPath = fs::absolute(runtime->paths.pactRoot / PACT::Config::ROOT_FILE);
        std::ofstream rootWriter(pactRootPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!rootWriter.is_open())
        {
            NC_LOG_CRITICAL("[AssetConverter] Failed to open the PACT root file {0}", pactRootPath.string());
            return false;
        }

        rootWriter.write(reinterpret_cast<const char*>(buffer->GetDataPointer()), static_cast<std::streamsize>(buffer->writtenData));
        rootWriter.flush();
        rootWriter.close();
        if (rootWriter.fail())
        {
            NC_LOG_CRITICAL("[AssetConverter] Failed to write the PACT root file {0}", pactRootPath.string());
            return false;
        }

        runtime->pactInfo.Initialize();
        return true;
    }
}

i32 main()
{
    quill::Backend::start();

    auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console_sink_1");
    quill::Logger* logger = quill::Frontend::create_or_get_logger("root", std::move(console_sink), "%(time:<16) LOG_%(log_level:<11) %(message)", "%H:%M:%S.%Qms", quill::Timezone::LocalTime, quill::ClockSourceType::System);

    Runtime* runtime = ServiceLocator::SetRuntime(new Runtime());
    bool isExtractingEnabled = false;
    bool isDB2Enabled = false;
    bool isMapEnabled = false;
    bool isNavMeshEnabled = false;
    bool isMapObjectEnabled = false;
    bool isComplexModelEnabled = false;
    bool isTextureEnabled = false;
    bool rebuildPact = false;

    // Setup Runtime
    {
        // Setup Paths
        {
            Runtime::Paths& paths = runtime->paths;

            paths.executable = fs::current_path();
            paths.data = paths.executable / "Data";
            paths.pactRoot = paths.data / "Pact";
            paths.pactManifest = paths.pactRoot / PACT::Config::MANIFEST_DIR;
            paths.pactData = paths.pactRoot / PACT::Config::DATA_DIR;
            paths.navMesh = paths.data / "NavMesh";

            fs::create_directories(paths.data);
            fs::create_directories(paths.navMesh);
        }

        // Setup Json
        {
            static const std::string CONFIG_VERSION = "0.6";
            static const std::string CONFIG_NAME = "AssetConverterConfig.json";

            fs::path configPath = runtime->paths.executable / CONFIG_NAME;
            std::string absolutePath = fs::absolute(configPath).string();

            nlohmann::ordered_json fallbackJson;

            bool configExists = fs::exists(configPath);
            if (!configExists)
            {
                NC_LOG_CRITICAL("[AssetConverter] Please copy {0} to this folder.\n\nPress 'Enter' to exit.", CONFIG_NAME);
            }

            if (!JsonUtils::LoadFromPathOrCreate(runtime->json, fallbackJson, configPath))
            {
                NC_LOG_CRITICAL("[AssetConverter] Failed to Load {0} from {1}", CONFIG_NAME, absolutePath.c_str());
            }

            std::string currentVersion = runtime->json["General"]["Version"];
            if (currentVersion != CONFIG_VERSION)
            {
                NC_LOG_CRITICAL("[AssetConverter] Attempted to load outdated {0}. (Config Version : {1}, Expected Version : {2})", CONFIG_NAME.c_str(), currentVersion.c_str(), CONFIG_VERSION.c_str());
            }

            runtime->isInDebugMode = runtime->json["General"]["DebugMode"];

            isExtractingEnabled = runtime->json["Extraction"]["Enabled"];
            isDB2Enabled = runtime->json["Extraction"]["ClientDB"]["Enabled"];
            isMapEnabled = runtime->json["Extraction"]["Map"]["Enabled"];
            isNavMeshEnabled = runtime->json["Extraction"]["NavMesh"]["Enabled"];
            isMapObjectEnabled = runtime->json["Extraction"]["MapObject"]["Enabled"];
            isComplexModelEnabled = runtime->json["Extraction"]["ComplexModel"]["Enabled"];
            isTextureEnabled = runtime->json["Extraction"]["Texture"]["Enabled"];
            rebuildPact = isExtractingEnabled &&
                (isDB2Enabled || isMapEnabled || isMapObjectEnabled || isComplexModelEnabled || isTextureEnabled);
        }

        if (!rebuildPact && isExtractingEnabled && isNavMeshEnabled)
        {
            NC_LOG_INFO("[AssetConverter] NavMesh-only extraction selected; preserving existing PACT storage");
        }

        // Setup Scheduler
        {
            u32 threadCount = runtime->json["General"]["ThreadCount"];
            if (threadCount == 0 || threadCount == std::numeric_limits<u32>().max())
            {
                threadCount = std::thread::hardware_concurrency() - 1;
            }

            runtime->scheduler.Initialize(threadCount);
        }
    }

    // Setup CascLoader
    {
        const std::string& listFile = runtime->json["Casc"]["ListFile"];
        const std::string& locale = runtime->json["Casc"]["Locale"];

        ServiceLocator::SetCascLoader(new CascLoader(listFile, locale));
    }

    // Setup Jolt
    {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    // Run Extractors
    {
        CascLoader* cascLoader = ServiceLocator::GetCascLoader();

        CascLoader::Result result = cascLoader->Load();
        switch (result)
        {
            case CascLoader::Result::Success:
            {
                if (rebuildPact && !InitializePact(runtime))
                {
                    cascLoader->Close();
                    return 1;
                }

                NC_LOG_INFO("");

                if (isExtractingEnabled)
                {
                    NC_LOG_INFO("[AssetConverter] Processing Extractors...");
                    bool mapDataAvailable = isDB2Enabled;

                    // DB2
                    if (isDB2Enabled)
                    {
                        NC_LOG_INFO("[AssetConverter] Processing ClientDB Extractor...");
                        ClientDBExtractor::Process();
                        NC_LOG_INFO("[AssetConverter] ClientDB Extractor Finished\n");
                    }
                    else if (isMapEnabled || isNavMeshEnabled)
                    {
                        NC_LOG_INFO("[AssetConverter] Loading Map data required by the Map/NavMesh extractor...");
                        mapDataAvailable = ClientDBExtractor::LoadMapStorage(isMapEnabled);
                        if (!mapDataAvailable)
                        {
                            NC_LOG_ERROR("[AssetConverter] Failed to load Map data required for map extraction");
                        }
                    }

                    // Map Object / Complex Model
                    {
                        ModelV2AllocationRegistry::Reset();

                        if (isMapObjectEnabled)
                        {
                            NC_LOG_INFO("[AssetConverter] Processing MapObject Extractor...");
                            MapObjectExtractor::Process();
                            NC_LOG_INFO("[AssetConverter] MapObject Extractor Finished\n");
                        }

                        if (isComplexModelEnabled)
                        {
                            NC_LOG_INFO("[AssetConverter] Processing ComplexModel Extractor...");
                            ComplexModelExtractor::Process();
                            NC_LOG_INFO("[AssetConverter] ComplexModel Extractor Finished\n");
                        }
                    }

                    // Map metadata consumes the Model V2 allocation registry,
                    // so maps follow model conversion and never scan payloads.
                    if ((isMapEnabled || isNavMeshEnabled) && mapDataAvailable)
                    {
                        NC_LOG_INFO("[AssetConverter] Processing Map/NavMesh Extractor...");
                        MapExtractor::Process(isMapEnabled, isNavMeshEnabled);
                        NC_LOG_INFO("[AssetConverter] Map/NavMesh Extractor Finished\n");
                    }

                    // Texture
                    if (isTextureEnabled)
                    {
                        NC_LOG_INFO("[AssetConverter] Processing Texture Extractor...");
                        TextureExtractor::Process();
                        NC_LOG_INFO("[AssetConverter] Texture Extractor Finished\n");
                    }

                    if (rebuildPact && !runtime->pactInfo.Finalize())
                    {
                        NC_LOG_CRITICAL("[AssetConverter] Failed to finalize PACT storage");
                        cascLoader->Close();
                        return 1;
                    }
                }

                cascLoader->Close();
                break;
            }

            case CascLoader::Result::MissingCasc:
            {
                NC_LOG_ERROR("[CascLoader] Could not load Casc. Failed to find Installation");
                break;
            }

            case CascLoader::Result::MissingListFile:
            {
                NC_LOG_ERROR("[CascLoader] Could not load Casc. Failed to find Listfile");
                break;
            }

            case CascLoader::Result::MissingLocale:
            {
                NC_LOG_ERROR("[CascLoader] Could not load Casc. Invalid Locale");
                break;
            }

            case CascLoader::Result::AlreadyInitialized:
            {
                NC_LOG_ERROR("[CascLoader] Could not load Casc. Already Initialized.");
                break;
            }

            default:
            {
                NC_LOG_ERROR("[CascLoader] Could not load Casc. Unknown Result.");
                break;
            }
        }
    }

    NC_LOG_INFO("");
    NC_LOG_INFO("Finished... Press 'Enter' to exit");
    std::cin.get();

    return 0;
}
