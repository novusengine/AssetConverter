local dep = Solution.Util.CreateDepTable("Meshoptimizer", {})

Solution.Util.CreateStaticLib(dep.Name, Solution.Projects.Current.BinDir, dep.Dependencies, function()
    Solution.Util.SetLanguage("C++")
    Solution.Util.SetCppDialect(20)

    local sourceDir = dep.Path .. "/meshoptimizer/src"
    local files =
    {
        sourceDir .. "/meshoptimizer.h",
        sourceDir .. "/allocator.cpp",
        sourceDir .. "/clusterizer.cpp",
        sourceDir .. "/indexgenerator.cpp",
        sourceDir .. "/meshletutils.cpp",
        sourceDir .. "/overdrawoptimizer.cpp",
        sourceDir .. "/simplifier.cpp",
        sourceDir .. "/tangentspace.cpp",
        sourceDir .. "/vcacheoptimizer.cpp",
        sourceDir .. "/vfetchoptimizer.cpp",
    }

    Solution.Util.SetFiles(files)
    Solution.Util.SetIncludes(sourceDir)
end)

Solution.Util.CreateDep(dep.NameLow, dep.Dependencies, function()
    Solution.Util.SetIncludes(dep.Path .. "/meshoptimizer/src")
    Solution.Util.SetLinks(dep.Name)
end)
