# Benchmark 2026-11-11, PL_Medieval_Docks

Full report with all the images and error maps: [index.html](index.html) (open it locally, GitHub does not render HTML).

Setup: `PL_Medieval_Docks`, 4 level bookmarks, 1395x619, RTX 2070 laptop. AO radius 141 cm for GTAO and GTDAO,
GTDAO 2 slices x 8 steps per side with HZB, DF rays 0.5-3 m with 4 cone samples, no denoiser.
Reference: `REFINF`, ray traced sky visibility (inline ray queries against UE's ray tracing scene, cosine rays up to 1 km, 16 rays x 256 frames, accumulated while the camera stays still).
Errors over the pixels closer than 500 m. Cost: median GPU ms over 240 frames (Unreal Insights).

| Config | DF resolution | MAE vs REFINF (B1 / B2 / B3 / B4) | mean | GPU ms (B1 / B2 / B3 / B4) | mean |
|---|---|---|---|---|---|
| GTAO (UE, full res) | - | 0.747 / 0.371 / 0.491 / 0.594 | 0.551 | 0.144 / 0.079 / 0.123 / 0.137 | 0.121 |
| GTDAO_DF | full | 0.347 / 0.304 / 0.369 / 0.368 | 0.347 | 0.212 / 0.232 / 0.231 / 0.222 | 0.224 |
| GTDAO_DF2 | 1/2 (1 ray per 2x2) | 0.314 / 0.290 / 0.331 / 0.332 | 0.317 | 0.187 / 0.194 / 0.196 / 0.193 | 0.193 |
| GTDAO_DF3 | 1/3 (1 ray per 3x3) | 0.307 / 0.290 / 0.323 / 0.326 | 0.312 | 0.180 / 0.182 / 0.185 / 0.185 | 0.183 |

Notes:
- Every method is brighter than the sky visibility (positive bias): GTAO +0.37..+0.75, GTDAO +0.19..+0.28.
- The low resolution DF is *better* than the full resolution one: the bilinear upsample averages the
  noise of neighbouring rays. It is a blur, not more information.
- The march costs ~0.16 ms in every GTDAO config; DF pass + composite add ~0.07 ms with the DF at full resolution, ~0.04 at 1/2, ~0.03 at 1/3.
- `summary_REFINF.csv`: bias, MAE, RMSE and % of pixels off by more than 0.1 for every bookmark and config.

## Cost against GTAO / SSAO + DFAO

A second run (trace `ao_bench_20261111_093401`, CSVs in `cost_csv/`), same views, 240 frames per config,
median GPU ms. DFAO = the `DistanceFieldLighting` scope; it was turned on/off with `ShowFlag.DistanceFieldAO`
(the Sky Light is movable with Cast Shadows, so DFAO really runs). UE SSAO has no scope of its own in
Insights: I took it as the growth of `DiffuseIndirectAndAO` compared to the GTAO run.

| Config | B1 / B2 / B3 / B4 | mean |
|---|---|---|
| GTAO | 0.146 / 0.078 / 0.125 / 0.137 | 0.121 |
| GTAO + DFAO | 0.512 / 0.425 / 0.603 / 0.403 | 0.486 |
| SSAO + DFAO | 0.366 / 1.049 / 0.481 / 0.270 | 0.541 |
| GTDAO, DF full | 0.212 / 0.233 / 0.231 / 0.222 | 0.225 |
| GTDAO, DF 1/3 | 0.181 / 0.182 / 0.186 / 0.187 | 0.184 |

DFAO alone is 0.367 / 0.348 / 0.478 / 0.266. The SSAO in B2 (~0.7 ms) looks odd and I didn't
investigate it; without B2, SSAO + DFAO is about the same as GTAO + DFAO.

What I take from it: GTDAO without denoiser costs 40-45% of GTAO + DFAO here, so a cheap denoiser
(0.05-0.1 ms) would still keep it cheaper. It says nothing about quality: DFAO is not in the captured
AO buffer, so its error against REFINF is not measured, and DFAO reaches farther (10 m, mesh distance
fields) than GTDAO (3 m, Global SDF).

The captures (HighResShot with the AO buffer dumped as linear EXR), the reference and the Insights
export were done with some scripts of my development project, not published here.
