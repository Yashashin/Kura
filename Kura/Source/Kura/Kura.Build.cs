using UnrealBuildTool;

public class Kura : ModuleRules
{
	public Kura(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"InputCore",
			"ApplicationCore", // clipboard (Copy Package Path)
			"AssetTools",    // IAssetTools::GetTypeColor - the Content Browser's own type colors
			"Json",
			"Projects",  // IPluginManager (KuraStyle locates Resources/)
			"ToolMenus",
			"DesktopPlatform", // OpenDirectoryDialog (Library Root picker, F5/first-run)
			"PythonScriptPlugin",
			"UnrealEd",      // ThumbnailTools, USceneThumbnailInfoWithPrimitive (SPEC 7.2)
			"ImageCore",     // FImageView for PNG export
			"ImageWrapper",  // PNG decode for the panel's thumbnail brushes
		});

		// SSearchableComboBox (the collection picker, F8) lived in ToolWidgets
		// until 5.8 moved it into Slate (SPEC 7.4).
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 8)
		{
			PrivateDependencyModuleNames.Add("ToolWidgets");
		}
	}
}
