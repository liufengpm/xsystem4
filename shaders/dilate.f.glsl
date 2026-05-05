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

uniform sampler2D tex;
uniform float threshold;
uniform vec4 color;  // .a is the discard threshold

in vec2 tex_coord;
out vec4 frag_color;

// Static loop radius for GPU compiler compatibility (supports bold_width up to 5.0).
// Dynamic loop bounds derived from uniforms can crash certain mobile GPU compilers
// (e.g. Huawei BiSheng / Kirin X90) in compiler_compile_pipeline during glLinkProgram.
#define DILATE_RADIUS 6

void main() {
        // the fractional part of threshold becomes a weight value for the
        // edge pixels
        int size = int(floor(threshold));
        float edge_weight = fract(threshold) / 4.0;
        vec2 tex_size = vec2(textureSize(tex, 0).xy);
        float t = ceil(threshold);

        float a_out = 0.0;

        // Use a compile-time-constant loop bound so the GPU compiler can reason
        // about the loop statically.  Pixels outside the actual dilation radius
        // are culled by the "d > t" check below.
        for (int x = -DILATE_RADIUS; x <= DILATE_RADIUS; x++) {
                for (int y = -DILATE_RADIUS; y <= DILATE_RADIUS; y++) {
                        // Explicit float conversion to avoid implicit int->float issues.
                        float d = length(vec2(float(x), float(y)));
                        if (d > t)
                                continue;
                        float a = texture(tex, tex_coord + vec2(float(x), float(y)) / tex_size).r;
                        if (d > float(size))
                                a *= edge_weight;
                        a_out += a;
                }
        }

        if (a_out < color.a)
                discard;
        frag_color = vec4(color.rgb, min(a_out, 1.0));
}
