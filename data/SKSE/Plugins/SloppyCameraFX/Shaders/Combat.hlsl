// SPDX-License-Identifier: GPL-3.0-or-later
// SLOPPY CAMERA FX - Combat by nicktheduke: combat and action effects on the camera lens. Everything here is procedural (no textures).
// Works on its own or alongside Screen Weather; the CPU side shares one lens budget between the two.
// nicktheduke: square without pow(), which gives NaN (black pixels) for a negative base
float Sq(float x) { return x * x; }

#ifdef VSHADER

float4 mainVS(uint vertexId : SV_VertexID) : SV_Position
{
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
#endif

#ifdef PSHADER
Texture2D<float4> Scene : register(t0);
SamplerState SceneSampler : register(s0);

#define MAX_SPLATS 12

cbuffer ScreenActionParameters : register(b0)
{
	float4 BufferDim;  // 1/w, 1/h, time, float input
	float4 HDRData;
	float4 Global;     // budget scale, safe zone radius (screen heights), photosensitive safe, splat count
	float4 LayersA;    // dragonfire, frost rime, poison, shock
	float4 LayersB;    // stomp dust, steam, poison pulse, cold breath vapor showing (Screen Weather)
	float4 RimeDir;    // screen direction toward the frost source xy, weather frost already on the lens, seconds since the last shock hit
	float4 Shout;      // age (s), strength, centre uv
	float4 Flash;      // hit uv, age (s), strength
	float4 Slash;      // swing direction xy (screen, unit), age (s), strength
	float4 Breath;     // seconds since the exhale began, exhale strength, stamina shimmer amount, shock hit counter + lasting shock (fraction)
	float4 Shake;      // nicktheduke: hit shake: camera offset xy (screen heights), roll (rad), zoom (hides the edges)
	float4 SplatA[MAX_SPLATS];  // centre uv, size (screen heights), progress 0-1
	float4 SplatB[MAX_SPLATS];  // type, variant, seed, rotation
	float4 SplatC[MAX_SPLATS];  // alpha, unused x3
};

// ---- Noise ----
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

float Hash11(float x, uint salt) { return Hash21(float2(floor(x * 997.0), 0.0), salt); }

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
	[loop] for (uint octave = 0; octave < 4; ++octave)
	{
		sum += ValueNoise(p, salt + octave) * amplitude;
		p = p * 2.03 + float2(17.1, 9.7);
		amplitude *= 0.5;
	}
	return sum / 0.9375;
}

float Luminance(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }

float2 Rotate(float2 p, float angle)
{
	float s, c;
	sincos(angle, s, c);
	return float2(p.x * c - p.y * s, p.x * s + p.y * c);
}

static const float2 kDisk[8] = {
	float2(0.707, 0.0), float2(-0.5, 0.5), float2(0.0, -0.9), float2(0.9, 0.55),
	float2(-0.85, -0.35), float2(0.3, 0.95), float2(-0.25, -0.45), float2(0.55, -0.6)
};

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

// ---- Blood and fluid palette ----
// 0 red, 1 vampire dark red, 2 insect ichor, 3 dwemer oil, 4 dry draugr dust, 5 bone dust, 6 spriggan sap,
// 7 flame atronach embers, 8 frost atronach ice, 9 storm atronach sparks, 10 ghost mist
float3 FluidTint(uint variant)
{
	switch (variant) {
	case 1: return float3(0.32, 0.02, 0.03);
	case 2: return float3(0.55, 0.62, 0.12);
	case 3: return float3(0.05, 0.045, 0.04);
	case 4: return float3(0.36, 0.30, 0.24);
	case 5: return float3(0.72, 0.68, 0.60);
	case 6: return float3(0.75, 0.48, 0.10);
	case 7: return float3(1.0, 0.45, 0.10);
	case 8: return float3(0.80, 0.92, 1.0);
	case 9: return float3(0.55, 0.65, 1.0);
	case 10: return float3(0.55, 0.92, 1.0);  // ectoplasm (ghosts, conjured spirits, familiars)
	default: return float3(0.50, 0.025, 0.02);
	}
}

// A splat outline: a warped blob plus satellite droplets. p is local (1 = the splat size).
float SplatShape(float2 p, float seed, out float core)
{
	const float r0 = length(p);
	const float n = Fbm(p * 2.2 + seed * 13.0, 1001u);
	const float boundary = 0.5 + (n - 0.5) * 0.55;
	core = 1.0 - smoothstep(boundary - 0.07, boundary, r0);
	float satellites = 0.0;
	[branch] if (r0 < 1.6)
	{
		const float2 g = p * 3.4 + seed * 7.0;
		const float2 cell = floor(g);
		const float h = Hash21(cell, 1003u);
		[branch] if (h < 0.3)
		{
			const float2 centre = 0.5 + (float2(Hash21(cell, 1005u), Hash21(cell, 1007u)) - 0.5) * 0.5;
			const float radius = 0.08 + 0.12 * Hash21(cell, 1009u);
			satellites = 1.0 - smoothstep(radius * 0.8, radius, length(frac(g) - centre));
			satellites *= 1.0 - smoothstep(1.0, 1.6, r0);
		}
	}
	return max(core, satellites);
}

// nicktheduke 2026-11: blood on the glass as a liquid, not a decal. A height field (how thick the fluid is) drives
// everything: light is absorbed by thickness (thin film = red-tinted view, thick pool = near black), the view
// behind is refracted by the slope, the rim of each pool and droplet darkens like a meniscus and one small
// highlight sits where the surface faces the light. Drips run down the glass under gravity (screen down, not
// the splat's rotation) in stick-slip hops, meandering, thinning behind a swollen bead, and drain away.
float DripHeight(float2 s, float seed, float progress, float speed)
{
	float h = 0.0;
	[loop] for (uint k = 0; k < 3; ++k)
	{
		const float ks = seed * 1.37 + k * 17.31;
		const float delay = 0.04 + 0.22 * Hash11(ks, 1501u);
		const float t = saturate((progress - delay) / (1.0 - delay));
		[branch] if (t <= 0.0 || Hash11(ks, 1503u) > 0.92)
			continue;
		// stick-slip: the drip holds, then lets go and runs, a few times; each hop a different length
		const float hops = t * (3.0 + 2.0 * Hash11(ks, 1505u));
		const float hop = floor(hops);
		const float run = hop + smoothstep(0.35, 1.0, frac(hops));
		const float len = run / (3.0 + 2.0 * Hash11(ks, 1505u)) * speed * (0.6 + 0.6 * Hash11(ks, 1507u));
		// start inside the pool so the drip is fed by it: it leaves the rim through a neck, never floats below it
		const float x0 = (Hash11(ks, 1509u) - 0.5) * 0.44;
		const float y0 = sqrt(max(0.0, 0.075 - x0 * x0)) * (0.35 + 0.25 * Hash11(ks, 1511u));
		const float y = s.y - y0;
		[branch] if (y < -0.05 || y > len + 0.12)
			continue;
		const float wander = (ValueNoise(float2(y * 3.2, ks), 1513u) - 0.5) * 0.14 + (ValueNoise(float2(y * 11.0, ks + 3.0), 1515u) - 0.5) * 0.025;
		const float dx = s.x - x0 - wander;
		const float along = saturate(y / max(len, 0.02));
		// the trail thins toward the bead and pinches where it drained; the bead swells while it hangs
		const float pinch = 0.8 + 0.2 * ValueNoise(float2(y * 4.0, ks + 7.0), 1517u);
		const float neck = 1.0 + 1.6 * (1.0 - smoothstep(0.0, 0.18, y));  // wide where it pulls out of the pool
		const float w = (0.06 * (1.0 - 0.5 * along) + 0.016) * pinch * (1.0 - 0.3 * t) * neck;
		const float trail = saturate(1.0 - (dx * dx) / (w * w)) * smoothstep(-0.02, 0.02, y) * step(y, len) * lerp(0.75, 0.6 - 0.2 * t, smoothstep(0.0, 0.2, y));
		const float rb = 0.05 + 0.025 * (1.0 - frac(hops)) * (1.0 - t);
		const float2 bd = float2(dx / rb, (y - len) / (rb * 1.45));
		const float bead = pow(saturate(1.0 - dot(bd, bd)), 0.5) * 0.85;
		h = max(h, max(trail, bead));
	}
	return h;
}

// Fluid thickness for a splat: an irregular pool with spatter spikes thrown along the hit, satellite droplets
// of mixed size, and drips. p: rotated splat space, s: unrotated splat space (for gravity).
float FluidHeight(float2 p, float2 s, float seed, float progress, float dripSpeed, bool drips)
{
	const float r0 = length(p);
	const float ang = atan2(p.y, p.x);
	const float edgeN = Fbm(p * 2.4 + seed * 13.0, 1521u);
	const float spikes = pow(saturate(ValueNoise(float2(ang * 1.4 + seed, seed * 3.0), 1523u) * 1.3 - 0.25), 3.0) * 0.45 * saturate(0.3 + p.x * 0.9);
	const float boundary = 0.34 + (edgeN - 0.5) * 0.4 + spikes;
	float h = saturate((boundary - r0) / 0.22);
	h = h * h * (3.0 - 2.0 * h) * (0.94 + 0.08 * ValueNoise(p * 1.6 + seed * 5.0, 1525u));
	[branch] if (r0 < 1.7)
	{
		[loop] for (uint layer = 0; layer < 2; ++layer)
		{
			const float sc = layer == 0 ? 4.0 : 9.0;
			const float2 g = p * sc + seed * (7.0 + layer * 5.0);
			const float2 cell = floor(g);
			const float hh = Hash21(cell, 1527u + layer);
			const float chance = (layer == 0 ? 0.12 : 0.05) * smoothstep(0.25, 0.5, r0) * (1.0 - smoothstep(0.7, 1.6, r0));
			[branch] if (hh < chance)
			{
				const float2 c = 0.5 + (float2(Hash21(cell, 1529u + layer), Hash21(cell, 1531u + layer)) - 0.5) * 0.55;
				const float rr = (layer == 0 ? 0.12 + 0.18 * Hash21(cell, 1533u) : 0.14 + 0.14 * Hash21(cell, 1535u));
				const float2 d = (frac(g) - c) / rr;
				h = max(h, pow(saturate(1.0 - dot(d, d)), 0.5) * (layer == 0 ? 0.5 : 0.35));
			}
		}
	}
	[branch] if (drips)
		h = max(h, DripHeight(s, seed, progress, dripSpeed));
	return h;
}

// nicktheduke 2026-11b: blood modelled on how it actually behaves on glass.
// Spatter is directional: the splat's +x axis is the way the blood was travelling. Drops land as ellipses stretched
// along their flight line with a tail thrown forward; most are small, a few are big, plus a fine mist. Blood is thick
// and grips glass, so only the main mass is heavy enough to run: one or two drips pull out of its lower rim through
// a neck, a swollen bead leading, a thin film behind that breaks into tiny beads, and the pool thins as it feeds them.
float BloodDrips(float2 s, float seed, float progress, float speed, out float drain)
{
	float h = 0.0;
	drain = 0.0;
	[loop] for (uint k = 0; k < 2; ++k)
	{
		const float ks = seed * 1.37 + k * 17.31;
		[branch] if (k == 1u && Hash11(ks, 1601u) > 0.55)
			continue;  // a second drip only sometimes
		const float delay = 0.03 + 0.18 * Hash11(ks, 1603u) + 0.12 * k;
		const float t = saturate((progress - delay) / (1.0 - delay));
		[branch] if (t <= 0.0)
			continue;
		const float nh = 3.0 + 2.0 * Hash11(ks, 1605u);
		const float hops = t * nh;
		const float run = floor(hops) + smoothstep(0.4, 1.0, frac(hops));
		const float len = run / nh * speed * (0.7 + 0.5 * Hash11(ks, 1607u));
		drain = max(drain, saturate(len / speed));
		const float x0 = (Hash11(ks, 1609u) - 0.5) * 0.3;
		const float y0 = 0.1 + 0.06 * Hash11(ks, 1611u);  // inside the pool's lower half
		const float y = s.y - y0;
		[branch] if (y < -0.12 || y > len + 0.12)
			continue;
		const float wander = (ValueNoise(float2(y * 2.6, ks), 1613u) - 0.5) * 0.12 + (ValueNoise(float2(y * 9.0, ks + 3.0), 1615u) - 0.5) * 0.02;
		const float dx = s.x - x0 - wander * smoothstep(0.0, 0.25, y);
		// neck: wide and full where it leaves the pool, narrowing into the rivulet
		const float neckT = 1.0 - smoothstep(0.0, 0.22, y);
		const float w = lerp(0.028, 0.11, neckT) * (0.85 + 0.3 * ValueNoise(float2(y * 5.0, ks + 7.0), 1617u));
		const float profile = saturate(1.0 - (dx * dx) / (w * w));
		// the film left behind: thin, and over time it beads up into small droplets along the path
		const float behindBead = smoothstep(0.06, 0.2, len - y);
		const float beadRow = pow(saturate(1.0 - abs(frac(y * 16.0 + ValueNoise(float2(y * 3.0, ks), 1619u) * 2.0) * 2.0 - 1.0) * 1.6), 2.0);
		// nicktheduke 2026-11c: a drip carries about the same thickness all the way down (no dense "tadpole" head); the film
		// it leaves behind thins a little and only gently beads up
		const float film = lerp(0.72, 0.58, smoothstep(0.0, 0.25, y)) * lerp(1.0, 0.65 + 0.35 * beadRow, behindBead * smoothstep(0.4, 0.95, t));
		const float trail = sqrt(profile) * film * step(-0.02, y) * step(y, len);
		// the bead: swollen, slightly longer than wide, biggest while it hangs before a slip
		// the leading edge: only a slightly rounder, slightly fuller end, the same thickness as the drip itself
		const float rb = 0.032 + 0.008 * (1.0 - frac(hops));
		const float2 bd = float2(dx / rb, (y - len) / (rb * 1.25));
		const float bead = sqrt(saturate(1.0 - dot(bd, bd))) * 0.62;
		h = max(h, max(trail, bead));
	}
	return h;
}

float BloodHeight(float2 p, float2 s, float seed, float progress, float dripSpeed)
{
	// main mass: stretched along the direction of travel, scalloped rim, spines thrown forward
	const float2 pe = float2(p.x * 0.78, p.y);
	const float r0 = length(pe);
	const float2 rdir = p / max(length(p), 1e-3);
	const float edgeN = Fbm(p * 2.2 + seed * 13.0, 1621u);
	const float lobes = (ValueNoise(rdir * 3.5 + seed * 5.0, 1625u) - 0.5) * 0.1;
	const float forward = saturate(rdir.x);
	const float spines = pow(saturate(ValueNoise(rdir * 7.0 + seed * 3.0, 1623u) * 1.5 - 0.55), 1.5) * 0.13 * forward;
	const float boundary = 0.3 + (edgeN - 0.5) * 0.26 + lobes + spines;
	const float x = saturate((boundary - r0) / 0.11);
	float drain;
	const float drip = BloodDrips(s, seed, progress, dripSpeed, drain);
	float h = x * (2.0 - x) * (1.0 - 0.3 * drain);  // flat-topped pool, thin at the rim, thinner as it feeds drips
	// spatter: big, medium and mist, flung forward in a cone
	const float rp = length(p);
	[branch] if (rp < 2.4)
	{
		[loop] for (uint layer = 0; layer < 3; ++layer)
		{
			const float sc = layer == 0 ? 4.5 : (layer == 1 ? 8.0 : 12.5);
			const float2 g = p * sc + seed * (7.0 + layer * 5.0);
			const float2 cell = floor(g);
			const float2 co = 0.5 + (float2(Hash21(cell, 1631u + layer), Hash21(cell, 1633u + layer)) - 0.5) * 0.2;
			const float2 c = (cell + co - seed * (7.0 + layer * 5.0)) / sc;  // droplet centre in splat space
			const float rc = length(c);
			const float2 dir = c / max(rc, 1e-3);
			const float cone = smoothstep(0.05, 0.85, dir.x);
			const float reach = layer == 0 ? 1.1 : (layer == 1 ? 1.45 : 1.75);
			const float chance = (layer == 0 ? 0.3 : (layer == 1 ? 0.3 : 0.16)) * cone * smoothstep(0.22, 0.42, rc) * (1.0 - smoothstep(reach * 0.5, reach, rc));
			[branch] if (Hash21(cell, 1635u + layer) < chance)
			{
				const float rr = (layer == 0 ? 0.07 + 0.07 * Hash21(cell, 1637u) : (layer == 1 ? 0.07 + 0.06 * Hash21(cell, 1639u) : 0.08 + 0.05 * Hash21(cell, 1641u)));
				const float el = 1.0 + 1.3 * smoothstep(0.3, 1.6, rc) * (0.5 + 0.5 * Hash21(cell, 1643u));  // shallower hits further out
				const float2 d = frac(g) - co;
				const float a = dot(d, dir);
				const float b2 = dot(d, float2(-dir.y, dir.x));
				const float e2 = (a / (rr * el)) * (a / (rr * el)) + (b2 / rr) * (b2 / rr);
				float drop = sqrt(saturate(1.0 - e2));
				// the tail thrown forward off the leading edge (not on the mist)
				[branch] if (layer < 2u)
				{
					const float ta = (a - rr * el * 0.7) / (rr * (1.0 + el));
					const float tw = rr * 0.45 * (1.0 - saturate(ta));
					const float tail = step(0.0, ta) * saturate(1.0 - ta) * saturate(1.0 - (b2 * b2) / max(tw * tw, 1e-6));
					drop = max(drop, sqrt(tail) * 0.7);
				}
				h = max(h, drop * (layer == 0 ? 0.8 : (layer == 1 ? 0.65 : 0.45)));
			}
		}
	}
	return max(h, drip);
}

// Lightning path: angular offset of a bolt channel at radius r (fractal zig-zag at every scale).
float BoltJag(float r, float seed, float jitter)
{
	float j = (ValueNoise(float2(r * 7.0, seed * 3.1), 1385u) - 0.5) * 0.55
	        + (ValueNoise(float2(r * 18.0, seed * 4.3), 1384u) - 0.5) * 0.24
	        + (ValueNoise(float2(r * 45.0, seed * 5.7), 1387u) - 0.5) * 0.1;
	const float x1 = r * 110.0 + seed * 11.0;
	const float h0 = Hash11(floor(x1) * 0.013 + seed + jitter, 1379u);
	const float h1 = Hash11((floor(x1) + 1.0) * 0.013 + seed + jitter, 1379u);
	j += (lerp(h0, h1, frac(x1)) - 0.5) * 0.045;
	return j * (0.25 + r);
}

// Electric arc path: sideways offset (screen heights) at distance 'along' from the arc's own origin. Fractal
// zig-zag at several scales, anchored at the origin.
float ArcOffset(float along, float seed, float jitter, float amp)
{
	float o = (ValueNoise(float2(along * 5.0, seed * 3.1), 1385u) - 0.5) * 0.14
	        + (ValueNoise(float2(along * 13.0, seed * 4.3), 1384u) - 0.5) * 0.06
	        + (ValueNoise(float2(along * 34.0, seed * 5.7), 1387u) - 0.5) * 0.025;
	const float x1 = along * 90.0 + seed * 11.0;
	const float h0 = Hash11(floor(x1) * 0.013 + seed + jitter, 1379u);
	const float h1 = Hash11((floor(x1) + 1.0) * 0.013 + seed + jitter, 1379u);
	o += (lerp(h0, h1, frac(x1)) - 0.5) * 0.012;
	return o * amp * smoothstep(0.0, 0.08, along);
}

// Bubbles trapped in venom: clear spheres that magnify and flip what is behind them, a bright rim on the lit side,
// a darker band on the far side and a small specular dot. Two in-focus sizes plus a few soft, out-of-focus discs.
// Kept sparse on purpose.
float3 VenomBubbles(float2 p, float seed, float goo, float2 uv, float sizeUV, float range, float3 base)
{
	float3 result = base;
	const float2 light = normalize(float2(-0.6, -0.8));
	[loop] for (uint layer = 0; layer < 3; ++layer)
	{
		const float scale = layer == 0 ? 4.5 : (layer == 1 ? 11.0 : 3.0);
		const float chance = layer == 0 ? 0.16 : (layer == 1 ? 0.2 : 0.08);
		const float2 g = p * scale + seed * (5.0 + layer * 3.0) + float2(0.0, -BufferDim.z * 0.01 * (layer + 1));
		const float2 cell = floor(g);
		const float h = Hash21(cell, 1101u + layer * 13u);
		[branch] if (h < chance)
		{
			const float2 centre = 0.5 + (float2(Hash21(cell, 1103u + layer), Hash21(cell, 1107u + layer)) - 0.5) * 0.4;
			const float radius = (layer == 2 ? 0.32 : 0.16 + 0.16 * Hash21(cell, 1109u + layer));
			const float2 b = (frac(g) - centre) / radius;
			const float d = length(b);
			[branch] if (d < 1.0)
			{
				if (layer == 2) {
					// Out of focus: a soft, slightly brighter disc deeper in the goo.
					result += (1.0 - smoothstep(0.4, 1.0, d)) * 0.06 * goo * (0.4 + Luminance(base));
					continue;
				}
				// Inside the sphere: the goo behind, magnified and flipped.
				const float2 lensUV = uv - b * radius / scale * sizeUV * 0.6;
				const float3 through = saturate(Scene.SampleLevel(SceneSampler, lensUV, 0).rgb / range);
				// nicktheduke: what is seen through the bubble is still seen through the goo (green-tinted), so the bubble never
				// reads darker than the goo around it; the far side only dips slightly, toward the goo colour, not black.
				float3 bubble = lerp(base, through * float3(0.7, 1.0, 0.55) * 0.9 + base * 0.25, 0.55);
				const float facing = dot(b / max(d, 1e-3), light);
				const float rimBand = smoothstep(0.72, 0.93, d) * (1.0 - smoothstep(0.93, 1.0, d));
				bubble += rimBand * saturate(facing) * 0.25;
				bubble = lerp(bubble, base * 0.9, smoothstep(0.8, 1.0, d) * saturate(-facing) * 0.35);
				bubble += (1.0 - smoothstep(0.0, 0.18, length(b - light * 0.45))) * 0.3;
				result = lerp(result, bubble, goo * (layer == 0 ? 0.9 : 0.7));
			}
		}
	}
	return result;
}

float3 Ember(float2 p, float seed, float progress)
{
	float3 light = 0.0;
	[loop] for (uint j = 0; j < 8; ++j)
	{
		const float angle = Hash11(seed + j * 0.137, 1201u) * 6.2832;
		const float reach = progress * (0.45 + 0.65 * Hash11(seed + j * 0.271, 1203u));
		const float2 pos = float2(cos(angle), sin(angle)) * reach + float2(0.0, progress * progress * 0.25);
		light += float3(1.0, 0.45, 0.12) * exp(-dot(p - pos, p - pos) * 500.0);
	}
	return light * (1.0 - progress);
}

float4 mainPS(float4 position : SV_Position) : SV_Target0
{
	const float2 uv = position.xy * BufferDim.xy;
	const float time = BufferDim.z;
	const float aspect = BufferDim.y / BufferDim.x;  // width / height
	const float2 q = float2(uv.x * aspect, uv.y);
	const float2 centred = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
	const float radial = length(centred);              // screen heights from the centre
	const float edgeR = length(uv - 0.5) / 0.7071;     // 0 centre, 1 corners
	const float budget = Global.x;
	const float safe = smoothstep(Global.y, Global.y + 0.08, radial);

	float range = 1.0;
	if (BufferDim.w > 0.5 && HDRData.x > 0.5 && HDRData.y > 0.0)
		range = max(1.0, HDRData.z / HDRData.y);
	if (!isfinite(range))
		range = 1.0;

	const float4 source = Scene.SampleLevel(SceneSampler, uv, 0);

	// ---- Distortion first: hit shake, shout ripple, stamina breath shimmer, dragonfire heat ----
	float2 offset = 0.0;
	// nicktheduke: hit shake moves the whole picture behind the lens (lens effects stay put, the HUD is drawn later).
	[branch] if (Shake.w > 1.000001 || any(abs(Shake.xyz) > 1e-7))
	{
		float sr, cr;
		sincos(Shake.z, sr, cr);
		const float2 shaken = float2(cr * centred.x - sr * centred.y, sr * centred.x + cr * centred.y) / max(Shake.w, 1.0) + Shake.xy;
		offset = float2(shaken.x / aspect + 0.5, shaken.y + 0.5) - uv;
	}
	float breathHaze = 0.0;
	float breathStreak = 0.0;
	[branch] if (Shout.y > 0.001)
	{
		const float2 toCentre = float2((uv.x - Shout.z) * aspect, uv.y - Shout.w);
		const float d = length(toCentre);
		const float ring = Shout.x * 1.3;
		const float width = 0.07;
		const float wave = (1.0 - smoothstep(0.0, width, abs(d - ring))) * sin((d - ring) / width * 3.1416);
		const float fade = 1.0 - smoothstep(0.2, 0.9, Shout.x);
		offset += toCentre / max(d, 1e-3) * wave * 0.012 * Shout.y * fade * float2(1.0 / aspect, 1.0);
	}
	[branch] if (Breath.z > 0.001)
	{
		// nicktheduke 2026-11d: stamina breath (first person only; the plugin gates it). A winded exhale is a gust of warm,
		// wet air pushed out from just below the view: it shoots down toward the bottom of the screen and fans out,
		// fast at first and slowing as it spreads and fades. Warm breath barely condenses, so it is not a cloud
		// (that is the cold breath): it shows as streaky, turbulent air that warps what is behind it, with a faint
		// pale veil along the streaks so the gust has shape on bright and dark scenes alike. nicktheduke 2026-11e: an open-mouthed,
		// exhausted exhale: it leaves a wide mouth and spreads sideways across the lower screen as it falls. No mask.
		const float age = Breath.x;
		const float strength = Breath.z * (0.6 + 0.4 * Breath.y);
		const float2 O = float2(0.5 * aspect, 0.74);
		const float front = 0.32 * (1.0 - exp(-age * 4.5));           // fast, then slowing
		const float a = q.y - O.y;                                     // distance travelled down
		const float halfW = 0.10 + max(a, 0.0) * 2.6 + 0.08 * (1.0 - exp(-age * 3.0));   // a wide mouth, fanning out sideways as it falls
		const float u = (q.x - O.x) / halfW;
		const float cone = smoothstep(-0.02, 0.05, a) * (1.0 - smoothstep(front - 0.04, front + 0.07, a)) * exp(-u * u * 1.1);
		const float head = 0.4 + 0.6 * smoothstep(front - 0.2, front - 0.02, a);   // densest at the leading edge
		const float env = smoothstep(0.0, 0.05, age) * exp(-age * 1.6);
		// streaks of air along the flow, carried down with the gust and breaking into turbulence as they slow
		const float along = (a - front * 0.85) * 8.0;
		// nicktheduke 2026-11h: mirrored across the mouth, so the gust fans out evenly to both sides (a little of the
		// unmirrored noise keeps it from looking stamped)
		const float au = abs(u);
		const float st = lerp(Fbm(float2(au * 2.6, along), 1305u), Fbm(float2(u * 2.6, along), 1305u), 0.1);
		const float curl = lerp(Fbm(float2(au * 5.0 + along * 0.4, along * 1.7 + 4.0), 1307u), Fbm(float2(u * 5.0 + along * 0.4, along * 1.7 + 4.0), 1307u), 0.1);
		const float streaks = smoothstep(0.46, 0.7, st);
		breathHaze = saturate(cone * head * env * strength * (0.35 + 1.3 * streaks) * 1.4);
		breathStreak = streaks;
		offset += float2((curl - 0.5) * sign(u), st - 0.5) * 0.03 * breathHaze;
	}
	[branch] if (LayersA.x > 0.001)
	{
		const float edge = smoothstep(0.4, 1.0, edgeR);
		const float2 hp = float2(q.x * 9.0, uv.y * 7.0 + time * 2.6);
		offset += (float2(ValueNoise(hp, 1311u), ValueNoise(hp * 1.9 + 5.3, 1313u)) - 0.5) * 0.006 * LayersA.x * edge;
	}

	float3 color = saturate(Scene.SampleLevel(SceneSampler, uv + offset, 0).rgb / range);
	// the gust: a faint pale veil along its streaks, slightly lower in contrast than what is behind it, so it has
	// shape against bright snow and against a dark room alike (never a fog)
	[branch] if (breathHaze > 0.01)
	{
		const float3 softB = SampleBlurred(uv + offset, 3.5, range);
		const float lumB = Luminance(softB);
		// pale wisps along the streaks, a slightly darker, softer air between them: definition on any background
		const float3 veil = (lumB * 0.45 + 0.4 + 0.1 * breathStreak) * float3(0.97, 0.98, 1.0);
		const float3 between = softB * (0.9 + 0.1 * lumB);
		color = lerp(color, lerp(between, veil, 0.25 + 0.75 * breathStreak), saturate(breathHaze * 0.62));
	}

	// ---- Shock (nicktheduke 2026-11f): lightning, modelled on high-speed footage of real discharges. Each bolt is an
	// event, not a standing filament:
	//  1. stepped leader: a faint, heavily branched channel feels its way outward in jerks from near the middle
	//     of the view (its start fades in, so there is no visible origin point);
	//  2. return stroke: when it reaches out, the whole connected channel discharges at once, overexposed white
	//     core, blue sheath, a wide blue glow and the view lit for an instant; branches that did not connect stay
	//     dim; the channel has gaps filled with a faint violet haze;
	//  3. re-strikes: one or two more flashes down the same channel tens of milliseconds apart;
	//  4. afterglow: the channel fades through blue-violet while hot sparks fly off it and burn out.
	// The air around a hot channel bends light, so the view behind wobbles along it. The whole effect runs on a
	// 20 fps clock (choppy like real lightning, not stop-motion). Photosensitive safe mode: one stroke, ramped
	// in and out over a few frames, no re-strikes, no flash of the whole view. ----
	[branch] if (LayersA.w > 0.001 && RimeDir.w < 1.3)   // a discharge plays only after a hit
	{
		const float amount = LayersA.w;
		const bool calm = Global.z > 0.5;
		const float tq = floor(time * 20.0) / 20.0;
		const float angle = atan2(centred.y, centred.x);
		const float r = radial;
		float core = 0.0, sheath = 0.0, glow = 0.0, haze = 0.0, sparks = 0.0, flash = 0.0, heat = 0.0;
		float2 bendDir = 0.0;
		// one discharge per hit, never looping: RimeDir.w is the time since the hit (the plugin resets it on each
		// shock hit). Several bolts leave the middle almost together, a few frames apart, each with its own
		// length and duration; the next hit starts a new discharge with a new layout.
		const float hitAge = floor(RimeDir.w * 20.0) / 20.0;        // the 20 fps clock
		const float burstN = floor(Breath.w) * 7.0 + 3.0;           // the plugin's hit counter: a new layout per hit
		[loop] for (uint b = 0; b < 7; ++b)  // nicktheduke: [loop], fxc cannot unroll this one (X3511)
		{
			const float seed = burstN * 7.0 + b;
			const float delay = Hash21(float2(burstN, b), 1366u) * 0.14;   // near-simultaneous, never in lockstep
			const float t = hitAge - delay;                                // seconds since this bolt began
			[branch] if (t < 0.0 || t > 0.9 || Hash21(float2(seed, 1.0), 1376u) > 0.55 + 0.4 * amount)
				continue;
			// each discharge has its own origin scattered around the middle (never the exact centre, never on a
			// ring), its own heading and its own curve; it is drawn in its own coordinates, so nothing radiates
			// from one point
			const float oa = Hash21(float2(seed, 17.0), 1377u) * 6.2832;
			const float orad = 0.04 + 0.16 * sqrt(Hash21(float2(seed, 18.0), 1363u));
			const float2 O = float2(cos(oa), sin(oa)) * orad;
			const float hd = oa + (Hash21(float2(seed, 3.0), 1383u) - 0.5) * 2.4;   // loosely outward
			const float2 dir = float2(cos(hd), sin(hd));
			const float2 nrm = float2(-dir.y, dir.x);
			const float curv = (Hash21(float2(seed, 21.0), 1362u) - 0.5) * 0.9;
			const float reach = 0.4 + 0.7 * Hash21(float2(seed, 7.0), 1389u) + amount * 0.1;
			const float life = 0.6 + 0.8 * Hash21(float2(seed, 29.0), 1365u);
			const float2 lp = centred - O;
			const float along = dot(lp, dir);
			const float perp = dot(lp, nrm);
			[branch] if (along < -0.08 || along > reach + 0.08 || abs(perp) > 0.35)
				continue;
			const float tLead = 0.05 + 0.07 * Hash21(float2(seed, 19.0), 1375u);
			float I;
			[branch] if (calm)
				I = smoothstep(tLead - 0.05, tLead + 0.05, t) * exp(-max(t - tLead - 0.05, 0.0) / (0.12 * life)) * 0.85;
			else
			{
				const float d0 = t - tLead;
				const float rs = 0.08 + 0.06 * Hash21(float2(seed, 31.0), 1364u);
				I = (d0 >= 0.0 ? exp(-d0 / (0.08 * life)) : 0.0)
				  + (d0 >= rs ? 0.75 * exp(-(d0 - rs) / (0.06 * life)) : 0.0)
				  + (d0 >= rs * 2.0 && Hash21(float2(seed, 23.0), 1374u) < 0.5 ? 0.5 * exp(-(d0 - rs * 2.0) / (0.06 * life)) : 0.0);
			}
			const float after = t > tLead ? 0.45 * exp(-(t - tLead) / (0.3 * life)) : 0.0;
			const float lead = saturate(t / tLead);
			const float headA = reach * (floor(lead * 7.0) + 1.0) / 7.0;
			const float leaderOn = t < tLead ? 0.4 : 0.0;
			const float jitter = calm ? 0.0 : floor(tq * 20.0) * 0.003;
			const float amp = 0.8 + 0.6 * Hash21(float2(seed, 33.0), 1361u);
			const float off = ArcOffset(along, seed, jitter, amp) + curv * along * along;
			const float dsg = perp - off;
			const float dist = abs(dsg);
			const float u2 = saturate(along / reach);
			// fades in over its own random distance from its origin, so no start line or ring ever shows
			const float fadeIn = smoothstep(0.0, 0.08 + 0.18 * Hash21(float2(seed, 35.0), 1360u), along);
			const float fizzle = 1.0 - smoothstep(0.55, 1.0, u2);
			const float strength = (0.7 + 0.3 * (1.0 - smoothstep(0.0, 0.4, u2))) * fadeIn * fizzle;
			const float gap = smoothstep(0.25, 0.45, ValueNoise(float2(along * 26.0, seed * 1.3), 1373u));
			const float lit = t < tLead ? step(along, headA) * leaderOn : I + after;
			const float width = 0.003 + 0.0045 * (1.0 - u2);
			const float k = lit * strength;
			core += exp(-dist * dist / (width * width * 0.4)) * k * gap;
			sheath += exp(-dist * dist / (width * width * 3.0)) * k * lerp(0.5, 1.0, gap);
			haze += exp(-dist * dist / (width * width * 4.0)) * k * (1.0 - gap) * 0.6;
			glow += (I + after * 0.6) * strength / (1.0 + dist * dist / (width * width * 600.0));
			const float hb = (I + after) * strength * exp(-dist / 0.012);
			[branch] if (hb > heat)
			{
				heat = hb;
				bendDir = nrm * sign(dsg);
			}
			// branches leave the main arc at random points and headings, each in its own coordinates
			[loop] for (uint bb = 0; bb < 3; ++bb)
			{
				const float bs = seed * 3.7 + bb * 11.0;
				const float ab = reach * (0.15 + 0.55 * Hash11(bs, 1372u));
				const float blen = reach * (0.12 + 0.3 * Hash11(bs, 1371u));
				const float2 P = O + dir * ab + nrm * (ArcOffset(ab, seed, jitter, amp) + curv * ab * ab);
				const float turn = (Hash11(bs, 1370u) < 0.5 ? -1.0 : 1.0) * (0.35 + 0.7 * Hash11(bs, 1359u));
				const float2 bdir = float2(dir.x * cos(turn) - dir.y * sin(turn), dir.x * sin(turn) + dir.y * cos(turn));
				const float2 bn = float2(-bdir.y, bdir.x);
				const float2 bl = centred - P;
				const float ba = dot(bl, bdir);
				[branch] if (ba < 0.0 || ba > blen)
					continue;
				const float bd = abs(dot(bl, bn) - ArcOffset(ba, bs, jitter, amp * 0.6));
				const float bt = 1.0 - smoothstep(0.0, 1.0, ba / blen);
				const float bw = width * (0.45 + 0.35 * bt);
				const float bLit = t < tLead ? step(ab + ba, headA) * leaderOn : (I * 0.35 + after * 0.5);
				const float kb = bLit * bt * fadeIn * fizzle;
				core += exp(-bd * bd / (bw * bw * 0.4)) * kb * 0.7;
				sheath += exp(-bd * bd / (bw * bw * 3.0)) * kb;
				glow += kb * 0.4 / (1.0 + bd * bd / (bw * bw * 200.0));
			}
			// sparks: hot bits thrown off the arc at the stroke, flying out and burning down
			[branch] if (t > tLead && t < tLead + 0.5 && dist < 0.2)
			{
				const float ds = t - tLead;
				const float cellA = floor(along * 28.0);
				[loop] for (int c = -1; c <= 1; ++c)
				{
					const float ca = cellA + c;
					const float sh = Hash21(float2(ca, seed), 1369u);
					[branch] if (sh > 0.4)
						continue;
					const float a0 = (ca + 0.5) / 28.0;
					const float side = Hash21(float2(ca, seed + 1.0), 1368u) < 0.5 ? -1.0 : 1.0;
					const float speed = 0.25 + 0.5 * Hash21(float2(ca, seed + 2.0), 1367u);
					const float travel = speed * ds * (1.0 - ds * 0.8);
					const float2 vel = normalize(dir * (Hash21(float2(ca, seed + 3.0), 1358u) - 0.3) + nrm * side);
					const float2 sp = O + dir * a0 + nrm * (ArcOffset(a0, seed, 0.0, amp) + curv * a0 * a0) + vel * travel;
					const float2 pd = centred - sp;
					const float2 streak = vel * 0.008;
					const float kk = saturate(dot(pd, streak) / dot(streak, streak) * 0.5 + 0.5);
					const float sdd = length(pd - streak * (kk * 2.0 - 1.0));
					const float burn = exp(-ds / (0.08 + 0.15 * sh)) * smoothstep(0.0, 0.1, a0) * step(a0, reach);
					sparks += (1.0 - smoothstep(0.0012, 0.0028, sdd)) * burn;
				}
			}
			flash += calm ? 0.0 : I * strength * 0.25;
		}
		const float centreSoft = lerp(0.65, 1.0, safe);
		const float g = amount * centreSoft * budget;
		// the view bends in the hot air along the channels
		[branch] if (heat > 0.01)
		{
			const float2 bend = bendDir * heat * 0.004 * g;
			color = lerp(color, saturate(Scene.SampleLevel(SceneSampler, uv + offset + float2(bend.x / aspect, bend.y), 0).rgb / range), saturate(heat * 2.0));
		}
		// glow in the air: tints the view electric blue around the channels
		const float glowA = saturate(glow * 0.8) * g;
		color = lerp(color, color * float3(0.5, 0.72, 1.15) + float3(0.03, 0.1, 0.38), glowA * 0.8);
		color += float3(0.12, 0.35, 1.0) * glowA * 0.55;
		color += float3(0.45, 0.35, 1.0) * saturate(haze) * 0.5 * g;                       // violet haze in the gaps
		color = lerp(color, float3(0.3, 0.58, 1.0) * 1.35, saturate(sheath * 0.8) * g);    // blue sheath
		color += float3(0.95, 0.97, 1.0) * saturate(core) * 1.8 * g;                       // overexposed core
		color += float3(1.0, 0.85, 0.6) * saturate(sparks) * 1.2 * g;                      // hot sparks, warmer than the arc
		// the whole view lit for an instant by the stroke (not in safe mode)
		color *= 1.0 + float3(0.4, 0.6, 1.0) * saturate(flash) * 0.6 * g;
		// slight colour fringing around the bright channels (lens chromatic aberration)
		const float2 split = float2(0.0025, 0.0) * saturate(glow) * amount;
		color.r = lerp(color.r, saturate(Scene.SampleLevel(SceneSampler, uv + offset + split, 0).r / range) + saturate(core) * 0.5, saturate(glow * 0.3) * 0.5);
		color.b = lerp(color.b, saturate(Scene.SampleLevel(SceneSampler, uv + offset - split, 0).b / range) + saturate(core) * 0.5, saturate(glow * 0.3) * 0.5);
	}

	// ---- Lasting shock (nicktheduke 2026-11g): while a shock effect stays on the player (a cloak, an aura, shock damage
	// over time) there are no full bolts. Small arcs crawl and snap along the edges and corners of the view, each
	// living a fraction of a second on its own clock, with a faint blue glow along the frame; now and then one short
	// arc flicks a little way in and dies. Nothing reaches the middle. A hit still plays its own discharge on top.
	[branch] if (frac(Breath.w) > 0.01)
	{
		const float p = saturate(frac(Breath.w) / 0.99);
		const bool calm = Global.z > 0.5;
		const float tq = floor(time * 20.0) / 20.0;
		const float2 axE = abs(uv - 0.5) * 2.0;
		const float edgeE = length(axE) * 0.8;  // nicktheduke 2026-11e: oval like a lens, no squared-off corners
		float core = 0.0, sheath = 0.0, glow = 0.0;
		[branch] if (edgeE > 0.45)
		{
			[loop] for (uint e = 0; e < 6; ++e)  // nicktheduke: [loop], large body with early-outs (fxc X3511 risk)
			{
				const float period = 0.6 + 0.8 * Hash11(e * 0.29 + 0.4, 1501u);
				const float clock = tq / period + Hash11(e * 0.53 + 0.1, 1502u);
				const float seed = floor(clock) * 5.0 + e + 500.0;
				const float t = frac(clock) * period;
				const float dur = 0.15 + 0.3 * Hash21(float2(seed, 1.0), 1503u);
				[branch] if (t > dur)
					continue;
				// a random point on the frame, and the frame's direction there
				const float bs = Hash21(float2(seed, 2.0), 1504u) * 4.0;
				const float f = frac(bs);
				const float hx = aspect * 0.5 - 0.015;
				const float hy = 0.485;
				float2 P, tang, inward;
				[branch] if (bs < 1.0)      { P = float2(lerp(-hx, hx, f), -hy); tang = float2(1.0, 0.0);  inward = float2(0.0, 1.0); }
				else if (bs < 2.0) { P = float2(hx, lerp(-hy, hy, f));  tang = float2(0.0, 1.0);  inward = float2(-1.0, 0.0); }
				else if (bs < 3.0) { P = float2(lerp(hx, -hx, f), hy);  tang = float2(-1.0, 0.0); inward = float2(0.0, -1.0); }
				else               { P = float2(-hx, lerp(hy, -hy, f)); tang = float2(0.0, -1.0); inward = float2(1.0, 0.0); }
				const bool flick = Hash21(float2(seed, 3.0), 1505u) < 0.18;
				const float tilt = flick ? 0.9 + 0.4 * Hash21(float2(seed, 4.0), 1506u) : 0.1 + 0.45 * Hash21(float2(seed, 4.0), 1506u);
				const float sgn = Hash21(float2(seed, 5.0), 1507u) < 0.5 ? -1.0 : 1.0;
				const float2 adir = normalize(tang * sgn * cos(tilt) + inward * sin(tilt));
				const float2 an = float2(-adir.y, adir.x);
				const float len = flick ? 0.18 + 0.14 * Hash21(float2(seed, 6.0), 1508u) : 0.07 + 0.16 * Hash21(float2(seed, 6.0), 1508u);
				const float2 lp = centred - P;
				const float along = dot(lp, adir);
				[branch] if (along < -0.02 || along > len + 0.02)
					continue;
				const float jitter = calm ? 0.0 : floor(tq * 20.0) * 0.003;
				const float dsg = dot(lp, an) - ArcOffset(along, seed, jitter, 0.35);
				const float dist = abs(dsg);
				const float env = smoothstep(0.0, 0.03, t) * (1.0 - smoothstep(dur * 0.45, dur, t));
				const float flicker = calm ? 1.0 : 0.55 + 0.45 * Hash21(float2(seed, floor(tq * 20.0)), 1509u);
				const float taper = 1.0 - smoothstep(len * 0.55, len, along);
				const float width = 0.0016 + 0.0012 * taper;
				const float k = env * flicker * taper * (flick ? 1.0 : 0.85);
				core += exp(-dist * dist / (width * width * 0.4)) * k;
				sheath += exp(-dist * dist / (width * width * 3.0)) * k;
				glow += k / (1.0 + dist * dist / (width * width * 250.0));
			}
		}
		const float g = p * budget;
		// a faint blue glow along the frame, breathing unevenly
		const float frameGlow = smoothstep(0.78, 1.0, edgeE) * (0.6 + 0.4 * ValueNoise(float2(q.x * 3.0 + q.y * 2.0, time * 1.5), 1510u));
		color += float3(0.1, 0.3, 1.0) * frameGlow * 0.07 * g;
		const float glowA = saturate(glow * 0.6) * g;
		color = lerp(color, color * float3(0.6, 0.78, 1.12) + float3(0.02, 0.07, 0.25), glowA * 0.6);
		color = lerp(color, float3(0.3, 0.58, 1.0) * 1.3, saturate(sheath * 0.75) * g);
		color += float3(0.92, 0.96, 1.0) * saturate(core) * 1.5 * g;
	}

	// ---- Dragonfire: the edges of the view catch and burn inward like paper: charred, with a glowing line of fire
	// along the burn front, heat glow just inside it and sparks lifting off the edge (not loose embers). ----
	[branch] if (LayersA.x > 0.001)
	{
		const float amount = LayersA.x;
		const float burnNoise = Fbm(q * 3.0 + float2(0.0, -time * 0.3), 1331u);
		const float burnFine = ValueNoise(q * 16.0 + float2(time * 0.5, -time * 1.8), 1332u);
		const float front = edgeR + (burnNoise - 0.5) * 0.35 + (burnFine - 0.5) * 0.05;
		const float burnStart = 1.1 - amount * 0.4;
		const float charred = smoothstep(burnStart + 0.02, burnStart + 0.12, front);
		const float rimLine = exp(-Sq((front - (burnStart + 0.02)) / 0.02));
		const float heatBand = exp(-Sq((front - burnStart) / 0.1));
		const float flame = 0.7 + 0.3 * ValueNoise(float2(time * 8.0, front * 30.0), 1334u);
		color = lerp(color, color * 0.16 + float3(0.02, 0.012, 0.008), saturate(charred * 0.85) * budget);
		const float3 rimColor = lerp(float3(1.0, 0.32, 0.04), float3(1.0, 0.82, 0.45), saturate(burnFine * 1.4 - 0.2));
		color += rimColor * rimLine * flame * 1.5 * amount * budget;
		color += float3(1.0, 0.4, 0.1) * heatBand * 0.16 * amount * budget;
		const float edge = smoothstep(burnStart - 0.2, burnStart + 0.05, front);
		float3 emberLight = 0.0;
		[loop] for (uint layer = 0; layer < 2; ++layer)
		{
			const float scaleE = layer == 0 ? 8.0 : 14.0;
			const float2 drift = float2(sin(time * 0.5 + layer) * 0.5 + time * 0.12, -time * (0.45 + 0.2 * layer));
			const float2 g = q * scaleE + drift * scaleE * 0.5;
			const float2 cell = floor(g);
			const float h = Hash21(cell, 1331u + layer * 7u);
			[branch] if (h < 0.035)
			{
				const float2 jitter = float2(Hash21(cell, 1333u), Hash21(cell, 1337u)) - 0.5;
				const float d = length(frac(g) - 0.5 - jitter * 0.6);
				const float flicker = saturate(0.6 + 0.4 * sin(time * (3.0 + 4.0 * h) + h * 40.0));
				emberLight += float3(1.0, 0.45, 0.1) * (1.0 - smoothstep(0.0, 0.1 - 0.03 * layer, d)) * flicker;
			}
		}
		color += emberLight * amount * edge * (1.0 - charred * 0.7) * 1.2 * budget;  // sparks only off the burning edge
	}

	// ---- Steam (dwemer): soft white fog closing in from the edges ----
	[branch] if (LayersB.y > 0.001)
	{
		const float n = Fbm(q * 2.4 + float2(time * 0.05, -time * 0.12), 1341u);
		const float m = smoothstep(0.35, 0.95, edgeR + (n - 0.5) * 0.5) * LayersB.y * safe * budget;
		const float3 soft = SampleBlurred(uv + offset, 8.0 * m, range);
		color = lerp(color, soft * 1.05 + 0.05, saturate(m * 0.6));
	}

	// ---- Stomp (nicktheduke 2026-11b): what a heavy impact on the ground really throws up. A dense surge of dust rises
	// from the bottom of the view, fast at first and slowing under drag, its top a rolling, cauliflower edge. It is
	// coloured by the ground it came from (sampled along the bottom of the view: snow powder, dirt, stone dust),
	// lit from above (bright tops, shadowed undersides) and it thins as it spreads. Heavy grit is thrown up on
	// ballistic arcs, motion-blurred, and falls back; fine dust drifts on up and fades. LayersB.x: 1 -> 0 over 1.4 s.
	[branch] if (LayersB.x > 0.001)
	{
		const float amount = LayersB.x;
		const float age = 1.0 - saturate(amount);
		const float t = age * 1.4;
		// the ground that was kicked up
		float3 ground = 0.0;
		[loop] for (uint gs = 0; gs < 4; ++gs)
			ground += Scene.SampleLevel(SceneSampler, float2(0.14 + 0.24 * gs, 0.93), 0).rgb;
		ground = saturate(ground / (4.0 * range));
		const float3 groundTone = lerp(float3(0.52, 0.46, 0.37), ground, 0.5);
		const float3 dustBase = lerp(groundTone, Luminance(groundTone).xxx * float3(1.04, 1.0, 0.94), 0.45) * 1.15 + 0.06;
		// the surge front: rises fast, slows under drag; uneven along its width
		const float height = 0.52 * (1.0 - exp(-t * 3.2));
		const float2 adv = q + float2(0.0, t * 0.22);   // material carried upward
		const float2 warp = float2(Fbm(adv * 2.2 + 3.1, 1551u), Fbm(adv * 2.2 + 8.7, 1553u)) - 0.5;
		const float roll = Fbm((adv + warp * 0.3) * 3.6, 1555u);
		const float fine = Fbm((adv + warp * 0.2) * 9.0 + 2.0, 1556u);
		const float frontY = 1.0 - height * (0.7 + 0.6 * Fbm(float2(q.x * 1.6, t * 0.4), 1557u));
		const float y = uv.y + (roll - 0.5) * 0.16 + (fine - 0.5) * 0.04;   // cauliflower edge
		const float inside = smoothstep(frontY - 0.02, frontY + 0.1, y);
		// densest near the ground and in the turbulent cores; the whole cloud thins as it spreads
		const float depth = saturate((y - frontY) / max(height, 0.05));
		const float density = inside * (0.35 + 0.65 * smoothstep(0.25, 0.75, roll)) * (0.55 + 0.6 * depth);
		const float life = smoothstep(0.0, 0.06, t) * (1.0 - smoothstep(0.35, 1.0, age));
		const float m = saturate(density * 1.4) * 0.92 * life * budget;
		[branch] if (m > 0.002)
		{
			const float3 soft = SampleBlurred(uv + offset + warp * 0.01, 7.0 * m, range);
			// lit from above: the top of each billow is bright, underneath falls into its own shadow
			const float lit = saturate(0.85 - depth * 0.5 + (roll - 0.5) * 1.3 + (1.0 - inside) * 0.4);
			const float3 dust = dustBase * lerp(0.5, 1.2, lit) * (0.8 + 0.3 * Luminance(soft));
			color = lerp(color, lerp(soft, dust, 0.8), saturate(m));
		}
		// grit: thrown up on ballistic arcs, slowed by drag, falling back; drawn as short motion-blurred streaks
		float grit = 0.0;
		[loop] for (uint gl = 0; gl < 2; ++gl)
		{
			const float cols = gl == 0 ? 26.0 : 48.0;
			const float colW = aspect / cols;
			const float c0 = floor(q.x / colW);
			[loop] for (int nb = -1; nb <= 1; ++nb)
			{
				const float cx = c0 + nb;
				const float h = Hash21(float2(cx, gl), 1581u);
				[branch] if (h > 0.55)
					continue;
				const float launch = 0.05 * Hash21(float2(cx, gl), 1583u);
				const float tt = max(t - launch, 0.0);
				const float v = (gl == 0 ? 1.0 : 0.75) * (0.55 + 0.6 * Hash21(float2(cx, gl), 1585u));
				const float vx = (Hash21(float2(cx, gl), 1587u) - 0.5) * 0.35;
				const float g = 2.4;
				const float2 pos = float2((cx + 0.5) * colW + vx * tt, 1.03 - v * tt + 0.5 * g * tt * tt);
				const float2 vel = float2(vx, -v + g * tt);
				const float2 blur = vel * 0.018;   // the distance it covers during one exposure
				const float2 pd = q - pos;
				const float k = saturate(dot(pd, blur) / max(dot(blur, blur), 1e-6) * 0.5 + 0.5);
				const float dd = length(pd - blur * (k * 2.0 - 1.0));
				const float r = (gl == 0 ? 0.0028 : 0.0018) * (0.7 + 0.6 * Hash21(float2(cx, gl), 1589u));
				grit = max(grit, (1.0 - smoothstep(r * 0.5, r, dd)) * step(tt, 1.2) * (gl == 0 ? 0.9 : 0.7));
			}
		}
		const float gritA = saturate(grit * smoothstep(0.0, 0.05, t) * (1.0 - smoothstep(0.6, 1.0, age)) * budget);
		color = lerp(color, dustBase * 0.45, gritA * 0.85);
		// fine dust: small motes carried up on the turbulence, each fading out on its own
		float motes = 0.0;
		[loop] for (uint layer = 0; layer < 2; ++layer)
		{
			const float scale = layer == 0 ? 140.0 : 80.0;
			const float2 drift = float2(warp.x * 0.06, -t * (layer == 0 ? 0.12 : 0.09));
			const float2 gg = (q - drift) * scale;
			const float2 cell = floor(gg);
			const float hh = Hash21(cell, 1561u + layer * 7u);
			[branch] if (hh < (layer == 0 ? 0.12 : 0.06))
			{
				const float2 c = 0.5 + (float2(Hash21(cell, 1563u + layer), Hash21(cell, 1565u + layer)) - 0.5) * 0.6;
				const float r = 0.16 * (0.5 + Hash21(cell, 1567u));
				const float dd = length(frac(gg) - c);
				const float wink = sin(saturate(t / (0.5 + 0.8 * Hash21(cell, 1569u)) - Hash21(cell, 1571u) * 0.3) * 3.1416);
				motes += (1.0 - smoothstep(r * 0.4, r, dd)) * saturate(wink) * (layer == 0 ? 0.8 : 0.55);
			}
		}
		const float moteA = saturate(motes * saturate(inside * 1.5 + 0.2) * smoothstep(0.35, 0.7, uv.y) * life * budget);
		color = lerp(color, dustBase * 1.2 + 0.05, moteA * 0.7);
	}

	// ---- Poison (nicktheduke 2026-11d): violet and black smoke seeping in from every edge toward the middle of the view.
	// Slow and sinister: the smoke curls (its flow direction field turns over time, so it winds into eddies) and the
	// two smokes curl opposite ways, winding around and through each other. The front creeps inward on fingers that
	// change shape slowly; nothing swells or pulses in size. Inside the violet a faint light breathes, pockets
	// glowing and dimming out of step, a little stronger after each poison tick. The view behind dims and drains
	// of colour toward the edges. Smoke on the lens is out of focus: everything is soft. ----
	[branch] if (LayersA.z > 0.001)
	{
		const float amount = LayersA.z;
		const float pulseRaw = saturate(LayersB.z);
		const float pp = pulseRaw * (1.0 - pulseRaw);
		const float tick = pp * pp * pp * 64.0;   // flat at both ends: a slow swell of light after a tick, never a flash
		const float tt = time;
		// nicktheduke 2026-11e: an oval like a lens vignette (it used to be a rounded rectangle, which read as a box in the corners)
		const float2 ax = abs(uv - 0.5) * 2.0;
		const float edgeE = length(ax) * 0.8;
		// a direction field that slowly turns: following it curls the smoke into eddies
		const float curlA = (Fbm(q * 1.1 + float2(tt * 0.015, -tt * 0.01), 1360u) - 0.5) * 9.0 + tt * 0.12;
		const float2 curl = float2(cos(curlA), sin(curlA));
		// violet: winds one way
		const float2 va = q * 1.4 + curl * 0.12 + float2(tt * 0.012, -tt * 0.008);
		const float2 vw1 = float2(Fbm(va + float2(tt * 0.025, 0.0), 1361u), Fbm(va + float2(0.0, -tt * 0.025) + 4.7, 1362u)) - 0.5;
		const float2 vw2 = float2(Fbm(va * 1.7 + vw1 * 1.8 + float2(-tt * 0.035, tt * 0.015), 1363u), Fbm(va * 1.7 + vw1 * 1.8 + float2(tt * 0.02, tt * 0.03) + 9.2, 1364u)) - 0.5;
		const float violet = Fbm((va + vw2 * 0.7) * 1.9, 1365u);
		// black: winds the other way and folds through the violet
		const float2 ba = q * 1.6 - curl * 0.12 + float2(-tt * 0.01, tt * 0.012) + 13.0;
		const float2 bw = float2(Fbm(ba + float2(-tt * 0.03, tt * 0.01) + vw1 * 0.9, 1366u), Fbm(ba + float2(tt * 0.015, tt * 0.03) + 3.3, 1368u)) - 0.5;
		// black as tendrils: ridges of the folded field (thin, winding), never broad solid areas
		const float blackField = Fbm((ba + bw * 0.9 - vw2 * 0.5) * 2.6, 1367u);
		const float black = pow(saturate(1.0 - abs(blackField * 2.0 - 1.0)), 0.55) * 0.75 + blackField * 0.1;   // wide, soft wisps
		// the creeping front: fingers whose shape drifts slowly (continuous all the way round, no seam)
		const float2 rd = centred / max(length(centred), 1e-3);
		const float fingers = Fbm(rd * 1.6 + float2(7.0, tt * 0.05), 1369u) + 0.5 * Fbm(rd * 4.5 + float2(2.0, tt * 0.08), 1370u);
		const float reach = edgeE + (max(violet, black) - 0.5) * 0.5 + (fingers - 0.75) * 0.4;
		// every edge and corner is always covered; the noise only shapes how far in it reaches
		const float region = max(smoothstep(1.16 - amount * 0.38, 1.36 - amount * 0.33, reach), smoothstep(0.84, 0.99, edgeE) * saturate(amount * 1.5)) * safe * budget;
		// the poison gets into you: the view dims and loses colour toward the edges
		const float dim = smoothstep(0.25, 1.05, edgeE) * amount * 0.3 * budget;
		color = lerp(color, Luminance(color).xxx * float3(0.85, 0.78, 0.95), dim * 0.5) * (1.0 - dim * 0.45);
		[branch] if (region > 0.001)
		{
			const float3 behind = SampleBlurred(uv + offset + vw2 * 0.006, 6.0 * region, range);
			const float vDen = smoothstep(0.3, 0.7, violet);
			const float bDen = smoothstep(0.54, 0.84, black) * 0.9;
			const float3 deepViolet = float3(0.16, 0.03, 0.26);
			const float3 glowViolet = float3(0.62, 0.32, 0.95);
			const float glowAmt = saturate(1.0 - abs(violet - 0.52) * 4.0) * 0.6 + pow(saturate(fingers - 0.6), 2.0) * 0.4;
			float3 smoke = lerp(deepViolet, glowViolet, saturate(glowAmt));
			// a faint light breathing inside the thick violet: pockets glow and dim out of step with each other
			const float pocket = smoothstep(0.55, 0.8, violet) * (1.0 - bDen);
			const float breath = 0.5 + 0.5 * sin(tt * 1.1 + Fbm(q * 2.3 + 5.0, 1371u) * 12.0);
			smoke += glowViolet * pocket * (breath * 0.35 + tick * 0.25);
			// where violet and black wind into each other the seam glows faintly
			const float seam = saturate(1.0 - abs(vDen - bDen) * 2.5) * vDen * bDen;
			smoke += glowViolet * seam * 0.3;
			smoke = lerp(smoke, float3(0.01, 0.004, 0.015), bDen * (1.0 - seam * 0.5));
			const float density = saturate(max(vDen, bDen) * 1.1 + 0.15 + 0.5 * smoothstep(0.85, 1.0, edgeE)) * region;
			color = lerp(color, lerp(behind * 0.6, smoke, 0.85), saturate(density));
			// (emissive inner light below)
			color += float3(0.55, 0.25, 0.9) * pocket * (breath * 0.12 + tick * 0.1) * saturate(density);
		}
		// tendrils: thin wisps that slip past the edge of the cloud toward the middle, winding, and fade out well
		// before the clear centre. They creep inward slowly (their pattern flows toward the middle).
		[branch] if (edgeE > 0.35)
		{
			const float2 tq2 = rd * 2.2 + vw1 * 0.6;
			const float tf = Fbm(tq2 + float2(edgeE * 2.6 + tt * 0.07, 3.0), 1372u);
			const float tendril = pow(saturate(1.0 - abs(tf * 2.0 - 1.0)), 7.0);
			const float tf2 = Fbm(tq2 * 1.7 + float2(edgeE * 3.4 + tt * 0.09, 8.0), 1373u);
			const float tendril2 = pow(saturate(1.0 - abs(tf2 * 2.0 - 1.0)), 9.0);
			const float zone = smoothstep(0.38, 0.62, edgeE) * (1.0 - region) * amount * safe * budget;
			const float tv = tendril * zone * 0.65;
			const float tb = tendril2 * zone * 0.5;
			color = lerp(color, lerp(color * 0.6, float3(0.45, 0.2, 0.7), 0.6), saturate(tv));
			color = lerp(color, float3(0.02, 0.008, 0.03), saturate(tb * 0.8));
		}
	}

	// ---- Frost rime from dragon frost breath: feathered ice growing in from the side the breath comes from ----
	[branch] if (LayersA.y > 0.001)
	{
		const float2 dir = normalize(RimeDir.xy + 1e-5);
		const float2 perp = float2(-dir.y, dir.x);
		const float side = dot(normalize(centred + 1e-5), dir);
		const float feathers = Fbm(float2(dot(q, perp) * 16.0, dot(q, dir) * 3.5) + float2(0.0, time * 0.01), 1371u);
		const float reach = edgeR + side * 0.35 + (feathers - 0.5) * 0.35;
		const float m = smoothstep(1.05 - LayersA.y * 0.6, 1.15 - LayersA.y * 0.5, reach) * safe;
		// If Screen Weather frost already covers the lens, sharpen and brighten it instead of piling on a second layer.
		const float merge = saturate(RimeDir.z * 1.5);
		[branch] if (m > 0.001)
		{
			// nicktheduke: rime is hard, directional ice: fern spines across the feathers, the view refracted through it,
			// and glints of snow and ice that twinkle. Screen Weather frost is a soft vignette; this is a blast.
			const float along = dot(q, dir);
			const float across = dot(q, perp);
			const float spines = pow(1.0 - abs(frac(across * 38.0 + feathers * 3.0 + along * 9.0) * 2.0 - 1.0), 6.0) * smoothstep(0.3, 0.7, feathers);
			const float2 iceShift = perp * (feathers - 0.5) * 0.014 * m;
			const float3 behind = SampleBlurred(uv + offset + float2(iceShift.x / aspect, iceShift.y), 5.0 * m, range);
			const float opacity = m * lerp(0.6, 0.25, merge) * budget;
			color = lerp(color, behind * float3(0.86, 0.94, 1.06) + 0.07, saturate(opacity));
			const float structure = saturate(pow(saturate(feathers), 3.0) * 0.8 + spines * 0.5);
			color += float3(0.75, 0.88, 1.0) * structure * m * (0.3 + 0.25 * merge) * budget;
			[loop] for (uint gl = 0; gl < 2; ++gl)
			{
				const float2 gg = q * (gl == 0 ? 70.0 : 150.0);
				const float2 gc = floor(gg);
				const float gh = Hash21(gc, 1375u + gl * 11u);
				[branch] if (gh < (gl == 0 ? 0.05 : 0.09))
				{
					const float2 gd2 = frac(gg) - (0.25 + 0.5 * float2(Hash21(gc, 1377u + gl), Hash21(gc, 1379u + gl)));
					const float gd = length(gd2);
					const float twinkle = pow(saturate(sin(time * (2.0 + 70.0 * gh) + gh * 120.0) * 0.5 + 0.5), 6.0);
					const float star = (1.0 - smoothstep(0.0, 0.1, gd)) + 0.5 * (1.0 - smoothstep(0.0, 0.025, min(abs(gd2.x), abs(gd2.y)))) * (1.0 - smoothstep(0.05, 0.3, gd));
					color += float3(0.9, 0.96, 1.0) * star * (0.3 + 0.7 * twinkle) * m * budget * (gl == 0 ? 0.9 : 0.5);
				}
			}
		}
	}

	// ---- Splats on the glass ----
	const uint splatCount = min((uint)Global.w, (uint)MAX_SPLATS);
	// nicktheduke: one wet highlight for all the fluid on the glass. Each splat used to add its own shine, so overlapping
	// splats stacked white "bubbles". Now the newest splat that covers a pixel owns its highlight (fluid on top hides
	// the shine of fluid under it) and the shine is tinted by the fluid, with much less white.
	float glossValue = 0.0;
	float bloodBelow = 0.0;   // nicktheduke: blood already on the glass under this pixel (from earlier splats)
	float3 glossTint = 1.0;
	[loop] for (uint i = 0; i < splatCount; ++i)
	{
		const float4 a = SplatA[i];
		const float4 b = SplatB[i];
		const float alpha = SplatC[i].x * budget;
		const float size = max(a.z, 0.005);
		const float2 centreQ = float2(a.x * aspect, a.y);
		float2 p = (q - centreQ) / size;
		[branch] if (dot(p, p) > 9.0 || alpha <= 0.001)
			continue;
		p = Rotate(p, b.w);
		const uint type = (uint)b.x;
		const uint variant = (uint)b.y;
		const float seed = b.z;
		const float progress = a.w;
		const float fade = 1.0 - smoothstep(0.7, 1.0, progress);
		const float splatSafe = safe;

		[branch] if (type == 0u)
		{
			// Blood and other body fluids, by creature.
			const float3 tint = FluidTint(variant);
			float core;
			[branch] if (variant == 7u || variant == 9u)
			{
				// Atronach embers or sparks: glowing flecks, no liquid.
				const float3 sparkLight = Ember(p, seed, saturate(progress * 1.6));
				color += sparkLight * tint * 1.4 * alpha * splatSafe;
			}
			else if (variant == 8u)
			{
				// Ice shards.
				const float r0 = length(p);
				const float angle = atan2(p.y, p.x);
				const float spikes = pow(abs(sin(angle * 6.0 + seed * 10.0)), 10.0);
				const float m = (1.0 - smoothstep(0.08, 0.2 + 0.65 * spikes, r0)) * fade * alpha * splatSafe;
				color = lerp(color, SampleBlurred(uv, 4.0, range) * 0.9 + tint * 0.25, saturate(m * 0.6));
				color += tint * Sq(spikes) * m * 0.25;
			}
			else if (variant == 10u)
			{
				// nicktheduke: ectoplasm (ghosts, conjured spirits, familiars): a pale glowing, translucent goo that bends the view
				// behind it and slides off slowly. Glows from inside instead of shining like blood.
				float ectoCore;
				float shape = SplatShape(p * 0.95, seed, ectoCore);
				shape = max(shape, DripHeight(p, seed, progress, 0.6));
				const float wisp = Fbm(p * 2.0 + seed * 9.0 + time * 0.08, 1401u);
				const float m = saturate(shape * (0.6 + 0.5 * wisp)) * fade * alpha * splatSafe;
				[branch] if (m > 0.001)
				{
					const float2 bend = (p * ectoCore * 0.02 + (wisp - 0.5) * 0.01) * size;
					const float3 through = SampleBlurred(uv - float2(bend.x / aspect, bend.y), 2.5, range);
					color = lerp(color, through * lerp(1.0.xxx, tint, 0.55) + tint * 0.05, saturate(m * 0.7));
					color += tint * (0.05 + 0.12 * pow(saturate(wisp), 3.0)) * m;
					if (ectoCore > 0.6) { glossValue = 0.0; }
				}
			}
			else
			{
				const bool dry = variant == 4u || variant == 5u;
				[branch] if (dry)
				{
					float dcore;
					float shape = SplatShape(p, seed, dcore) * step(0.35, ValueNoise(p * 9.0 + seed * 31.0, 1403u));  // crumbly
					const float m = saturate(shape) * fade * alpha * splatSafe;
					color = lerp(color, lerp(color, tint * (0.55 + 0.45 * Luminance(color)), 0.8), saturate(m * 0.75));
				}
				else
				{
					// unrotated splat space: drips follow gravity whatever the splat's rotation
					const float2 s = (q - centreQ) / size;
					const float dripSpeed = variant == 3u ? 1.2 : (variant == 2u ? 1.6 : 2.3);
					const float e = 0.006;
					const float h0 = BloodHeight(p, s, seed, progress, dripSpeed);
					[branch] if (h0 > 0.002)
					{
						const float hx = BloodHeight(p + Rotate(float2(e, 0.0), b.w), s + float2(e, 0.0), seed, progress, dripSpeed);
						const float hy = BloodHeight(p + Rotate(float2(0.0, e), b.w), s + float2(0.0, e), seed, progress, dripSpeed);
						const float2 grad = float2(hx - h0, hy - h0) / e;  // screen-aligned slope
						const float slope = length(grad);
						const float cover = smoothstep(0.0, 0.05, h0) * fade * alpha * splatSafe;
						// refraction: every drop is a small lens bending the view behind it
						const float2 bend = -grad * 0.004 * size;
						// what this fluid sits on: the scene (bent by the drop), or blood already on the glass. Seeing
						// the raw scene through the thin edge of an overlapping splat made a bright line between them.
						const float3 sceneBehind = saturate(Scene.SampleLevel(SceneSampler, uv + offset + float2(bend.x / aspect, bend.y), 0).rgb / range);
						const float3 behind = lerp(sceneBehind, color, saturate(bloodBelow * 1.6));
						// fresh and wet for the whole effect (blood takes minutes to clot). Beer-Lambert by thickness: thin film
						// lets through bright saturated red, the thick middle goes deep crimson. Rims are thin, so lighter.
						const float3 absorb = -log(max(tint, 0.004));
						float3 fluid = behind * exp(-absorb * h0 * 1.15) + tint * (0.05 + 0.1 * h0);
						// a fine dark line where the surface turns over at the contact line
						fluid *= 1.0 - saturate(slope * 0.02 - 0.2) * 0.25;
						// nicktheduke 2026-11c: no highlight. The blood is on the outside of the lens and we see it from behind,
						// through the glass: the side facing us lies flat on the glass, and its curved surface faces the
						// scene. Light comes from the scene, through the blood: thin parts glow red (above), and the
						// curved rims bend a little of the brightest light behind toward us, a faint warm rim on the
						// side facing that light. Nothing white, nothing that reads as a bubble.
						const float lR = Luminance(Scene.SampleLevel(SceneSampler, uv + float2(0.05, 0.0), 0).rgb / range);
						const float lL = Luminance(Scene.SampleLevel(SceneSampler, uv - float2(0.05, 0.0), 0).rgb / range);
						const float lD = Luminance(Scene.SampleLevel(SceneSampler, uv + float2(0.0, 0.05), 0).rgb / range);
						const float lU = Luminance(Scene.SampleLevel(SceneSampler, uv - float2(0.0, 0.05), 0).rgb / range);
						const float2 lg = float2(lR - lL, lD - lU);
						const float2 toLight = lg / max(length(lg), 1e-3);
						const float lightAmt = saturate(max(max(lR, lL), max(lD, lU)) * 1.2);
						const float rimBand = saturate(slope * 0.025) * smoothstep(0.04, 0.2, h0) * (1.0 - smoothstep(0.4, 0.85, h0));
						const float rimFacing = saturate(dot(grad / max(slope, 1e-3), toLight) * 0.7 + 0.3);
						fluid += lerp(tint * 2.4, 1.0.xxx, 0.15) * rimBand * rimFacing * lightAmt * 0.16 * (1.0 - saturate(bloodBelow * 2.0));
						color = lerp(color, fluid, saturate(cover));
						bloodBelow = max(bloodBelow, saturate(cover));
					}
				}
			}
		}
		else if (type == 1u)
		{
			// Venom spit: thick green goo with a few trapped bubbles, dripping as it slides off.
			float core;
			float shape = SplatShape(p * 0.9, seed, core);
			shape = max(shape, DripHeight(p, seed, progress, 1.1));
			const float m = saturate(shape) * fade * alpha * splatSafe;
			[branch] if (m > 0.001)
			{
				const float3 goo = float3(0.36, 0.62, 0.12);
				float3 film = color * goo * 1.6 + goo * 0.12;
				film = VenomBubbles(p, seed, saturate(core * 1.3), uv, size, range, film);
				color = lerp(color, film, saturate(m * 0.85));
				const float shine = (1.0 - smoothstep(0.0, 0.25, length(p - float2(-0.15, -0.2)))) * core * 0.22 * m;
				if (core > 0.6 || shine > glossValue)
				{
					glossValue = shine;
					glossTint = goo;
				}
			}
		}
		else if (type == 2u)
		{
			// nicktheduke 2026-11c: a glob of spider silk hitting the lens, sticking, then sliding off.
			// Hit: it arrives out of focus and small, slaps flat onto the glass with a little overshoot, and its
			// few loose strands snap outward. Stick: it hangs, sagging slowly under its own weight. Slide: it lets go
			// and runs down, slowly then faster, stretching, its top pulled into a pointed tail; behind it stays a thin
			// milky residue that sheds a few tiny beads (pearling) and fades quickly.
			// Light: silk scatters like tiny glass rods, a soft white glint (no rainbow) wherever a fibre lies across
			// the direction the light comes from. The light comes from the bright part of the scene behind, so glints
			// travel over the web as it sags and slides and as the view behind changes.
			const float2 s = (q - centreQ) / size;   // unrotated: gravity is screen-down
			// hit hard: it arrives fast, splats wider than it ends up, recoils and wobbles to rest
			// THWACK: it arrives in a few frames, smacks flat much wider than it ends up, snaps back and shudders to rest
			const float impact = saturate(progress / 0.012);
			const float settle = 1.0 - pow(1.0 - impact, 4.0);
			const float since = max(progress - 0.012, 0.0);
			const float wobble = 0.05 * exp(-since * 70.0) * sin(since * 150.0 + 1.6);   // a small, heavy shudder
			const float spread = 0.3 + 0.7 * settle + 0.18 * sin(impact * 3.1416) * impact + wobble;
			const float slap = exp(-since * 35.0) * step(0.012, progress);   // the instant of impact
			// one continuous motion after it lands: a slow sag that keeps speeding up into the slide (no stop, no restart)
			const float x = saturate((progress - 0.03) / 0.5);
			const float slideY = 0.05 * x + 2.2 * x * x * x;   // velocity rises steadily from the moment it sticks
			const float slideT = x * x;                         // stretch and tail grow with it
			const float sagY = slideY;
			float2 w = s;
			w.y -= slideY;
			w /= spread;
			w.y = w.y < 0.0 ? w.y / (1.0 + slideT * 1.6) : w.y / (1.0 + slideT * 0.25);   // the top drags into a tail
			w.x *= 1.0 + slideT * 0.35 * smoothstep(0.0, -0.6, w.y);                     // and narrows to a cusp
			const float sway = time * 0.3;
			const float2 air = float2(ValueNoise(float2(w.y * 1.5, sway), 1411u), ValueNoise(float2(w.x * 1.5, sway + 3.0), 1412u)) - 0.5;
			w += air * 0.03;
			const float r0 = length(w * float2(1.0, 1.15));
			const float lump = Fbm(w * 2.0 + seed * 5.0, 1413u);
			const float focus = smoothstep(0.0, 0.85, impact);   // out of focus until it touches the glass
			const float edgeSoft = lerp(0.6, 0.0, focus);
			const float rr = r0 + (lump - 0.5) * 0.5;
			const float body = 1.0 - smoothstep(0.28 - edgeSoft * 0.2, 0.95 + edgeSoft, rr);
			// the glob's edge on the glass: a thick, curved rim that bends the light (a soft dark line where the view
			// is refracted away, a faint bright line just inside it) and a soft contact shadow just outside
			const float rimD = rr - 0.7;
			const float rim = exp(-rimD * rimD / (0.006 + edgeSoft * 0.05)) * focus;
			const float rimInner = exp(-(rr - 0.66) * (rr - 0.66) / 0.0012) * focus;   // a thin refractive line
			const float shadowRing = exp(-(rr - 0.82) * (rr - 0.82) / 0.02) * focus;
			const float2 fw = w + (float2(Fbm(w * 1.7 + seed, 1414u), Fbm(w * 1.7 + seed + 6.1, 1415u)) - 0.5) * 0.6;
			const float ridgeA = 1.0 - abs(ValueNoise(fw * float2(5.0, 2.2) + seed * 2.0, 1416u) * 2.0 - 1.0);
			const float ridgeB = 1.0 - abs(ValueNoise(Rotate(fw, 1.1) * float2(11.0, 4.0) + seed * 3.0, 1417u) * 2.0 - 1.0);
			const float filaments = (pow(saturate(ridgeA), 14.0) * 0.45 + pow(saturate(ridgeB), 20.0) * 0.3) * focus;
			// where the light comes from: the brighter side of the scene behind the web
			float2 lightDir;
			float lightAmt;
			{
				const float lR = Luminance(Scene.SampleLevel(SceneSampler, uv + float2(0.06, 0.0), 0).rgb / range);
				const float lL = Luminance(Scene.SampleLevel(SceneSampler, uv - float2(0.06, 0.0), 0).rgb / range);
				const float lD = Luminance(Scene.SampleLevel(SceneSampler, uv + float2(0.0, 0.06), 0).rgb / range);
				const float lU = Luminance(Scene.SampleLevel(SceneSampler, uv - float2(0.0, 0.06), 0).rgb / range);
				const float2 g = float2(lR - lL, lD - lU) + float2(-0.04, -0.06);   // a faint default: light from above
				lightDir = g / max(length(g), 1e-4);
				lightAmt = saturate(0.25 + (lR + lL + lD + lU) * 0.25 * 0.9);
			}
			// a few loose strands that snap outward on impact
			float strands = 0.0, strandEdge = 0.0, glints = 0.0;
			const float snap = 1.0 - pow(1.0 - saturate(progress / 0.035), 3.0);
			[loop] for (uint k = 0; k < 3; ++k)
			{
				const float ks = seed + k * 0.731;
				const float a = (Hash11(ks, 1418u) - 0.5) * 2.6 + (k & 1u) * 3.1416;
				const float2 dir = normalize(float2(cos(a), sin(a) * 0.55));
				const float2 nrm = float2(-dir.y, dir.x);
				const float tLen = dot(w, dir);
				const float reachS = (0.5 + 0.45 * Hash11(ks, 1419u)) * snap;
				[branch] if (tLen < -0.1 || tLen > reachS)
					continue;
				const float sag = tLen * tLen * (0.12 + 0.18 * Hash11(ks, 1420u)) * (1.0 + slideT * 1.5);
				const float wave = (Fbm(float2(tLen * 1.8 + sway * 0.6, ks * 9.0), 1421u) - 0.5) * 0.22;
				const float offs = (Hash11(ks, 1422u) - 0.5) * 0.3;
				const float d = dot(w, nrm) - offs - wave - sag * sign(nrm.y + 1e-4);
				const float taper = 1.0 - smoothstep(reachS * 0.4, max(reachS, 1e-3), tLen);
				const float thick = (0.006 + 0.008 * Hash11(ks, 1423u)) * (0.4 + 0.6 * taper) / (1.0 + slideT * 0.8);
				const float broken = smoothstep(0.32, 0.5, ValueNoise(float2(tLen * 5.0, ks * 4.0), 1424u));
				const float sCore = exp(-d * d / (thick * thick)) * taper * broken * 0.45 * focus;
				strands = max(strands, sCore);
				strandEdge = max(strandEdge, exp(-d * d / (thick * thick * 5.0)) * taper * broken * 0.45 * focus);
				// the strand glints where it lies across the light; the sag and wave bend it, so the glint slides along
				const float2 tangent = normalize(dir + nrm * (wave * 0.8 + sag * 2.0 * sign(nrm.y + 1e-4)));
				glints = max(glints, sCore * pow(saturate(1.0 - abs(dot(tangent, lightDir))), 6.0) * lightAmt);
			}
			// silk specks: a few big soft ones (out of focus), some smaller crisp ones; each a short fibre with its own
			// orientation that glints when it turns across the light
			float specks = 0.0, speckRim = 0.0, speckGlint = 0.0;
			const float turn = air.x * 2.5 + slideT * 2.0 + sagY * 6.0;   // fibres turn as the web sags, sways and slides
			[loop] for (uint sl = 0; sl < 3; ++sl)
			{
				const float sc = sl == 0 ? 30.0 : (sl == 1 ? 17.0 : 8.5);
				const float2 sg = w * sc + seed * (3.0 + sl * 2.0);
				const float2 scell = floor(sg);
				const float sh = Hash21(scell, 1441u + sl);
				const float dens = (sl == 0 ? 0.1 : (sl == 1 ? 0.13 : 0.17)) * saturate(body * 1.8);
				[branch] if (sh < dens)
				{
					const float2 sc2 = 0.5 + (float2(Hash21(scell, 1443u + sl), Hash21(scell, 1445u + sl)) - 0.5) * 0.6;
					const float sr = (sl == 2 ? 0.24 : 0.22) * (0.45 + 0.8 * Hash21(scell, 1447u + sl));
					const float sd = length(frac(sg) - sc2);
					const float soft = lerp(sl == 2 ? 0.35 : (sl == 1 ? 0.5 : 0.6), 0.05, 1.0 - focus);
					const float speck = (1.0 - smoothstep(sr * soft, sr, sd)) * (sl == 2 ? 0.6 : (sl == 1 ? 0.75 : 0.85)) * focus;
					specks = max(specks, speck);
					[branch] if (sl > 0u)
						speckRim = max(speckRim, exp(-((sd - sr * 0.8) / (sr * 0.2)) * ((sd - sr * 0.8) / (sr * 0.2))) * (sl == 2 ? 0.8 : 0.5) * focus);
					const float ang = Hash21(scell, 1449u) * 3.1416 + turn * (0.6 + 0.8 * Hash21(scell, 1451u));
					const float across = 1.0 - abs(dot(float2(cos(ang), sin(ang)), lightDir));
					speckGlint = max(speckGlint, speck * pow(saturate(across), 10.0) * lightAmt);
				}
			}
			// residue left behind by the slide: a thin streaky film between where it stuck and where it is now,
			// shedding a few tiny beads, fading quickly behind the moving glob
			float residue = 0.0, pearls = 0.0;
			[branch] if (slideT > 0.02)
			{
				const float topNow = slideY - 0.45 * spread;
				const float behindGlob = topNow - s.y;
				const float band = (1.0 - smoothstep(0.25, 0.5, abs(s.x) + (ValueNoise(float2(s.y * 3.0, seed), 1461u) - 0.5) * 0.2)) * step(-0.35, s.y);
				const float fresh = smoothstep(-0.05, 0.05, behindGlob) * (1.0 - smoothstep(0.0, 0.45, behindGlob));
				const float streak = 0.5 + 0.5 * ValueNoise(float2(s.x * 28.0, s.y * 1.5 + seed), 1463u);
				residue = band * fresh * streak;
				const float2 pg = float2(s.x * 14.0, s.y * 10.0) + seed * 4.0;
				const float2 pc = floor(pg);
				[branch] if (Hash21(pc, 1465u) < 0.12)
				{
					const float pr = 0.18 + 0.12 * Hash21(pc, 1467u);
					pearls = (1.0 - smoothstep(pr * 0.5, pr, length(frac(pg) - 0.5))) * band * smoothstep(-0.02, 0.05, behindGlob) * (1.0 - smoothstep(0.0, 0.7, behindGlob));
				}
			}
			// spatter thrown outward on impact: little silk gobs flying off the edge, smearing as they go, gone fast
			float spat = 0.0;
			[branch] if (progress > 0.012 && since < 0.07)
			{
				const float ls = length(s);
				const float fly = since * 7.0;
				const float2 s0 = s * ls / max(ls + fly, 1e-3);   // where this point was at the moment of impact
				const float2 g = s0 * 7.0 + seed * 2.0;
				const float2 cell = floor(g);
				const float2 cc = 0.5 + (float2(Hash21(cell, 1471u), Hash21(cell, 1473u)) - 0.5) * 0.5;
				const float rc = length((cell + cc - seed * 2.0) / 7.0);
				[branch] if (Hash21(cell, 1475u) < 0.22 && rc > 0.6 && rc < 1.5)
				{
					const float2 dd = (frac(g) - cc);
					const float2 radial = s0 / max(length(s0), 1e-3);
					const float elong = 1.0 + fly * 6.0;   // smeared along its flight
					const float a = dot(dd, radial) / elong;
					const float b2 = dot(dd, float2(-radial.y, radial.x));
					spat = (1.0 - smoothstep(0.08, 0.16, length(float2(a, b2)))) * exp(-since * 40.0);
				}
			}
			const float globFade = (1.0 - smoothstep(0.44, 0.58, progress)) * alpha * splatSafe;
			const float webFade = globFade * smoothstep(0.0, 0.25, impact);
			const float residueFade = alpha * splatSafe * (1.0 - smoothstep(0.45, 0.62, progress));
			const float milk = body * (0.55 + 0.6 * lump);
			const float fibres = pow(saturate(1.0 - abs(ValueNoise(Rotate(fw, 2.3) * float2(22.0, 7.0) + seed * 4.0, 1428u) * 2.0 - 1.0)), 10.0) * focus;
			const float silkAmount = saturate(milk * (0.45 + 0.4 * fibres) + filaments * saturate(body * 1.8 + 0.15) + strands);
			const float m = silkAmount * webFade;
			// the clump is a thin uneven lens: the view behind bends through it and softens (more while out of focus)
			const float2 webBend = (((fw - w) * 0.05 + (lump - 0.5) * 0.02) * body + w / max(length(w), 1e-3) * rim * 0.03) * size * (1.0 + 2.0 * slap);   // the lens shock as it hits
			const float3 behindWeb = SampleBlurred(uv + offset + float2(webBend.x / aspect, webBend.y), (2.0 + 4.0 * (1.0 - focus)) * body, range);
			const float bgLum = Luminance(behindWeb);
			const float3 silk = float3(0.88, 0.89, 0.9);
			color = lerp(color, silk * (0.6 + 0.4 * bgLum), saturate(spat * 0.7 * alpha * splatSafe));
			// residue first (it is under the glob)
			color = lerp(color, behindWeb * 0.92 + 0.05, saturate(residue * 0.32 * residueFade));
			color = lerp(color, color * 0.8, saturate(pearls * 0.4 * residueFade));
			color += silk * pow(pearls, 3.0) * 0.25 * residueFade;
			color = lerp(color, behindWeb, saturate(rim * 0.8 * webFade));   // refracted view through the rim
			color *= 1.0 - saturate((rim * 0.28 + shadowRing * 0.14) * webFade);
			// the thin line glints on the side facing the light
			const float rimFacing = saturate(dot(w / max(length(w), 1e-3), -lightDir) * 0.8 + 0.35);
			color += float3(1.0, 0.99, 0.97) * rimInner * (0.12 + 0.55 * pow(rimFacing, 3.0) * lightAmt) * webFade;
			color = lerp(color, behindWeb * 0.86 + 0.07, saturate(body * (0.58 + 0.42 * slap) * webFade));
			color *= 1.0 - saturate((strandEdge - strands) * webFade) * 0.35 * smoothstep(0.35, 0.8, bgLum);
			color = lerp(color, silk * (0.55 + 0.45 * bgLum), saturate(m * 0.9));
			color *= 1.0 - saturate(speckRim * webFade) * 0.3;
			color = lerp(color, silk * (0.8 + 0.3 * bgLum), saturate(specks * 0.85 * webFade));
			color += silk * specks * 0.15 * webFade;
			// light catching the silk: soft white glints, broad rather than pin-sharp
			const float fibreGlint = filaments * pow(saturate(1.0 - abs(dot(normalize(fw - w + 1e-4), lightDir))), 6.0) * lightAmt;
			color += float3(1.0, 0.99, 0.97) * (glints * 0.6 + speckGlint * 0.45 + fibreGlint * 0.3) * webFade;
		}
		else if (type == 3u)
		{
			// nicktheduke 2026-11g: ice hit. A packed lump of snow strikes the lens and fractures the glass: jagged cracks
			// shoot out from the impact in a fifth of a second, two broken rings join them, and the pieces between sit at
			// slightly different angles, so each one bends the view its own way and some catch the light like mirrors.
			// Frost then feathers out along the cracks while the snow lump melts to slush, and it all thaws away.
			const float r0 = length(p);
			const float ang = atan2(p.y, p.x);
			const float reach = 1.0 - pow(1.0 - saturate(progress / 0.06), 3.0);   // cracks run fast, then stop
			const float frostGrow = smoothstep(0.01, 0.25, progress);
			const float thaw = 1.0 - smoothstep(0.55, 1.0, progress);
			const float m = alpha * splatSafe * fade;
			const float2 lightDirI = float2(-0.33, -0.94);   // light from above, a little from the left

			// radial cracks: 8 rays at uneven angles, each with its own length and a jagged path
			const float kRays = 8.0;
			const float sector = 6.2831853 / kRays;
			const float s = floor((ang + 3.1415927) / sector);
			float crackLine = 0.0, crackGlint = 0.0, dMin = 9.0, sideCount = 0.0;
			[unroll] for (int j = -1; j <= 1; ++j)
			{
				const float k = s + (float)j;
				const float kw = k - kRays * floor(k / kRays);
				const float hk = Hash21(float2(kw, seed * 61.0), 1501u);
				const float theta = -3.1415927 + (k + 0.5 + 0.6 * (hk - 0.5)) * sector;
				const float len = 0.6 + 0.65 * Hash21(float2(kw, seed * 61.0), 1503u);
				const float lateral = (ValueNoise(float2(r0 * 9.0, kw * 7.3 + seed * 3.0), 1505u) - 0.5) * 0.07 +
				                      (ValueNoise(float2(r0 * 31.0, kw * 5.1 + seed * 7.0), 1507u) - 0.5) * 0.018;
				float sn, cs;
				sincos(ang - theta, sn, cs);
				const float sd = r0 * sn - lateral * saturate(r0 * 4.0);   // signed distance to the crack's path
				sideCount += sd > 0.0 ? 1.0 : 0.0;
				const float on = (cs > 0.0 ? 1.0 : 0.0) * (1.0 - smoothstep(len * reach - 0.06, len * reach, r0));
				const float width = (0.011 * (1.0 - 0.65 * saturate(r0 / len)) + 0.0025);
				const float d = abs(sd);
				const float tipR = len * reach;
				dMin = min(dMin, cs > 0.0 ? (r0 < tipR ? d : length(float2(d, r0 - tipR))) : 9.0);   // frost hugs the real crack
				crackLine = max(crackLine, (1.0 - smoothstep(width * 0.35, width, d)) * on);
				// the edge facing the light glints, the other side stays in shadow
				const float2 nrm = float2(-sin(theta), cos(theta)) * (sd > 0.0 ? 1.0 : -1.0);
				const float facing = pow(saturate(dot(nrm, -lightDirI) * 0.7 + 0.3), 2.0);
				crackGlint = max(crackGlint, exp(-Sq((d - width * 1.25) / (width * 0.45))) * facing * on);
			}
			const float rayId = s - 2.0 + sideCount;                     // which wedge between two cracks this pixel is in
			const float rayIdW = rayId - kRays * floor(rayId / kRays);
			// two broken rings, wobbling, each missing in some wedges
			// each wedge breaks its ring at its own radius, so the ring steps where it meets a crack (never a clean circle)
			const float ring1R = (0.22 + 0.06 * Hash21(float2(seed, 3.0), 1511u)) + (Hash21(float2(rayIdW, seed * 5.0), 1512u) - 0.5) * 0.07 +
			                     (ValueNoise(float2(ang * 6.0, seed * 9.0), 1513u) - 0.5) * 0.06;
			const float ring2R = (0.5 + 0.1 * Hash21(float2(seed, 5.0), 1515u)) + (Hash21(float2(rayIdW, seed * 5.0), 1516u) - 0.5) * 0.12 +
			                     (ValueNoise(float2(ang * 5.0, seed * 9.0 + 3.0), 1517u) - 0.5) * 0.09;
			const bool ring1On = Hash21(float2(rayIdW, seed * 17.0), 1519u) < 0.8;
			const bool ring2On = Hash21(float2(rayIdW, seed * 17.0 + 1.0), 1521u) < 0.6;
			const float ringIdx = (ring1On && r0 > ring1R ? 1.0 : 0.0) + (ring2On && r0 > ring2R ? 2.0 : 0.0);
			const float ringReach = smoothstep(0.0, 0.15, reach - 0.2);
			const float rw1 = 0.007, rw2 = 0.006;
			const float ring = max((ring1On ? 1.0 - smoothstep(rw1 * 0.35, rw1, abs(r0 - ring1R)) : 0.0),
			                       (ring2On ? 1.0 - smoothstep(rw2 * 0.35, rw2, abs(r0 - ring2R)) : 0.0)) * ringReach * (1.0 - smoothstep(0.85, 1.05, r0));
			crackLine = max(crackLine, ring);
			crackGlint = max(crackGlint, (ring1On ? exp(-Sq((r0 - ring1R + rw1 * 1.3) / (rw1 * 0.5))) : 0.0) * ringReach * 0.6);
			dMin = min(dMin, min(ring1On ? abs(r0 - ring1R) : 9.0, ring2On ? abs(r0 - ring2R) : 9.0) / max(ringReach, 0.05));

			// the pieces: each tilted its own way; about a third are mirror-bright
			const float2 shard = float2(rayIdW, ringIdx);
			const float h1 = Hash21(shard + seed * 13.0, 1523u);
			const float h2 = Hash21(shard + seed * 13.0, 1525u);
			const float h3 = Hash21(shard + seed * 13.0, 1527u);
			const float broken = (1.0 - smoothstep(0.55, 1.15, r0)) * smoothstep(0.0, 0.3, reach);
			const float2 tilt = (float2(h1, h2) - 0.5) * 2.0;
			const float2 bend = tilt * 0.010 * broken * (1.0 + 0.6 * (1.0 - smoothstep(0.0, 0.3, r0)));
			const float3 through = Scene.SampleLevel(SceneSampler, uv + offset + float2(bend.x / aspect, bend.y), 0).rgb / range;
			float3 ice = saturate(through) * (0.94 + 0.12 * h3) * float3(0.93, 0.97, 1.0) + float3(0.02, 0.03, 0.05);
			// mirror pieces: a flipped, paler view of the scene and a sheen sliding across the face of the piece
			const float mirror = (h3 < 0.36 ? 1.0 : 0.0) * broken;
			const float2 uvMirror = centreQ / float2(aspect, 1.0) - (uv - centreQ / float2(aspect, 1.0)) * 0.7 + tilt * 0.03;
			const float3 reflected = SampleBlurred(uvMirror, 1.5, range);
			const float sheen = pow(saturate(dot(normalize(p + 1e-4), normalize(tilt + 1e-4)) * 0.5 + 0.5), 4.0);
			const float lightHere = Luminance(through);
			ice = lerp(ice, reflected * float3(0.9, 0.97, 1.06) * 1.15 + 0.03, mirror * (0.32 + 0.5 * sheen));
			ice += float3(0.9, 0.95, 1.0) * sheen * broken * (0.04 + 0.26 * mirror) * (0.45 + 0.8 * lightHere);
			// a mirror piece is brightest along one face, like a tilted pane catching the sky
			ice += float3(0.85, 0.92, 1.0) * mirror * pow(saturate(dot(p, normalize(tilt + 1e-4)) * 2.0 + 0.4), 2.0) * 0.12 * (1.0 - smoothstep(0.3, 0.9, r0));
			color = lerp(color, ice, saturate(broken * m * thaw));

			// frost feathering out from the cracks and the impact
			const float along = r0 * 26.0;
			const float across = dMin * 70.0;
			const float fern = pow(saturate(1.0 - abs(ValueNoise(float2(along + across * 0.6, across - along * 0.3) + seed * 5.0, 1531u) * 2.0 - 1.0)), 6.0);
			const float grain = Fbm(p * 7.0 + seed * 3.0, 1533u);
			const float nearCrack = exp(-dMin / (0.025 + 0.05 * frostGrow));
			const float nearCore = 1.0 - smoothstep(0.08, 0.35 + 0.1 * frostGrow, r0);
			const float frost = saturate((max(nearCrack, nearCore * 0.8) * (0.3 + 0.7 * grain * grain) + fern * nearCrack * 0.9) * frostGrow - 0.1) * thaw;   // crystalline, not a haze
			const float3 frostBehind = SampleBlurred(uv + offset, 4.0, range);
			const float3 frosted = frostBehind * 0.6 + (0.24 + 0.3 * Luminance(frostBehind)) * float3(0.9, 0.95, 1.0);
			color = lerp(color, frosted, saturate(frost * 0.9 * m));
			// feathered crystals growing off the cracks
			const float feather = pow(saturate(1.0 - abs(ValueNoise(float2(along * 1.6 + across * 0.9, across * 1.4 - along * 0.5) + seed * 9.0, 1549u) * 2.0 - 1.0)), 9.0) * exp(-dMin / 0.05) * frostGrow * thaw;
			color += float3(0.88, 0.94, 1.0) * feather * (0.25 + 0.35 * Luminance(frostBehind)) * m;
			// glints of ice in the frost
			const float2 gc = floor(p * 24.0 + seed * 7.0);
			const float gh = Hash21(gc, 1535u);
			const float twinkle = 0.5 + 0.5 * sin(time * (3.0 + 5.0 * gh) + gh * 40.0);
			const float glit = (gh < 0.08 ? 1.0 : 0.0) * (1.0 - smoothstep(0.0, 0.18, length(frac(p * 24.0 + seed * 7.0) - 0.5))) * twinkle;
			color += float3(0.92, 0.97, 1.0) * glit * frost * 0.8 * m;

			// the cracks themselves: a dark hairline with a bright lit edge
			const float crackFade = m * (1.0 - smoothstep(0.75, 1.0, progress));
			color *= 1.0 - saturate(crackLine * 0.55 * crackFade);
			color += float3(0.95, 0.98, 1.0) * crackGlint * 0.75 * crackFade;
			// crushed glass where it struck: a tight web of fine white fractures
			const float crush = pow(saturate(1.0 - abs(ValueNoise(Rotate(p, seed * 6.0) * float2(34.0, 9.0) + seed, 1537u) * 2.0 - 1.0)), 10.0) +
			                    pow(saturate(1.0 - abs(ValueNoise(Rotate(p, seed * 6.0 + 1.3) * float2(30.0, 8.0) + seed, 1539u) * 2.0 - 1.0)), 10.0);
			color += float3(0.85, 0.92, 1.0) * saturate(crush) * (1.0 - smoothstep(0.05, 0.2, r0)) * reach * crackFade * 0.5;

			// the snow lump: packed, grainy, melting to slush; loose grains thrown off on impact
			const float melt = smoothstep(0.15, 0.75, progress);
			const float2 rim = float2(cos(ang), sin(ang));
			const float lumpEdge = 0.13 * (1.0 - 0.45 * melt) * (0.4 + 1.2 * Fbm(rim * 1.8 + seed * 11.0, 1541u));   // a ragged clump, not a disc
			const float lump = (1.0 - smoothstep(lumpEdge - 0.03, lumpEdge, r0)) * (0.75 + 0.25 * Fbm(p * 11.0 + seed, 1547u));
			const float snowGrain = Fbm(p * 22.0 + seed * 4.0, 1543u);
			const float3 slushBehind = SampleBlurred(uv + offset, 6.0, range);
			const float snowLight = 0.62 + 0.5 * Luminance(slushBehind);   // lit by the scene, never glowing in a dark room
			const float3 snow = float3(0.9, 0.93, 0.97) * (0.62 + 0.38 * snowGrain) * snowLight * (0.85 + 0.15 * saturate(-p.y * 6.0 + 0.5));
			const float3 slush = slushBehind * 0.85 + 0.1;
			color = lerp(color, lerp(snow, slush, melt), saturate(lump * 1.25 * (1.0 - 0.6 * melt) * m));
			// small flecks of snow stuck to the glass around the strike, thinning and melting with the lump
			float flecks = 0.0;
			[unroll] for (int fl = 0; fl < 2; ++fl)
			{
				const float fs = fl == 0 ? 16.0 : 30.0;
				const float2 fp = p * fs + seed * (5.0 + fl);
				const float2 fc = floor(fp);
				const float fh = Hash21(fc, 1551u + fl);
				const float2 fo = float2(Hash21(fc, 1553u + fl), Hash21(fc, 1555u + fl)) * 0.5 + 0.25;
				const float fr = (0.12 + 0.18 * Hash21(fc, 1557u + fl)) * (1.0 - 0.5 * melt);
				const float density = (1.0 - smoothstep(0.1, 0.75, r0)) * (fl == 0 ? 0.35 : 0.5);
				flecks = max(flecks, (fh < density ? 1.0 : 0.0) * (1.0 - smoothstep(fr * 0.6, fr, length(frac(fp) - fo))));
			}
			color = lerp(color, snow, saturate(flecks * smoothstep(0.0, 0.02, progress) * (1.0 - 0.5 * melt) * m * 0.85));
			const float flyT = saturate(progress * 9.0);
			const float2 gp = p * (1.0 / (0.35 + 0.9 * flyT));
			const float2 gcell = floor(gp * 9.0 + seed * 3.0);
			const float gHash = Hash21(gcell, 1545u);
			const float grainDot = (gHash < 0.18 ? 1.0 : 0.0) * (1.0 - smoothstep(0.12, 0.3, length(frac(gp * 9.0 + seed * 3.0) - 0.5)));
			color = lerp(color, snow, saturate(grainDot * (1.0 - smoothstep(0.2, 0.9, flyT + progress)) * (1.0 - smoothstep(0.3, 0.8, length(gp))) * m));
		}
		else if (type == 4u)
		{
			// Fire hit or fire spit: an ember burst flying outward.
			color += Ember(p, seed, progress) * 1.6 * alpha * splatSafe;
			color += float3(1.0, 0.5, 0.15) * exp(-dot(p, p) * 6.0) * (1.0 - smoothstep(0.0, 0.35, progress)) * 0.12 * alpha * splatSafe;
		}
		else if (type == 5u)
		{
			// Explosion grit (nicktheduke 2026-11-03): fine debris thrown onto the glass, in three sizes: a dusting of sand,
			// small grit and the odd chip. Each piece is irregular (off-centre, turned, stretched, a rough angular edge),
			// takes its colour from the scene behind it darkened toward earth rather than black, and catches a thin rim of
			// light on one side, so it reads as dirt on the lens and never as a dead pixel. Sparser toward the edge.
			const float spread = 1.0 - smoothstep(0.45, 1.05, length(p));
			float occ = 0.0;
			float rim = 0.0;
			[unroll] for (uint s = 0; s < 3; ++s)
			{
				const float sc = s == 0 ? 34.0 : (s == 1 ? 18.0 : 9.0);
				const float keep = (s == 0 ? 0.5 : (s == 1 ? 0.22 : 0.06)) * (0.35 + 0.65 * spread);
				const float2 g = p * sc + seed * (11.0 + 7.0 * s);
				const float2 cell = floor(g);
				const uint salt = 1421u + s * 16u;
				[branch] if (Hash21(cell, salt) < keep)
				{
					const float2 j = (float2(Hash21(cell, salt + 1u), Hash21(cell, salt + 2u)) - 0.5) * 0.45;
					const float2 fu = frac(g) - 0.5 - j;
					const float ang = Hash21(cell, salt + 3u) * 6.2831853;
					const float cs = cos(ang), sn = sin(ang);
					float2 f = float2(fu.x * cs - fu.y * sn, fu.x * sn + fu.y * cs);
					f.x *= 1.0 + 1.3 * Hash21(cell, salt + 4u);
					const float rad = (s == 0 ? 0.24 : (s == 1 ? 0.30 : 0.36)) * (0.6 + 0.8 * Hash21(cell, salt + 5u));
					const float d = lerp(length(f), max(abs(f.x), abs(f.y)) * 1.15, 0.55) + (ValueNoise(g * 5.0 + s, salt + 6u) - 0.5) * rad * 0.6;
					const float body = 1.0 - smoothstep(rad * 0.55, rad, d);
					const float core = 1.0 - smoothstep(rad * 0.2, rad * 0.65, d);
					const float strength = s == 0 ? 0.75 : (s == 1 ? 0.9 : 0.95);
					occ = max(occ, lerp(body * 0.6, body, core) * strength);
					rim = max(rim, saturate(body - core) * saturate(-fu.y / max(rad, 1e-3) + 0.2));
				}
			}
			const float m = spread * fade * alpha * splatSafe;
			const float3 grain = color * 0.42 + float3(0.30, 0.24, 0.17) * 0.22;   // the scene behind, darkened toward earth
			color = lerp(color, grain, saturate(occ * m));
			color += rim * m * 0.4 * (0.25 + Luminance(color));
		}
		else if (type == 6u)
		{
			// Disintegrate ash puff: a grey cloud that thins out, with flakes.
			const float n = Fbm(p * 2.0 + seed * 7.0 + float2(0.0, -progress), 1431u);
			const float cloud = saturate((1.0 - smoothstep(0.1, 1.0, length(p) / (0.6 + progress * 0.5))) * (0.3 + n));
			const float2 g = p * 12.0 + seed * 5.0 + float2(0.0, progress * 2.0);
			const float flake = Hash21(floor(g), 1433u) < 0.12 ? 1.0 - smoothstep(0.1, 0.3, length(frac(g) - 0.5)) : 0.0;
			const float m = max(cloud * 0.5, flake * (1.0 - smoothstep(0.5, 1.1, length(p)))) * fade * alpha * splatSafe;
			color = lerp(color, float3(0.42, 0.41, 0.39) * (0.55 + 0.45 * Luminance(color)), saturate(m * 0.7));
		}
		else if (type == 7u)
		{
			// Steam blast: a soft local fog on the glass with fine beads.
			const float n = Fbm(p * 1.5 + seed * 3.0, 1441u);
			const float m = saturate((1.0 - smoothstep(0.2, 1.0, length(p))) * (0.5 + n)) * fade * alpha * splatSafe;
			const float3 soft = SampleBlurred(uv, 10.0 * m, range);
			color = lerp(color, soft * 1.05 + 0.05, saturate(m * 0.65));
		}
	}

	color += lerp(saturate(glossTint * 2.5), 1.0.xxx, 0.4) * glossValue * 0.8;

	// ---- Impact flash and slash streak at the hit point (small, short, never full screen) ----
	[branch] if (Flash.w > 0.001)
	{
		const float2 hit = float2(Flash.x * aspect, Flash.y);
		const float2 d = q - hit;
		const float envelope = exp(-Flash.z * 24.0);
		const float glow = exp(-dot(d, d) * 260.0) * envelope * Flash.w;
		const float2 split = d * 0.06 * glow;
		color.r = lerp(color.r, saturate(Scene.SampleLevel(SceneSampler, uv + offset + split, 0).r / range), saturate(glow));
		color += color * glow * 0.5 + glow * 0.1;
		[branch] if (Slash.w > 0.001)
		{
			// nicktheduke 2026-11: a heavy blade cutting through the air across the view, not a paper cut. A wide arc of
			// smeared, slightly brightened air sweeps across in about a seventh of a second; its width breathes along
			// its length, the leading edge is soft-bright (no hard line) and a turbulent wake trails behind it.
			const float2 dir = normalize(Slash.xy + 1e-5);
			const float2 perp = float2(-dir.y, dir.x);
			const float along = dot(d, dir);
			const float across = dot(d, perp) - along * along * 0.75;  // a broad arc
			const float reach = 0.82;   // nicktheduke 2026-11b: longer, so the cut reads across the view
			const float head = lerp(-reach, reach, smoothstep(0.0, 1.0, saturate(Slash.z * 5.5)));
			const float behindTip = head - along;
			const float drawn = smoothstep(-0.03, 0.02, behindTip) * (1.0 - smoothstep(reach * 0.7, reach * 1.6, behindTip));
			const float taper = 1.0 - smoothstep(reach * 0.55, reach, abs(along));
			const float breathe = 0.75 + 0.5 * ValueNoise(float2(along * 9.0, Slash.x * 13.0), 1451u);
			const float width = 0.062 * (0.3 + taper) * breathe;
			const float band = exp(-(across * across) / (width * width)) * taper * drawn;
			const float lead = exp(-Sq((across + width * 0.45) / (width * 0.35))) * taper * drawn * smoothstep(0.0, 0.12, behindTip) * (1.0 - smoothstep(0.08, 0.3, behindTip));
			const float slashEnvelope = exp(-max(Slash.z - 0.15, 0.0) * 8.0) * Slash.w;
			[branch] if (band * slashEnvelope > 0.002)
			{
				// smear the view along the blade's path, with a little turbulence in the wake
				const float turb = (ValueNoise(float2(along * 14.0, across * 30.0 + Slash.z * 20.0), 1453u) - 0.5) * 0.03 * band;
				float3 smear = 0.0;
				[loop] for (uint s = 0; s < 8; ++s)
				{
					const float2 o = dir * ((s / 7.0) - 0.5) * 0.19 * band + perp * turb;
					smear += Scene.SampleLevel(SceneSampler, uv + offset + float2(o.x / aspect, o.y), 0).rgb;
				}
				smear = saturate(smear / 8.0 / range);
				color = lerp(color, smear * 1.18 + 0.04, saturate(band * slashEnvelope * 0.95));
				color += float3(0.95, 0.97, 1.0) * lead * slashEnvelope * 0.6 * (0.6 + 0.4 * Luminance(smear));
				// a faint second streak just behind the first (the back of the blade), giving the cut definition
				const float back = exp(-Sq((across - width * 0.35) / (width * 0.18))) * taper * drawn * smoothstep(0.02, 0.15, behindTip);
				color += float3(0.9, 0.93, 1.0) * back * slashEnvelope * 0.18;
				// a small hot glint riding the tip while it travels
				const float2 tipD = float2(along - head, across + width * 0.45) / float2(0.05, 0.012);
				color += float3(1.0, 0.98, 0.95) * exp(-dot(tipD, tipD)) * slashEnvelope * saturate(1.0 - Slash.z * 6.0) * 0.6;
				// the trailing side of the cut: air pushed aside, a soft darker wake that gives the streak body on bright scenes
				const float wake = exp(-Sq((across - width * 0.7) / (width * 0.55))) * taper * drawn;
				color *= 1.0 - saturate(wake * slashEnvelope * 0.28);
			}
		}
	}

	return float4(saturate(color) * range, source.a);
}
#endif
