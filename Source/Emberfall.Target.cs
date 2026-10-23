// Copyright ADWEB Conseil. Tous droits réservés.

using UnrealBuildTool;
using System.Collections.Generic;

public class EmberfallTarget : TargetRules
{
	public EmberfallTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_4;
		ExtraModuleNames.Add("Emberfall");
	}
}
