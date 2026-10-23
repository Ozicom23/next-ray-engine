#pragma once

// Gamepad button glyphs inline in text: control characters stand for them, the font draws them from
// ui\ui_gamepad_glyphs (tools/gamepad/make_glyphs.py) as squares of the text's height. Single-byte fonts only.
// Glyphs: the 16 buttons in GP_A order (A B X Y LB RB LT RT View Menu L3 R3 D-pad up down left right), then the left
// and right stick
namespace gamepad_glyphs
{
	const int count = 18;
	const int left_stick = 16, right_stick = 17;

	// The set drawn: PlayStation's or Xbox's
	ENGINE_API extern bool playstation;

	// Characters 1-8 and 14-23 (no tab, line feed or carriage return)
	IC char to_char(int glyph) { return char(glyph < 8 ? 1 + glyph : 14 + glyph - 8); }
	IC int from_char(u8 c)
	{
		if (c >= 1 && c <= 8)
			return c - 1;
		if (c >= 14 && c < 14 + count - 8)
			return 8 + c - 14;
		return -1;
	}
}
