// Integer ALU coverage: shifts, min/max, negate, constants on either side.
uniform vec3 iResolution;
uniform float iGlobalTime;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int a = p.x * 37 - 1000;
    int b = p.y * 91 - 2000;
    uint ua = uint(p.x) * 2654435761u;
    uint ub = uint(p.y) * 40503u + 7u;
    int s1 = a >> 3, s2 = a >> (p.y & 7), s3 = b << (p.x & 3);
    uint u1 = ua >> 7u, u2 = ua >> (ub & 15u), u3 = ub << 5u;
    int mn = min(a, b), mx = max(a, 17);
    uint umn = min(ua, ub), umx = max(ua, 1000u);
    int ng = -a, sb = 5 - b, sb2 = b - 5, x = a ^ b, nt = ~a;
    int an = a & 0xff, orr = b | 0x100;
    bool c1 = 3 < a, c2 = a <= b, c3 = ua >= 77u, c4 = 5.0 > float(p.x);
    bool c5 = a != b, c6 = a == 37;
    int sel1 = c1 ? 7 : a, sel2 = c2 ? b : 9, sel3 = c3 ? 1 : 2;
    int sel4 = ((c4 || c5) && !c6) ? a : b;
    uint h = uint(s1) ^ uint(s2) * 3u ^ uint(s3) * 5u ^ u1 * 7u ^ u2 * 11u ^
             u3 * 13u ^ uint(mn) ^ uint(mx) * 17u ^ umn ^ umx * 19u ^ uint(ng) ^
             uint(sb) * 23u ^ uint(sb2) ^ uint(x) * 29u ^ uint(nt) ^
             uint(an) * 31u ^ uint(orr) ^ uint(sel1) * 37u ^ uint(sel2) ^
             uint(sel3) * 41u ^ uint(sel4);
    gl_FragColor = vec4(float(h & 255u), float((h >> 8u) & 255u),
                        float((h >> 16u) & 255u), 255.0) / 255.0;
}
