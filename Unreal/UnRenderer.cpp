#include "Core.h"

#include "UnCore.h"
#include "UnObject.h"
#include "UnMaterial.h"

#include "UnPackage.h"					// for accessing Game field

#include "UnMaterial2.h"
#include "UnMaterial3.h"

#if RENDERING

#include "GlWindow.h"

#include "Shaders.h"

#define MAX_IMG_SIZE		4096


static GLuint DefaultTexNum = 0;		//?? bad code ... should move default texture to CoreGL ?

//#define SHOW_SHADER_PARAMS	1

/*-----------------------------------------------------------------------------
	Mipmapping and resampling
-----------------------------------------------------------------------------*/

//?? move to CoreGL

static void ResampleTexture(unsigned* in, int inwidth, int inheight, unsigned* out, int outwidth, int outheight)
{
	int		i;
	unsigned p1[MAX_IMG_SIZE], p2[MAX_IMG_SIZE];

	unsigned fracstep = (inwidth << 16) / outwidth;
	unsigned frac = fracstep >> 2;
	for (i = 0; i < outwidth; i++)
	{
		p1[i] = 4 * (frac >> 16);
		frac += fracstep;
	}
	frac = 3 * (fracstep >> 2);
	for (i = 0; i < outwidth; i++)
	{
		p2[i] = 4 * (frac >> 16);
		frac += fracstep;
	}

	float f, f1, f2;
	f = (float)inheight / outheight;
	for (i = 0, f1 = 0.25f * f, f2 = 0.75f * f; i < outheight; i++, out += outwidth, f1 += f, f2 += f)
	{
		unsigned *inrow  = in + inwidth * appFloor(f1);
		unsigned *inrow2 = in + inwidth * appFloor(f2);
		for (int j = 0; j < outwidth; j++)
		{
			int		n, r, g, b, a;
			byte	*pix;

			n = r = g = b = a = 0;
#define PROCESS_PIXEL(row,col)	\
	pix = (byte *)row + col[j];		\
	if (pix[3])	\
	{			\
		n++;	\
		r += *pix++; g += *pix++; b += *pix++; a += *pix;	\
	}
			PROCESS_PIXEL(inrow,  p1);
			PROCESS_PIXEL(inrow,  p2);
			PROCESS_PIXEL(inrow2, p1);
			PROCESS_PIXEL(inrow2, p2);
#undef PROCESS_PIXEL

			switch (n)		// NOTE: generic version ("x /= n") is 50% slower
			{
			// case 1 - divide by 1 - do nothing
			case 2: r >>= 1; g >>= 1; b >>= 1; a >>= 1; break;
			case 3: r /= 3;  g /= 3;  b /= 3;  a /= 3;  break;
			case 4: r >>= 2; g >>= 2; b >>= 2; a >>= 2; break;
			case 0: r = g = b = 0; break;
			}

			((byte *)(out+j))[0] = r;
			((byte *)(out+j))[1] = g;
			((byte *)(out+j))[2] = b;
			((byte *)(out+j))[3] = a;
		}
	}
}


static void MipMap(byte* in, int width, int height)
{
	width *= 4;		// sizeof(rgba)
	height >>= 1;
	byte *out = in;
	for (int i = 0; i < height; i++, in += width)
	{
		for (int j = 0; j < width; j += 8, out += 4, in += 8)
		{
			int		r, g, b, a, am, n;

			r = g = b = a = am = n = 0;
//!! should perform removing of alpha-channel when IMAGE_NOALPHA specified
//!! should perform removing (making black) color channel when alpha==0 (NOT ALWAYS?)
//!!  - should analyze shader, and it will not use blending with alpha (or no blending at all)
//!!    then remove alpha channel (require to process shader's *map commands after all other commands, this
//!!    can be done with delaying [map|animmap|clampmap|animclampmap] lines and executing after all)
#if 0
#define PROCESS_PIXEL(idx)	\
	if (in[idx+3])	\
	{	\
		n++;	\
		r += in[idx]; g += in[idx+1]; b += in[idx+2]; a += in[idx+3];	\
		am = max(am, in[idx+3]);	\
	}
#else
#define PROCESS_PIXEL(idx)	\
	{	\
		n++;	\
		r += in[idx]; g += in[idx+1]; b += in[idx+2]; a += in[idx+3];	\
		am = max(am, in[idx+3]);	\
	}
#endif
			PROCESS_PIXEL(0);
			PROCESS_PIXEL(4);
			PROCESS_PIXEL(width);
			PROCESS_PIXEL(width+4);
#undef PROCESS_PIXEL
			//!! NOTE: currently, always n==4 here
			switch (n)
			{
			// case 1 - divide by 1 - do nothing
			case 2:
				r >>= 1; g >>= 1; b >>= 1; a >>= 1;
				break;
			case 3:
				r /= 3; g /= 3; b /= 3; a /= 3;
				break;
			case 4:
				r >>= 2; g >>= 2; b >>= 2; a >>= 2;
				break;
			case 0:
				r = g = b = 0;
				break;
			}
			out[0] = r; out[1] = g; out[2] = b;
			// generate alpha-channel for mipmaps (don't let it be transparent)
			// dest alpha = (MAX(a[0]..a[3]) + AVG(a[0]..a[3])) / 2
			// if alpha = 255 or 0 (for all 4 points) -- it will holds its value
			out[3] = (am + a) / 2;
		}
	}
}


/*-----------------------------------------------------------------------------
	Uploading textures
-----------------------------------------------------------------------------*/

//?? move to CoreGL

static void GetImageDimensions(int width, int height, int* scaledWidth, int* scaledHeight)
{
	int sw, sh;
	for (sw = 1; sw < width;  sw <<= 1) ;
	for (sh = 1; sh < height; sh <<= 1) ;

	// scale down only when new image dimension is larger than 64 and
	// larger than 4/3 of original image dimension
	if (sw > 64 && sw > (width * 4 / 3))  sw >>= 1;
	if (sh > 64 && sh > (height * 4 / 3)) sh >>= 1;

	while (sw > MAX_IMG_SIZE) sw >>= 1;
	while (sh > MAX_IMG_SIZE) sh >>= 1;

	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;

	*scaledWidth  = sw;
	*scaledHeight = sh;
}


static void UploadTex(GLenum target, const void *pic, int width, int height, bool doMipmap)
{
	guard(UploadTex);

	/*----- Calculate internal dimensions of the new texture --------*/
	int scaledWidth, scaledHeight;
	GetImageDimensions(width, height, &scaledWidth, &scaledHeight);

	/*---------------- Resample/lightscale texture ------------------*/
	unsigned *scaledPic = new unsigned [scaledWidth * scaledHeight];
	if (width != scaledWidth || height != scaledHeight)
		ResampleTexture((unsigned*)pic, width, height, scaledPic, scaledWidth, scaledHeight);
	else
		memcpy(scaledPic, pic, scaledWidth * scaledHeight * sizeof(unsigned));

	/*------------- Determine texture format to upload --------------*/
	GLenum format;
	int alpha = 1; //?? image->alphaType;
	format = (alpha ? 4 : 3);

	/*------------------ Upload the image ---------------------------*/
	glTexImage2D(target, 0, format, scaledWidth, scaledHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, scaledPic);
	if (doMipmap)
	{
		int miplevel = 0;
		while (scaledWidth > 1 || scaledHeight > 1)
		{
			MipMap((byte *) scaledPic, scaledWidth, scaledHeight);
			miplevel++;
			scaledWidth  >>= 1;
			scaledHeight >>= 1;
			if (scaledWidth < 1)  scaledWidth  = 1;
			if (scaledHeight < 1) scaledHeight = 1;
			glTexImage2D(target, miplevel, format, scaledWidth, scaledHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, scaledPic);
		}
	}
	delete scaledPic;

	unguard;
}


// Try to upload texture without decompression
// Return false when not uploaded
static bool UploadCompressedTex(GLenum target, GLenum target2, CTextureData &TexData, bool doMipmap)
{
	guard(UploadCompressedTex);

	// verify GL capabilities
	if (!GL_SUPPORT(QGL_EXT_TEXTURE_COMPRESSION_S3TC))
		return false;		// no DXTn support
	if (!GL_SUPPORT(QGL_1_4))
		return false;		// cannot automatically generate mipmaps

	//?? support some other formats too
	// TPF_V8U8 = GL_RG8_SNORM (GL3.1)
	// TPF_BC5  = GL_COMPRESSED_RG_RGTC2 (GL_ARB_texture_compression_rgtc/GL_EXT_texture_compression_rgtc)
	// TPF_BC7  = GL_COMPRESSED_RGBA_BPTC_UNORM_ARB (GL_ARB_texture_compression_bptc)
	// TPF_G8   = GL_LUMINANCE
	// Notes:
	// - most formats are uploaded with glTexImage2D(), not with glCompressedTexImage2D()
	// - formats has different extension requirements (not QGL_EXT_TEXTURE_COMPRESSION_S3TC)

	GLenum format;
	switch (TexData.Format)
	{
	case TPF_DXT1:
		format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
		break;
	case TPF_DXT3:
		format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
		break;
	case TPF_DXT5:
		format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
		break;
//	case TPF_V8U8:
//		if (!GL_SUPPORT(QGL_3_1)) return false;
//		format = GL_RG8_SNORM;
//		break;
	case TPF_BC5:
		if (!GL_SUPPORT(QGL_ARB_TEXTURE_COMPRESSION_RGTC)) return false;
		format = GL_COMPRESSED_RG_RGTC2;
		break;
	case TPF_BC7:
		if (!GL_SUPPORT(QGL_ARB_TEXTURE_COMPRESSION_BPTC)) return false;
		format = GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
		break;
	default:
		return false;
	}

	if (!doMipmap)
	{
		// no mipmaps required
		glCompressedTexImage2D(target, 0, format, TexData.USize, TexData.VSize, 0, TexData.DataSize, TexData.CompressedData);
	}
	else if (GL_SUPPORT(QGL_EXT_FRAMEBUFFER_OBJECT))
	{
		// GL 3.0 or GL_EXT_framebuffer_object
		glCompressedTexImage2D(target, 0, format, TexData.USize, TexData.VSize, 0, TexData.DataSize, TexData.CompressedData);
		glGenerateMipmapEXT(target2);	//!! warning: this function could be slow, perhaps we should use mipmaps from texture data
	}
	else
	{
		// GL 1.4 - set GL_GENERATE_MIPMAP before uploading
		glTexParameteri(target2, GL_GENERATE_MIPMAP, GL_TRUE);
		glCompressedTexImage2D(target, 0, format, TexData.USize, TexData.VSize, 0, TexData.DataSize, TexData.CompressedData);
	}
	if (glGetError() != 0)
	{
		appNotify("Failed to upload texture in format 0x%04X", format);
		return false;
	}

	return true;

	unguard;
}


static int Upload2D(UUnrealMaterial *Tex, bool doMipmap, bool clampS, bool clampT)
{
	guard(Upload2D);

	CTextureData TexData;
	if (!Tex->GetTextureData(TexData))
	{
		appPrintf("WARNING: %s %s has no valid mipmaps\n", Tex->GetClassName(), Tex->Name);
	default_tex:
		if (!DefaultTexNum) BindDefaultMaterial();	//?? will produce bad result, but only for one frame
		return DefaultTexNum;						// "default texture"
	}

	GLuint TexNum;
	glGenTextures(1, &TexNum);
	glBindTexture(GL_TEXTURE_2D, TexNum);

	if (!UploadCompressedTex(GL_TEXTURE_2D, GL_TEXTURE_2D, TexData, doMipmap))
	{
		// upload uncompressed
		byte *pic = TexData.Decompress();
		if (!pic)
		{
			// some internal decompression error, message should be already displayed
			glDeleteTextures(1, &TexNum);
			goto default_tex;
		}
		UploadTex(GL_TEXTURE_2D, pic, TexData.USize, TexData.VSize, doMipmap);
		delete pic;
	}

	bool isDefault = (Tex->Package == NULL) && (Tex->Name == "Default");

	// setup min/max filter
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, doMipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);	// trilinear filter
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, isDefault ? GL_NEAREST : GL_LINEAR);

	// setup wrap flags
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clampS ? GL_CLAMP : GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clampT ? GL_CLAMP : GL_REPEAT);

	return TexNum;

	unguard;
}


static bool UploadCubeSide(UUnrealMaterial *Tex, bool doMipmap, int side)
{
	guard(UploadCubeSide);

	CTextureData TexData;
	if (!Tex->GetTextureData(TexData))
	{
		appPrintf("WARNING: %s %s has no valid mipmaps\n", Tex->GetClassName(), Tex->Name);
		return false;
	}

#if 0
	byte *pic2 = pic;
	for (int i = 0; i < USize * VSize; i++)
	{
		*pic2++ = (side & 1) * 255;
		*pic2++ = (side & 2) * 255;
		*pic2++ = (side & 4) * 255;
		*pic2++ = 255;
	}
#endif

	//!! BUGS: does not works with mipmap generation on Intel graphics (other not tested)
	//!! Works fine when calling glTexParameteri(target2, GL_GENERATE_MIPMAP, GL_TRUE) before 1st side upload
	//!! GL_INVALID_OPERATION when calling glGenerateMipmapEXT(GL_TEXTURE_CUBE_MAP_ARB) after last face
	//!! If will not find a working path, should remove target2 from UploadCompressedTex()
	//?? Info:
	//?? http://www.opengl.org/sdk/docs/man3/xhtml/glGenerateMipmap.xml
	doMipmap = false;	//!! workaround?

	GLenum target = GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB + side;
	if (!UploadCompressedTex(target, GL_TEXTURE_CUBE_MAP_ARB, TexData, doMipmap))
	{
		// upload uncompressed
		byte *pic = TexData.Decompress();
		if (!pic)
		{
			// some internal decompression error, message should be already displayed
			return false;
		}
		UploadTex(target, pic, TexData.USize, TexData.VSize, doMipmap);
		delete pic;
	}

	if (side == 5)
	{
		// the last one

		// setup min/max filter
		glTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_MIN_FILTER, doMipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);	// trilinear filter
		glTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		// setup wrap flags
		glTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenerateMipmapEXT(GL_TEXTURE_CUBE_MAP_ARB);
		GL_ResetError();	// previous function could fail for some reasons
	}

	return true;

	unguard;
}


/*-----------------------------------------------------------------------------
	Unreal materials support
-----------------------------------------------------------------------------*/

const CShader &GL_UseGenericShader(GenericShaderType type)
{
	guard(GL_UseGenericShader);

	assert(type >= 0 && type < GS_Count);

	static CShader shaders[GS_Count];
	static const char* defines[GS_Count] =
	{
		NULL,							// GS_Textured
		"#define TEXTURING 0",			// GS_White
		"#define NORMALMAP 1"			// GS_NormalMap
	};

	CShader &Sh = shaders[type];
	if (!Sh.IsValid()) Sh.Make(Generic_ush, defines[type]);
	Sh.Use();
	Sh.SetUniform("useLighting", glIsEnabled(GL_LIGHTING));
	return Sh;

	unguardf("type=%d", type);
}


#if SHOW_SHADER_PARAMS
#define DBG(x)		DrawTextLeft x
#else
#define DBG(x)
#endif

const CShader &GL_NormalmapShader(CShader &shader, CMaterialParams &Params)
{
	guard(GL_NormalmapShader);

	const char *subst[10];
	char  defines[512];
	defines[0] = 0;

	enum
	{
		I_Diffuse = 0,
		I_Normal,
		I_Specular,
		I_SpecularPower,
		I_Opacity,
		I_Emissive,
		I_Cube,
		I_Mask,
	};

	static const char *maskChannel[] =
	{
		"0.0",									// TC_NONE - bad, should not be used
		"texture2D(maskTex, TexCoord).r",		// TC_R
		"texture2D(maskTex, TexCoord).g",		// TC_G
		"texture2D(maskTex, TexCoord).b",		// TC_B
		"texture2D(maskTex, TexCoord).a",		// TC_A
		"(1.0-texture2D(maskTex, TexCoord).a)",	// TC_MA
	};

#define ADD_DEFINE(name)	appStrcatn(ARRAY_ARG(defines), "#define "name"\n")

	// diffuse
	glActiveTexture(GL_TEXTURE0);	// used for BindDefaultMaterial() too
	if (Params.Diffuse)
	{
		DBG(("Diffuse  : %s", Params.Diffuse->Name));
		ADD_DEFINE("DIFFUSE 1");
		Params.Diffuse->Bind();
	}
	else if (!Params.Normal && !Params.Cube)
	{
		BindDefaultMaterial();
		ADD_DEFINE("DIFFUSE 1");
	}

	// normal
	if (Params.Normal)	//!! reimplement ! plus, check for correct normalmap texture (VTC texture compression etc ...)
	{
		DBG(("Normal   : %s", Params.Normal->Name));
		glActiveTexture(GL_TEXTURE0 + I_Normal);
		Params.Normal->Bind();
	}

	// specular
	const char *specularExpr = "vec3(0.0)"; //?? vec3(specular)
	if (Params.Specular)
	{
		DBG(("Specular : %s", Params.Specular->Name));
		specularExpr = va("texture2D(specTex, TexCoord).%s * vec3(specular) * 1.5", !Params.SpecularFromAlpha ? "rgb" : "a");
		glActiveTexture(GL_TEXTURE0 + I_Specular);
		Params.Specular->Bind();
	}
	// specular power
	const char *specPowerExpr = "gl_FrontMaterial.shininess";
	if (Params.SpecPower)
	{
		DBG(("SpecPower: %s", Params.SpecPower->Name));
		specPowerExpr = "texture2D(spPowTex, TexCoord).g * 100.0 + 5.0";
		glActiveTexture(GL_TEXTURE0 + I_SpecularPower);
		Params.SpecPower->Bind();
	}

	// opacity mask
	const char *opacityExpr = "1.0";
	if (Params.Opacity)
	{
		DBG(("Opacity  : %s", Params.Opacity->Name));
		opacityExpr = va("texture2D(opacTex, TexCoord).%s", !Params.OpacityFromAlpha ? "g" : "a");
		glActiveTexture(GL_TEXTURE0 + I_Opacity);
		Params.Opacity->Bind();
	}
	else if (Params.Diffuse)
	{
		DBG(("Opacity from diffuse"));
		opacityExpr = va("texture2D(diffTex, TexCoord).a");
	}

	// emission
	const char *emissExpr = "vec3(0.0)";
	if (Params.Emissive)
	{
		DBG(("Emissive : %s", Params.Emissive->Name));
		emissExpr = va("vec3(%g,%g,%g) * texture2D(emisTex, TexCoord).g * 2.0",
			Params.EmissiveColor.R, Params.EmissiveColor.G, Params.EmissiveColor.B
		);
		ADD_DEFINE("EMISSIVE 1");
		glActiveTexture(GL_TEXTURE0 + I_Emissive);
		Params.Emissive->Bind();
	}

	// cubemap
	const char *cubeExpr = "vec3(0.0)";
	const char *cubeMaskExpr = "1.0";
	if (Params.Cube)
	{
		DBG(("Cubemap  : %s", Params.Cube->Name));
		cubeExpr = "textureCube(cubeTex, TexCoord).rgb";
		ADD_DEFINE("CUBE 1");
		glActiveTexture(GL_TEXTURE0 + I_Cube);
		Params.Cube->Bind();
		if (Params.Emissive)
		{
			// use emissive as cubemap mask
			cubeMaskExpr = "texture2D(emisTex, TexCoord).g";
			emissExpr = "vec3(0.0)";
		}
	}

	// mask
	if (Params.Mask)
	{
		DBG(("Mask     : %s", Params.Mask->Name));
		glActiveTexture(GL_TEXTURE0 + I_Mask);
		Params.Mask->Bind();
		// channels
		if (Params.EmissiveChannel)
		{
			emissExpr = va("vec3(%g,%g,%g) * %s * 2.0",
				Params.EmissiveColor.R, Params.EmissiveColor.G, Params.EmissiveColor.B,
				maskChannel[Params.EmissiveChannel]
			);
			ADD_DEFINE("EMISSIVE 1");
		}
		if (Params.SpecularMaskChannel)
			specularExpr  = va("vec3(%s)", maskChannel[Params.SpecularMaskChannel]);
		if (Params.SpecularPowerChannel)
			specPowerExpr = va("%s * 100.0 + 5.0", maskChannel[Params.SpecularPowerChannel]);
		if (Params.CubemapMaskChannel)
			cubeMaskExpr = maskChannel[Params.CubemapMaskChannel];
	}

	//!! NOTE: Specular and SpecPower are scaled by const to improve visual; should be scaled by parameters from material
	subst[0] = Params.Normal ? "texture2D(normTex, TexCoord).rgb * 2.0 - 1.0"  : "vec3(0.0, 0.0, 1.0)";
	subst[1] = specularExpr;
	subst[2] = specPowerExpr;
	subst[3] = opacityExpr;
	subst[4] = emissExpr;
	subst[5] = cubeExpr;
	subst[6] = cubeMaskExpr;
	// finalize paramerers and make shader
	subst[7] = NULL;

	if (Params.bUseMobileSpecular)
	{
		static const char* mobileSpecExpr[] = {				// EMobileSpecularMask values
			"vec3(1.0)",									// MSM_Constant
			"texture2D(diffTex, TexCoord).rgb * 2.0",		// MSM_Luminance
			"texture2D(diffTex, TexCoord).r * vec3(2.0)",	// MSM_DiffuseRed
			"texture2D(diffTex, TexCoord).g * vec3(2.0)",	// MSM_DiffuseGreen
			"texture2D(diffTex, TexCoord).b * vec3(2.0)",	// MSM_DiffuseBlue
			"texture2D(diffTex, TexCoord).a * vec3(2.0)",	// MSM_DiffuseAlpha
			"texture2D(opacTex, TexCoord).rgb * 2.0",		// MSM_MaskTextureRGB
			"texture2D(opacTex, TexCoord).r * vec3(2.0)",	// MSM_MaskTextureRed
			"texture2D(opacTex, TexCoord).g * vec3(2.0)",	// MSM_MaskTextureGreen
			"texture2D(opacTex, TexCoord).b * vec3(2.0)",	// MSM_MaskTextureBlue
			"texture2D(opacTex, TexCoord).a * vec3(2.0)",	// MSM_MaskTextureAlpha
		};
//??		check presence of textures used in a shader ?
		subst[1] = mobileSpecExpr[Params.MobileSpecularMask];
		subst[2] = va("%f * 1.0", Params.MobileSpecularPower);	//??
	}

	glActiveTexture(GL_TEXTURE0);

	//?? should check IsValid before preparing params above (they're needed only once)
	if (!shader.IsValid()) shader.Make(Normal_ush, defines, subst);
	shader.Use();

	shader.SetUniform("diffTex",  I_Diffuse);
	shader.SetUniform("normTex",  I_Normal);
	shader.SetUniform("specTex",  I_Specular);
	shader.SetUniform("spPowTex", I_SpecularPower);
	shader.SetUniform("opacTex",  I_Opacity);
	shader.SetUniform("emisTex",  I_Emissive);
	shader.SetUniform("cubeTex",  I_Cube);
	shader.SetUniform("maskTex",  I_Mask);
	return shader;

	unguard;
}


void UUnrealMaterial::Release()
{
	guard(UUnrealMaterial::Release);
#if USE_GLSL
	GLShader.Release();
#endif
	DrawTimestamp = 0;
	unguard;
}


void UUnrealMaterial::SetMaterial()
{
	guard(UUnrealMaterial::SetMaterial);

	SetupGL();

	CMaterialParams Params;
	GetParams(Params);

	if (Params.IsNull())
	{
		//?? may be, use diffuse color + other params
		BindDefaultMaterial();
		return;
	}

#if USE_GLSL
	if (GUseGLSL)
	{
		if (Params.Diffuse == this)
		{
			// simple texture
			Params.Diffuse->Bind();
			GL_UseGenericShader(GS_Textured);
		}
		else
		{
			DBG((S_BLUE"---- %s %s ----", GetClassName(), Name));
			GL_NormalmapShader(GLShader, Params);
		}
	}
	else
#endif // USE_GLSL
	{
		if (Params.Diffuse)	//?? already checked above
			Params.Diffuse->Bind();
		else
			BindDefaultMaterial();
	}

	unguardf("%s", Name);
}


void UUnrealMaterial::SetupGL()
{
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
}


void UMaterialWithPolyFlags::SetupGL()
{
	Super::SetupGL();
	if (Material) Material->SetupGL();

	// twosided material
	if (PolyFlags & PF_TwoSided)
		glDisable(GL_CULL_FACE);

	// handle blending
	if (PolyFlags & PF_Translucent)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
	}
	else if (PolyFlags & PF_Modulated)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
	}
	else if (PolyFlags & (PF_Masked/*|PF_TwoSided*/))
	{
		glEnable(GL_BLEND);
		glEnable(GL_ALPHA_TEST);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glAlphaFunc(GL_GREATER, 0.1f);
	}
}


static TArray<UMaterialWithPolyFlags*> WrappedMaterials;

UUnrealMaterial *UMaterialWithPolyFlags::Create(UUnrealMaterial *OriginalMaterial, unsigned PolyFlags)
{
	if (PolyFlags == 0) return OriginalMaterial;	// no wrapping required
	// find for the same material with the same flags wrapped before
	for (int i = 0; i < WrappedMaterials.Num(); i++)
	{
		UMaterialWithPolyFlags *WM = WrappedMaterials[i];
		if (WM->Material == OriginalMaterial && WM->PolyFlags == PolyFlags)
			return WM;
	}
	// material is not wrapped yet, so wrap it
	UMaterialWithPolyFlags *WM = new UMaterialWithPolyFlags;
	WM->Name      = (OriginalMaterial) ? OriginalMaterial->Name : "None";
	WM->Material  = OriginalMaterial;
	WM->PolyFlags = PolyFlags;
	WrappedMaterials.AddItem(WM);
//	appNotify("WRAP: %s %X", OriginalMaterial ? OriginalMaterial->Name : "NULL", PolyFlags);
	return WM;
}


void BindDefaultMaterial(bool White)
{
	guard(BindDefaultMaterial);

	glDepthMask(GL_TRUE);		//?? place into other places too

	if (GL_SUPPORT(QGL_1_3))
	{
		// disable all texture units except 0
		int numTexUnits = 1;
		glGetIntegerv(GL_MAX_TEXTURE_UNITS, &numTexUnits);
		for (int i = 1; i < numTexUnits; i++)
		{
			glActiveTexture(GL_TEXTURE0 + i);
			glDisable(GL_TEXTURE_2D);
			glDisable(GL_TEXTURE_CUBE_MAP_ARB);
		}
		glActiveTexture(GL_TEXTURE0);
	}

#if USE_GLSL
	if (GUseGLSL) GL_UseGenericShader(White ? GS_White : GS_Textured);
#endif

	if (White)
	{
		glDisable(GL_TEXTURE_2D);
		glDisable(GL_TEXTURE_CUBE_MAP_ARB);
		return;
	}

	static UTexture *Mat = NULL;
	if (!Mat)
	{
		// allocate material
		Mat = new UTexture;
		Mat->bTwoSided = true;
		Mat->Mips.Add();
		Mat->Format = TEXF_RGBA8;
		Mat->Package = NULL;
		Mat->Name = "Default";
		// create 1st mipmap
#define TEX_SIZE	64
		FMipmap &Mip = Mat->Mips[0];
		Mip.USize = Mip.VSize = TEX_SIZE;
		Mip.DataArray.Add(TEX_SIZE*TEX_SIZE*4);
		byte *pic = &Mip.DataArray[0];
		for (int x = 0; x < TEX_SIZE; x++)
		{
			for (int y = 0; y < TEX_SIZE; y++)
			{
				static const byte colors[4][4] =
				{
//					{192,64,64,255}, {32,32,192,255}, {32,239,32,255}, {32,192,192,255}				// BGRA; most colorful
#define N 96		// saturation level; 0 = greyscale, 255-64-16 = max color (64 is max greyscale, 16 for nested checkerboards)
					{64+N,64,64,255}, {48,48,48+N,255}, {48,48+N,48,255}, {64,64+N/2,64+N/2,255}	// BGRA; colorized
#undef N
//					{64,64,64,255}, {48,48,48,255}, {48,48,48,255}, {64,64,64,255}					// BGRA; greyscale
				};
				byte *p = pic + y * TEX_SIZE * 4 + x * 4;
				int i1 = x < TEX_SIZE / 2;
				int i2 = y < TEX_SIZE / 2;
				const byte *c = colors[i1 * 2 + i2];	// top checkerboard level
				int corr = ((x ^ y) & 4) ? 4 : -4;		// nested checkerboard
//				corr += ((x ^ y) & 8) ? 12 : -12;		// one more nested checkerboard level
				p[0] = c[0] + corr;
				p[1] = c[1] + corr;
				p[2] = c[2] + corr;
				p[3] = c[3];
			}
		}
#undef TEX_SIZE
	}
	Mat->Bind();
	DefaultTexNum = Mat->TexNum;

	unguard;
}


void UTexture::SetupGL()
{
	guard(UTexture::SetupGL);

	glEnable(GL_TEXTURE_2D);
	glDisable(GL_TEXTURE_CUBE_MAP_ARB);

	glEnable(GL_DEPTH_TEST);
	// bTwoSided
	if (bTwoSided)
	{
		glDisable(GL_CULL_FACE);
	}
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}
	// alpha-test
	if (bMasked || (bAlphaTexture && Format == TEXF_DXT1))	// masked or 1-bit alpha
	{
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.8f);
	}
	else if (bAlphaTexture)
	{
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.1f);
	}
	// blending
	// partially taken from UT/OpenGLDrv
	if (bAlphaTexture || bMasked)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else
	{
		glDisable(GL_ALPHA_TEST);
		glDisable(GL_BLEND);
	}

	unguard;
}


void UTexture::Bind()
{
	guard(UTexture::Bind);

	glEnable(GL_TEXTURE_2D);
	glDisable(GL_TEXTURE_CUBE_MAP_ARB);

	bool upload = !GL_TouchObject(DrawTimestamp);
	if (upload)
	{
		TexNum = Upload2D(this, Mips.Num() > 1, UClampMode == TC_Clamp, VClampMode == TC_Clamp);
	}
	// bind texture
	glBindTexture(GL_TEXTURE_2D, TexNum);

	unguard;
}


void UTexture::GetParams(CMaterialParams &Params) const
{
	Params.Diffuse = (UUnrealMaterial*)this;
}


bool UTexture::IsTranslucent() const
{
	return bAlphaTexture || bMasked;
}


void UTexture::Release()
{
	guard(UTexture::Release);
	if (GL_IsValidObject(TexNum, DrawTimestamp) && TexNum != DefaultTexNum)
		glDeleteTextures(1, &TexNum);
	Super::Release();
	unguard;
}


void UModifier::SetupGL()
{
	guard(UModifier::SetupGL);
	if (Material) Material->SetupGL();
	unguard;
}

void UModifier::GetParams(CMaterialParams &Params) const
{
	guard(UModifier::GetParams);
	if (Material)
		Material->GetParams(Params);
	unguard;
}


void UFinalBlend::SetupGL()
{
	guard(UFinalBlend::SetupGL);

	glEnable(GL_DEPTH_TEST);
	// override material settings
	// TwoSided
	if (TwoSided)
	{
		glDisable(GL_CULL_FACE);
	}
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}
	// AlphaTest
	if (AlphaTest)
	{
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, AlphaRef / 255.0f);
	}
	else
	{
		glDisable(GL_ALPHA_TEST);
	}
	// FrameBufferBlending
	if (FrameBufferBlending == FB_Overwrite)
		glDisable(GL_BLEND);
	else
		glEnable(GL_BLEND);
	switch (FrameBufferBlending)
	{
//	case FB_Overwrite:
//		glBlendFunc(GL_ONE, GL_ZERO);				// src
//		break;
	case FB_Modulate:
		glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);	// src*dst*2
		break;
	case FB_AlphaBlend:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case FB_AlphaModulate_MightNotFogCorrectly:
		//!!
		break;
	case FB_Translucent:
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
		break;
	case FB_Darken:
		glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR); // dst - src
		break;
	case FB_Brighten:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);			// src*srcA + dst
		break;
	case FB_Invisible:
		glBlendFunc(GL_ZERO, GL_ONE);				// dst
		break;
	}
	//!! ZWrite, ZTest

	unguard;
}


bool UFinalBlend::IsTranslucent() const
{
	return (FrameBufferBlending != FB_Overwrite) || AlphaTest;
}


void UShader::SetupGL()
{
	guard(UShader::SetupGL);

	glEnable(GL_DEPTH_TEST);
	// TwoSided
	if (TwoSided)
		glDisable(GL_CULL_FACE);
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}
	//?? glEnable(GL_ALPHA_TEST) only when Opacity is set ?
	glDisable(GL_ALPHA_TEST);

	// blending
	if (OutputBlending == OB_Normal && !Opacity)
		glDisable(GL_BLEND);
	else
		glEnable(GL_BLEND);
	switch (OutputBlending)
	{
	case OB_Normal:
		if (Opacity) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case OB_Masked:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glAlphaFunc(GL_GREATER, 0.0f);
		glEnable(GL_ALPHA_TEST);
		break;
	case OB_Modulate:
		glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);	// src*dst*2
		break;
	case OB_Translucent:
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
		break;
	case OB_Invisible:
		glBlendFunc(GL_ZERO, GL_ONE);				// dst
		break;
	case OB_Brighten:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);			// src*srcA + dst
		break;
	case OB_Darken:
		glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR); // dst - src
		break;
	}

	unguard;
}


void UShader::GetParams(CMaterialParams &Params) const
{
	guard(UShader::GetParams);

	if (Diffuse)
	{
		Diffuse->GetParams(Params);
	}
#if BIOSHOCK
	if (NormalMap)
	{
		if ((UShader*)this == NormalMap) return;	// recurse; Bioshock has such data ...
		CMaterialParams Params2;
		NormalMap->GetParams(Params2);
		Params.Normal = Params2.Diffuse;
	}
#endif
	if (SpecularityMask)
	{
		CMaterialParams Params2;
		SpecularityMask->GetParams(Params2);
		Params.Specular          = Params2.Diffuse;
		Params.SpecularFromAlpha = true;
	}
	if (Opacity)
	{
		CMaterialParams Params2;
		Opacity->GetParams(Params2);
		Params.Opacity          = Params2.Diffuse;
		Params.OpacityFromAlpha = true;
	}

	unguardf("%s", Name);
}


bool UShader::IsTranslucent() const
{
	return (OutputBlending != OB_Normal);
}



#if BIOSHOCK

//?? NOTE: Bioshock EFrameBufferBlending is used for UShader and UFinalBlend, plus it has different values
// based on UShader and UFinalBlend Bind()
void UFacingShader::SetupGL()
{
	guard(UFacingShader::SetupGL);

	glEnable(GL_DEPTH_TEST);
	// TwoSided
	if (TwoSided)
		glDisable(GL_CULL_FACE);
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}
	// part of UFinalBlend::SetupGL()
	switch (OutputBlending)
	{
//	case FB_Overwrite:
//		glBlendFunc(GL_ONE, GL_ZERO);				// src
//		break;
	case FB_Modulate:
		glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);	// src*dst*2
		break;
	case FB_AlphaBlend:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case FB_AlphaModulate_MightNotFogCorrectly:
		//!!
		break;
	case FB_Translucent:
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
		break;
	case FB_Darken:
		glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR); // dst - src
		break;
	case FB_Brighten:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);			// src*srcA + dst
		break;
	case FB_Invisible:
		glBlendFunc(GL_ZERO, GL_ONE);				// dst
		break;
	}

	unguard;
}


void UFacingShader::GetParams(CMaterialParams &Params) const
{
	guard(UFacingShader::GetParams);

	if (FacingDiffuse)
	{
		CMaterialParams Params2;
		FacingDiffuse->GetParams(Params2);
		Params.Diffuse = Params2.Diffuse;
	}
	if (NormalMap)
	{
		CMaterialParams Params2;
		NormalMap->GetParams(Params2);
		Params.Normal = Params2.Diffuse;
	}
	if (FacingSpecularColorMap)
	{
		CMaterialParams Params2;
		FacingSpecularColorMap->GetParams(Params2);
		Params.Specular = Params2.Diffuse;
	}
	if (FacingEmissive)
	{
		CMaterialParams Params2;
		FacingEmissive->GetParams(Params2);
		Params.Emissive = Params2.Diffuse;
	}

	unguard;
}


bool UFacingShader::IsTranslucent() const
{
	return (OutputBlending != FB_Overwrite);
}

#endif // BIOSHOCK


#if SPLINTER_CELL

void UUnreal3Material::SetupGL()
{
	guard(UUnreal3Material::SetupGL);

	glEnable(GL_DEPTH_TEST);
	// TwoSided
	if (bDoubleSided)
		glDisable(GL_CULL_FACE);
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}
	// part of UFinalBlend::SetupGL()
	if (BlendingMode == U3BM_OPAQUE)
		glDisable(GL_BLEND);
	else
		glEnable(GL_BLEND);
	switch (BlendingMode)
	{
//	case U3BM_OPAQUE:
//		glBlendFunc(GL_ONE, GL_ZERO);				// src
//		break;
	case U3BM_TRANSLUCENT:
	case U3BM_TRANSLUCENT_NO_DISTORTION:
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
		break;
	case U3BM_ADDITIVE:
		glBlendFunc(GL_ONE, GL_ONE);				// src + dst
		break;
	case U3BM_MASKED:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);	// src*srcA + dst*(1-srcA)
		break;
	}

	if (BlendingMode == U3BM_MASKED)
	{
		glAlphaFunc(GL_GREATER, 0.0f);
		glEnable(GL_ALPHA_TEST);
	}
	else
	{
		glDisable(GL_ALPHA_TEST);
	}

	unguard;
}


void UUnreal3Material::GetParams(CMaterialParams &Params) const
{
	guard(UUnreal3Material::GetParams);

	for (int i = 0; i < ARRAY_COUNT(Textures); i++)
	{
		UTexture *Tex = Textures[i];
		if (!Tex) continue;
		const char *Name = Tex->Name;
		int len = strlen(Name);
		if (!stricmp(Name + len - 2, "_d"))
			Params.Diffuse = Tex;
		else if (!stricmp(Name + len - 2, "_n"))
			Params.Normal = Tex;
		else if (!stricmp(Name + len - 2, "_m"))
			Params.Mask = Tex;
//		else
//			appPrintf("Tex: %s\n", Name);
	}
	Params.SpecularMaskChannel = TC_G;

	unguard;
}


bool UUnreal3Material::IsTranslucent() const
{
	return (BlendingMode != U3BM_OPAQUE);
}


void USCX_basic_material::SetupGL()
{
	guard(USCX_basic_material::SetupGL);

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);

	unguard;
}


void USCX_basic_material::GetParams(CMaterialParams &Params) const
{
	guard(USCX_basic_material::GetParams);

	Params.Diffuse = Base;
	Params.Normal  = Normal;
	Params.Mask    = SpecularMask;		// Params.Specular, but single channel
	Params.Cube    = Environment;

	switch (SpecularSource)
	{
	case SpecSrc_SRed:
		Params.SpecularMaskChannel = TC_R;
		break;
	case SpecSrc_SGreen:
		Params.SpecularMaskChannel = TC_G;
		break;
	case SpecSrc_SBlue:
		Params.SpecularMaskChannel = TC_B;
		break;
	case SpecSrc_NAlpha:
		Params.SpecularMaskChannel = TC_MA;	//?? TC_A ?
		break;
	}

	unguard;
}


bool USCX_basic_material::IsTranslucent() const
{
	return false;
}


#endif // SPLINTER_CELL


void UCombiner::GetParams(CMaterialParams &Params) const
{
	guard(UCombiner::GetParams);

	CMaterialParams Params2;

	switch (CombineOperation)
	{
	case CO_Use_Color_From_Material1:
		if (Material1) Material1->GetParams(Params2);
		break;
	case CO_Use_Color_From_Material2:
		if (Material2) Material2->GetParams(Params2);
		break;
	case CO_Multiply:
	case CO_Add:
	case CO_Subtract:
	case CO_AlphaBlend_With_Mask:
	case CO_Add_With_Mask_Modulation:
		if (Material1 && Material2)
		{
			if (Material2->IsA("TexEnvMap"))
			{
				// special case: Material1 is a UTexEnvMap
				Material1->GetParams(Params2);
				Params.Specular = Params2.Diffuse;
				Params.SpecularFromAlpha = true;
			}
			else if (Material1->IsA("TexEnvMap"))
			{
				// special case: Material1 is a UTexEnvMap
				Material2->GetParams(Params2);
				Params.Specular = Params2.Diffuse;
				Params.SpecularFromAlpha = true;
			}
			else
			{
				// no specular; heuristic: usually Material2 contains more significant texture
				Material2->GetParams(Params2);
				if (!Params2.Diffuse) Material1->GetParams(Params2);	// fallback
			}
		}
		else if (Material1)
			Material1->GetParams(Params2);
		else if (Material2)
			Material2->GetParams(Params2);
		break;
	case CO_Use_Color_From_Mask:
		if (Mask) Mask->GetParams(Params2);
		break;
	}
	Params.Diffuse = Params2.Diffuse;

	// cannot implement masking right now: UE3 uses color, but UE2 - alpha
/*	switch (AlphaOperation)
	{
	case AO_Use_Mask:
	case AO_Multiply:
	case AO_Add:
	case AO_Use_Alpha_From_Material1:
	case AO_Use_Alpha_From_Material2:
	} */

	unguard;
}


#if UNREAL3


void UMaterialInterface::GetParams(CMaterialParams &Params) const
{
#if IPHONE
	//?? these parameters are common for UMaterial3 and UMaterialInstanceConstant (UMaterialInterface)
	if (FlattenedTexture)		Params.Diffuse = FlattenedTexture;
	if (MobileBaseTexture)		Params.Diffuse = MobileBaseTexture;
	if (MobileNormalTexture)	Params.Normal  = MobileNormalTexture;
	if (MobileMaskTexture)		Params.Opacity = MobileMaskTexture;
	Params.bUseMobileSpecular  = bUseMobileSpecular;
	Params.MobileSpecularPower = MobileSpecularPower;
	Params.MobileSpecularMask  = MobileSpecularMask;
#endif // IPHONE
}


//!! NOTE: when using this function sharing of shader between MaterialInstanceConstant's is impossible
//!! (shader may differs because of different texture sets - some available, some - not)

void UMaterial3::SetupGL()
{
	guard(UMaterial3::SetupGL);

	glDisable(GL_ALPHA_TEST);
	glDisable(GL_BLEND);

	// TwoSided
	if (TwoSided)
		glDisable(GL_CULL_FACE);
	else
	{
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}
	// depth test
	if (bDisableDepthTest)
		glDisable(GL_DEPTH_TEST);
	else
		glEnable(GL_DEPTH_TEST);
	// alpha mask
	if (bIsMasked)
	{
		glEnable(GL_BLEND);
//		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glAlphaFunc(GL_GREATER, OpacityMaskClipValue);
		glEnable(GL_ALPHA_TEST);
	}

	glDepthMask(BlendMode == BLEND_Translucent ? GL_FALSE : GL_TRUE); // may be, BLEND_Masked too

	if (BlendMode == BLEND_Opaque || BlendMode == BLEND_Masked)
		glDisable(GL_BLEND);
	else
	{
		glEnable(GL_BLEND);
		switch (BlendMode)
		{
//		case BLEND_Masked:
//			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);	//?? should use opacity channel; has lighting
//			break;
		case BLEND_Translucent:
//			glBlendFunc(GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR);	//?? should use opacity channel; no lighting
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);	//?? should use opacity channel; no lighting
			break;
		case BLEND_Additive:
			glBlendFunc(GL_ONE, GL_ONE);
			break;
		case BLEND_Modulate:
			glBlendFunc(GL_DST_COLOR, GL_ZERO);
			break;
		default:
			glDisable(GL_BLEND);
			DrawTextLeft("Unknown BlendMode %d", BlendMode);
		}
	}

	unguard;
}


bool UMaterial3::IsTranslucent() const
{
	return BlendMode != BLEND_Opaque;
}


void UMaterial3::GetParams(CMaterialParams &Params) const
{
	guard(UMaterial3::GetParams);

	Super::GetParams(Params);

	int DiffWeight = 0, NormWeight = 0, SpecWeight = 0, SpecPowWeight = 0, OpWeight = 0, EmWeight = 0, CubeWeight = 0;
#define DIFFUSE(check,weight)			\
	if (check && weight > DiffWeight)	\
	{									\
	/*	DrawTextLeft("D: %d > %d = %s", weight, DiffWeight, Tex->Name); */ \
		Params.Diffuse = Tex;			\
		DiffWeight = weight;			\
	}
#define NORMAL(check,weight)			\
	if (check && weight > NormWeight)	\
	{									\
	/*	DrawTextLeft("N: %d > %d = %s", weight, NormWeight, Tex->Name); */ \
		Params.Normal = Tex;			\
		NormWeight = weight;			\
	}
#define SPECULAR(check,weight)			\
	if (check && weight > SpecWeight)	\
	{									\
	/*	DrawTextLeft("S: %d > %d = %s", weight, SpecWeight, Tex->Name); */ \
		Params.Specular = Tex;			\
		SpecWeight = weight;			\
	}
#define SPECPOW(check,weight)			\
	if (check && weight > SpecPowWeight)\
	{									\
	/*	DrawTextLeft("SP: %d > %d = %s", weight, SpecPowWeight, Tex->Name); */ \
		Params.SpecPower = Tex;			\
		SpecPowWeight = weight;			\
	}
#define OPACITY(check,weight)			\
	if (check && weight > OpWeight)		\
	{									\
	/*	DrawTextLeft("O: %d > %d = %s", weight, OpWeight, Tex->Name); */ \
		Params.Opacity = Tex;			\
		OpWeight = weight;				\
	}
#define EMISSIVE(check,weight)			\
	if (check && weight > EmWeight)		\
	{									\
	/*	DrawTextLeft("E: %d > %d = %s", weight, EmWeight, Tex->Name); */ \
		Params.Emissive = Tex;			\
		EmWeight = weight;				\
	}
#define CUBEMAP(check,weight)			\
	if (check && weight > CubeWeight)	\
	{									\
	/*	DrawTextLeft("CUB: %d > %d = %s", weight, CubeWeight, Tex->Name); */ \
		Params.Cube = Tex;				\
		CubeWeight = weight;			\
	}
#define BAKEDMASK(check,weight)			\
	if (check && weight > MaskWeight)	\
	{									\
	/*	DrawTextLeft("MASK: %d > %d = %s", weight, MaskWeight, Tex->Name); */ \
		Params.Mask = Tex;				\
		MaskWeight = weight;			\
	}
#define EMISSIVE_COLOR(check,weight)	\
	if (check && weight > EmcWeight)	\
	{									\
	/*	DrawTextLeft("EC: %d > %d = %g %g %g", weight, EmcWeight, FCOLOR_ARG(Color)); */ \
		Params.EmissiveColor = Color;	\
		EmcWeight = weight;				\
	}
	for (int i = 0; i < ReferencedTextures.Num(); i++)
	{
		UTexture3 *Tex = ReferencedTextures[i];
		if (!Tex) continue;
		const char *Name = Tex->Name;
		int len = strlen(Name);
		//!! - separate code (common for UMaterial3 + UMaterialInstanceConstant)
		//!! - may implement with tables + macros
		//!! - catch normalmap, specular and emissive textures
		if (appStristr(Name, "noise")) continue;

		DIFFUSE(appStristr(Name, "diff"), 100);
		NORMAL (appStristr(Name, "norm"), 100);
		DIFFUSE(!stricmp(Name + len - 4, "_Tex"), 80);
		DIFFUSE(appStristr(Name, "_Tex"), 60);
		DIFFUSE(!stricmp(Name + len - 2, "_D"), 20);
		OPACITY(appStristr(Name, "_OM"), 20);
//		CUBEMAP(appStristr(Name, "cubemap"), 100); -- bad
#if 0
		if (!stricmp(Name + len - 3, "_DI"))		// The Last Remnant ...
			Diffuse = Tex;
		if (!strnicmp(Name + len - 4, "_DI", 3))	// The Last Remnant ...
			Diffuse = Tex;
		if (appStristr(Name, "_Diffuse"))
			Diffuse = Tex;
#endif
		DIFFUSE (appStristr(Name, "_DI"), 20);
//		DIFFUSE (appStristr(Name, "_MA"), 8 );		// The Last Remnant; low priority
		DIFFUSE (appStristr(Name, "_D" ), 11);
		DIFFUSE (!stricmp(Name + len - 2, "_C"), 10);
		DIFFUSE (!stricmp(Name + len - 3, "_CM"), 12);
		NORMAL  (!stricmp(Name + len - 2, "_N"), 20);
		NORMAL  (!stricmp(Name + len - 3, "_NM"), 20);
		NORMAL  (appStristr(Name, "_N"), 9);
#if BULLETSTORM
		if (Package->Game == GAME_Bulletstorm)
		{
			DIFFUSE (appStristr(Name, "_C"), 12);
			NORMAL(appStristr(Name, "_TS"), 5);
			SPECULAR(appStristr(Name, "_S"), 5);
		}
#endif // BULLETSTORM
		SPECULAR(!stricmp(Name + len - 2, "_S"), 20);
		SPECULAR(appStristr(Name, "_S_"), 15);
		SPECPOW (!stricmp(Name + len - 3, "_SP"), 20);
		SPECPOW (!stricmp(Name + len - 3, "_SM"), 20);
		SPECPOW (appStristr(Name, "_SP"), 9);
		EMISSIVE(!stricmp(Name + len - 2, "_E"), 20);
		EMISSIVE(!stricmp(Name + len - 3, "_EM"), 21);
		OPACITY (!stricmp(Name + len - 2, "_A"), 20);
		if (bIsMasked)
		{
			OPACITY (!stricmp(Name + len - 5, "_Mask"), 2);
		}
		// Magna Catra 2
		DIFFUSE (!strnicmp(Name, "df_", 3), 20);
		SPECULAR(!strnicmp(Name, "sp_", 3), 20);
//		OPACITY (!strnicmp(Name, "op_", 3), 20);
		NORMAL  (!strnicmp(Name, "no_", 3), 20);

		NORMAL  (appStristr(Name, "Norm"), 80);
		EMISSIVE(appStristr(Name, "Emis"), 80);
		SPECULAR(appStristr(Name, "Specular"), 80);
		OPACITY (appStristr(Name, "Opac"),  80);

		DIFFUSE(i == 0, 1);							// 1st texture as lowest weight
//		CUBEMAP(Tex->IsTextureCube(), 1);			// any cubemap
	}
	// do not allow normal map became a diffuse
	if ( (Params.Diffuse == Params.Normal && DiffWeight < NormWeight) ||
		 (Params.Diffuse && Params.Diffuse->IsTextureCube()) )
		Params.Diffuse = NULL;

	unguard;
}


void UTexture2D::Bind()
{
	guard(UTexture2D::Bind);

	glEnable(GL_TEXTURE_2D);
	glDisable(GL_TEXTURE_CUBE_MAP_ARB);

	bool upload = !GL_TouchObject(DrawTimestamp);
	if (upload)
	{
		TexNum = Upload2D(this, Mips.Num() > 1, AddressX == TA_Clamp, AddressY == TA_Clamp);
	}
	// bind texture
	glBindTexture(GL_TEXTURE_2D, TexNum);

	unguard;
}


void UTexture2D::GetParams(CMaterialParams &Params) const
{
	Params.Diffuse = (UUnrealMaterial*)this;
}


void UTexture2D::Release()
{
	guard(UTexture2D::Release);
	if (GL_IsValidObject(TexNum, DrawTimestamp))
		glDeleteTextures(1, &TexNum);
	Super::Release();
	unguard;
}


void UTextureCube::Bind()
{
	guard(UTextureCube::Bind);

	if (!GUseGLSL || !FacePosX || !FacePosY || !FacePosZ || !FaceNegX || !FaceNegY || !FaceNegZ)
	{
		BindDefaultMaterial();
		return;
	}

	glEnable(GL_TEXTURE_CUBE_MAP_ARB);

	bool upload = !GL_TouchObject(DrawTimestamp);

	if (upload)
	{
		// upload texture
		glGenTextures(1, &TexNum);
		glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, TexNum);
		for (int side = 0; side < 6; side++)
		{
			//?? can validate USize/VSize to be identical for all sides, plus - the same for "doMipmap" value
			UTexture2D *Tex = NULL;
			switch (side)
			{
			case 0:
				Tex = FacePosX;
				break;
			case 1:
				Tex = FaceNegX;
				break;
			case 2:
				Tex = FacePosY;
				break;
			case 3:
				Tex = FaceNegY;
				break;
			case 4:
				Tex = FacePosZ;
				break;
			case 5:
				Tex = FaceNegZ;
				break;
			}

			if (!UploadCubeSide(Tex, Tex->Mips.Num() > 1, side))
			{
				if (!DefaultTexNum) BindDefaultMaterial();	//?? will produce bad result, but only for one frame
				TexNum = DefaultTexNum;						// "default texture"; not working (see UTexture::Bind())
				break;
				//?? warning: DefaultTexNum is 2D texture, we need some default cubemap!
			}
		}
	}
	// bind texture
	glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, TexNum);

	unguard;
}


void UTextureCube::GetParams(CMaterialParams &Params) const
{
	Params.Cube = (UUnrealMaterial*)this;
}


void UTextureCube::Release()
{
	guard(UTextureCube::Release);
	if (GL_IsValidObject(TexNum, DrawTimestamp))
		glDeleteTextures(1, &TexNum);
	Super::Release();
	unguard;
}


void UMaterialInstanceConstant::SetupGL()
{
	// redirect to Parent until UMaterial3
	if (Parent) Parent->SetupGL();
}


void UMaterialInstanceConstant::GetParams(CMaterialParams &Params) const
{
	guard(UMaterialInstanceConstant::GetParams);

	// get params from linked UMaterial3
	if (Parent) Parent->GetParams(Params);

	Super::GetParams(Params);

	// get local parameters
	bool normalSet = false;
	int DiffWeight = 0, NormWeight = 0, SpecWeight = 0, SpecPowWeight = 0, OpWeight = 0, EmWeight = 0, EmcWeight = 0, CubeWeight = 0, MaskWeight = 0;

	if (TextureParameterValues.Num())
		Params.Opacity = NULL;			// it's better to disable opacity mask from parent material

	int i;
	for (i = 0; i < TextureParameterValues.Num(); i++)
	{
		const FTextureParameterValue &P = TextureParameterValues[i];
		const char *Name = P.ParameterName;
		UTexture3  *Tex  = P.ParameterValue;
		if (!Tex) continue;

		DIFFUSE (appStristr(Name, "dif"), 100);
		DIFFUSE (appStristr(Name, "color"), 80);
		NORMAL  (appStristr(Name, "norm") && !appStristr(Name, "fx"), 100);
		SPECPOW (appStristr(Name, "specpow"), 100);
		SPECULAR(appStristr(Name, "spec"), 100);
		EMISSIVE(appStristr(Name, "emiss"), 100);
		CUBEMAP (appStristr(Name, "cube"), 100);
		CUBEMAP (appStristr(Name, "refl"), 90);
		OPACITY (appStristr(Name, "opac"), 90);
		OPACITY (appStristr(Name, "trans") && !appStristr(Name, "transmission"), 80);
//??		OPACITY (appStristr(Name, "mask"), 100);
//??		Params.OpacityFromAlpha = true;
#if TRON
		if (Package->Game == GAME_Tron)
		{
			SPECPOW (appStristr(Name, "SPPW"), 100);
			EMISSIVE(appStristr(Name, "Emss"), 100);
			BAKEDMASK(appStristr(Name, "Mask"), 100);
		}
#endif // TRON
#if BATMAN
		if (Package->Game == GAME_Batman2)
		{
			BAKEDMASK(!stricmp(Name, "Material_Attributes"), 100);
			EMISSIVE (appStristr(Name, "Reflection_Mask"), 100);
		}
#endif // BATMAN
#if BLADENSOUL
		if (Package->Game == GAME_BladeNSoul)
		{
			BAKEDMASK(!stricmp(Name, "Body_mask_RGB"), 100);
		}
#endif // BLADENSOUL
#if DISHONORED
		if (Package->Game == GAME_Dishonored)
		{
			CUBEMAP (appStristr(Name, "cubemap_tex"), 100);
			EMISSIVE(appStristr(Name, "cubemap_mask"), 100);
		}
#endif // DISHONORED
	}
	for (i = 0; i < VectorParameterValues.Num(); i++)
	{
		const FVectorParameterValue &P = VectorParameterValues[i];
		const char *Name = P.ParameterName;
		const FLinearColor &Color = P.ParameterValue;
		EMISSIVE_COLOR(appStristr(Name, "Emissive"), 100);
#if TRON
		if (Package->Game == GAME_Tron)
		{
			EMISSIVE_COLOR(appStristr(Name, "PipingColour"), 90);
		}
#endif
	}

#if TRON
	if (Package->Game == GAME_Tron)
	{
		if (Params.Mask && Params.SpecPower && Params.Emissive)
			Params.Mask = NULL;		// some different meaning for this texture
		if (Params.Mask)
		{
			Params.EmissiveChannel      = TC_MA;
			Params.SpecularMaskChannel  = TC_G;
			Params.SpecularPowerChannel = TC_B;
			Params.CubemapMaskChannel   = TC_R;
		}
	}
#endif // TRON

#if BATMAN
	if (Package->Game == GAME_Batman2)
	{
		if (Params.Mask)
		{
			Params.SpecularMaskChannel  = TC_R;
			Params.SpecularPowerChannel = TC_G;
			// TC_B = skin mask
		}
	}
#endif // BATMAN

#if BLADENSOUL
	if (Package->Game == GAME_BladeNSoul)
	{
		if (Params.Mask)
		{
			Params.CubemapMaskChannel   = TC_B;
			Params.SpecularPowerChannel = TC_G;
			// TC_R = skin
		}
	}
#endif // BLADENSOUL

	// try to get diffuse texture when nothing found
	if (!Params.Diffuse && TextureParameterValues.Num() == 1)
		Params.Diffuse = TextureParameterValues[0].ParameterValue;

	unguard;
}


#endif // UNREAL3


#endif // RENDERING
