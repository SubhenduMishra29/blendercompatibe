uniform vec2 wireAlpha = vec2(1.0, 0.0);

float wire_alpha(float depth)
{
	float alpha = wireAlpha.x;

	if (wireAlpha.y > 0) {
		float view_z = get_view_z_from_depth(depth);

		alpha *= pow(0.5, abs(view_z) * wireAlpha.y);
	}

	return clamp(alpha, 0, 1);
}
