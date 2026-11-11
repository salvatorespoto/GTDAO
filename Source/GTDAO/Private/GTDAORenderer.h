// Copyright (c) 2026 Salvatore Francesco Spoto
// SPDX-License-Identifier: MIT (see LICENSE at the repository root)

#pragma once

#include "CoreMinimal.h"

class FViewInfo;

namespace GTDAO
{
	/** Hook the GI plugin delegate */
	void RegisterRenderer();
	
	/** Unhook the GI plugin delegate */	
	void UnregisterRenderer();
	
	/** Side size of a compute thread group (8x8). */
	inline constexpr int32 ThreadGroupSize = 8;
	
	/** AO Intensity from the Post Process Volume. */
	float GetAOIntensity(const FViewInfo& View);
	
	/** World-space AO radius in uu from the Post Process Volume Radius */
	float GetAORadius(const FViewInfo& View);
}
