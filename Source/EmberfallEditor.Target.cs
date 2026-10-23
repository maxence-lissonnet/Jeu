// Copyright ADWEB Conseil. Tous droits réservés.

using UnrealBuildTool;
using System.Collections.Generic;

public class EmberfallEditorTarget : TargetRules
{
	public EmberfallEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_4;
		ExtraModuleNames.Add("Emberfall");
	}
}
