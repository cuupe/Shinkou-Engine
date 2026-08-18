#version 450

float hash21(vec2 point) {
    return fract(sin(dot(point, vec2(127.1, 311.7))) * 43758.5453);
}

float noise2(vec2 point) {
    vec2 cell = floor(point);
    vec2 local = fract(point);
    local = local * local * (3.0 - 2.0 * local);
    float a = hash21(cell);
    float b = hash21(cell + vec2(1.0, 0.0));
    float c = hash21(cell + vec2(0.0, 1.0));
    float d = hash21(cell + vec2(1.0, 1.0));
    return mix(mix(a, b, local.x), mix(c, d, local.x), local.y);
}

float fbm(vec2 point) {
    float value = 0.0;
    float amplitude = 0.5;
    for (int octave = 0; octave < 4; ++octave) {
        value += amplitude * noise2(point);
        point = point * 2.03 + 17.0;
        amplitude *= 0.5;
    }
    return value;
}

layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(1280.0, 720.0);
    vec2 centered = uv * 2.0 - 1.0;
    float aspect = 1280.0 / 720.0;
    float x = centered.x * aspect;
    float depth = 1.0 - uv.y;
    float horizon = 0.47 + 0.025 * sin(x * 2.7) + 0.018 * sin(x * 6.0 + 1.4);
    float ground = smoothstep(horizon - 0.012, horizon + 0.018, depth);

    float skyGradient = clamp(1.0 - depth / max(horizon, 0.001), 0.0, 1.0);
    vec3 sky = mix(vec3(0.80, 0.91, 1.0), vec3(0.20, 0.53, 0.84), skyGradient);
    float sun = exp(-22.0 * length(centered - vec2(0.47, 0.32)));
    sky += sun * vec3(1.0, 0.72, 0.32);
    float cloud = smoothstep(0.60, 0.80, fbm(vec2(x * 0.32, depth * 1.5) + 4.0));
    cloud *= smoothstep(horizon + 0.04, horizon - 0.12, depth);
    sky = mix(sky, sky + vec3(0.16, 0.18, 0.17), cloud * 0.45);

    float foreground = clamp((depth - horizon) / max(1.0 - horizon, 0.001), 0.0, 1.0);
    float riverCenter = 0.10 * sin(depth * 5.0 + 0.5) + 0.055 * sin(depth * 13.0);
    float riverWidth = mix(0.025, 0.24, pow(foreground, 0.72));
    float riverDistance = abs(x * 0.38 - riverCenter);
    float river = 1.0 - smoothstep(riverWidth * 0.42, riverWidth, riverDistance);
    float grassNoise = fbm(vec2(x * 2.1, depth * 7.0));
    vec3 grass = mix(vec3(0.08, 0.20, 0.055), vec3(0.30, 0.52, 0.12), grassNoise);
    grass *= mix(0.72, 1.15, foreground);
    float blades = smoothstep(0.55, 0.86, noise2(vec2(x * 75.0, depth * 32.0)));
    grass += blades * vec3(0.10, 0.17, 0.035) * foreground;
    vec3 water = mix(vec3(0.035, 0.22, 0.30), vec3(0.10, 0.52, 0.60), 1.0 - riverDistance / max(riverWidth, 0.001));
    water += 0.06 * sin(vec3(1.0, 1.3, 1.7) * (x * 65.0 + depth * 34.0));
    vec3 scene = mix(sky, mix(grass, water, river), ground);
    float haze = smoothstep(horizon + 0.02, horizon - 0.03, depth) * 0.16;
    scene = mix(scene, vec3(0.66, 0.79, 0.78), haze);
    outColor = vec4(scene, 1.0);
}
