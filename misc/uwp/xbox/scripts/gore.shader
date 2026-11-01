// Blood pools and wall runs for the gore effects (code/cgame/cg_gore.c).
// Textures are generated art in xbox/gore/. Green blood uses the
// automatic "#green" variants of these shaders.

gore/bloodpool1
{
	polygonOffset
	nopicmip
	{
		clampmap gore/bloodpool1.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}

gore/bloodpool2
{
	polygonOffset
	nopicmip
	{
		clampmap gore/bloodpool2.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}

gore/bloodrun
{
	polygonOffset
	nopicmip
	cull none
	{
		clampmap gore/bloodrun.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}

// Extreme: close-range blood on the screen (drawn in 2D)
gore/screensplat1
{
	nopicmip
	nomipmaps
	{
		clampmap gore/screensplat1.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}

gore/screensplat2
{
	nopicmip
	nomipmaps
	{
		clampmap gore/screensplat2.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}

gore/screensplat3
{
	nopicmip
	nomipmaps
	{
		clampmap gore/screensplat3.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen vertex
		alphaGen vertex
	}
}
