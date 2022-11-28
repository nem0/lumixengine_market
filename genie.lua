project "market"
	libType()
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	defines { "BUILDING_MARKET" }
	links { "engine" }
	useLua()
	defaultConfigurations()

linkPlugin("market")