// According to Nvidia's "Understanding Structured Buffer Performance" guide
// one should aim for structures with sizes divisible by 128 bits (16 bytes).
// This prevents elements from spanning cache lines.
struct Cell
{
    uint glyphPos;
    uint flags;
    uint2 color;
};

cbuffer ConstantBuffer : register(b0)
{
    uint2 cellSize;
    uint2 cellCount;
    uint backgroundColor;
};
StructuredBuffer<Cell> cells : register(t0);
Texture2D<float4> glyphs : register(t1);

float3 decodeRGB(uint i)
{
    uint r = i & 0xff;
    uint g = (i >> 8) & 0xff;
    uint b = i >> 16;
    return float3(r, g, b) / 255.0;
}

uint2 decodeU16x2(uint i)
{
    return uint2(i & 0xffff, i >> 16);
}

float4 main(float4 pos: SV_Position): SV_Target
{
    uint2 cellIndex = pos.xy / cellSize;
    uint2 cellPos = pos.xy % cellSize;

    Cell cell = cells[cellIndex.y * cellCount.x + cellIndex.x];

    uint2 glyphPos = decodeU16x2(cell.glyphPos);
    uint2 pixelPos = glyphPos + cellPos;
    float3 alpha = glyphs[pixelPos].xyz;

    float3 colorFg = decodeRGB(cell.color.x);
    float3 colorBg = decodeRGB(cell.color.y);
    float3 color = lerp(colorBg, colorFg, alpha);

    if (cell.flags & 1)
    {
        color = abs(glyphs[cellPos].xyz - color);
    }

    return float4(color, 1);
}
