Texture2D    gDiffuseMap : register(t0);

SamplerState gsamPointWrap  : register(s0);
SamplerState gsamPointClamp  : register(s1);
SamplerState gsamLinearWrap  : register(s2);
SamplerState gsamLinearClamp  : register(s3);
SamplerState gsamAnisotropicWrap  : register(s4);
SamplerState gsamAnisotropicClamp  : register(s5);


cbuffer cbPerObject : register(b0)
{
	float4x4 gWorld; 
};

cbuffer cbPass : register(b1)
{
	
	float3 gCamPos;
	float pad0;
    float4x4 gView;
    float4x4 gViewProj; 



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
    float fog_val : TEXCOORD1;
};


struct FogBulb
{
	float3 WorldPos;
	float3 vec;
	float3 pos;
	float  rad;
	float  sqrad;
	float  inv_sqrad;
	float  dist;
	float  density;
	bool   inRange;
};



float Update_Fog(float3 PosL)
{

	FogBulb bulb;

	bulb.inRange = true;
	bulb.WorldPos = float3(46433.0f, 6376.0f, 48650.0f);
	bulb.rad = 4128.0f;
	bulb.sqrad = bulb.rad * bulb.rad;
	bulb.inv_sqrad = 1.0f / bulb.sqrad;
	bulb.density = 128.0f;

	//вектор от камеры к источнику тумана
	bulb.vec = bulb.WorldPos - gCamPos;

	bulb.vec = mul(float4(bulb.vec, 0.0f), gView).xyz;
	
	bulb.pos = bulb.vec;

	//нормализация
	bulb.vec = normalize(bulb.vec);

	//позиция края сферы тумана в направлении луча
	bulb.vec = bulb.rad * bulb.vec + bulb.pos;

	//дистанция от камеры до центра
	bulb.dist = length(bulb.pos);



	float3 pos, dP, dV;
	float val, val2;
	float lVal;
	float3 v;
	float prevAlpha = 255.0f;

	//преобразование вершины в видовое пространство (камера → мир)
	float3 viewPos = mul(float4(PosL, 1.0f), gView).xyz;

	v.x = viewPos.x;
	v.y = viewPos.y;
	v.z = viewPos.z;
	
	

if (bulb.inRange)
{
    
    pos.x = v.x;
    pos.y = v.y;
    pos.z = v.z;

    if (bulb.pos.z < pos.z)
    {
        pos.x *= bulb.dist * (1.0f / v.z);
        pos.y *= bulb.dist * (1.0f / v.z);
        pos.z *= bulb.dist * (1.0f / v.z);
    }

    
    dP.x = pos.x - bulb.pos.x;
    dP.y = pos.y - bulb.pos.y;
    dP.z = pos.z - bulb.pos.z;

    
    dV.x = bulb.vec.x - bulb.pos.x;
    dV.y = bulb.vec.y - bulb.pos.y;
    dV.z = bulb.vec.z - bulb.pos.z;

    val = dV.x * dV.x + dV.y * dV.y + dV.z * dV.z;

    if (val != 0.0f)
    {
        val2 = (dP.x * dV.x + dP.y * dV.y + dP.z * dV.z) / val;

        if (val2 >= -1.0f)
        {
            if (val2 > 0.0f)
            {
                dP.x -= val2 * dV.x;
                dP.y -= val2 * dV.y;
                dP.z -= val2 * dV.z;
            }

            val = dP.x * dP.x + dP.y * dP.y + dP.z * dP.z;

            if (val != 0.0f && val < bulb.sqrad)
            {

				val *= bulb.inv_sqrad * bulb.density;

				lVal = val + prevAlpha - bulb.density;

					if (lVal < 0.0f)
						lVal = 0.0f;

					if (lVal > 255.0f)
						lVal = 255.0f;

					lVal = 255.0f - lVal;

					//переводим обратно в диапазон 0..1
					return saturate(lVal / 255.0f);
            }
        }
    }

}

return 0.0f;

	
	
}



VertexOut VS(VertexIn vin)
{
	VertexOut vout;

	vout.fog_val = Update_Fog(vin.PosL);
	
	float4 Pos = mul(float4(vin.PosL, 1.0f), gWorld);

	// Transform to homogeneous clip space.
	vout.PosH = mul(Pos, gViewProj);

	vout.Tex = vin.Tex;

	
    
    return vout;
}


float4 PS(VertexOut pin) : SV_Target
{
	//get texel color
	float4 ResColor =  gDiffuseMap.Sample(gsamLinearWrap, pin.Tex);

	//get fog valule
	float FogVal = pin.fog_val;

	//fog color
	float fog_r = 0.0f;
	float fog_g = 223.0f / 255.0f;
	float fog_b = 191.0f / 255.0f;

	/*
	//вариант #1
	//fog value inverse
	float InvFogVal = 1.0f - pin.fog_val;

	//calculating alpha blending value for fog color and texel color
	float4 SrcAlpha = float4(fog_r, fog_g * FogVal, fog_b * FogVal, 1.0f);

	//calculating alpha blending value for fog color and texel color
	float4 InvSrcAlpha = float4(ResColor.x * InvFogVal,
			ResColor.y * InvFogVal, ResColor.z * InvFogVal, 1.0f);

	return SrcAlpha + InvSrcAlpha;
	*/
	
	//вариант #2
	float4 fogColor = float4(fog_r, fog_g, fog_b, 1.0f);
	float4 finalColor = lerp(ResColor, fogColor, FogVal);
	return finalColor;


	
}


