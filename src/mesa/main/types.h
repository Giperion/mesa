#pragma once

typedef struct {
	GLboolean rgbFlag;
	GLboolean dbFlag;
	GLboolean stereoFlag;
	GLint RedBits;
	GLint GreenBits;
	GLint BlueBits;
	GLint AlphaBits;
	GLint IndexBits;
	GLint DepthBits;
	GLint StencilBits;
	GLint AccumRedBits;
	GLint AccumGreenBits;
	GLint AccumBlueBits;
	GLint AccumAlphaBits;
	GLint numSamples
} GLvisual;

typedef struct  
{
	GLvisual* Visual;
	GLboolean UseSoftwareDepthBuffer;
	GLboolean UseSoftwareStencilBuffer;
	GLboolean UseSoftwareAccumBuffer;
	GLboolean UseSoftwareAlphaBuffers;
} GLframebuffer;



typedef struct
{
	GLint Dummy;
	void* DriverCtx;
	GLvisual* Visual;
	GLframebuffer* DrawBuffer;
} gl_context;
