// SPDX-License-Identifier: GPL-3.0-or-later
// nicktheduke: lens frost and lens water.
// Frost: glaze ice frozen onto the lens, growing in from the screen edges as you get colder (v9). Idea credit: kingeric1992's Weather FX lens. No code or textures from it;
// FrostMask_nicktheduke.dds and LensDrops_nicktheduke.dds are generated from scratch (tools/nicktheduke_*_generator.py).
#ifdef VSHADER
float4 mainVS(uint vertexId : SV_VertexID) : SV_Position
{
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
#endif

#ifdef PSHADER
Texture2D<float4> Scene : register(t0);
Texture2D<float4> FrostMask : register(t1);  // nicktheduke v9: screen-shaped glaze ice map (R fern, G arrival, B bubbles, A snow)
Texture2D<float4> DropAtlas : register(t2);
Texture2D<float> LensDirt : register(t3);  // nicktheduke: procedural lens dirt (dust bokeh, smudges, streaks, lint)  // 8x8 droplet shapes: rgb = normal, a = water thickness
Texture2D<float> SceneDepth : register(t4);  // nicktheduke: depth after the z-prepass, for heat shimmer
SamplerState SceneSampler : register(s0);
SamplerState WrapSampler : register(s1);

cbuffer ScreenWeatherParameters : register(b0)
{
	float4 BufferDim;    // 1/w, 1/h, time, float input
	float4 Frost;        // amount, coverage, scale, opacity
	float4 FrostLook;    // refraction, scene blur, glow, crystal intensity (day/night already applied)
	float4 FrostTint;    // rgb, edge softness
	float4 Water;        // rain, sheet, drop size, refraction
	float4 WaterTiming;  // drop lifetime, frost mask texture present, droplet atlas present
	float4 HDRData;
	float4 FrostBreath;  // rate (Hz), depth, contrast, unused
	float4 SunLens;      // sun screen uv xy, strength (0 = off), lens dirt amount
	float4 SunLook;      // glint strength, unused x3
	float4 Atmos;        // heat haze, ash, dust, fog (only one of ash/dust/fog is non-zero at a time)
	float4 AtmosLook;    // fog halo, fog condensation, embers, cold breath shape (0-5: three shapes, each either way)
	float4 Thermal;      // thaw drips, cold breath vapor, seconds since the exhale began, entry fog
	float4 ThermalLook;  // exhale strength, shimmer source count, depth sign (+1 larger = farther), depth present
	float4 Shimmer[8];   // per heat source: screen uv xy, radius (screen heights), strength
	float4 ShimmerDepth[2];
	float4 MoonLens;     // nicktheduke: moon screen uv xy, strength (0 = off), apparent radius (screen heights)
	float4 FrostShape;   // nicktheduke: frost shape (0 = hugs the frame, 1 = round), unused x3
};

// PCG hash (O'Neill), integer in, [0,1) out.
uint PcgHash(uint v)
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float Hash21(float2 cell, uint salt)
{
	int2 c = int2(cell);
	uint h = PcgHash(asuint(c.x) ^ PcgHash(asuint(c.y) ^ (salt * 0x9E3779B9u)));
	return float(h & 0x00FFFFFFu) / 16777216.0;
}

float ValueNoise(float2 p, uint salt)
{
	float2 i = floor(p);
	float2 f = frac(p);
	float2 u = f * f * (3.0 - 2.0 * f);
	float a = Hash21(i, salt);
	float b = Hash21(i + float2(1, 0), salt);
	float c = Hash21(i + float2(0, 1), salt);
	float d = Hash21(i + float2(1, 1), salt);
	return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float Fbm(float2 p, uint salt)
{
	float sum = 0.0;
	float amplitude = 0.5;
	[loop] for (uint octave = 0; octave < 5; ++octave)
	{
		sum += ValueNoise(p, salt + octave) * amplitude;
		p = p * 2.03 + float2(17.1, 9.7);
		amplitude *= 0.5;
	}
	return sum / 0.96875;
}

float Luminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

static const float2 kDisk[8] = {
	float2(0.707, 0.0), float2(-0.5, 0.5), float2(0.0, -0.9), float2(0.9, 0.55),
	float2(-0.85, -0.35), float2(0.3, 0.95), float2(-0.25, -0.45), float2(0.55, -0.6)
};

// Wide two-ring blur for the frosted area (looking through ice is very soft).
float3 SampleFrosted(float2 uv, float radiusPixels, float range)
{
	float3 sum = Scene.SampleLevel(SceneSampler, uv, 0).rgb;
	[loop] for (uint i = 0; i < 8; ++i)
	{
		const float2 d = kDisk[i] * radiusPixels * BufferDim.xy;
		sum += Scene.SampleLevel(SceneSampler, uv + d, 0).rgb;
		sum += Scene.SampleLevel(SceneSampler, uv + float2(-d.y, d.x) * 0.45, 0).rgb;
	}
	return saturate(sum / 17.0 / range);
}

float3 SampleBlurred(float2 uv, float radiusPixels, float range)
{
	float3 sum = Scene.SampleLevel(SceneSampler, uv, 0).rgb;
	[branch] if (radiusPixels > 0.25)
	{
		[loop] for (uint i = 0; i < 8; ++i)
			sum += Scene.SampleLevel(SceneSampler, uv + kDisk[i] * radiusPixels * BufferDim.xy, 0).rgb;
		sum /= 9.0;
	}
	return saturate(sum / range);
}

// The frost glow: a very wide blur that keeps mostly the bright parts of the scene, so only bright areas
// survive as a wide, faint haze, so the ice picks up a soft milky haze where the scene is bright.
float3 SampleGlow(float2 uv, float range)
{
	const float radius = 0.045;  // fraction of the screen height
	float3 sum = 0.0;
	[loop] for (uint ring = 1; ring <= 3; ++ring)
	{
		const float rr = radius * ring / 3.0;
		[loop] for (uint i = 0; i < 8; ++i)
		{
			float2 d = kDisk[i] * rr;
			d = (ring == 2) ? float2(-d.y, d.x) : d;
			d.x *= BufferDim.x / BufferDim.y;
			sum += Scene.SampleLevel(SceneSampler, uv + d, 0).rgb;
		}
	}
	const float3 blurred = saturate(sum / 24.0 / range);
	const float bright = saturate((Luminance(blurred) - 0.2) * 1.6);
	return blurred * (0.35 + 0.65 * bright);
}

struct LensWater
{
	float2 offset;  // uv offset to apply to the scene
	float mask;     // how much water covers this pixel
	float highlight;
	float rim;
	float2 normal;  // nicktheduke: surface normal of the water at this pixel, screen space (y down)
};

void AddDropLayer(inout LensWater water, float2 q, float aspect, float cellSize, uint salt, float spawn, float slide, float time, float lifetime)
{
	float2 g = q / cellSize;
	float2 cell = floor(g);
	float2 f = frac(g);
	float h0 = Hash21(cell, salt);
	float h1 = Hash21(cell, salt + 1u);
	float h2 = Hash21(cell, salt + 2u);
	float h3 = Hash21(cell, salt + 3u);
	if (h2 >= spawn)
		return;

	float life = lifetime * (0.6 + 0.8 * h0);
	float t = frac(time / life + h1);
	float2 center = float2(0.3 + 0.4 * h3, 0.25 + 0.3 * frac(h0 * 7.13));
	center.y += slide * t * t * step(0.55, h1);
	float radius = (0.14 + 0.16 * frac(h3 * 13.7)) * (1.0 - 0.35 * t);
	float2 d = f - center;
	d.y *= 1.15;
	float dist = length(d) / radius;
	float presence = smoothstep(0.0, 0.06, t) * (1.0 - smoothstep(0.7, 1.0, t));
	float m = (1.0 - smoothstep(0.82, 1.0, dist)) * presence;
	if (WaterTiming.z <= 0.5 && m <= water.mask)
		return;

	[branch] if (WaterTiming.z > 0.5)
	{
		// One of 64 generated droplet shapes (tails, merged pairs, satellites), slightly tilted, sized to this drop.
		float sn, cs;
		sincos((h0 - 0.5) * 0.5, sn, cs);
		float2 local = float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs) / (radius * 2.2) + 0.5;
		[branch] if (any(local < 0.03) || any(local > 0.97))
			return;
		const uint tile = min((uint)(frac(h0 * 91.7 + h3 * 13.1) * 64.0), 63u);
		const float2 atlasUV = (float2(tile % 8u, tile / 8u) + local) / 8.0;
		const float4 drop = DropAtlas.SampleLevel(SceneSampler, atlasUV, 0);
		const float dm = smoothstep(0.05, 0.35, drop.a) * presence;
		if (dm <= water.mask)
			return;
		const float2 normal = drop.rg * 2.0 - 1.0;
		const float2 lensOffset = -normal * radius * cellSize * 1.6;
		water.mask = dm;
		water.offset = float2(lensOffset.x / aspect, lensOffset.y);
		// Light from the upper left catches the curved side of the drop; the thin edge darkens it.
		water.highlight = pow(saturate(dot(normal, normalize(float2(-0.55, -0.75))) * 1.4), 3.0) * dm;
		water.normal = normal;
		water.rim = saturate(length(normal) * 1.3 - 0.3) * dm * (1.0 - drop.a);
		return;
	}

	float2 n = d / max(radius, 1e-3);
	float2 lensOffset = -n * radius * cellSize * 0.9;
	water.mask = m;
	water.offset = float2(lensOffset.x / aspect, lensOffset.y);
	water.highlight = smoothstep(0.35, 0.0, length(n + float2(0.32, 0.42))) * m;
	water.normal = n;
	water.rim = smoothstep(0.55, 1.0, dist) * m;
}

// Rivulets: a drop that has grown heavy runs down the lens, meandering, with a wet trail that tapers and
// breaks into beads behind it. One possible rivulet per column; each has its own speed, start and length.
void AddRivulets(inout LensWater water, float2 q, float aspect, float time, float amount)
{
	const float columns = 16.0;
	const float col = floor(q.x * columns);
	const float h0 = Hash21(float2(col, 0.0), 601u);
	const float h1 = Hash21(float2(col, 0.0), 602u);
	const float h2 = Hash21(float2(col, 0.0), 603u);
	const float h3 = Hash21(float2(col, 0.0), 604u);
	if (h0 >= amount * 0.6)
		return;

	// Head position in screen heights: enters above the top, leaves below the bottom, then rests off screen.
	const float speed = 0.05 + 0.11 * h1;
	const float cycle = 1.6 / speed;
	const float t = frac(time / cycle + h2);
	float headY = t * 1.6 - 0.25;
	headY -= 0.03 * smoothstep(0.0, 1.0, frac(t * 6.0 + h3));  // stop-and-go: the water catches, then lets go

	// Meandering path, kept inside the column so neighbours never cut it off.
	const float wander = sin(q.y * (5.0 + 6.0 * h3) + h1 * 6.2832) * 0.22 + (ValueNoise(float2(col * 3.1, q.y * 9.0), 605u) - 0.5) * 0.25;
	const float dx = (frac(q.x * columns) - (0.5 + wander)) / columns;

	const float headRadius = (0.0035 + 0.0035 * h3) * 1.15;  // screen heights (nicktheduke: 15% larger)
	const float above = headY - q.y;
	const float trailLength = 0.18 + 0.4 * h2;
	const float trail = above > 0.0 ? saturate(1.0 - above / trailLength) : 0.0;
	const float w = headRadius * (0.22 + 0.4 * pow(trail, 1.2));
	// The older half of the trail has pulled apart into beads.
	const float beads = above > trailLength * 0.3 ? smoothstep(0.35, 0.6, ValueNoise(float2(col * 7.7, above * 90.0), 607u)) : 1.0;
	const float trailMask = (1.0 - smoothstep(0.55, 1.0, abs(dx) / w)) * saturate(trail * 4.0) * beads * 0.9;

	const float2 hd = float2(dx, (q.y - headY) * 0.8 + headRadius * 0.2) / headRadius;
	const float headMask = 1.0 - smoothstep(0.7, 1.0, length(hd));

	const float m = max(trailMask, headMask);
	if (m <= water.mask)
		return;

	// Cross-section normal: a rounded bead at the head, a half cylinder along the trail.
	const bool head = headMask >= trailMask;
	const float2 n = head ? hd : float2(dx / w, 0.0);
	const float r = head ? headRadius : w;
	const float2 lensOffset = -n * r * 4.0;
	water.mask = m;
	water.offset = float2(lensOffset.x / aspect, lensOffset.y);
	water.highlight = saturate(pow(saturate(dot(n, normalize(float2(-0.6, -0.8))) * 1.3), 2.5) * 2.0) * m;
	water.rim = saturate(length(n) * 1.25 - 0.35) * m * 0.8;
	water.normal = n;
}

// Lens dirt with natural variation: the texture when installed (varied sizes, strengths and focus),
// a soft noise fallback otherwise. a_offset/a_scale let the same texture serve ash, grit and condensation.
float DirtAt(float2 a_uv, float a_scale, float2 a_offset)
{
	float dirt;
	[branch] if (WaterTiming.w > 0.5)
		dirt = LensDirt.SampleLevel(WrapSampler, a_uv * a_scale + a_offset, 0);
	else
		dirt = smoothstep(0.55, 0.9, Fbm(a_uv * float2(16.0, 9.0) * a_scale + a_offset * 17.0, 901u)) * 0.6;
	return dirt;
}

// nicktheduke 2026-11f: the frost's shape is a rounded rectangle (superellipse) whose roundness follows the frost. Light frost
// hugs the cold frame, nearly rectangular (subtle, frosty); as it thickens the corners (cold on two sides) fill first
// and the clear area rounds off, so it never closes into a box. Frost Shape biases it: 0 = hugs the frame, 1 = round.
// n is the superellipse exponent: 2 = oval, large = rectangle.
float FrostShapeN(float amount, float shape)
{
	const float s = saturate(shape);
	const float nLight = lerp(9.0, 3.0, s);
	const float nHeavy = lerp(3.2, 2.0, s);
	return lerp(nLight, nHeavy, smoothstep(0.25, 0.9, amount));
}

// Superellipse radius: 1 at the middle of each screen edge, up to 2^(1/n) in the corners.
float FrostShapeR(float2 uv, float n)
{
	const float2 x = abs(uv - 0.5) * 2.0;
	return pow(pow(x.x, n) + pow(x.y, n), 1.0 / n);
}

// When the ice reaches each point (0 = first, 1 = last): the shape, with the baked map's own irregular edge kept on top
float FrostArrival(float2 uv, float n)
{
	const float g = FrostMask.SampleLevel(SceneSampler, uv, 0).g;
	const float shaped = saturate(1.0 - FrostShapeR(uv, n) / pow(2.0, 1.0 / n));
	return lerp(g, shaped, 0.6);
}

// nicktheduke glaze ice: growth helpers shared by the frost pass (see the frost block in mainPS).
float GlazeVis(float arrival, float a)
{
	float v = saturate((a - arrival) / 0.06 + 1.0);
	return v * v * (3.0 - 2.0 * v);
}

float GlazeIce(float arrival)
{
	return pow(saturate(1.0 - arrival / 0.72), 1.6);
}

// How much the ice bends the view at this point: its thickness now, plus a hint while it is still thin.
float GlazeBend(float arrival, float a, float amount)
{
	const float vis = GlazeVis(arrival, a);
	const float age = saturate((a - arrival) / 0.55);
	const float cold = saturate((amount - 0.35) / 0.65);
	const float ice = GlazeIce(arrival);
	return ice * vis * ((0.12 + 0.88 * pow(age, 1.2)) * (0.25 + 0.75 * cold) + 0.12);
}

float4 mainPS(float4 position : SV_Position) : SV_Target0
{
	float2 uv = position.xy * BufferDim.xy;
	float time = BufferDim.z;
	float aspect = BufferDim.y / BufferDim.x;

	float range = 1.0;
	if (BufferDim.w > 0.5 && HDRData.x > 0.5 && HDRData.y > 0.0)
		range = max(1.0, HDRData.z / HDRData.y);
	if (!isfinite(range))
		range = 1.0;

	float4 source = Scene.SampleLevel(SceneSampler, uv, 0);

	// ---- Lens water ----
	LensWater water;
	water.offset = 0.0;
	water.mask = 0.0;
	water.highlight = 0.0;
	water.rim = 0.0;
	water.normal = 0.0;
	float2 q = float2(uv.x * aspect, uv.y);
	float rainAmount = Water.x;
	// nicktheduke: after surfacing, the draining sheet leaves drops and rivulets running down the glass as well.
	const float surfacing = Water.y;
	// nicktheduke: melting frost near heat runs off as drips, around the frosted edge only.
	const float thawDrips = Thermal.x;
	const float dropAmount = max(max(rainAmount, surfacing * 0.8), thawDrips * 0.5);
	[branch] if (dropAmount > 0.001)
	{
		float size = Water.z * 1.15;  // nicktheduke: drops 15% larger
		float lifetime = WaterTiming.x;
		AddDropLayer(water, q, aspect, 0.085 * size, 101u, saturate(dropAmount * 0.32), 0.35, time, lifetime);
		AddDropLayer(water, q + 0.37, aspect, 0.05 * size, 211u, saturate(dropAmount * 0.45), 0.15, time * 1.13, lifetime * 0.8);
		AddDropLayer(water, q + 0.71, aspect, 0.026 * size, 307u, saturate(dropAmount * 0.55), 0.0, time * 0.91, lifetime * 0.6);
		// Running water: rain gives occasional rivulets, surfacing sends most columns running at once.
		AddRivulets(water, q, aspect, time, saturate(max(max(rainAmount, surfacing * 1.6), thawDrips * 0.8)));
		[branch] if (rainAmount <= 0.001 && surfacing <= 0.001)
		{
			const float thawEdge = smoothstep(0.22, 0.5, length(uv - 0.5));
			water.mask *= thawEdge;
			water.highlight *= thawEdge;
			water.rim *= thawEdge;
		}
	}

	float sheetAmount = Water.y;
	float sheetMask = 0.0;
	float2 sheetOffset = 0.0;
	[branch] if (sheetAmount > 0.001)
	{
		float flow = ValueNoise(uv * float2(7.0, 3.5) + float2(0.0, time * 0.8), 503u);
		float top = (1.0 - sheetAmount) * 1.15 - 0.1;
		sheetMask = smoothstep(top - 0.05, top + 0.15, uv.y + (flow - 0.5) * 0.25);
		float2 film = float2(ValueNoise(uv * float2(9.0, 4.0) + float2(0.0, time * 0.9), 509u),
						  ValueNoise(uv * float2(9.0, 4.0) + float2(13.7, time * 0.9), 521u)) -
		              0.5;
		// Fine running lines in the sheet (thin and soft, not the old blocky bars).
		float streak = smoothstep(0.55, 0.9, Fbm(float2(uv.x * 160.0, uv.y * 1.5 - time * 0.9), 523u));
		sheetOffset = (film * 0.014 + float2(streak * 0.0015, streak * 0.003)) * sheetMask * sheetAmount;
	}

	// ---- nicktheduke: heat haze, rising shimmer, strongest low on the screen where the hot air is ----
	float2 hazeOffset = 0.0;
	[branch] if (Atmos.x > 0.001)
	{
		const float2 hp = float2(q.x * 9.0, uv.y * 7.0 + time * 2.2);
		const float2 wobble = float2(ValueNoise(hp, 801u), ValueNoise(hp * 1.9 + 5.3, 809u)) - 0.5;
		const float fine = ValueNoise(float2(q.x * 26.0, uv.y * 20.0 + time * 4.0), 811u) - 0.5;
		const float low = 0.2 + 0.8 * smoothstep(0.1, 0.95, uv.y);
		// nicktheduke: capped so a strong setting or a forge up close bends the view without swimming the whole screen.
		hazeOffset = (wobble * 0.0045 + float2(0.0, fine * 0.002)) * min(Atmos.x, 1.4) * low;
	}

	// ---- nicktheduke: heat shimmer above lava and big fires (in the world, so only where the scene is behind the hot air) ----
	float2 shimmerOffset = 0.0;
	[branch] if (ThermalLook.y > 0.5)
	{
		const float pixelDepth = ThermalLook.w > 0.5 ? SceneDepth.Load(int3(position.xy, 0)) : 0.0;
		const uint count = min((uint)ThermalLook.y, 8u);
		[loop] for (uint i = 0; i < count; ++i)
		{
			const float4 src = Shimmer[i];
			const float radius = max(src.z, 0.005);
			const float2 rel = float2((uv.x - src.x) * aspect, uv.y - src.y) / radius;
			const float up = -rel.y;  // above the source top
			const float column = exp(-rel.x * rel.x * 1.6) * smoothstep(-0.5, 0.15, up) * (1.0 - smoothstep(0.8, 3.2, up));
			[branch] if (column < 0.01)
				continue;
			const float srcDepth = ShimmerDepth[i >> 2u][i & 3u];
			const float behind = ThermalLook.w > 0.5 ? step(0.0, (pixelDepth - srcDepth) * ThermalLook.z) : 1.0;
			const float2 sp = float2(rel.x * 2.6, rel.y * 2.2 + time * 1.7);
			const float2 wobble = float2(ValueNoise(sp, 951u), ValueNoise(sp * 1.7 + 3.1, 953u)) - 0.5;
			const float fine = ValueNoise(float2(rel.x * 7.0, rel.y * 6.0 + time * 3.2), 957u) - 0.5;
			const float near = saturate(radius * 5.0);  // close sources bend more
			shimmerOffset += (wobble * 0.0045 + float2(0.0, fine * 0.002)) * src.w * column * behind * (0.35 + 0.65 * near);
		}
		shimmerOffset = clamp(shimmerOffset, -0.008, 0.008);
	}

	float waterRefraction = Water.w;
	float2 waterUV = uv + (water.offset * water.mask + sheetOffset) * waterRefraction + hazeOffset + shimmerOffset;
	float waterBlur = sheetMask * sheetAmount * 2.0;
	float3 color = SampleBlurred(waterUV, waterBlur, range);
	[branch] if (Atmos.x > 0.001 && waterBlur <= 0.25)
	{
		// Hot air bends red and blue slightly differently: a faint colour split along the shimmer.
		const float split = 0.55;
		color.r = Scene.SampleLevel(SceneSampler, waterUV + hazeOffset * split, 0).r / range;
		color.b = Scene.SampleLevel(SceneSampler, waterUV - hazeOffset * split, 0).b / range;
		color = saturate(color);
	}
	color *= 1.0 - water.rim * 0.25;
	color += water.highlight * 0.25 * (0.3 + Luminance(color));

	// ---- nicktheduke: ash, dust, embers and fog on the lens ----
	const float edgeR = length(uv - 0.5) / 0.7071;  // 0 centre, 1 corners
	[branch] if (Atmos.y > 0.001)
	{
		// Ash is heavier than dust: it drifts down slowly and sways instead of streaking sideways.
		// Light ashfall barely moves; a full ash storm (strength toward 1+) falls and creeps faster.
		const float ashAmount = saturate(Atmos.y);
		const float storm = saturate((Atmos.y - 0.45) * 1.8);
		const float3 ashColor = float3(0.40, 0.39, 0.37);

		// Settled on the glass: flakes and a grey film gather at the edges and slowly slide down.
		const float slide = time * (0.004 + 0.012 * storm);
		const float2 sway = float2(sin(time * 0.13 + uv.y * 3.0) * 0.008, 0.0);
		const float2 settleUV = uv + sway - float2(0.0, slide);
		const float reachAsh = edgeR + (Fbm(q * 3.0 - float2(0.0, slide * 3.0), 821u) - 0.5) * 0.3;
		const float edge = smoothstep(0.45, 1.05, reachAsh);
		// nicktheduke: smaller flakes (the dirt texture's discs read as bokeh at the old scale), broken up so they are not round.
		const float flakeBreak = smoothstep(0.3, 0.7, ValueNoise(settleUV * float2(140.0 * aspect, 140.0), 823u));
		const float flake = saturate(DirtAt(settleUV, 3.4, float2(0.31, 0.57)) * 1.6 + DirtAt(settleUV, 1.8, float2(0.62, 0.14)) * 0.8) * (0.35 + 0.65 * flakeBreak);
		const float film = smoothstep(0.45, 0.85, Fbm(q * 9.0 - float2(0.0, slide * 9.0), 831u));
		const float ashMask = saturate(edge * (flake * 0.85 + film * 0.5)) * ashAmount;
		color = lerp(color, ashColor * (0.55 + 0.45 * Luminance(color)), ashMask * 0.65);

		// Falling through the air in front of the lens: two depths, the near one larger, faster and softer.
		const float fallNear = time * (0.035 + 0.11 * storm);
		const float fallFar = time * (0.018 + 0.06 * storm);
		const float2 driftNear = float2(sin(time * 0.31) * 0.05 + sin(time * 0.07 + 1.3) * 0.08, -fallNear);
		const float2 driftFar = float2(sin(time * 0.23 + 2.1) * 0.03, -fallFar);
		const float airNear = DirtAt(uv, 2.0, float2(0.13, 0.44) + driftNear * 2.0) * smoothstep(0.25, 0.6, ValueNoise(uv * float2(90.0 * aspect, 90.0) + driftNear * 40.0, 825u));
		const float airFar = DirtAt(uv, 4.2, float2(0.71, 0.29) + driftFar * 4.2);
		const float airborne = saturate(airNear * 1.1 + airFar * 0.7) * ashAmount * (0.35 + 0.4 * storm);
		color = lerp(color, ashColor * (0.7 + 0.3 * Luminance(color)), saturate(airborne * 0.55));

		color = lerp(color, color * 0.75 + ashColor * 0.25, ashAmount * (0.10 + 0.06 * storm));  // grey haze in the air
	}
	[branch] if (Atmos.z > 0.001)
	{
		// Dust: warm haze, gusts streaking sideways across the view, fine grit at the edges.
		const float3 dustColor = float3(0.62, 0.53, 0.42);
		const float gust = smoothstep(0.42, 0.85, Fbm(float2(q.x * 1.4 - time * 0.9, q.y * 12.0 + sin(time * 0.3)), 841u));
		// nicktheduke: the centre of the view stays mostly clear; the gusts thicken toward the edges.
		const float dustCentre = 0.25 + 0.75 * smoothstep(0.2, 0.62, edgeR + (Fbm(q * 1.5 + time * 0.05, 843u) - 0.5) * 0.15);
		color = lerp(color, dustColor * (0.5 + 0.5 * Luminance(color)), saturate(Atmos.z * (0.14 + 0.28 * gust) * dustCentre));
		const float grit = saturate(DirtAt(uv, 2.3, float2(0.47, 0.83)) * 1.5);
		const float edge = smoothstep(0.55, 1.05, edgeR + (Fbm(q * 2.0, 851u) - 0.5) * 0.3);
		color = lerp(color, dustColor * 0.55, saturate(grit * edge * Atmos.z * 0.6));
	}
	[branch] if (AtmosLook.z > 0.001)
	{
		// Embers drifting up and across, flickering out.
		float3 emberLight = 0.0;
		[loop] for (uint layer = 0; layer < 2; ++layer)
		{
			const float scaleE = layer == 0 ? 9.0 : 16.0;
			const float2 drift = float2(sin(time * 0.35 + layer) * 0.4 + time * 0.08, -time * (0.25 + 0.15 * layer));
			const float2 g = q * scaleE + drift * scaleE * 0.5;
			const float2 cell = floor(g);
			const float h = Hash21(cell, 861u + layer * 7u);
			[branch] if (h < 0.05)
			{
				const float2 jitter = float2(Hash21(cell, 863u), Hash21(cell, 867u)) - 0.5;
				const float d = length(frac(g) - 0.5 - jitter * 0.4);
				// nicktheduke 2026-11-03: no strobing. Each ember glows slowly (about 1 to 2 times a second, small swing),
				// fades in and out over its own few-second life, and fades before it reaches the edge of its cell, so
				// embers never pop on or off.
				const float glow = 0.85 + 0.15 * sin(time * (1.2 + 1.5 * h) + h * 40.0);
				const float life = sin(3.14159 * frac(time * (0.18 + 0.1 * h) + h * 7.0));
				const float2 fromCentre = abs(frac(g) - 0.5);
				const float edgeFade = 1.0 - smoothstep(0.32, 0.46, max(fromCentre.x, fromCentre.y));
				emberLight += float3(1.0, 0.42, 0.1) * (1.0 - smoothstep(0.0, 0.09 - 0.03 * layer, d)) * glow * life * life * edgeFade;
			}
		}
		color += emberLight * AtmosLook.z * 1.6;
	}
	[branch] if (Atmos.w > 0.001)
	{
		// Fog: bright lights bloom into wide halos, and fine condensation beads soften the edges of the glass.
		const float3 glow = SampleGlow(uv, range);
		// Mist filter look: highlights bleed into a soft glow and the darks lift slightly toward the glow,
		// so dense fog reads on the lens by day as well as around lights at night.
		const float halo = smoothstep(0.15, 0.8, Luminance(glow));
		color += glow * halo * AtmosLook.x * Atmos.w * 0.9;
		color = lerp(color, max(color, glow * 1.05), saturate(AtmosLook.x * Atmos.w * 0.35));
		// nicktheduke: a mist veil that does not depend on bright lights, so fog shows at night too: the view softens,
		// contrast drops and the darks lift a little toward a cool grey.
		{
			const float3 veilBlur = SampleBlurred(waterUV, 5.0, range);
			const float veilLum = Luminance(veilBlur);
			const float3 mist = veilBlur * 0.85 + (0.025 + veilLum * 0.25) * float3(0.9, 0.94, 1.0);
			color = lerp(color, mist, saturate(Atmos.w * 0.4));
		}
		[branch] if (AtmosLook.y > 0.001)
		{
			const float edge = smoothstep(0.5, 1.05, edgeR + (Fbm(q * 2.5, 871u) - 0.5) * 0.35);
			// Beads of every size from the dirt texture's dust discs, so no two match.
			const float bead = saturate(DirtAt(uv, 3.1, float2(0.19, 0.71)) * 1.8);
			const float cond = saturate(edge * (0.45 + 0.55 * bead) * AtmosLook.y * Atmos.w * 1.3);
			const float3 soft = SampleBlurred(waterUV, 4.0, range);
			color = lerp(color, soft * 1.03 + 0.035, cond * 0.6);
		}
	}

	// ---- nicktheduke 2026-11e: cold breath, first person. Warm wet breath leaves the lips (just above the middle of the bottom
	// quarter of the view), condenses into a visible cloud in front of you, billows out and rises, slowing and thinning
	// as it mixes into the cold air. Three shapes, each leaning either way, so the same puff never shows twice in a row:
	//   0 rolling puff: one round billow that swells and rises straight up
	//   1 drifting plume: rises while the air carries it to one side, trailing back toward the mouth
	//   2 double breath: a quick burst that spreads wide and low, then a smaller wisp that rises after it
	// The cloud scatters light: it lifts a dark scene toward a pale grey and greys a bright one, and every billow is lit
	// on top and shaded underneath, so it has shape on snow and in a dark room alike. No masks, no hard edges. ----
	[branch] if (Thermal.y > 0.001)
	{
		const float age = Thermal.z;
		float envelope = smoothstep(0.0, 0.12, age) * (1.0 - smoothstep(1.1, 3.0, age));
		// "Force Cold Breath" sends strength exactly 1 and a real-time clock; keep a floor so a paused frame still shows it
		if (ThermalLook.x == 1.0)
			envelope = max(envelope, 0.35);
		[branch] if (envelope > 0.001)
		{
			const float strength = ThermalLook.x;
			const uint id = (uint)clamp(AtmosLook.w, 0.0, 5.0);
			const uint shape = id % 3u;
			const float side = id >= 3u ? -1.0 : 1.0;
			const float g = 1.0 - exp(-age * 1.5);  // quick out of the mouth, slowing as it mixes
			const float2 lips = float2(0.5 * aspect, 0.82 - (FrostBreath.w - 0.12) * 0.5);
			float2 centre = lips;
			float radius = 0.045 + 0.17 * g;
			float2 stretch = float2(1.35, 1.0);
			float rise = 0.17 + 0.05 * strength;
			if (shape == 1u) { centre.x += side * 0.10 * g * g; rise *= 0.85; stretch = float2(1.2, 1.1); }
			if (shape == 2u) { rise *= 0.55; radius *= 1.15; stretch = float2(1.7, 0.85); }
			centre.y -= rise * g;
			centre.x += sin(time * 0.6 + age * 1.3) * 0.008;
			const float2 p = (q - centre) / radius / stretch;
			const float salt = float(shape) * 7.3 + side * 3.1;
			// billows: domain-warped noise carried up and rolling outward as the cloud grows
			const float2 warp = float2(Fbm(p * 1.2 + float2(age * 0.35 + salt, 3.1), 961u), Fbm(p * 1.2 + float2(5.7, -age * 0.35 + salt), 963u)) - 0.5;
			const float billow = Fbm(p * 2.0 + warp * 1.4 + float2(salt, age * 0.5), 965u);
			const float fine = Fbm(p * 5.0 + warp * 0.8 + float2(salt, age * 0.9), 967u);
			float body = exp(-dot(p, p) * 1.6);
			if (shape == 1u)  // the plume trails back toward the mouth as it drifts
			{
				const float2 tail = (p - float2(-side * 0.7, 0.55)) * float2(1.4, 0.9);
				body = max(body, exp(-dot(tail, tail) * 2.2) * 0.65);
			}
			if (shape == 2u)  // the second, smaller breath rises from the mouth after the first has spread
			{
				const float a2 = max(age - 0.45, 0.0);
				const float g2 = 1.0 - exp(-a2 * 1.6);
				const float2 c2 = lips + float2(side * 0.03, -0.20 * g2);
				const float2 p2 = (q - c2) / (0.03 + 0.10 * g2);
				body = max(body, exp(-dot(p2, p2) * 1.8) * smoothstep(0.0, 0.25, a2) * 0.8);
			}
			float den = saturate(body * (0.55 + 1.3 * (billow - 0.4)) + body * (fine - 0.5) * 0.4);
			den = smoothstep(0.05, 0.45, den);                         // soft, rounded billow edges
			den *= 1.0 - 0.7 * smoothstep(0.4, 2.6, age);              // thins as it mixes into the cold air
			den *= 1.0 - 0.85 * smoothstep(0.35, 0.62, 1.0 - uv.y);    // gone before it reaches the middle of the view
			const float alpha = saturate(den * envelope * Thermal.y * (0.65 + 0.35 * strength) * 0.75);
			[branch] if (alpha > 0.002)
			{
				const float3 behind = SampleBlurred(waterUV + warp * 0.004 * alpha, 4.0 + 10.0 * alpha, range);
				const float bgLum = Luminance(behind);
				const float lit = saturate(0.55 - p.y * 0.35 + (billow - 0.5) * 1.2);  // tops lit, undersides shaded
				const float tone = lerp(bgLum, 0.78, 0.55) * (0.80 + 0.40 * lit);
				color = lerp(color, lerp(behind, tone * float3(0.95, 0.97, 1.0), 0.85), alpha);
			}
		}
	}

	// ---- nicktheduke: a cold lens fogs up in a warm room, then clears from the centre out ----
	[branch] if (Thermal.w > 0.001)
	{
		const float fogAmount = Thermal.w;
		const float clearRadius = (1.0 - fogAmount) * 0.8;
		const float edgeNoise = (Fbm(q * 3.0, 971u) - 0.5) * 0.25;
		const float mask = smoothstep(clearRadius, clearRadius + 0.22, edgeR * 0.7071 + edgeNoise) * saturate(fogAmount * 1.5);
		[branch] if (mask > 0.001)
		{
			const float beads = saturate(DirtAt(uv, 3.1, float2(0.37, 0.11)) * 1.6);
			const float3 soft = SampleFrosted(waterUV, 14.0 * mask, range);
			color = lerp(color, soft * 1.04 + 0.03, mask * 0.75);
			color = lerp(color, soft * 0.9 + 0.06, beads * mask * 0.3);
		}
	}

	// ---- nicktheduke: low sun shining into the lens ----
	// Strength is already 0 unless the sky is clear and the sun low and visible (set on the CPU side).
	const float sunStrength = SunLens.z;
	// nicktheduke: the weather's own sunlight colour (set on the CPU side), so night glare is not gold.
	const float3 sunColor = (SunLook.y + SunLook.z + SunLook.w) > 0.01 ? SunLook.yzw : float3(1.0, 0.95, 0.88);
	float sunNear = 0.0;
	float2 toSun = float2(0.0, -1.0);
	[branch] if (sunStrength > 0.001)
	{
		toSun = float2((SunLens.x - uv.x) * aspect, SunLens.y - uv.y);
		const float sunDistance = length(toSun);
		// Light floods the glass around the sun and spills in from the edge when the sun is just off screen.
		sunNear = exp(-sunDistance * sunDistance * 2.5) * 0.85 + exp(-sunDistance * 1.2) * 0.15;

		// Lens dirt: soft smudges, fine dust specks and a few big out-of-focus blobs, only visible in the glare.
		[branch] if (SunLens.w > 0.001)
		{
			// Lens dirt is bloom seen through a dirt mask: each speck and smudge glows by the light behind it
			// (sun glare, bright sky, a torch), so strength varies naturally across the glass.
			const float dirt = DirtAt(uv, 1.0, 0.0);
			const float3 bloom = SampleGlow(uv, range);
			const float3 light = bloom * smoothstep(0.25, 1.0, Luminance(bloom)) + sunColor * sunNear * 0.55;
			const float3 veil = saturate(dirt * light * SunLens.w * sunStrength * 1.6);
			color = 1.0 - (1.0 - saturate(color)) * (1.0 - veil);
		}

		// Water on the lens catches the sun along the edge that faces it.
		[branch] if (water.mask > 0.001 && SunLook.x > 0.001)
		{
			const float facing = saturate(dot(water.normal, normalize(toSun + 1e-5)));
			color += sunColor * (pow(facing, 3.0) * 1.2 + water.rim * 0.6) * water.mask * sunNear * sunStrength * SunLook.x * 0.6;
		}
	}

	// ---- nicktheduke 2026-11e: a full (or nearly full) moon on a clear night. Much gentler than the sun: a faint silver
	// aureole, a thin diffraction ring, lens dirt catching its light near it, and a small ghost across the centre. ----
	[branch] if (MoonLens.z > 0.001)
	{
		const float3 moonColor = float3(0.78, 0.84, 1.0);
		const float r = max(MoonLens.w, 0.008);
		const float dm = length(float2((MoonLens.x - uv.x) * aspect, MoonLens.y - uv.y));
		const float aureole = exp(-dm * dm / (r * r * 9.0)) * 0.20 + exp(-dm / (r * 6.0)) * 0.05;
		const float ringX = (dm - r * 4.2) / (r * 0.9);
		const float ring = exp(-ringX * ringX) * 0.03;
		const float2 toGhost = float2((1.0 - MoonLens.x - uv.x) * aspect, 1.0 - MoonLens.y - uv.y);
		const float gl = length(toGhost);
		const float ghostX = (gl - r * 1.4) / (r * 0.3);
		const float ghost = exp(-gl * gl / (r * r * 2.5)) * 0.025 + exp(-ghostX * ghostX) * 0.012;
		float3 moonAdd = moonColor * (aureole + ring + ghost);
		[branch] if (SunLens.w > 0.001)
		{
			const float moonNear = exp(-dm * dm * 6.0);
			moonAdd += moonColor * DirtAt(uv, 1.0, 0.0) * moonNear * SunLens.w * 0.35;
		}
		color += moonAdd * MoonLens.z;
	}

	// ---- Lens frost: glaze ice frozen onto the lens (nicktheduke v9) ----
	// FrostMask_nicktheduke.dds is one screen-shaped RGBA map baked by tools/nicktheduke_frost_glaze_bake.py:
	//   R = fern frost relief near the frame, G = when the ice reaches each point (0 = frame, 1 = centre),
	//   B = bubbles in the ice (0.5 = none, below = dark rim, above = bright ring), A = snow grains + bottom crust.
	// Cold drives everything: a frosted edge and faint blue glaze first; the glaze deepens, the edge thickens and
	// the ice starts to bend the view; fern cracks creep in from the frame after 50%; bubbles, snow dust,
	// glints and the bottom crust fill in toward 100%. Lengths below are in reference pixels (1152 = screen height).
	float frostAmount = Frost.x;
	[branch] if (frostAmount > 0.001 && WaterTiming.y > 0.5)
	{
		const float coverage = Frost.y;
		const float opacity = Frost.w;
		const float amount = saturate(frostAmount);
		const float refPx = 1152.0;
		const float2 p = float2(uv.x * aspect, uv.y) * refPx;  // aspect = width / height   // reference pixels
		const float2 toUV = float2(1.0 / aspect, 1.0) / refPx;          // reference pixels -> uv
		const float screenPx = 1.0 / (BufferDim.y * refPx);       // screen pixels per reference pixel

		const float4 tex = FrostMask.SampleLevel(SceneSampler, uv, 0);
		const float covScale = 0.8 / clamp(coverage, 0.2, 1.0);  // default coverage 0.8 = as previewed
		const float shapeN = FrostShapeN(amount, FrostShape.x);
		const float arrivalBase = lerp(tex.g, saturate(1.0 - FrostShapeR(uv, shapeN) / pow(2.0, 1.0 / shapeN)), 0.6);
		const float arrival = arrivalBase * covScale;
		const float a = pow(amount, 1.7);  // the front creeps: slow along the frame, faster as it deepens
		const float vis = GlazeVis(arrival, a);
		// nicktheduke 2026-11-05: how much of the frost pass replaces the image at this pixel. The ice used to replace it
		// outright wherever the frost pass ran, which cut a hard line through the sun glare, lens dirt, rain and haze
		// layers at the front of new frost (and erased them everywhere once frost passed 20%). Now the ice fades in
		// over a wide, irregular band at its front and leaves the layers under it alone where there is no ice yet.
		const float frontJitter = (Fbm(float2(uv.x * aspect, uv.y) * 6.0, 1361u) - 0.5) * 0.08;
		float cover = saturate((a - arrival + frontJitter) / 0.16 + 1.0);
		cover = cover * cover * (3.0 - 2.0 * cover);
		const float age = saturate((a - arrival) / 0.55);
		const float cold = saturate((amount - 0.35) / 0.65);
		const float iceNoise = 0.9 + 0.2 * ValueNoise(p / 90.0, 1201u);
		const float ice = GlazeIce(arrival) * iceNoise;
		const float t = ice * vis * (0.12 + 0.88 * pow(age, 1.2)) * (0.25 + 0.75 * cold);  // thickness now
		const float tb = GlazeBend(arrival, a, amount) * iceNoise;                         // + a hint of bend early

		// Fern frost: appears just after 50% at the frame and creeps inward.
		const float fernArrival = arrivalBase * 0.70 + (ValueNoise(p / 120.0, 1297u) - 0.5) * 0.12;  // fern growth time
		const float fernT = 0.56 + 0.44 * saturate(fernArrival / 0.40);
		float rv = saturate((amount - fernT) / 0.05 + 1.0);
		rv = rv * rv * (3.0 - 2.0 * rv);
		const float fcold = saturate((amount - 0.5) / 0.5);
		const float relW = 0.65 * 0.95 * rv * (0.6 + 0.4 * fcold);
		const float rel = tex.r * relW;

		[branch] if (cover > 0.001 || vis > 0.001 || rel > 0.001 || amount > 0.2)
		{
			// ---- refraction: the ice is a thick, uneven lens ----
			float2 off = 0.0;  // reference pixels
			{
				// thickness gradient (the lens), measured over a wide step so it stays smooth
				const float e = 24.0;
				const float2 ex = float2(e, 0.0) * toUV, ey = float2(0.0, e) * toUV;
				const float tR = GlazeBend(FrostArrival(uv + ex, shapeN) * covScale, a, amount) * iceNoise;
				const float tL = GlazeBend(FrostArrival(uv - ex, shapeN) * covScale, a, amount) * iceNoise;
				const float tD = GlazeBend(FrostArrival(uv + ey, shapeN) * covScale, a, amount) * iceNoise;
				const float tU = GlazeBend(FrostArrival(uv - ey, shapeN) * covScale, a, amount) * iceNoise;
				off += float2(tR - tL, tD - tU) / (2.0 * e) * 6000.0;
			}
			// broad undulation of the ice body
			const float2 und = float2(Fbm(p / 260.0, 1211u), Fbm(p / 260.0 + 7.3, 1223u)) - 0.5;
			off += und * 130.0 * tb;
			// frozen ripples in the ice surface: large, irregular waves (multi-octave, domain-warped noise, so nothing
			// tiles or repeats), kept gentle so the view swims but never folds over. They only come in at the very end (about 90-100% cold).
			{
				const float rippleIn = smoothstep(0.88, 0.99, amount);  // nicktheduke: only at the very end, as if frozen solid
				[branch] if (rippleIn > 0.001)
				{
					const float2 warp = float2(Fbm(p / 520.0, 1263u), Fbm(p / 520.0 + 11.7, 1267u)) - 0.5;
					const float2 rp = p / 150.0 + warp * 2.5;
					const float2 rip = float2(Fbm(rp, 1259u), Fbm(rp + 5.3, 1277u)) - 0.5;
					off += rip * 22.0 * tb * rippleIn;
				}
			}
			// fern relief and bubbles are tiny lenses
			{
				const float2 ex = float2(1.0, 0.0) * toUV, ey = float2(0.0, 1.0) * toUV;
				const float4 tR = FrostMask.SampleLevel(SceneSampler, uv + ex, 0);
				const float4 tL = FrostMask.SampleLevel(SceneSampler, uv - ex, 0);
				const float4 tD = FrostMask.SampleLevel(SceneSampler, uv + ey, 0);
				const float4 tU = FrostMask.SampleLevel(SceneSampler, uv - ey, 0);
				const float2 relGrad = float2(tR.r - tL.r, tD.r - tU.r) * 0.5 * relW * 3.6;
				off += relGrad * 2.6;
				const float ageB0 = vis * saturate(age * 1.6) * saturate(0.35 + cold * 1.6);
				off += float2(tR.b - tL.b, tD.b - tU.b) * 0.5 * 10.0 * ageB0;
			}
			// Refraction slider: 0 = the ice does not bend the view at all, 1 = as designed. Capped and clamped so no
			// setting can throw the view around (or feed NaNs into the samplers).
			off *= clamp(FrostLook.x, 0.0, 1.5);
			const float offLen = length(off);
			off = (offLen > 28.0) ? off * (28.0 / offLen) : off;
			off = all(isfinite(off)) ? off : 0.0;

			// ---- white frosted edge: a hint at 35%, wider and denser by 100%, irregular inner edge ----
			// nicktheduke 2026-11f: the rim follows the same shape: along the frame for light frost, rounding off as it thickens
			const float dEdge = max(0.0, (1.0 + 0.1 * saturate((9.0 - shapeN) / 7.0) - FrostShapeR(uv, shapeN)) * 0.5);
			const float rimn = saturate((Fbm(p / 210.0, 1283u) - 0.25) * 1.6);
			const float phase = 6.2831853 * FrostBreath.x * time;
			const float breathe = 1.0 + 0.12 * FrostBreath.y * (sin(phase) + 0.35 * sin(phase * 1.618 + 1.3)) / 1.35;
			const float rimWidth = (0.034 + 0.075 * cold) * (0.55 + 0.9 * rimn) * saturate((amount - 0.2) / 0.12) * breathe;
			// nicktheduke 2026-11-05: fade the rim in with the frost. Before 20% the width was 0, and 1 - 0/0 made the corners
			// beyond the frost shape jump to full white rim: a hard line at the very start of frost.
			float rim = pow(saturate(1.0 - dEdge / max(rimWidth, 1e-4)), 1.5) * smoothstep(0.2, 0.32, amount);
			rim *= 0.7 + 0.5 * smoothstep(0.55, 0.85, ValueNoise(p / 1.6, 1289u));
			rim = saturate(rim);

			const float ageB = vis * saturate(age * 1.6) * saturate(0.35 + cold * 1.6);  // bubbles/clouds: full by ~65%
			const float cloud = saturate(Fbm(p / 150.0, 1291u) * 2.6 - 1.5) * ice;
			const float relOpacityBlur = 1.0 - exp(-rel * 1.8);
			const float sigmaRef = t * 2.5 + cloud * ageB * 4.0 + rim * 8.0 + relOpacityBlur * 6.0 + FrostLook.y * 4.0 * vis;
			const float2 seenUV = waterUV + off * toUV;
			const float radius = sigmaRef * screenPx / 0.6;
			float3 seen = radius > 3.0 ? SampleFrosted(seenUV, radius, range) : SampleBlurred(seenUV, radius, range);

			// ---- light the ice from the scene: wide ambient, never fully black ----
			float3 amb = 0.0;
			[loop] for (uint k = 0; k < 8; ++k)
			{
				const float2 d = kDisk[k] * float2(1.0 / aspect, 1.0);
				amb += Scene.SampleLevel(SceneSampler, uv + d * 0.09, 0).rgb;
				amb += Scene.SampleLevel(SceneSampler, 0.5 + d * 0.32, 0).rgb;
			}
			amb = saturate(amb / 16.0 / range);

			// work in linear light like the preview
			float3 o = pow(saturate(seen), 2.2);
			float3 lamb = max(pow(amb, 2.2), 0.004);
			const float3 tintIce = lerp(1.0, FrostTint.rgb, 0.5);

			// ---- blue glaze: faint early, deep at 100%, fading out toward the clear middle ----
			float soft = saturate((a - arrival) / 0.30);
			soft = soft * soft * (3.0 - 2.0 * soft);
			const float blue = ice * soft * (0.45 + 0.55 * cold) * (0.5 + 0.5 * age);
			o *= exp(-blue * float3(0.6, 0.2, 0.08) * 1.2);
			o = o * (1.0 - t * 0.10) + lamb * float3(0.85, 0.95, 1.0) * (t * 0.10);
			const float cl = cloud * ageB * cold;  // milky micro-bubble clouds in the thickest ice
			o = o * (1.0 - cl * 0.25) + lamb * (cl * 0.45);
			[branch] if (FrostLook.z > 0.001)
			{
				const float3 glow = pow(SampleGlow(uv, range), 2.2);
				o += glow * FrostLook.z * 0.12 * t;
			}

			// ---- fern frost with relief: scattering, raking light (lit ridge, shadowed flank), specular ----
			[branch] if (rel > 0.002)
			{
				const float2 ex = float2(1.0, 0.0) * toUV, ey = float2(0.0, 1.0) * toUV;
				const float rx2 = (FrostMask.SampleLevel(SceneSampler, uv + ex, 0).r - FrostMask.SampleLevel(SceneSampler, uv - ex, 0).r) * 0.5 * relW * 3.6;
				const float ry2 = (FrostMask.SampleLevel(SceneSampler, uv + ey, 0).r - FrostMask.SampleLevel(SceneSampler, uv - ey, 0).r) * 0.5 * relW * 3.6;
				const float opac = 1.0 - exp(-rel * 1.5);
				cover = max(cover, opac);
				const float3 Lv = normalize(float3(-0.55, -0.62, 0.56));
				float3 n = float3(-rx2 * 5.0, -ry2 * 5.0, 1.0);
				n = normalize(n);
				const float ndl = dot(n, Lv);
				const float shd = clamp(0.95 + 0.55 * FrostBreath.z * (ndl - Lv.z), 0.7, 1.5);
				const float3 Hv = normalize(Lv + float3(0.0, 0.0, 1.0));
				const float spec = pow(saturate(dot(n, Hv)), 40.0) * saturate(rel * 2.0);
				const float3 lit = lamb * 1.1 * float3(0.93, 0.97, 1.0) * tintIce;
				o = o * (1.0 - opac) + lit * shd * opac + spec * lit * 2.2;
				// facets on the fronds catch the light: tiny points, the same size at any cold
				const float2 gc0 = floor(p / 4.0);
				float fg = 0.0;
				[loop] for (int gj = -1; gj <= 1; ++gj)
				[loop] for (int gi2 = -1; gi2 <= 1; ++gi2)
				{
					const float2 gc = gc0 + float2(gi2, gj);
					const float gh = Hash21(gc, 1301u);
					[branch] if (gh < 0.05)
					{
						const float2 gp = (gc + 0.25 + 0.5 * float2(Hash21(gc, 1303u), Hash21(gc, 1307u))) * 4.0;
						const float relAt = FrostMask.SampleLevel(SceneSampler, gp * toUV, 0).r * relW;
						const float gd2 = dot(p - gp, p - gp);
						const float twinkle = 0.65 + 0.35 * sin(time * (0.8 + 2.0 * Hash21(gc, 1319u)) + gh * 120.0);
						fg += (exp(-gd2 / 0.72) + 0.12 * exp(-gd2 / 12.5)) * (0.8 + 2.2 * Hash21(gc, 1321u)) * twinkle * step(0.15, relAt);
					}
				}
				o += fg * dot(lamb, 0.3333) * 1.0 * max(FrostLook.w, 0.3);
			}

			// ---- white frosted edge ----
			{
				const float ro = rim * (0.62 + 0.3 * cold);
				cover = max(cover, ro);
				o = o * (1.0 - ro) + lamb * 1.3 * tintIce * ro;
			}

			// ---- bubbles: dark rim, bright inner ring and a pin highlight (baked at four depths) ----
			{
				const float x = (tex.b - 0.5) * 2.0 * 0.6;  // bubbles toned down
				const float dark = saturate(-x) / 1.4;
				const float bright = saturate(x) * 1.4;
				o = o * (1.0 - dark * ageB) + bright * ageB * lamb * 1.4;
			}

			// ---- snow: grains on the outer surface (same size at any cold) and a packed crust along the bottom ----
			{
				const float sv = vis * saturate((amount - 0.25) / 0.35);
				const float crustZone = smoothstep(0.88, 0.92, uv.y);
				const float crustRamp = saturate((amount - 0.45) / 0.3);
				const float snowAlpha = tex.a * lerp(sv * 0.5, crustRamp, crustZone);  // grains at half strength, crust full
				[branch] if (snowAlpha > 0.002)
				{
					const float2 ex = float2(1.5, 0.0) * toUV, ey = float2(0.0, 1.5) * toUV;
					const float sgx = (FrostMask.SampleLevel(SceneSampler, uv + ex, 0).a - FrostMask.SampleLevel(SceneSampler, uv - ex, 0).a) / 3.0;
					const float sgy = (FrostMask.SampleLevel(SceneSampler, uv + ey, 0).a - FrostMask.SampleLevel(SceneSampler, uv - ey, 0).a) / 3.0;
					const float shade = lerp(1.0, clamp(1.0 + (-sgx * 0.6 - sgy * 0.8) * 25.0 * FrostBreath.z, 0.75, 1.25), crustZone);
					const float3 snowLit = lamb * float3(1.0, 1.0, 1.02) * 1.35 * shade * lerp(1.0, float3(0.97, 0.99, 1.03), crustZone);
					o = lerp(o, snowLit, saturate(snowAlpha));
					cover = max(cover, saturate(snowAlpha));
				}
				// glints: snow grains and crust facets catching the light, a few with a colour fringe
				[branch] if (sv > 0.5)
				{
					const float2 sc0 = floor(p / 5.0);
					float3 sg = 0.0;
					[loop] for (int sj = -1; sj <= 1; ++sj)
					[loop] for (int si = -1; si <= 1; ++si)
					{
						const float2 sc = sc0 + float2(si, sj);
						const float sh = Hash21(sc, 1331u);
						[branch] if (sh < 0.035)
						{
							const float2 sp = (sc + 0.2 + 0.6 * float2(Hash21(sc, 1333u), Hash21(sc, 1337u))) * 5.0;
							const float snowAt = FrostMask.SampleLevel(SceneSampler, sp * toUV, 0).a;
							const float sd2 = dot(p - sp, p - sp);
							const float hue = Hash21(sc, 1339u);
							const float3 col = hue < 0.05 ? float3(1.0, 0.45, 0.35) : (hue < 0.10 ? float3(0.4, 1.0, 0.55) : (hue < 0.15 ? float3(0.45, 0.6, 1.0) : float3(1.0, 1.0, 1.0)));
							const float twinkle = 0.6 + 0.4 * sin(time * (0.7 + 1.8 * Hash21(sc, 1341u)) + sh * 300.0);
							sg += col * (exp(-sd2 / 0.72) + 0.4 * exp(-sd2 / 12.5)) * (0.8 + 2.2 * Hash21(sc, 1343u)) * twinkle * step(0.3, snowAt);
						}
					}
					o += sg * dot(lamb, 0.3333) * 0.8 * max(FrostLook.w, 0.3);
				}
			}

			const float3 frosted = pow(max(o, 0.0), 1.0 / 2.2);
			color = lerp(color, frosted, opacity * cover);
		}
	}

	return float4(saturate(color) * range, source.a);
}
#endif
