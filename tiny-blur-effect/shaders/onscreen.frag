#version 140

uniform sampler2D texUnit;
uniform mat4 colorMatrix;
uniform float offset;
uniform vec2 halfpixel;
uniform sampler2D originalTexture;
uniform int apertureCount;
uniform int apertureCircle;
uniform int apertureInverted;
uniform vec2 rectSize;
uniform float modulation;
uniform vec4 apertures[16];
uniform float zoomFactors[16];
uniform int outlineCount;      // > 0: apertures are this hand-drawn polygon, not circles or boxes
uniform vec2 outline[64];      // polygon points in 0..1 of the aperture box, y down

in vec2 uv;

out vec4 fragColor;

// Signed distance in device pixels from this fragment to aperture i
// (negative inside). Apertures are circles or boxes.
float polygonDistance(vec2 p, vec2 size)
{
    vec2 first = outline[0] * size;
    float d = dot(p - first, p - first);
    float parity = 1.0;
    for (int i = 0, j = outlineCount - 1; i < outlineCount; j = i, ++i) {
        vec2 a = outline[i] * size;
        vec2 b = outline[j] * size;
        vec2 e = b - a;
        vec2 w = p - a;
        vec2 closest = w - e * clamp(dot(w, e) / max(dot(e, e), 1e-6), 0.0, 1.0);
        d = min(d, dot(closest, closest));
        bvec3 c = bvec3(p.y >= a.y, p.y < b.y, e.x * w.y > e.y * w.x);
        if (all(c) || all(not(c))) {
            parity = -parity;
        }
    }
    return parity * sqrt(d);
}

float apertureDistance(int i)
{
    vec2 p = (uv - apertures[i].xy) * rectSize;
    vec2 halfSize = apertures[i].zw * rectSize;
    if (outlineCount > 0) {
        // Box-local pixels with y down (uv.y points up).
        return polygonDistance(vec2(p.x + halfSize.x, halfSize.y - p.y), halfSize * 2.0);
    }
    if (apertureCircle != 0) {
        return length(p) - halfSize.x;
    }
    vec2 q = abs(p) - halfSize;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}

void main(void)
{
    vec4 sum = texture(texUnit, uv + vec2(-halfpixel.x * 2.0, 0.0) * offset);
    sum += texture(texUnit, uv + vec2(-halfpixel.x, halfpixel.y) * offset) * 2.0;
    sum += texture(texUnit, uv + vec2(0.0, halfpixel.y * 2.0) * offset);
    sum += texture(texUnit, uv + vec2(halfpixel.x, halfpixel.y) * offset) * 2.0;
    sum += texture(texUnit, uv + vec2(halfpixel.x * 2.0, 0.0) * offset);
    sum += texture(texUnit, uv + vec2(halfpixel.x, -halfpixel.y) * offset) * 2.0;
    sum += texture(texUnit, uv + vec2(0.0, -halfpixel.y * 2.0) * offset);
    sum += texture(texUnit, uv + vec2(-halfpixel.x, -halfpixel.y) * offset) * 2.0;

    fragColor = (sum / 12.0) * colorMatrix;

    if (apertureCount == 0) {
        return;
    }

    // Nearest aperture decides the edge; coverage is 1 inside, 0 outside,
    // with a one pixel linear ramp so the edge is antialiased.
    float nearest = 1e9;
    int nearestIndex = 0;
    for (int i = 0; i < apertureCount; ++i) {
        float d = apertureDistance(i);
        if (d < nearest) {
            nearest = d;
            nearestIndex = i;
        }
    }
    float coverage = clamp(0.5 - nearest, 0.0, 1.0);

    vec3 blurred = fragColor.rgb;
    if (apertureInverted != 0) {
        // Blur only inside the apertures.
        fragColor = vec4(blurred, 1.0) * coverage;
    } else {
        // Blur everywhere except the apertures, which show the live
        // background, optionally magnified.
        float zoom = zoomFactors[nearestIndex];
        vec3 inside = vec3(0.0);
        float insideAlpha = 0.0;
        if (zoom > 1.0) {
            vec2 center = apertures[nearestIndex].xy;
            inside = texture(originalTexture, center + (uv - center) / zoom).rgb;
            insideAlpha = 1.0;
        }
        fragColor = vec4(blurred * (1.0 - coverage) + inside * coverage,
                         (1.0 - coverage) + insideAlpha * coverage);
    }
    fragColor *= modulation;
}
