#version 450
/* Each instance covers one pixel center of a 512x512 target, chosen by gl_InstanceIndex, which includes the indirect
 * command's firstInstance. */
void main()
{
   uint id = uint(gl_InstanceIndex);
   vec2 size = vec2(512.0, 512.0);
   vec2 center = (vec2(float(id % 512u), float(id / 512u)) + 0.5) / size * 2.0 - 1.0;
   vec2 corner[3] = vec2[](vec2(-0.4, -0.4), vec2(0.8, -0.4), vec2(-0.4, 0.8));
   gl_Position = vec4(center + corner[gl_VertexIndex] * 2.0 / size, 0.0, 1.0);
}
