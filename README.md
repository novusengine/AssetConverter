## What is Novus
NovusCore is an MMO Engine.

Currently Novus is developing a game/server solution where our main focus is to be feature complete with the most popular MMOs out there.
Long term, we hope to support many more features.

## The Novus Promise
The project was made with the promise that we would always focus on reliability, redundancy, and performance over convenience. We achieve this through making use of experience, but also applying modern techniques and design patterns.

The end-goal is to provide a game/server setup, capable of tackling all of the limitations set by the current standard. Solving those issues are complicated, but we start by applying a proper foundation for our architecture to allow for better flow of information(data) and performance.

## Novus Discord
The project has an official [Discord](https://discord.gg/gz6FMZa).
You will find the developers to be active on the discord and always up for answering any questions you might have regarding the project. Despite Novus not currently being ready for production level use, we are always welcoming any users that want to try using it.

## Dependencies
* [OpenSSL 1.1.0](https://www.openssl.org/source/)
* [Premake5](https://premake.github.io/)
* [Vulkan 1.1 (or higher)](https://vulkan.lunarg.com/)

## Libraries
Here we include a honorable mention to all the libraries included directly into the source of Novus. You do not need to download these on your own, but we felt it was important to address these as without them, Novus would be a lot more time consuming to develop.
- [All Libraries from Engine](https://github.com/novusengine/Engine)
- [Jolt](https://github.com/jrouwe/JoltPhysics)
- [Casc](https://github.com/heksesang/CascLib)
- [Cuttlefish](https://github.com/akb825/Cuttlefish)

## How to build (Microsoft Visual 2022)
>[!TIP]
> It's recommended to fork, clone, and build [Engine](https://github.com/novusengine/Engine) first.
1. Download the dependencies.
2. Fork and clone the repositry
3. Open the project folder and open a terminal within the project's directory.
4. Use the command `premake5 vs2022`
> [!NOTE]
> You can change the files generated by using a different [action](https://premake.github.io/docs/using-premake) than `vs2022`.   
5. Open the new `Build` folder and double click the `AssetConverter.sln` file.
6. Set the build configuration to `Release`.
> [!WARNING]
> Make sure you set the build configuration to release. If you build this in debug, you exponentially increase asset extraction and conversion time.
7. Use `ctrl`+`shift`+`b` to build the solution or right click the solution and select `Build Solution`
8. The solution builds to `../Build/Bin/AssetConverter/[BuildConfig]`

## How to use
1. Download the `community-listfile.csv` from [here](https://github.com/wowdev/wow-listfile) and rename it to `listfile`.
2. Go to your `../World of Warcraft/_classic"` folder and copy the `listfile.csv`, `AssetConverterConfig.json`, and the `AssetConverter.exe` to this folder.
>[!TIP]
>The `AssetConverterConfig.json` is located in the `[root]/Resources` folder.
3. Run the `AssetConverter.exe`. This process may take up to 5 minutes, but varies from setup to setup.
4. When the Asset Converter is finished, a new folder called `Data` will be generated. This folder will be used by the [Game](https://github.com/novusengine/Game) and will be discused in that project's README.
