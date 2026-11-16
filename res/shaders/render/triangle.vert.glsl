#version 460

const vec3 positions[3] = vec3[](
	vec3( 0.0, -0.5, 0.0),
	vec3( 0.5,  0.5, 0.0),
	vec3(-0.5,  0.5, 0.0)
);

const vec3 colors[3] = vec3[](
	vec3(1.0, 0.0, 0.0),  // Red
	vec3(0.0, 1.0, 0.0),  // Green
	vec3(0.0, 0.0, 1.0)   // Blue
);

layout(location = 0) out vec3 fragColor;

void main()
{
	vec3 pos = positions[gl_VertexIndex];
	gl_Position = vec4(pos, 1.0);
	fragColor = colors[gl_VertexIndex];
}
