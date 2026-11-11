// Copyright (c) 2026 Salvatore Francesco Spoto
// SPDX-License-Identifier: MIT (see LICENSE at the repository root)

#include "GTDAOModule.h"
#include "GTDAORenderer.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

void FGTDAOModule::StartupModule()
{
	// Map the plugin's Shaders folder to a virtual path so .usf files can be referenced as "/Plugin/GTDAO/...".
	// Must happen before shaders are compiled (LoadingPhase = PostConfigInit in the .uplugin).
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GTDAO"));
	check(Plugin.IsValid());
	const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/GTDAO"), ShaderDir);

	GTDAO::RegisterRenderer();
}

void FGTDAOModule::ShutdownModule()
{
	GTDAO::UnregisterRenderer();
}

IMPLEMENT_MODULE(FGTDAOModule, GTDAO)
