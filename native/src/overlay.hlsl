// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// Port of the overlay shader in skse/src/Overlay.cpp to D3D12 (root constants instead of a cbuffer
// object) plus HDR output encodings. Compiled at build time by fxc (see native/CMakeLists.txt).
// See THIRD-PARTY-NOTICES.md.
//
// Root signature (built in overlay.cpp): b0 = 8 root constants, t0 = descriptor table, s0 = static
// point/clamp sampler.

cbuffer Params : register(b0)
{
	float2 cursor;      // back buffer pixels
	float2 viewport;    // back buffer size
	float  cursorOn;
	float  flipY;       // 1 = overlay rows are bottom-up (Minecraft's readback)
	float  mode;        // 0 = SDR (write as is), 1 = scRGB (linear FP16), 2 = HDR10 (PQ, BT.2020)
	float  paperWhite;  // nits of overlay white in HDR modes
};

Texture2D<float4> overlay : register(t0);
SamplerState      samp : register(s0);

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
	VSOut  o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}

float3 SrgbToLinear(float3 c)
{
	float3 lo = c / 12.92;
	float3 hi = pow((c + 0.055) / 1.055, 2.4);
	return lerp(hi, lo, step(c, 0.04045));
}

float3 PqEncode(float3 nits)
{
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	float3      y = pow(saturate(nits / 10000.0), m1);
	return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

static const float3x3 kRec709ToRec2020 = {
	0.6274040, 0.3292820, 0.0433136,
	0.0690970, 0.9195400, 0.0113612,
	0.0163916, 0.0880132, 0.8955950
};

// In: premultiplied, sRGB-encoded (what Minecraft wrote). Out: premultiplied in the back buffer's
// encoding, for the ONE / INV_SRC_ALPHA blend.
float4 EncodeForTarget(float4 c)
{
	if (mode < 0.5 || c.a <= 0.0)
		return c;
	float3 straight = saturate(c.rgb / c.a);
	float3 lin = SrgbToLinear(straight);
	float3 o;
	if (mode < 1.5)
		o = lin * (paperWhite / 80.0);  // scRGB: 1.0 = 80 nits
	else
		o = PqEncode(mul(kRec709ToRec2020, lin) * paperWhite);
	return float4(o * c.a, c.a);
}

float4 PSMain(VSOut i) : SV_Target
{
	float2 uv = i.uv;
	if (flipY > 0.5)
		uv.y = 1.0 - uv.y;
	float4 c = overlay.Sample(samp, uv);  // premultiplied alpha straight from Minecraft
	if (cursorOn > 0.5)
	{
		float2 p = i.pos.xy - cursor;
		if (p.x >= 0 && p.y >= 0 && p.y < 18 && p.x <= p.y * 0.6)
		{
			bool edge = p.x < 1.5 || p.x > p.y * 0.6 - 1.5 || p.y > 16.5;
			c = float4(edge ? float3(0, 0, 0) : float3(1, 1, 1), 1);
		}
	}
	return EncodeForTarget(c);
}
