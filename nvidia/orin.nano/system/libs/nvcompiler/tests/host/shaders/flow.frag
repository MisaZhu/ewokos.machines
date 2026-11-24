// return from inside a loop, a phi swap cycle, continue from a nested if.
uniform vec3 iResolution;
uniform float iGlobalTime;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int n = int(iResolution.z) * 12;
    float a = 0.1, b = 0.7, t;
    for (int i = 0; i < n; i++) {
        t = a; a = b; b = t;
        if (i > (p.x & 7)) {
            a += 0.05;
            if ((p.y & 1) == 0) continue;
            b -= 0.01;
        }
        if (i == (p.y & 15) + 3) {
            gl_FragColor = vec4(a, b, 0.25, 1.0);
            return;
        }
        b *= 0.9;
    }
    if (p.x > p.y) {
        if ((p.x & 3) == 1) a = 1.0 - a; else b = 1.0 - b;
    }
    gl_FragColor = vec4(a, b, 1.0, 1.0);
}
