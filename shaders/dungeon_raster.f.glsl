/* Copyright (C) 2023 kichikuou <KichikuouChrome@gmail.com>
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

uniform float amp;
uniform float t;

// [OHOS] PI inlined to avoid Maleoon 920 S0015 (const float used in mul expression).

in vec2 tex_coord;
out vec4 frag_color;

vec4 sample_texture(float offset_x) {
	return texture(tex, vec2(tex_coord.x + offset_x, tex_coord.y));
}

void main() {
	float a = amp * cos((tex_coord.y + fract(t / 8.0)) * 50.26548); // 16*PI
	float p1 = 6.0 * tex_coord.y + 1.5;
	float p2 = 4.0 * tex_coord.y + 2.0;
	float offset1 = cos(p1 * 6.28318530) * a; // 2*PI
	float offset2 = cos(p2 * 6.28318530) * a; // 2*PI

	frag_color = mix(sample_texture(offset1), sample_texture(offset2), 0.5);
}
