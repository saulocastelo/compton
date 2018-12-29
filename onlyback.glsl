uniform float opacity;
uniform bool invert_color;
uniform sampler2D tex;
void main() {
  vec4 c = texture2D(tex, gl_TexCoord[0]);
  float eps = 0.018f;
  float eps1 = 0.0f;
  if (invert_color)
     c = vec4(vec3(c.a, c.a, c.a) - vec3(c), c.a);
  if(
  (c.r > 0.1+eps1-eps && c.r < 0.1+eps1+eps &&
   c.g > 0.1+eps1-eps && c.g < 0.1+eps1+eps &&
   c.b > 0.1+eps1-eps && c.b < 0.1+eps1+eps) ||
  (c.r > 0.03846153846153846154+eps1-eps && c.r < 0.03846153846153846154+eps1+eps &&
   c.g > 0.03846153846153846154+eps1-eps && c.g < 0.03846153846153846154+eps1+eps &&
   c.b > 0.03846153846153846154+eps1-eps && c.b < 0.03846153846153846154+eps1+eps)
  ) { c *= opacity; } 
  else {  }
  gl_FragColor = c;
}
