// Data-dependent loops the frontend cannot unroll: break, continue, nesting.
uniform vec3 iResolution;
uniform float iGlobalTime;
void main() {
    vec2 uv = gl_FragCoord.xy / iResolution.xy;
    int one = int(iResolution.z);
    vec2 c = uv * 3.0 - vec2(2.0, 1.5);
    vec2 z = vec2(0.0);
    int n = 0;
    for (int i = 0; i < 40 * one; i++) {
        z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
        if (dot(z, z) > 4.0) break;
        n++;
    }
    float acc = 0.0;
    int px = int(gl_FragCoord.x);
    for (int j = 0; j < 6 * one; j++) {
        if ((j & 1) == (px & 1)) continue;
        for (int k = 0; k < 5 * one; k++) {
            if (k == j) continue;
            if (float(k) * uv.y > 1.2) break;
            acc += float(j * 4 + k) * 0.01;
        }
        if (acc > 0.9 + iGlobalTime * 0.001) break;
    }
    gl_FragColor = vec4(float(n) / 40.0, acc, fract(z.x * 0.25), 1.0);
}
