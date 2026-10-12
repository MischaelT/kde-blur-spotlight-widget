#version 140

uniform sampler2D texUnit;
uniform mat4 colorMatrix;
uniform float offset;
uniform vec2 halfpixel;
uniform sampler2D originalTexture;
uniform int zoomCount;
uniform int zoomCircle;
uniform vec4 zoomRegions[16];
uniform float zoomFactors[16];

in vec2 uv;

out vec4 fragColor;

void main(void)
{
    vec2 sampleUV = uv;
    bool zoomed = false;
    for (int i = 0; i < zoomCount; ++i) {
        vec2 relative = (uv - zoomRegions[i].xy) / zoomRegions[i].zw;
        bool inside = zoomCircle != 0 ? dot(relative, relative) <= 1.0
                                      : max(abs(relative.x), abs(relative.y)) <= 1.0;
        if (inside) {
            sampleUV = zoomRegions[i].xy + (uv - zoomRegions[i].xy) / zoomFactors[i];
            zoomed = true;
            break;
        }
    }
    if (zoomed) {
        fragColor = texture(originalTexture, sampleUV);
        return;
    }
    vec4 sum = texture(texUnit, uv + vec2(-halfpixel.x * 2.0, 0.0) * offset);
    sum += texture(texUnit, uv + vec2(-halfpixel.x, halfpixel.y) * offset) * 2.0;
    sum += texture(texUnit, uv + vec2(0.0, halfpixel.y * 2.0) * offset);
    sum += texture(texUnit, uv + vec2(halfpixel.x, halfpixel.y) * offset) * 2.0;
    sum += texture(texUnit, uv + vec2(halfpixel.x * 2.0, 0.0) * offset);
    sum += texture(texUnit, uv + vec2(halfpixel.x, -halfpixel.y) * offset) * 2.0;
    sum += texture(texUnit, uv + vec2(0.0, -halfpixel.y * 2.0) * offset);
    sum += texture(texUnit, uv + vec2(-halfpixel.x, -halfpixel.y) * offset) * 2.0;

    fragColor = (sum / 12.0) * colorMatrix;
}
