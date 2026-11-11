// Copyright (c) 2026 Salvatore Francesco Spoto
// SPDX-License-Identifier: MIT (see LICENSE at the repository root)

#include "GTDAORenderer.h"

// Renderer private headers: FScene, FViewInfo, FGlobalIlluminationPluginDelegates.
#include "DeferredShadingRenderer.h"
#include "ScenePrivate.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "PixelShaderUtils.h"
#include "RenderUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneRenderTargetParameters.h"
#include "ShaderParameterStruct.h"
#include "SystemTextures.h"

//// Console variables

static TAutoConsoleVariable<int32> CVarGTDAOEnable(
	TEXT("r.GTDAO.Enable"), 1,
	TEXT("Enable GTDAO (requires Dynamic Global Illumination Method = Plugin in Project Settings)."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarGTDAOAO(
	TEXT("r.GTDAO.AO"), 0,
	TEXT("Apply the GTDAO horizon AO to the scene, in place of UE's SSAO o GTAO.\n")
	TEXT(" 0: off (default)\n")
	TEXT(" 1: on"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGTDAORadius(
	TEXT("r.GTDAO.Radius"), 0.0f,
	TEXT("World-space radius of the horizon search, in uu. 0 (default): use the Post Process Volume AO Radius."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarGTDAOSlices(
	TEXT("r.GTDAO.Slices"), 2,
	TEXT("Slices per pixel (each slice is marched in both directions)."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarGTDAOSteps(
	TEXT("r.GTDAO.Steps"), 8,
	TEXT("Steps per side of each slice."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarGTDAODF(
	TEXT("r.GTDAO.DF"), 0,
	TEXT("Add the Global Distance Field rays to the AO (needs r.AOGlobalDistanceField.DetailedNecessityCheck 0 when nothing else builds it)."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarGTDAODFRays(
	TEXT("r.GTDAO.DF.Rays"), 1.0f,
	TEXT("Distance field rays per pixel.\n")
	TEXT(" >= 1: rays per pixel, at full resolution (rounded, max 16)\n")
	TEXT(" 0..1: fraction of a ray per pixel: one ray per NxN pixels, N = round(1 / sqrt(value)), max 8\n")
	TEXT("       (0.25 = 1 ray per 2x2 pixels, 0.111 = 1 per 3x3; the DF pass runs at 1/N resolution)"),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarGTDAODFMinDistance(
	TEXT("r.GTDAO.DF.MinDistance"), 50.0f,
	TEXT("Where the distance field rays start, in cm."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarGTDAODFMaxDistance(
	TEXT("r.GTDAO.DF.MaxDistance"), 300.0f,
	TEXT("Where the distance field rays end, in cm: a hit fades out toward this distance."),
	ECVF_RenderThreadSafe | ECVF_Scalability);


//// Compile-time settings of the AO shader not exposed as CVars, added to the shader as #defines 
namespace GTDAO
{
	// Clamp of the projected search radius, in pixels
	static constexpr float MaxRadiusPixels = 256.0f;
	// Thickness heuristic, 0 = off (infinitely thick occluders), bigger values reduce the occlusion of thin objects.
	static constexpr float Thickness = 0.5f;
	// March the HZB instead of the full resolution depth.
	static constexpr int32 UseHZB = 1;
	// Width of the DF cones: tan of their half angle
	static constexpr float DFConeTan = 0.5f;
}

//// SHADERS:
// - FGTDAOSampleDistanceFieldCS (SampleDistanceFieldCS, GTDAO.usf): DF cone samples
// - FGTDAOCS (GTDAOCS, GTDAO.usf): screens space horizon based march per pixel
// - FGTDAOCompositePS (GTDAOCompositePS): copies the AO into UE's AO texture (r.GTDAO.AO 1).

namespace GTDAO
{
	/**
	 * Compute Shader: computes the Ambient Occlusion using horizon marching and distance field sampling.
	 */
	class FGTDAOCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGTDAOCS);
		SHADER_USE_PARAMETER_STRUCT(FGTDAOCS, FGlobalShader);

		// Declare Shader Permutation: USE_DF_TEXTURE 0 and 1.
		class FUseDFTextureDim : SHADER_PERMUTATION_BOOL("USE_DF_TEXTURE");
		using FPermutationDomain = TShaderPermutationDomain<FUseDFTextureDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			// Info about the view
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			// Scene textures: depth, normal, position, motion vectors, etc.
			SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
			// Size of the AO texture: the view rect size. Used to discard threads outside the AO texture.
			SHADER_PARAMETER(FIntPoint, OutputSize)	
			SHADER_PARAMETER(float, Radius)
			SHADER_PARAMETER(int32, NumSlices)
			SHADER_PARAMETER(int32, NumSteps)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HZBTexture)
			SHADER_PARAMETER(float, AOIntensity)
			// Only USE_DF_TEXTURE 1: the DF texture (see OutDF in GTDAO.usf), its sampler and downsample factor
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DFTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, DFSampler)
			SHADER_PARAMETER(int32, DFDownsampleFactor)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutAO)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			// These parameters are not exposed as CVars, but just hardcoded at compile time
			OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
			// Some not exposed shader paramters
			OutEnvironment.SetDefine(TEXT("MAX_RADIUS_PIXELS"), MaxRadiusPixels);
			// The shader wants the weight of the old horizon when a sample is lower than it: converted here
			// like UE GTAO does (1 - Thickness^2), capped below 1 so the horizon can still come down.
			OutEnvironment.SetDefine(TEXT("THICKNESS_BLEND"), FMath::Clamp(1.0f - Thickness * Thickness, 0.0f, 0.99f));
			OutEnvironment.SetDefine(TEXT("USE_HZB"), UseHZB);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FGTDAOCS, "/Plugin/GTDAO/Private/GTDAO.usf", "GTDAOCS", SF_Compute);
	
	/**
	 * Compute Shader: cone sampling the signed distance field 
	 */
	class FGTDAOSampleDistanceFieldCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGTDAOSampleDistanceFieldCS);
		SHADER_USE_PARAMETER_STRUCT(FGTDAOSampleDistanceFieldCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			// Depth and normal of the pixel the rays start from
			SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
			// Size of the DF texture (view rect / DFDownsampleFactor, rounded up)
			SHADER_PARAMETER(FIntPoint, OutputSize)
			// 1 = one DF texel per pixel, 2 = one per 2x2 pixels, ...
			SHADER_PARAMETER(int32, DFDownsampleFactor)
			SHADER_PARAMETER(int32, DFRaysPerTexel)
			SHADER_PARAMETER(float, DFMinDistance)
			SHADER_PARAMETER(float, DFMaxDistance)
			// R16F: visibility (1 = unoccluded)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutDF)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
			OutEnvironment.SetDefine(TEXT("DF_CONE_TAN"), DFConeTan);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FGTDAOSampleDistanceFieldCS, "/Plugin/GTDAO/Private/GTDAO.usf", "SampleDistanceFieldCS", SF_Compute);

	/*
	 * Pixel Shader: copies the computed AO into UE's scene AO texture.
	 */
	class FGTDAOCompositePS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FGTDAOCompositePS);
		SHADER_USE_PARAMETER_STRUCT(FGTDAOCompositePS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AOTexture)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};
	IMPLEMENT_GLOBAL_SHADER(FGTDAOCompositePS, "/Plugin/GTDAO/Private/GTDAO.usf", "GTDAOCompositePS", SF_Pixel);
	
	/** Get AO intensity from the Post Process Volume */
	float GetAOIntensity(const FViewInfo& View)
	{
		return View.FinalPostProcessSettings.AmbientOcclusionIntensity;
	}
	
	/** Get AO world-space radius from the Post Process Volume */
	float GetAORadius(const FViewInfo& View)
	{
		const float Override = CVarGTDAORadius.GetValueOnRenderThread(); // the CVar overrides the radius
		return FMath::Max(Override > 0.0f ? Override : View.FinalPostProcessSettings.AmbientOcclusionRadius, 1.0f);
	}
	
	/** Get the UE's scene AO texture. */
	static FRDGTextureRef GetSceneAOTexture(const FViewInfo& View)
	{
		const FSceneTextures& SceneTextures = static_cast<const FViewFamilyInfo&>(*View.Family).GetSceneTextures();
		return SceneTextures.ScreenSpaceAO;
	}

	/**
	 * If the parameter r.GTDAO.DF.Rays (number of cone traced rays per pixel) is between 0 and 1, the sampling resolution
	 * is downsampled.
	 * Eg:
	 * r.GTDAO.DF.Rays >= 1, full resolution, round(Rays) rays per pixel in one pass, each one with DF_CONE_SAMPLES reads
	 * r.GTDAO.DF.Rays .25 -> N 2 (half resolution), 1 ray each 2x2 (4) pixels
	 */
	static int32 GetDFDownsampleFactor()
	{
		const float DFRays = CVarGTDAODFRays.GetValueOnRenderThread();
		if (DFRays <= 0.0f || DFRays >= 1.0f)
		{
			return 1;
		}
		return FMath::Clamp(FMath::RoundToInt(1.0f / FMath::Sqrt(DFRays)), 1, 8);
	}

	/** Add the horizon based ambient occlusion pass, compute the GTAO and merges the previous DF pass */
	static FRDGTextureRef AddGTDAOPass(FRDGBuilder& GraphBuilder, const FViewInfo& View, FRDGTextureRef DFTexture)
	{
		// Define scope for Unreal Insights
		RDG_EVENT_SCOPE(GraphBuilder, "GTDAOMarch");
		
		const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.FeatureLevel);
		
		// Allocate parameters for AO pass
		FGTDAOCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGTDAOCS::FParameters>();
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTextures = GetSceneTextureShaderParameters(View);
		const FIntPoint ViewSize = View.ViewRect.Size();
		PassParameters->OutputSize = ViewSize;
		PassParameters->Radius = GetAORadius(View);
		PassParameters->AOIntensity = GetAOIntensity(View);
		PassParameters->NumSlices = FMath::Clamp(CVarGTDAOSlices.GetValueOnRenderThread(), 1, 8);
		PassParameters->NumSteps = FMath::Clamp(CVarGTDAOSteps.GetValueOnRenderThread(), 1, 32);
		PassParameters->HZBTexture = View.HZB != nullptr ? View.HZB : GSystemTextures.GetBlackDummy(GraphBuilder); // GetBlackDummy is a fallback 1x1 black texture 
		// Use DF texture if available
		const bool bUseDF = DFTexture != nullptr;
		PassParameters->DFTexture = DFTexture;
		// Bilinear upsample, clamped at the borders (the last texel row/column is not repeated beyond)
		PassParameters->DFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->DFDownsampleFactor = GetDFDownsampleFactor();

		// Instruct the RDG to create a texture for the output AO
		const FRDGTextureDesc AODesc = FRDGTextureDesc::Create2D(ViewSize, PF_R16F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
		FRDGTextureRef AOTexture = GraphBuilder.CreateTexture(AODesc, TEXT("GTDAO.AO"));
		PassParameters->OutAO = GraphBuilder.CreateUAV(AOTexture);

		// Pick the permutation compiled with USE_DF_TEXTURE = bUseDF
		FGTDAOCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FGTDAOCS::FUseDFTextureDim>(bUseDF);
		TShaderMapRef<FGTDAOCS> ComputeShader(ShaderMap, PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("AO %dx%d%s", ViewSize.X, ViewSize.Y, bUseDF ? TEXT(" DF") : TEXT("")),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(ViewSize, ThreadGroupSize));

		// Return the AO texture to be passed to the next pass (so the RDG could infer the pass order)
		return AOTexture;
	}
	
	/**
	 * Samples the distance field and returns the DF texture.
	 */
	static FRDGTextureRef AddDistanceFieldSamplingPass(FRDGBuilder& GraphBuilder, const FViewInfo& View)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "GTDAODF");

		const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.FeatureLevel);

		FGTDAOSampleDistanceFieldCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGTDAOSampleDistanceFieldCS::FParameters>();
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTextures = GetSceneTextureShaderParameters(View);

		// r.GTDAO.DF.Rays >= 1: rays per texel (= per pixel); below 1: downsampled, one ray per texel.
		const int32 DFDownsampleFactor = GetDFDownsampleFactor();
		PassParameters->DFDownsampleFactor = DFDownsampleFactor;
		PassParameters->DFRaysPerTexel = FMath::Clamp(FMath::RoundToInt(CVarGTDAODFRays.GetValueOnRenderThread()), 1, 16);
		PassParameters->DFMinDistance = FMath::Max(CVarGTDAODFMinDistance.GetValueOnRenderThread(), 0.0f);
		PassParameters->DFMaxDistance = FMath::Max(CVarGTDAODFMaxDistance.GetValueOnRenderThread(), PassParameters->DFMinDistance + 1.0f);
		const FIntPoint DFSize = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), DFDownsampleFactor);
		PassParameters->OutputSize = DFSize;
		// White = visibility 1 (unoccluded): only the clear value, the pass writes every texel
		const FRDGTextureDesc DFDesc = FRDGTextureDesc::Create2D(DFSize, PF_R16F, FClearValueBinding::White, TexCreate_ShaderResource | TexCreate_UAV);
		FRDGTextureRef DFTexture = GraphBuilder.CreateTexture(DFDesc, TEXT("GTDAO.DF"));
		PassParameters->OutDF = GraphBuilder.CreateUAV(DFTexture);

		// One thread per DF texel, not per pixel
		TShaderMapRef<FGTDAOSampleDistanceFieldCS> ComputeShader(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("DF %dx%d", DFSize.X, DFSize.Y),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DFSize, ThreadGroupSize));

		return DFTexture;
	}

	
	/** Writes our AO into UE's scene AO texture (Target) */
	static void AddAOCompositePass(FRDGBuilder& GraphBuilder, const FViewInfo& View, FRDGTextureRef Target, FRDGTextureRef AOTexture)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "GTDAOComposite");
		const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.FeatureLevel);

		FGTDAOCompositePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGTDAOCompositePS::FParameters>();
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->AOTexture = AOTexture;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(Target, HasBeenProduced(Target) ? ERenderTargetLoadAction::ELoad : ERenderTargetLoadAction::ENoAction);

		TShaderMapRef<FGTDAOCompositePS> PixelShader(ShaderMap);
		FPixelShaderUtils::AddFullscreenPass(
			GraphBuilder,
			ShaderMap,
			RDG_EVENT_NAME("AO Composite"),
			PixelShader,
			PassParameters,
			View.ViewRect);
	}
	
	/** GI Plugin Delegate callbacks, called by the engine renderer once per view. */
	static void RenderDiffuseIndirectLight(
		const FScene& Scene,
		const FViewInfo& View,
		FRDGBuilder& GraphBuilder,
		FGlobalIlluminationPluginResources& Resources)
	{
		// Plugin enabled / disabled
		if (CVarGTDAOEnable.GetValueOnRenderThread() == 0)
			return;

		// Custom AO enabled / disabled
		if (CVarGTDAOAO.GetValueOnRenderThread() == 0)
			return;
		
		// Get the UE AO buffer
		FRDGTextureRef SceneAO =  GetSceneAOTexture(View);
		if (!SceneAO)
			return;

		RDG_EVENT_SCOPE(GraphBuilder, "GTDAO");

		// Add the Ambient Occlusion passes to the RDG.
		// Distance field first: the march reads it (RDG orders the passes from the reads and writes anyway).
		FRDGTextureRef DFTexture = nullptr;
		if (CVarGTDAODF.GetValueOnRenderThread() != 0)
		{
			DFTexture = AddDistanceFieldSamplingPass(GraphBuilder, View);
		}
		FRDGTextureRef AOTexture = AddGTDAOPass(GraphBuilder, View, DFTexture);
		AddAOCompositePass(GraphBuilder, View, SceneAO, AOTexture); // overwrite UE's SSAO
	}
	
	static FDelegateHandle RenderDelegateHandle;

	// Register GI plugin delegate
	void RegisterRenderer()
	{
		RenderDelegateHandle = FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectLight().AddStatic(&RenderDiffuseIndirectLight);
	}

	// Un-register GI plugin delegate
	void UnregisterRenderer()
	{
		if (RenderDelegateHandle.IsValid())
		{
			FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectLight().Remove(RenderDelegateHandle);
			RenderDelegateHandle.Reset();
		}
	}
}
