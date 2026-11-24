// Float ALU coverage: rounding, pow/div/sqrt, min/max/clamp, mix/step, swizzles,
// source modifiers and multiply-add fusion.
uniform vec3 iResolution;
uniform float iGlobalTime;
void main() {
    vec2 uv = gl_FragCoord.xy / iResolution.xy;
    vec3 v = vec3(uv * 8.0 - 4.0, iGlobalTime * 0.37);
    float fl = floor(v.x) + ceil(v.y) * 0.5 + fract(v.x * 1.7) + trunc(v.y * 2.3);
    float pw = pow(abs(v.x) + 0.25, 1.7) + sqrt(abs(v.y)) + inversesqrt(uv.x + 0.1);
    float dv = v.x / (abs(v.y) + 0.5) + 3.0 / (uv.x + 1.0) + (uv.y - 0.25) / 7.0;
    float ex = exp2(v.x * 0.25) + log2(uv.y + 1.5) + exp(-uv.x) + log(uv.y + 2.0);
    float mm = min(v.x, v.y) + max(v.x, 0.3) + clamp(v.y, -1.0, 1.0) + abs(v.x) - (-v.y);
    float mx = mix(v.x, v.y, uv.x) + step(0.5, uv.y) + smoothstep(0.2, 0.8, uv.x);
    vec3 w = v.zxy * vec3(1.0, -2.0, 0.5) + v.yyx;
    float ln = length(w) + dot(w.xz, v.yx) + normalize(w + vec3(0.1, 0.2, 5.0)).z;
    float tr = sin(v.x * 3.0) * cos(v.y * 2.0 + v.z) + sin(fl);
    // source modifiers and multiply-add shapes: |a|*b, a*b-c, c-a*b, c-|a|, -a*b+c
    float a = v.x * v.y, b2 = uv.x - uv.y;
    float fm = abs(a) * b2 + abs(b2) * 3.0 + (v.x * b2 - a) + (b2 - v.y * a)
             + (2.5 - abs(a)) + (-b2) * v.x + (1.5 - b2) + abs(a) * abs(b2)
             - abs(v.y) + sin(a) * cos(a) + (-a - b2)
             + exp2(abs(uv.y - 0.5) * 0.3) + sqrt(abs(b2 - 0.1) * uv.x);
    // floor/fract: a provably small argument, an unknown one, and one past 2^22
    float big = v.x * 3.0e6 + v.y;
    float fr = fract(sin(a) * 43758.5453) + fract(floor(big) * 0.37) + fract(big)
             + floor(-uv.x * 0.001) + fract(cos(b2) * 917.3 - 0.5) + floor(b2 * 1.0e-30);
    // whole exponents become multiplies: even and odd, negative base, and one left to EX2/LG2
    float pi = pow(v.x, 2.0) + pow(v.y, 3.0) + pow(uv.x + 0.2, 5.0) + pow(uv.y + 0.9, 60.0) * 1.0e-9
             + pow(uv.x + 0.5, 1.0) + pow(uv.y + 0.3, 200.0) * 1.0e-12;
    vec3 col = vec3(fract(fl * 0.173 + pw * 0.31 + dv * 0.07 + fr * 0.29 + pi * 0.23),
                    fract(ex * 0.21 + mm * 0.13 + mx * 0.4 + fm * 0.37),
                    fract(ln * 0.11 + tr * 0.5 + 0.5));
    gl_FragColor = vec4(col, 1.0);
}
