// Copyright (c) 2026 Salvatore Francesco Spoto
// SPDX-License-Identifier: MIT (see LICENSE at the repository root)

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * Module for GTDAO, an Ambient Occlusion that combines a Ground Truth Ambient Occlusion style march, a screen
 * space technique, with some samples from the Global Signed Distance Field.
 */
class FGTDAOModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
