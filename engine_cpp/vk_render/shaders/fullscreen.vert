#version 450

// fullscreen.vert — generates a fullscreen triangle from gl_VertexIndex alone.
// No vertex buffer needed. Dispatch with vkCmdDraw(cmd, 3, 1, 0, 0).

layout(location = 0) out vec2 v_uv;

void main() {
    // Classic fullscreen triangle trick:
    // index 0 → (-1,-1), 1 → (3,-1), 2 → (-1, 3)
    vec2 pos = vec2(
        (gl_VertexIndex == 1) ? 3.0 : -1.0,
        (gl_VertexIndex == 2) ? 3.0 : -1.0
    );
    // SpriteBatch uses a negative-height viewport (Y-flip trick) so the scene
    // image is stored top-to-bottom correctly. Sample it straight — no UV flip.
    v_uv = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
