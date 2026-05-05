/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

uniform sampler2D tex;   // the new scene
uniform sampler2D old;   // the old scene
uniform vec2 resolution; // the screen resolution
uniform float progress;  // effect progress (0..1)

in vec2 tex_coord;
out vec4 frag_color;

// [OHOS] PI and sigma inlined to avoid Maleoon 920 S0015 (const*const mul triggers spurious error).
const vec2 tex_offset = vec2(1.0, 1.0);

vec3 get_pixel(vec2 xy) {
        return texture(old, xy).rgb;
}

// incremental gaussian blur algorithm
void main() {
        vec2 p = tex_coord * resolution;
        float blur_size = progress * 100.0;
        float blur_half = blur_size / 2.0;

        vec3 gauss;
        gauss.x = 1.0 / (sqrt(6.28318530) * 50.0); // 1/(sqrt(2*PI)*sigma), PI=3.14159265, sigma=50
        gauss.y = exp(-0.5 / 2500.0);               // exp(-0.5/sigma^2), sigma=50
        gauss.z = gauss.y * gauss.y;

        vec3 avg = vec3(0.0);
        float coefficient_sum = 0.0;

        avg += get_pixel(p / resolution) * gauss.x;
        coefficient_sum += gauss.x;
        gauss.xy *= gauss.yz;

        vec2 tex_offset = vec2(0.25, 0.75);

        for (float i = 1.0; i <= blur_half; i += 1.0) {
                avg += get_pixel((p - i * tex_offset) / resolution) * gauss.x;
                avg += get_pixel((p + i * tex_offset) / resolution) * gauss.x;
                coefficient_sum += 2.0 * gauss.x;
                gauss.xy *= gauss.yz;
        }

        tex_offset = vec2(0.75, 0.25);

        for (float i = 1.0; i <= blur_half; i += 1.0) {
                avg += get_pixel((p - i * tex_offset) / resolution) * gauss.x;
                avg += get_pixel((p + i * tex_offset) / resolution) * gauss.x;
                coefficient_sum += 2.0 * gauss.x;
                gauss.xy *= gauss.yz;
        }

        vec3 old_px = avg / coefficient_sum;
        vec3 new_px = texture(tex, tex_coord).rgb;
        frag_color = vec4(mix(old_px, new_px, pow(progress, 3.0)), 1.0);
}
