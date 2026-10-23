// Copyright ADWEB Conseil. Tous droits réservés.

using UnrealBuildTool;

public class Emberfall : ModuleRules
{
	public Emberfall(ReadOnlyTargetRules Target) : base(Target)
	{
		// PCH partagés du moteur + PCH explicites si un .cpp inclut son propre en-tête en premier.
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"GameplayTags",
			"AIModule",
			"NavigationSystem",
			"Niagara",
			"DeveloperSettings",
			"UMG",
			"NetCore"
		});

		// Modules utilisés uniquement dans les .cpp. Chaos / GeometryCollectionEngine / PhysicsCore retirés :
		// aucun fichier du module n'en inclut d'en-tête (les débris passent par un acteur Blueprint). À rajouter
		// ici si du code C++ doit un jour manipuler directement une GeometryCollection.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"GameplayTasks"
		});
	}
}
