# Game Shader Analysis — Visual Parity Guide

Analysis of Forza Horizon 6's car shaders (DXIL bytecode, `Cars/_library/shaders/`)
and how they compare to the project's OpenGL 3.3 GLSL renderer.

## Decompilation method

The `.pcdxil.pso` files are DXBC/DXIL bytecode (LLVM IR with DX intrinsics).
`dxc.exe -dumpbin` (Windows SDK 10.0.26100.0) successfully disassembles them to
readable LLVM IR. The `.shaderbin.xml` sidecar files contain the authoritative
texture/parameter/vertex-input metadata.

**Decompiled shaders saved to:** `tools/temp/shaders/`

---

## Our current GLSL renderer (car_model_renderer.cpp:46-377)

### What it implements

| Feature | Implementation | Quality |
|---------|---------------|---------|
| **Lighting** | Single directional light (fixed `vec3(0.4, 0.8, 0.6)`) | Basic Blinn-Phong |
| **Diffuse** | `max(dot(N, L), 0.0)` — Lambertian | Correct |
| **Specular** | Blinn-Phong `pow(max(dot(N, H), 0.0), shininess)` where shininess = `mix(8, 128, gloss)` | Approximate — not GGX |
| **Fresnel** | Schlick: `F = F0 + (1-F0) * (1-NdotV)^5` | Correct |
| **Environment** | Gradient sky (`mix(low, high, reflected.y)`) — no cubemap | Very simple |
| **Normal mapping** | Parallax-free tangent-space normal map | Correct |
| **Metallic** | `F0 = mix(0.04, albedo, metallic)` | Correct (standard PBR) |
| **Paint pattern** | UV-tiled finish texture multiplied with paint color | Correct |
| **Glitter/flake** | Procedural: quantized world-space cells + sparkle on highlight | Good approximation |
| **AO** | Single float, multiplies ambient | Correct |
| **Alpha** | Cutout with `discard` at 0.02 | Correct |

### What it does NOT implement (gaps vs game)

| Feature | Game has it | Our status | Impact |
|---------|-------------|------------|--------|
| **GGX BRDF** | ✅ Yes (DXBC inlined, likely via SMGGX.h) | ❌ No — Blinn-Phong | **HIGH** — specular highlights look wrong |
| **Clear coat** | ✅ Separate layer with roughness, coverage, tint | ❌ No | **HIGH** — metallic/candy paints look flat |
| **Glancing flop color** | ✅ `GlancingFlopColorColorParam` — color shifts at grazing angles | ❌ No (simple secondary_mix) | **MEDIUM** — two-tone paint effect missing |
| **Multiple paint colors** | ✅ `PaintColor`, `GlancingFlopColor`, `FlakeColor`, `g_CarUserColor0/1` | ❌ Only `base_paint` + `secondary_paint` | **MEDIUM** |
| **Orange peel normal** | ✅ `OrangePeelNormal_Texture` + `OrangePeelStrength` | ❌ No | **LOW** — subtle surface bump |
| **Normal map tiling** | ✅ `NormalMap00_UVTiling`, `NormalMap0_UVTiling` — multi-scale normals | ❌ Single scale | **LOW** |
| **Clear coat normal** | ✅ `ClearCoatNormal_Texture` (separate from base normal) | ❌ No | **MEDIUM** |
| **Environment cubemap** | ✅ Two cube maps (T23/T25 — diffuse + specular probes) + SSGI | ❌ Gradient only | **HIGH** — reflections look fake |
| **Radiosity** | ✅ `pg_radiosityShaderFunction` — 3 radiosity light sources with functions/colors/EV | ❌ No | **MEDIUM** — indirect lighting |
| **Light probe** | ✅ `RealtimeSpecularProbeLookupParams` — clustered specular probe lookup | ❌ No | **HIGH** — realistic reflections |
| **PBR validation** | ✅ `PBRValidation_MinColorParam`, `PBRValidation_Dielectric_MaxColorParam` | ❌ No | **LOW** — energy conservation |
| **SurfaceFX weather** | ✅ Rain streaks, snow, mud, wetness edge, noise | ❌ No (not needed for editor) | **N/A** |
| **Damage FX** | ✅ Scrape reveal, impact scratches, flake damage | ❌ No (not needed for editor) | **N/A** |
| **Rain streaks** | ✅ `carrainstreaks` UV rotation + normal perturbation | ❌ No | **N/A** |
| **Iridescence** | ✅ (paint_user variant only) `IridescenceU/V_Tiling`, `GlancingIridescencePower` | ❌ No | **LOW** |
| **Distance fade** | ✅ `DistanceFade` parameters | ❌ No | **LOW** |

---

## Game automotive_paint shader — full parameter map

### Texture slots (22 total, 21 referenced)

| Slot | Name | Purpose | Our equivalent |
|------|------|---------|---------------|
| T0 | `BaseColorAlpha_Texture` | Base colour RGB + alpha | `native_diffuse` |
| T1 | `Alpha_Texture` | Alpha cutout mask | `native_alpha` |
| T2 | `RoughMetalAO_Texture` | Roughness(R) + Metallic(G) + AO(B) | `native_surface` |
| T3 | `Normal_Texture` | Tangent-space normal map | `native_normal` |
| T4 | `NormalMap00Texture` | Fine-scale normal (orange peel) | — |
| T5 | `NormalMap0Texture` | Medium-scale normal | — |
| T6 | `OrangePeelNormal_Texture` | Orange peel normal detail | — |
| T7-T13 | `SurfaceFX_*` | Wetness, mud, snow, noise textures | — (weather FX) |
| T14 | `radTexture_pg_radiosityTexture` | Radiosity lookup texture | — |
| T15-T17 | `carrainstreaks_*` | Rain streak diffuse + normal + UV rot | — |
| T18 | `TextureWetnessEdgeTexture` | Wetness edge mask | — |
| T19 | `SnowDiffuseTexture` | Snow coverage diffuse | — |
| T20 | `SnowHighFreqNoiseTexture` | Snow detail noise | — |
| T21 | (unbounded) | Dynamic texture binding | — |
| T22 | 3D texture | 3D noise/lut | — |
| T23 | Cube map | Diffuse environment probe | — |
| T24 | 2D texture | — | — |
| T25 | Cube map | Specular environment probe | — |
| T26-T27 | StructuredBuffer | Light data buffers | — |
| T28 | 2D texture | — | — |
| T29-T30 | (unbounded) | Dynamic textures | — |
| T31-T35 | 3D + Structured | SSGI / lighting data | — |
| T36 | 2DArray | Side mask array | `side_masks` |
| T37-T38 | Cubemaps | Diffuse + specular probes | — |
| T39 | 3D | LUT | — |
| T40-T41 | 2D | Additional maps | — |

### Constant buffer parameters (65 total, 64 referenced)

#### Core paint parameters

| Parameter | Size | Type | Description |
|-----------|------|------|-------------|
| `PaintColorColorParam` | 4 | vec4 | Main paint colour (RGB + unused) |
| `GlancingFlopColorColorParam` | 4 | vec4 | Colour at grazing angles (flop) |
| `FlakeColorColorParam` | 4 | vec4 | Metallic flake/sparkle colour |
| `g_CarUserColor0` | 4 | vec4 | User-selected custom colour |
| `UserColorOverrideBool` | 1 | bool | Use user colour override |
| `ColorFlopSwitchBool` | 1 | bool | Enable glancing colour flop |

#### Paint material properties

| Parameter | Size | Type | Description |
|-----------|------|------|-------------|
| `GlancingFlopPower_floatVal` | 1 | float | Falloff exponent for colour flop |
| `FlakeRoughness_floatVal` | 1 | float | Roughness of metallic flakes |
| `FlakeCoverage_floatVal` | 1 | float | Density/coverage of flakes (0-1) |
| `Roughness_Shift_floatVal` | 1 | float | Global roughness offset |
| `GlitterIntensity_floatVal` | 1 | float | Sparkle/glitter intensity |
| `GlancingAngleStrength_floatVal` | 1 | float | How much colour changes at grazing angles |
| `Normal_Intensity_floatVal` | 1 | float | Normal map strength multiplier |

#### Clear coat

| Parameter | Size | Type | Description |
|-----------|------|------|-------------|
| `ClearCoatRoughness_floatVal` | 1 | float | Clear coat layer roughness |
| `ClearCoatCoverage_floatVal` | 1 | float | Clear coat coverage (0-1) |
| `ClearCoatOnLiveryBool` | 1 | bool | Apply clear coat over livery |
| `ClearCoatTintColorParam` | 4 | vec4 | Clear coat tint colour |

#### Normal map tiling

| Parameter | Size | Type | Description |
|-----------|------|------|-------------|
| `NormalMap00_UVTiling` | 4 | vec4 | UV tiling for fine normal |
| `NormalMap0_UVTiling` | 4 | vec4 | UV tiling for medium normal |
| `OrangePeelStrength_floatVal` | 1 | float | Orange peel normal intensity |

#### Livery switches

| Parameter | Size | Type | Description |
|-----------|------|------|-------------|
| `MaskedLiveryBool` | 1 | bool | Livery uses mask-based projection |
| `UniqueLiverySwitchBool` | 1 | bool | Unique/custom livery mode |
| `UserLiverySwitchBool` | 1 | bool | User-applied livery |
| `AlphaTransparencyBool` | 1 | bool | Enable alpha transparency |

#### Damage FX (SurfaceFX_Standard_PG)

| Parameter | Size | Type | Description |
|-----------|------|------|-------------|
| `DamageFX...ScrapeRevealColor` | 4 | vec4 | Colour under scraped paint |
| `DamageFX...ImpactMaskScratchesOnOff` | 1 | bool | Impact scratch visibility |
| `DamageFX...ScrapeScratchesOnOff` | 1 | bool | Scrape scratch visibility |
| `DamageFX...DiffuseColorDamageOnOff` | 1 | bool | Diffuse damage tinting |
| `DamageFX...FlakeGlossDamageOnOff` | 1 | bool | Flake/gloss damage |
| `DamageFX...FlakeCoverageDamageOnOff` | 1 | bool | Flake coverage damage |
| `DamageFX...ClearCoatMaskDamageOnOff` | 1 | bool | Clear coat damage |

#### PBR validation

| Parameter | Size | Type | Description |
|-----------|------|------|-------------|
| `PBRValidation_Min0ColorParam` | 4 | vec4 | Min energy conservation |
| `PBRValidation_Dielectric_Max0ColorParam` | 4 | vec4 | Max dielectric F0 |

#### Radiosity (3 light sources)

| Parameter | Size | Type | Description |
|-----------|------|------|-------------|
| `radEV1/2/3_pg_radiosity_floatVal` | 1 | float | EV value per light |
| `radColour1/2/3_pg_radiosityColorParam` | 4 | vec4 | Colour per light |
| `radFunction1/2/3_pg_radiosityCarLightParam` | 1 | int | Function type per light |

#### T10 Radiosity (car light system, 20 params)

| Parameter | Description |
|-----------|-------------|
| `DTRL_Color/Brightness` | Daytime running lights |
| `Headlight_Color/Brightness` | Headlights |
| `MarkerLight_Color/Brightness` | Marker lights |
| `Reverselight_Color/Brightness` | Reverse lights |
| `Taillight_Color/Brightness` | Tail lights |
| `Brakelight_Color/Brightness` | Brake lights |
| `TaillightIsReverseSwitch` | Tail = reverse combined |
| `TaillightSwitch` | Taillight enable |
| `pgModEV` | Global EV modifier |
| `ReceivesRadiosity_T10` | Receives radiosity flag |
| `GlassSwitch` | Glass material switch |

#### Distance fade

| Parameter | Size | Type | Description |
|-----------|------|------|-------------|
| `DEFAULT_StartDistance0_DistanceFade_floatVal` | 1 | float | Fade start distance |
| `DEFAULT_EndDistance_DistanceFade_floatVal` | 1 | float | Fade end distance |

### Light scenario variants

| Scenario | Description | LOD | Uses |
|----------|-------------|-----|------|
| `CarLightScenario` | Standard car rendering | Full | All textures, full PBR |
| `CarFPlusPlusLightScenario` | "Forza Plus Plus" enhanced | Full | All textures, enhanced |
| `CarFPlusPlusDebugLightScenario` | Debug with F++ | Full | All textures + debug viz |
| `CarLOD15LightScenario` | Medium LOD | LOD15 | Reduced features |
| `CarLOD15FPlusPlusLightScenario` | Medium LOD with F++ | LOD15 | Reduced + enhanced |
| `SimpleCarLightScenario` | Simplified car | Low | Fewer textures |
| `CarShadowDepthLightScenario` | Shadow map pass | — | Depth only |
| `CarShadowDepthNoPSLightScenario` | Shadow, no pixel shader | — | Depth only |
| `ProxyLODLightScenario` | Proxy/far LOD | Very low | Minimal |
| `WheelBlurScenario` | Wheel spin blur | — | Special |
| `CarRTBufferLightScenario` | Ray tracing buffer | — | RT |
| `CarRayTracing_T10LightScenario` | Ray tracing hit | — | RT |

### Vertex inputs

| Semantic | Components | Source |
|----------|-----------|--------|
| `TEXCOORD0` | xy | Primary UV |
| `TEXCOORD1` | zw | Secondary UV |
| `TEXCOORD2` | xy | Material UV |
| `TEXCOORD3` | zw | Livery UV |
| `TEXCOORD4` | w | Livery data |
| `WORLDPOS0` | xyz | World-space position |
| `WORLDNORMAL0` | xyz | Interpolated world normal |
| `TEXCOORD4y` | x | Damage impact |
| `DAMAGEIMPACT` | y | Damage impact |
| `DAMAGESCRAPE` | z | Damage scrape |
| `WORLDTANGENT0` | xyzw | World tangent (row 0) |
| `WORLDTANGENT1` | xyzw | World tangent (row 1) |
| `WORLDTANGENT2` | xyzw | World tangent (row 2) |
| `COLOR0` | xyzw | Vertex colour (GI radiance + AO) |
| `VERTEX_GI_RADIANCE0` | w | GI radiance |
| `VERTEX_GI_AO` | z | Vertex AO |

---

## Priority improvements for visual parity

### Tier 1 — High impact, moderate effort

1. **Replace Blinn-Phong with GGX BRDF**
   - GGX (Trowbridge-Reitz) normal distribution + Smith geometry + Fresnel
   - ~50 lines of GLSL, massive visual improvement
   - Key difference: GGX has longer specular tails, more realistic highlights

2. **Add clear coat layer**
   - Separate specular lobe on top of base paint
   - Parameters: `ClearCoatRoughness`, `ClearCoatCoverage`, `ClearCoatTintColor`
   - ~30 lines of GLSL

3. **Environment cubemap reflection**
   - Replace gradient sky with a simple HDR cubemap (can be generated procedurally)
   - Or use a baked reflection probe from the game data
   - The game uses two cubemaps (diffuse + specular probes)

### Tier 2 — Medium impact

4. **Glancing flop color**
   - Color shifts from `PaintColor` to `GlancingFlopColor` at grazing angles
   - Controlled by `GlancingFlopPower` and `GlancingAngleStrength`
   - ~10 lines of GLSL

5. **Multi-color paint system**
   - Add `FlakeColor` separate from base paint
   - The game has 4 color channels: PaintColor, GlancingFlopColor, FlakeColor, UserColor

6. **Orange peel normal map**
   - Subtle surface roughness variation (like real car paint orange peel)
   - Add `OrangePeelNormal_Texture` + `OrangePeelStrength`

### Tier 3 — Low impact (optional)

7. **PBR energy conservation** — `PBRValidation` min/max
8. **Distance fade** — fade out distant parts
9. **Radiosity** — indirect lighting from car lights (taillights illuminating ground)
10. **Normal map tiling** — multi-scale normals (`NormalMap00_UVTiling`, `NormalMap0_UVTiling`)

---

## Cross-shader comparison

| Feature | automotive_paint | livery | standard | glass | carbonfiber | tires_pg |
|---------|-----------------|--------|----------|-------|-------------|----------|
| Textures | 22 | 19 | 19 | 17 | 22 | 21 |
| Parameters | 65 | 61 | 77 | 68 | 51 | 38 |
| Clear coat | ✅ | ❌ | ❌ | ❌ | ✅ | ❌ |
| Flake/sparkle | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ |
| Glancing flop | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ |
| Emissive | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ |
| Iridescence | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ |
| Break/crack | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ |
| Weather FX | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Radiosity | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ |
| Rain streaks | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ |
| Weave pattern | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ |
| Tire wear | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ |
| Sidewall text | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ |
| Parallax | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ |
