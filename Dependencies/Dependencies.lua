-- Dependencies
Solution.Util.Print("-- Creating Dependencies --")
Solution.Util.ClearFilter()
Solution.Util.SetGroup(Solution.DependencyGroup)

local dependencies =
{
    "casc/casc.lua",
    "cuttlefish/cuttlefish.lua",
    "jolt/jolt.lua",
}

for k,v in pairs(dependencies) do
    include(v)
    Solution.Util.ClearFilter()
end

Solution.Util.SetGroup("")
Solution.Util.Print("-- Finished with Dependencies --\n")