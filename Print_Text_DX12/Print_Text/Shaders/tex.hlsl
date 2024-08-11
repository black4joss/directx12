Texture2D    gDiffuseMap : register(t0);

SamplerState gsamPointWrap  : register(s0);
SamplerState gsamPointClamp  : register(s1);
SamplerState gsamLinearWrap  : register(s2);
SamplerState gsamLinearClamp  : register(s3);
SamplerState gsamAnisotropicWrap  : register(s4);
SamplerState gsamAnisotropicClamp  : register(s5);


cbuffer cbPerObject : register(b0)
{
	float4x4 gWorldViewProj;
	float4x4 gWorldView;
};

struct VertexIn
{
	float3 PosL  : POSITION;
    float2 Tex : TEXCOORD;
};

struct VertexOut
{
	float4 PosH  : SV_POSITION;
    float2 Tex : TEXCOORD;
};


VertexOut VS(VertexIn vin)
{
	
	VertexOut vout;

	float2 VertPos = vin.PosL.xy - float2(400.0f, 300.0f);

	VertPos.y = -1.0f * VertPos.y;

	VertPos = VertPos / float2(400.0f, 300.0f);

	vout.PosH = float4(VertPos.x, VertPos.y, 0.0f ,1.0);

	vout.Tex = vin.Tex;
    
    return vout;
}


float4 PS(VertexOut pin) : SV_Target
{
	float4 ResColor =  gDiffuseMap.Sample(gsamLinearWrap, pin.Tex);

	return ResColor;
}


