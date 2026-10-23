// Copyright ADWEB Conseil. Tous droits réservés.

using UnrealBuildTool;
using System.Collections.Generic;

public class EmberfallServerTarget : TargetRules
{
	public EmberfallServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_4;
		ExtraModuleNames.Add("Emberfall");
	}
}
