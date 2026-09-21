#version 450

layout(location = 0) out vec2 uv;

void main() {
    const vec2 positions[6] = vec2[](
        vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0)
    );
    const vec4 localPosition = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    gl_Position = localPosition;
    uv = positions[gl_VertexIndex] * 0.5 + 0.5;
}
