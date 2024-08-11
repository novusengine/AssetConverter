#include "Runtime.h"
#include "Blp/BlpConvert.h"
#include "Casc/CascLoader.h"
#include "Extractors/ClientDBExtractor.h"
#include "Extractors/MapExtractor.h"
#include "Extractors/MapObjectExtractor.h"
#include "Extractors/ComplexModelExtractor.h"
#include "Extractors/TextureExtractor.h"
#include "Util/ServiceLocator.h"

#include <Base/Types.h>
#include <Base/Util/JsonUtils.h>
#include <Base/Util/DebugHandler.h>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <quill/Backend.h>

#include <filesystem>
namespace fs = std::filesystem;

i32 main()
{
    quill::Backend::start();

    auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console_sink_1");
    quill::Logger* logger = quill::Frontend::create_or_get_logger("root", std::move(console_sink), "%(time:<16) LOG_%(log_level:<11) %(message)", "%H:%M:%S.%Qms", quill::Timezone::LocalTime, quill::ClockSourceType::System);

    Runtime* runtime = ServiceLocator::SetRuntime(new Runtime());

    // Setup Runtime
    {
        // Setup Paths
        {
            Runtime::Paths& paths = runtime->paths;

            paths.executable = fs::current_path();
            paths.data = paths.executable / "Data";
            paths.clientDB = paths.data / "ClientDB";
            paths.texture = paths.data / "Texture";
            paths.textureBlendMap = paths.texture / "blendmaps";
            paths.map = paths.data / "Map";
            paths.mapObject = paths.data / "MapObject";
            paths.complexModel = paths.data / "ComplexModel";

            fs::create_directories(paths.data);
            fs::create_directories(paths.clientDB);
            fs::create_directories(paths.texture);
            fs::create_directories(paths.textureBlendMap);
            fs::create_directories(paths.map);
            fs::create_directories(paths.mapObject);
            fs::create_directories(paths.complexModel);
        }

        // Setup Json
        {
            static const std::string CONFIG_VERSION = "0.4";
            static const std::string CONFIG_NAME = "AssetConverterConfig.json";

            fs::path configPath = runtime->paths.executable / CONFIG_NAME;
            std::string absolutePath = fs::absolute(configPath).string();

            nlohmann::ordered_json fallbackJson;

			bool configExists = fs::exists(configPath);
			if (!configExists)
			{
				NC_LOG_CRITICAL("[AssetConverter] Please copy the {0} to this folder.\n\nPress 'Enter' to exit.", CONFIG_NAME);
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
                NC_LOG_INFO("");

                bool isExtractingEnabled = runtime->json["Extraction"]["Enabled"];
                if (isExtractingEnabled)
                {
                    NC_LOG_INFO("[AssetConverter] Processing Extractors...");

                    // DB2
                    bool isDB2Enabled = runtime->json["Extraction"]["ClientDB"]["Enabled"];
                    if (isDB2Enabled)
                    {
                        NC_LOG_INFO("[AssetConverter] Processing ClientDB Extractor...");
                        ClientDBExtractor::Process();
                        NC_LOG_INFO("[AssetConverter] ClientDB Extractor Finished\n");
                    }

                    // Map
                    bool isMapEnabled = runtime->json["Extraction"]["Map"]["Enabled"];
                    if (isMapEnabled)
                    {
                        NC_LOG_INFO("[AssetConverter] Processing Map Extractor...");
                        MapExtractor::Process();
                        NC_LOG_INFO("[AssetConverter] Map Extractor Finished\n");
                    }

                    // Map Object
                    bool isMapObjectEnabled = runtime->json["Extraction"]["MapObject"]["Enabled"];
                    if (isMapObjectEnabled)
                    {
                        NC_LOG_INFO("[AssetConverter] Processing MapObject Extractor...");
                        MapObjectExtractor::Process();
                        NC_LOG_INFO("[AssetConverter] MapObject Extractor Finished\n");
                    }

                    // Complex Model
                    bool isComplexModelEnabled = runtime->json["Extraction"]["ComplexModel"]["Enabled"];
                    if (isComplexModelEnabled)
                    {
                        NC_LOG_INFO("[AssetConverter] Processing ComplexModel Extractor...");
                        ComplexModelExtractor::Process();
                        NC_LOG_INFO("[AssetConverter] ComplexModel Extractor Finished\n");
                    }

                    // Texture
                    bool isTextureEnabled = runtime->json["Extraction"]["Texture"]["Enabled"];
                    if (isTextureEnabled)
                    {
                        NC_LOG_INFO("[AssetConverter] Processing Texture Extractor...");
                        TextureExtractor::Process();
                        NC_LOG_INFO("[AssetConverter] Texture Extractor Finished\n");
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