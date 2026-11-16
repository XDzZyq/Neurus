#version 450
// ---------------------------------------------------------------------------
// Shadow Depth Fragment Shader — Point Light Cubemap Depth Pass
//
// Computes linear Euclidean distance from the point light to the fragment,
// normalised by far_plane, and writes it to gl_FragDepth.
//
// The fragment shader runs solely to set gl_FragDepth; no colour attachments
// are written (depth-only dynamic rendering pass).
//
// lightWorldPos and farPlane come from push constants (per-light).
// ---------------------------------------------------------------------------

layout(location = 0) in vec3 fragWorldPos;

layout(push_constant) uniform PushConstants
{
	layout(offset = 0)  vec3 lightWorldPos;
	layout(offset = 12) float farPlane;
};

void main()
{
	float dist = length(fragWorldPos - lightWorldPos);
	gl_FragDepth = dist / farPlane;
}
