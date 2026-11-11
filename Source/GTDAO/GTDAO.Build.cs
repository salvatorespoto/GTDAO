// Copyright (c) 2026 Salvatore Francesco Spoto
// SPDX-License-Identifier: MIT (see LICENSE at the repository root)

using System.IO;
using UnrealBuildTool;

public class GTDAO : ModuleRules
{
	public GTDAO(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// FScene, FViewInfo, and the GI plugin delegate live in the Renderer's private/internal headers.
		string RendererDir = GetModuleDirectory("Renderer");
		PrivateIncludePaths.AddRange(new string[]
		{
			Path.Combine(RendererDir, "Private"),
			Path.Combine(RendererDir, "Internal"),
		});

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Projects",
			"RenderCore",
			"RHI",
			"Renderer",
		});
	}
}
