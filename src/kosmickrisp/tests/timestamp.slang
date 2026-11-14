[shader("vertex")]
float4 vertex_main(uint vertex_id : SV_VertexID) : SV_Position {
    float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(positions[vertex_id], 0, 1);
}
[shader("fragment")]
float4 fragment_main() : SV_Target0 {
    return float4(0.25, 0.5, 0.75, 1);
}
