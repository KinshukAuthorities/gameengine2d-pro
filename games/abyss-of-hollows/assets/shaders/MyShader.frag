#version 450
layout(location=0) in vec2 frag_uv;
layout(location=1) in vec4 frag_color;
layout(location=0) out vec4 out_color;
layout(set=0,binding=0) uniform sampler2D albedo_sampler;
layout(push_constant) uniform PushConstants { vec2 viewport_size; float alpha_cutoff; int use_texture; } pc;
void main() {
  vec4 graph_color = texture(albedo_sampler, (vec4(frag_uv, 0.0, 1.0)).xy);
  graph_color *= frag_color;
  graph_color.a *= (vec4(1.0)).r;
  if (pc.alpha_cutoff >= 0.0 && graph_color.a < pc.alpha_cutoff) discard;
  out_color = graph_color;
}
