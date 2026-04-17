// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class TraceAgentCli : ModuleRules
{
	public TraceAgentCli(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicIncludePathModuleNames.Add("Launch");

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Json",
				"Projects",
				"TraceServices",
			}
		);
	}
}
