// Skulls (code/cgame/cg_xbskulls.c). Named zz_* to be read before the broken
// RealRTCW shader file (see zz_xbox_gore.shader). Assets: tools/gen_skull_assets.py

xbox/confetti
{
	nopicmip
	cull none
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}

xbox/newsreel_grain
{
	nopicmip
	nomipmaps
	{
		map gfx/xbox/newsreel_grain.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}

xbox/newsreel_scratch
{
	nopicmip
	nomipmaps
	{
		clampmap gfx/xbox/newsreel_scratch.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}

xbox/newsreel_vignette
{
	nopicmip
	nomipmaps
	{
		clampmap gfx/xbox/newsreel_vignette.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}
