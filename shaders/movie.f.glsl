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

uniform sampler2D texture_y;
uniform sampler2D texture_cb;
uniform sampler2D texture_cr;

in vec2 tex_coord;
out vec4 frag_color;

void main() {
	float y = texture(texture_y, tex_coord).r;
	float cb = texture(texture_cb, tex_coord).r;
	float cr = texture(texture_cr, tex_coord).r;

	float r = 1.16438 * y + 1.59603 * cr - 0.87079;
	float g = 1.16438 * y - 0.39176 * cb - 0.81297 * cr + 0.52959;
	float b = 1.16438 * y + 2.01723 * cb - 1.08139;

	frag_color = vec4(r, g, b, 1.0);
}
