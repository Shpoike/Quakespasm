/* vim: set tabstop=4: */
/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2016      Spike

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//provides a few convienience extensions, primarily builtins, but also autocvars.
//Also note the set+seta features.


#include "../headers/quakedef.h"
#include "../headers/q_ctype.h"

static float PR_GetVMScale(void)
{	//sigh, this is horrible (divides glwidth)
	float s;
	if (qcvm == &cls.menu_qcvm)
	{
		s = q_min((float)glwidth / 320.0, (float)glheight / 200.0);
		s = CLAMP (1.0, scr_menuscale.value, s);
	}
	else
		s = CLAMP (1.0, scr_sbarscale.value, (float)glwidth / 320.0);
	return s;
}

//there's a few different aproaches to tempstrings...
//the lame way is to just have a single one (vanilla).
//the only slightly less lame way is to just cycle between 16 or so (most engines).
//one funky way is to allocate a single large buffer and just concatenate it for more tempstring space. don't forget to resize (dp).
//alternatively, just allocate them persistently and purge them only when there appear to be no more references to it (fte). makes strzone redundant.

extern cvar_t sv_gameplayfix_setmodelrealbox;
cvar_t pr_checkextension = {"pr_checkextension", "1", CVAR_NONE};	//spike - enables qc extensions. if 0 then they're ALL BLOCKED! MWAHAHAHA! *cough* *splutter*
static int pr_ext_warned_particleeffectnum;	//so these only spam once per map

static void *PR_FindExtGlobal(int type, const char *name);
void SV_CheckVelocity (edict_t *ent);

typedef enum multicast_e
{
	MULTICAST_ALL_U,
	MULTICAST_PHS_U,
	MULTICAST_PVS_U,
	MULTICAST_ALL_R,
	MULTICAST_PHS_R,
	MULTICAST_PVS_R,

	MULTICAST_ONE_U,
	MULTICAST_ONE_R,
	MULTICAST_INIT
} multicast_t;
static void SV_Multicast(multicast_t to, float *org, int msg_entity, unsigned int requireext2);

#define Z_StrDup(s) strcpy(Z_Malloc(strlen(s)+1), s)
#define	RETURN_EDICT(e) (((int *)qcvm->globals)[OFS_RETURN] = EDICT_TO_PROG(e))

int PR_MakeTempString (const char *val)
{
	char *tmp = PR_GetTempString();
	q_strlcpy(tmp, val, STRINGTEMP_LENGTH);
	return PR_SetEngineString(tmp);
}

#define ishex(c) ((c>='0' && c<= '9') || (c>='a' && c<='f') || (c>='A' && c<='F'))
static int dehex(char c)
{
	if (c >= '0' && c <= '9')
		return c-'0';
	if (c >= 'A' && c <= 'F')
		return c-('A'-10);
	if (c >= 'a' && c <= 'f')
		return c-('a'-10);
	return 0;
}
//returns the next char...
struct markup_s
{
	const unsigned char *txt;
	vec4_t tint;	//predefined colour that applies to the entire string
	vec4_t colour;	//colour for the specific glyph in question
	unsigned char mask;
};
void PR_Markup_Begin(struct markup_s *mu, const char *text, float *rgb, float alpha)
{
	if (*text == '\1' || *text == '\2')
	{
		mu->mask = 128;
		text++;
	}
	else
		mu->mask = 0;
	mu->txt = (const unsigned char *)text;
	VectorCopy(rgb, mu->tint);
	mu->tint[3] = alpha;
	VectorCopy(rgb, mu->colour);
	mu->colour[3] = alpha;
}
int PR_Markup_Parse(struct markup_s *mu)
{
	static const vec4_t q3rgb[10] = {
		{0.00,0.00,0.00, 1.0},
		{1.00,0.33,0.33, 1.0},
		{0.00,1.00,0.33, 1.0},
		{1.00,1.00,0.33, 1.0},
		{0.33,0.33,1.00, 1.0},
		{0.33,1.00,1.00, 1.0},
		{1.00,0.33,1.00, 1.0},
		{1.00,1.00,1.00, 1.0},
		{1.00,1.00,1.00, 0.5},
		{0.50,0.50,0.50, 1.0}
	};
	unsigned int c;
	const float *f;
	while ((c = *mu->txt))
	{
		if (c == '^' && pr_checkextension.value)
		{	//parse markup like FTE/DP might.
			switch(mu->txt[1])
			{
			case '^':	//doubled up char for escaping.
				mu->txt++;
				break;
			case '0':	//black
			case '1':	//red
			case '2':	//green
			case '3':	//yellow
			case '4':	//blue
			case '5':	//cyan
			case '6':	//magenta
			case '7':	//white
			case '8':	//white+half-alpha
			case '9':	//grey
				f = q3rgb[mu->txt[1]-'0'];
				mu->colour[0] = mu->tint[0] * f[0];
				mu->colour[1] = mu->tint[1] * f[1];
				mu->colour[2] = mu->tint[2] * f[2];
				mu->colour[3] = mu->tint[3] * f[3];
				mu->txt+=2;
				continue;
			case 'h':	//toggle half-alpha
				if (mu->colour[3] != mu->tint[3] * 0.5)
					mu->colour[3] = mu->tint[3] * 0.5;
				else
					mu->colour[3] = mu->tint[3];
				mu->txt+=2;
				continue;
			case 'd':	//reset to defaults (fixme: should reset ^m without resetting \1)
				mu->colour[0] = mu->tint[0];
				mu->colour[1] = mu->tint[1];
				mu->colour[2] = mu->tint[2];
				mu->colour[3] = mu->tint[3];
				mu->mask = 0;
				mu->txt+=2;
				break;
			case 'b':	//blink
			case 's':	//modstack push
			case 'r':	//modstack restore
				mu->txt+=2;
				continue;
			case 'x':	//RGB 12-bit colour
				if (ishex(mu->txt[2]) && ishex(mu->txt[3]) && ishex(mu->txt[4]))
				{
					mu->colour[0] = mu->tint[0] * dehex(mu->txt[2])/15.0;
					mu->colour[1] = mu->tint[1] * dehex(mu->txt[3])/15.0;
					mu->colour[2] = mu->tint[2] * dehex(mu->txt[4])/15.0;
					mu->txt+=5;
					continue;
				}
				break;	//malformed
			case '[':	//start fte's ^[text\key\value\key\value^] links
			case ']':	//end link
				break;	//fixme... skip the keys, recolour properly, etc
		//				txt+=2;
		//				continue;
			case '&':
				if ((ishex(mu->txt[2])||mu->txt[2]=='-') && (ishex(mu->txt[3])||mu->txt[3]=='-'))
				{	//ignore fte's fore/back ansi colours
					mu->txt += 4;
					continue;
				}
				break;	//malformed
			case 'a':	//alternate charset (read: masked)...
			case 'm':	//toggle masking.
				mu->txt+=2;
				mu->mask ^= 128;
				continue;
			case 'U':	//ucs-2 unicode codepoint
				if (ishex(mu->txt[2]) && ishex(mu->txt[3]) && ishex(mu->txt[4]) && ishex(mu->txt[5]))
				{
					c = (dehex(mu->txt[2])<<12) | (dehex(mu->txt[3])<<8) | (dehex(mu->txt[4])<<4) | dehex(mu->txt[5]);
					mu->txt += 6;

					if (c >= 0xe000 && c <= 0xe0ff)
						c &= 0xff;	//private-use 0xE0XX maps to quake's chars
					else if (c >= 0x20 && c <= 0x7f)
						c &= 0x7f;	//ascii is okay too.
					else
						c = '?'; //otherwise its some unicode char that we don't know how to handle.
					return c;
				}
				break; //malformed
			case '{':	//full unicode codepoint, for chars up to 0x10ffff
				mu->txt += 2;
				c = 0;	//no idea
				while(*mu->txt)
				{
					if (*mu->txt == '}')
					{
						mu->txt++;
						break;
					}
					if (!ishex(*mu->txt))
						break;
					c<<=4;
					c |= dehex(*mu->txt++);
				}

				if (c >= 0xe000 && c <= 0xe0ff)
					c &= 0xff;	//private-use 0xE0XX maps to quake's chars
				else if (c >= 0x20 && c <= 0x7f)
					c &= 0x7f;	//ascii is okay too.
				//it would be nice to include a table to de-accent latin scripts, as well as translate cyrilic somehow, but not really necessary.
				else
					c = '?'; //otherwise its some unicode char that we don't know how to handle.
				return c;
			}
		}

		//regular char
		mu->txt++;
		return c|mu->mask;
	}
	return 0;
}

#define D(typestr,desc) typestr,desc

//#define fixme

//maths stuff
static void PF_Sin(void)
{
	G_FLOAT(OFS_RETURN) = sin(G_FLOAT(OFS_PARM0));
}
static void PF_asin(void)
{
	G_FLOAT(OFS_RETURN) = asin(G_FLOAT(OFS_PARM0));
}
static void PF_Cos(void)
{
	G_FLOAT(OFS_RETURN) = cos(G_FLOAT(OFS_PARM0));
}
static void PF_acos(void)
{
	G_FLOAT(OFS_RETURN) = acos(G_FLOAT(OFS_PARM0));
}
static void PF_tan(void)
{
	G_FLOAT(OFS_RETURN) = tan(G_FLOAT(OFS_PARM0));
}
static void PF_atan(void)
{
	G_FLOAT(OFS_RETURN) = atan(G_FLOAT(OFS_PARM0));
}
static void PF_atan2(void)
{
	G_FLOAT(OFS_RETURN) = atan2(G_FLOAT(OFS_PARM0), G_FLOAT(OFS_PARM1));
}
static void PF_Sqrt(void)
{
	G_FLOAT(OFS_RETURN) = sqrt(G_FLOAT(OFS_PARM0));
}
static void PF_pow(void)
{
	G_FLOAT(OFS_RETURN) = pow(G_FLOAT(OFS_PARM0), G_FLOAT(OFS_PARM1));
}
static void PF_Logarithm(void)
{
	//log2(v) = ln(v)/ln(2)
	double r;
	r = log(G_FLOAT(OFS_PARM0));
	if (qcvm->argc > 1)
		r /= log(G_FLOAT(OFS_PARM1));
	G_FLOAT(OFS_RETURN) = r;
}
static void PF_mod(void)
{
	float a = G_FLOAT(OFS_PARM0);
	float n = G_FLOAT(OFS_PARM1);

	if (n == 0)
	{
		Con_DWarning("PF_mod: mod by zero\n");
		G_FLOAT(OFS_RETURN) = 0;
	}
	else
	{
		//because QC is inherantly floaty, lets use floats.
		G_FLOAT(OFS_RETURN) = a - (n * (int)(a/n));
	}
}
static void PF_min(void)
{
	float r = G_FLOAT(OFS_PARM0);
	int i;
	for (i = 1; i < qcvm->argc; i++)
	{
		if (r > G_FLOAT(OFS_PARM0 + i*3))
			r = G_FLOAT(OFS_PARM0 + i*3);
	}
	G_FLOAT(OFS_RETURN) = r;
}
static void PF_max(void)
{
	float r = G_FLOAT(OFS_PARM0);
	int i;
	for (i = 1; i < qcvm->argc; i++)
	{
		if (r < G_FLOAT(OFS_PARM0 + i*3))
			r = G_FLOAT(OFS_PARM0 + i*3);
	}
	G_FLOAT(OFS_RETURN) = r;
}
static void PF_bound(void)
{
	float minval = G_FLOAT(OFS_PARM0);
	float curval = G_FLOAT(OFS_PARM1);
	float maxval = G_FLOAT(OFS_PARM2);
	if (curval > maxval)
		curval = maxval;
	if (curval < minval)
		curval = minval;
	G_FLOAT(OFS_RETURN) = curval;
}
static void PF_anglemod(void)
{
	float v = G_FLOAT(OFS_PARM0);

	while (v >= 360)
		v = v - 360;
	while (v < 0)
		v = v + 360;

	G_FLOAT(OFS_RETURN) = v;
}
static void PF_bitshift(void)
{
	int bitmask = G_FLOAT(OFS_PARM0);
	int shift = G_FLOAT(OFS_PARM1);
	if (shift < 0)
		bitmask >>= -shift;
	else
		bitmask <<= shift;
	G_FLOAT(OFS_RETURN) = bitmask;
}
static void PF_crossproduct(void)
{
	CrossProduct(G_VECTOR(OFS_PARM0), G_VECTOR(OFS_PARM1), G_VECTOR(OFS_RETURN));
}
static void PF_vectorvectors(void)
{
	VectorCopy(G_VECTOR(OFS_PARM0), pr_global_struct->v_forward);
	VectorNormalize(pr_global_struct->v_forward);
	if (!pr_global_struct->v_forward[0] && !pr_global_struct->v_forward[1])
	{
		if (pr_global_struct->v_forward[2])
			pr_global_struct->v_right[1] = -1;
		else
			pr_global_struct->v_right[1] = 0;
		pr_global_struct->v_right[0] = pr_global_struct->v_right[2] = 0;
	}
	else
	{
		pr_global_struct->v_right[0] = pr_global_struct->v_forward[1];
		pr_global_struct->v_right[1] = -pr_global_struct->v_forward[0];
		pr_global_struct->v_right[2] = 0;
		VectorNormalize(pr_global_struct->v_right);
	}
	CrossProduct(pr_global_struct->v_right, pr_global_struct->v_forward, pr_global_struct->v_up);
}
static void PF_ext_vectoangles(void)
{	//alternative version of the original builtin, that can deal with roll angles too, by accepting an optional second argument for 'up'.
	float	*value1, *up;

	value1 = G_VECTOR(OFS_PARM0);
	if (qcvm->argc >= 2)
		up = G_VECTOR(OFS_PARM1);
	else
		up = NULL;

	VectorAngles(value1, up, G_VECTOR(OFS_RETURN));
	G_VECTOR(OFS_RETURN)[PITCH] *= -1;	//this builtin is for use with models. models have an inverted pitch. consistency with makevectors would never do!
}

//string stuff
static void PF_strlen(void)
{	//FIXME: doesn't try to handle utf-8
	const char *s = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = strlen(s);
}
static void PF_strcat(void)
{
	int		i;
	char *out = PR_GetTempString();
	size_t s;

	out[0] = 0;
	s = 0;
	for (i = 0; i < qcvm->argc; i++)
	{
		s = q_strlcat(out, G_STRING((OFS_PARM0+i*3)), STRINGTEMP_LENGTH);
		if (s >= STRINGTEMP_LENGTH)
		{
			Con_Warning("PF_strcat: overflow (string truncated)\n");
			break;
		}
	}

	G_INT(OFS_RETURN) = PR_SetEngineString(out);
}
static void PF_substring(void)
{
	int start, length, slen;
	const char *s;
	char *string;

	s = G_STRING(OFS_PARM0);
	start = G_FLOAT(OFS_PARM1);
	length = G_FLOAT(OFS_PARM2);

	slen = strlen(s);	//utf-8 should use chars, not bytes.

	if (start < 0)
		start = slen+start;
	if (length < 0)
		length = slen-start+(length+1);
	if (start < 0)
	{
	//	length += start;
		start = 0;
	}

	if (start >= slen || length<=0)
	{
		G_INT(OFS_RETURN) = PR_SetEngineString("");
		return;
	}

	slen -= start;
	if (length > slen)
		length = slen;
	//utf-8 should switch to bytes now.
	s += start;

	if (length >= STRINGTEMP_LENGTH)
	{
		length = STRINGTEMP_LENGTH-1;
		Con_Warning("PF_substring: truncation\n");
	}

	string = PR_GetTempString();
	memcpy(string, s, length);
	string[length] = '\0';
	G_INT(OFS_RETURN) = PR_SetEngineString(string);
}
/*our zoned strings implementation is somewhat specific to quakespasm, so good luck porting*/
static void PF_strzone(void)
{
	char *buf;
	size_t len = 0;
	const char *s[8];
	size_t l[8];
	int i;
	size_t id;

	for (i = 0; i < qcvm->argc; i++)
	{
		s[i] = G_STRING(OFS_PARM0+i*3);
		l[i] = strlen(s[i]);
		len += l[i];
	}
	len++; /*for the null*/

	buf = Z_Malloc(len);
	G_INT(OFS_RETURN) = PR_SetEngineString(buf);
	id = -1-G_INT(OFS_RETURN);
	if (id >= qcvm->knownzonesize)
	{
		qcvm->knownzonesize = (id+32)&~7;
		qcvm->knownzone = Z_Realloc(qcvm->knownzone, (qcvm->knownzonesize+7)>>3);
	}
	qcvm->knownzone[id>>3] |= 1u<<(id&7);

	for (i = 0; i < qcvm->argc; i++)
	{
		memcpy(buf, s[i], l[i]);
		buf += l[i];
	}
	*buf = '\0';
}
static void PF_strunzone(void)
{
	size_t id;
	const char *foo = G_STRING(OFS_PARM0);

	if (!G_INT(OFS_PARM0))
		return;	//don't bug out if they gave a null string
	id = -1-G_INT(OFS_PARM0);
	if (id < qcvm->knownzonesize && (qcvm->knownzone[id>>3] & (1u<<(id&7))))
	{
		qcvm->knownzone[id>>3] &= ~(1u<<(id&7));
		PR_ClearEngineString(G_INT(OFS_PARM0));
		Z_Free((void*)foo);
	}
	else
		Con_Warning("PF_strunzone: string wasn't strzoned\n");
}
static void PR_UnzoneAll(void)
{	//called to clean up all zoned strings.
	while (qcvm->knownzonesize --> 0)
	{
		size_t id = qcvm->knownzonesize;
		if (qcvm->knownzone[id>>3] & (1u<<(id&7)))
		{
			string_t s = -1-(int)id;
			char *ptr = (char*)PR_GetString(s);
			PR_ClearEngineString(s);
			Z_Free(ptr);
		}
	}
	if (qcvm->knownzone)
		Z_Free(qcvm->knownzone);
	qcvm->knownzonesize = 0;
	qcvm->knownzone = NULL;
}
static qboolean qc_isascii(unsigned int u)
{
	if (u < 256)	//should be just \n and 32-127, but we don't actually support any actual unicode and we don't really want to make things worse.
		return true;
	return false;
}
static void PF_str2chr(void)
{
	const char *instr = G_STRING(OFS_PARM0);
	int ofs = (qcvm->argc>1)?G_FLOAT(OFS_PARM1):0;

	if (ofs < 0)
		ofs = strlen(instr)+ofs;

	if (ofs && (ofs < 0 || ofs > (int)strlen(instr)))
		G_FLOAT(OFS_RETURN) = '\0';
	else
		G_FLOAT(OFS_RETURN) = (unsigned char)instr[ofs];
}
static void PF_chr2str(void)
{
	char *ret = PR_GetTempString(), *out;
	int i;
	for (i = 0, out=ret; out-ret < STRINGTEMP_LENGTH-6 && i < qcvm->argc; i++)
	{
		unsigned int u = G_FLOAT(OFS_PARM0 + i*3);
		if (u >= 0xe000 && u < 0xe100)
			*out++ = (unsigned char)u;	//quake chars.
		else if (qc_isascii(u))
			*out++ = u;
		else
			*out++ = '?';	//no unicode support
	}
	*out = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(ret);
}
//part of PF_strconv
static int chrconv_number(int i, int base, int conv)
{
	i -= base;
	switch (conv)
	{
	default:
	case 5:
	case 6:
	case 0:
		break;
	case 1:
		base = '0';
		break;
	case 2:
		base = '0'+128;
		break;
	case 3:
		base = '0'-30;
		break;
	case 4:
		base = '0'+128-30;
		break;
	}
	return i + base;
}
//part of PF_strconv
static int chrconv_punct(int i, int base, int conv)
{
	i -= base;
	switch (conv)
	{
	default:
	case 0:
		break;
	case 1:
		base = 0;
		break;
	case 2:
		base = 128;
		break;
	}
	return i + base;
}
//part of PF_strconv
static int chrchar_alpha(int i, int basec, int baset, int convc, int convt, int charnum)
{
	//convert case and colour seperatly...

	i -= baset + basec;
	switch (convt)
	{
	default:
	case 0:
		break;
	case 1:
		baset = 0;
		break;
	case 2:
		baset = 128;
		break;

	case 5:
	case 6:
		baset = 128*((charnum&1) == (convt-5));
		break;
	}

	switch (convc)
	{
	default:
	case 0:
		break;
	case 1:
		basec = 'a';
		break;
	case 2:
		basec = 'A';
		break;
	}
	return i + basec + baset;
}
//FTE_STRINGS
//bulk convert a string. change case or colouring.
static void PF_strconv (void)
{
	int ccase = G_FLOAT(OFS_PARM0);		//0 same, 1 lower, 2 upper
	int redalpha = G_FLOAT(OFS_PARM1);	//0 same, 1 white, 2 red,  5 alternate, 6 alternate-alternate
	int rednum = G_FLOAT(OFS_PARM2);	//0 same, 1 white, 2 red, 3 redspecial, 4 whitespecial, 5 alternate, 6 alternate-alternate
	const unsigned char *string = (const unsigned char*)PF_VarString(3);
	int len = strlen((const char*)string);
	int i;
	unsigned char *resbuf = (unsigned char*)PR_GetTempString();
	unsigned char *result = resbuf;

	//UTF-8-FIXME: cope with utf+^U etc

	if (len >= STRINGTEMP_LENGTH)
		len = STRINGTEMP_LENGTH-1;

	for (i = 0; i < len; i++, string++, result++)	//should this be done backwards?
	{
		if (*string >= '0' && *string <= '9')	//normal numbers...
			*result = chrconv_number(*string, '0', rednum);
		else if (*string >= '0'+128 && *string <= '9'+128)
			*result = chrconv_number(*string, '0'+128, rednum);
		else if (*string >= '0'+128-30 && *string <= '9'+128-30)
			*result = chrconv_number(*string, '0'+128-30, rednum);
		else if (*string >= '0'-30 && *string <= '9'-30)
			*result = chrconv_number(*string, '0'-30, rednum);

		else if (*string >= 'a' && *string <= 'z')	//normal numbers...
			*result = chrchar_alpha(*string, 'a', 0, ccase, redalpha, i);
		else if (*string >= 'A' && *string <= 'Z')	//normal numbers...
			*result = chrchar_alpha(*string, 'A', 0, ccase, redalpha, i);
		else if (*string >= 'a'+128 && *string <= 'z'+128)	//normal numbers...
			*result = chrchar_alpha(*string, 'a', 128, ccase, redalpha, i);
		else if (*string >= 'A'+128 && *string <= 'Z'+128)	//normal numbers...
			*result = chrchar_alpha(*string, 'A', 128, ccase, redalpha, i);

		else if ((*string & 127) < 16 || !redalpha)	//special chars..
			*result = *string;
		else if (*string < 128)
			*result = chrconv_punct(*string, 0, redalpha);
		else
			*result = chrconv_punct(*string, 128, redalpha);
	}
	*result = '\0';

	G_INT(OFS_RETURN) = PR_SetEngineString((char*)resbuf);
}
static void PF_strpad(void)
{
	char *destbuf = PR_GetTempString();
	char *dest = destbuf;
	int pad = G_FLOAT(OFS_PARM0);
	const char *src = PF_VarString(1);

	//UTF-8-FIXME: pad is chars not bytes...

	if (pad < 0)
	{	//pad left
		pad = -pad - strlen(src);
		if (pad>=STRINGTEMP_LENGTH)
			pad = STRINGTEMP_LENGTH-1;
		if (pad < 0)
			pad = 0;

		q_strlcpy(dest+pad, src, STRINGTEMP_LENGTH-pad);
		while(pad)
		{
			dest[--pad] = ' ';
		}
	}
	else
	{	//pad right
		if (pad>=STRINGTEMP_LENGTH)
			pad = STRINGTEMP_LENGTH-1;
		pad -= strlen(src);
		if (pad < 0)
			pad = 0;

		q_strlcpy(dest, src, STRINGTEMP_LENGTH);
		dest+=strlen(dest);

		while(pad-->0)
			*dest++ = ' ';
		*dest = '\0';
	}

	G_INT(OFS_RETURN) = PR_SetEngineString(destbuf);
}
static void PF_infoadd(void)
{
	const char *info = G_STRING(OFS_PARM0);
	const char *key = G_STRING(OFS_PARM1);
	const char *value = PF_VarString(2);
	char *destbuf = PR_GetTempString(), *o = destbuf, *e = destbuf + STRINGTEMP_LENGTH - 1;

	size_t keylen = strlen(key);
	size_t valuelen = strlen(value);
	if (!*key)
	{	//error
		G_INT(OFS_RETURN) = G_INT(OFS_PARM0);
		return;
	}

	//copy the string to the output, stripping the named key
	while(*info)
	{
		const char *l = info;
		if (*info++ != '\\')
			break;	//error / end-of-string

		if (!strncmp(info, key, keylen) && info[keylen] == '\\')
		{
			//skip the key name
			info += keylen+1;
			//this is the old value for the key. skip over it
			while (*info && *info != '\\')
				info++;
		}
		else
		{
			//skip the key
			while (*info && *info != '\\')
				info++;

			//validate that its a value now
			if (*info++ != '\\')
				break;	//error
			//skip the value
			while (*info && *info != '\\')
				info++;

			//copy them over
			if (o + (info-l) >= e)
				break;	//exceeds maximum length
			while (l < info)
				*o++ = *l++;
		}
	}

	if (*info)
		Con_Warning("PF_infoadd: invalid source info\n");
	else if (!*value)
		; //nothing needed
	else if (!*key || strchr(key, '\\') || strchr(value, '\\'))
		Con_Warning("PF_infoadd: invalid key/value\n");
	else if (o + 2 + keylen + valuelen >= e)
		Con_Warning("PF_infoadd: length exceeds max\n");
	else
	{
		*o++ = '\\';
		memcpy(o, key, keylen);
		o += keylen;
		*o++ = '\\';
		memcpy(o, value, valuelen);
		o += valuelen;
	}

	*o = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(destbuf);
}
static void PF_infoget(void)
{
	const char *info = G_STRING(OFS_PARM0);
	const char *key = G_STRING(OFS_PARM1);
	size_t keylen = strlen(key);
	while(*info)
	{
		if (*info++ != '\\')
			break;	//error / end-of-string

		if (!strncmp(info, key, keylen) && info[keylen] == '\\')
		{
			char *destbuf = PR_GetTempString(), *o = destbuf, *e = destbuf + STRINGTEMP_LENGTH - 1;

			//skip the key name
			info += keylen+1;
			//this is the old value for the key. copy it to the result
			while (*info && *info != '\\' && o < e)
				*o++ = *info++;
			*o++ = 0;

			//success!
			G_INT(OFS_RETURN) = PR_SetEngineString(destbuf);
			return;
		}
		else
		{
			//skip the key
			while (*info && *info != '\\')
				info++;

			//validate that its a value now
			if (*info++ != '\\')
				break;	//error
			//skip the value
			while (*info && *info != '\\')
				info++;
		}
	}
	G_INT(OFS_RETURN) = 0;

}
static void PF_strncmp(void)
{
	const char *a = G_STRING(OFS_PARM0);
	const char *b = G_STRING(OFS_PARM1);

	if (qcvm->argc > 2)
	{
		int len = G_FLOAT(OFS_PARM2);
		int aofs = qcvm->argc>3?G_FLOAT(OFS_PARM3):0;
		int bofs = qcvm->argc>4?G_FLOAT(OFS_PARM4):0;
		if (aofs < 0 || (aofs && aofs > (int)strlen(a)))
			aofs = strlen(a);
		if (bofs < 0 || (bofs && bofs > (int)strlen(b)))
			bofs = strlen(b);
		G_FLOAT(OFS_RETURN) = Q_strncmp(a + aofs, b, len);
	}
	else
		G_FLOAT(OFS_RETURN) = Q_strcmp(a, b);
}
static void PF_strncasecmp(void)
{
	const char *a = G_STRING(OFS_PARM0);
	const char *b = G_STRING(OFS_PARM1);

	if (qcvm->argc > 2)
	{
		int len = G_FLOAT(OFS_PARM2);
		int aofs = qcvm->argc>3?G_FLOAT(OFS_PARM3):0;
		int bofs = qcvm->argc>4?G_FLOAT(OFS_PARM4):0;
		if (aofs < 0 || (aofs && aofs > (int)strlen(a)))
			aofs = strlen(a);
		if (bofs < 0 || (bofs && bofs > (int)strlen(b)))
			bofs = strlen(b);
		G_FLOAT(OFS_RETURN) = q_strncasecmp(a + aofs, b, len);
	}
	else
		G_FLOAT(OFS_RETURN) = q_strcasecmp(a, b);
}
static void PF_strstrofs(void)
{
	const char *instr = G_STRING(OFS_PARM0);
	const char *match = G_STRING(OFS_PARM1);
	int firstofs = (qcvm->argc>2)?G_FLOAT(OFS_PARM2):0;

	if (firstofs && (firstofs < 0 || firstofs > (int)strlen(instr)))
	{
		G_FLOAT(OFS_RETURN) = -1;
		return;
	}

	match = strstr(instr+firstofs, match);
	if (!match)
		G_FLOAT(OFS_RETURN) = -1;
	else
		G_FLOAT(OFS_RETURN) = match - instr;
}
static void PF_strtrim(void)
{
	const char *str = G_STRING(OFS_PARM0);
	const char *end;
	char *news;
	size_t len;

	//figure out the new start
	while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')
		str++;

	//figure out the new end.
	end = str + strlen(str);
	while(end > str && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
		end--;

	//copy that substring into a tempstring.
	len = end - str;
	if (len >= STRINGTEMP_LENGTH)
		len = STRINGTEMP_LENGTH-1;

	news = PR_GetTempString();
	memcpy(news, str, len);
	news[len] = 0;

	G_INT(OFS_RETURN) = PR_SetEngineString(news);
}
static void PF_strreplace(void)
{
	char *resultbuf = PR_GetTempString();
	char *result = resultbuf;
	const char *search = G_STRING(OFS_PARM0);
	const char *replace = G_STRING(OFS_PARM1);
	const char *subject = G_STRING(OFS_PARM2);
	int searchlen = strlen(search);
	int replacelen = strlen(replace);

	if (searchlen)
	{
		while (*subject && result < resultbuf + STRINGTEMP_LENGTH - replacelen - 2)
		{
			if (!strncmp(subject, search, searchlen))
			{
				subject += searchlen;
				memcpy(result, replace, replacelen);
				result += replacelen;
			}
			else
				*result++ = *subject++;
		}
		*result = 0;
		G_INT(OFS_RETURN) = PR_SetEngineString(resultbuf);
	}
	else
		G_INT(OFS_RETURN) = PR_SetEngineString(subject);
}
static void PF_strireplace(void)
{
	char *resultbuf = PR_GetTempString();
	char *result = resultbuf;
	const char *search = G_STRING(OFS_PARM0);
	const char *replace = G_STRING(OFS_PARM1);
	const char *subject = G_STRING(OFS_PARM2);
	int searchlen = strlen(search);
	int replacelen = strlen(replace);

	if (searchlen)
	{
		while (*subject && result < resultbuf + sizeof(resultbuf) - replacelen - 2)
		{
			//UTF-8-FIXME: case insensitivity is awkward...
			if (!q_strncasecmp(subject, search, searchlen))
			{
				subject += searchlen;
				memcpy(result, replace, replacelen);
				result += replacelen;
			}
			else
				*result++ = *subject++;
		}
		*result = 0;
		G_INT(OFS_RETURN) = PR_SetEngineString(resultbuf);
	}
	else
		G_INT(OFS_RETURN) = PR_SetEngineString(subject);
}


static void PF_sprintf_internal (const char *s, int firstarg, char *outbuf, int outbuflen)
{
	const char *s0;
	char *o = outbuf, *end = outbuf + outbuflen, *err;
	int width, precision, thisarg, flags;
	char formatbuf[16];
	char *f;
	int argpos = firstarg;
	int isfloat;
	static int dummyivec[3] = {0, 0, 0};
	static float dummyvec[3] = {0, 0, 0};

#define PRINTF_ALTERNATE 1
#define PRINTF_ZEROPAD 2
#define PRINTF_LEFT 4
#define PRINTF_SPACEPOSITIVE 8
#define PRINTF_SIGNPOSITIVE 16

	formatbuf[0] = '%';

#define GETARG_FLOAT(a) (((a)>=firstarg && (a)<qcvm->argc) ? (G_FLOAT(OFS_PARM0 + 3 * (a))) : 0)
#define GETARG_VECTOR(a) (((a)>=firstarg && (a)<qcvm->argc) ? (G_VECTOR(OFS_PARM0 + 3 * (a))) : dummyvec)
#define GETARG_INT(a) (((a)>=firstarg && (a)<qcvm->argc) ? (G_INT(OFS_PARM0 + 3 * (a))) : 0)
#define GETARG_INTVECTOR(a) (((a)>=firstarg && (a)<qcvm->argc) ? ((int*) G_VECTOR(OFS_PARM0 + 3 * (a))) : dummyivec)
#define GETARG_STRING(a) (((a)>=firstarg && (a)<qcvm->argc) ? (G_STRING(OFS_PARM0 + 3 * (a))) : "")

	for(;;)
	{
		s0 = s;
		switch(*s)
		{
			case 0:
				goto finished;
			case '%':
				++s;

				if(*s == '%')
					goto verbatim;

				// complete directive format:
				// %3$*1$.*2$ld

				width = -1;
				precision = -1;
				thisarg = -1;
				flags = 0;
				isfloat = -1;

				// is number following?
				if(*s >= '0' && *s <= '9')
				{
					width = strtol(s, &err, 10);
					if(!err)
					{
						Con_Warning("PF_sprintf: bad format string: %s\n", s0);
						goto finished;
					}
					if(*err == '$')
					{
						thisarg = width + (firstarg-1);
						width = -1;
						s = err + 1;
					}
					else
					{
						if(*s == '0')
						{
							flags |= PRINTF_ZEROPAD;
							if(width == 0)
								width = -1; // it was just a flag
						}
						s = err;
					}
				}

				if(width < 0)
				{
					for(;;)
					{
						switch(*s)
						{
							case '#': flags |= PRINTF_ALTERNATE; break;
							case '0': flags |= PRINTF_ZEROPAD; break;
							case '-': flags |= PRINTF_LEFT; break;
							case ' ': flags |= PRINTF_SPACEPOSITIVE; break;
							case '+': flags |= PRINTF_SIGNPOSITIVE; break;
							default:
								goto noflags;
						}
						++s;
					}
noflags:
					if(*s == '*')
					{
						++s;
						if(*s >= '0' && *s <= '9')
						{
							width = strtol(s, &err, 10);
							if(!err || *err != '$')
							{
								Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
								goto finished;
							}
							s = err + 1;
						}
						else
							width = argpos++;
						width = GETARG_FLOAT(width);
						if(width < 0)
						{
							flags |= PRINTF_LEFT;
							width = -width;
						}
					}
					else if(*s >= '0' && *s <= '9')
					{
						width = strtol(s, &err, 10);
						if(!err)
						{
							Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
							goto finished;
						}
						s = err;
						if(width < 0)
						{
							flags |= PRINTF_LEFT;
							width = -width;
						}
					}
					// otherwise width stays -1
				}

				if(*s == '.')
				{
					++s;
					if(*s == '*')
					{
						++s;
						if(*s >= '0' && *s <= '9')
						{
							precision = strtol(s, &err, 10);
							if(!err || *err != '$')
							{
								Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
								goto finished;
							}
							s = err + 1;
						}
						else
							precision = argpos++;
						precision = GETARG_FLOAT(precision);
					}
					else if(*s >= '0' && *s <= '9')
					{
						precision = strtol(s, &err, 10);
						if(!err)
						{
							Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
							goto finished;
						}
						s = err;
					}
					else
					{
						Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
						goto finished;
					}
				}

				for(;;)
				{
					switch(*s)
					{
						case 'h': isfloat = 1; break;
						case 'l': isfloat = 0; break;
						case 'L': isfloat = 0; break;
						case 'j': break;
						case 'z': break;
						case 't': break;
						default:
							goto nolength;
					}
					++s;
				}
nolength:

				// now s points to the final directive char and is no longer changed
				if (*s == 'p' || *s == 'P')
				{
					//%p is slightly different from %x.
					//always 8-bytes wide with 0 padding, always ints.
					flags |= PRINTF_ZEROPAD;
					if (width < 0) width = 8;
					if (isfloat < 0) isfloat = 0;
				}
				else if (*s == 'i')
				{
					//%i defaults to ints, not floats.
					if(isfloat < 0) isfloat = 0;
				}

				//assume floats, not ints.
				if(isfloat < 0)
					isfloat = 1;

				if(thisarg < 0)
					thisarg = argpos++;

				if(o < end - 1)
				{
					f = &formatbuf[1];
					if(*s != 's' && *s != 'c')
						if(flags & PRINTF_ALTERNATE) *f++ = '#';
					if(flags & PRINTF_ZEROPAD) *f++ = '0';
					if(flags & PRINTF_LEFT) *f++ = '-';
					if(flags & PRINTF_SPACEPOSITIVE) *f++ = ' ';
					if(flags & PRINTF_SIGNPOSITIVE) *f++ = '+';
					*f++ = '*';
					if(precision >= 0)
					{
						*f++ = '.';
						*f++ = '*';
					}
					if (*s == 'p')
						*f++ = 'x';
					else if (*s == 'P')
						*f++ = 'X';
					else if (*s == 'S')
						*f++ = 's';
					else
						*f++ = *s;
					*f++ = 0;

					if(width < 0) // not set
						width = 0;

					switch(*s)
					{
						case 'd': case 'i':
							if(precision < 0) // not set
								q_snprintf(o, end - o, formatbuf, width, (isfloat ? (int) GETARG_FLOAT(thisarg) : (int) GETARG_INT(thisarg)));
							else
								q_snprintf(o, end - o, formatbuf, width, precision, (isfloat ? (int) GETARG_FLOAT(thisarg) : (int) GETARG_INT(thisarg)));
							o += strlen(o);
							break;
						case 'o': case 'u': case 'x': case 'X': case 'p': case 'P':
							if(precision < 0) // not set
								q_snprintf(o, end - o, formatbuf, width, (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) : (unsigned int) GETARG_INT(thisarg)));
							else
								q_snprintf(o, end - o, formatbuf, width, precision, (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) : (unsigned int) GETARG_INT(thisarg)));
							o += strlen(o);
							break;
						case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
							if(precision < 0) // not set
								q_snprintf(o, end - o, formatbuf, width, (isfloat ? (double) GETARG_FLOAT(thisarg) : (double) GETARG_INT(thisarg)));
							else
								q_snprintf(o, end - o, formatbuf, width, precision, (isfloat ? (double) GETARG_FLOAT(thisarg) : (double) GETARG_INT(thisarg)));
							o += strlen(o);
							break;
						case 'v': case 'V':
							f[-2] += 'g' - 'v';
							if(precision < 0) // not set
								q_snprintf(o, end - o, va("%s %s %s", /* NESTED SPRINTF IS NESTED */ formatbuf, formatbuf, formatbuf),
									width, (isfloat ? (double) GETARG_VECTOR(thisarg)[0] : (double) GETARG_INTVECTOR(thisarg)[0]),
									width, (isfloat ? (double) GETARG_VECTOR(thisarg)[1] : (double) GETARG_INTVECTOR(thisarg)[1]),
									width, (isfloat ? (double) GETARG_VECTOR(thisarg)[2] : (double) GETARG_INTVECTOR(thisarg)[2])
								);
							else
								q_snprintf(o, end - o, va("%s %s %s", /* NESTED SPRINTF IS NESTED */ formatbuf, formatbuf, formatbuf),
									width, precision, (isfloat ? (double) GETARG_VECTOR(thisarg)[0] : (double) GETARG_INTVECTOR(thisarg)[0]),
									width, precision, (isfloat ? (double) GETARG_VECTOR(thisarg)[1] : (double) GETARG_INTVECTOR(thisarg)[1]),
									width, precision, (isfloat ? (double) GETARG_VECTOR(thisarg)[2] : (double) GETARG_INTVECTOR(thisarg)[2])
								);
							o += strlen(o);
							break;
						case 'c':
							//UTF-8-FIXME: figure it out yourself
//							if(flags & PRINTF_ALTERNATE)
							{
								if(precision < 0) // not set
									q_snprintf(o, end - o, formatbuf, width, (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) : (unsigned int) GETARG_INT(thisarg)));
								else
									q_snprintf(o, end - o, formatbuf, width, precision, (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) : (unsigned int) GETARG_INT(thisarg)));
								o += strlen(o);
							}
/*							else
							{
								unsigned int c = (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) : (unsigned int) GETARG_INT(thisarg));
								char charbuf16[16];
								const char *buf = u8_encodech(c, NULL, charbuf16);
								if(!buf)
									buf = "";
								if(precision < 0) // not set
									precision = end - o - 1;
								o += u8_strpad(o, end - o, buf, (flags & PRINTF_LEFT) != 0, width, precision);
							}
*/							break;
						case 'S':
							{	//tokenizable string
								const char *quotedarg = GETARG_STRING(thisarg);

								//try and escape it... hopefully it won't get truncated by precision limits...
								char quotedbuf[65536];
								size_t l;
								l = strlen(quotedarg);
								if (strchr(quotedarg, '\"') || strchr(quotedarg, '\n') || strchr(quotedarg, '\r') || l+3 >= sizeof(quotedbuf))
								{	//our escapes suck...
									Con_Warning("PF_sprintf: unable to safely escape arg: %s\n", s0);
									quotedarg="";
								}
								quotedbuf[0] = '\"';
								memcpy(quotedbuf+1, quotedarg, l);
								quotedbuf[1+l] = '\"';
								quotedbuf[1+l+1] = 0;
								quotedarg = quotedbuf;

								//UTF-8-FIXME: figure it out yourself
//								if(flags & PRINTF_ALTERNATE)
								{
									if(precision < 0) // not set
										q_snprintf(o, end - o, formatbuf, width, quotedarg);
									else
										q_snprintf(o, end - o, formatbuf, width, precision, quotedarg);
									o += strlen(o);
								}
/*								else
								{
									if(precision < 0) // not set
										precision = end - o - 1;
									o += u8_strpad(o, end - o, quotedarg, (flags & PRINTF_LEFT) != 0, width, precision);
								}
*/							}
							break;
						case 's':
							//UTF-8-FIXME: figure it out yourself
//							if(flags & PRINTF_ALTERNATE)
							{
								if(precision < 0) // not set
									q_snprintf(o, end - o, formatbuf, width, GETARG_STRING(thisarg));
								else
									q_snprintf(o, end - o, formatbuf, width, precision, GETARG_STRING(thisarg));
								o += strlen(o);
							}
/*							else
							{
								if(precision < 0) // not set
									precision = end - o - 1;
								o += u8_strpad(o, end - o, GETARG_STRING(thisarg), (flags & PRINTF_LEFT) != 0, width, precision);
							}
*/							break;
						default:
							Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
							goto finished;
					}
				}
				++s;
				break;
			default:
verbatim:
				if(o < end - 1)
					*o++ = *s;
				s++;
				break;
		}
	}
finished:
	*o = 0;
}

static void PF_sprintf(void)
{
	char *outbuf = PR_GetTempString();
	PF_sprintf_internal(G_STRING(OFS_PARM0), 1, outbuf, STRINGTEMP_LENGTH);
	G_INT(OFS_RETURN) = PR_SetEngineString(outbuf);
}

//string tokenizing (gah)
#define MAXQCTOKENS 64
static struct {
	char *token;
	unsigned int start;
	unsigned int end;
} qctoken[MAXQCTOKENS];
static unsigned int qctoken_count;

static void tokenize_flush(void)
{
	while(qctoken_count > 0)
	{
		qctoken_count--;
		free(qctoken[qctoken_count].token);
	}
	qctoken_count = 0;
}

static void PF_ArgC(void)
{
	G_FLOAT(OFS_RETURN) = qctoken_count;
}

static int tokenizeqc(const char *str, qboolean dpfuckage)
{
	//FIXME: if dpfuckage, then we should handle punctuation specially, as well as /*.
	const char *start = str;
	while(qctoken_count > 0)
	{
		qctoken_count--;
		free(qctoken[qctoken_count].token);
	}
	qctoken_count = 0;
	while (qctoken_count < MAXQCTOKENS)
	{
		/*skip whitespace here so the token's start is accurate*/
		while (*str && *(const unsigned char*)str <= ' ')
			str++;

		if (!*str)
			break;

		qctoken[qctoken_count].start = str - start;
//		if (dpfuckage)
//			str = COM_ParseDPFuckage(str);
//		else
			str = COM_Parse(str);
		if (!str)
			break;

		qctoken[qctoken_count].token = strdup(com_token);

		qctoken[qctoken_count].end = str - start;
		qctoken_count++;
	}
	return qctoken_count;
}

/*KRIMZON_SV_PARSECLIENTCOMMAND added these two - note that for compatibility with DP, this tokenize builtin is veeery vauge and doesn't match the console*/
static void PF_Tokenize(void)
{
	G_FLOAT(OFS_RETURN) = tokenizeqc(G_STRING(OFS_PARM0), true);
}

static void PF_tokenize_console(void)
{
	G_FLOAT(OFS_RETURN) = tokenizeqc(G_STRING(OFS_PARM0), false);
}

static void PF_tokenizebyseparator(void)
{
	const char *str = G_STRING(OFS_PARM0);
	const char *sep[7];
	int seplen[7];
	int seps = 0, s;
	const char *start = str;
	int tlen;
	qboolean found = true;

	while (seps < qcvm->argc - 1 && seps < 7)
	{
		sep[seps] = G_STRING(OFS_PARM1 + seps*3);
		seplen[seps] = strlen(sep[seps]);
		seps++;
	}

	tokenize_flush();

	qctoken[qctoken_count].start = 0;
	if (*str)
	for(;;)
	{
		found = false;
		/*see if its a separator*/
		if (!*str)
		{
			qctoken[qctoken_count].end = str - start;
			found = true;
		}
		else
		{
			for (s = 0; s < seps; s++)
			{
				if (!strncmp(str, sep[s], seplen[s]))
				{
					qctoken[qctoken_count].end = str - start;
					str += seplen[s];
					found = true;
					break;
				}
			}
		}
		/*it was, split it out*/
		if (found)
		{
			tlen = qctoken[qctoken_count].end - qctoken[qctoken_count].start;
			qctoken[qctoken_count].token = malloc(tlen + 1);
			memcpy(qctoken[qctoken_count].token, start + qctoken[qctoken_count].start, tlen);
			qctoken[qctoken_count].token[tlen] = 0;

			qctoken_count++;

			if (*str && qctoken_count < MAXQCTOKENS)
				qctoken[qctoken_count].start = str - start;
			else
				break;
		}
		str++;
	}
	G_FLOAT(OFS_RETURN) = qctoken_count;
}

static void PF_argv_start_index(void)
{
	int idx = G_FLOAT(OFS_PARM0);

	/*negative indexes are relative to the end*/
	if (idx < 0)
		idx += qctoken_count;

	if ((unsigned int)idx >= qctoken_count)
		G_FLOAT(OFS_RETURN) = -1;
	else
		G_FLOAT(OFS_RETURN) = qctoken[idx].start;
}

static void PF_argv_end_index(void)
{
	int idx = G_FLOAT(OFS_PARM0);

	/*negative indexes are relative to the end*/
	if (idx < 0)
		idx += qctoken_count;

	if ((unsigned int)idx >= qctoken_count)
		G_FLOAT(OFS_RETURN) = -1;
	else
		G_FLOAT(OFS_RETURN) = qctoken[idx].end;
}

static void PF_ArgV(void)
{
	int idx = G_FLOAT(OFS_PARM0);

	/*negative indexes are relative to the end*/
	if (idx < 0)
		idx += qctoken_count;

	if ((unsigned int)idx >= qctoken_count)
		G_INT(OFS_RETURN) = 0;
	else
	{
		char *ret = PR_GetTempString();
		q_strlcpy(ret, qctoken[idx].token, STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(ret);
	}
}

//conversions (mostly string)
static void PF_strtoupper(void)
{
	const char *in = G_STRING(OFS_PARM0);
	char *out, *result = PR_GetTempString();
	for (out = result; *in && out < result+STRINGTEMP_LENGTH-1;)
		*out++ = q_toupper(*in++);
	*out = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_strtolower(void)
{
	const char *in = G_STRING(OFS_PARM0);
	char *out, *result = PR_GetTempString();
	for (out = result; *in && out < result+STRINGTEMP_LENGTH-1;)
		*out++ = q_tolower(*in++);
	*out = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
#include <time.h>
static void PF_strftime(void)
{
	const char *in = G_STRING(OFS_PARM1);
	char *result = PR_GetTempString();

	time_t ctime;
	struct tm *tm;

	ctime = time(NULL);

	if (G_FLOAT(OFS_PARM0))
		tm = localtime(&ctime);
	else
		tm = gmtime(&ctime);

#ifdef _WIN32
	//msvc sucks. this is a crappy workaround.
	if (!strcmp(in, "%R"))
		in = "%H:%M";
	else if (!strcmp(in, "%F"))
		in = "%Y-%m-%d";
#endif

	strftime(result, STRINGTEMP_LENGTH, in, tm);

	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_stof(void)
{
	G_FLOAT(OFS_RETURN) = atof(G_STRING(OFS_PARM0));
}
static void PF_stov(void)
{
	const char *s = G_STRING(OFS_PARM0);
	s = COM_Parse(s);
	G_VECTOR(OFS_RETURN)[0] = atof(com_token);
	s = COM_Parse(s);
	G_VECTOR(OFS_RETURN)[1] = atof(com_token);
	s = COM_Parse(s);
	G_VECTOR(OFS_RETURN)[2] = atof(com_token);
}
static void PF_stoi(void)
{
	G_INT(OFS_RETURN) = atoi(G_STRING(OFS_PARM0));
}
static void PF_itos(void)
{
	char *result = PR_GetTempString();
	q_snprintf(result, STRINGTEMP_LENGTH, "%i", G_INT(OFS_PARM0));
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_etos(void)
{	//yes, this is lame
	char *result = PR_GetTempString();
	q_snprintf(result, STRINGTEMP_LENGTH, "entity %i", G_EDICTNUM(OFS_PARM0));
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_stoh(void)
{
	G_INT(OFS_RETURN) = strtoul(G_STRING(OFS_PARM0), NULL, 16);
}
static void PF_htos(void)
{
	char *result = PR_GetTempString();
	q_snprintf(result, STRINGTEMP_LENGTH, "%x", G_INT(OFS_PARM0));
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_ftoi(void)
{
	G_INT(OFS_RETURN) = G_FLOAT(OFS_PARM0);
}
static void PF_itof(void)
{
	G_FLOAT(OFS_RETURN) = G_INT(OFS_PARM0);
}

//collision stuff
static void PF_tracebox(void)
{	//alternative version of traceline that just passes on two extra args. trivial really.
	float	*v1, *mins, *maxs, *v2;
	trace_t	trace;
	int	nomonsters;
	edict_t	*ent;

	v1 = G_VECTOR(OFS_PARM0);
	mins = G_VECTOR(OFS_PARM1);
	maxs = G_VECTOR(OFS_PARM2);
	v2 = G_VECTOR(OFS_PARM3);
	nomonsters = G_FLOAT(OFS_PARM4);
	ent = G_EDICT(OFS_PARM5);

	/* FIXME FIXME FIXME: Why do we hit this with certain progs.dat ?? */
	if (developer.value) {
	  if (IS_NAN(v1[0]) || IS_NAN(v1[1]) || IS_NAN(v1[2]) ||
	      IS_NAN(v2[0]) || IS_NAN(v2[1]) || IS_NAN(v2[2])) {
	    Con_Warning ("NAN in traceline:\nv1(%f %f %f) v2(%f %f %f)\nentity %d\n",
		      v1[0], v1[1], v1[2], v2[0], v2[1], v2[2], NUM_FOR_EDICT(ent));
	  }
	}

	if (IS_NAN(v1[0]) || IS_NAN(v1[1]) || IS_NAN(v1[2]))
		v1[0] = v1[1] = v1[2] = 0;
	if (IS_NAN(v2[0]) || IS_NAN(v2[1]) || IS_NAN(v2[2]))
		v2[0] = v2[1] = v2[2] = 0;

	trace = SV_Move (v1, mins, maxs, v2, nomonsters, ent);

	pr_global_struct->trace_allsolid = trace.allsolid;
	pr_global_struct->trace_startsolid = trace.startsolid;
	pr_global_struct->trace_fraction = trace.fraction;
	pr_global_struct->trace_inwater = trace.inwater;
	pr_global_struct->trace_inopen = trace.inopen;
	VectorCopy (trace.endpos, pr_global_struct->trace_endpos);
	VectorCopy (trace.plane.normal, pr_global_struct->trace_plane_normal);
	pr_global_struct->trace_plane_dist =  trace.plane.dist;
	if (trace.ent)
		pr_global_struct->trace_ent = EDICT_TO_PROG(trace.ent);
	else
		pr_global_struct->trace_ent = EDICT_TO_PROG(qcvm->edicts);
}
static void PF_TraceToss(void)
{
	extern cvar_t sv_maxvelocity, sv_gravity;
	int i;
	float gravity;
	vec3_t move, end;
	trace_t trace;
	eval_t	*val;

	vec3_t origin, velocity;

	edict_t *tossent, *ignore;
	tossent = G_EDICT(OFS_PARM0);
	if (tossent == qcvm->edicts)
		Con_Warning("tracetoss: can not use world entity\n");
	ignore = G_EDICT(OFS_PARM1);

	val = GetEdictFieldValue(tossent, qcvm->extfields.gravity);
	if (val && val->_float)
		gravity = val->_float;
	else
		gravity = 1;
	gravity *= sv_gravity.value * 0.05;

	VectorCopy (tossent->v.origin, origin);
	VectorCopy (tossent->v.velocity, velocity);

	SV_CheckVelocity (tossent);

	for (i = 0;i < 200;i++) // LordHavoc: sanity check; never trace more than 10 seconds
	{
		velocity[2] -= gravity;
		VectorScale (velocity, 0.05, move);
		VectorAdd (origin, move, end);
		trace = SV_Move (origin, tossent->v.mins, tossent->v.maxs, end, MOVE_NORMAL, tossent);
		VectorCopy (trace.endpos, origin);

		if (trace.fraction < 1 && trace.ent && trace.ent != ignore)
			break;

		if (VectorLength(velocity) > sv_maxvelocity.value)
		{
//			Con_DPrintf("Slowing %s\n", PR_GetString(w->progs, tossent->v->classname));
			VectorScale (velocity, sv_maxvelocity.value/VectorLength(velocity), velocity);
		}
	}

	trace.fraction = 0; // not relevant

	//and return those as globals.
	pr_global_struct->trace_allsolid = trace.allsolid;
	pr_global_struct->trace_startsolid = trace.startsolid;
	pr_global_struct->trace_fraction = trace.fraction;
	pr_global_struct->trace_inwater = trace.inwater;
	pr_global_struct->trace_inopen = trace.inopen;
	VectorCopy (trace.endpos, pr_global_struct->trace_endpos);
	VectorCopy (trace.plane.normal, pr_global_struct->trace_plane_normal);
	pr_global_struct->trace_plane_dist =  trace.plane.dist;
	if (trace.ent)
		pr_global_struct->trace_ent = EDICT_TO_PROG(trace.ent);
	else
		pr_global_struct->trace_ent = EDICT_TO_PROG(qcvm->edicts);
}

//model stuff
void SetMinMaxSize (edict_t *e, float *minvec, float *maxvec, qboolean rotate);
static void PF_sv_setmodelindex(void)
{
	edict_t	*e			= G_EDICT(OFS_PARM0);
	unsigned int newidx	= G_FLOAT(OFS_PARM1);
	qmodel_t *mod = qcvm->GetModel(newidx);
	e->v.model = (newidx<MAX_MODELS)?PR_SetEngineString(sv.model_precache[newidx]):0;
	e->v.modelindex = newidx;

	if (mod)
	//johnfitz -- correct physics cullboxes for bmodels
	{
		if (mod->type == mod_brush || !sv_gameplayfix_setmodelrealbox.value)
			SetMinMaxSize (e, mod->clipmins, mod->clipmaxs, true);
		else
			SetMinMaxSize (e, mod->mins, mod->maxs, true);
	}
	//johnfitz
	else
		SetMinMaxSize (e, vec3_origin, vec3_origin, true);
}
static void PF_cl_setmodelindex(void)
{
	edict_t	*e		= G_EDICT(OFS_PARM0);
	int newidx		= G_FLOAT(OFS_PARM1);
	qmodel_t *mod = qcvm->GetModel(newidx);
	e->v.model = mod?PR_SetEngineString(mod->name):0;	//FIXME: is this going to cause issues with vid_restart?
	e->v.modelindex = newidx;

	if (mod)
	//johnfitz -- correct physics cullboxes for bmodels
	{
		if (mod->type == mod_brush || !sv_gameplayfix_setmodelrealbox.value)
			SetMinMaxSize (e, mod->clipmins, mod->clipmaxs, true);
		else
			SetMinMaxSize (e, mod->mins, mod->maxs, true);
	}
	//johnfitz
	else
		SetMinMaxSize (e, vec3_origin, vec3_origin, true);
}

static void PF_frameforname(void)
{
	unsigned int modelindex	= G_FLOAT(OFS_PARM0);
	const char *framename	= G_STRING(OFS_PARM1);
	qmodel_t *mod = qcvm->GetModel(modelindex);
	aliashdr_t *alias;

	G_FLOAT(OFS_RETURN) = -1;
	if (mod && mod->type == mod_alias && (alias = Mod_Extradata(mod)))
	{
		int i;
		for (i = 0; i < alias->numframes; i++)
		{
			if (!strcmp(alias->frames[i].name, framename))
			{
				G_FLOAT(OFS_RETURN) = i;
				break;
			}
		}
	}
}
static void PF_frametoname(void)
{
	unsigned int modelindex	= G_FLOAT(OFS_PARM0);
	unsigned int framenum	= G_FLOAT(OFS_PARM1);
	qmodel_t *mod = qcvm->GetModel(modelindex);
	aliashdr_t *alias;

	if (mod && mod->type == mod_alias && (alias = Mod_Extradata(mod)) && framenum < (unsigned int)alias->numframes)
		G_INT(OFS_RETURN) = PR_SetEngineString(alias->frames[framenum].name);
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_frameduration(void)
{
	unsigned int modelindex	= G_FLOAT(OFS_PARM0);
	unsigned int framenum	= G_FLOAT(OFS_PARM1);
	qmodel_t *mod = qcvm->GetModel(modelindex);
	aliashdr_t *alias;

	if (mod && mod->type == mod_alias && (alias = Mod_Extradata(mod)) && framenum < (unsigned int)alias->numframes)
		G_FLOAT(OFS_RETURN) = alias->frames[framenum].numposes * alias->frames[framenum].interval;
}
static void PF_getsurfacenumpoints(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int modelindex = ed->v.modelindex;
	qmodel_t *mod = qcvm->GetModel(modelindex);

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces)
	{
		surfidx += mod->firstmodelsurface;
		G_FLOAT(OFS_RETURN) = mod->surfaces[surfidx].numedges;
	}
	else
		G_FLOAT(OFS_RETURN) = 0;
}
static mvertex_t *PF_getsurfacevertex(qmodel_t *mod, msurface_t *surf, unsigned int vert)
{
	signed int edge = mod->surfedges[vert+surf->firstedge];
	if (edge >= 0)
		return &mod->vertexes[mod->edges[edge].v[0]];
	else
		return &mod->vertexes[mod->edges[-edge].v[1]];
}
static void PF_getsurfacepoint(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int point		= G_FLOAT(OFS_PARM2);
	qmodel_t *mod = qcvm->GetModel(ed->v.modelindex);

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces && point < (unsigned int)mod->surfaces[surfidx].numedges)
	{
		mvertex_t *v = PF_getsurfacevertex(mod, &mod->surfaces[surfidx+mod->firstmodelsurface], point);
		VectorCopy(v->position, G_VECTOR(OFS_RETURN));
	}
	else
	{
		G_FLOAT(OFS_RETURN+0) = 0;
		G_FLOAT(OFS_RETURN+1) = 0;
		G_FLOAT(OFS_RETURN+2) = 0;
	}
}
static void PF_getsurfacenumtriangles(void)
{	//for q3bsp compat (which this engine doesn't support, so its fairly simple)
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	qmodel_t *mod = qcvm->GetModel(ed->v.modelindex);

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces)
		G_FLOAT(OFS_RETURN) = (mod->surfaces[surfidx+mod->firstmodelsurface].numedges-2);	//q1bsp is only triangle fans
	else
		G_FLOAT(OFS_RETURN) = 0;
}
static void PF_getsurfacetriangle(void)
{	//for q3bsp compat (which this engine doesn't support, so its fairly simple)
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int triangleidx= G_FLOAT(OFS_PARM2);
	qmodel_t *mod = qcvm->GetModel(ed->v.modelindex);

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces && triangleidx < (unsigned int)mod->surfaces[surfidx].numedges-2)
	{
		G_FLOAT(OFS_RETURN+0) = 0;
		G_FLOAT(OFS_RETURN+1) = triangleidx+1;
		G_FLOAT(OFS_RETURN+2) = triangleidx+2;
	}
	else
	{
		G_FLOAT(OFS_RETURN+0) = 0;
		G_FLOAT(OFS_RETURN+1) = 0;
		G_FLOAT(OFS_RETURN+2) = 0;
	}
}
static void PF_getsurfacenormal(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	qmodel_t *mod = qcvm->GetModel(ed->v.modelindex);

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces)
	{
		surfidx += mod->firstmodelsurface;
		VectorCopy(mod->surfaces[surfidx].plane->normal, G_VECTOR(OFS_RETURN));
		if (mod->surfaces[surfidx].flags & SURF_PLANEBACK)
			VectorInverse(G_VECTOR(OFS_RETURN));
	}
	else
		G_FLOAT(OFS_RETURN) = 0;
}
static void PF_getsurfacetexture(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	qmodel_t *mod = qcvm->GetModel(ed->v.modelindex);

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces)
	{
		surfidx += mod->firstmodelsurface;
		G_INT(OFS_RETURN) = PR_SetEngineString(mod->surfaces[surfidx].texinfo->texture->name);
	}
	else
		G_INT(OFS_RETURN) = 0;
}

#define TriangleNormal(a,b,c,n) ( \
	(n)[0] = ((a)[1] - (b)[1]) * ((c)[2] - (b)[2]) - ((a)[2] - (b)[2]) * ((c)[1] - (b)[1]), \
	(n)[1] = ((a)[2] - (b)[2]) * ((c)[0] - (b)[0]) - ((a)[0] - (b)[0]) * ((c)[2] - (b)[2]), \
	(n)[2] = ((a)[0] - (b)[0]) * ((c)[1] - (b)[1]) - ((a)[1] - (b)[1]) * ((c)[0] - (b)[0]) \
	)
static float getsurface_clippointpoly(qmodel_t *model, msurface_t *surf, vec3_t point, vec3_t bestcpoint, float bestdist)
{
	int e, edge;
	vec3_t edgedir, edgenormal, cpoint, temp;
	mvertex_t *v1, *v2;
	float dist = DotProduct(point, surf->plane->normal) - surf->plane->dist;
	//don't care about SURF_PLANEBACK, the maths works out the same.

	if (dist*dist < bestdist)
	{	//within a specific range
		//make sure it's within the poly
		VectorMA(point, dist, surf->plane->normal, cpoint);
		for (e = surf->firstedge+surf->numedges; e > surf->firstedge; edge++)
		{
			edge = model->surfedges[--e];
			if (edge < 0)
			{
				v1 = &model->vertexes[model->edges[-edge].v[0]];
				v2 = &model->vertexes[model->edges[-edge].v[1]];
			}
			else
			{
				v2 = &model->vertexes[model->edges[edge].v[0]];
				v1 = &model->vertexes[model->edges[edge].v[1]];
			}

			VectorSubtract(v1->position, v2->position, edgedir);
			CrossProduct(edgedir, surf->plane->normal, edgenormal);
			if (!(surf->flags & SURF_PLANEBACK))
			{
				VectorSubtract(vec3_origin, edgenormal, edgenormal);
			}
			VectorNormalize(edgenormal);

			dist = DotProduct(v1->position, edgenormal) - DotProduct(cpoint, edgenormal);
			if (dist < 0)
				VectorMA(cpoint, dist, edgenormal, cpoint);
		}

		VectorSubtract(cpoint, point, temp);
		dist = DotProduct(temp, temp);
		if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(cpoint, bestcpoint);
		}
	}
	return bestdist;
}

// #438 float(entity e, vector p) getsurfacenearpoint (DP_QC_GETSURFACE)
static void PF_getsurfacenearpoint(void)
{
	qmodel_t *model;
	edict_t *ent;
	msurface_t *surf;
	int i;
	float *point;

	vec3_t cpoint = {0,0,0};
	float bestdist = 0x7fffffff, dist;
	int bestsurf = -1;

	ent = G_EDICT(OFS_PARM0);
	point = G_VECTOR(OFS_PARM1);

	G_FLOAT(OFS_RETURN) = -1;

	model = qcvm->GetModel(ent->v.modelindex);

	if (!model || model->type != mod_brush || model->needload)
		return;

	bestdist = 256;

	//all polies, we can skip parts. special case.
	surf = model->surfaces + model->firstmodelsurface;
	for (i = 0; i < model->nummodelsurfaces; i++, surf++)
	{
		dist = getsurface_clippointpoly(model, surf, point, cpoint, bestdist);
		if (dist < bestdist)
		{
			bestdist = dist;
			bestsurf = i;
		}
	}
	G_FLOAT(OFS_RETURN) = bestsurf;
}

// #439 vector(entity e, float s, vector p) getsurfaceclippedpoint (DP_QC_GETSURFACE)
static void PF_getsurfaceclippedpoint(void)
{
	qmodel_t *model;
	edict_t *ent;
	msurface_t *surf;
	float *point;
	int surfnum;

	float *result = G_VECTOR(OFS_RETURN);

	ent = G_EDICT(OFS_PARM0);
	surfnum = G_FLOAT(OFS_PARM1);
	point = G_VECTOR(OFS_PARM2);

	VectorCopy(point, result);

	model = qcvm->GetModel(ent->v.modelindex);

	if (!model || model->type != mod_brush || model->needload)
		return;
	if (surfnum >= model->nummodelsurfaces)
		return;

	//all polies, we can skip parts. special case.
	surf = model->surfaces + model->firstmodelsurface + surfnum;
	getsurface_clippointpoly(model, surf, point, result, 0x7fffffff);
}

enum
{
	SPA_POSITION	= 0,
	SPA_S_AXIS		= 1,
	SPA_T_AXIS		= 2,
	SPA_R_AXIS		= 3,	//normal
	SPA_TEXCOORDS0	= 4,
	SPA_LIGHTMAP0_TEXCOORDS	= 5,
	SPA_LIGHTMAP0_COLOR		= 6,
};
static void PF_getsurfacepointattribute(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int point		= G_FLOAT(OFS_PARM2);
	unsigned int attribute	= G_FLOAT(OFS_PARM3);

	qmodel_t *mod = qcvm->GetModel(ed->v.modelindex);

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces && point < (unsigned int)mod->surfaces[mod->firstmodelsurface+surfidx].numedges)
	{
		msurface_t *fa = &mod->surfaces[surfidx+mod->firstmodelsurface];
		mvertex_t *v = PF_getsurfacevertex(mod, fa, point);
		switch(attribute)
		{
		default:
			Con_Warning("PF_getsurfacepointattribute: attribute %u not supported\n", attribute);
			G_FLOAT(OFS_RETURN+0) = 0;
			G_FLOAT(OFS_RETURN+1) = 0;
			G_FLOAT(OFS_RETURN+2) = 0;
			break;
		case SPA_POSITION:
			VectorCopy(v->position, G_VECTOR(OFS_RETURN));
			break;
		case SPA_S_AXIS:
		case SPA_T_AXIS:
			{
				//figure out how similar to the normal it is, and negate any influence, so that its perpendicular
				float sc = -DotProduct(fa->plane->normal, fa->texinfo->vecs[attribute-1]);
				VectorMA(fa->texinfo->vecs[attribute-1], sc, fa->plane->normal, G_VECTOR(OFS_RETURN));
				VectorNormalize(G_VECTOR(OFS_RETURN));
			}
			break;
		case SPA_R_AXIS: //normal
			VectorCopy(fa->plane->normal, G_VECTOR(OFS_RETURN));
			if (fa->flags & SURF_PLANEBACK)
				VectorInverse(G_VECTOR(OFS_RETURN));
			break;
		case SPA_TEXCOORDS0:
			G_FLOAT(OFS_RETURN+0) = (DotProduct(v->position, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3]) / fa->texinfo->texture->width;
			G_FLOAT(OFS_RETURN+1) = (DotProduct(v->position, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3]) / fa->texinfo->texture->height;
			G_FLOAT(OFS_RETURN+2) = 0;
			break;
		case SPA_LIGHTMAP0_TEXCOORDS: //lmst coord, not actually very useful
			G_FLOAT(OFS_RETURN+0) = (DotProduct(v->position, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3] - fa->texturemins[0] + (fa->light_s+.5)*(1<<fa->lmshift)) / (LMBLOCK_WIDTH*(1<<fa->lmshift));
			G_FLOAT(OFS_RETURN+1) = (DotProduct(v->position, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3] - fa->texturemins[1] + (fa->light_t+.5)*(1<<fa->lmshift)) / (LMBLOCK_HEIGHT*(1<<fa->lmshift));
			G_FLOAT(OFS_RETURN+2) = 0;
			break;
		case SPA_LIGHTMAP0_COLOR: //colour
			G_FLOAT(OFS_RETURN+0) = 1;
			G_FLOAT(OFS_RETURN+1) = 1;
			G_FLOAT(OFS_RETURN+2) = 1;
			break;
		}
	}
	else
	{
		G_FLOAT(OFS_RETURN+0) = 0;
		G_FLOAT(OFS_RETURN+1) = 0;
		G_FLOAT(OFS_RETURN+2) = 0;
	}
}
static void PF_sv_getlight(void)
{
	qmodel_t *om = cl.worldmodel;
	float *point = G_VECTOR(OFS_PARM0);

	cl.worldmodel = qcvm->worldmodel;	//R_LightPoint is really clientside, so if its called from ssqc then try to make things work regardless
										//FIXME: d_lightstylevalue isn't set on dedicated servers

	//FIXME: seems like quakespasm doesn't do lits for model lighting, so we won't either.
	G_FLOAT(OFS_RETURN+0) = G_FLOAT(OFS_RETURN+1) = G_FLOAT(OFS_RETURN+2) = R_LightPoint(point) / 255.0;

	cl.worldmodel = om;
}
#define PF_cl_getlight PF_sv_getlight

//server/client stuff
static void PF_checkcommand(void)
{
	const char *name = G_STRING(OFS_PARM0);
	if (Cmd_Exists(name))
		G_FLOAT(OFS_RETURN) = 1;
	else if (Cmd_AliasExists(name))
		G_FLOAT(OFS_RETURN) = 2;
	else if (Cvar_FindVar(name))
		G_FLOAT(OFS_RETURN) = 3;
	else
		G_FLOAT(OFS_RETURN) = 0;
}
static void PF_setcolors(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	int newcol				= G_FLOAT(OFS_PARM1);
	unsigned int i			= NUM_FOR_EDICT(ed)-1;
	if (i >= (unsigned int)svs.maxclients)
	{
		Con_Printf ("tried to setcolor a non-client\n");
		return;
	}

	SV_UpdateInfo(i+1, "topcolor",    va("%i", (newcol>>4)&0xf));
	SV_UpdateInfo(i+1, "bottomcolor", va("%i", (newcol>>0)&0xf));
}
static void PF_clientcommand(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	const char *str			= G_STRING(OFS_PARM1);
	unsigned int i			= NUM_FOR_EDICT(ed)-1;
	if (i < (unsigned int)svs.maxclients && svs.clients[i].active)
	{
		client_t *ohc = host_client;
		host_client = &svs.clients[i];
		Cmd_ExecuteString (str, src_client);
		host_client = ohc;
	}
	else
		Con_Printf("PF_clientcommand: not a client\n");
}
static void PF_clienttype(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int i			= NUM_FOR_EDICT(ed)-1;
	if (i >= (unsigned int)svs.maxclients)
	{
		G_FLOAT(OFS_RETURN) = 3;	//CLIENTTYPE_NOTACLIENT
		return;
	}
	if (svs.clients[i].active)
	{
		if (svs.clients[i].netconnection)
			G_FLOAT(OFS_RETURN) = 1;//CLIENTTYPE_REAL;
		else
			G_FLOAT(OFS_RETURN) = 2;//CLIENTTYPE_BOT;
	}
	else
		G_FLOAT(OFS_RETURN) = 0;//CLIENTTYPE_DISCONNECTED;
}
static void PF_spawnclient(void)
{
	edict_t *ent;
	unsigned int i;
	if (svs.maxclients)
	for (i = svs.maxclients; i --> 0; )
	{
		if (!svs.clients[i].active)
		{
			svs.clients[i].netconnection = NULL;	//botclients have no net connection, obviously.
			SV_ConnectClient(i);
			svs.clients[i].spawned = true;
			ent = svs.clients[i].edict;
			memset (&ent->v, 0, qcvm->progs->entityfields * 4);
			ent->v.colormap = NUM_FOR_EDICT(ent);
			ent->v.team = (svs.clients[i].colors & 15) + 1;
			ent->v.netname = PR_SetEngineString(svs.clients[i].name);
			RETURN_EDICT(ent);
			return;
		}
	}
	RETURN_EDICT(qcvm->edicts);
}
static void PF_dropclient(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int i			= NUM_FOR_EDICT(ed)-1;
	if (i < (unsigned int)svs.maxclients && svs.clients[i].active)
	{	//FIXME: should really set a flag or something, to avoid recursion issues.
		client_t *ohc = host_client;
		host_client = &svs.clients[i];
		SV_DropClient (false);
		host_client = ohc;
	}
}

//console/cvar stuff
static void PF_print(void)
{
	int i;
	for (i = 0; i < qcvm->argc; i++)
		Con_Printf("%s", G_STRING(OFS_PARM0 + i*3));
}
static void PF_cvar_string(void)
{
	const char *name = G_STRING(OFS_PARM0);
	cvar_t *var = Cvar_FindVar(name);
	if (var && var->string)
	{
		//cvars can easily change values.
		//this would result in leaks/exploits/slowdowns if the qc spams calls to cvar_string+changes.
		//so keep performance consistent, even if this is going to be slower.
		char *temp = PR_GetTempString();
		q_strlcpy(temp, var->string, STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(temp);
	}
	else if (!strcmp(name, "game"))
	{	//game looks like a cvar in most other respects (and is a cvar in fte). let cvar_string work on it as a way to find out the current gamedir.
		char *temp = PR_GetTempString();
		q_strlcpy(temp, COM_GetGameNames(true), STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(temp);
	}
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_cvar_defstring(void)
{
	const char *name = G_STRING(OFS_PARM0);
	cvar_t *var = Cvar_FindVar(name);
	if (var && var->default_string)
		G_INT(OFS_RETURN) = PR_SetEngineString(var->default_string);
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_cvar_type(void)
{
	const char	*str = G_STRING(OFS_PARM0);
	int ret = 0;
	cvar_t *v;

	v = Cvar_FindVar(str);
	if (v)
	{
		ret |= 1; // CVAR_EXISTS
		if(v->flags & CVAR_ARCHIVE)
			ret |= 2; // CVAR_TYPE_SAVED
//		if(v->flags & CVAR_NOTFROMSERVER)
//			ret |= 4; // CVAR_TYPE_PRIVATE
		if(!(v->flags & CVAR_USERDEFINED))
			ret |= 8; // CVAR_TYPE_ENGINE
//		if (v->description)
//			ret |= 16; // CVAR_TYPE_HASDESCRIPTION
	}
	G_FLOAT(OFS_RETURN) = ret;
}
static void PF_cvar_description(void)
{	//quakespasm does not support cvar descriptions. we provide this stub to avoid crashes.
	G_INT(OFS_RETURN) = 0;
}
static void PF_registercvar(void)
{
	const char *name = G_STRING(OFS_PARM0);
	const char *value = (qcvm->argc>1)?G_STRING(OFS_PARM0):"";
	Cvar_Create(name, value);
}

//temp entities + networking
static void PF_WriteString2(void)
{	//writes a string without the null. a poor-man's strcat.
	const char *string = G_STRING(OFS_PARM0);
	SZ_Write (WriteDest(), string, Q_strlen(string));
}
static void PF_WriteFloat(void)
{	//curiously, this was missing in vanilla.
	MSG_WriteFloat(WriteDest(), G_FLOAT(OFS_PARM0));
}
static void PF_sv_te_blooddp(void)
{	//blood is common enough that we should emulate it for when engines do actually support it.
	float *org = G_VECTOR(OFS_PARM0);
	float *dir = G_VECTOR(OFS_PARM1);
	float color = 73;
	float count = G_FLOAT(OFS_PARM2);
	SV_StartParticle (org, dir, color, count);
}
static void PF_sv_te_bloodqw(void)
{	//qw tried to strip a lot.
	float *org = G_VECTOR(OFS_PARM0);
	float *dir = vec3_origin;
	float color = 73;
	float count = G_FLOAT(OFS_PARM1)*20;
	SV_StartParticle (org, dir, color, count);
}
static void PF_sv_te_lightningblood(void)
{	//a qw builtin, to replace particle.
	float *org = G_VECTOR(OFS_PARM0);
	vec3_t dir = {0, 0, -100};
	float color = 20;
	float count = 225;
	SV_StartParticle (org, dir, color, count);
}
static void PF_sv_te_spike(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_SPIKE);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PVS_U, org, 0, 0);
}
static void PF_cl_te_spike(void)
{
	float *pos = G_VECTOR(OFS_PARM0);

	if (PScript_RunParticleEffectTypeString(pos, NULL, 1, "TE_SPIKE"))
		R_RunParticleEffect (pos, vec3_origin, 0, 10);
	if ( rand() % 5 )
		S_StartSound (-1, 0, S_PrecacheSound ("weapons/tink1.wav"), pos, 1, 1);
	else
	{
		int rnd = rand() & 3;
		if (rnd == 1)
			S_StartSound (-1, 0, S_PrecacheSound ("weapons/ric1.wav"), pos, 1, 1);
		else if (rnd == 2)
			S_StartSound (-1, 0, S_PrecacheSound ("weapons/ric2.wav"), pos, 1, 1);
		else
			S_StartSound (-1, 0, S_PrecacheSound ("weapons/ric3.wav"), pos, 1, 1);
	}
}
static void PF_sv_te_superspike(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_SUPERSPIKE);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PVS_U, org, 0, 0);
}
static void PF_cl_te_superspike(void)
{
	float *pos = G_VECTOR(OFS_PARM0);

	if (PScript_RunParticleEffectTypeString(pos, NULL, 1, "TE_SUPERSPIKE"))
		R_RunParticleEffect (pos, vec3_origin, 0, 20);

	if ( rand() % 5 )
		S_StartSound (-1, 0, S_PrecacheSound ("weapons/tink1.wav"), pos, 1, 1);
	else
	{
		int rnd = rand() & 3;
		if (rnd == 1)
			S_StartSound (-1, 0, S_PrecacheSound ("weapons/ric1.wav"), pos, 1, 1);
		else if (rnd == 2)
			S_StartSound (-1, 0, S_PrecacheSound ("weapons/ric2.wav"), pos, 1, 1);
		else
			S_StartSound (-1, 0, S_PrecacheSound ("weapons/ric3.wav"), pos, 1, 1);
	}
}
static void PF_sv_te_gunshot(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	//float count = G_FLOAT(OFS_PARM1)*20;
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_GUNSHOT);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PVS_U, org, 0, 0);
}
static void PF_cl_te_gunshot(void)
{
	float *pos = G_VECTOR(OFS_PARM0);

	int rnd = 20;
	if (PScript_RunParticleEffectTypeString(pos, NULL, rnd, "TE_GUNSHOT"))
		R_RunParticleEffect (pos, vec3_origin, 0, rnd);
}
static void PF_sv_te_explosion(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_EXPLOSION);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_cl_te_explosion(void)
{
	float *pos = G_VECTOR(OFS_PARM0);

	dlight_t *dl;
	if (PScript_RunParticleEffectTypeString(pos, NULL, 1, "TE_EXPLOSION"))
		R_ParticleExplosion (pos);
	dl = CL_AllocDlight (0);
	VectorCopy (pos, dl->origin);
	dl->radius = 350;
	dl->die = cl.time + 0.5;
	dl->decay = 300;
	S_StartSound (-1, 0, S_PrecacheSound ("weapons/r_exp3.wav"), pos, 1, 1);
}
static void PF_sv_te_tarexplosion(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_TAREXPLOSION);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_cl_te_tarexplosion(void)
{
	float *pos = G_VECTOR(OFS_PARM0);

	if (PScript_RunParticleEffectTypeString(pos, NULL, 1, "TE_TAREXPLOSION"))
		R_BlobExplosion (pos);
	S_StartSound (-1, 0, S_PrecacheSound ("weapons/r_exp3.wav"), pos, 1, 1);
}
static void PF_sv_te_lightning1(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_LIGHTNING1);
	MSG_WriteShort(&sv.datagram, NUM_FOR_EDICT(ed));
	MSG_WriteCoord(&sv.datagram, start[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[2], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
static void PF_cl_te_lightning1(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);

	CL_UpdateBeam (Mod_ForName("progs/bolt.mdl", true), "TE_LIGHTNING1", "TE_LIGHTNING1_END", -NUM_FOR_EDICT(ed), start, end);
}
static void PF_sv_te_lightning2(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_LIGHTNING2);
	MSG_WriteShort(&sv.datagram, NUM_FOR_EDICT(ed));
	MSG_WriteCoord(&sv.datagram, start[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[2], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
static void PF_cl_te_lightning2(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);

	CL_UpdateBeam (Mod_ForName("progs/bolt2.mdl", true), "TE_LIGHTNING2", "TE_LIGHTNING2_END", -NUM_FOR_EDICT(ed), start, end);
}
static void PF_sv_te_wizspike(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_WIZSPIKE);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_cl_te_wizspike(void)
{
	float *pos = G_VECTOR(OFS_PARM0);

	if (PScript_RunParticleEffectTypeString(pos, NULL, 1, "TE_WIZSPIKE"))
		R_RunParticleEffect (pos, vec3_origin, 20, 30);
	S_StartSound (-1, 0, S_PrecacheSound ("wizard/hit.wav"), pos, 1, 1);
}
static void PF_sv_te_knightspike(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_KNIGHTSPIKE);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_cl_te_knightspike(void)
{
	float *pos = G_VECTOR(OFS_PARM0);

	if (PScript_RunParticleEffectTypeString(pos, NULL, 1, "TE_KNIGHTSPIKE"))
		R_RunParticleEffect (pos, vec3_origin, 226, 20);
	S_StartSound (-1, 0, S_PrecacheSound ("hknight/hit.wav"), pos, 1, 1);
}
static void PF_sv_te_lightning3(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_LIGHTNING3);
	MSG_WriteShort(&sv.datagram, NUM_FOR_EDICT(ed));
	MSG_WriteCoord(&sv.datagram, start[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[2], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
static void PF_cl_te_lightning3(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);

	CL_UpdateBeam (Mod_ForName("progs/bolt3.mdl", true), "TE_LIGHTNING3", "TE_LIGHTNING3_END", -NUM_FOR_EDICT(ed), start, end);
}
static void PF_sv_te_lavasplash(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_LAVASPLASH);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_cl_te_lavasplash(void)
{
	float *pos = G_VECTOR(OFS_PARM0);

	if (PScript_RunParticleEffectTypeString(pos, NULL, 1, "TE_LAVASPLASH"))
		R_LavaSplash (pos);
}
static void PF_sv_te_teleport(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.multicast, svc_temp_entity);
	MSG_WriteByte(&sv.multicast, TE_TELEPORT);
	MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_cl_te_teleport(void)
{
	float *pos = G_VECTOR(OFS_PARM0);
	if (PScript_RunParticleEffectTypeString(pos, NULL, 1, "TE_TELEPORT"))
		R_TeleportSplash (pos);
}
static void PF_sv_te_explosion2(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	int palstart = G_FLOAT(OFS_PARM1);
	int palcount = G_FLOAT(OFS_PARM1);
	MSG_WriteByte(&sv.multicast, svc_temp_entity);
	MSG_WriteByte(&sv.multicast, TE_EXPLOSION2);
	MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
	MSG_WriteByte(&sv.multicast, palstart);
	MSG_WriteByte(&sv.multicast, palcount);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_cl_te_explosion2(void)
{
	float *pos = G_VECTOR(OFS_PARM0);
	int colorStart = G_FLOAT(OFS_PARM1);
	int colorLength = G_FLOAT(OFS_PARM1);
	dlight_t *dl;

	if (PScript_RunParticleEffectTypeString(pos, NULL, 1, va("TE_EXPLOSION2_%i_%i", colorStart, colorLength)))
		R_ParticleExplosion2 (pos, colorStart, colorLength);
	dl = CL_AllocDlight (0);
	VectorCopy (pos, dl->origin);
	dl->radius = 350;
	dl->die = cl.time + 0.5;
	dl->decay = 300;
	S_StartSound (-1, 0, S_PrecacheSound ("weapons/r_exp3.wav"), pos, 1, 1);
}
static void PF_sv_te_beam(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);
	MSG_WriteByte(&sv.multicast, svc_temp_entity);
	MSG_WriteByte(&sv.multicast, TE_BEAM);
	MSG_WriteShort(&sv.multicast, NUM_FOR_EDICT(ed));
	MSG_WriteCoord(&sv.multicast, start[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, start[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, start[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
static void PF_cl_te_beam(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);

	CL_UpdateBeam (Mod_ForName("progs/beam.mdl", true), "TE_BEAM", "TE_BEAM_END", -NUM_FOR_EDICT(ed), start, end);
}
#ifdef PSET_SCRIPT
static void PF_sv_te_particlerain(void)
{
	float *min = G_VECTOR(OFS_PARM0);
	float *max = G_VECTOR(OFS_PARM1);
	float *velocity = G_VECTOR(OFS_PARM2);
	float count = G_FLOAT(OFS_PARM3);
	float colour = G_FLOAT(OFS_PARM4);

	if (count < 1)
		return;
	if (count > 65535)
		count = 65535;

	MSG_WriteByte(&sv.multicast, svc_temp_entity);
	MSG_WriteByte(&sv.multicast, TEDP_PARTICLERAIN);
	MSG_WriteCoord(&sv.multicast, min[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, min[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, min[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[2], sv.protocolflags);
	MSG_WriteShort(&sv.multicast, count);
	MSG_WriteByte(&sv.multicast, colour);

	SV_Multicast (MULTICAST_ALL_U, NULL, 0, PEXT2_REPLACEMENTDELTAS);
}
static void PF_sv_te_particlesnow(void)
{
	float *min = G_VECTOR(OFS_PARM0);
	float *max = G_VECTOR(OFS_PARM1);
	float *velocity = G_VECTOR(OFS_PARM2);
	float count = G_FLOAT(OFS_PARM3);
	float colour = G_FLOAT(OFS_PARM4);

	if (count < 1)
		return;
	if (count > 65535)
		count = 65535;

	MSG_WriteByte(&sv.multicast, svc_temp_entity);
	MSG_WriteByte(&sv.multicast, TEDP_PARTICLESNOW);
	MSG_WriteCoord(&sv.multicast, min[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, min[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, min[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[2], sv.protocolflags);
	MSG_WriteShort(&sv.multicast, count);
	MSG_WriteByte(&sv.multicast, colour);

	SV_Multicast (MULTICAST_ALL_U, NULL, 0, PEXT2_REPLACEMENTDELTAS);
}
#else
#define PF_sv_te_particlerain PF_void_stub
#define PF_sv_te_particlesnow PF_void_stub
#endif
#define PF_sv_te_bloodshower PF_void_stub
#define PF_sv_te_explosionrgb PF_void_stub
#define PF_sv_te_particlecube PF_void_stub
#define PF_sv_te_spark PF_void_stub
#define PF_sv_te_gunshotquad PF_sv_te_gunshot
#define PF_sv_te_spikequad PF_sv_te_spike
#define PF_sv_te_superspikequad PF_sv_te_superspike
#define PF_sv_te_explosionquad PF_sv_te_explosion
#define PF_sv_te_smallflash PF_void_stub
#define PF_sv_te_customflash PF_void_stub
#define PF_sv_te_plasmaburn PF_sv_te_tarexplosion
#define PF_sv_effect PF_void_stub

static void PF_sv_pointsound(void)
{
	float *origin = G_VECTOR(OFS_PARM0);
	const char *sample = G_STRING(OFS_PARM1);
	float volume = G_FLOAT(OFS_PARM2);
	float attenuation = G_FLOAT(OFS_PARM3);
	SV_StartSound (qcvm->edicts, origin, 0, sample, volume, attenuation);
}
static void PF_cl_pointsound(void)
{
	float *origin = G_VECTOR(OFS_PARM0);
	const char *sample = G_STRING(OFS_PARM1);
	float volume = G_FLOAT(OFS_PARM2);
	float attenuation = G_FLOAT(OFS_PARM3);
	S_StartSound(0, 0, S_PrecacheSound(sample), origin, volume, attenuation);
}
static void PF_cl_soundlength(void)
{
	const char *sample = G_STRING(OFS_PARM0);
	sfx_t *sfx = S_PrecacheSound(sample);
	sfxcache_t *sc;
	G_FLOAT(OFS_RETURN) = 0;
	if (sfx)
	{
		sc = S_LoadSound (sfx);
		if (sc)
			G_FLOAT(OFS_RETURN) = (double)sc->length / sc->speed;
	}
}
static void PF_cl_localsound(void)
{
	const char *sample = G_STRING(OFS_PARM0);
	float channel = (qcvm->argc>1)?G_FLOAT(OFS_PARM1):-1;
	float volume = (qcvm->argc>2)?G_FLOAT(OFS_PARM2):1;

	//FIXME: svc_setview or map changes can break sound replacements here.
	S_StartSound(cl.viewentity, channel, S_PrecacheSound(sample), vec3_origin, volume, 0);
}

//file stuff

//returns false if the file is denied.
//fallbackread can be NULL, if the qc is not allowed to read that (original) file at all.
static qboolean QC_FixFileName(const char *name, const char **result, const char **fallbackread)
{
	if (!*name ||	//blank names are bad
		strchr(name, ':') ||	//dos/win absolute path, ntfs ADS, amiga drives. reject them all.
		strchr(name, '\\') ||	//windows-only paths.
		*name == '/' ||	//absolute path was given - reject
		strstr(name, ".."))	//someone tried to be clever.
	{
		return false;
	}

	*fallbackread = name;
	//if its a user config, ban any fallback locations so that csqc can't read passwords or whatever.
	if ((!strchr(name, '/') || q_strncasecmp(name, "configs/", 8))
		&& !q_strcasecmp(COM_FileGetExtension(name), "cfg")
		&& q_strncasecmp(name, "particles/", 10) && q_strncasecmp(name, "huds/", 5) && q_strncasecmp(name, "models/", 7))
		*fallbackread = NULL;
	*result = va("data/%s", name);
	return true;
}

//small note on access modes:
//when reading, we fopen files inside paks, for compat with (crappy non-zip-compatible) filesystem code
//when writing, we directly fopen the file such that it can never be inside a pak.
//this means that we need to take care when reading in order to detect EOF properly.
//writing doesn't need anything like that, so it can just dump stuff out, but we do need to ensure that the modes don't get mixed up, because trying to read from a writable file will not do what you would expect.
//even libc mandates a seek between reading+writing, so no great loss there.
static struct qcfile_s
{
	qcvm_t *owningvm;
	char cache[1024];
	int cacheoffset, cachesize;
	FILE *file;
	int fileoffset;
	int filesize;
	int filebase;	//the offset of the file inside a pak
	int mode;
} *qcfiles;
static size_t qcfiles_max;
#define QC_FILE_BASE 1
static void PF_fopen(void)
{
	const char *fname = G_STRING(OFS_PARM0);
	int fmode = G_FLOAT(OFS_PARM1);
	const char *fallback;
	FILE *file;
	size_t i;
	char name[MAX_OSPATH], *sl;
	int filesize = 0;

	G_FLOAT(OFS_RETURN) = -1;	//assume failure

	if (!QC_FixFileName(fname, &fname, &fallback))
	{
		Con_Printf("qcfopen: Access denied: %s\n", fname);
		return;
	}
	//if we were told to use 'foo.txt'
	//fname is now 'data/foo.txt'
	//fallback is now 'foo.txt', and must ONLY be read.

	switch(fmode)
	{
	case 0: //read
		filesize = COM_FOpenFile (fname, &file, NULL);
		if (!file && fallback)
			filesize = COM_FOpenFile (fallback, &file, NULL);
		break;
	case 1:	//append
		q_snprintf (name, sizeof(name), "%s/%s", com_gamedir, fname);
		Sys_mkdir (name);
		file = fopen(name, "w+b");
		if (file)
			fseek(file, 0, SEEK_END);
		break;
	case 2: //write
		q_snprintf (name, sizeof(name), "%s/%s", com_gamedir, fname);
		sl = name+1;
		while (*sl)
		{
			if (*sl == '/')
			{
				*sl = 0;
				Sys_mkdir (name);	//make sure each part of the path exists.
				*sl = '/';
			}
			sl++;
		}
		file = fopen(name, "wb");
		break;
	}
	if (!file)
		return;

	for (i = 0; ; i++)
	{
		if (i == qcfiles_max)
		{
			qcfiles_max++;
			qcfiles = Z_Realloc(qcfiles, sizeof(*qcfiles)*qcfiles_max);
		}
		if (!qcfiles[i].file)
			break;
	}
	qcfiles[i].filebase = ftell(file);
	qcfiles[i].owningvm = qcvm;
	qcfiles[i].file = file;
	qcfiles[i].mode = fmode;
	//reading needs size info
	qcfiles[i].filesize = filesize;
	//clear the read cache.
	qcfiles[i].fileoffset = qcfiles[i].cacheoffset = qcfiles[i].cachesize = 0;

	G_FLOAT(OFS_RETURN) = i+QC_FILE_BASE;
}
static void PF_fgets(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	G_INT(OFS_RETURN) = 0;
	if (fileid >= qcfiles_max)
		Con_Warning("PF_fgets: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fgets: file not open\n");
	else if (qcfiles[fileid].mode != 0)
		Con_Warning("PF_fgets: file not open for reading\n");
	else
	{
		struct qcfile_s *f = &qcfiles[fileid];
		char *ret = PR_GetTempString();
		char *s = ret;
		char *end = ret+STRINGTEMP_LENGTH;
		for (;;)
		{
			if (f->cacheoffset == f->cachesize)
			{
				//figure out how much we can try to cache.
				int sz = f->filesize - f->fileoffset;
				if (sz < 0 || f->fileoffset < 0)	//... maybe we shouldn't have implemented seek support.
					sz = 0;
				else if ((size_t)sz > sizeof(f->cache))
					sz = sizeof(f->cache);
				//read a chunk
				f->cacheoffset = 0;
				f->cachesize = fread(f->cache, 1, sz, f->file);
				f->fileoffset += f->cachesize;
				if (!f->cachesize)
				{
					if (s == ret)
					{	//absolutely nothing to spew
						G_INT(OFS_RETURN) = 0;
						return;
					}
					//classic eof...
					break;
				}
			}
			*s = f->cache[f->cacheoffset++];
			if (*s == '\n')	//new line, yay!
				break;
			s++;
			if (s == end)
				s--;	//rewind if we're overflowing, such that we truncate the string.
		}
		if (s > ret && s[-1] == '\r')
			s--;	//terminate it on the \r of a \r\n pair.
		*s = 0;	//terminate it
		G_INT(OFS_RETURN) = PR_SetEngineString(ret);
	}
}
static void PF_fputs(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	const char *str = PF_VarString(1);
	if (fileid >= qcfiles_max)
		Con_Warning("PF_fputs: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fputs: file not open\n");
	else if (qcfiles[fileid].mode == 0)
		Con_Warning("PF_fgets: file not open for writing\n");
	else
		fputs(str, qcfiles[fileid].file);
}
static void PF_fclose(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	if (fileid >= qcfiles_max)
		Con_Warning("PF_fclose: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fclose: file not open\n");
	else
	{
		fclose(qcfiles[fileid].file);
		qcfiles[fileid].file = NULL;
		qcfiles[fileid].owningvm = NULL;
	}
}
static void PF_frikfile_shutdown(void)
{
	size_t i;
	for (i = 0; i < qcfiles_max; i++)
	{
		if (qcfiles[i].owningvm == qcvm)
		{
			fclose(qcfiles[i].file);
			qcfiles[i].file = NULL;
			qcfiles[i].owningvm = NULL;
		}
	}
}
static void PF_fseek(void)
{	//returns current position. or changes that position.
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	G_INT(OFS_RETURN) = 0;
	if (fileid >= qcfiles_max)
		Con_Warning("PF_fread: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fread: file not open\n");
	else
	{
		if (qcfiles[fileid].mode == 0)
			G_INT(OFS_RETURN) = qcfiles[fileid].fileoffset;	//when we're reading, use the cached read offset
		else
			G_INT(OFS_RETURN) = ftell(qcfiles[fileid].file)-qcfiles[fileid].filebase;
		if (qcvm->argc>1)
		{
			qcfiles[fileid].fileoffset = G_INT(OFS_PARM1);
			fseek(qcfiles[fileid].file, qcfiles[fileid].filebase+qcfiles[fileid].fileoffset, SEEK_SET);
			qcfiles[fileid].cachesize = qcfiles[fileid].cacheoffset = 0;
		}
	}
}
#if 0
static void PF_fread(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	int qcptr = G_INT(OFS_PARM1);
	size_t size = G_INT(OFS_PARM2);

	//FIXME: validate. make usable.
	char *nativeptr = (char*)sv.edicts + qcptr;

	G_INT(OFS_RETURN) = 0;
	if (fileid >= maxfiles)
		Con_Warning("PF_fread: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fread: file not open\n");
	else
		G_INT(OFS_RETURN) = fread(nativeptr, 1, size, qcfiles[fileid].file);
}
static void PF_fwrite(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	int qcptr = G_INT(OFS_PARM1);
	size_t size = G_INT(OFS_PARM2);

	//FIXME: validate. make usable.
	const char *nativeptr = (const char*)sv.edicts + qcptr;

	G_INT(OFS_RETURN) = 0;
	if (fileid >= maxfiles)
		Con_Warning("PF_fwrite: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fwrite: file not open\n");
	else
		G_INT(OFS_RETURN) = fwrite(nativeptr, 1, size, qcfiles[fileid].file);
}
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
static void PF_fsize(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	G_INT(OFS_RETURN) = 0;
	if (fileid >= maxfiles)
		Con_Warning("PF_fread: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fread: file not open\n");
	else if (qcfiles[fileid].mode == 0)
	{
		G_INT(OFS_RETURN) = qcfiles[fileid].filesize;
		//can't truncate if we're reading.
	}
	else
	{
		long curpos = ftell(qcfiles[fileid].file);
		fseek(qcfiles[fileid].file, 0, SEEK_END);
		G_INT(OFS_RETURN) = ftell(qcfiles[fileid].file);
		fseek(qcfiles[fileid].file, curpos, SEEK_SET);

		if (pr_argc>1)
		{
			//specifically resize. or maybe extend.
#ifdef _WIN32
			_chsize(fileno(qcfiles[fileid].file), G_INT(OFS_PARM1));
#else
			ftruncate(fileno(qcfiles[fileid].file), G_INT(OFS_PARM1));
#endif
		}
	}
}
#endif

static struct filesearch_s
{
	qcvm_t *owner;
	size_t numfiles;
	size_t maxfiles;
	unsigned int flags;
	struct
	{
		char name[MAX_QPATH];
		time_t mtime;
		size_t fsize;
		searchpath_t *spath;
	} *file;
} searches[16];
static qboolean PR_Search_AddFile(void *ctx, const char *fname, time_t mtime, size_t fsize, searchpath_t *spath)
{
	struct filesearch_s *c = ctx;
	if (c->numfiles == c->maxfiles)
	{
		c->maxfiles = c->maxfiles*2+2;
		c->file = realloc(c->file, c->maxfiles*sizeof(*c->file));
	}
	q_strlcpy(c->file[c->numfiles].name, fname, sizeof(c->file[c->numfiles].name));
	c->file[c->numfiles].mtime = mtime;
	c->file[c->numfiles].fsize = fsize;
	c->file[c->numfiles].spath = spath;
	c->numfiles++;
	return true;
}
static void PF_search_shutdown(void)
{
	size_t i;
	for (i = 0; i < countof(searches); i++)
	{
		if (searches[i].owner == qcvm)
		{
			searches[i].owner = NULL;
			searches[i].numfiles = 0;
			searches[i].maxfiles = 0;
			free(searches[i].file);
			searches[i].file = NULL;
		}
	}
}

static void PF_search_begin(void)
{
	size_t i;
	const char *pattern = G_STRING(OFS_PARM0);
	unsigned int flags = G_FLOAT(OFS_PARM1);
//	qboolaen quiet = !!G_FLOAT(OFS_PARM2);
	const char *pkgfilter = (qcvm->argc>3)?G_STRING(OFS_PARM3):NULL;

	for (i = 0; i < countof(searches); i++)
	{
		if (!searches[i].owner)
		{
			searches[i].flags = flags;
			COM_ListAllFiles(&searches[i], pattern, PR_Search_AddFile, flags, pkgfilter);
			if (!searches[i].numfiles)
				break;
			searches[i].owner = qcvm;
			G_FLOAT(OFS_RETURN) = i;
			return;
		}
	}
	G_FLOAT(OFS_RETURN) = -1;
}
static void PF_search_end(void)
{
	int handle = G_FLOAT(OFS_PARM0);
	if (handle < 0 || handle >= countof(searches))
		return; //erk
	searches[handle].owner = NULL;
	searches[handle].numfiles = 0;
	searches[handle].maxfiles = 0;
	free(searches[handle].file);
	searches[handle].file = NULL;
}
static void PF_search_getsize(void)
{
	size_t handle = G_FLOAT(OFS_PARM0);
	if (handle < 0 || handle >= countof(searches))
	{
		G_FLOAT(OFS_RETURN) = 0;
		return; //erk
	}
	G_FLOAT(OFS_RETURN) = searches[handle].numfiles;
}
static void PF_search_getfilename(void)
{
	size_t handle = G_FLOAT(OFS_PARM0);
	size_t index = G_FLOAT(OFS_PARM1);
	G_INT(OFS_RETURN) = 0;
	if (handle < 0 || handle >= countof(searches))
		return; //erk
	if (index < 0 || index >= searches[handle].numfiles)
		return; //erk
	G_INT(OFS_RETURN) = PR_MakeTempString(searches[handle].file[index].name);
}
static void PF_search_getfilesize(void)
{
	size_t handle = G_FLOAT(OFS_PARM0);
	size_t index = G_FLOAT(OFS_PARM1);
	G_FLOAT(OFS_RETURN) = 0;
	if (handle < 0 || handle >= countof(searches))
		return; //erk
	if (index < 0 || index >= searches[handle].numfiles)
		return; //erk
	G_FLOAT(OFS_RETURN) = searches[handle].file[index].fsize;
}
static void PF_search_getfilemtime(void)
{
	char *ret;
	size_t handle = G_FLOAT(OFS_PARM0);
	size_t index = G_FLOAT(OFS_PARM1);
	G_INT(OFS_RETURN) = 0;
	if (handle < 0 || handle >= countof(searches))
		return; //erk
	if (index < 0 || index >= searches[handle].numfiles)
		return; //erk

	ret = PR_GetTempString();
	strftime(ret, STRINGTEMP_LENGTH, "%Y-%m-%d %H:%M:%S", localtime(&searches[handle].file[index].mtime));
	G_INT(OFS_RETURN) = PR_SetEngineString(ret);
}
static void PF_search_getpackagename(void)
{
	searchpath_t *spath;
//	char *ret;
	size_t handle = G_FLOAT(OFS_PARM0);
	size_t index = G_FLOAT(OFS_PARM1);
	G_INT(OFS_RETURN) = 0;
	if (handle < 0 || handle >= countof(searches))
		return; //erk
	if (index < 0 || index >= searches[handle].numfiles)
		return; //erk

	for (spath = com_searchpaths; spath; spath = spath->next)
		if (spath == searches[handle].file[index].spath)
			break;

	if (spath)
	{
		if (searches[handle].flags & 2)
		{	//gamedir/packagename. not necessarily fopenable.
			G_INT(OFS_RETURN) = PR_MakeTempString(spath->purename);
		}
		else
		{	//like whichpack thus like frik_file. which sucks.
			if (spath->pack)
				G_INT(OFS_RETURN) = PR_MakeTempString(COM_SkipPath(spath->purename));
			else
				G_INT(OFS_RETURN) = 0;
		}
	}
	else
		G_INT(OFS_RETURN) = 0;	//no idea where it came from. sorry.
}

static void PF_whichpack(void)
{
	const char *fname = G_STRING(OFS_PARM0);	//uses native paths, as this isn't actually reading anything.
	unsigned int path_id;
	if (COM_FileExists(fname, &path_id))
	{
		//FIXME: quakespasm reports which gamedir the file is in, but paks are hidden.
		//I'm too lazy to rewrite COM_FindFile, so I'm just going to hack something small to get the gamedir, just not the paks

		searchpath_t *path;
		for (path = com_searchpaths; path; path = path->next)
			if (!path->pack && path->path_id == path_id)
				break;	//okay, this one looks like one we can report

		//sandbox it by stripping the basedir
		fname = path->filename;
		if (!strncmp(fname, com_basedir, strlen(com_basedir)))
			fname += strlen(com_basedir);
		else
			fname = "?";	//no idea where this came from. something is screwing with us.
		while (*fname == '/' || *fname == '\\')
			fname++;	//small cleanup, just in case
		G_INT(OFS_RETURN) = PR_SetEngineString(fname);
	}
	else
		G_INT(OFS_RETURN) = 0;
}

//string buffers

struct strbuf {
	qcvm_t  *owningvm;
	char **strings;
	unsigned int used;
	unsigned int allocated;
};

#define BUFSTRBASE 1
#define NUMSTRINGBUFS 64u
static struct strbuf strbuflist[NUMSTRINGBUFS];

static void PF_buf_shutdown(void)
{
	unsigned int i, bufno;

	for (bufno = 0; bufno < NUMSTRINGBUFS; bufno++)
	{
		if (strbuflist[bufno].owningvm != qcvm)
			continue;

		for (i = 0; i < strbuflist[bufno].used; i++)
			Z_Free(strbuflist[bufno].strings[i]);
		Z_Free(strbuflist[bufno].strings);

		strbuflist[bufno].owningvm = NULL;
		strbuflist[bufno].strings = NULL;
		strbuflist[bufno].used = 0;
		strbuflist[bufno].allocated = 0;
	}
}

// #440 float() buf_create (DP_QC_STRINGBUFFERS)
static void PF_buf_create(void)
{
	unsigned int i;

	const char *type = ((qcvm->argc>0)?G_STRING(OFS_PARM0):"string");
//	unsigned int flags = ((pr_argc>1)?G_FLOAT(OFS_PARM1):1);

	if (!q_strcasecmp(type, "string"))
		;
	else
	{
		G_FLOAT(OFS_RETURN) = -1;
		return;
	}

	//flags&1 == saved. apparently.

	for (i = 0; i < NUMSTRINGBUFS; i++)
	{
		if (!strbuflist[i].owningvm)
		{
			strbuflist[i].owningvm = qcvm;
			strbuflist[i].used = 0;
			strbuflist[i].allocated = 0;
			strbuflist[i].strings = NULL;
			G_FLOAT(OFS_RETURN) = i+BUFSTRBASE;
			return;
		}
	}
	G_FLOAT(OFS_RETURN) = -1;
}
// #441 void(float bufhandle) buf_del (DP_QC_STRINGBUFFERS)
static void PF_buf_del(void)
{
	unsigned int i;
	unsigned int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;

	if (bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].owningvm)
		return;

	for (i = 0; i < strbuflist[bufno].used; i++)
	{
		if (strbuflist[bufno].strings[i])
			Z_Free(strbuflist[bufno].strings[i]);
	}
	Z_Free(strbuflist[bufno].strings);

	strbuflist[bufno].strings = NULL;
	strbuflist[bufno].used = 0;
	strbuflist[bufno].allocated = 0;

	strbuflist[bufno].owningvm = NULL;
}
// #442 float(float bufhandle) buf_getsize (DP_QC_STRINGBUFFERS)
static void PF_buf_getsize(void)
{
	int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].owningvm)
		return;

	G_FLOAT(OFS_RETURN) = strbuflist[bufno].used;
}
// #443 void(float bufhandle_from, float bufhandle_to) buf_copy (DP_QC_STRINGBUFFERS)
static void PF_buf_copy(void)
{
	unsigned int buffrom = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	unsigned int bufto = G_FLOAT(OFS_PARM1)-BUFSTRBASE;
	unsigned int i;

	if (bufto == buffrom)	//err...
		return;
	if (buffrom >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[buffrom].owningvm)
		return;
	if (bufto >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufto].owningvm)
		return;

	//obliterate any and all existing data.
	for (i = 0; i < strbuflist[bufto].used; i++)
		if (strbuflist[bufto].strings[i])
			Z_Free(strbuflist[bufto].strings[i]);
	Z_Free(strbuflist[bufto].strings);

	//copy new data over.
	strbuflist[bufto].used = strbuflist[bufto].allocated = strbuflist[buffrom].used;
	strbuflist[bufto].strings = Z_Malloc(strbuflist[buffrom].used * sizeof(char*));
	for (i = 0; i < strbuflist[buffrom].used; i++)
		strbuflist[bufto].strings[i] = strbuflist[buffrom].strings[i]?Z_StrDup(strbuflist[buffrom].strings[i]):NULL;
}
static int PF_buf_sort_sortprefixlen;
static int PF_buf_sort_ascending(const void *a, const void *b)
{
	return strncmp(*(char*const*)a, *(char*const*)b, PF_buf_sort_sortprefixlen);
}
static int PF_buf_sort_descending(const void *b, const void *a)
{
	return strncmp(*(char*const*)a, *(char*const*)b, PF_buf_sort_sortprefixlen);
}
// #444 void(float bufhandle, float sortprefixlen, float backward) buf_sort (DP_QC_STRINGBUFFERS)
static void PF_buf_sort(void)
{
	int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	int sortprefixlen = G_FLOAT(OFS_PARM1);
	int backwards = G_FLOAT(OFS_PARM2);
	unsigned int s,d;
	char **strings;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].owningvm)
		return;

	if (sortprefixlen <= 0)
		sortprefixlen = 0x7fffffff;

	//take out the nulls first, to avoid weird/crashy sorting
	for (s = 0, d = 0, strings = strbuflist[bufno].strings; s < strbuflist[bufno].used; )
	{
		if (!strings[s])
		{
			s++;
			continue;
		}
		strings[d++] = strings[s++];
	}
	strbuflist[bufno].used = d;

	//no nulls now, sort it.
	PF_buf_sort_sortprefixlen = sortprefixlen;	//eww, a global. burn in hell.
	if (backwards)	//z first
		qsort(strings, strbuflist[bufno].used, sizeof(char*), PF_buf_sort_descending);
	else	//a first
		qsort(strings, strbuflist[bufno].used, sizeof(char*), PF_buf_sort_ascending);
}
// #445 string(float bufhandle, string glue) buf_implode (DP_QC_STRINGBUFFERS)
static void PF_buf_implode(void)
{
	int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	const char *glue = G_STRING(OFS_PARM1);
	unsigned int gluelen = strlen(glue);
	unsigned int retlen, l, i;
	char **strings;
	char *ret;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].owningvm)
		return;

	//count neededlength
	strings = strbuflist[bufno].strings;
	/*
	for (i = 0, retlen = 0; i < strbuflist[bufno].used; i++)
	{
		if (strings[i])
		{
			if (retlen)
				retlen += gluelen;
			retlen += strlen(strings[i]);
		}
	}
	ret = malloc(retlen+1);*/

	//generate the output
	ret = PR_GetTempString();
	for (i = 0, retlen = 0; i < strbuflist[bufno].used; i++)
	{
		if (strings[i])
		{
			if (retlen)
			{
				if (retlen+gluelen+1 > STRINGTEMP_LENGTH)
				{
					Con_Printf("PF_buf_implode: tempstring overflow\n");
					break;
				}
				memcpy(ret+retlen, glue, gluelen);
				retlen += gluelen;
			}
			l = strlen(strings[i]);
			if (retlen+l+1 > STRINGTEMP_LENGTH)
			{
				Con_Printf("PF_buf_implode: tempstring overflow\n");
				break;
			}
			memcpy(ret+retlen, strings[i], l);
			retlen += l;
		}
	}

	//add the null and return
	ret[retlen] = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(ret);
}
// #446 string(float bufhandle, float string_index) bufstr_get (DP_QC_STRINGBUFFERS)
static void PF_bufstr_get(void)
{
	unsigned int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	unsigned int index = G_FLOAT(OFS_PARM1);
	char *ret;

	if (bufno >= NUMSTRINGBUFS)
	{
		G_INT(OFS_RETURN) = 0;
		return;
	}
	if (!strbuflist[bufno].owningvm)
	{
		G_INT(OFS_RETURN) = 0;
		return;
	}

	if (index >= strbuflist[bufno].used)
	{
		G_INT(OFS_RETURN) = 0;
		return;
	}

	if (strbuflist[bufno].strings[index])
	{
		ret = PR_GetTempString();
		q_strlcpy(ret, strbuflist[bufno].strings[index], STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(ret);
	}
	else
		G_INT(OFS_RETURN) = 0;
}
// #447 void(float bufhandle, float string_index, string str) bufstr_set (DP_QC_STRINGBUFFERS)
static void PF_bufstr_set(void)
{
	unsigned int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	unsigned int index = G_FLOAT(OFS_PARM1);
	const char *string = G_STRING(OFS_PARM2);
	unsigned int oldcount;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].owningvm)
		return;

	if (index >= strbuflist[bufno].allocated)
	{
		oldcount = strbuflist[bufno].allocated;
		strbuflist[bufno].allocated = (index + 256);
		strbuflist[bufno].strings = Z_Realloc(strbuflist[bufno].strings, strbuflist[bufno].allocated*sizeof(char*));
		memset(strbuflist[bufno].strings+oldcount, 0, (strbuflist[bufno].allocated - oldcount) * sizeof(char*));
	}
	if (strbuflist[bufno].strings[index])
		Z_Free(strbuflist[bufno].strings[index]);
	strbuflist[bufno].strings[index] = Z_Malloc(strlen(string)+1);
	strcpy(strbuflist[bufno].strings[index], string);

	if (index >= strbuflist[bufno].used)
		strbuflist[bufno].used = index+1;
}

static int PF_bufstr_add_internal(unsigned int bufno, const char *string, int appendonend)
{
	unsigned int index;
	if (appendonend)
	{
		//add on end
		index = strbuflist[bufno].used;
	}
	else
	{
		//find a hole
		for (index = 0; index < strbuflist[bufno].used; index++)
			if (!strbuflist[bufno].strings[index])
				break;
	}

	//expand it if needed
	if (index >= strbuflist[bufno].allocated)
	{
		unsigned int oldcount;
		oldcount = strbuflist[bufno].allocated;
		strbuflist[bufno].allocated = (index + 256);
		strbuflist[bufno].strings = Z_Realloc(strbuflist[bufno].strings, strbuflist[bufno].allocated*sizeof(char*));
		memset(strbuflist[bufno].strings+oldcount, 0, (strbuflist[bufno].allocated - oldcount) * sizeof(char*));
	}

	//add in the new string.
	if (strbuflist[bufno].strings[index])
		Z_Free(strbuflist[bufno].strings[index]);
	strbuflist[bufno].strings[index] = Z_Malloc(strlen(string)+1);
	strcpy(strbuflist[bufno].strings[index], string);

	if (index >= strbuflist[bufno].used)
		strbuflist[bufno].used = index+1;

	return index;
}

// #448 float(float bufhandle, string str, float order) bufstr_add (DP_QC_STRINGBUFFERS)
static void PF_bufstr_add(void)
{
	size_t bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	const char *string = G_STRING(OFS_PARM1);
	qboolean ordered = G_FLOAT(OFS_PARM2);

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].owningvm)
		return;

	G_FLOAT(OFS_RETURN) = PF_bufstr_add_internal(bufno, string, ordered);
}
// #449 void(float bufhandle, float string_index) bufstr_free (DP_QC_STRINGBUFFERS)
static void PF_bufstr_free(void)
{
	size_t bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	size_t index = G_FLOAT(OFS_PARM1);

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].owningvm)
		return;

	if (index >= strbuflist[bufno].used)
		return;	//not valid anyway.

	if (strbuflist[bufno].strings[index])
		Z_Free(strbuflist[bufno].strings[index]);
	strbuflist[bufno].strings[index] = NULL;
}

static void PF_buf_cvarlist(void)
{
	size_t bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	const char *pattern = G_STRING(OFS_PARM1);
	const char *antipattern = G_STRING(OFS_PARM2);
	int i;
	cvar_t	*var;
	int plen = strlen(pattern), alen = strlen(antipattern);
	qboolean pwc = strchr(pattern, '*')||strchr(pattern, '?'),
			 awc = strchr(antipattern, '*')||strchr(antipattern, '?');

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].owningvm)
		return;

	//obliterate any and all existing data.
	for (i = 0; i < strbuflist[bufno].used; i++)
		if (strbuflist[bufno].strings[i])
			Z_Free(strbuflist[bufno].strings[i]);
	if (strbuflist[bufno].strings)
		Z_Free(strbuflist[bufno].strings);
	strbuflist[bufno].used = strbuflist[bufno].allocated = 0;

	for (var=Cvar_FindVarAfter ("", CVAR_NONE) ; var ; var=var->next)
	{
		if (plen && (pwc?!wildcmp(pattern, var->name):strncmp(var->name, pattern, plen)))
			continue;
		if (alen && (awc?wildcmp(antipattern, var->name):!strncmp(var->name, antipattern, alen)))
			continue;

		PF_bufstr_add_internal(bufno, var->name, true);
	}

	qsort(strbuflist[bufno].strings, strbuflist[bufno].used, sizeof(char*), PF_buf_sort_ascending);
}

//directly reads a file into a stringbuffer
static void PF_buf_loadfile(void)
{
	const char *fname = G_STRING(OFS_PARM0);
	size_t bufno = G_FLOAT(OFS_PARM1)-BUFSTRBASE;
	char *line, *nl;
	const char *fallback;

	G_FLOAT(OFS_RETURN) = 0;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].owningvm)
		return;

	if (!QC_FixFileName(fname, &fname, &fallback))
	{
		Con_Printf("qcfopen: Access denied: %s\n", fname);
		return;
	}
	line = (char*)COM_LoadTempFile(fname, NULL);
	if (!line && fallback)
		line = (char*)COM_LoadTempFile(fallback, NULL);
	if (!line)
		return;

	while(line)
	{
		nl = strchr(line, '\n');
		if (nl)
			*nl++ = 0;
		PF_bufstr_add_internal(bufno, line, true);
		line = nl;
	}

	G_FLOAT(OFS_RETURN) = true;
}

static void PF_buf_writefile(void)
{
	size_t fnum = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	size_t bufno = G_FLOAT(OFS_PARM1)-BUFSTRBASE;
	char **strings;
	unsigned int idx, midx;

	G_FLOAT(OFS_RETURN) = 0;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].owningvm)
		return;

	if (fnum >= qcfiles_max)
		return;
	if (!qcfiles[fnum].file)
		return;

	if (qcvm->argc >= 3)
	{
		if (G_FLOAT(OFS_PARM2) <= 0)
			idx = 0;
		else
			idx = G_FLOAT(OFS_PARM2);
	}
	else
		idx = 0;
	if (qcvm->argc >= 4)
		midx = idx + G_FLOAT(OFS_PARM3);
	else
		midx = strbuflist[bufno].used - idx;
	if (idx > strbuflist[bufno].used)
		idx = strbuflist[bufno].used;
	if (midx > strbuflist[bufno].used)
		midx = strbuflist[bufno].used;
	for(strings = strbuflist[bufno].strings; idx < midx; idx++)
	{
		if (strings[idx])
			fprintf(qcfiles[fnum].file, "%s\n", strings[idx]);
	}
	G_FLOAT(OFS_RETURN) = 1;
}

//entity stuff
static void PF_WasFreed(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = ed->free;
}
static void PF_copyentity(void)
{
	edict_t *src = G_EDICT(OFS_PARM0);
	edict_t *dst = (qcvm->argc<2)?ED_Alloc():G_EDICT(OFS_PARM1);
	if (src->free || dst->free)
		Con_Printf("PF_copyentity: entity is free\n");
	memcpy(&dst->v, &src->v, qcvm->edict_size - sizeof(entvars_t));
	dst->alpha = src->alpha;
	dst->sendinterval = src->sendinterval;
	SV_LinkEdict(dst, false);

	G_INT(OFS_RETURN) = EDICT_TO_PROG(dst);
}
static void PF_edict_for_num(void)
{
	G_INT(OFS_RETURN) = EDICT_TO_PROG(EDICT_NUM(G_FLOAT(OFS_PARM0)));
}
static void PF_num_for_edict(void)
{
	G_FLOAT(OFS_RETURN) = G_EDICTNUM(OFS_PARM0);
}
static void PF_findchain(void)
{
	edict_t	*ent, *chain;
	int	i, f;
	const char *s, *t;
	int cfld;

	chain = (edict_t *)qcvm->edicts;

	f = G_INT(OFS_PARM0);
	s = G_STRING(OFS_PARM1);
	if (qcvm->argc > 2)
		cfld = G_INT(OFS_PARM2);
	else
		cfld = &ent->v.chain - (int*)&ent->v;

	ent = NEXT_EDICT(qcvm->edicts);
	for (i = 1; i < qcvm->num_edicts; i++, ent = NEXT_EDICT(ent))
	{
		if (ent->free)
			continue;
		t = E_STRING(ent,f);
		if (strcmp(s, t))
			continue;
		((int*)&ent->v)[cfld] = EDICT_TO_PROG(chain);
		chain = ent;
	}

	RETURN_EDICT(chain);
}
static void PF_findfloat(void)
{
	int		e;
	int		f;
	float	s, t;
	edict_t	*ed;

	e = G_EDICTNUM(OFS_PARM0);
	f = G_INT(OFS_PARM1);
	s = G_FLOAT(OFS_PARM2);

	for (e++ ; e < qcvm->num_edicts ; e++)
	{
		ed = EDICT_NUM(e);
		if (ed->free)
			continue;
		t = E_FLOAT(ed,f);
		if (t == s)
		{
			RETURN_EDICT(ed);
			return;
		}
	}

	RETURN_EDICT(qcvm->edicts);
}
static void PF_findchainfloat(void)
{
	edict_t	*ent, *chain;
	int	i, f;
	float s, t;
	int cfld;

	chain = (edict_t *)qcvm->edicts;

	f = G_INT(OFS_PARM0);
	s = G_FLOAT(OFS_PARM1);
	if (qcvm->argc > 2)
		cfld = G_INT(OFS_PARM2);
	else
		cfld = &ent->v.chain - (int*)&ent->v;

	ent = NEXT_EDICT(qcvm->edicts);
	for (i = 1; i < qcvm->num_edicts; i++, ent = NEXT_EDICT(ent))
	{
		if (ent->free)
			continue;
		t = E_FLOAT(ent,f);
		if (s != t)
			continue;
		((int*)&ent->v)[cfld] = EDICT_TO_PROG(chain);
		chain = ent;
	}

	RETURN_EDICT(chain);
}
static void PF_findflags(void)
{
	int		e;
	int		f;
	int		s, t;
	edict_t	*ed;

	e = G_EDICTNUM(OFS_PARM0);
	f = G_INT(OFS_PARM1);
	s = G_FLOAT(OFS_PARM2);

	for (e++ ; e < qcvm->num_edicts ; e++)
	{
		ed = EDICT_NUM(e);
		if (ed->free)
			continue;
		t = E_FLOAT(ed,f);
		if (t & s)
		{
			RETURN_EDICT(ed);
			return;
		}
	}

	RETURN_EDICT(qcvm->edicts);
}
static void PF_findchainflags(void)
{
	edict_t	*ent, *chain;
	int	i, f;
	int s, t;
	int cfld;

	chain = (edict_t *)qcvm->edicts;

	f = G_INT(OFS_PARM0);
	s = G_FLOAT(OFS_PARM1);
	if (qcvm->argc > 2)
		cfld = G_INT(OFS_PARM2);
	else
		cfld = &ent->v.chain - (int*)&ent->v;

	ent = NEXT_EDICT(qcvm->edicts);
	for (i = 1; i < qcvm->num_edicts; i++, ent = NEXT_EDICT(ent))
	{
		if (ent->free)
			continue;
		t = E_FLOAT(ent,f);
		if (!(s & t))
			continue;
		((int*)&ent->v)[cfld] = EDICT_TO_PROG(chain);
		chain = ent;
	}

	RETURN_EDICT(chain);
}
static void PF_numentityfields(void)
{
	G_FLOAT(OFS_RETURN) = qcvm->progs->numfielddefs;
}
static void PF_findentityfield(void)
{
	ddef_t *fld = ED_FindField(G_STRING(OFS_PARM0));
	if (fld)
		G_FLOAT(OFS_RETURN) = fld - qcvm->fielddefs;
	else
		G_FLOAT(OFS_RETURN) = 0;	//the first field is meant to be some dummy placeholder. or it could be modelindex...
}
static void PF_entityfieldref(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	if (fldidx >= (unsigned int)qcvm->progs->numfielddefs)
		G_INT(OFS_RETURN) = 0;
	else
		G_INT(OFS_RETURN) = qcvm->fielddefs[fldidx].ofs;
}
static void PF_entityfieldname(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	if (fldidx < (unsigned int)qcvm->progs->numfielddefs)
		G_INT(OFS_RETURN) = qcvm->fielddefs[fldidx].s_name;
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_entityfieldtype(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	if (fldidx >= (unsigned int)qcvm->progs->numfielddefs)
		G_FLOAT(OFS_RETURN) = ev_void;
	else
		G_FLOAT(OFS_RETURN) = qcvm->fielddefs[fldidx].type;
}
static void PF_getentfldstr(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	edict_t *ent = G_EDICT(OFS_PARM1);
	if (fldidx < (unsigned int)qcvm->progs->numfielddefs)
	{
		char *ret = PR_GetTempString();
		const char *val = PR_UglyValueString (qcvm->fielddefs[fldidx].type, (eval_t*)((float*)&ent->v + qcvm->fielddefs[fldidx].ofs));
		q_strlcpy(ret, val, STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(ret);
	}
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_putentfldstr(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	edict_t *ent = G_EDICT(OFS_PARM1);
	const char *value = G_STRING(OFS_PARM2);
	if (fldidx < (unsigned int)qcvm->progs->numfielddefs)
		G_FLOAT(OFS_RETURN) = ED_ParseEpair ((void *)&ent->v, qcvm->fielddefs+fldidx, value, true);
	else
		G_FLOAT(OFS_RETURN) = false;
}
//static void PF_loadfromdata(void)
//{
//fixme;
//}
//static void PF_loadfromfile(void)
//{
//fixme;
//}
static void PF_writetofile(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	edict_t *ent = G_EDICT(OFS_PARM1);

	G_INT(OFS_RETURN) = 0;
	if (fileid >= qcfiles_max)
		Con_Warning("PF_fwrite: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fwrite: file not open\n");
	else
		ED_Write(qcfiles[fileid].file, ent);
}

static void PF_parseentitydata(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	const char *data = G_STRING(OFS_PARM1), *end;
	unsigned int offset = (qcvm->argc>2)?G_FLOAT(OFS_PARM2):0;
	if (offset)
	{
		unsigned int len = strlen(data);
		if (offset > len)
			offset = len;
	}
	if (!data[offset])
		G_FLOAT(OFS_RETURN) = 0;
	else
	{
		end = COM_Parse(data+offset);
		if (!strcmp(com_token, "{"))
		{
			end = ED_ParseEdict(end, ed);
			G_FLOAT(OFS_RETURN) = end - data;
		}
		else
			G_FLOAT(OFS_RETURN) = 0;	//doesn't look like an ent to me.
	}
}
/*static void PF_generateentitydata(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	edict_t *ent = G_EDICT(OFS_PARM1);
	if (fldidx < (unsigned int)qcvm->progs->numfielddefs)
	{
		char *ret = PR_GetTempString();
		const char *val = PR_UglyValueString (qcvm->fielddefs[fldidx].type, (eval_t*)((float*)&ent->v + qcvm->fielddefs[fldidx].ofs));
		q_strlcpy(ret, val, STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(ret);
	}
	else
		G_INT(OFS_RETURN) = 0;
}*/
static void PF_callfunction(void)
{
	dfunction_t *fnc;
	const char *fname;
	if (!qcvm->argc)
		return;
	qcvm->argc--;
	fname = G_STRING(OFS_PARM0 + qcvm->argc*3);
	fnc = ED_FindFunction(fname);
	if (fnc && fnc->first_statement > 0)
	{
		PR_ExecuteProgram(fnc - qcvm->functions);
	}
}
static void PF_isfunction(void)
{
	const char *fname = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = ED_FindFunction(fname)?true:false;
}

//other stuff
static void PF_gettime (void)
{
	int timer = (qcvm->argc > 0)?G_FLOAT(OFS_PARM0):0;
	switch(timer)
	{
	default:
		Con_DPrintf("PF_gettime: unsupported timer %i\n", timer);
	case 0:		//cached time at start of frame
		G_FLOAT(OFS_RETURN) = realtime;
		break;
	case 1:		//actual time
		G_FLOAT(OFS_RETURN) = Sys_DoubleTime();
		break;
	//case 2:	//highres.. looks like time into the frame. no idea
	//case 3:	//uptime
	//case 4:	//cd track
	//case 5:	//client simtime
	}
}
#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)
static void PF_infokey_internal(qboolean returnfloat)
{
	unsigned int ent = G_EDICTNUM(OFS_PARM0);
	const char *key = G_STRING(OFS_PARM1);
	const char *r;
	char buf[1024];
	if (!ent)
	{
		if (!strcmp(key, "*version"))
		{
			q_snprintf(buf, sizeof(buf), ENGINE_NAME_AND_VER);
			r = buf;
		}
		else
		{
			r = Info_GetKey(svs.serverinfo, key, buf, sizeof(buf));
			if (!*r)
				r = NULL;
		}
	}
	else if (ent <= (unsigned int)svs.maxclients && svs.clients[ent-1].active)
	{
		client_t *cl = &svs.clients[ent-1];
		r = buf;
		if (!strcmp(key, "ip"))
			r = NET_QSocketGetTrueAddressString(cl->netconnection);
		else if (!strcmp(key, "ping"))
		{
			float total = 0;
			unsigned int j;
			for (j = 0; j < NUM_PING_TIMES; j++)
				total+=cl->ping_times[j];
			total /= NUM_PING_TIMES;
			q_snprintf(buf, sizeof(buf), "%f", total);
		}
		else if (!strcmp(key, "protocol"))
		{
			switch(sv.protocol)
			{
			case PROTOCOL_NETQUAKE:
				r = "quake";
				break;
			case PROTOCOL_FITZQUAKE:
				r = "fitz666";
				break;
			case PROTOCOL_RMQ:
				r = "rmq999";
				break;
			default:
				r = "";
				break;
			}
		}
		else if (!strcmp(key, "name"))
			r = cl->name;
		else if (!strcmp(key, "topcolor"))
			q_snprintf(buf, sizeof(buf), "%u", cl->colors>>4);
		else if (!strcmp(key, "bottomcolor"))
			q_snprintf(buf, sizeof(buf), "%u", cl->colors&15);
		else if (!strcmp(key, "team"))	//nq doesn't really do teams. qw does though. yay compat?
			q_snprintf(buf, sizeof(buf), "t%u", (cl->colors&15)+1);
		else if (!strcmp(key, "*VIP"))
			r = "";
		else if (!strcmp(key, "*spectator"))
			r = "";
		else if (!strcmp(key, "csqcactive"))
			r = (cl->csqcactive?"1":"0");
		else
		{
			r = Info_GetKey(cl->userinfo, key, buf, sizeof(buf));
			if (!*r)
				r = NULL;
			r = NULL;
		}
	}
	else r = NULL;

	if (returnfloat)
	{
		if (r)
			G_FLOAT(OFS_RETURN) = atof(r);
		else
			G_FLOAT(OFS_RETURN) = 0;
	}
	else
	{
		if (r)
		{
			char *temp = PR_GetTempString();
			q_strlcpy(temp, r, STRINGTEMP_LENGTH);
			G_INT(OFS_RETURN) = PR_SetEngineString(temp);
		}
		else
			G_INT(OFS_RETURN) = 0;
	}
}
static void PF_infokey_s(void)
{
	PF_infokey_internal(false);
}
static void PF_infokey_f(void)
{
	PF_infokey_internal(true);
}

static void PF_multicast_internal(qboolean reliable, byte *pvs, unsigned int requireext2)
{
	unsigned int i;
	int cluster;
	mleaf_t *playerleaf;
	if (!pvs)
	{
		if (!requireext2)
			SZ_Write((reliable?&sv.reliable_datagram:&sv.datagram), sv.multicast.data, sv.multicast.cursize);
		else
		{
			for (i = 0; i < (unsigned int)svs.maxclients; i++)
			{
				if (!svs.clients[i].active)
					continue;
				if (!(svs.clients[i].protocol_pext2 & requireext2))
					continue;
				SZ_Write((reliable?&svs.clients[i].message:&svs.clients[i].datagram), sv.multicast.data, sv.multicast.cursize);
			}
		}
	}
	else
	{
		for (i = 0; i < (unsigned int)svs.maxclients; i++)
		{
			if (!svs.clients[i].active)
				continue;

			if (requireext2 && !(svs.clients[i].protocol_pext2 & requireext2))
				continue;

			//figure out which cluster (read: pvs index) to use.
			playerleaf = Mod_PointInLeaf(svs.clients[i].edict->v.origin, qcvm->worldmodel);
			cluster = playerleaf - qcvm->worldmodel->leafs;
			cluster--;	//pvs is 1-based, leaf 0 is discarded.
			if (cluster < 0 || (pvs[cluster>>3] & (1<<(cluster&7))))
			{
				//they can see it. add it in to whichever buffer is appropriate.
				if (reliable)
					SZ_Write(&svs.clients[i].message, sv.multicast.data, sv.multicast.cursize);
				else
					SZ_Write(&svs.clients[i].datagram, sv.multicast.data, sv.multicast.cursize);
			}
		}
	}
}
//FIXME: shouldn't really be using pext2, but we don't track the earlier extensions, and it should be safe enough.
static void SV_Multicast(multicast_t to, float *org, int msg_entity, unsigned int requireext2)
{
	unsigned int i;

	if (to == MULTICAST_INIT && sv.state != ss_loading)
	{
		SZ_Write (&sv.signon, sv.multicast.data, sv.multicast.cursize);
		to = MULTICAST_ALL_R;	//and send to players that are already on
	}

	switch(to)
	{
	case MULTICAST_INIT:
		SZ_Write (&sv.signon, sv.multicast.data, sv.multicast.cursize);
		break;
	case MULTICAST_ALL_R:
	case MULTICAST_ALL_U:
		PF_multicast_internal(to==MULTICAST_ALL_R, NULL, requireext2);
		break;
	case MULTICAST_PHS_R:
	case MULTICAST_PHS_U:
		//we don't support phs, that would require lots of pvs decompression+merging stuff, and many q1bsps have a LOT of leafs.
		PF_multicast_internal(to==MULTICAST_PHS_R, NULL/*Mod_LeafPHS(Mod_PointInLeaf(org, qcvm->worldmodel), qcvm->worldmodel)*/, requireext2);
		break;
	case MULTICAST_PVS_R:
	case MULTICAST_PVS_U:
		PF_multicast_internal(to==MULTICAST_PVS_R, Mod_LeafPVS(Mod_PointInLeaf(org, qcvm->worldmodel), qcvm->worldmodel), requireext2);
		break;
	case MULTICAST_ONE_R:
	case MULTICAST_ONE_U:
		i = msg_entity-1;
		if (i >= (unsigned int)svs.maxclients)
			break;
		//a unicast, which ignores pvs.
		//(unlike vanilla this allows unicast unreliables, so woo)
		if (svs.clients[i].active)
		{
			SZ_Write(((to==MULTICAST_ONE_R)?&svs.clients[i].message:&svs.clients[i].datagram), sv.multicast.data, sv.multicast.cursize);
		}
		break;
	default:
		break;
	}
	SZ_Clear(&sv.multicast);
}
static void PF_multicast(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	multicast_t to = G_FLOAT(OFS_PARM1);
	SV_Multicast(to, org, NUM_FOR_EDICT(PROG_TO_EDICT(pr_global_struct->msg_entity)), 0);
}
static void PF_randomvector(void)
{
	vec3_t temp;
	do
	{
		temp[0] = (rand() & 32767) * (2.0 / 32767.0) - 1.0;
		temp[1] = (rand() & 32767) * (2.0 / 32767.0) - 1.0;
		temp[2] = (rand() & 32767) * (2.0 / 32767.0) - 1.0;
	} while (DotProduct(temp, temp) >= 1);
	VectorCopy (temp, G_VECTOR(OFS_RETURN));
}
static void PF_checkextension(void);
static void PF_checkbuiltin(void);
static void PF_builtinsupported(void);

static void PF_uri_escape(void)
{
	static const char *hex = "0123456789ABCDEF";

	char *result = PR_GetTempString();
	char *o = result;
	const unsigned char *s = (const unsigned char*)G_STRING(OFS_PARM0);
	*result = 0;
	while (*s && o < result+STRINGTEMP_LENGTH-4)
	{
		if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')
				|| *s == '.' || *s == '-' || *s == '_')
			*o++ = *s++;
		else
		{
			*o++ = '%';
			*o++ = hex[*s>>4];
			*o++ = hex[*s&0xf];
			s++;
		}
	}
	*o = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_uri_unescape(void)
{
	const char *s = G_STRING(OFS_PARM0), *i;
	char *resultbuf = PR_GetTempString(), *o;
	unsigned char hex;
	i = s; o = resultbuf;
	while (*i && o < resultbuf+STRINGTEMP_LENGTH-2)
	{
		if (*i == '%')
		{
			hex = 0;
			if (i[1] >= 'A' && i[1] <= 'F')
				hex += i[1]-'A'+10;
			else if (i[1] >= 'a' && i[1] <= 'f')
				hex += i[1]-'a'+10;
			else if (i[1] >= '0' && i[1] <= '9')
				hex += i[1]-'0';
			else
			{
				*o++ = *i++;
				continue;
			}
			hex <<= 4;
			if (i[2] >= 'A' && i[2] <= 'F')
				hex += i[2]-'A'+10;
			else if (i[2] >= 'a' && i[2] <= 'f')
				hex += i[2]-'a'+10;
			else if (i[2] >= '0' && i[2] <= '9')
				hex += i[2]-'0';
			else
			{
				*o++ = *i++;
				continue;
			}
			*o++ = hex;
			i += 3;
		}
		else
			*o++ = *i++;
	}
	*o = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(resultbuf);
}
static void PF_crc16(void)
{
	qboolean insens = G_FLOAT(OFS_PARM0);
	const char *str = PF_VarString(1);
	size_t len = strlen(str);

	if (insens)
	{
		unsigned short	crc;

		CRC_Init (&crc);
		while (len--)
			CRC_ProcessByte(&crc, q_tolower(*str++));
		G_FLOAT(OFS_RETURN) = crc;
	}
	else
		G_FLOAT(OFS_RETURN) = CRC_Block((const byte*)str, len);
}
static void PF_digest_hex(void)
{
	const char *hashtype = G_STRING(OFS_PARM0);
	const byte *data = (const byte*)PF_VarString(1);
	size_t len = strlen((const char*)data);
	static const char *hex = "0123456789ABCDEF";
	char *resultbuf;
	byte hashdata[20];

	if (!strcmp(hashtype, "CRC16"))
	{
		int crc = CRC_Block((const byte*)data, len);
		hashdata[0] = crc&0xff;
		hashdata[1] = (crc>>8)&0xff;
		len = 2;
	}
	else if (!strcmp(hashtype, "MD4"))
	{
		Com_BlockFullChecksum((void*)data, len, hashdata);
		len = 16;
	}
	else
	{
		Con_Printf("PF_digest_hex: Unsupported digest %s\n", hashtype);
		G_INT(OFS_RETURN) = 0;
		return;
	}

	resultbuf = PR_GetTempString();
	G_INT(OFS_RETURN) = PR_SetEngineString(resultbuf);
	data = hashdata;
	while (len --> 0)
	{
		*resultbuf++ = hex[*data>>4];
		*resultbuf++ = hex[*data&0xf];
		data++;
	}
	*resultbuf = 0;
}

static void PF_strlennocol(void)
{
	int r = 0;
	struct markup_s mu;

	PR_Markup_Begin(&mu, G_STRING(OFS_PARM0), vec3_origin, 1);
	while (PR_Markup_Parse(&mu))
		r++;
	G_FLOAT(OFS_RETURN) = r;
}
static void PF_strdecolorize(void)
{
	int l, c;
	char *r = PR_GetTempString();
	struct markup_s mu;

	PR_Markup_Begin(&mu, G_STRING(OFS_PARM0), vec3_origin, 1);
	for (l = 0; l < STRINGTEMP_LENGTH-1; l++)
	{
		c = PR_Markup_Parse(&mu);
		if (!c)
			break;
		r[l] = c;
	}
	r[l] = 0;

	G_INT(OFS_RETURN) = PR_SetEngineString(r);
}
static void PF_setattachment(void)
{
	edict_t *ent = G_EDICT(OFS_PARM0);
	edict_t *tagent = G_EDICT(OFS_PARM1);
	const char *tagname = G_STRING(OFS_PARM2);
	eval_t *val;

	if (*tagname)
	{
		//we don't support md3s, or any skeletal formats, so all tag names are logically invalid for us.
		Con_DWarning("PF_setattachment: tag %s not found\n", tagname);
	}

	if ((val = GetEdictFieldValue(ent, qcvm->extfields.tag_entity)))
		val->edict = EDICT_TO_PROG(tagent);
	if ((val = GetEdictFieldValue(ent, qcvm->extfields.tag_index)))
		val->_float = 0;
}
static void PF_void_stub(void)
{
	G_FLOAT(OFS_RETURN) = 0;
}

static struct svcustomstat_s *PR_CustomStat(int idx, int type)
{
	size_t i;
	if (idx < 0 || idx >= MAX_CL_STATS)
		return NULL;
	switch(type)
	{
	case ev_ext_integer:
	case ev_float:
	case ev_vector:
	case ev_entity:
		break;
	default:
		return NULL;
	}

	for (i = 0; i < sv.numcustomstats; i++)
	{
		if (sv.customstats[i].idx == idx && (sv.customstats[i].type==ev_string) == (type==ev_string))
			break;
	}
	if (i == sv.numcustomstats)
		sv.numcustomstats++;
	sv.customstats[i].idx = idx;
	sv.customstats[i].type = type;
	sv.customstats[i].fld = 0;
	sv.customstats[i].ptr = NULL;
	return &sv.customstats[i];
}
static void PF_clientstat(void)
{
	int idx = G_FLOAT(OFS_PARM0);
	int type = G_FLOAT(OFS_PARM1);
	int fldofs = G_INT(OFS_PARM2);
	struct svcustomstat_s *stat = PR_CustomStat(idx, type);
	if (!stat)
		return;
	stat->fld = fldofs;
}
static void PF_globalstat(void)
{
	int idx = G_FLOAT(OFS_PARM0);
	int type = G_FLOAT(OFS_PARM1);
	const char *globname = G_STRING(OFS_PARM2);
	eval_t *ptr = PR_FindExtGlobal(type, globname);
	struct svcustomstat_s *stat;
	if (ptr)
	{
		stat = PR_CustomStat(idx, type);
		if (!stat)
			return;
		stat->ptr = ptr;
	}
}
static void PF_pointerstat(void)
{
	int idx = G_FLOAT(OFS_PARM0);
	int type = G_FLOAT(OFS_PARM1);
	int qcptr = G_INT(OFS_PARM2);
	struct svcustomstat_s *stat;
	if (qcptr < 0 || qcptr >= qcvm->max_edicts*qcvm->edict_size || (qcptr%qcvm->edict_size)<sizeof(edict_t)-sizeof(entvars_t))
		return;	//invalid pointer. this is a more strict check than the qcvm...
	stat = PR_CustomStat(idx, type);
	if (!stat)
		return;
	stat->ptr = (eval_t *)((byte *)qcvm->edicts + qcptr);
}

static void PF_isbackbuffered(void)
{
	unsigned int plnum = G_EDICTNUM(OFS_PARM0)-1;
	G_FLOAT(OFS_RETURN) = true;	//assume the connection is clogged.
	if (plnum > (unsigned int)svs.maxclients)
		return;	//make error?
	if (!svs.clients[plnum].active)
		return; //empty slot
	if (svs.clients[plnum].message.cursize > DATAGRAM_MTU)
		return;
	G_FLOAT(OFS_RETURN) = false;	//okay to spam with more reliables.
}

#ifdef PSET_SCRIPT
int PF_SV_ForceParticlePrecache(const char *s)
{
	unsigned int i;
	for (i = 1; i < MAX_PARTICLETYPES; i++)
	{
		if (!sv.particle_precache[i])
		{
			if (sv.state != ss_loading)
			{
				MSG_WriteByte(&sv.multicast, svcdp_precache);
				MSG_WriteShort(&sv.multicast, i|0x4000);
				MSG_WriteString(&sv.multicast, s);
				SV_Multicast(MULTICAST_ALL_R, NULL, 0, PEXT2_REPLACEMENTDELTAS); //FIXME
			}

			sv.particle_precache[i] = strcpy(Hunk_Alloc(strlen(s)+1), s);	//weirdness to avoid issues with tempstrings
			return i;
		}
		if (!strcmp(sv.particle_precache[i], s))
			return i;
	}
	return 0;
}
static void PF_sv_particleeffectnum(void)
{
	const char	*s;
	unsigned int i;
	extern cvar_t r_particledesc;

	s = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = 0;
//	PR_CheckEmptyString (s);

	if (!*s)
		return;

	if (!sv.particle_precache[1] && (!strncmp(s, "effectinfo.", 11) || strstr(r_particledesc.string, "effectinfo")))
		COM_Effectinfo_Enumerate(PF_SV_ForceParticlePrecache);

	for (i = 1; i < MAX_PARTICLETYPES; i++)
	{
		if (!sv.particle_precache[i])
		{
			if (sv.state != ss_loading)
			{
				if (pr_ext_warned_particleeffectnum++ < 3)
					Con_Warning ("PF_sv_particleeffectnum(%s): Precache should only be done in spawn functions\n", s);

				MSG_WriteByte(&sv.multicast, svcdp_precache);
				MSG_WriteShort(&sv.multicast, i|0x4000);
				MSG_WriteString(&sv.multicast, s);
				SV_Multicast(MULTICAST_ALL_R, NULL, 0, PEXT2_REPLACEMENTDELTAS);
			}

			sv.particle_precache[i] = strcpy(Hunk_Alloc(strlen(s)+1), s);	//weirdness to avoid issues with tempstrings
			G_FLOAT(OFS_RETURN) = i;
			return;
		}
		if (!strcmp(sv.particle_precache[i], s))
		{
			if (sv.state != ss_loading && !pr_checkextension.value)
			{
				if (pr_ext_warned_particleeffectnum++ < 3)
					Con_Warning ("PF_sv_particleeffectnum(%s): Precache should only be done in spawn functions\n", s);
			}
			G_FLOAT(OFS_RETURN) = i;
			return;
		}
	}
	PR_RunError ("PF_sv_particleeffectnum: overflow");
}
static void PF_sv_trailparticles(void)
{
	int efnum;
	int ednum;
	float *start = G_VECTOR(OFS_PARM2);
	float *end = G_VECTOR(OFS_PARM3);

	/*DP gets this wrong, lets try to be compatible*/
	if ((unsigned int)G_INT(OFS_PARM1) >= MAX_EDICTS*(unsigned int)qcvm->edict_size)
	{
		ednum = G_EDICTNUM(OFS_PARM0);
		efnum = G_FLOAT(OFS_PARM1);
	}
	else
	{
		efnum = G_FLOAT(OFS_PARM0);
		ednum = G_EDICTNUM(OFS_PARM1);
	}

	if (efnum <= 0)
		return;

	MSG_WriteByte(&sv.multicast, svcdp_trailparticles);
	MSG_WriteShort(&sv.multicast, ednum);
	MSG_WriteShort(&sv.multicast, efnum);
	MSG_WriteCoord(&sv.multicast, start[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, start[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, start[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[2], sv.protocolflags);

	SV_Multicast(MULTICAST_PHS_U, start, 0, PEXT2_REPLACEMENTDELTAS);
}
static void PF_sv_pointparticles(void)
{
	int efnum = G_FLOAT(OFS_PARM0);
	float *org = G_VECTOR(OFS_PARM1);
	float *vel = (qcvm->argc < 3)?vec3_origin:G_VECTOR(OFS_PARM2);
	int count = (qcvm->argc < 4)?1:G_FLOAT(OFS_PARM3);

	if (efnum <= 0)
		return;
	if (count > 65535)
		count = 65535;
	if (count < 1)
		return;

	if (count == 1 && !vel[0] && !vel[1] && !vel[2])
	{
		MSG_WriteByte(&sv.multicast, svcdp_pointparticles1);
		MSG_WriteShort(&sv.multicast, efnum);
		MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
	}
	else
	{
		MSG_WriteByte(&sv.multicast, svcdp_pointparticles);
		MSG_WriteShort(&sv.multicast, efnum);
		MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, vel[0], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, vel[1], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, vel[2], sv.protocolflags);
		MSG_WriteShort(&sv.multicast, count);
	}

	SV_Multicast(MULTICAST_PVS_U, org, 0, PEXT2_REPLACEMENTDELTAS);
}

int PF_CL_ForceParticlePrecache(const char *s)
{
	int i;

	//check if an ssqc one already exists with that name
	for (i = 1; i < MAX_PARTICLETYPES; i++)
	{
		if (!cl.particle_precache[i].name)
			break;	//nope, no more known
		if (!strcmp(cl.particle_precache[i].name, s))
			return i;
	}

	//nope, check for a csqc one, and allocate if needed
	for (i = 1; i < MAX_PARTICLETYPES; i++)
	{
		if (!cl.local_particle_precache[i].name)
		{
			cl.local_particle_precache[i].name = strcpy(Hunk_Alloc(strlen(s)+1), s);	//weirdness to avoid issues with tempstrings
			cl.local_particle_precache[i].index = PScript_FindParticleType(cl.local_particle_precache[i].name);
			return -i;
		}
		if (!strcmp(cl.local_particle_precache[i].name, s))
			return -i;
	}

	//err... too many. bum.
	return 0;
}
int PF_CL_GetParticle(int idx)
{	//negatives are csqc-originated particles, positives are ssqc-originated, for consistency allowing networking of particles as identifiers
	if (!idx)
		return P_INVALID;
	if (idx < 0)
	{
		idx = -idx;
		if (idx >= MAX_PARTICLETYPES)
			return P_INVALID;
		return cl.local_particle_precache[idx].index;
	}
	else
	{
		if (idx >= MAX_PARTICLETYPES)
			return P_INVALID;
		return cl.particle_precache[idx].index;
	}
}

static void PF_cl_particleeffectnum(void)
{
	const char	*s;

	s = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = 0;
//	PR_CheckEmptyString (s);

	if (!*s)
		return;

	G_FLOAT(OFS_RETURN) = PF_CL_ForceParticlePrecache(s);
	if (!G_FLOAT(OFS_RETURN))
		PR_RunError ("PF_cl_particleeffectnum: overflow");
}
static void PF_cl_trailparticles(void)
{
	int efnum;
	edict_t *ent;
	float *start = G_VECTOR(OFS_PARM2);
	float *end = G_VECTOR(OFS_PARM3);

	if ((unsigned int)G_INT(OFS_PARM1) >= MAX_EDICTS*(unsigned int)qcvm->edict_size)
	{	/*DP gets this wrong, lets try to be compatible*/
		ent = G_EDICT(OFS_PARM0);
		efnum = G_FLOAT(OFS_PARM1);
	}
	else
	{
		efnum = G_FLOAT(OFS_PARM0);
		ent = G_EDICT(OFS_PARM1);
	}

	if (efnum <= 0)
		return;
	efnum = PF_CL_GetParticle(efnum);
	PScript_ParticleTrail(start, end, efnum, host_frametime, -NUM_FOR_EDICT(ent), NULL, NULL/*&ent->trailstate*/);
}
static void PF_cl_pointparticles(void)
{
	int efnum = G_FLOAT(OFS_PARM0);
	float *org = G_VECTOR(OFS_PARM1);
	float *vel = (qcvm->argc < 3)?vec3_origin:G_VECTOR(OFS_PARM2);
	int count = (qcvm->argc < 4)?1:G_FLOAT(OFS_PARM3);

	if (efnum <= 0)
		return;
	if (count < 1)
		return;
	efnum = PF_CL_GetParticle(efnum);
	PScript_RunParticleEffectState (org, vel, count, efnum, NULL);
}
#else
#define PF_sv_particleeffectnum PF_void_stub
#define PF_sv_trailparticles PF_void_stub
#define PF_sv_pointparticles PF_void_stub
#define PF_cl_particleeffectnum PF_void_stub
#define PF_cl_trailparticles PF_void_stub
#define PF_cl_pointparticles PF_void_stub
#endif


static void PF_cl_getstat_int(void)
{
	int stnum = G_FLOAT(OFS_PARM0);
	if (stnum < 0 || stnum > countof(cl.stats))
		G_INT(OFS_RETURN) = 0;
	else
		G_INT(OFS_RETURN) = cl.stats[stnum];
}
static void PF_cl_getstat_float(void)
{
	int stnum = G_FLOAT(OFS_PARM0);
	if (stnum < 0 || stnum > countof(cl.stats))
		G_FLOAT(OFS_RETURN) = 0;
	else if (qcvm->argc > 1)
	{
		int firstbit = G_FLOAT(OFS_PARM1);
		int bitcount = G_FLOAT(OFS_PARM2);
		G_FLOAT(OFS_RETURN) = (cl.stats[stnum]>>firstbit) & ((1<<bitcount)-1);
	}
	else
		G_FLOAT(OFS_RETURN) = cl.statsf[stnum];
}
static void PF_cl_getstat_string(void)
{
	int stnum = G_FLOAT(OFS_PARM0);
	if (stnum < 0 || stnum > countof(cl.statss) || !cl.statss[stnum])
		G_INT(OFS_RETURN) = 0;
	else
	{
		char *result = PR_GetTempString();
		q_strlcpy(result, cl.statss[stnum], STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(result);
	}
}

static struct
{
	char name[MAX_QPATH];
	unsigned int flags;
	qpic_t *pic;
} *qcpics;
static size_t numqcpics;
static size_t maxqcpics;
void PR_ReloadPics(qboolean purge)
{
	numqcpics = 0;

	free(qcpics);
	qcpics = NULL;
	maxqcpics = 0;
}
#define PICFLAG_AUTO		0	//value used when no flags known
#define PICFLAG_WAD			(1u<<0)	//name matches that of a wad lump
//#define PICFLAG_TEMP		(1u<<1)
#define PICFLAG_WRAP		(1u<<2)	//make sure npot stuff doesn't break wrapping.
#define PICFLAG_MIPMAP		(1u<<3)	//disable use of scrap...
//#define PICFLAG_DOWNLOAD	(1u<<8)	//request to download it from the gameserver if its not stored locally.
#define PICFLAG_BLOCK		(1u<<9)	//wait until the texture is fully loaded.
#define PICFLAG_NOLOAD		(1u<<31)
static qpic_t *DrawQC_CachePic(const char *picname, unsigned int flags)
{	//okay, so this is silly. we've ended up with 3 different cache levels. qcpics, pics, and images.
	size_t i;
	unsigned int texflags;
	for (i = 0; i < numqcpics; i++)
	{	//binary search? something more sane?
		if (!strcmp(picname, qcpics[i].name))
		{
			if (qcpics[i].pic)
				return qcpics[i].pic;
			break;
		}
	}

	if (strlen(picname) >= MAX_QPATH)
		return NULL;	//too long. get lost.

	if (flags & PICFLAG_NOLOAD)
		return NULL;	//its a query, not actually needed.

	if (i+1 > maxqcpics)
	{
		maxqcpics = i + 32;
		qcpics = realloc(qcpics, maxqcpics * sizeof(*qcpics));
	}

	strcpy(qcpics[i].name, picname);
	qcpics[i].flags = flags;
	qcpics[i].pic = NULL;

	texflags = TEXPREF_ALPHA | TEXPREF_PAD | TEXPREF_NOPICMIP;
	if (flags & PICFLAG_WRAP)
		texflags &= ~TEXPREF_PAD;	//don't allow padding if its going to need to wrap (even if we don't enable clamp-to-edge normally). I just hope we have npot support.
	if (flags & PICFLAG_MIPMAP)
		texflags |= TEXPREF_MIPMAP;

	//try to load it from a wad if applicable.
	//the extra gfx/ crap is because DP insists on it for wad images. and its a nightmare to get things working in all engines if we don't accept that quirk too.
	if (flags & PICFLAG_WAD)
		qcpics[i].pic = Draw_PicFromWad2(picname + (strncmp(picname, "gfx/", 4)?0:4), texflags);
	else if (!strncmp(picname, "gfx/", 4) && !strchr(picname+4, '.'))
		qcpics[i].pic = Draw_PicFromWad2(picname+4, texflags);

	//okay, not a wad pic, try and load a lmp/tga/etc
	if (!qcpics[i].pic)
		qcpics[i].pic = Draw_TryCachePic(picname, texflags);

	if (i == numqcpics)
		numqcpics++;

	return qcpics[i].pic;
}
static void DrawQC_CharacterQuad (float x, float y, int num, float w, float h)
{
	float size = 0.0625;
	float frow = (num>>4)*size;
	float fcol = (num&15)*size;
	size = 0.0624;	//avoid rounding errors...

	glTexCoord2f (fcol, frow);
	glVertex2f (x, y);
	glTexCoord2f (fcol + size, frow);
	glVertex2f (x+w, y);
	glTexCoord2f (fcol + size, frow + size);
	glVertex2f (x+w, y+h);
	glTexCoord2f (fcol, frow + size);
	glVertex2f (x, y+h);
}
static void PF_cl_drawcharacter(void)
{
	extern gltexture_t *char_texture;

	float *pos	= G_VECTOR(OFS_PARM0);
	int charcode= (int)G_FLOAT (OFS_PARM1) & 0xff;
	float *size	= G_VECTOR(OFS_PARM2);
	float *rgb	= G_VECTOR(OFS_PARM3);
	float alpha	= G_FLOAT (OFS_PARM4);
//	int flags	= G_FLOAT (OFS_PARM5);

	if (charcode == 32)
		return; //don't waste time on spaces

	GL_Bind (char_texture);
	glColor4f (rgb[0], rgb[1], rgb[2], alpha);
	glBegin (GL_QUADS);
	DrawQC_CharacterQuad (pos[0], pos[1], charcode, size[0], size[1]);
	glEnd ();
}

static void PF_cl_drawrawstring(void)
{
	extern gltexture_t *char_texture;

	float *pos	= G_VECTOR(OFS_PARM0);
	const char *text = G_STRING (OFS_PARM1);
	float *size	= G_VECTOR(OFS_PARM2);
	float *rgb	= G_VECTOR(OFS_PARM3);
	float alpha	= G_FLOAT (OFS_PARM4);
//	int flags	= G_FLOAT (OFS_PARM5);

	float x = pos[0];
	int c;

	if (!*text)
		return; //don't waste time on spaces

	GL_Bind (char_texture);
	glColor4f (rgb[0], rgb[1], rgb[2], alpha);
	glBegin (GL_QUADS);
	while ((c = *text++))
	{
		DrawQC_CharacterQuad (x, pos[1], c, size[0], size[1]);
		x += size[0];
	}
	glEnd ();
}
static void PF_cl_drawstring(void)
{
	extern gltexture_t *char_texture;

	float *pos	= G_VECTOR(OFS_PARM0);
	const char *text = G_STRING (OFS_PARM1);
	float *size	= G_VECTOR(OFS_PARM2);
	float *rgb	= G_VECTOR(OFS_PARM3);
	float alpha	= G_FLOAT (OFS_PARM4);
//	int flags	= G_FLOAT (OFS_PARM5);

	float x = pos[0];
	struct markup_s mu;
	int c;

	if (!*text)
		return; //don't waste time on spaces

	PR_Markup_Begin(&mu, text, rgb, alpha);

	GL_Bind (char_texture);
	glBegin (GL_QUADS);
	while ((c = PR_Markup_Parse(&mu)))
	{
		glColor4fv (mu.colour);
		DrawQC_CharacterQuad (x, pos[1], c, size[0], size[1]);
		x += size[0];
	}
	glEnd ();
}
static void PF_cl_stringwidth(void)
{
	static const float defaultfontsize[] = {8,8};
	const char *text = G_STRING (OFS_PARM0);
	qboolean usecolours = G_FLOAT(OFS_PARM1);
	const float *fontsize = (qcvm->argc>2)?G_VECTOR (OFS_PARM2):defaultfontsize;
	struct markup_s mu;
	int r = 0;

	if (!usecolours)
		r = strlen(text);
	else
	{
		PR_Markup_Begin(&mu, text, vec3_origin, 1);
		while (PR_Markup_Parse(&mu))
		{
			r += 1;
		}
	}

	//primitive and lame, but hey.
	G_FLOAT(OFS_RETURN) = fontsize[0] * r;
}


static void PF_cl_drawsetclip(void)
{
	float s = PR_GetVMScale();

	float x = G_FLOAT(OFS_PARM0)*s;
	float y = G_FLOAT(OFS_PARM1)*s;
	float w = G_FLOAT(OFS_PARM2)*s;
	float h = G_FLOAT(OFS_PARM3)*s;

	glScissor(x, glheight-(y+h), w, h);
	glEnable(GL_SCISSOR_TEST);
}
static void PF_cl_drawresetclip(void)
{
	glDisable(GL_SCISSOR_TEST);
}

static void PF_cl_precachepic(void)
{
	const char *name	= G_STRING(OFS_PARM0);
	unsigned int flags = G_FLOAT(OFS_PARM1);

	G_INT(OFS_RETURN) = G_INT(OFS_PARM0);	//return input string, for convienience

	if (!DrawQC_CachePic(name, flags) && (flags & PICFLAG_BLOCK))
		G_INT(OFS_RETURN) = 0;	//return input string, for convienience
}
static void PF_cl_iscachedpic(void)
{
	const char *name	= G_STRING(OFS_PARM0);
	if (DrawQC_CachePic(name, PICFLAG_NOLOAD))
		G_FLOAT(OFS_RETURN) = true;
	else
		G_FLOAT(OFS_RETURN) = false;
}

static void PF_cl_drawpic(void)
{
	float *pos	= G_VECTOR(OFS_PARM0);
	qpic_t *pic	= DrawQC_CachePic(G_STRING(OFS_PARM1), PICFLAG_AUTO);
	float *size	= G_VECTOR(OFS_PARM2);
	float *rgb	= G_VECTOR(OFS_PARM3);
	float alpha	= G_FLOAT (OFS_PARM4);
//	int flags	= G_FLOAT (OFS_PARM5);

	if (pic)
	{
		glColor4f (rgb[0], rgb[1], rgb[2], alpha);
		Draw_SubPic (pos[0], pos[1], size[0], size[1], pic, 0, 0, 1, 1);
	}
}

static void PF_cl_getimagesize(void)
{
	qpic_t *pic	= DrawQC_CachePic(G_STRING(OFS_PARM0), PICFLAG_AUTO);
	if (pic)
		G_VECTORSET(OFS_RETURN, pic->width, pic->height, 0);
	else
		G_VECTORSET(OFS_RETURN, 0, 0, 0);
}

static void PF_cl_drawsubpic(void)
{
	float *pos	= G_VECTOR(OFS_PARM0);
	float *size	= G_VECTOR(OFS_PARM1);
	qpic_t *pic	= DrawQC_CachePic(G_STRING(OFS_PARM2), PICFLAG_AUTO);
	float *srcpos	= G_VECTOR(OFS_PARM3);
	float *srcsize	= G_VECTOR(OFS_PARM4);
	float *rgb	= G_VECTOR(OFS_PARM5);
	float alpha	= G_FLOAT (OFS_PARM6);
//	int flags	= G_FLOAT (OFS_PARM7);

	if (pic)
	{
		glColor4f (rgb[0], rgb[1], rgb[2], alpha);
		Draw_SubPic (pos[0], pos[1], size[0], size[1], pic, srcpos[0], srcpos[1], srcsize[0], srcsize[1]);
	}
}

static void PF_cl_drawfill(void)
{
	float *pos	= G_VECTOR(OFS_PARM0);
	float *size	= G_VECTOR(OFS_PARM1);
	float *rgb	= G_VECTOR(OFS_PARM2);
	float alpha	= G_FLOAT (OFS_PARM3);
//	int flags	= G_FLOAT (OFS_PARM4);

	glDisable (GL_TEXTURE_2D);

	glColor4f (rgb[0], rgb[1], rgb[2], alpha);

	glBegin (GL_QUADS);
	glVertex2f (pos[0],			pos[1]);
	glVertex2f (pos[0]+size[0],	pos[1]);
	glVertex2f (pos[0]+size[0],	pos[1]+size[1]);
	glVertex2f (pos[0],			pos[1]+size[1]);
	glEnd ();

	glEnable (GL_TEXTURE_2D);
}


static qpic_t *polygon_pic;
#define MAX_POLYVERTS
static polygonvert_t polygon_verts[256];
static unsigned int polygon_numverts;
static void PF_R_PolygonBegin(void)
{
	qpic_t *pic	= DrawQC_CachePic(G_STRING(OFS_PARM0), PICFLAG_AUTO);
	int flags = (qcvm->argc>1)?G_FLOAT(OFS_PARM1):0;
	int is2d = (qcvm->argc>2)?G_FLOAT(OFS_PARM2):0;

	if (!is2d)
		PR_RunError ("PF_R_PolygonBegin: scene polygons are not supported");
	if (flags)
		PR_RunError ("PF_R_PolygonBegin: modifier flags are not supported");

	polygon_pic = pic;
	polygon_numverts = 0;
}
static void PF_R_PolygonVertex(void)
{
	polygonvert_t *v = &polygon_verts[polygon_numverts];
	if (polygon_numverts == countof(polygon_verts))
		return;	//panic!
	polygon_numverts++;

	v->xy[0] = G_FLOAT(OFS_PARM0+0);
	v->xy[1] = G_FLOAT(OFS_PARM0+1);
	v->st[0] = G_FLOAT(OFS_PARM1+0);
	v->st[1] = G_FLOAT(OFS_PARM1+1);
	v->rgba[0] = G_FLOAT(OFS_PARM2+0);
	v->rgba[1] = G_FLOAT(OFS_PARM2+1);
	v->rgba[2] = G_FLOAT(OFS_PARM2+2);
	v->rgba[3] = G_FLOAT(OFS_PARM3);
}
static void PF_R_PolygonEnd(void)
{
	if (polygon_pic)
		Draw_PicPolygon(polygon_pic, polygon_numverts, polygon_verts);
	polygon_numverts = 0;
}

static void PF_cl_cprint(void)
{
	const char *str = PF_VarString(0);
	SCR_CenterPrint (str);
}
static void PF_cl_keynumtostring(void)
{
	int keynum = Key_QCToNative(G_FLOAT(OFS_PARM0));
	char *s = PR_GetTempString();
	if (keynum < 0)
		keynum = -1;
	Q_strncpy(s, Key_KeynumToString(keynum), STRINGTEMP_LENGTH);
	G_INT(OFS_RETURN) = PR_SetEngineString(s);
}
static void PF_cl_stringtokeynum(void)
{
	const char *keyname = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = Key_NativeToQC(Key_StringToKeynum(keyname));
}
static void PF_cl_getkeybind(void)
{
	int keynum = Key_QCToNative(G_FLOAT(OFS_PARM0));
	int bindmap = (qcvm->argc<=1)?0:G_FLOAT(OFS_PARM1);
	char *s = PR_GetTempString();
	if (bindmap < 0 || bindmap >= MAX_BINDMAPS)
		bindmap = 0;
	if (keynum >= 0 && keynum < MAX_KEYS && keybindings[bindmap][keynum])
		Q_strncpy(s, keybindings[bindmap][keynum], STRINGTEMP_LENGTH);
	else
		Q_strncpy(s, "", STRINGTEMP_LENGTH);
	G_INT(OFS_RETURN) = PR_SetEngineString(s);
}
static void PF_cl_setkeybind(void)
{
	int keynum = Key_QCToNative(G_FLOAT(OFS_PARM0));
	const char *binding = G_STRING(OFS_PARM1);
	int bindmap = (qcvm->argc<=1)?0:G_FLOAT(OFS_PARM2);
	int modifier = (qcvm->argc<=2)?0:G_FLOAT(OFS_PARM3);	//shift,alt,ctrl bitmask.
	if (modifier != 0)	//we don't support modifiers.
		return;
	if (bindmap < 0 || bindmap >= MAX_BINDMAPS)
		bindmap = 0;
	if (keynum >= 0 && keynum < MAX_KEYS)
		Key_SetBinding(keynum, binding, bindmap);
}
static void PF_cl_getbindmaps(void)
{
	G_FLOAT(OFS_RETURN+0) = key_bindmap[0];
	G_FLOAT(OFS_RETURN+1) = key_bindmap[1];
	G_FLOAT(OFS_RETURN+2) = 0;
}
static void PF_cl_setbindmaps(void)
{
	float *bm = G_VECTOR(OFS_PARM0);
	key_bindmap[0] = bm[0];
	key_bindmap[1] = bm[1];

	if (key_bindmap[0] < 0 || key_bindmap[0] >= MAX_BINDMAPS)
		key_bindmap[0] = 0;
	if (key_bindmap[1] < 0 || key_bindmap[1] >= MAX_BINDMAPS)
		key_bindmap[1] = 0;

	G_FLOAT(OFS_RETURN) = false;
}
static void PF_cl_findkeysforcommand(void)
{
	const char *command = G_STRING(OFS_PARM0);
	int bindmap = (qcvm->argc>1)?G_FLOAT(OFS_PARM1):0;
	int keys[5];
	char gah[64];
	char *s = PR_GetTempString();
	int		count = 0;
	int		j;
	int		l = strlen(command);
	const char	*b;
	if (bindmap < 0 || bindmap >= MAX_BINDMAPS)
		bindmap = 0;

	for (j = 0; j < MAX_KEYS; j++)
	{
		b = keybindings[bindmap][j];
		if (!b)
			continue;
		if (!strncmp (b, command, l) && (!b[l] || (!strchr(command, ' ') && (b[l] == ' ' || b[l] == '\t'))))
		{
			keys[count++] = Key_NativeToQC(j);
			if (count == countof(keys))
				break;
		}
	}

	while (count < 2)
		keys[count++] = -1;	//always return at least two keys. DP behaviour.

	*s = 0;
	for (j = 0; j < count; j++)
	{
		if (*s)
			q_strlcat(s, " ", STRINGTEMP_LENGTH);

		//FIXME: This should be "'%i'", but our tokenize builtin doesn't support DP's fuckage, so this tends to break everything. So I'm just going to keep things sane by being less compatible-but-cleaner here.
		q_snprintf(gah, sizeof(gah), "%i", keys[j]);
		q_strlcat(s, gah, STRINGTEMP_LENGTH);
	}

	G_INT(OFS_RETURN) = PR_SetEngineString(s);
}
//this extended version returns actual key names. which modifiers can be returned.
static void PF_cl_findkeysforcommandex(void)
{
	const char *command = G_STRING(OFS_PARM0);
	int bindmap = (qcvm->argc<=1)?0:G_FLOAT(OFS_PARM1);
	int keys[16];
	char *s = PR_GetTempString();
	int		count = 0;
	int		j;
	int		l = strlen(command);
	const char	*b;
	if (bindmap < 0 || bindmap >= MAX_BINDMAPS)
		bindmap = 0;

	for (j = 0; j < MAX_KEYS; j++)
	{
		b = keybindings[bindmap][j];
		if (!b)
			continue;
		if (!strncmp (b, command, l) && (!b[l] || (!strchr(command, ' ') && (b[l] == ' ' || b[l] == '\t'))))
		{
			keys[count++] = j;
			if (count == countof(keys))
				break;
		}
	}

	*s = 0;
	for (j = 0; j < count; j++)
	{
		if (*s)
			q_strlcat(s, " ", STRINGTEMP_LENGTH);

		q_strlcat(s, Key_KeynumToString(keys[j]), STRINGTEMP_LENGTH);
	}

	G_INT(OFS_RETURN) = PR_SetEngineString(s);
}

static void PF_cl_setcursormode(void)
{
	qboolean absmode = G_FLOAT(OFS_PARM0);
	const char *cursorname = (qcvm->argc<=1)?"":G_STRING(OFS_PARM1);
	float *hotspot = (qcvm->argc<=2)?NULL:G_VECTOR(OFS_PARM2);
	float cursorscale = (qcvm->argc<=3)?1:G_FLOAT(OFS_PARM3);

	VID_SetCursor(qcvm, cursorname, hotspot, cursorscale);
	qcvm->cursorforced = absmode;
}
static void PF_cl_getcursormode(void)
{
//	qboolean effectivemode = (qcvm->argc==0)?false:G_FLOAT(OFS_PARM0);

//	if (effectivemode)
//		G_FLOAT(OFS_RETURN) = qcvm->cursorforced;
//	else
		G_FLOAT(OFS_RETURN) = qcvm->cursorforced;
}
static void PF_cl_setsensitivity(void)
{
	float sens = G_FLOAT(OFS_PARM0);

	cl.csqc_sensitivity = sens;
}
void PF_cl_playerkey_internal(int player, const char *key, qboolean retfloat)
{
	char buf[1024];
	const char *ret = buf;
	extern int	fragsort[MAX_SCOREBOARD];
	extern int	scoreboardlines;
	extern int	Sbar_ColorForMap (int m);
	if (player < 0 && player >= -scoreboardlines)
		player = fragsort[-1-player];
	if (player < 0 || player >= MAX_SCOREBOARD)
		ret = NULL;
	else if (!strcmp(key, "viewentity"))
		q_snprintf(buf, sizeof(buf), "%i", player+1);	//hack for DP compat. always returned even when the slot is empty (so long as its valid).
	else if (!*cl.scores[player].name)
		ret = NULL;
	else if (!strcmp(key, "name"))
		ret = cl.scores[player].name;
	else if (!strcmp(key, "frags"))
		q_snprintf(buf, sizeof(buf), "%i", cl.scores[player].frags);
	else if (!strcmp(key, "ping"))
		q_snprintf(buf, sizeof(buf), "%i", cl.scores[player].ping);
	else if (!strcmp(key, "pl"))
		ret = NULL;	//unknown
	else if (!strcmp(key, "entertime"))
		q_snprintf(buf, sizeof(buf), "%g", cl.scores[player].entertime);
	else if (!strcmp(key, "topcolor_rgb"))
	{
		byte *pal = (byte *)d_8to24table + 4*Sbar_ColorForMap((cl.scores[player].colors)&0xf0); //johnfitz -- use d_8to24table instead of host_basepal
		q_snprintf(buf, sizeof(buf), "%g %g %g", pal[0]/255.0, pal[1]/255.0, pal[2]/255.0);
	}
	else if (!strcmp(key, "bottomcolor_rgb"))
	{
		byte *pal = (byte *)d_8to24table + 4*Sbar_ColorForMap((cl.scores[player].colors<<4)&0xf0); //johnfitz -- use d_8to24table instead of host_basepal
		q_snprintf(buf, sizeof(buf), "%g %g %g", pal[0]/255.0, pal[1]/255.0, pal[2]/255.0);
	}
	else if (!strcmp(key, "topcolor"))
		q_snprintf(buf, sizeof(buf), "%i", (cl.scores[player].colors>>4)&0xf);
	else if (!strcmp(key, "bottomcolor"))
		q_snprintf(buf, sizeof(buf), "%i", cl.scores[player].colors&0xf);
	else if (!strcmp(key, "team"))	//quakeworld uses team infokeys to decide teams (instead of colours). but NQ never did, so that's fun. Lets allow mods to use either so that they can favour QW and let the engine hide differences .
		q_snprintf(buf, sizeof(buf), "%i", (cl.scores[player].colors&0xf)+1);
	else if (!strcmp(key, "userid"))
		ret = NULL;	//unknown
//	else if (!strcmp(key, "vignored"))	//checks to see this player's voicechat is ignored.
//		q_snprintf(buf, sizeof(buf), "%i", (int)cl.scores[player].vignored);
	else if (!strcmp(key, "voipspeaking"))
		q_snprintf(buf, sizeof(buf), "%i", S_Voip_Speaking(player));
	else if (!strcmp(key, "voiploudness"))
	{
		if (player == cl.viewentity-1)
			q_snprintf(buf, sizeof(buf), "%i", S_Voip_Loudness(false));
		else
			ret = NULL;
	}
	else
	{
		ret = Info_GetKey(cl.scores[player].userinfo, key, buf, sizeof(buf));
		if (!*ret)
			ret = NULL;
	}

	if (retfloat)
		G_FLOAT(OFS_RETURN) = ret?atof(ret):0;
	else
		G_INT(OFS_RETURN) = ret?PR_MakeTempString(ret):0;
}
static void PF_cl_playerkey_s(void)
{
	int playernum = G_FLOAT(OFS_PARM0);
	const char *keyname = G_STRING(OFS_PARM1);
	PF_cl_playerkey_internal(playernum, keyname, false);
}
static void PF_cl_playerkey_f(void)
{
	int playernum = G_FLOAT(OFS_PARM0);
	const char *keyname = G_STRING(OFS_PARM1);
	PF_cl_playerkey_internal(playernum, keyname, true);
}
static void PF_cl_isdemo(void)
{
	G_FLOAT(OFS_RETURN) = !!cls.demoplayback;
}
static void PF_cl_isserver(void)
{
	//this is a bit of a mess because its not well defined.
	if (sv.active && svs.maxclients > 1)
		G_FLOAT(OFS_RETURN) = 1;
	else if (sv.active)
		G_FLOAT(OFS_RETURN) = 0.5;
	else
		G_FLOAT(OFS_RETURN) = 0;
}
static void PF_cl_registercommand(void)
{
	const char *cmdname = G_STRING(OFS_PARM0);
	Cmd_AddCommand(cmdname, NULL);
}
void PF_cl_serverkey_internal(const char *key, qboolean retfloat)
{
	char buf[1024];
	const char *ret;
	if (!strcmp(key, "constate"))
	{
		if (cls.state != ca_connected)
			ret = "disconnected";
		else if (cls.signon == SIGNONS)
			ret = "active";
		else
			ret = "connecting";
	}
	else
	{
		ret = Info_GetKey(cl.serverinfo, key, buf, sizeof(buf));
	}

	if (retfloat)
		G_FLOAT(OFS_RETURN) = atof(ret);
	else
		G_INT(OFS_RETURN) = PR_SetEngineString(ret);
}
static void PF_cl_serverkey_s(void)
{
	const char *keyname = G_STRING(OFS_PARM0);
	PF_cl_serverkey_internal(keyname, false);
}
static void PF_cl_serverkey_f(void)
{
	const char *keyname = G_STRING(OFS_PARM0);
	PF_cl_serverkey_internal(keyname, true);
}

static void PF_sv_forceinfokey(void)
{
	int edict = G_EDICTNUM(OFS_PARM0);
	const char *keyname = G_STRING(OFS_PARM1);
	const char *value = G_STRING(OFS_PARM2);
	SV_UpdateInfo(edict, keyname, value);
}

static void PF_cl_readbyte(void)
{
	G_FLOAT(OFS_RETURN) = MSG_ReadByte();
}
static void PF_cl_readchar(void)
{
	G_FLOAT(OFS_RETURN) = MSG_ReadChar();
}
static void PF_cl_readshort(void)
{
	G_FLOAT(OFS_RETURN) = MSG_ReadShort();
}
static void PF_cl_readlong(void)
{
	G_FLOAT(OFS_RETURN) = MSG_ReadLong();
}
static void PF_cl_readcoord(void)
{
	G_FLOAT(OFS_RETURN) = MSG_ReadCoord(cl.protocolflags);
}
static void PF_cl_readangle(void)
{
	G_FLOAT(OFS_RETURN) = MSG_ReadAngle(cl.protocolflags);
}
static void PF_cl_readstring(void)
{
	G_INT(OFS_RETURN) = PR_MakeTempString(MSG_ReadString());
}
static void PF_cl_readfloat(void)
{
	G_FLOAT(OFS_RETURN) = MSG_ReadFloat();
}
static void PF_cl_readentitynum(void)
{
	G_FLOAT(OFS_RETURN) = MSG_ReadEntity(cl.protocol_pext2);
}
static void PF_cl_sendevent(void)
{
	const char *eventname = G_STRING(OFS_PARM0);
	const char *eventargs = G_STRING(OFS_PARM1);
	int a;
	eval_t *val;
	edict_t *ed;

	MSG_WriteByte(&cls.message, clcfte_qcrequest);
	for (a = 2; a < 8 && *eventargs; a++, eventargs++)
	{
		switch(*eventargs)
		{
		case 's':
			MSG_WriteByte(&cls.message, ev_string);
			MSG_WriteString(&cls.message, G_STRING(OFS_PARM0+a*3));
			break;
		case 'f':
			MSG_WriteByte(&cls.message, ev_float);
			MSG_WriteFloat(&cls.message, G_FLOAT(OFS_PARM0+a*3));
			break;
		case 'i':
			MSG_WriteByte(&cls.message, ev_ext_integer);
			MSG_WriteLong(&cls.message, G_INT(OFS_PARM0+a*3));
			break;
		case 'v':
			MSG_WriteByte(&cls.message, ev_vector);
			MSG_WriteFloat(&cls.message, G_FLOAT(OFS_PARM0+a*3+0));
			MSG_WriteFloat(&cls.message, G_FLOAT(OFS_PARM0+a*3+1));
			MSG_WriteFloat(&cls.message, G_FLOAT(OFS_PARM0+a*3+2));
			break;
		case 'e':
			ed = G_EDICT(OFS_PARM0+a*3);
			MSG_WriteByte(&cls.message, ev_entity);
			val = GetEdictFieldValue(ed, ED_FindFieldOffset("entnum"));	//we need to transmit the SERVER's number, the client's number is meaningless to it.
			MSG_WriteEntity(&cls.message, val?val->_float:0, cl.protocol_pext2);
			break;
		}
	}
	MSG_WriteByte(&cls.message, 0);
	MSG_WriteString(&cls.message, eventname);
}

static void PF_cl_setwindowcaption(void)
{
	VID_SetWindowCaption(G_STRING(OFS_PARM0));
}

static void PF_m_setkeydest(void)
{
	int d = G_FLOAT(OFS_PARM0);
	switch(d)
	{
	case 0:
		key_dest = key_game;
		break;
	case 1:
		key_dest = key_message;
		break;
	case 2:
		key_dest = key_menu;
		break;
	default:
		break;
	}
}
static void PF_m_getkeydest(void)
{
	switch(key_dest)
	{
	case key_game:
		G_FLOAT(OFS_RETURN) = 0;
		break;
	case key_message:
		G_FLOAT(OFS_RETURN) = 1;
		break;
	case key_menu:
		G_FLOAT(OFS_RETURN) = 2;
		break;
	default:
		G_FLOAT(OFS_RETURN) = 0;
		break;
	}
}
static void PF_m_getmousepos(void)
{
	float s = PR_GetVMScale();
	G_FLOAT(OFS_RETURN+0) = vid.cursorpos[0]/s;
	G_FLOAT(OFS_RETURN+1) = vid.cursorpos[1]/s;
	G_FLOAT(OFS_RETURN+2) = 0;
}
static void PF_m_setmousetarget(void)
{
	int d = G_FLOAT(OFS_PARM0);
	switch(d)
	{
	case 1:
		qcvm->cursorforced = false;
		break;
	case 2:
		qcvm->cursorforced = true;
		break;
	default:
		break;
	}
}
static void PF_m_getmousetarget(void)
{
	if (qcvm->cursorforced)
		G_FLOAT(OFS_RETURN) = 2;
	else
		G_FLOAT(OFS_RETURN) = 1;
}
static void PF_cl_getresolution(void)
{
	int mode = G_FLOAT(OFS_PARM0);
	static struct
	{
		int w, h;
	} modes[] = {
		{640,480},{800,600},{1024,768},

		{1366,768},
		{1920,1080},
		{2560,1440}
	};
	if (mode == -1)
		G_VECTORSET(OFS_RETURN, 0,0,0);
	else if (mode < 0 || mode >= countof(modes))
		G_VECTORSET(OFS_RETURN, 0,0,0);
	else
		G_VECTORSET(OFS_RETURN, modes[mode].w,modes[mode].h,1);
}

enum
{
	GGDI_GAMEDIR = 0,   /* Used with getgamedirinfo to query the mod's public gamedir. There is often other info that cannot be expressed with just a gamedir name, resulting in dupes or other weirdness. */
	GGDI_DESCRIPTION = 1,   /* The human-readable title of the mod. Empty when no data is known (ie: the gamedir just contains some maps). */
	GGDI_OVERRIDES = 2, /* A list of settings overrides. */
	GGDI_LOADCOMMAND = 3,   /* The console command needed to actually load the mod. */
	GGDI_ICON = 4,  /* The mod's Icon path, ready for drawpic. */
	GGDI_GAMEDIRLIST = 5,   /* A semi-colon delimited list of gamedirs that the mod's content can be loaded through. */
};
static void PF_getgamedirinfo(void)
{
	int diridx = G_FLOAT(OFS_PARM0);
	int prop = G_FLOAT(OFS_PARM1);

	struct filelist_item_s *mod = modlist;
	char *s;

	G_INT(OFS_RETURN) = 0;
	if (diridx < 0)
		return;

	while (mod && diridx --> 0)
		mod = mod->next;

	switch(prop)
	{
	case GGDI_GAMEDIR:	//gamedir name
		G_INT(OFS_RETURN) = PR_SetEngineString(mod->name);
		break;
	case GGDI_LOADCOMMAND:	//loadcommand (localcmd it)
		s = PR_GetTempString();
		q_snprintf(s, STRINGTEMP_LENGTH, "gamedir \"%s\"\n", mod->name);
		G_INT(OFS_RETURN) = PR_SetEngineString(s);
		break;
	case GGDI_DESCRIPTION:	//gamedir description
	case GGDI_OVERRIDES:	//custom overrides
	case GGDI_ICON:	//icon name (drawpic it)
	case GGDI_GAMEDIRLIST:	//id1;hipnotic;quoth type list
	default:	//unknown
		G_INT(OFS_RETURN) = 0;
		break;
	}
}


enum hostfield_e
{
	SLKEY_INVALID=-1,
//	SLKEY_PING,
	SLKEY_NAME,
	SLKEY_ADDRESS,
	SLKEY_GAMEDIR,
	SLKEY_MAP,
	SLKEY_NUMPLAYERS,
	SLKEY_MAXPLAYERS,
};

#include "../headers/arch_def.h"
#include "../headers/net_sys.h"
#include "../headers/net_defs.h"
static size_t hostCacheSortCount;
static size_t hostCacheSort[HOSTCACHESIZE];
static enum hostfield_e serversort_field;
static int serversort_descending;
enum hostcacheprop_e {
		SLIST_HOSTCACHEVIEWCOUNT,
		SLIST_HOSTCACHETOTALCOUNT,
		SLIST_MASTERQUERYCOUNT_DP,
		SLIST_MASTERREPLYCOUNT_DP,
		SLIST_SERVERQUERYCOUNT_DP,
		SLIST_SERVERREPLYCOUNT_DP,
		SLIST_SORTFIELD,
		SLIST_SORTDESCENDING
};
static void PF_gethostcachevalue(void)
{
	switch((enum hostcacheprop_e)G_FLOAT(OFS_PARM0))
	{
	case SLIST_HOSTCACHEVIEWCOUNT:
		G_FLOAT(OFS_RETURN) = hostCacheSortCount?hostCacheSortCount:hostCacheCount;
		break;
	case SLIST_HOSTCACHETOTALCOUNT:
		G_FLOAT(OFS_RETURN) = hostCacheCount;
		break;
	case SLIST_MASTERQUERYCOUNT_DP:
	case SLIST_MASTERREPLYCOUNT_DP:
	case SLIST_SERVERQUERYCOUNT_DP:
	case SLIST_SERVERREPLYCOUNT_DP:
		G_FLOAT(OFS_RETURN) = 0;
		break;
	case SLIST_SORTFIELD:
		G_FLOAT(OFS_RETURN) = serversort_field;
		break;
	case SLIST_SORTDESCENDING:
		G_FLOAT(OFS_RETURN) = serversort_descending;
		break;
	}
}
static void PF_gethostcachestring(void)
{
	enum hostfield_e fld = G_FLOAT(OFS_PARM0);
	int idx = G_FLOAT(OFS_PARM1);
	char *s;
	if (idx >= 0 && idx < hostCacheSortCount)
		idx = hostCacheSort[idx];
	else if (hostCacheSortCount)
		idx = -1;
	if (idx < 0 || idx >= hostCacheCount)
	{
		G_INT(OFS_RETURN) = 0;
		return;
	}

	switch(fld)
	{
	case SLKEY_NAME:
		G_INT(OFS_RETURN) = PR_SetEngineString(hostcache[idx].name);
		break;
	case SLKEY_ADDRESS:
		G_INT(OFS_RETURN) = PR_SetEngineString(hostcache[idx].cname);
		break;
	case SLKEY_GAMEDIR:
		G_INT(OFS_RETURN) = PR_SetEngineString(hostcache[idx].gamedir);
		break;
	case SLKEY_MAP:
		G_INT(OFS_RETURN) = PR_SetEngineString(hostcache[idx].map);
		break;
	case SLKEY_MAXPLAYERS:
		s = PR_GetTempString();
		q_snprintf(s, STRINGTEMP_LENGTH, "%d", hostcache[idx].maxusers);
		G_INT(OFS_RETURN) = PR_SetEngineString(s);
		break;
	case SLKEY_NUMPLAYERS:
		s = PR_GetTempString();
		q_snprintf(s, STRINGTEMP_LENGTH, "%d", hostcache[idx].users);
		G_INT(OFS_RETURN) = PR_SetEngineString(s);
		break;

	default:
		G_INT(OFS_RETURN) = 0;
		break;
	}
}
static void PF_gethostcachenumber(void)
{
	enum hostfield_e fld = G_FLOAT(OFS_PARM0);
	int idx = G_FLOAT(OFS_PARM1);
	if (idx >= 0 && idx < hostCacheSortCount)
		idx = hostCacheSort[idx];
	else if (hostCacheSortCount)
		idx = -1;
	if (idx < 0 || idx >= hostCacheCount)
	{
		G_FLOAT(OFS_RETURN) = 0;
		return;
	}

	switch(fld)
	{
	case SLKEY_MAXPLAYERS:
		G_FLOAT(OFS_RETURN) = hostcache[idx].maxusers;
		break;
	case SLKEY_NUMPLAYERS:
		G_FLOAT(OFS_RETURN) = hostcache[idx].users;
		break;

	//these don't make sense as floats.
	case SLKEY_NAME:
	case SLKEY_ADDRESS:
	case SLKEY_GAMEDIR:
	case SLKEY_MAP:
	default:
		G_FLOAT(OFS_RETURN) = 0;
		break;
	}
}

static void PF_resethostcachemasks(void){G_VECTORSET(OFS_RETURN, 0,0,0);}
static void PF_sethostcachemaskstring(void){G_VECTORSET(OFS_RETURN, 0,0,0);}
static void PF_sethostcachemasknumber(void){G_VECTORSET(OFS_RETURN, 0,0,0);}

static int CompareSortServers (const void *va, const void *vb)
{
	size_t idx1 = *(const size_t*)va;
	size_t idx2 = *(const size_t*)vb;
	int r;
	switch(serversort_field)
	{
	case SLKEY_NUMPLAYERS:
		if (hostcache[idx1].users == hostcache[idx2].users)
			r = 0;
		else r = (hostcache[idx1].users>hostcache[idx2].users)?1:-1;
		break;
	case SLKEY_MAXPLAYERS:
		if (hostcache[idx1].maxusers == hostcache[idx2].maxusers)
			r = 0;
		else r = (hostcache[idx1].maxusers>hostcache[idx2].maxusers)?1:-1;
		break;
	case SLKEY_ADDRESS:
		r = strcmp(hostcache[idx1].cname, hostcache[idx2].cname);
		break;
	case SLKEY_GAMEDIR:
		r = strcmp(hostcache[idx1].cname, hostcache[idx2].cname);
		break;
	case SLKEY_MAP:
		r = strcmp(hostcache[idx1].map, hostcache[idx2].map);
		break;
	default:
	case SLKEY_NAME:
		r = strcmp(hostcache[idx1].name, hostcache[idx2].name);
		break;
	}
	if (serversort_descending)
		r = -r;
	return r;
}
static void PF_resorthostcache(void)
{
	size_t i;
	for (hostCacheSortCount = 0, i = 0; i < hostCacheCount; i++)
		hostCacheSort[hostCacheSortCount++] = i;
	qsort(hostCacheSort, hostCacheSortCount, sizeof(hostCacheSort[0]), CompareSortServers);
}
static void PF_sethostcachesort(void)
{
	serversort_field = G_FLOAT(OFS_PARM0);
	serversort_descending = G_FLOAT(OFS_PARM1);
}

static void PF_refreshhostcache(void)
{
	hostCacheSortCount = 0;
	slistSilent = true;
	slistScope = SLIST_INTERNET;
	NET_Slist_f();
}
static void PF_gethostcacheindexforkey(void)
{
	const char *key = G_STRING(OFS_PARM0);
	enum hostfield_e r = SLKEY_INVALID;
	     if (!strcmp(key, "cname"))		r = SLKEY_ADDRESS;
//	else if (!strcmp(key, "ping"))		r = SLKEY_PING;
//	else if (!strcmp(key, "game"))		r = SLKEY_PROTONAME;
	else if (!strcmp(key, "mod"))		r = SLKEY_GAMEDIR;
	else if (!strcmp(key, "map"))		r = SLKEY_MAP;
	else if (!strcmp(key, "name"))		r = SLKEY_NAME;
//	else if (!strcmp(key, "qcstatus"))	r = SLKEY_QCSTATUS;
//	else if (!strcmp(key, "players"))	r = SLKEY_PLAYERS;
	else if (!strcmp(key, "maxplayers"))r = SLKEY_MAXPLAYERS;
	else if (!strcmp(key, "numplayers"))r = SLKEY_NUMPLAYERS;
//	else if (!strcmp(key, "numbots"))	r = SLKEY_NUMBOTS;
//	else if (!strcmp(key, "numhumans"))	r = SLKEY_NUMHUMANS;
//	else if (!strcmp(key, "freeslots"))	r = SLKEY_FREESLOTS;
//	else if (!strcmp(key, "protocol"))	r = SLKEY_PROTOVER;
//	else if (!strcmp(key, "category"))	r = SLKEY_CATEGORY;
//	else if (!strcmp(key, "isfavorite"))r = SLKEY_ISFAVORITE;
	G_FLOAT(OFS_RETURN) = r;
}
//static void PF_addwantedhostcachekey(void){G_VECTORSET(OFS_RETURN, 0,0,0);}
//static void PF_getextresponse(void){G_VECTORSET(OFS_RETURN, 0,0,0);}
//static void PF_netaddress_resolve(void){G_VECTORSET(OFS_RETURN, 0,0,0);}
static void PF_uri_get(void){G_VECTORSET(OFS_RETURN, 0,0,0);}

static const char *csqcmapentitydata;
static void PF_cs_getentitytoken(void)
{
	if (qcvm->argc)
	{
		csqcmapentitydata = cl.worldmodel->entities;
		G_INT(OFS_RETURN) = 0;
		return;
	}

	if (csqcmapentitydata)
	{
		csqcmapentitydata = COM_Parse(csqcmapentitydata);
		if (!csqcmapentitydata)
			G_INT(OFS_RETURN) = 0;
		else
			G_INT(OFS_RETURN) = PR_MakeTempString(com_token);
	}
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_m_setmodel(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	const char *newmodel = G_STRING(OFS_PARM1);

	qmodel_t *model = Mod_ForName(newmodel, false);
	eval_t *emodel = GetEdictFieldValue(ed, ED_FindFieldOffset("model"));
	eval_t *emins = GetEdictFieldValue(ed, ED_FindFieldOffset("mins"));
	eval_t *emaxs = GetEdictFieldValue(ed, ED_FindFieldOffset("maxs"));
	if (emodel)
		emodel->_int = PR_SetEngineString(model?model->name:"");
	if (emins)
	{
		if (model)
		{
			VectorCopy(model->mins, emins->vector);
		}
		else
		{
			VectorCopy(vec3_origin, emins->vector);
		}
	}
	if (emaxs)
	{
		if (model)
		{
			VectorCopy(model->maxs, emaxs->vector);
		}
		else
		{
			VectorCopy(vec3_origin, emaxs->vector);
		}
	}
}
static void PF_m_precache_model(void)
{
	const char *modname = G_STRING(OFS_PARM0);
	G_INT(OFS_RETURN) = G_INT(OFS_PARM0);
	Mod_ForName(modname, false);
}
entity_t *CL_NewTempEntity (void);
extern int num_temp_entities;
enum
{
	RF_VIEWMODEL		= 1u<<0,	//uses a different view matrix, hidden from mirrors.
	RF_EXTERNALMODEL	= 1u<<1,	//hidden except in mirrors.
	RF_DEPTHHACK		= 1u<<2,	//uses a different projection matrix (with depth squished by a third, optionally also using separate viewmodel fov)
	RF_ADDITIVE			= 1u<<3,	//additive blend mode, because .effects was meant to be handled by the csqc.
	RF_USEAXIS			= 1u<<4,	//explicit matrix defined in v_forward/v_right/v_up/.origin instead of calculating from angles (yay skew+scale)
	RF_NOSHADOW			= 1u<<5,	//disable shadows, because .effects was meant to be handled by the csqc.
	RF_WEIRDFRAMETIMES	= 1u<<6,	//change frame*time fields to need to be translated as realframeNtime='time-frameNtime' to determine which pose to use.
						//CONSULT OTHER ENGINES BEFORE ADDING NEW FLAGS.
};
static void PR_addentity_internal(edict_t *ed)	//adds a csqc entity into the scene.
{
	qmodel_t *model = qcvm->GetModel(ed->v.modelindex);

	if (model)
	{
		entity_t *e = CL_NewTempEntity();
		if (e)
		{
			eval_t *frame2 = GetEdictFieldValue(ed, qcvm->extfields.frame2);
			eval_t *lerpfrac = GetEdictFieldValue(ed, qcvm->extfields.lerpfrac);
			eval_t *frame1time = GetEdictFieldValue(ed, qcvm->extfields.frame1time);
			eval_t *frame2time = GetEdictFieldValue(ed, qcvm->extfields.frame2time);
			eval_t *alpha = GetEdictFieldValue(ed, qcvm->extfields.alpha);
			eval_t *renderflags = GetEdictFieldValue(ed, qcvm->extfields.renderflags);
			int rf = renderflags?renderflags->_float:0;

			VectorCopy(ed->v.origin, e->origin);
			VectorCopy(ed->v.angles, e->angles);
			e->model = model;
			e->skinnum = ed->v.skin;
			e->alpha = alpha?ENTALPHA_ENCODE(alpha->_float):ENTALPHA_DEFAULT;

			//can't exactly use currentpose/previous pose, as we don't know them.
			e->lerpflags = LERP_EXPLICIT|LERP_RESETANIM|LERP_RESETMOVE;
			e->frame = ed->v.frame;
			e->lerp.snap.frame2 = frame2?frame2->_float:0;
			e->lerp.snap.lerpfrac = lerpfrac?lerpfrac->_float:0;
			e->lerp.snap.lerpfrac = q_max(0.f, e->lerp.snap.lerpfrac);
			e->lerp.snap.lerpfrac = q_min(1.f, e->lerp.snap.lerpfrac);
			e->lerp.snap.time[0] = frame1time?frame1time->_float:0;
			e->lerp.snap.time[1] = frame2time?frame2time->_float:0;

			if (rf & RF_VIEWMODEL)
				e->eflags |= EFLAGS_VIEWMODEL;
			if (rf & RF_EXTERNALMODEL)
				e->eflags |= EFLAGS_EXTERIORMODEL;
			if (rf & RF_WEIRDFRAMETIMES)
			{
				e->lerp.snap.time[0] = qcvm->time - e->lerp.snap.time[0];
				e->lerp.snap.time[1] = qcvm->time - e->lerp.snap.time[1];
			}
		}
	}
}
static void PF_cs_addentity(void)
{
	PR_addentity_internal(G_EDICT(OFS_PARM0));
}
static void PF_m_addentity(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);

	eval_t *emodel = GetEdictFieldValue(ed, ED_FindFieldOffset("model"));
	const char *modelname = emodel?PR_GetString(emodel->_int):"";
	qmodel_t *model = *modelname?Mod_ForName(modelname, false):NULL;

	if (model)
	{
		entity_t *e = CL_NewTempEntity();
		if (e)
		{
			eval_t *origin = GetEdictFieldValue(ed, qcvm->extfields.origin);
			eval_t *angles = GetEdictFieldValue(ed, qcvm->extfields.angles);
			eval_t *frame = GetEdictFieldValue(ed, qcvm->extfields.frame);
			eval_t *frame2 = GetEdictFieldValue(ed, qcvm->extfields.frame2);
			eval_t *lerpfrac = GetEdictFieldValue(ed, qcvm->extfields.lerpfrac);
			eval_t *frame1time = GetEdictFieldValue(ed, qcvm->extfields.frame1time);
			eval_t *frame2time = GetEdictFieldValue(ed, qcvm->extfields.frame2time);
			eval_t *skin = GetEdictFieldValue(ed, qcvm->extfields.skin);
			eval_t *alpha = GetEdictFieldValue(ed, qcvm->extfields.alpha);

			if (origin)
				VectorCopy(origin->vector, e->origin);
			if (angles)
				VectorCopy(angles->vector, e->angles);
			e->model = model;
			e->skinnum = skin?skin->_float:0;
			e->alpha = alpha?ENTALPHA_ENCODE(alpha->_float):ENTALPHA_DEFAULT;

			//can't exactly use currentpose/previous pose, as we don't know them.
			e->lerpflags = LERP_EXPLICIT|LERP_RESETANIM|LERP_RESETMOVE;
			e->frame = frame?frame->_float:0;
			e->lerp.snap.frame2 = frame2?frame2->_float:0;
			e->lerp.snap.lerpfrac = lerpfrac?lerpfrac->_float:0;
			e->lerp.snap.time[0] = frame1time?frame1time->_float:0;
			e->lerp.snap.time[1] = frame2time?frame2time->_float:0;
		}
	}
}
enum
{
	MASK_ENGINE		= 1<<0,
	MASK_VIEWMODEL	= 1<<1,
};
static void PF_cs_addentities(void)
{
	int mask = G_FLOAT(OFS_PARM0);
	int i;
	size_t count;

	edict_t		*ed;
	entity_t	*ent;	//spike -- moved into here
	eval_t		*ev;

	if (!mask)
		return;	//o.O

	if ((mask & MASK_ENGINE) && qcvm->worldmodel)
	{
		CL_UpdateTEnts();
		for (i=1,ent=cl.entities+1 ; i<cl.num_entities ; i++,ent++)
		{
			if (!ent->model)
				continue;

			if (i == cl.viewentity && !chase_active.value)
				continue;

			if (cl_numvisedicts < cl_maxvisedicts)
			{
				cl_visedicts[cl_numvisedicts] = ent;
				cl_numvisedicts++;
			}
		}
	}

	if (qcvm->extfields.drawmask >= 0)
	{	//walk the csqc entities to add those to the scene (if their drawmask matches our arg)
		for (count = qcvm->num_edicts-1, ed = EDICT_NUM(1); count --> 0; ed = NEXT_EDICT(ed))
		{
			if (ed->free)
				continue;
			ev = GetEdictFieldValue(ed, qcvm->extfields.drawmask);
			if ((int)ev->_float & mask)
			{	//its a candidate!
				ev = GetEdictFieldValue(ed, qcvm->extfields.predraw);
				if (ev && ev->function)
				{
					pr_global_struct->self = EDICT_TO_PROG(ed);
					PR_ExecuteProgram(ev->function);
					if (ed->free || G_FLOAT(OFS_RETURN))
						continue;	//bummer...
				}
				PR_addentity_internal(ed);
			}
		}
	}

	if (mask & MASK_VIEWMODEL)
	{
		//default viewmodel.
	}


}
static void PF_cs_addlight(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	float radius = G_FLOAT(OFS_PARM1);
	float *rgb = G_VECTOR(OFS_PARM2);
//	float style = G_FLOAT(OFS_PARM3);
//	const char *cubename = G_STRING(OFS_PARM4);
//	float pflags = G_FLOAT(OFS_PARM5);

	int key = 0;
	dlight_t *dl = CL_AllocDlight (key);
	VectorCopy(rgb, dl->color);
	VectorCopy (org,  dl->origin);
	dl->radius = radius;
	dl->minlight = 32;
	dl->die = 0;
	dl->decay = 0;
}
static struct
{
	float rect_pos[2];
	float rect_size[2];
	float afov;
	float fov_x;
	float fov_y;
	vec3_t origin;
	vec3_t angles;

	qboolean drawsbar;
	qboolean drawcrosshair;
} viewprops;

void V_CalcRefdef (void);
static void PF_cl_clearscene(void)
{
	if (cl_numvisedicts + 64 > cl_maxvisedicts)
	{
		cl_maxvisedicts = cl_maxvisedicts+64;
		cl_visedicts = realloc(cl_visedicts, sizeof(*cl_visedicts)*cl_maxvisedicts);
	}
	cl_numvisedicts = 0;

	num_temp_entities = 0;

	memset(&viewprops, 0, sizeof(viewprops));
	viewprops.rect_size[0] = vid.width;
	viewprops.rect_size[1] = vid.height;

	if (qcvm == &cl.qcvm)
	{
		r_refdef.drawworld = true;
		V_CalcRefdef();
		VectorCopy(r_refdef.vieworg, viewprops.origin);
		VectorCopy(r_refdef.viewangles, viewprops.angles);
	}
	else	//menuqc has different defaults, with the assumption that the world will never be drawn etc.
		r_refdef.drawworld = false;
}

enum
{
//the basic range.
	VF_MIN					= 1,
	VF_MIN_X				= 2,
	VF_MIN_Y				= 3,
	VF_SIZE					= 4,
	VF_SIZE_X				= 5,
	VF_SIZE_Y				= 6,
	VF_VIEWPORT				= 7,
	VF_FOV					= 8,
	VF_FOV_X				= 9,
	VF_FOV_Y				= 10,
	VF_ORIGIN				= 11,
	VF_ORIGIN_X				= 12,
	VF_ORIGIN_Y				= 13,
	VF_ORIGIN_Z				= 14,
	VF_ANGLES				= 15,
	VF_ANGLES_X				= 16,
	VF_ANGLES_Y				= 17,
	VF_ANGLES_Z				= 18,
	VF_DRAWWORLD			= 19,
	VF_DRAWENGINESBAR		= 20,
	VF_DRAWCROSSHAIR		= 21,
//	VF_						= 22,
	VF_MINDIST				= 23,
	VF_MAXDIST				= 24,
//	VF_						= 25,
//	VF_						= 26,
//	VF_						= 27,
//	VF_						= 28,
//	VF_						= 29,
//	VF_						= 30,
//	VF_						= 31,
//	VF_						= 32,

//the weird DP hacks range.
	VF_CL_VIEWANGLES		= 33,
	VF_CL_VIEWANGLES_X		= 34,
	VF_CL_VIEWANGLES_Y		= 35,
	VF_CL_VIEWANGLES_Z		= 36,

//the FTE range.
	//VF_PERSPECTIVE		= 200,
	//VF_DP_CLEARSCREEN		= 201,
	VF_ACTIVESEAT			= 202,
	VF_AFOV					= 203,
	VF_SCREENVSIZE			= 204,
	VF_SCREENPSIZE			= 205,
	//VF_VIEWENTITY			= 206,
	//VF_STATSENTITY		= 207,	//the player number for the stats.
	//VF_SCREENVOFFSET		= 208,
	//VF_RT_SOURCECOLOUR	= 209,
	//VF_RT_DEPTH			= 210,
	//VF_RT_RIPPLE			= 211,	/**/
	//VF_RT_DESTCOLOUR0		= 212,
	//VF_RT_DESTCOLOUR1		= 213,
	//VF_RT_DESTCOLOUR2		= 214,
	//VF_RT_DESTCOLOUR3		= 215,
	//VF_RT_DESTCOLOUR4		= 216,
	//VF_RT_DESTCOLOUR5		= 217,
	//VF_RT_DESTCOLOUR6		= 218,
	//VF_RT_DESTCOLOUR7		= 219,
	//VF_ENVMAP				= 220,	//cubemap image for reflectcube
	//VF_USERDATA			= 221,	//data for glsl
	//VF_SKYROOM_CAMERA		= 222,	//vec origin
	//VF_PIXELPSCALE		= 223,	//[dpi_x, dpi_y, dpi_y/dpi_x]
	//VF_PROJECTIONOFFSET	= 224,	//allows for off-axis projections.

//DP's range
	//VF_DP_MAINVIEW		= 400, // defective. should be a viewid instead, allowing for per-view motionblur instead of disabling it outright
	//VF_DP_MINFPS_QUALITY	= 401, //multiplier for lod and culling to try to reduce costs.
};
static void PF_cl_getproperty(void)
{
	float s;
	int prop = G_FLOAT(OFS_PARM0);
	G_FLOAT(OFS_RETURN+0) =
	G_FLOAT(OFS_RETURN+1) =
	G_FLOAT(OFS_RETURN+2) = 0;
	switch(prop)
	{	//NOTE: These indexes are shared between engines. whoever thought THAT would be a good idea...
	case VF_MIN:		//min
		G_FLOAT(OFS_RETURN+0) = viewprops.rect_pos[0];
		G_FLOAT(OFS_RETURN+1) = viewprops.rect_pos[1];
		break;
	case VF_MIN_X:
		G_FLOAT(OFS_RETURN+0) = viewprops.rect_pos[0];
		break;
	case VF_MIN_Y:
		G_FLOAT(OFS_RETURN+0) = viewprops.rect_pos[1];
		break;
	case VF_SIZE:		//size
		G_FLOAT(OFS_RETURN+0) = viewprops.rect_size[0];
		G_FLOAT(OFS_RETURN+1) = viewprops.rect_size[1];
		break;
	case VF_SIZE_X:
		G_FLOAT(OFS_RETURN+0) = viewprops.rect_size[0];
		break;
	case VF_SIZE_Y:
		G_FLOAT(OFS_RETURN+0) = viewprops.rect_size[1];
		break;
	//case VF_VIEWPORT:	//viewport. doesn't make sense for a single vector return.
	case VF_FOV:
		G_FLOAT(OFS_RETURN+0) = viewprops.fov_x;
		G_FLOAT(OFS_RETURN+1) = viewprops.fov_y;
		break;
	case VF_FOV_X:
		G_FLOAT(OFS_RETURN+0) = viewprops.fov_x;
		break;
	case VF_FOV_Y:
		G_FLOAT(OFS_RETURN+0) = viewprops.fov_y;
		break;
	case VF_ORIGIN:	//vieworigin
		if (qcvm->nogameaccess)
			goto accessblocked;
		VectorCopy(viewprops.origin, G_VECTOR(OFS_PARM1));
		break;
	case VF_ORIGIN_X:
	case VF_ORIGIN_Y:
	case VF_ORIGIN_Z:
		if (qcvm->nogameaccess)
			goto accessblocked;
		G_FLOAT(OFS_RETURN+0) = viewprops.origin[prop-VF_ORIGIN_X];
		break;
	case VF_ANGLES:	//viewangles
		if (qcvm->nogameaccess)
			goto accessblocked;
		VectorCopy(viewprops.angles, G_VECTOR(OFS_PARM1));
		break;
	case VF_ANGLES_X:
	case VF_ANGLES_Y:
	case VF_ANGLES_Z:
		if (qcvm->nogameaccess)
			goto accessblocked;
		G_FLOAT(OFS_RETURN+0) = viewprops.angles[prop-VF_ANGLES_X];
		break;
	case VF_DRAWWORLD: //drawworld
		G_FLOAT(OFS_RETURN+0) = r_refdef.drawworld;
		break;
	case VF_DRAWENGINESBAR: //engine sbar
		G_FLOAT(OFS_RETURN+0) = viewprops.drawsbar;
		break;
	case VF_DRAWCROSSHAIR: //crosshair
		G_FLOAT(OFS_RETURN+0) = viewprops.drawcrosshair;
		break;
	case VF_MINDIST:
		#define NEARCLIP 4
		G_FLOAT(OFS_RETURN+0) = NEARCLIP;
		break;
	case VF_MAXDIST: //maxdist
		{
			extern cvar_t gl_farclip;
			G_FLOAT(OFS_RETURN+0) = gl_farclip.value;
		}
		break;

	case VF_CL_VIEWANGLES:	//viewangles hack
		if (qcvm->nogameaccess)
			goto accessblocked;
		VectorCopy(cl.viewangles, G_VECTOR(OFS_RETURN));
		break;
	case VF_CL_VIEWANGLES_X:
	case VF_CL_VIEWANGLES_Y:
	case VF_CL_VIEWANGLES_Z:
		if (qcvm->nogameaccess)
			goto accessblocked;
		G_FLOAT(OFS_RETURN+0) = cl.viewangles[prop-VF_CL_VIEWANGLES_X];
		break;

	//fte's range
//	case VF_PERSPECTIVE:	//useperspective (0 for isometric)
//	case VF_DP_CLEARSCREEN:
	case VF_ACTIVESEAT:		//active seat
		G_FLOAT(OFS_RETURN+0) = 0;
		break;
	case VF_AFOV:	//aproximate fov
		G_FLOAT(OFS_RETURN+0) = viewprops.afov;
		break;
	case VF_SCREENVSIZE:	//virtual size (readonly)
		s = PR_GetVMScale();
		G_FLOAT(OFS_RETURN+0) = glwidth/s;
		G_FLOAT(OFS_RETURN+1) = glheight/s;
		break;
	case VF_SCREENPSIZE:	//physical size (readonly)
		G_FLOAT(OFS_RETURN+0) = glwidth;
		G_FLOAT(OFS_RETURN+1) = glheight;
		break;
//	case VF_VIEWENTITY:	//viewentity
//	case VF_STATSENTITY:	//stats entity
//	case VF_SCREENVOFFSET:
//	case VF_RT_SOURCECOLOUR:	//sourcecolour
//	case VF_RT_DEPTH:	//depth
//	case VF_RT_RIPPLE:	//ripplemap
//	case VF_RT_DESTCOLOUR0:	//destcolour0
//	case VF_RT_DESTCOLOUR1:	//destcolour1
//	case VF_RT_DESTCOLOUR2:	//destcolour2
//	case VF_RT_DESTCOLOUR3:	//destcolour3
//	case VF_RT_DESTCOLOUR4:	//destcolour4
//	case VF_RT_DESTCOLOUR5:	//destcolour5
//	case VF_RT_DESTCOLOUR6:	//destcolour6
//	case VF_RT_DESTCOLOUR7:	//destcolour7
//	case VF_ENVMAP:	//envmap
//	case VF_USERDATA:	//userdata (glsl uniform vec4 array)
//	case VF_SKYROOM_CAMERA:	//skyroom camera
//	case VF_PIXELPSCALE:	//dots-per-inch x,y
//	case VF_PROJECTIONOFFSET:	//projection offset

	//dp's range
//	case VF_DP_MAINVIEW: //mainview
//	case VF_DP_MINFPS_QUALITY: //lodquality
	default:
		Con_Printf("PF_getproperty(%i): unsupported property\n", prop);
		break;
	accessblocked:
		Con_Printf("PF_getproperty(%i): not allowed to access game properties\n", prop);
		break;
	}
}
static void PF_cl_setproperty(void)
{
	int prop = G_FLOAT(OFS_PARM0);
	switch(prop)
	{	//NOTE: These indexes are shared between engines. whoever thought THAT would be a good idea...
	case VF_MIN:		//min
		viewprops.rect_pos[0] = G_FLOAT(OFS_PARM1+0);
		viewprops.rect_pos[1] = G_FLOAT(OFS_PARM1+1);
		break;
	case VF_MIN_X:
		viewprops.rect_pos[0] = G_FLOAT(OFS_PARM1);
		break;
	case VF_MIN_Y:
		viewprops.rect_pos[1] = G_FLOAT(OFS_PARM1);
		break;
	case VF_SIZE:		//size
		viewprops.rect_size[0] = G_FLOAT(OFS_PARM1+0);
		viewprops.rect_size[1] = G_FLOAT(OFS_PARM1+1);
		break;
	case VF_SIZE_X:
		viewprops.rect_size[0] = G_FLOAT(OFS_PARM1);
		break;
	case VF_SIZE_Y:
		viewprops.rect_size[1] = G_FLOAT(OFS_PARM1);
		break;
	case VF_VIEWPORT:
		viewprops.rect_pos[0] = G_FLOAT(OFS_PARM1+0);
		viewprops.rect_pos[1] = G_FLOAT(OFS_PARM1+1);
		viewprops.rect_size[0] = G_FLOAT(OFS_PARM2+0);
		viewprops.rect_size[1] = G_FLOAT(OFS_PARM2+1);
		break;
	case VF_FOV:
		viewprops.fov_x = G_FLOAT(OFS_PARM1+0);
		viewprops.fov_y = G_FLOAT(OFS_PARM1+1);
		break;
	case VF_FOV_X:
		viewprops.fov_x = G_FLOAT(OFS_PARM1);
		break;
	case VF_FOV_Y:
		viewprops.fov_y = G_FLOAT(OFS_PARM1);
		break;
	case VF_ORIGIN:	//vieworigin
		VectorCopy(G_VECTOR(OFS_PARM1), viewprops.origin);
		break;
	case VF_ORIGIN_X:
	case VF_ORIGIN_Y:
	case VF_ORIGIN_Z:
		viewprops.origin[prop-VF_ORIGIN_X] = G_FLOAT(OFS_PARM1);
		break;
	case VF_ANGLES:	//viewangles
		VectorCopy(G_VECTOR(OFS_PARM1), viewprops.angles);
		break;
	case VF_ANGLES_X:
	case VF_ANGLES_Y:
	case VF_ANGLES_Z:
		viewprops.angles[prop-VF_ANGLES_X] = G_FLOAT(OFS_PARM1);
		break;
	case VF_DRAWWORLD: //drawworld
		r_refdef.drawworld = G_FLOAT(OFS_PARM1);
		break;
	case VF_DRAWENGINESBAR: //engine sbar
		viewprops.drawsbar = G_FLOAT(OFS_PARM1);
		break;
	case VF_DRAWCROSSHAIR: //crosshair
		viewprops.drawcrosshair = G_FLOAT(OFS_PARM1);
		break;
//	case VF_MINDIST: //mindist
//	case VF_MAXDIST: //maxdist

	case VF_CL_VIEWANGLES:	//viewangles hack
		VectorCopy(G_VECTOR(OFS_PARM1), cl.viewangles);
		break;
	case VF_CL_VIEWANGLES_X:
	case VF_CL_VIEWANGLES_Y:
	case VF_CL_VIEWANGLES_Z:
		cl.viewangles[prop-VF_CL_VIEWANGLES_X] = G_FLOAT(OFS_PARM1);
		break;

	//fte's range
//	case VF_PERSPECTIVE:	//useperspective (0 for isometric)
//	case VF_DP_CLEARSCREEN:	//active seat
	case VF_ACTIVESEAT:		//active seat
		if (G_FLOAT(OFS_PARM1))
			Con_Printf("VF_ACTIVESEAT: splitscreen not supported\n");
		break;
	case VF_AFOV:	//aproximate fov
		viewprops.afov = G_FLOAT(OFS_PARM1);
		break;
//	case VF_SCREENVSIZE:	//virtual size (readonly)
//	case VF_SCREENPSIZE:	//physical size (readonly)
//	case VF_VIEWENTITY:	//viewentity
//	case VF_STATSENTITY:	//stats entity
//	case VF_SCREENVOFFSET:
//	case VF_RT_SOURCECOLOUR:	//sourcecolour
//	case VF_RT_DEPTH:	//depth
//	case VF_RT_RIPPLE:	//ripplemap
//	case VF_RT_DESTCOLOUR0:	//destcolour0
//	case VF_RT_DESTCOLOUR1:	//destcolour1
//	case VF_RT_DESTCOLOUR2:	//destcolour2
//	case VF_RT_DESTCOLOUR3:	//destcolour3
//	case VF_RT_DESTCOLOUR4:	//destcolour4
//	case VF_RT_DESTCOLOUR5:	//destcolour5
//	case VF_RT_DESTCOLOUR6:	//destcolour6
//	case VF_RT_DESTCOLOUR7:	//destcolour7
//	case VF_ENVMAP:	//envmap
//	case VF_USERDATA:	//userdata (glsl uniform vec4 array)
//	case VF_SKYROOM_CAMERA:	//skyroom camera
//	case VF_PIXELPSCALE:	//dots-per-inch x,y
//	case VF_PROJECTIONOFFSET:	//projection offset

	//dp's range
//	case VF_DP_MAINVIEW: //mainview
//	case VF_DP_MINFPS_QUALITY: //lodquality
	default:
		Con_Printf("PF_m_setproperty: unsupported property %i\n", prop);
		break;
	}
}

void V_PolyBlend (void);
void SCR_DrawCrosshair (void);
float CalcFovy (float fov_x, float width, float height);
extern cvar_t scr_fov;
static void PF_cl_computerefdef(void)
{
	float s = PR_GetVMScale();

	VectorCopy(viewprops.origin, r_refdef.vieworg);
	VectorCopy(viewprops.angles, r_refdef.viewangles);
	r_refdef.vrect.x = viewprops.rect_pos[0]*s;
	r_refdef.vrect.y = viewprops.rect_pos[1]*s;
	r_refdef.vrect.width = viewprops.rect_size[0]*s;
	r_refdef.vrect.height = viewprops.rect_size[1]*s;

	if (viewprops.fov_x && viewprops.fov_y)
	{
		r_refdef.fov_x = viewprops.fov_x;
		r_refdef.fov_y = viewprops.fov_y;
	}
	else
	{
		if (!viewprops.afov)
			viewprops.afov = scr_fov.value;
		if (r_refdef.vrect.width/r_refdef.vrect.height < 4/3)
		{
			r_refdef.fov_y = viewprops.afov;
			r_refdef.fov_x = CalcFovy(r_refdef.fov_y, r_refdef.vrect.height, r_refdef.vrect.width);
		}
		else
		{
			r_refdef.fov_y = tan(viewprops.afov * M_PI / 360.0) * (3.0 / 4.0);
			r_refdef.fov_x = r_refdef.fov_y * r_refdef.vrect.width / r_refdef.vrect.height;
			r_refdef.fov_x = atan(r_refdef.fov_x) * (360.0 / M_PI);
			r_refdef.fov_y = atan(r_refdef.fov_y) * (360.0 / M_PI);
		}
	}
}
static void PF_cl_renderscene(void)
{
	PF_cl_computerefdef();
	if (r_refdef.vrect.width < 1 || r_refdef.vrect.height < 1)
		return;	//can't draw nuffin...

	R_RenderView();
	if (r_refdef.drawworld)
		V_PolyBlend ();

	vid.recalc_refdef = true;
	GL_Set2D();

	if (!cl.intermission && viewprops.drawsbar)
		Sbar_Draw ();
	if (!cl.intermission && viewprops.drawcrosshair)
		SCR_DrawCrosshair ();

	if (qcvm == &cls.menu_qcvm)
		GL_SetCanvas (CANVAS_MENUQC);
	else
		GL_SetCanvas (CANVAS_CSQC);
	glEnable (GL_BLEND);
	glDisable (GL_ALPHA_TEST);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

static void PF_cl_genProjectMatricies(mat4_t viewproj)
{
	mat4_t view;
	mat4_t proj;
	extern cvar_t gl_farclip;

	//note: we ignore r_waterwarp here (we don't really know if we're underwater or not here), and you're unlikely to want such warping for mouse clicks.
	Matrix4_ProjectionMatrix(r_refdef.fov_x, r_refdef.fov_y, NEARCLIP, gl_farclip.value, false, 0, 0, proj);
	Matrix4_ViewMatrix(r_refdef.viewangles, r_refdef.vieworg, view);

	//combine
	Matrix4_Multiply(proj, view, viewproj);
}

//vector(vector v) unproject
//Transform a 2d screen-space point (with depth) into a 3d world-space point, according the various origin+angle+fov etc settings set via setproperty.
static void PF_cl_unproject(void)
{
	mat4_t viewproj, viewproj_inv;
	vec4_t pos, res;
	float s = PR_GetVMScale();
	VectorCopy(G_VECTOR(OFS_PARM0), pos);

	PF_cl_computerefdef();	//determine correct vrect (in pixels), and compute fovxy accordingly. this could be cached...
	PF_cl_genProjectMatricies(viewproj);
	Matrix4_Invert(viewproj, viewproj_inv);

	//convert virtual coords to (pixel) screenspace
	pos[0] *= s;
	pos[1] *= s;

	//convert to viewport (0-1 within vrect)...
	pos[0] = (pos[0]-r_refdef.vrect.x)/r_refdef.vrect.width;
	pos[1] = (pos[1]-r_refdef.vrect.y)/r_refdef.vrect.height;
	pos[1] = 1-pos[1];	//matricies for opengl are bottom-up.

	//convert to clipspace (+/- 1)
	pos[0] = pos[0]*2-1;
	pos[1] = pos[1]*2-1;
	pos[2] = pos[2]*2-1;	//input_z==0 means near clip plane.

	//don't allow 1 here as that causes issues with infinite far clip plane maths, and some DP mods abuse it with depth values much larger than 1.
	if (pos[2] >= 1)
		pos[2] = 0.999999;
	//w coord is 1... yay 4*4 matricies.
	pos[3] = 1;

	//transform our point...
	Matrix4_Transform4(viewproj_inv, pos, res);

	//result is then res_xyz/res_w. we don't really care for interpolating texture projection...
	VectorScale(res, 1/res[3], G_VECTOR(OFS_RETURN));
}
//vector(vector v) project
//Transform a 3d world-space point into a 2d screen-space point, according the various origin+angle+fov etc settings set via setproperty.
static void PF_cl_project(void)
{
	mat_t viewproj[16];
	vec4_t pos, res;
	float s = PR_GetVMScale();
	VectorCopy(G_VECTOR(OFS_PARM0), pos);
	pos[3] = 1;	//w is always 1.

	PF_cl_computerefdef();	//determine correct vrect (in pixels), and compute fovxy accordingly. this could be cached...
	PF_cl_genProjectMatricies(viewproj);

	Matrix4_Transform4(viewproj, pos, res);
	VectorScale(res, 1/res[3], res);
	if (res[3] < 0)
		res[2] *= -1;	//oops... keep things behind us behind... this allows it to act as an error condition.

	//clipspace to viewport
	res[0] = (1+res[0])/2;
	res[1] = (1+res[1])/2;
	res[1] = 1-res[1]; //matricies for opengl are bottom-up.

	//viewport to screenspace (0-1 within vrect)...
	res[0] = r_refdef.vrect.x + r_refdef.vrect.width *res[0];
	res[1] = r_refdef.vrect.y + r_refdef.vrect.height*res[1];

	//screenspace to virtual
	res[0] /= s;
	res[1] /= s;

	VectorCopy(res, G_VECTOR(OFS_RETURN));
}


static void PF_cs_setlistener(void)
{
	float *origin = G_VECTOR(OFS_PARM0);
	float *forward = G_VECTOR(OFS_PARM1);
	float *right = G_VECTOR(OFS_PARM2);
	float *up = G_VECTOR(OFS_PARM3);
//	int reverbtype = (qcvm->argc <= 4)?0:G_FLOAT(OFS_PARM4);

	cl.listener_defined = true;	//lasts until the next video frame.
	VectorCopy(origin, cl.listener_origin);
	VectorCopy(forward, cl.listener_axis[0]);
	VectorCopy(right, cl.listener_axis[1]);
	VectorCopy(up, cl.listener_axis[2]);
}

void CL_CSQC_SetInputs(usercmd_t *cmd, qboolean set);
static void PF_cs_getinputstate(void)
{
	unsigned int seq = G_FLOAT(OFS_PARM0);
	if (seq == cl.movemessages)
	{	//the partial/pending frame!
		G_FLOAT(OFS_RETURN) = 1;
		CL_CSQC_SetInputs(&cl.pendingcmd, true);
	}
	else if (cl.movecmds[seq&MOVECMDS_MASK].sequence == seq)
	{	//valid sequence slot.
		G_FLOAT(OFS_RETURN) = 1;
		CL_CSQC_SetInputs(&cl.movecmds[seq&MOVECMDS_MASK], true);
	}
	else
		G_FLOAT(OFS_RETURN) = 0; //invalid
}

static void PF_touchtriggers (void)
{
	edict_t	*e;
	float	*org;

	e = (qcvm->argc > 0)?G_EDICT(OFS_PARM0):G_EDICT(pr_global_struct->self);
	if (qcvm->argc > 1)
	{
		org = G_VECTOR(OFS_PARM1);
		VectorCopy (org, e->v.origin);
	}
	SV_LinkEdict (e, true);
}

static void PF_checkpvs (void)
{
	float *org = G_VECTOR(OFS_PARM0);
	edict_t *ed = G_EDICT(OFS_PARM1);

	mleaf_t *leaf = Mod_PointInLeaf (org, qcvm->worldmodel);
	byte *pvs = Mod_LeafPVS (leaf, qcvm->worldmodel); //johnfitz -- worldmodel as a parameter
	int i;

	for (i=0 ; i < ed->num_leafs ; i++)
	{
		if (pvs[ed->leafnums[i] >> 3] & (1 << (ed->leafnums[i]&7) ))
		{
			G_FLOAT(OFS_RETURN) = true;
			return;
		}
	}

	G_FLOAT(OFS_RETURN) = false;
}

enum getrenderentityfield_e
{
	GE_MAXENTS			= -1,
	GE_ACTIVE			= 0,
	GE_ORIGIN			= 1,
	GE_FORWARD			= 2,
	GE_RIGHT			= 3,
	GE_UP				= 4,
	GE_SCALE			= 5,
	GE_ORIGINANDVECTORS	= 6,
	GE_ALPHA			= 7,
	GE_COLORMOD			= 8,
	GE_PANTSCOLOR		= 9,
	GE_SHIRTCOLOR		= 10,
	GE_SKIN				= 11,
//	GE_MINS				= 12,
//	GE_MAXS				= 13,
//	GE_ABSMIN			= 14,
//	GE_ABSMAX			= 15,
//	GE_LIGHT			= 16,
};
static void PF_cl_getrenderentity(void)
{
	extern int	Sbar_ColorForMap (int m);
	vec3_t tmp;
	size_t entnum = G_FLOAT(OFS_PARM0);
	enum getrenderentityfield_e fldnum = G_FLOAT(OFS_PARM1);
	if (qcvm->nogameaccess)
	{
		G_FLOAT(OFS_RETURN+0) = 0;
		G_FLOAT(OFS_RETURN+1) = 0;
		G_FLOAT(OFS_RETURN+2) = 0;
		Con_Printf("PF_getrenderentity: not permitted\n");
		return;
	}
	if (fldnum == GE_MAXENTS)
		G_FLOAT(OFS_RETURN+0) = cl.num_entities;
	else if (entnum >= cl.num_entities || !cl.entities[entnum].model)
	{
		G_FLOAT(OFS_RETURN+0) = 0;
		G_FLOAT(OFS_RETURN+1) = 0;
		G_FLOAT(OFS_RETURN+2) = 0;
	}
	else switch(fldnum)
	{
	case GE_ACTIVE:
		G_FLOAT(OFS_RETURN+0) = true;
		break;
	case GE_ORIGIN:
		VectorCopy(cl.entities[entnum].origin, G_VECTOR(OFS_RETURN));
		break;
	case GE_ORIGINANDVECTORS:
		VectorCopy(cl.entities[entnum].origin, G_VECTOR(OFS_RETURN));
		VectorCopy(cl.entities[entnum].angles, tmp);
		if (cl.entities[entnum].model->type == mod_alias)
			tmp[0] *= -1;
		AngleVectors(tmp, pr_global_struct->v_forward, pr_global_struct->v_right, pr_global_struct->v_up);
		break;
	case GE_FORWARD:
		VectorCopy(cl.entities[entnum].angles, tmp);
		if (cl.entities[entnum].model->type == mod_alias)
			tmp[0] *= -1;
		AngleVectors(tmp, G_VECTOR(OFS_RETURN), tmp, tmp);
		break;
	case GE_RIGHT:
		VectorCopy(cl.entities[entnum].angles, tmp);
		if (cl.entities[entnum].model->type == mod_alias)
			tmp[0] *= -1;
		AngleVectors(tmp, tmp, G_VECTOR(OFS_RETURN), tmp);
		break;
	case GE_UP:
		VectorCopy(cl.entities[entnum].angles, tmp);
		if (cl.entities[entnum].model->type == mod_alias)
			tmp[0] *= -1;
		AngleVectors(tmp, tmp, tmp, G_VECTOR(OFS_RETURN));
		break;
	case GE_SCALE:
		G_FLOAT(OFS_RETURN+0) = cl.entities[entnum].netstate.scale/16.0;
		break;
	case GE_ALPHA:
		G_FLOAT(OFS_RETURN+0) = ENTALPHA_DECODE(cl.entities[entnum].alpha);
		break;
	case GE_COLORMOD:
		G_FLOAT(OFS_RETURN+0) = cl.entities[entnum].netstate.colormod[0] / 32.0;
		G_FLOAT(OFS_RETURN+1) = cl.entities[entnum].netstate.colormod[1] / 32.0;
		G_FLOAT(OFS_RETURN+2) = cl.entities[entnum].netstate.colormod[2] / 32.0;
		break;
	case GE_PANTSCOLOR:
		{
			int palidx = cl.entities[entnum].netstate.colormap;
			byte *pal;
			if (!(cl.entities[entnum].netstate.eflags & EFLAGS_COLOURMAPPED))
				palidx = cl.scores[palidx].colors;
			pal = (byte *)d_8to24table + 4*Sbar_ColorForMap(palidx&0x0f);
			G_FLOAT(OFS_RETURN+0) = pal[0] / 255.0;
			G_FLOAT(OFS_RETURN+1) = pal[1] / 255.0;
			G_FLOAT(OFS_RETURN+2) = pal[2] / 255.0;
		}
		break;
	case GE_SHIRTCOLOR:
		{
			int palidx = cl.entities[entnum].netstate.colormap;
			byte *pal;
			if (!(cl.entities[entnum].netstate.eflags & EFLAGS_COLOURMAPPED))
				palidx = cl.scores[palidx].colors;
			pal = (byte *)d_8to24table + 4*Sbar_ColorForMap(palidx&0xf0);
			G_FLOAT(OFS_RETURN+0) = pal[0] / 255.0;
			G_FLOAT(OFS_RETURN+1) = pal[1] / 255.0;
			G_FLOAT(OFS_RETURN+2) = pal[2] / 255.0;
		}
		break;
	case GE_SKIN:
		G_FLOAT(OFS_RETURN+0) = cl.entities[entnum].skinnum;
		break;
//	case GE_MINS:
//	case GE_MAXS:
//	case GE_ABSMIN:
//	case GE_ABSMAX:
//	case GE_LIGHT:
	case GE_MAXENTS:
//	default:
		Con_Printf("PF_cl_getrenderentity(,%i): not implemented\n", fldnum);
		break;
	}
}

//A quick note on number ranges.
//0: automatically assigned. more complicated, but no conflicts over numbers, just names...
//   NOTE: #0 is potentially ambiguous - vanilla will interpret it as instruction 0 (which is normally reserved) rather than a builtin.
//         if such functions were actually used, this would cause any 64bit engines that switched to unsigned types to crash due to an underflow.
//         we do some sneaky hacks to avoid changes to the vm... because we're evil.
//0-199: free for all.
//200-299: fte's random crap
//300-399: csqc's random crap
//400+: dp's random crap
static struct
{
	const char *name;
	builtin_t ssqcfunc;
	builtin_t csqcfunc;
	int documentednumber;
	builtin_t menufunc;
	int documentednumber_menuqc;
	const char *typestr;
	const char *desc;
	int number;
	int number_menuqc;
} extensionbuiltins[] =
#define PF_NoSSQC NULL
#define PF_NoCSQC NULL
#define PF_FullCSQCOnly NULL
#define PF_NoMenu NULL,0
{
	{"setmodel",		PF_NoSSQC,			PF_NoCSQC,			3,	PF_m_setmodel,		90, "void(entity ent, string modelname)", ""},
	{"precache_model",	PF_NoSSQC,			PF_NoCSQC,			20,	PF_m_precache_model,91, "string(string modelname)", ""},

	{"vectoangles2",	PF_ext_vectoangles,	PF_ext_vectoangles,	51,	PF_NoMenu, D("vector(vector fwd, optional vector up)", "Returns the angles (+x=UP) required to orient an entity to look in the given direction. The 'up' argument is required if you wish to set a roll angle, otherwise it will be limited to just monster-style turning.")},
	{"sin",				PF_Sin,				PF_Sin,				60,	PF_Sin,38, "float(float angle)"},	//60
	{"cos",				PF_Cos,				PF_Cos,				61,	PF_Cos,39, "float(float angle)"},	//61
	{"sqrt",			PF_Sqrt,			PF_Sqrt,			62,	PF_Sqrt,40, "float(float value)"},	//62
	{"tracetoss",		PF_TraceToss,		PF_TraceToss,		64,	PF_NoMenu, "void(entity ent, entity ignore)"},
	{"etos",			PF_etos,			PF_etos,			65,	PF_NoMenu, "string(entity ent)"},
	{"etof",			PF_num_for_edict,	PF_num_for_edict,	0, PF_num_for_edict,79, "float(entity ent)"},
	{"ftoe",			PF_edict_for_num,	PF_edict_for_num,	0, PF_edict_for_num,80, "entity(float ent)"},
	{"infokey",			PF_infokey_s,		PF_NoCSQC,			80,	PF_NoMenu, D("string(entity e, string key)", "If e is world, returns the field 'key' from either the serverinfo or the localinfo. If e is a player, returns the value of 'key' from the player's userinfo string. There are a few special exceptions, like 'ip' which is not technically part of the userinfo.")},	//80
	{"infokeyf",		PF_infokey_f,		PF_NoCSQC,			0,	PF_NoMenu, D("float(entity e, string key)", "Identical to regular infokey, but returns it as a float instead of creating new tempstrings.")},	//80
	{"stof",			PF_stof,			PF_stof,			81,	PF_stof,21, "float(string)"},	//81
	{"multicast",		PF_multicast,		PF_NoCSQC,			82,	PF_NoMenu, D("#define unicast(pl,reli) do{msg_entity = pl; multicast('0 0 0', reli?MULITCAST_ONE_R:MULTICAST_ONE);}while(0)\n"
																				 "void(vector where, float set)", "Once the MSG_MULTICAST network message buffer has been filled with data, this builtin is used to dispatch it to the given target, filtering by pvs for reduced network bandwidth.")},	//82
	{"tracebox",		PF_tracebox,		PF_tracebox,		90,	PF_NoMenu, D("void(vector start, vector mins, vector maxs, vector end, float nomonsters, entity ent)", "Exactly like traceline, but a box instead of a uselessly thin point. Acceptable sizes are limited by bsp format, q1bsp has strict acceptable size values.")},
	{"randomvec",		PF_randomvector,	PF_randomvector,	91,	PF_NoMenu, D("vector()", "Returns a vector with random values. Each axis is independantly a value between -1 and 1 inclusive.")},
	{"getlight",		PF_sv_getlight,		PF_cl_getlight,		92, PF_NoMenu, "vector(vector org)"},// (DP_QC_GETLIGHT),
	{"registercvar",	PF_registercvar,	PF_registercvar,	93,	PF_NoMenu, D("float(string cvarname, string defaultvalue)", "Creates a new cvar on the fly. If it does not already exist, it will be given the specified value. If it does exist, this is a no-op.\nThis builtin has the limitation that it does not apply to configs or commandlines. Such configs will need to use the set or seta command causing this builtin to be a noop.\nIn engines that support it, you will generally find the autocvar feature easier and more efficient to use.")},
	{"min",				PF_min,				PF_min,				94,	PF_min,43, D("float(float a, float b, ...)", "Returns the lowest value of its arguments.")},// (DP_QC_MINMAXBOUND)
	{"max",				PF_max,				PF_max,				95,	PF_max,44, D("float(float a, float b, ...)", "Returns the highest value of its arguments.")},// (DP_QC_MINMAXBOUND)
	{"bound",			PF_bound,			PF_bound,			96,	PF_bound,45, D("float(float minimum, float val, float maximum)", "Returns val, unless minimum is higher, or maximum is less.")},// (DP_QC_MINMAXBOUND)
	{"pow",				PF_pow,				PF_pow,				97,	PF_pow,46, "float(float value, float exp)"},
	{"findfloat",		PF_findfloat,		PF_findfloat,		98, PF_NoMenu, D("#define findentity findfloat\nentity(entity start, .__variant fld, __variant match)", "Equivelent to the find builtin, but instead of comparing strings contents, this builtin compares the raw values. This builtin requires multiple calls in order to scan all entities - set start to the previous call's return value.\nworld is returned when there are no more entities.")},	// #98 (DP_QC_FINDFLOAT)
	{"checkextension",	PF_checkextension,	PF_checkextension,	99,	PF_checkextension,1, D("float(string extname)", "Checks for an extension by its name (eg: checkextension(\"FRIK_FILE\") says that its okay to go ahead and use strcat).\nUse cvar(\"pr_checkextension\") to see if this builtin exists.")},	// #99	//darkplaces system - query a string to see if the mod supports X Y and Z.
	{"checkbuiltin",	PF_checkbuiltin,	PF_checkbuiltin,	0,	PF_checkbuiltin,0, D("float(__variant funcref)", "Checks to see if the specified builtin is supported/mapped. This is intended as a way to check for #0 functions, allowing for simple single-builtin functions.")},
	{"builtin_find",	PF_builtinsupported,PF_builtinsupported,100,PF_NoMenu, D("float(string builtinname)", "Looks to see if the named builtin is valid, and returns the builtin number it exists at.")},	// #100	//per builtin system.
	{"anglemod",		PF_anglemod,		PF_anglemod,		102,PF_NoMenu, "float(float value)"},	//telejano

	{"fopen",			PF_fopen,			PF_fopen,				110, PF_fopen,48, D("filestream(string filename, float mode, optional float mmapminsize)", "Opens a file, typically prefixed with \"data/\", for either read or write access.")},	// (FRIK_FILE)
	{"fclose",			PF_fclose,			PF_fclose,				111, PF_fclose,49, "void(filestream fhandle)"},	// (FRIK_FILE)
	{"fgets",			PF_fgets,			PF_fgets,				112, PF_fgets,50, D("string(filestream fhandle)", "Reads a single line out of the file. The new line character is not returned as part of the string. Returns the null string on EOF (use if not(string) to easily test for this, which distinguishes it from the empty string which is returned if the line being read is blank")},	// (FRIK_FILE)
	{"fputs",			PF_fputs,			PF_fputs,				113, PF_fputs,51, D("void(filestream fhandle, string s, optional string s2, optional string s3, optional string s4, optional string s5, optional string s6, optional string s7)", "Writes the given string(s) into the file. For compatibility with fgets, you should ensure that the string is terminated with a \\n - this will not otherwise be done for you. It is up to the engine whether dos or unix line endings are actually written.")},	// (FRIK_FILE)
//		{"fread",			PF_fread,			PF_fread,				0,	 PF_fread,0, D("int(filestream fhandle, void *ptr, int size)", "Reads binary data out of the file. Returns truncated lengths if the read exceeds the length of the file.")},
//		{"fwrite",			PF_fwrite,			PF_fwrite,				0,	 PF_fwrite,0, D("int(filestream fhandle, void *ptr, int size)", "Writes binary data out of the file.")},
		{"fseek",			PF_fseek,			PF_fseek,				0,	 PF_fseek,0, D("#define ftell fseek //c-compat\nint(filestream fhandle, optional int newoffset)", "Changes the current position of the file, if specified. Returns prior position, in bytes.")},
//		{"fsize",			PF_fsize,			PF_fsize,				0,	 PF_fsize,0, D("int(filestream fhandle, optional int newsize)", "Reports the total size of the file, in bytes. Can also be used to truncate/extend the file")},
	{"strlen",			PF_strlen,			PF_strlen,			114, PF_strlen,52, "float(string s)"},	// (FRIK_FILE)
	{"strcat",			PF_strcat,			PF_strcat,			115, PF_strcat,53, "string(string s1, optional string s2, optional string s3, optional string s4, optional string s5, optional string s6, optional string s7, optional string s8)"},	// (FRIK_FILE)
	{"substring",		PF_substring,		PF_substring,		116, PF_substring,54, "string(string s, float start, float length)"},	// (FRIK_FILE)
	{"stov",			PF_stov,			PF_stov,			117, PF_stov,55, "vector(string s)"},	// (FRIK_FILE)
	{"strzone",			PF_strzone,			PF_strzone,			118, PF_strzone,56,		D("string(string s, ...)", "Create a semi-permanent copy of a string that only becomes invalid once strunzone is called on the string (instead of when the engine assumes your string has left scope).")},	// (FRIK_FILE)
	{"strunzone",		PF_strunzone,		PF_strunzone,		119, PF_strunzone,57,	D("void(string s)", "Destroys a string that was allocated by strunzone. Further references to the string MAY crash the game.")},	// (FRIK_FILE)
	{"tokenize_menuqc",	PF_Tokenize,		PF_Tokenize,		0,	PF_Tokenize,58, "float(string s)"},
	{"localsound",		PF_NoSSQC,			PF_cl_localsound,	177,	PF_cl_localsound,65, D("void(string soundname, optional float channel, optional float volume)", "Plays a sound... locally... probably best not to call this from ssqc. Also disables reverb.")},//	#177
//	{"getmodelindex",	PF_getmodelindex,	PF_getmodelindex,	200,	PF_NoMenu, D("float(string modelname, optional float queryonly)", "Acts as an alternative to precache_model(foo);setmodel(bar, foo); return bar.modelindex;\nIf queryonly is set and the model was not previously precached, the builtin will return 0 without needlessly precaching the model.")},
//	{"externcall",		PF_externcall,		PF_externcall,		201,	PF_NoMenu, D("__variant(float prnum, string funcname, ...)", "Directly call a function in a different/same progs by its name.\nprnum=0 is the 'default' or 'main' progs.\nprnum=-1 means current progs.\nprnum=-2 will scan through the active progs and will use the first it finds.")},
//	{"addprogs",		PF_addprogs,		PF_addprogs,		202,	PF_NoMenu, D("float(string progsname)", "Loads an additional .dat file into the current qcvm. The returned handle can be used with any of the externcall/externset/externvalue builtins.\nThere are cvars that allow progs to be loaded automatically.")},
//	{"externvalue",		PF_externvalue,		PF_externvalue,		203,	PF_NoMenu, D("__variant(float prnum, string varname)", "Reads a global in the named progs by the name of that global.\nprnum=0 is the 'default' or 'main' progs.\nprnum=-1 means current progs.\nprnum=-2 will scan through the active progs and will use the first it finds.")},
//	{"externset",		PF_externset,		PF_externset,		204,	PF_NoMenu, D("void(float prnum, __variant newval, string varname)", "Sets a global in the named progs by name.\nprnum=0 is the 'default' or 'main' progs.\nprnum=-1 means current progs.\nprnum=-2 will scan through the active progs and will use the first it finds.")},
//	{"externrefcall",	PF_externrefcall,	PF_externrefcall,	205,	PF_NoMenu, D("__variant(float prnum, void() func, ...)","Calls a function between progs by its reference. No longer needed as direct function calls now switch progs context automatically, and have done for a long time. There is no remaining merit for this function."), true},
//	{"instr",			PF_instr,			PF_instr,			206,	PF_NoMenu, D("float(string input, string token)", "Returns substring(input, strstrpos(input, token), -1), or the null string if token was not found in input. You're probably better off using strstrpos."), true},
//	{"openportal",		PF_OpenPortal,		PF_OpenPortal,		207,	PF_NoMenu, D("void(entity portal, float state)", "Opens or closes the portals associated with a door or some such on q2 or q3 maps. On Q2BSPs, the entity should be the 'func_areaportal' entity - its style field will say which portal to open. On Q3BSPs, the entity is the door itself, the portal will be determined by the two areas found from a preceding setorigin call.")},
//	{"RegisterTempEnt", PF_RegisterTEnt,	PF_NoCSQC,			208,	PF_NoMenu, "float(float attributes, string effectname, ...)"},
//	{"CustomTempEnt",	PF_CustomTEnt,		PF_NoCSQC,			209,	PF_NoMenu, "void(float type, vector pos, ...)"},
//	{"fork",			PF_Fork,			PF_Fork,			210,	PF_NoMenu, D("float(optional float sleeptime)", "When called, this builtin simply returns. Twice.\nThe current 'thread' will return instantly with a return value of 0. The new 'thread' will return after sleeptime seconds with a return value of 1. See documentation for the 'sleep' builtin for limitations/requirements concerning the new thread. Note that QC should probably call abort in the new thread, as otherwise the function will return to the calling qc function twice also.")},
//	{"abort",			PF_Abort,			PF_Abort,			211,	PF_NoMenu, D("void(optional __variant ret)", "QC execution is aborted. Parent QC functions on the stack will be skipped, effectively this forces all QC functions to 'return ret' until execution returns to the engine. If ret is ommited, it is assumed to be 0.")},
//	{"sleep",			PF_Sleep,			PF_Sleep,			212,	PF_NoMenu, D("void(float sleeptime)", "Suspends the current QC execution thread for 'sleeptime' seconds.\nOther QC functions can and will be executed in the interim, including changing globals and field state (but not simultaneously).\nThe self and other globals will be restored when the thread wakes up (or set to world if they were removed since the thread started sleeping). Locals will be preserved, but will not be protected from remove calls.\nIf the engine is expecting the QC to return a value (even in the parent/root function), the value 0 shall be used instead of waiting for the qc to resume.")},
	{"forceinfokey",	PF_sv_forceinfokey,	PF_NoCSQC,			213,	PF_NoMenu, D("void(entity player, string key, string value)", "Directly changes a user's info without pinging off the client. Also allows explicitly setting * keys, including *spectator. Does not affect the user's config or other servers.")},
//	{"chat",			PF_chat,			PF_NoCSQC,			214,	PF_NoMenu, "void(string filename, float starttag, entity edict)"}, //(FTE_NPCCHAT)
//	{"particle2",		PF_sv_particle2,	PF_cl_particle2,	215,	PF_NoMenu, "void(vector org, vector dmin, vector dmax, float colour, float effect, float count)"},
//	{"particle3",		PF_sv_particle3,	PF_cl_particle3,	216,	PF_NoMenu, "void(vector org, vector box, float colour, float effect, float count)"},
//	{"particle4",		PF_sv_particle4,	PF_cl_particle4,	217,	PF_NoMenu, "void(vector org, float radius, float colour, float effect, float count)"},
	{"bitshift",		PF_bitshift,		PF_bitshift,		218,	PF_bitshift,0, "float(float number, float quantity)"},
	{"te_lightningblood",PF_sv_te_lightningblood,NULL,			219,	PF_NoMenu, "void(vector org)"},
//	{"map_builtin",		PF_builtinsupported,PF_builtinsupported,220,	PF_NoMenu, D("float(string builtinname, float builtinnum)","Attempts to map the named builtin at a non-standard builtin number. Returns 0 on failure."), true},	//like #100 - takes 2 args. arg0 is builtinname, 1 is number to map to.
	{"strstrofs",		PF_strstrofs,		PF_strstrofs,		221,	PF_strstrofs,221, D("float(string s1, string sub, optional float startidx)", "Returns the 0-based offset of sub within the s1 string, or -1 if sub is not in s1.\nIf startidx is set, this builtin will ignore matches before that 0-based offset.")},
	{"str2chr",			PF_str2chr,			PF_str2chr,			222,	PF_str2chr,222, D("float(string str, float index)", "Retrieves the character value at offset 'index'.")},
	{"chr2str",			PF_chr2str,			PF_chr2str,			223,	PF_chr2str,223, D("string(float chr, ...)", "The input floats are considered character values, and are concatenated.")},
	{"strconv",			PF_strconv,			PF_strconv,			224,	PF_strconv,224, D("string(float ccase, float redalpha, float redchars, string str, ...)", "Converts quake chars in the input string amongst different representations.\nccase specifies the new case for letters.\n 0: not changed.\n 1: forced to lower case.\n 2: forced to upper case.\nredalpha and redchars switch between colour ranges.\n 0: no change.\n 1: Forced white.\n 2: Forced red.\n 3: Forced gold(low) (numbers only).\n 4: Forced gold (high) (numbers only).\n 5+6: Forced to white and red alternately.\nYou should not use this builtin in combination with UTF-8.")},
	{"strpad",			PF_strpad,			PF_strpad,			225,	PF_strpad,225, D("string(float pad, string str1, ...)", "Pads the string with spaces, to ensure its a specific length (so long as a fixed-width font is used, anyway). If pad is negative, the spaces are added on the left. If positive the padding is on the right.")},	//will be moved
	{"infoadd",			PF_infoadd,			PF_infoadd,			226,	PF_infoadd,226, D("string(infostring old, string key, string value)", "Returns a new tempstring infostring with the named value changed (or added if it was previously unspecified). Key and value may not contain the \\ character.")},
	{"infoget",			PF_infoget,			PF_infoget,			227,	PF_infoget,227, D("string(infostring info, string key)", "Reads a named value from an infostring. The returned value is a tempstring")},
//	{"strcmp",			PF_strncmp,			PF_strncmp,			228,	PF_strncmp,228, D("float(string s1, string s2)", "Compares the two strings exactly. s1ofs allows you to treat s2 as a substring to compare against, or should be 0.\nReturns 0 if the two strings are equal, a negative value if s1 appears numerically lower, and positive if s1 appears numerically higher.")},
	{"strncmp",			PF_strncmp,			PF_strncmp,			228,	PF_strncmp,228, D("#define strcmp strncmp\nfloat(string s1, string s2, optional float len, optional float s1ofs, optional float s2ofs)", "Compares up to 'len' chars in the two strings. s1ofs allows you to treat s2 as a substring to compare against, or should be 0.\nReturns 0 if the two strings are equal, a negative value if s1 appears numerically lower, and positive if s1 appears numerically higher.")},
	{"strcasecmp",		PF_strncasecmp,		PF_strncasecmp,		229,	PF_strncasecmp,229, D("float(string s1, string s2)",  "Compares the two strings without case sensitivity.\nReturns 0 if they are equal. The sign of the return value may be significant, but should not be depended upon.")},
	{"strncasecmp",		PF_strncasecmp,		PF_strncasecmp,		230,	PF_strncasecmp,230, D("float(string s1, string s2, float len, optional float s1ofs, optional float s2ofs)", "Compares up to 'len' chars in the two strings without case sensitivity. s1ofs allows you to treat s2 as a substring to compare against, or should be 0.\nReturns 0 if they are equal. The sign of the return value may be significant, but should not be depended upon.")},
	{"strtrim",			PF_strtrim,			PF_strtrim,			0,		PF_strtrim,0, D("string(string s)", "Trims the whitespace from the start+end of the string.")},
//	{"calltimeofday",	PF_calltimeofday,	PF_calltimeofday,	231,	PF_NoMenu, D("void()", "Asks the engine to instantly call the qc's 'timeofday' function, before returning. For compatibility with mvdsv.\ntimeofday should have the prototype: void(float secs, float mins, float hour, float day, float mon, float year, string strvalue)\nThe strftime builtin is more versatile and less weird.")},
	{"clientstat",		PF_clientstat,		PF_NoCSQC,			232,	PF_NoMenu, D("void(float num, float type, .__variant fld)", "Specifies what data to use in order to send various stats, in a client-specific way.\n'num' should be a value between 32 and 127, other values are reserved.\n'type' must be set to one of the EV_* constants, one of EV_FLOAT, EV_STRING, EV_INTEGER, EV_ENTITY.\nfld must be a reference to the field used, each player will be sent only their own copy of these fields.")},	//EXT_CSQC
	{"globalstat",		PF_globalstat,		PF_NoCSQC,			233,	PF_NoMenu, D("void(float num, float type, string name)", "Specifies what data to use in order to send various stats, in a non-client-specific way. num and type are as in clientstat, name however, is the name of the global to read in the form of a string (pass \"foo\").")},	//EXT_CSQC_1 actually
	{"pointerstat",		PF_pointerstat,		PF_NoCSQC,			0,		PF_NoMenu, D("void(float num, float type, __variant *address)", "Specifies what data to use in order to send various stats, in a non-client-specific way. num and type are as in clientstat, address however, is the address of the variable you would like to use (pass &foo).")},
	{"isbackbuffered",	PF_isbackbuffered,	PF_NoCSQC,			234,	PF_NoMenu, D("float(entity player)", "Returns if the given player's network buffer will take multiple network frames in order to clear. If this builtin returns non-zero, you should delay or reduce the amount of reliable (and also unreliable) data that you are sending to that client.")},
//	{"rotatevectorsbyangle",PF_rotatevectorsbyangles,PF_rotatevectorsbyangles,235,PF_NoMenu,D("void(vector angle)", "rotates the v_forward,v_right,v_up matrix by the specified angles.")}, // #235
//	{"rotatevectorsbyvectors",PF_rotatevectorsbymatrix,PF_rotatevectorsbymatrix,PF_NoMenu, 236,"void(vector fwd, vector right, vector up)"}, // #236
//	{"skinforname",		PF_skinforname,		PF_skinforname,		237,	"float(float mdlindex, string skinname)"},		// #237
//	{"shaderforname",	PF_Fixme,			PF_Fixme,			238,	PF_NoMenu, D("float(string shadername, optional string defaultshader, ...)", "Caches the named shader and returns a handle to it.\nIf the shader could not be loaded from disk (missing file or ruleset_allow_shaders 0), it will be created from the 'defaultshader' string if specified, or a 'skin shader' default will be used.\ndefaultshader if not empty should include the outer {} that you would ordinarily find in a shader.")},
	{"te_bloodqw",		PF_sv_te_bloodqw,	NULL,				239,	PF_NoMenu, "void(vector org, float count)"},
//	{"te_muzzleflash",	PF_te_muzzleflash,	PF_clte_muzzleflash,0,		PF_NoMenu, "void(entity ent)"},
	{"checkpvs",		PF_checkpvs,		PF_checkpvs,		240,	PF_NoMenu, "float(vector viewpos, entity entity)"},
//	{"matchclientname",	PF_matchclient,		PF_NoCSQC,			241,	PF_NoMenu, "entity(string match, optional float matchnum)"},
//	{"sendpacket",		PF_SendPacket,		PF_SendPacket,		242,	PF_NoMenu, "void(string destaddress, string content)"},// (FTE_QC_SENDPACKET)
//	{"rotatevectorsbytag",PF_Fixme,			PF_Fixme,			244,	PF_NoMenu, "vector(entity ent, float tagnum)"},
	{"mod",				PF_mod,				PF_mod,				245,	PF_mod,70, "float(float a, float n)"},
	{"stoi",			PF_stoi,			PF_stoi,			259,	PF_stoi,0, D("int(string)", "Converts the given string into a true integer. Base 8, 10, or 16 is determined based upon the format of the string.")},
	{"itos",			PF_itos,			PF_itos,			260,	PF_itos,0, D("string(int)", "Converts the passed true integer into a base10 string.")},
	{"stoh",			PF_stoh,			PF_stoh,			261,	PF_stoh,0, D("int(string)", "Reads a base-16 string (with or without 0x prefix) as an integer. Bugs out if given a base 8 or base 10 string. :P")},
	{"htos",			PF_htos,			PF_htos,			262,	PF_htos,0, D("string(int)", "Formats an integer as a base16 string, with leading 0s and no prefix. Always returns 8 characters.")},
	{"ftoi",			PF_ftoi,			PF_ftoi,			0,		PF_ftoi,0, D("int(float)", "Converts the given float into a true integer without depending on extended qcvm instructions.")},
	{"itof",			PF_itof,			PF_itof,			0,		PF_itof,0, D("float(int)", "Converts the given true integer into a float without depending on extended qcvm instructions.")},
//	{"skel_create",		PF_skel_create,		PF_skel_create,		263,	PF_NoMenu, D("float(float modlindex, optional float useabstransforms)", "Allocates a new uninitiaised skeletal object, with enough bone info to animate the given model.\neg: self.skeletonobject = skel_create(self.modelindex);")}, // (FTE_CSQC_SKELETONOBJECTS)
//	{"skel_build",		PF_skel_build,		PF_skel_build,		264,	PF_NoMenu, D("float(float skel, entity ent, float modelindex, float retainfrac, float firstbone, float lastbone, optional float addfrac)", "Animation data (according to the entity's frame info) is pulled from the specified model and blended into the specified skeletal object.\nIf retainfrac is set to 0 on the first call and 1 on the others, you can blend multiple animations together according to the addfrac value. The final weight should be 1. Other values will result in scaling and/or other weirdness. You can use firstbone and lastbone to update only part of the skeletal object, to allow legs to animate separately from torso, use 0 for both arguments to specify all, as bones are 1-based.")}, // (FTE_CSQC_SKELETONOBJECTS)
//	{"skel_get_numbones",PF_skel_get_numbones,PF_skel_get_numbones,265,	PF_NoMenu, D("float(float skel)", "Retrives the number of bones in the model. The valid range is 1<=bone<=numbones.")}, // (FTE_CSQC_SKELETONOBJECTS)
//	{"skel_get_bonename",PF_skel_get_bonename,PF_skel_get_bonename,266,	PF_NoMenu, D("string(float skel, float bonenum)", "Retrieves the name of the specified bone. Mostly only for debugging.")}, // (FTE_CSQC_SKELETONOBJECTS) (returns tempstring)
//	{"skel_get_boneparent",PF_skel_get_boneparent,PF_skel_get_boneparentPF_NoMenu, ,267,D("float(float skel, float bonenum)", "Retrieves which bone this bone's position is relative to. Bone 0 refers to the entity's position rather than an actual bone")}, // (FTE_CSQC_SKELETONOBJECTS)
//	{"skel_find_bone",	PF_skel_find_bone,	PF_skel_find_bone,	268,	PF_NoMenu, D("float(float skel, string tagname)", "Finds a bone by its name, from the model that was used to create the skeletal object.")}, // (FTE_CSQC_SKELETONOBJECTS)
//	{"skel_get_bonerel",PF_skel_get_bonerel,PF_skel_get_bonerel,269,	PF_NoMenu, D("vector(float skel, float bonenum)", "Gets the bone position and orientation relative to the bone's parent. Return value is the offset, and v_forward, v_right, v_up contain the orientation.")}, // (FTE_CSQC_SKELETONOBJECTS) (sets v_forward etc)
//	{"skel_get_boneabs",PF_skel_get_boneabs,PF_skel_get_boneabs,270,	PF_NoMenu, D("vector(float skel, float bonenum)", "Gets the bone position and orientation relative to the entity. Return value is the offset, and v_forward, v_right, v_up contain the orientation.\nUse gettaginfo for world coord+orientation.")}, // (FTE_CSQC_SKELETONOBJECTS) (sets v_forward etc)
//	{"skel_set_bone",	PF_skel_set_bone,	PF_skel_set_bone,	271,	PF_NoMenu, D("void(float skel, float bonenum, vector org, optional vector fwd, optional vector right, optional vector up)", "Sets a bone position relative to its parent. If the orientation arguments are not specified, v_forward+v_right+v_up are used instead.")}, // (FTE_CSQC_SKELETONOBJECTS) (reads v_forward etc)
//	{"skel_premul_bone",PF_skel_premul_bone,PF_skel_premul_bone,272,	PF_NoMenu, D("void(float skel, float bonenum, vector org, optional vector fwd, optional vector right, optional vector up)", "Transforms a single bone by a matrix. You can use makevectors to generate a rotation matrix from an angle.")}, // (FTE_CSQC_SKELETONOBJECTS) (reads v_forward etc)
//	{"skel_premul_bones",PF_skel_premul_bones,PF_skel_premul_bones,273,	PF_NoMenu, D("void(float skel, float startbone, float endbone, vector org, optional vector fwd, optional vector right, optional vector up)", "Transforms an entire consecutive range of bones by a matrix. You can use makevectors to generate a rotation matrix from an angle, but you'll probably want to divide the angle by the number of bones.")}, // (FTE_CSQC_SKELETONOBJECTS) (reads v_forward etc)
//	{"skel_postmul_bone",PF_skel_postmul_bone,PF_skel_postmul_bone,0,	PF_NoMenu, D("void(float skel, float bonenum, vector org, optional vector fwd, optional vector right, optional vector up)", "Transforms a single bone by a matrix. You can use makevectors to generate a rotation matrix from an angle.")}, // (FTE_CSQC_SKELETONOBJECTS) (reads v_forward etc)
//	{"skel_postmul_bones",PF_skel_postmul_bones,PF_skel_postmul_bones,0,PF_NoMenu, D("void(float skel, float startbone, float endbone, vector org, optional vector fwd, optional vector right, optional vector up)", "Transforms an entire consecutive range of bones by a matrix. You can use makevectors to generate a rotation matrix from an angle, but you'll probably want to divide the angle by the number of bones.")}, // (FTE_CSQC_SKELETONOBJECTS) (reads v_forward etc)
//	{"skel_copybones",	PF_skel_copybones,	PF_skel_copybones,	274,	PF_NoMenu, D("void(float skeldst, float skelsrc, float startbone, float entbone)", "Copy bone data from one skeleton directly into another.")}, // (FTE_CSQC_SKELETONOBJECTS)
//	{"skel_delete",		PF_skel_delete,		PF_skel_delete,		275,	PF_NoMenu, D("void(float skel)", "Deletes a skeletal object. The actual delete is delayed, allowing the skeletal object to be deleted in an entity's predraw function yet still be valid by the time the addentity+renderscene builtins need it. Also uninstanciates any ragdoll currently in effect on the skeletal object.")}, // (FTE_CSQC_SKELETONOBJECTS)
	{"crossproduct",	PF_crossproduct,	PF_crossproduct,	0,		PF_crossproduct,0, D("#ifndef dotproduct\n#define dotproduct(v1,v2) ((vector)(v1)*(vector)(v2))\n#endif\nvector(vector v1, vector v2)", "Small helper function to calculate the crossproduct of two vectors.")},
//	{"pushmove", 		PF_pushmove, 		PF_pushmove,	 	0, 		PF_NoMenu, "float(entity pusher, vector move, vector amove)"},
	{"frameforname",	PF_frameforname,	PF_frameforname,	276,	PF_NoMenu, D("float(float modidx, string framename)", "Looks up a framegroup from a model by name, avoiding the need for hardcoding. Returns -1 on error.")},// (FTE_CSQC_SKELETONOBJECTS)
	{"frameduration",	PF_frameduration,	PF_frameduration,	277,	PF_NoMenu, D("float(float modidx, float framenum)", "Retrieves the duration (in seconds) of the specified framegroup.")},// (FTE_CSQC_SKELETONOBJECTS)
//	{"processmodelevents",PF_processmodelevents,PF_processmodelevents,0,PF_NoMenu, D("void(float modidx, float framenum, __inout float basetime, float targettime, void(float timestamp, int code, string data) callback)", "Calls a callback for each event that has been reached. Basetime is set to targettime.")},
//	{"getnextmodelevent",PF_getnextmodelevent,PF_getnextmodelevent,0,	PF_NoMenu, D("float(float modidx, float framenum, __inout float basetime, float targettime, __out int code, __out string data)", "Reports the next event within a model's animation. Returns a boolean if an event was found between basetime and targettime. Writes to basetime,code,data arguments (if an event was found, basetime is set to the event's time, otherwise to targettime).\nWARNING: this builtin cannot deal with multiple events with the same timestamp (only the first will be reported).")},
//	{"getmodeleventidx",PF_getmodeleventidx,PF_getmodeleventidx,0,		PF_NoMenu, D("float(float modidx, float framenum, int eventidx, __out float timestamp, __out int code, __out string data)", "Reports an indexed event within a model's animation. Writes to timestamp,code,data arguments on success. Returns false if the animation/event/model was out of range/invalid. Does not consider looping animations (retry from index 0 if it fails and you know that its a looping animation). This builtin is more annoying to use than getnextmodelevent, but can be made to deal with multiple events with the exact same timestamp.")},
	{"touchtriggers",	PF_touchtriggers,	PF_touchtriggers,	279,	PF_NoMenu, D("void(optional entity ent, optional vector neworigin)", "Triggers a touch events between self and every SOLID_TRIGGER entity that it is in contact with. This should typically just be the triggers touch functions. Also optionally updates the origin of the moved entity.")},//
	{"WriteFloat",		PF_WriteFloat,		PF_NoCSQC,			280,	PF_NoMenu, "void(float buf, float fl)"},
//	{"skel_ragupdate",	PF_skel_ragedit,	PF_skel_ragedit,	281,	PF_NoMenu, D("float(entity skelent, string dollcmd, float animskel)", "Updates the skeletal object attached to the entity according to its origin and other properties.\nif animskel is non-zero, the ragdoll will animate towards the bone state in the animskel skeletal object, otherwise they will pick up the model's base pose which may not give nice results.\nIf dollcmd is not set, the ragdoll will update (this should be done each frame).\nIf the doll is updated without having a valid doll, the model's default .doll will be instanciated.\ncommands:\n doll foo.doll : sets up the entity to use the named doll file\n dollstring TEXT : uses the doll file directly embedded within qc, with that extra prefix.\n cleardoll : uninstanciates the doll without destroying the skeletal object.\n animate 0.5 : specifies the strength of the ragdoll as a whole \n animatebody somebody 0.5 : specifies the strength of the ragdoll on a specific body (0 will disable ragdoll animations on that body).\n enablejoint somejoint 1 : enables (or disables) a joint. Disabling joints will allow the doll to shatter.")}, // (FTE_CSQC_RAGDOLL)
//	{"skel_mmap",		PF_skel_mmap,		PF_skel_mmap,		282,	PF_NoMenu, D("float*(float skel)", "Map the bones in VM memory. They can then be accessed via pointers. Each bone is 12 floats, the four vectors interleaved (sadly).")},// (FTE_QC_RAGDOLL)
//	{"skel_set_bone_world",PF_skel_set_bone_world,PF_skel_set_bone_world,283,PF_NoMenu, D("void(entity ent, float bonenum, vector org, optional vector angorfwd, optional vector right, optional vector up)", "Sets the world position of a bone within the given entity's attached skeletal object. The world position is dependant upon the owning entity's position. If no orientation argument is specified, v_forward+v_right+v_up are used for the orientation instead. If 1 is specified, it is understood as angles. If 3 are specified, they are the forawrd/right/up vectors to use.")},
	{"frametoname",		PF_frametoname,		PF_frametoname,		284,	PF_NoMenu, "string(float modidx, float framenum)"},
//	{"skintoname",		PF_skintoname,		PF_skintoname,		285,	PF_NoMenu, "string(float modidx, float skin)"},
//	{"resourcestatus",	PF_NoSSQC,			PF_resourcestatus,	286,	PF_NoMenu, D("float(float resourcetype, float tryload, string resourcename)", "resourcetype must be one of the RESTYPE_ constants. Returns one of the RESSTATE_ constants. Tryload 0 is a query only. Tryload 1 will attempt to reload the content if it was flushed.")},
//	{"hash_createtab",	PF_hash_createtab,	PF_hash_createtab,	287,	PF_NoMenu, D("hashtable(float tabsize, optional float defaulttype)", "Creates a hash table object with at least 'tabsize' slots. hash table with index 0 is a game-persistant table and will NEVER be returned by this builtin (except as an error return).")},
//	{"hash_destroytab",	PF_hash_destroytab,	PF_hash_destroytab,	288,	PF_NoMenu, D("void(hashtable table)", "Destroys a hash table object.")},
//	{"hash_add",		PF_hash_add,		PF_hash_add,		289,	PF_NoMenu, D("void(hashtable table, string name, __variant value, optional float typeandflags)", "Adds the given key with the given value to the table.\nIf flags&HASH_REPLACE, the old value will be removed, if not set then multiple values may be added for a single key, they won't overwrite.\nThe type argument describes how the value should be stored and saved to files. While you can claim that all variables are just vectors, being more precise can result in less issues with tempstrings or saved games.")},
//	{"hash_get",		PF_hash_get,		PF_hash_get,		290,	PF_NoMenu, D("__variant(hashtable table, string name, optional __variant deflt, optional float requiretype, optional float index)", "looks up the specified key name in the hash table. returns deflt if key was not found. If stringsonly=1, the return value will be in the form of a tempstring, otherwise it'll be the original value argument exactly as it was. If requiretype is specified, then values not of the specified type will be ignored. Hurrah for multiple types with the same name.")},
//	{"hash_delete",		PF_hash_delete,		PF_hash_delete,		291,	PF_NoMenu, D("__variant(hashtable table, string name)", "removes the named key. returns the value of the object that was destroyed, or 0 on error.")},
//	{"hash_getkey",		PF_hash_getkey,		PF_hash_getkey,		292,	PF_NoMenu, D("string(hashtable table, float idx)", "gets some random key name. add+delete can change return values of this, so don't blindly increment the key index if you're removing all.")},
//	{"hash_getcb",		PF_hash_getcb,		PF_hash_getcb,		293,	PF_NoMenu, D("void(hashtable table, void(string keyname, __variant val) callback, optional string name)", "For each item in the table that matches the name, call the callback. if name is omitted, will enumerate ALL keys."), true},
	{"checkcommand",	PF_checkcommand,	PF_checkcommand,	294,	PF_checkcommand,294, D("float(string name)", "Checks to see if the supplied name is a valid command, cvar, or alias. Returns 0 if it does not exist.")},
//	{"argescape",		PF_argescape,		PF_argescape,		295,	PF_NoMenu, D("string(string s)", "Marks up a string so that it can be reliably tokenized as a single argument later.")},
//	{"clusterevent",	PF_clusterevent,	PF_NoCSQC,			0,		PF_NoMenu, D("void(string dest, string from, string cmd, string info)", "Only functions in mapcluster mode. Sends an event to whichever server the named player is on. The destination server can then dispatch the event to the client or handle it itself via the SV_ParseClusterEvent entrypoint. If dest is empty, the event is broadcast to ALL servers. If the named player can't be found, the event will be returned to this server with the cmd prefixed with 'error:'.")},
//	{"clustertransfer",	PF_clustertransfer,	PF_NoCSQC,			0,		PF_NoMenu, D("string(entity player, optional string newnode)", "Only functions in mapcluster mode. Initiate transfer of the player to a different node. Can take some time. If dest is specified, returns null on error. Otherwise returns the current/new target node (or null if not transferring).")},
//	{"modelframecount", PF_modelframecount, PF_modelframecount,	0,		PF_NoMenu, D("float(float mdlidx)", "Retrieves the number of frames in the specified model.")},

	{"clearscene",		PF_NoSSQC,			PF_cl_clearscene,	300,	PF_cl_clearscene,300, D("void()", "Forgets all rentities, polygons, and temporary dlights. Resets all view properties to their default values.")},// (EXT_CSQC)
	{"addentities",		PF_NoSSQC,			PF_cs_addentities,	301,	PF_NoMenu, D("void(float mask)", "Walks through all entities effectively doing this:\n if (ent.drawmask&mask){ if (!ent.predaw()) addentity(ent); }\nIf mask&MASK_DELTA, non-csqc entities, particles, and related effects will also be added to the rentity list.\n If mask&MASK_STDVIEWMODEL then the default view model will also be added.")},// (EXT_CSQC)
	{"addentity",		PF_NoSSQC,			PF_cs_addentity,	302,	PF_m_addentity,302, D("void(entity ent)", "Copies the entity fields into a new rentity for later rendering via addscene.")},// (EXT_CSQC)
//	{"removeentity",	PF_NoSSQC,			PF_FullCSQCOnly,	0,		PF_NoMenu, D("void(entity ent)", "Undoes all addentities added to the scene from the given entity, without removing ALL entities (useful for splitscreen/etc, readd modified versions as desired).")},// (EXT_CSQC)
//	{"addtrisoup_simple",PF_NoSSQC,			PF_FullCSQCOnly,	0,		PF_NoMenu, D("typedef float vec2[2];\ntypedef float vec3[3];\ntypedef float vec4[4];\ntypedef struct trisoup_simple_vert_s {vec3 xyz;vec2 st;vec4 rgba;} trisoup_simple_vert_t;\nvoid(string texturename, int flags, struct trisoup_simple_vert_s *verts, int *indexes, int numindexes)", "Adds the specified trisoup into the scene as additional geometry. This permits caching geometry to reduce builtin spam. Indexes are a triangle list (so eg quads will need 6 indicies to form two triangles). NOTE: this is not going to be a speedup over polygons if you're still generating lots of new data every frame.")},
	{"setproperty",		PF_NoSSQC,			PF_cl_setproperty,	303,	PF_cl_setproperty,303, D("#define setviewprop setproperty\nfloat(float property, ...)", "Allows you to override default view properties like viewport, fov, and whether the engine hud will be drawn. Different VF_ values have slightly different arguments, some are vectors, some floats.")},// (EXT_CSQC)
	{"renderscene",		PF_NoSSQC,			PF_cl_renderscene,	304,	PF_cl_renderscene,304, D("void()", "Draws all entities, polygons, and particles on the rentity list (which were added via addentities or addentity), using the various view properties set via setproperty. There is no ordering dependancy.\nThe scene must generally be cleared again before more entities are added, as entities will persist even over to the next frame.\nYou may call this builtin multiple times per frame, but should only be called from CSQC_UpdateView.")},// (EXT_CSQC)
	{"dynamiclight_add",PF_NoSSQC,			PF_cs_addlight,		305,	PF_NoMenu, D("float(vector org, float radius, vector lightcolours, optional float style, optional string cubemapname, optional float pflags)", "Adds a temporary dlight, ready to be drawn via addscene. Cubemap orientation will be read from v_forward/v_right/v_up.")},// (EXT_CSQC)
	{"R_BeginPolygon",	PF_NoSSQC,			PF_R_PolygonBegin,	306,	PF_R_PolygonBegin,	306, D("void(string texturename, optional float flags, optional float is2d)", "Specifies the shader to use for the following polygons, along with optional flags.\nIf is2d, the polygon will be drawn as soon as the EndPolygon call is made, rather than waiting for renderscene. This allows complex 2d effects.")},// (EXT_CSQC_???)
	{"R_PolygonVertex",	PF_NoSSQC,			PF_R_PolygonVertex,	307,	PF_R_PolygonVertex,	307, D("void(vector org, vector texcoords, vector rgb, float alpha)", "Specifies a polygon vertex with its various properties.")},// (EXT_CSQC_???)
	{"R_EndPolygon",	PF_NoSSQC,			PF_R_PolygonEnd,	308,	PF_R_PolygonEnd,	308, D("void()", "Ends the current polygon. At least 3 verticies must have been specified. You do not need to call beginpolygon if you wish to draw another polygon with the same shader.")},
	{"getproperty",		PF_NoSSQC,			PF_cl_getproperty,	309,	PF_cl_getproperty,	309, D("#define getviewprop getproperty\n__variant(float property)", "Retrieve a currently-set (typically view) property, allowing you to read the current viewport or other things. Due to cheat protection, certain values may be unretrievable.")},// (EXT_CSQC_1)
	{"unproject",		PF_NoSSQC,			PF_cl_unproject,	310,	PF_NoMenu, D("vector (vector v)", "Transform a 2d screen-space point (with depth) into a 3d world-space point, according the various origin+angle+fov etc settings set via setproperty.")},// (EXT_CSQC)
	{"project",			PF_NoSSQC,			PF_cl_project,		311,	PF_NoMenu, D("vector (vector v)", "Transform a 3d world-space point into a 2d screen-space point, according the various origin+angle+fov etc settings set via setproperty.")},// (EXT_CSQC)
//	{"drawtextfield",	PF_NoSSQC,			PF_FullCSQCOnly,	0,		PF_NoMenu, D("void(vector pos, vector size, float alignflags, string text)", "Draws a multi-line block of text, including word wrapping and alignment. alignflags bits are RTLB, typically 3.")},// (EXT_CSQC)
//	{"drawline",		PF_NoSSQC,			PF_FullCSQCOnly,	315,	PF_NoMenu, D("void(float width, vector pos1, vector pos2, vector rgb, float alpha, optional float drawflag)", "Draws a 2d line between the two 2d points.")},// (EXT_CSQC)
	{"iscachedpic",		PF_NoSSQC,			PF_cl_iscachedpic,	316,	PF_cl_iscachedpic,451,		D("float(string name)", "Checks to see if the image is currently loaded. Engines might lie, or cache between maps.")},// (EXT_CSQC)
	{"precache_pic",	PF_NoSSQC,			PF_cl_precachepic,	317,	PF_cl_precachepic,452,		D("string(string name, optional float flags)", "Forces the engine to load the named image. If trywad is specified, the specified name must any lack path and extension.")},// (EXT_CSQC)
//	{"r_uploadimage",	PF_NoSSQC,			PF_FullCSQCOnly,	0,		PF_NoMenu, D("void(string imagename, int width, int height, int *pixeldata)", "Updates a texture with the specified rgba data. Will be created if needed.")},
//	{"r_readimage",		PF_NoSSQC,			PF_FullCSQCOnly,	0,		PF_NoMenu, D("int*(string filename, __out int width, __out int height)", "Reads and decodes an image from disk, providing raw pixel data. Returns __NULL__ if the image could not be read for any reason. Use memfree to free the data once you're done with it.")},
	{"drawgetimagesize",PF_NoSSQC,			PF_cl_getimagesize,	318,	PF_cl_getimagesize,460,		D("#define draw_getimagesize drawgetimagesize\nvector(string picname)", "Returns the dimensions of the named image. Images specified with .lmp should give the original .lmp's dimensions even if texture replacements use a different resolution.")},// (EXT_CSQC)
//	{"freepic",			PF_NoSSQC,			PF_FullCSQCOnly,	319,	PF_NoMenu, D("void(string name)", "Tells the engine that the image is no longer needed. The image will appear to be new the next time its needed.")},// (EXT_CSQC)
	{"drawcharacter",	PF_NoSSQC,			PF_cl_drawcharacter,320,	PF_cl_drawcharacter,454,	D("float(vector position, float character, vector size, vector rgb, float alpha, optional float drawflag)", "Draw the given quake character at the given position.\nIf flag&4, the function will consider the char to be a unicode char instead (or display as a ? if outside the 32-127 range).\nsize should normally be something like '8 8 0'.\nrgb should normally be '1 1 1'\nalpha normally 1.\nSoftware engines may assume the named defaults.\nNote that ALL text may be rescaled on the X axis due to variable width fonts. The X axis may even be ignored completely.")},// (EXT_CSQC, [EXT_CSQC_???])
	{"drawrawstring",	PF_NoSSQC,			PF_cl_drawrawstring,321,	PF_cl_drawrawstring,455,	D("float(vector position, string text, vector size, vector rgb, float alpha, optional float drawflag)", "Draws the specified string without using any markup at all, even in engines that support it.\nIf UTF-8 is globally enabled in the engine, then that encoding is used (without additional markup), otherwise it is raw quake chars.\nSoftware engines may assume a size of '8 8 0', rgb='1 1 1', alpha=1, flag&3=0, but it is not an error to draw out of the screen.")},// (EXT_CSQC, [EXT_CSQC_???])
	{"drawpic",			PF_NoSSQC,			PF_cl_drawpic,		322,	PF_cl_drawpic,456,			D("float(vector position, string pic, vector size, vector rgb, float alpha, optional float drawflag)", "Draws an shader within the given 2d screen box. Software engines may omit support for rgb+alpha, but must support rescaling, and must clip to the screen without crashing.")},// (EXT_CSQC, [EXT_CSQC_???])
	{"drawfill",		PF_NoSSQC,			PF_cl_drawfill,		323,	PF_cl_drawfill,457,			D("float(vector position, vector size, vector rgb, float alpha, optional float drawflag)", "Draws a solid block over the given 2d box, with given colour, alpha, and blend mode (specified via flags).\nflags&3=0 simple blend.\nflags&3=1 additive blend")},// (EXT_CSQC, [EXT_CSQC_???])
	{"drawsetcliparea",	PF_NoSSQC,			PF_cl_drawsetclip,	324,	PF_cl_drawsetclip,458,		D("void(float x, float y, float width, float height)", "Specifies a 2d clipping region (aka: scissor test). 2d draw calls will all be clipped to this 2d box, the area outside will not be modified by any 2d draw call (even 2d polygons).")},// (EXT_CSQC_???)
	{"drawresetcliparea",PF_NoSSQC,			PF_cl_drawresetclip,325,	PF_cl_drawresetclip,459,	D("void(void)", "Reverts the scissor/clip area to the whole screen.")},// (EXT_CSQC_???)
	{"drawstring",		PF_NoSSQC,			PF_cl_drawstring,	326,	PF_cl_drawstring,467,		D("float(vector position, string text, vector size, vector rgb, float alpha, float drawflag)", "Draws a string, interpreting markup and recolouring as appropriate.")},// #326
	{"stringwidth",		PF_NoSSQC,			PF_cl_stringwidth,	327,	PF_cl_stringwidth,468,		D("float(string text, float usecolours, vector fontsize='8 8')", "Calculates the width of the screen in virtual pixels. If usecolours is 1, markup that does not affect the string width will be ignored. Will always be decoded as UTF-8 if UTF-8 is globally enabled.\nIf the char size is not specified, '8 8 0' will be assumed.")},// EXT_CSQC_'DARKPLACES'
	{"drawsubpic",		PF_NoSSQC,			PF_cl_drawsubpic,	328,	PF_cl_drawsubpic,369,		D("void(vector pos, vector sz, string pic, vector srcpos, vector srcsz, vector rgb, float alpha, optional float drawflag)", "Draws a rescaled subsection of an image to the screen.")},// #328 EXT_CSQC_'DARKPLACES'
//	{"drawrotpic",		PF_NoSSQC,			PF_FullCSQCOnly,	0,		PF_NoMenu, D("void(vector pivot, vector mins, vector maxs, string pic, vector rgb, float alpha, float angle)", "Draws an image rotating at the pivot. To rotate in the center, use mins+maxs of half the size with mins negated. Angle is in degrees.")},
//	{"drawrotsubpic",	PF_NoSSQC,			PF_FullCSQCOnly,	0,		PF_NoMenu, D("void(vector pivot, vector mins, vector maxs, string pic, vector txmin, vector txsize, vector rgb, vector alphaandangles)", "Overcomplicated draw function for over complicated people. Positions follow drawrotpic, while texture coords follow drawsubpic. Due to argument count limitations in builtins, the alpha value and angles are combined into separate fields of a vector (tip: use fteqcc's [alpha, angle] feature.")},
	{"getstati",		PF_NoSSQC,			PF_cl_getstat_int,	330,	PF_NoMenu, D("#define getstati_punf(stnum) (float)(__variant)getstati(stnum)\nint(float stnum)", "Retrieves the numerical value of the given EV_INTEGER or EV_ENTITY stat. Use getstati_punf if you wish to type-pun a float stat as an int to avoid truncation issues in DP.")},// (EXT_CSQC)
	{"getstatf",		PF_NoSSQC,			PF_cl_getstat_float,331,	PF_NoMenu, D("#define getstatbits getstatf\nfloat(float stnum, optional float firstbit, optional float bitcount)", "Retrieves the numerical value of the given EV_FLOAT stat. If firstbit and bitcount are specified, retrieves the upper bits of the STAT_ITEMS stat (converted into a float, so there are no VM dependancies).")},// (EXT_CSQC)
	{"getstats",		PF_NoSSQC,			PF_cl_getstat_string,332,	PF_NoMenu, D("string(float stnum)", "Retrieves the value of the given EV_STRING stat, as a tempstring.\nString stats use a separate pool of stats from numeric ones.\n")},
//	{"getplayerstat",	PF_NoSSQC,			PF_FullCSQCOnly,	0,		PF_NoMenu, D("__variant(float playernum, float statnum, float stattype)", "Retrieves a specific player's stat, matching the type specified on the server. This builtin is primarily intended for mvd playback where ALL players are known. For EV_ENTITY, world will be returned if the entity is not in the pvs, use type-punning with EV_INTEGER to get the entity number if you just want to see if its set. STAT_ITEMS should be queried as an EV_INTEGER on account of runes and items2 being packed into the upper bits.")},
	{"setmodelindex",	PF_sv_setmodelindex,PF_cl_setmodelindex,333,	PF_NoMenu, D("void(entity e, float mdlindex)", "Sets a model by precache index instead of by name. Otherwise identical to setmodel.")},//
//	{"modelnameforindex",PF_modelnameforidx,PF_modelnameforidx,	334,	PF_NoMenu, D("string(float mdlindex)", "Retrieves the name of the model based upon a precache index. This can be used to reduce csqc network traffic by enabling model matching.")},//
	{"particleeffectnum",PF_sv_particleeffectnum,PF_cl_particleeffectnum,335,PF_NoMenu, D("float(string effectname)", "Precaches the named particle effect. If your effect name is of the form 'foo.bar' then particles/foo.cfg will be loaded by the client if foo.bar was not already defined.\nDifferent engines will have different particle systems, this specifies the QC API only.")},// (EXT_CSQC)
	{"trailparticles",	PF_sv_trailparticles,PF_cl_trailparticles,336,	PF_NoMenu,	D("void(float effectnum, entity ent, vector start, vector end)", "Draws the given effect between the two named points. If ent is not world, distances will be cached in the entity in order to avoid framerate dependancies. The entity is not otherwise used.")},// (EXT_CSQC),
	{"pointparticles",	PF_sv_pointparticles,PF_cl_pointparticles,337,	PF_NoMenu,	D("void(float effectnum, vector origin, optional vector dir, optional float count)", "Spawn a load of particles from the given effect at the given point traveling or aiming along the direction specified. The number of particles are scaled by the count argument.")},// (EXT_CSQC)
	{"cprint",			PF_NoSSQC,			PF_cl_cprint,		338,	PF_NoMenu,	D("void(string s, ...)", "Print into the center of the screen just as ssqc's centerprint would appear.")},//(EXT_CSQC)
	{"print",			PF_print,			PF_print,			339,	PF_print,339,D("void(string s, ...)", "Unconditionally print on the local system's console, even in ssqc (doesn't care about the value of the developer cvar).")},//(EXT_CSQC)
	{"menu_print",		PF_NoSSQC,			PF_NoCSQC,			0,		PF_print,4,	D("void(string s, ...)", "Unconditionally print on the local system's console, even in ssqc (doesn't care about the value of the developer cvar).")},//(EXT_CSQC)
	{"keynumtostring",	NULL,				PF_cl_keynumtostring,340,	PF_cl_keynumtostring,609,	D("string(float keynum)", "Returns a hunam-readable name for the given keycode, as a tempstring.")},// (EXT_CSQC)
	{"stringtokeynum",	NULL,				PF_cl_stringtokeynum,341,	PF_cl_stringtokeynum,341,	D("float(string keyname)", "Looks up the key name in the same way that the bind command would, returning the keycode for that key.")},// (EXT_CSQC)
	{"getkeybind",		NULL,				PF_cl_getkeybind,	342,	PF_cl_getkeybind,342, D("string(float keynum)", "Returns the current binding for the given key (returning only the command executed when no modifiers are pressed).")},// (EXT_CSQC)
	{"setcursormode",	PF_NoSSQC,			PF_cl_setcursormode,343,	PF_cl_setcursormode,343, D("void(float usecursor, optional string cursorimage, optional vector hotspot, optional float scale)", "Pass TRUE if you want the engine to release the mouse cursor (absolute input events + touchscreen mode). Pass FALSE if you want the engine to grab the cursor (relative input events + standard looking). If the image name is specified, the engine will use that image for a cursor (use an empty string to clear it again), in a way that will not conflict with the console. Images specified this way will be hardware accelerated, if supported by the platform/port.")},
	{"getcursormode",	PF_NoSSQC,			PF_cl_getcursormode,0,		PF_cl_getcursormode,0, D("float(float effective)", "Reports the cursor mode this module previously attempted to use. If 'effective' is true, reports the cursor mode currently active (if was overriden by a different module which has precidence, for instance, or if there is only a touchscreen and no mouse).")},
	{"getmousepos",		PF_NoSSQC,			PF_NoCSQC,	344,	PF_m_getmousepos,66, D("vector()", "Nasty convoluted DP extension. Typically returns deltas instead of positions. Use CSQC_InputEvent for such things in csqc mods.")},	// #344 This is a DP extension
	{"getinputstate",	PF_NoSSQC,			PF_cs_getinputstate,345,	PF_NoMenu, D("float(float inputsequencenum)", "Looks up an input frame from the log, setting the input_* globals accordingly.\nThe sequence number range used for prediction should normally be servercommandframe < sequence <= clientcommandframe.\nThe sequence equal to clientcommandframe will change between input frames.")},// (EXT_CSQC)
	{"setsensitivityscaler",PF_NoSSQC,		PF_cl_setsensitivity,346,	PF_NoMenu, D("void(float sens)", "Temporarily scales the player's mouse sensitivity based upon something like zoom, avoiding potential cvar saving and thus corruption.")},// (EXT_CSQC)
//	{"runstandardplayerphysics",NULL,		PF_FullCSQCOnly,	347,	PF_NoMenu, D("void(entity ent)", "Perform the engine's standard player movement prediction upon the given entity using the input_* globals to describe movement.")},
	{"getplayerkeyvalue",NULL,				PF_cl_playerkey_s,	348,	PF_NoMenu, D("string(float playernum, string keyname)", "Look up a player's userinfo, to discover things like their name, topcolor, bottomcolor, skin, team, *ver.\nAlso includes scoreboard info like frags, ping, pl, userid, entertime, as well as voipspeaking and voiploudness.")},// (EXT_CSQC)
	{"getplayerkeyfloat",NULL,				PF_cl_playerkey_f,	0,		PF_NoMenu, D("float(float playernum, string keyname, optional float assumevalue)", "Cheaper version of getplayerkeyvalue that avoids the need for so many tempstrings.")},
	{"isdemo",			PF_NoSSQC,			PF_cl_isdemo,		349,	PF_cl_isdemo,349, D("float()", "Returns if the client is currently playing a demo or not")},// (EXT_CSQC)
	{"isserver",		PF_NoSSQC,			PF_cl_isserver,		350,	PF_cl_isserver,60, D("float()", "Returns if the client is acting as the server (aka: listen server)")},//(EXT_CSQC)
	{"SetListener",		NULL,				PF_cs_setlistener,	351,	PF_NoMenu, D("void(vector origin, vector forward, vector right, vector up, optional float reverbtype)", "Sets the position of the view, as far as the audio subsystem is concerned. This should be called once per CSQC_UpdateView as it will otherwise revert to default. For reverbtype, see setup_reverb or treat as 'underwater'.")},// (EXT_CSQC)
//	{"setup_reverb",	PF_NoSSQC,			PF_FullCSQCOnly,	0,		PF_NoMenu, D("typedef struct {\n\tfloat flDensity;\n\tfloat flDiffusion;\n\tfloat flGain;\n\tfloat flGainHF;\n\tfloat flGainLF;\n\tfloat flDecayTime;\n\tfloat flDecayHFRatio;\n\tfloat flDecayLFRatio;\n\tfloat flReflectionsGain;\n\tfloat flReflectionsDelay;\n\tvector flReflectionsPan;\n\tfloat flLateReverbGain;\n\tfloat flLateReverbDelay;\n\tvector flLateReverbPan;\n\tfloat flEchoTime;\n\tfloat flEchoDepth;\n\tfloat flModulationTime;\n\tfloat flModulationDepth;\n\tfloat flAirAbsorptionGainHF;\n\tfloat flHFReference;\n\tfloat flLFReference;\n\tfloat flRoomRolloffFactor;\n\tint   iDecayHFLimit;\n} reverbinfo_t;\nvoid(float reverbslot, reverbinfo_t *reverbinfo, int sizeofreverinfo_t)", "Reconfigures a reverb slot for weird effects. Slot 0 is reserved for no effects. Slot 1 is reserved for underwater effects. Reserved slots will be reinitialised on snd_restart, but can otherwise be changed. These reverb slots can be activated with SetListener. Note that reverb will currently only work when using OpenAL.")},
	{"registercommand",	NULL,				PF_cl_registercommand,352,	PF_cl_registercommand,352, D("void(string cmdname)", "Register the given console command, for easy console use.\nConsole commands that are later used will invoke CSQC_ConsoleCommand.")},//(EXT_CSQC)
	{"wasfreed",		PF_WasFreed,		PF_WasFreed,		353,	PF_WasFreed,353, D("float(entity ent)", "Quickly check to see if the entity is currently free. This function is only valid during the two-second non-reuse window, after that it may give bad results. Try one second to make it more robust.")},//(EXT_CSQC) (should be availabe on server too)
	{"serverkey",		NULL,				PF_cl_serverkey_s,	354,	PF_NoMenu, D("string(string key)", "Look up a key in the server's public serverinfo string")},//
	{"serverkeyfloat",	NULL,				PF_cl_serverkey_f,	0,		PF_NoMenu, D("float(string key, optional float assumevalue)", "Version of serverkey that returns the value as a float (which avoids tempstrings).")},//
	{"getentitytoken",	PF_NoSSQC,			PF_cs_getentitytoken,355,	PF_NoMenu, D("string(optional string resetstring)", "Grab the next token in the map's entity lump.\nIf resetstring is not specified, the next token will be returned with no other sideeffects.\nIf empty, will reset from the map before returning the first token, probably {.\nIf not empty, will tokenize from that string instead.\nAlways returns tempstrings.")},//;
//	{"findfont",		PF_NoSSQC,			PF_FullCSQCOnly,	356,	PF_NoMenu, D("float(string s)", "Looks up a named font slot. Matches the actual font name as a last resort.")},//;
//	{"loadfont",		PF_NoSSQC,			PF_FullCSQCOnly,	357,	PF_NoMenu, D("float(string fontname, string fontmaps, string sizes, float slot, optional float fix_scale, optional float fix_voffset)", "too convoluted for me to even try to explain correct usage. Try drawfont = loadfont(\"\", \"cour\", \"16\", -1, 0, 0); to switch to the courier font (optimised for 16 virtual pixels high), if you have the freetype2 library in windows..")},
	{"sendevent",		PF_NoSSQC,			PF_cl_sendevent,	359,	PF_NoMenu, D("void(string evname, string evargs, ...)", "Invoke Cmd_evname_evargs in ssqc. evargs must be a string of initials refering to the types of the arguments to pass. v=vector, e=entity(.entnum field is sent), f=float, i=int. 6 arguments max - you can get more if you pack your floats into vectors.")},// (EXT_CSQC_1)
	{"readbyte",		PF_NoSSQC,			PF_cl_readbyte,		360,	PF_NoMenu, "float()"},// (EXT_CSQC)
	{"readchar",		PF_NoSSQC,			PF_cl_readchar,		361,	PF_NoMenu, "float()"},// (EXT_CSQC)
	{"readshort",		PF_NoSSQC,			PF_cl_readshort,	362,	PF_NoMenu, "float()"},// (EXT_CSQC)
	{"readlong",		PF_NoSSQC,			PF_cl_readlong,		363,	PF_NoMenu, "float()"},// (EXT_CSQC)
	{"readcoord",		PF_NoSSQC,			PF_cl_readcoord,	364,	PF_NoMenu, "float()"},// (EXT_CSQC)
	{"readangle",		PF_NoSSQC,			PF_cl_readangle,	365,	PF_NoMenu, "float()"},// (EXT_CSQC)
	{"readstring",		PF_NoSSQC,			PF_cl_readstring,	366,	PF_NoMenu, "string()"},// (EXT_CSQC)
	{"readfloat",		PF_NoSSQC,			PF_cl_readfloat,	367,	PF_NoMenu, "float()"},// (EXT_CSQC)
	{"readentitynum",	PF_NoSSQC,			PF_cl_readentitynum,368,	PF_NoMenu, "float()"},// (EXT_CSQC)
//	{"deltalisten",		NULL,				PF_FullCSQCOnly,	371,	PF_NoMenu, D("float(string modelname, float(float isnew) updatecallback, float flags)", "Specifies a per-modelindex callback to listen for engine-networking entity updates. Such entities are automatically interpolated by the engine (unless flags specifies not to).\nThe various standard entity fields will be overwritten each frame before the updatecallback function is called.")},//  (EXT_CSQC_1)
//	{"dynamiclight_get",PF_NoSSQC,			PF_FullCSQCOnly,	372,	PF_NoMenu, D("__variant(float lno, float fld)", "Retrieves a property from the given dynamic/rt light. Return type depends upon the light field requested.")},
//	{"dynamiclight_set",PF_NoSSQC,			PF_FullCSQCOnly,	373,	PF_NoMenu, D("void(float lno, float fld, __variant value)", "Changes a property on the given dynamic/rt light. Value type depends upon the light field to be changed.")},
//	{"particleeffectquery",PF_NoSSQC,		PF_FullCSQCOnly,	374,	PF_NoMenu, D("string(float efnum, float body)", "Retrieves either the name or the body of the effect with the given number. The effect body is regenerated from internal state, and can be changed before being reapplied via the localcmd builtin.")},
//	{"adddecal",		PF_NoSSQC,			PF_FullCSQCOnly,	375,	PF_NoMenu, D("void(string shadername, vector origin, vector up, vector side, vector rgb, float alpha)", "Adds a temporary clipped decal shader to the scene, centered at the given point with given orientation. Will be drawn by the next renderscene call, and freed by the next clearscene call.")},
//	{"setcustomskin",	PF_NoSSQC,			PF_FullCSQCOnly,	376,	PF_NoMenu, D("void(entity e, string skinfilename, optional string skindata)", "Sets an entity's skin overrides. These are custom per-entity surface->shader lookups. The skinfilename/data should be in .skin format:\nsurfacename,shadername - makes the named surface use the named shader\nreplace \"surfacename\" \"shadername\" - same.\nqwskin \"foo\" - use an unmodified quakeworld player skin (including crop+repalette rules)\nq1lower 0xff0000 - specify an override for the entity's lower colour, in this case to red\nq1upper 0x0000ff - specify an override for the entity's lower colour, in this case to blue\ncompose \"surfacename\" \"shader\" \"imagename@x,y:w,h$s,t,s2,t2?r,g,b,a\" - compose a skin texture from multiple images.\n  The texture is determined to be sufficient to hold the first named image, additional images can be named as extra tokens on the same line.\n  Use a + at the end of the line to continue reading image tokens from the next line also, the named shader must use 'map $diffuse' to read the composed texture (compatible with the defaultskin shader).")},
//	{"memalloc",		PF_memalloc,		PF_memalloc,		384,	PF_NoMenu, D("__variant*(int size)", "Allocate an arbitary block of memory")},
//	{"memfree",			PF_memfree,			PF_memfree,			385,	PF_NoMenu, D("void(__variant *ptr)", "Frees a block of memory that was allocated with memfree")},
//	{"memcpy",			PF_memcpy,			PF_memcpy,			386,	PF_NoMenu, D("void(__variant *dst, __variant *src, int size)", "Copys memory from one location to another")},
//	{"memfill8",		PF_memfill8,		PF_memfill8,		387,	PF_NoMenu, D("void(__variant *dst, int val, int size)", "Sets an entire block of memory to a specified value. Pretty much always 0.")},
//	{"memgetval",		PF_memgetval,		PF_memgetval,		388,	PF_NoMenu, D("__variant(__variant *dst, float ofs)", "Looks up the 32bit value stored at a pointer-with-offset.")},
//	{"memsetval",		PF_memsetval,		PF_memsetval,		389,	PF_NoMenu, D("void(__variant *dst, float ofs, __variant val)", "Changes the 32bit value stored at the specified pointer-with-offset.")},
//	{"memptradd",		PF_memptradd,		PF_memptradd,		390,	PF_NoMenu, D("__variant*(__variant *base, float ofs)", "Perform some pointer maths. Woo.")},
//	{"memstrsize",		PF_memstrsize,		PF_memstrsize,		0,		PF_NoMenu, D("float(string s)", "strlen, except ignores utf-8")},
//	{"con_getset",		PF_NoSSQC,			PF_FullCSQCOnly,	391,	PF_NoMenu, D("string(string conname, string field, optional string newvalue)", "Reads or sets a property from a console object. The old value is returned. Iterrate through consoles with the 'next' field. Valid properties: 	title, name, next, unseen, markup, forceutf8, close, clear, hidden, linecount")},
//	{"con_printf",		PF_NoSSQC,			PF_FullCSQCOnly,	392,	PF_NoMenu, D("void(string conname, string messagefmt, ...)", "Prints onto a named console.")},
//	{"con_draw",		PF_NoSSQC,			PF_FullCSQCOnly,	393,	PF_NoMenu, D("void(string conname, vector pos, vector size, float fontsize)", "Draws the named console.")},
//	{"con_input",		PF_NoSSQC,			PF_FullCSQCOnly,	394,	PF_NoMenu, D("float(string conname, float inevtype, float parama, float paramb, float paramc)", "Forwards input events to the named console. Mouse updates should be absolute only.")},
	{"setwindowcaption",PF_NoSSQC,			PF_cl_setwindowcaption,0,	PF_cl_setwindowcaption,0, D("void(string newcaption)", "Replaces the title of the game window, as seen when task switching or just running in windowed mode.")},
//	{"cvars_haveunsaved",PF_NoSSQC,			PF_FullCSQCOnly,	0,		PF_NoMenu, D("float()", "Returns true if any archived cvar has an unsaved value.")},
//	{"entityprotection",NULL,				NULL,				0,		PF_NoMenu, D("float(entity e, float nowreadonly)", "Changes the protection on the specified entity to protect it from further edits from QC. The return value is the previous setting. Note that this can be used to unprotect the world, but doing so long term is not advised as you will no longer be able to detect invalid entity references. Also, world is not networked, so results might not be seen by clients (or in other words, world.avelocity_y=64 is a bad idea).")},


	{"copyentity",		PF_copyentity,		PF_copyentity,		400,	PF_copyentity,47, D("entity(entity from, optional entity to)", "Copies all fields from one entity to another.")},// (DP_QC_COPYENTITY)
	{"setcolors",		PF_setcolors,		PF_NoCSQC,			401,	PF_NoMenu, D("void(entity ent, float colours)", "Changes a player's colours. The bits 0-3 are the lower/trouser colour, bits 4-7 are the upper/shirt colours.")},//DP_SV_SETCOLOR
	{"findchain",		PF_findchain,		PF_findchain,		402,	PF_findchain,26, "entity(.string field, string match, optional .entity chainfield)"},// (DP_QC_FINDCHAIN)
	{"findchainfloat",	PF_findchainfloat,	PF_findchainfloat,	403,	PF_findchainfloat,27, "entity(.float fld, float match, optional .entity chainfield)"},// (DP_QC_FINDCHAINFLOAT)
	{"effect",			PF_sv_effect,		NULL,				404,	PF_NoMenu, D("void(vector org, string modelname, float startframe, float endframe, float framerate)", "stub. Spawns a self-animating sprite")},// (DP_SV_EFFECT)
	{"te_blood",		PF_sv_te_blooddp,	NULL,				405,	PF_NoMenu, "void(vector org, vector dir, float count)"},// #405 te_blood
	{"te_bloodshower",	PF_sv_te_bloodshower,NULL,				406,	PF_NoMenu, "void(vector mincorner, vector maxcorner, float explosionspeed, float howmany)", "stub."},// (DP_TE_BLOODSHOWER)
	{"te_explosionrgb",	PF_sv_te_explosionrgb,NULL,				407,	PF_NoMenu, "void(vector org, vector color)", "stub."},// (DP_TE_EXPLOSIONRGB)
	{"te_particlecube",	PF_sv_te_particlecube,NULL,				408,	PF_NoMenu, "void(vector mincorner, vector maxcorner, vector vel, float howmany, float color, float gravityflag, float randomveljitter)", "stub."},// (DP_TE_PARTICLECUBE)
	{"te_particlerain",	PF_sv_te_particlerain,NULL,				409,	PF_NoMenu, "void(vector mincorner, vector maxcorner, vector vel, float howmany, float color)"},// (DP_TE_PARTICLERAIN)
	{"te_particlesnow",	PF_sv_te_particlesnow,NULL,				410,	PF_NoMenu, "void(vector mincorner, vector maxcorner, vector vel, float howmany, float color)"},// (DP_TE_PARTICLESNOW)
	{"te_spark",		PF_sv_te_spark,		NULL,				411,	PF_NoMenu, "void(vector org, vector vel, float howmany)", "stub."},// (DP_TE_SPARK)
	{"te_gunshotquad",	PF_sv_te_gunshotquad,NULL,				412,	PF_NoMenu, "void(vector org)", "stub."},// (DP_TE_QUADEFFECTS1)
	{"te_spikequad",	PF_sv_te_spikequad,	NULL,				413,	PF_NoMenu, "void(vector org)", "stub."},// (DP_TE_QUADEFFECTS1)
	{"te_superspikequad",PF_sv_te_superspikequad,NULL,			414,	PF_NoMenu, "void(vector org)", "stub."},// (DP_TE_QUADEFFECTS1)
	{"te_explosionquad",PF_sv_te_explosionquad,NULL,			415,	PF_NoMenu, "void(vector org)", "stub."},// (DP_TE_QUADEFFECTS1)
	{"te_smallflash",	PF_sv_te_smallflash,NULL,				416,	PF_NoMenu, "void(vector org)", "stub."},// (DP_TE_SMALLFLASH)
	{"te_customflash",	PF_sv_te_customflash,NULL,				417,	PF_NoMenu, "void(vector org, float radius, float lifetime, vector color)", "stub."},// (DP_TE_CUSTOMFLASH)
	{"te_gunshot",		PF_sv_te_gunshot,	PF_cl_te_gunshot,	418,	PF_NoMenu, "void(vector org, optional float count)"},// #418 te_gunshot
	{"te_spike",		PF_sv_te_spike,		PF_cl_te_spike,		419,	PF_NoMenu, "void(vector org)"},// #419 te_spike
	{"te_superspike",	PF_sv_te_superspike,PF_cl_te_superspike,420,	PF_NoMenu, "void(vector org)"},// #420 te_superspike
	{"te_explosion",	PF_sv_te_explosion,	PF_cl_te_explosion,	421,	PF_NoMenu, "void(vector org)"},// #421 te_explosion
	{"te_tarexplosion",	PF_sv_te_tarexplosion,PF_cl_te_tarexplosion,422,PF_NoMenu, "void(vector org)"},// #422 te_tarexplosion
	{"te_wizspike",		PF_sv_te_wizspike,	PF_cl_te_wizspike,	423,	PF_NoMenu, "void(vector org)"},// #423 te_wizspike
	{"te_knightspike",	PF_sv_te_knightspike,PF_cl_te_knightspike,424,	PF_NoMenu, "void(vector org)"},// #424 te_knightspike
	{"te_lavasplash",	PF_sv_te_lavasplash,PF_cl_te_lavasplash,425,	PF_NoMenu, "void(vector org)"},// #425 te_lavasplash
	{"te_teleport",		PF_sv_te_teleport,	PF_cl_te_teleport,	426,	PF_NoMenu, "void(vector org)"},// #426 te_teleport
	{"te_explosion2",	PF_sv_te_explosion2,PF_cl_te_explosion2,427,	PF_NoMenu, "void(vector org, float color, float colorlength)"},// #427 te_explosion2
	{"te_lightning1",	PF_sv_te_lightning1,PF_cl_te_lightning1,428,	PF_NoMenu, "void(entity own, vector start, vector end)"},// #428 te_lightning1
	{"te_lightning2",	PF_sv_te_lightning2,PF_cl_te_lightning2,429,	PF_NoMenu, "void(entity own, vector start, vector end)"},// #429 te_lightning2
	{"te_lightning3",	PF_sv_te_lightning3,PF_cl_te_lightning3,430,	PF_NoMenu, "void(entity own, vector start, vector end)"},// #430 te_lightning3
	{"te_beam",			PF_sv_te_beam,		PF_cl_te_beam,		431,	PF_NoMenu, "void(entity own, vector start, vector end)"},// #431 te_beam
	{"vectorvectors",	PF_vectorvectors,	PF_vectorvectors,	432,	PF_NoMenu, "void(vector dir)"},// (DP_QC_VECTORVECTORS)
	{"te_plasmaburn",	PF_sv_te_plasmaburn,NULL,				433,	PF_NoMenu, "void(vector org)", "stub."},// (DP_TE_PLASMABURN)
	{"getsurfacenumpoints",PF_getsurfacenumpoints,PF_getsurfacenumpoints,434,PF_NoMenu, "float(entity e, float s)"},// (DP_QC_GETSURFACE)
	{"getsurfacepoint",	PF_getsurfacepoint,	PF_getsurfacepoint,	435,	PF_NoMenu, "vector(entity e, float s, float n)"},// (DP_QC_GETSURFACE)
	{"getsurfacenormal",PF_getsurfacenormal,PF_getsurfacenormal,436,	PF_NoMenu, "vector(entity e, float s)"},// (DP_QC_GETSURFACE)
	{"getsurfacetexture",PF_getsurfacetexture,PF_getsurfacetexture,437,	PF_NoMenu, "string(entity e, float s)"},// (DP_QC_GETSURFACE)
	{"getsurfacenearpoint",PF_getsurfacenearpoint,PF_getsurfacenearpoint,438,PF_NoMenu, "float(entity e, vector p)"},// (DP_QC_GETSURFACE)
	{"getsurfaceclippedpoint",PF_getsurfaceclippedpoint,PF_getsurfaceclippedpoint,439,PF_NoMenu, "vector(entity e, float s, vector p)"},// (DP_QC_GETSURFACE)
	{"clientcommand",	PF_clientcommand,	PF_NoCSQC,			440,	PF_NoMenu, "void(entity e, string s)"},// (KRIMZON_SV_PARSECLIENTCOMMAND)
	{"tokenize",		PF_Tokenize,		PF_Tokenize,		441,	PF_Tokenize,441, "float(string s)"},// (KRIMZON_SV_PARSECLIENTCOMMAND)
	{"argv",			PF_ArgV,			PF_ArgV,			442,	PF_ArgV,59, "string(float n)"},// (KRIMZON_SV_PARSECLIENTCOMMAND
	{"argc",			PF_ArgC,			PF_ArgC,			0,		PF_ArgC,0, "float()"},
	{"setattachment",	PF_setattachment,	PF_setattachment,	443,	PF_NoMenu, "void(entity e, entity tagentity, string tagname)", ""},// (DP_GFX_QUAKE3MODELTAGS)
	{"search_begin",	PF_search_begin,	PF_search_begin,	444,	PF_search_begin,74, "searchhandle(string pattern, float flags, float quiet, optional string filterpackage)", "initiate a filesystem scan based upon filenames. Be sure to call search_end on the returned handle."},
	{"search_end",		PF_search_end,		PF_search_end,		445,	PF_search_end,75, "void(searchhandle handle)", ""},
	{"search_getsize",	PF_search_getsize,	PF_search_getsize,	446,	PF_search_getsize,76, "float(searchhandle handle)", " Retrieves the number of files that were found."},
	{"search_getfilename",PF_search_getfilename,PF_search_getfilename,447,PF_search_getfilename,77, "string(searchhandle handle, float num)", "Retrieves name of one of the files that was found by the initial search."},
	{"search_getfilesize",PF_search_getfilesize,PF_search_getfilesize,0,PF_search_getfilesize,0, "float(searchhandle handle, float num)", "Retrieves the size of one of the files that was found by the initial search."},
	{"search_getfilemtime",PF_search_getfilemtime,PF_search_getfilemtime,0,PF_search_getfilemtime,0, "string(searchhandle handle, float num)", "Retrieves modification time of one of the files in %Y-%m-%d %H:%M:%S format."},
	{"search_getpackagename",PF_search_getpackagename,PF_search_getpackagename,0,PF_search_getpackagename,0, "string(searchhandle handle, float num)", "Retrieves the name of the package the file was found inside."},
	{"cvar_string",		PF_cvar_string,		PF_cvar_string,		448,	PF_cvar_string,71, "string(string cvarname)"},//DP_QC_CVAR_STRING
	{"findflags",		PF_findflags,		PF_findflags,		449,	PF_findflags,0, "entity(entity start, .float fld, float match)"},//DP_QC_FINDFLAGS
	{"findchainflags",	PF_findchainflags,	PF_findchainflags,	450,	PF_NoMenu, "entity(.float fld, float match, optional .entity chainfield)"},//DP_QC_FINDCHAINFLAGS
	{"dropclient",		PF_dropclient,		PF_NoCSQC,			453,	PF_NoMenu, "void(entity player)"},//DP_SV_BOTCLIENT
	{"spawnclient",		PF_spawnclient,		PF_NoCSQC,			454,	PF_NoMenu, "entity()", "Spawns a dummy player entity.\nNote that such dummy players will be carried from one map to the next.\nWarning: DP_SV_CLIENTCOLORS DP_SV_CLIENTNAME are not implemented in quakespasm, so use KRIMZON_SV_PARSECLIENTCOMMAND's clientcommand builtin to change the bot's name/colours/skin/team/etc, in the same way that clients would ask."},//DP_SV_BOTCLIENT
	{"clienttype",		PF_clienttype,		PF_NoCSQC,			455,	PF_NoMenu, "float(entity client)"},//botclient
	{"WriteUnterminatedString",PF_WriteString2,PF_NoCSQC,		456,	PF_NoMenu, "void(float target, string str)"},	//writestring but without the null terminator. makes things a little nicer.
	{"edict_num",		PF_edict_for_num,	PF_edict_for_num,	459,	PF_NoMenu, "entity(float entnum)"},//DP_QC_EDICT_NUM
	{"buf_create",		PF_buf_create,		PF_buf_create,		460,	PF_buf_create,440, "strbuf()"},//DP_QC_STRINGBUFFERS
	{"buf_del",			PF_buf_del,			PF_buf_del,			461,	PF_buf_del,461, "void(strbuf bufhandle)"},//DP_QC_STRINGBUFFERS
	{"buf_getsize",		PF_buf_getsize,		PF_buf_getsize,		462,	PF_buf_getsize,442, "float(strbuf bufhandle)"},//DP_QC_STRINGBUFFERS
	{"buf_copy",		PF_buf_copy,		PF_buf_copy,		463,	PF_buf_copy,443, "void(strbuf bufhandle_from, strbuf bufhandle_to)"},//DP_QC_STRINGBUFFERS
	{"buf_sort",		PF_buf_sort,		PF_buf_sort,		464,	PF_buf_sort,444, "void(strbuf bufhandle, float sortprefixlen, float backward)"},//DP_QC_STRINGBUFFERS
	{"buf_implode",		PF_buf_implode,		PF_buf_implode,		465,	PF_buf_implode,445, "string(strbuf bufhandle, string glue)"},//DP_QC_STRINGBUFFERS
	{"bufstr_get",		PF_bufstr_get,		PF_bufstr_get,		466,	PF_bufstr_get,446, "string(strbuf bufhandle, float string_index)"},//DP_QC_STRINGBUFFERS
	{"bufstr_set",		PF_bufstr_set,		PF_bufstr_set,		467,	PF_bufstr_set,447, "void(strbuf bufhandle, float string_index, string str)"},//DP_QC_STRINGBUFFERS
	{"bufstr_add",		PF_bufstr_add,		PF_bufstr_add,		468,	PF_bufstr_add,448, "float(strbuf bufhandle, string str, float order)"},//DP_QC_STRINGBUFFERS
	{"bufstr_free",		PF_bufstr_free,		PF_bufstr_free,		469,	PF_bufstr_free,449, "void(strbuf bufhandle, float string_index)"},//DP_QC_STRINGBUFFERS

	{"asin",			PF_asin,			PF_asin,			471,	PF_asin,471, "float(float s)"},//DP_QC_ASINACOSATANATAN2TAN
	{"acos",			PF_acos,			PF_acos,			472,	PF_acos,472, "float(float c)"},//DP_QC_ASINACOSATANATAN2TAN
	{"atan",			PF_atan,			PF_atan,			473,	PF_atan,473, "float(float t)"},//DP_QC_ASINACOSATANATAN2TAN
	{"atan2",			PF_atan2,			PF_atan2,			474,	PF_atan2,474, "float(float c, float s)"},//DP_QC_ASINACOSATANATAN2TAN
	{"tan",				PF_tan,				PF_tan,				475,	PF_tan,475, "float(float a)"},//DP_QC_ASINACOSATANATAN2TAN
	{"strlennocol",		PF_strlennocol,		PF_strlennocol,		476,	PF_strlennocol,476, D("float(string s)", "Returns the number of characters in the string after any colour codes or other markup has been parsed.")},//DP_QC_STRINGCOLORFUNCTIONS
	{"strdecolorize",	PF_strdecolorize,	PF_strdecolorize,	477,	PF_strdecolorize,477, D("string(string s)", "Flattens any markup/colours, removing them from the string.")},//DP_QC_STRINGCOLORFUNCTIONS
	{"strftime",		PF_strftime,		PF_strftime,		478,	PF_strftime,478, "string(float uselocaltime, string format, ...)"},	//DP_QC_STRFTIME
	{"tokenizebyseparator",PF_tokenizebyseparator,PF_tokenizebyseparator,479,PF_tokenizebyseparator,479, "float(string s, string separator1, ...)"},	//DP_QC_TOKENIZEBYSEPARATOR
	{"strtolower",		PF_strtolower,		PF_strtolower,		480,	PF_strtolower,480, "string(string s)"},	//DP_QC_STRING_CASE_FUNCTIONS
	{"strtoupper",		PF_strtoupper,		PF_strtoupper,		481,	PF_strtoupper,481, "string(string s)"},	//DP_QC_STRING_CASE_FUNCTIONS
	{"cvar_defstring",	PF_cvar_defstring,	PF_cvar_defstring,	482,	PF_cvar_defstring,89, "string(string s)"},	//DP_QC_CVAR_DEFSTRING
	{"pointsound",		PF_sv_pointsound,	PF_cl_pointsound,	483,	PF_NoMenu, "void(vector origin, string sample, float volume, float attenuation)"},//DP_SV_POINTSOUND
	{"strreplace",		PF_strreplace,		PF_strreplace,		484,	PF_strreplace,484, "string(string search, string replace, string subject)"},//DP_QC_STRREPLACE
	{"strireplace",		PF_strireplace,		PF_strireplace,		485,	PF_strireplace,485, "string(string search, string replace, string subject)"},//DP_QC_STRREPLACE
	{"getsurfacepointattribute",PF_getsurfacepointattribute,PF_getsurfacepointattribute,				486,	PF_NoMenu, "vector(entity e, float s, float n, float a)"},//DP_QC_GETSURFACEPOINTATTRIBUTE

	{"crc16",			PF_crc16,			PF_crc16,			494,	PF_crc16,494, "float(float caseinsensitive, string s, ...)"},//DP_QC_CRC16
	{"cvar_type",		PF_cvar_type,		PF_cvar_type,		495,	PF_cvar_type,495, "float(string name)"},//DP_QC_CVAR_TYPE
	{"numentityfields",	PF_numentityfields,	PF_numentityfields,	496,	PF_numentityfields,496, D("float()", "Gives the number of named entity fields. Note that this is not the size of an entity, but rather just the number of unique names (ie: vectors use 4 names rather than 3).")},//DP_QC_ENTITYDATA
	{"findentityfield",	PF_findentityfield,	PF_findentityfield,	0,		PF_findentityfield,0, D("float(string fieldname)", "Find a field index by name.")},
	{"entityfieldref",	PF_entityfieldref,	PF_entityfieldref,	0,		PF_entityfieldref,0, D("typedef .__variant field_t;\nfield_t(float fieldnum)", "Returns a field value that can be directly used to read entity fields. Be sure to validate the type with entityfieldtype before using.")},//DP_QC_ENTITYDATA
	{"entityfieldname",	PF_entityfieldname,	PF_entityfieldname,	497,	PF_entityfieldname,497, D("string(float fieldnum)", "Retrieves the name of the given entity field.")},//DP_QC_ENTITYDATA
	{"entityfieldtype",	PF_entityfieldtype,	PF_entityfieldtype,	498,	PF_entityfieldtype,498, D("float(float fieldnum)", "Provides information about the type of the field specified by the field num. Returns one of the EV_ values.")},//DP_QC_ENTITYDATA
	{"getentityfieldstring",PF_getentfldstr,PF_getentfldstr,	499,	PF_getentfldstr,499, "string(float fieldnum, entity ent)"},//DP_QC_ENTITYDATA
	{"putentityfieldstring",PF_putentfldstr,PF_putentfldstr,	500,	PF_putentfldstr,500, "float(float fieldnum, entity ent, string s)"},//DP_QC_ENTITYDATA
	{"whichpack",		PF_whichpack,		PF_whichpack,		503,	PF_whichpack,503, D("string(string filename, optional float makereferenced)", "Returns the pak file name that contains the file specified. progs/player.mdl will generally return something like 'pak0.pak'. If makereferenced is true, clients will automatically be told that the returned package should be pre-downloaded and used, even if allow_download_refpackages is not set.")},//DP_QC_WHICHPACK
	{"getentity",		PF_NoSSQC,			PF_cl_getrenderentity,504,	PF_NoMenu, D("__variant(float entnum, float fieldnum)", "Looks up fields from non-csqc-visible entities. The entity will need to be within the player's pvs. fieldnum should be one of the GE_ constants.")},//DP_CSQC_QUERYRENDERENTITY
	{"uri_escape",		PF_uri_escape,		PF_uri_escape,		510,	PF_uri_escape,510, "string(string in)"},//DP_QC_URI_ESCAPE
	{"uri_unescape",	PF_uri_unescape,	PF_uri_unescape,	511,	PF_uri_unescape,511, "string(string in)"},//DP_QC_URI_ESCAPE
	{"num_for_edict",	PF_num_for_edict,	PF_num_for_edict,	512,	PF_num_for_edict,512, "float(entity ent)"},//DP_QC_NUM_FOR_EDICT
	{"uri_get",			PF_uri_get,			PF_uri_get,			513,	PF_uri_get,513, "float(string uril, float id, optional string postmimetype, optional string postdata)", "stub."},//DP_QC_URI_GET
	{"tokenize_console",PF_tokenize_console,PF_tokenize_console,514,	PF_tokenize_console,514, D("float(string str)", "Tokenize a string exactly as the console's tokenizer would do so. The regular tokenize builtin became bastardized for convienient string parsing, which resulted in a large disparity that can be exploited to bypass checks implemented in a naive SV_ParseClientCommand function, therefore you can use this builtin to make sure it exactly matches.")},
	{"argv_start_index",PF_argv_start_index,PF_argv_start_index,515,	PF_argv_start_index,515, D("float(float idx)", "Returns the character index that the tokenized arg started at.")},
	{"argv_end_index",	PF_argv_end_index,	PF_argv_end_index,	516,	PF_argv_end_index,516, D("float(float idx)", "Returns the character index that the tokenized arg stopped at.")},
	{"buf_cvarlist",	PF_buf_cvarlist,	PF_buf_cvarlist,	517,	PF_buf_cvarlist,517, D("void(strbuf strbuf, string pattern, string antipattern)", "Populates the strbuf with a list of known cvar names.")},
	{"cvar_description",PF_cvar_description,PF_cvar_description,518,	PF_cvar_description,518, D("string(string cvarname)", "Retrieves the description of a cvar, which might be useful for tooltips or help files. This may still not be useful.")},
	{"gettime",			PF_gettime,			PF_gettime,			519,	PF_gettime,519, "float(optional float timetype)"},
	{"findkeysforcommand",PF_NoSSQC,		PF_cl_findkeysforcommand,521,PF_cl_findkeysforcommand,610, D("string(string command, optional float bindmap)", "Returns a list of keycodes that perform the given console command in a format that can only be parsed via tokenize (NOT tokenize_console). This always returns at least two values - if only one key is actually bound, -1 will be returned. The bindmap argument is listed for compatibility with dp-specific defs, but is ignored in FTE.")},
	{"findkeysforcommandex",PF_NoSSQC,		PF_cl_findkeysforcommandex,0,PF_cl_findkeysforcommandex,0, D("string(string command, optional float bindmap)", "Returns a list of key bindings in keyname format instead of keynums. Use tokenize to parse. This list may contain modifiers. May return large numbers of keys.")},
//	{"loadfromdata",	PF_loadfromdata,	PF_loadfromdata,	529,	PF_NoMenu, D("void(string s)", "Reads a set of entities from the given string. This string should have the same format as a .ent file or a saved game. Entities will be spawned as required. If you need to see the entities that were created, you should use parseentitydata instead.")},
//	{"loadfromfile",	PF_loadfromfile,	PF_loadfromfile,	530,	PF_NoMenu, D("void(string s)", "Reads a set of entities from the named file. This file should have the same format as a .ent file or a saved game. Entities will be spawned as required. If you need to see the entities that were created, you should use parseentitydata instead.")},
	{"log",				PF_Logarithm,		PF_Logarithm,		532,	PF_Logarithm,532, D("float(float v, optional float base)", "Determines the logarithm of the input value according to the specified base. This can be used to calculate how much something was shifted by.")},
	{"soundlength",		PF_NoSSQC,			PF_cl_soundlength,	534,	PF_cl_soundlength,534, D("float(string sample)", "Provides a way to query the duration of a sound sample, allowing you to set up a timer to chain samples.")},
	{"buf_loadfile",	PF_buf_loadfile,	PF_buf_loadfile,	535,	PF_buf_loadfile,535, D("float(string filename, strbuf bufhandle)", "Appends the named file into a string buffer (which must have been created in advance). The return value merely says whether the file was readable.")},
	{"buf_writefile",	PF_buf_writefile,	PF_buf_writefile,	536,	PF_buf_writefile,536, D("float(filestream filehandle, strbuf bufhandle, optional float startpos, optional float numstrings)", "Writes the contents of a string buffer onto the end of the supplied filehandle (you must have already used fopen). Additional optional arguments permit you to constrain the writes to a subsection of the stringbuffer.")},
	//{"bufstr_find",	PF_bufstr_find,		PF_bufstr_find,		537,	PF_bufstr_find,537, D("float(strbuf bufhandle, string match, float matchtype=5, optional float firstidx=0, optional float step=1)", "Finds the first occurence of the requested string, returning its index (or -1).")},

	{"setkeydest",		PF_NoSSQC,			PF_NoCSQC,			601,	PF_m_setkeydest,601, D("void(float dest)", "Grab key focus")},
	{"getkeydest",		PF_NoSSQC,			PF_NoCSQC,			602,	PF_m_getkeydest,602, D("float()", "Returns key focus")},
	{"setmousetarget",	PF_NoSSQC,			PF_NoCSQC,			603,	PF_m_setmousetarget,603, D("void(float dest)", "Grab mouse focus")},
	{"getmousetarget",	PF_NoSSQC,			PF_NoCSQC,			604,	PF_m_getmousetarget,604, D("float()", "Returns mouse focus")},

	{"callfunction",	PF_callfunction,	PF_callfunction,			605,PF_callfunction,			605, D("void(.../*, string funcname*/)", "Invokes the named function. The function name is always passed as the last parameter and must always be present. The others are passed to the named function as-is")},
	{"writetofile",		PF_writetofile,		PF_writetofile,				606,PF_writetofile,				606, D("void(filestream fh, entity e)", "Writes an entity's fields to a frik_file file handle.")},
	{"isfunction",		PF_isfunction,		PF_isfunction,				607,PF_isfunction,				607, D("float(string s)", "Returns true if the named function exists and can be called with the callfunction builtin.")},
	{"getresolution",			PF_NoSSQC,	PF_cl_getresolution,		608,PF_cl_getresolution,		608, D("vector(float mode, float forfullscreen)", "Returns available/common video modes.")},
	{"gethostcachevalue",		PF_NoSSQC,	PF_gethostcachevalue,		611,PF_gethostcachevalue,		611, D("float(float type)", "Returns number of servers known")},
	{"gethostcachestring",		PF_NoSSQC,	PF_gethostcachestring,		612,PF_gethostcachestring,		612, D("string(float type, float hostnr)", "Retrieves some specific type of info about the specified server.")},
	{"parseentitydata",	PF_parseentitydata, PF_parseentitydata,			613,PF_parseentitydata,			613, D("float(entity e, string s, optional float offset)", "Reads a single entity's fields into an already-spawned entity. s should contain field pairs like in a saved game: {\"foo1\" \"bar\" \"foo2\" \"5\"}. Returns <=0 on failure, otherwise returns the offset in the string that was read to.")},
//	{"generateentitydata",PF_generateentitydata,PF_generateentitydata,	0,	PF_generateentitydata,		0,	 D("string(entity e)", "Dumps the entities fields into a string which can later be parsed with parseentitydata.")},
	{"resethostcachemasks",		PF_NoSSQC,	PF_resethostcachemasks,		615,PF_resethostcachemasks,		615, D("void()", "Resets server listing filters.")},
	{"sethostcachemaskstring",	PF_NoSSQC,	PF_sethostcachemaskstring,	616,PF_sethostcachemaskstring,	616, D("void(float mask, float fld, string str, float op)", "stub. Changes a serverlist filter (string comparisons).")},
	{"sethostcachemasknumber",	PF_NoSSQC,	PF_sethostcachemasknumber,	617,PF_sethostcachemasknumber,	617, D("void(float mask, float fld, float num, float op)", "stub. Changes a serverlist filter (numeric comparisons).")},
	{"resorthostcache",			PF_NoSSQC,	PF_resorthostcache,			618,PF_resorthostcache,			618, D("void()", "Rebuild the visible server list according to filters and sort terms")},
	{"sethostcachesort",		PF_NoSSQC,	PF_sethostcachesort,		619,PF_sethostcachesort,		619, D("void(float fld, float descending)", "Changes which field to sort by.")},
	{"refreshhostcache",		PF_NoSSQC,	PF_refreshhostcache,		620,PF_refreshhostcache,		620, D("void(optional float dopurge)", "Refresh all sources, requery maps, etc.")},
	{"gethostcachenumber",		PF_NoSSQC,	PF_gethostcachenumber,		621,PF_gethostcachenumber,		621, D("float(float fld, float hostnr)", "Alternative to gethostcachestring, avoiding tempstrings.")},
	{"gethostcacheindexforkey",	PF_NoSSQC,	PF_gethostcacheindexforkey,	622,PF_gethostcacheindexforkey,	622, D("float(string key)", "Retrieves a field index for the provided name")},
	{"addwantedhostcachekey",	PF_NoSSQC,	PF_gethostcacheindexforkey,	623,PF_gethostcacheindexforkey,	623, D("void(string key)", "stub.")},
//	{"getextresponse",			PF_NoSSQC,	PF_NoCSQC,					634,PF_getextresponse,			624, D("string()", "stub. Reads from the 'extResponse' queue.")},
//	{"netaddress_resolve",		PF_NoSSQC,	PF_NoCSQC,					635,PF_netaddress_resolve,		625, D("string(string dnsname)", "stub. Looks up a name in dns, returning the resulting ip address in string form.")},


//	{"generateentitydata",PF_generateentitydata,NULL,				0,	PF_NoMenu, D("string(entity e)", "Dumps the entities fields into a string which can later be parsed with parseentitydata."}),
	{"getgamedirinfo",	PF_NoSSQC,			PF_getgamedirinfo,			626,	PF_getgamedirinfo,626, D("string(float n, float prop)", "Provides a way to enumerate available mod directories.")},
	{"sprintf",			PF_sprintf,			PF_sprintf,					627,	PF_sprintf,627, "string(string fmt, ...)"},
	{"getsurfacenumtriangles",PF_getsurfacenumtriangles,PF_getsurfacenumtriangles,628,PF_NoMenu, "float(entity e, float s)"},
	{"getsurfacetriangle",PF_getsurfacetriangle,PF_getsurfacetriangle,	629,	PF_NoMenu, "vector(entity e, float s, float n)"},
	{"setkeybind",		PF_NoSSQC,			PF_cl_setkeybind,			630,	PF_cl_setkeybind,630, "float(float key, string bind, optional float bindmap, optional float modifier)", "Changes a key binding."},
	{"getbindmaps",		PF_NoSSQC,			PF_cl_getbindmaps,			631,	PF_cl_getbindmaps,631, "vector()", "stub."},
	{"setbindmaps",		PF_NoSSQC,			PF_cl_setbindmaps,			632,	PF_cl_setbindmaps,632, "float(vector bm)", "stub."},
	{"digest_hex",		PF_digest_hex,		PF_digest_hex,				639,	PF_digest_hex, 639, "string(string digest, string data, ...)"},
};

qboolean PR_Can_Particles(unsigned int prot, unsigned int pext1, unsigned int pext2)
{
	if (prot == PROTOCOL_VERSION_DP7)
		return true;	//a bit different, but works
	else if (pext2 || (pext1&PEXT1_CSQC))
		return true;	//a bit different, but works
	else
		return false;	//sorry. don't report it as supported.
}
qboolean PR_Can_Ent_Alpha(unsigned int prot, unsigned int pext1, unsigned int pext2)
{
	if (prot != PROTOCOL_NETQUAKE)
		return true;	//most base protocols support it
	else if (pext2 & PEXT2_REPLACEMENTDELTAS)
		return true;	//as does fte's extensions
	else
		return false;	//sorry. don't report it as supported.
}
qboolean PR_Can_Ent_ColorMod(unsigned int prot, unsigned int pext1, unsigned int pext2)
{
	if (prot == PROTOCOL_VERSION_DP7)
		return true;	//dpp7 supports it
	else if (pext2 & PEXT2_REPLACEMENTDELTAS)
		return true;	//as does fte's extensions
	else
		return false;	//sorry. don't report it as supported.
}
qboolean PR_Can_Ent_Scale(unsigned int prot, unsigned int pext1, unsigned int pext2)
{
	if (prot == PROTOCOL_RMQ || prot == PROTOCOL_VERSION_DP7)
		return true;	//some base protocols support it
	else if (pext2 & PEXT2_REPLACEMENTDELTAS)
		return true;	//as does fte's extensions
	else
		return false;	//sorry. don't report it as supported.
}
static struct
{
	const char *name;
	qboolean (*checkextsupported)(unsigned int prot, unsigned int pext1, unsigned int pext2);
} qcextensions[] =
{
	{"DP_CON_SET"},
	{"DP_CON_SETA"},
	{"DP_CSQC_QUERYRENDERENTITY"},
	{"DP_EF_NOSHADOW"},
	{"DP_ENT_ALPHA",			PR_Can_Ent_Alpha},	//already in quakespasm, supposedly.
	{"DP_ENT_COLORMOD",			PR_Can_Ent_ColorMod},
	{"DP_ENT_SCALE",			PR_Can_Ent_Scale},
	{"DP_ENT_TRAILEFFECTNUM",	PR_Can_Particles},
	//{"DP_GFX_QUAKE3MODELTAGS"}, //we support attachments but no md3/iqm/tags, so we can't really advertise this (although the builtin is complete if you ignore the lack of md3/iqms/tags)
	{"DP_INPUTBUTTONS"},
	{"DP_QC_AUTOCVARS"},	//they won't update on changes
	{"DP_QC_ASINACOSATANATAN2TAN"},
	{"DP_QC_COPYENTITY"},
	{"DP_QC_CRC16"},
	//{"DP_QC_DIGEST"},
	{"DP_QC_CVAR_DEFSTRING"},
	{"DP_QC_CVAR_STRING"},
	{"DP_QC_CVAR_TYPE"},
	{"DP_QC_EDICT_NUM"},
	{"DP_QC_ENTITYDATA"},
	{"DP_QC_ETOS"},
	{"DP_QC_FINDCHAIN"},
	{"DP_QC_FINDCHAINFLAGS"},
	{"DP_QC_FINDCHAINFLOAT"},
	{"DP_QC_FINDFLAGS"},
	{"DP_QC_FINDFLOAT"},
	{"DP_QC_GETLIGHT"},
	{"DP_QC_GETSURFACE"},
	{"DP_QC_GETSURFACETRIANGLE"},
	{"DP_QC_GETSURFACEPOINTATTRIBUTE"},
	{"DP_QC_MINMAXBOUND"},
	{"DP_QC_MULTIPLETEMPSTRINGS"},
	{"DP_QC_RANDOMVEC"},
	{"DP_QC_RENDER_SCENE"},	//meaningful for menuqc
	{"DP_QC_SINCOSSQRTPOW"},
	{"DP_QC_SPRINTF"},
	{"DP_QC_STRFTIME"},
	{"DP_QC_STRING_CASE_FUNCTIONS"},
	{"DP_QC_STRINGBUFFERS"},
	{"DP_QC_STRINGCOLORFUNCTIONS"},
	{"DP_QC_STRREPLACE"},
	{"DP_QC_TOKENIZEBYSEPARATOR"},
	{"DP_QC_TRACEBOX"},
	{"DP_QC_TRACETOSS"},
	{"DP_QC_TRACE_MOVETYPES"},
	{"DP_QC_URI_ESCAPE"},
	{"DP_QC_VECTOANGLES_WITH_ROLL"},
	{"DP_QC_VECTORVECTORS"},
	{"DP_QC_WHICHPACK"},
	{"DP_VIEWZOOM"},
	{"DP_REGISTERCVAR"},
	{"DP_SV_BOTCLIENT"},
	{"DP_SV_DROPCLIENT"},
//	{"DP_SV_POINTPARTICLES",	PR_Can_Particles},	//can't enable this, because certain mods then assume that we're DP and all the particles break.
	{"DP_SV_POINTSOUND"},
	{"DP_SV_PRINT"},
	{"DP_SV_SETCOLOR"},
	{"DP_SV_SPAWNFUNC_PREFIX"},
	{"DP_SV_WRITEUNTERMINATEDSTRING"},
//	{"DP_TE_BLOOD",				PR_Can_Particles},
#ifdef PSET_SCRIPT
	{"DP_TE_PARTICLERAIN",		PR_Can_Particles},
	{"DP_TE_PARTICLESNOW",		PR_Can_Particles},
#endif
	{"DP_TE_STANDARDEFFECTBUILTINS"},
	{"EXT_BITSHIFT"},
	{"FRIK_FILE"},				//lacks the file part, but does have the strings part.
	{"FTE_CSQC_SERVERBROWSER"},	//callable from csqc too, for feature parity.
	{"FTE_ENT_SKIN_CONTENTS"},	//SOLID_BSP&&skin==CONTENTS_FOO changes CONTENTS_SOLID to CONTENTS_FOO, allowing you to swim in moving ents without qc hacks, as well as correcting view cshifts etc.
#ifdef PSET_SCRIPT
	{"FTE_PART_SCRIPT"},
	{"FTE_PART_NAMESPACES"},
#ifdef PSET_SCRIPT_EFFECTINFO
	{"FTE_PART_NAMESPACE_EFFECTINFO"},
#endif
#endif
	{"FTE_QC_CHECKCOMMAND"},
	{"FTE_QC_CROSSPRODUCT"},
	{"FTE_QC_HARDWARECURSORS"},
	{"FTE_QC_INFOKEY"},
	{"FTE_QC_INTCONV"},
	{"FTE_QC_MULTICAST"},
	{"FTE_STRINGS"},
#ifdef PSET_SCRIPT
	{"FTE_SV_POINTPARTICLES",	PR_Can_Particles},
#endif
	{"KRIMZON_SV_PARSECLIENTCOMMAND"},
	{"ZQ_QC_STRINGS"},
};

static void PF_checkextension(void)
{
	const char *extname = G_STRING(OFS_PARM0);
	unsigned int i;
	cvar_t *v;
	char *cvn;
	for (i = 0; i < countof(qcextensions); i++)
	{
		if (!strcmp(extname, qcextensions[i].name))
		{
			if (qcextensions[i].checkextsupported)
			{
				unsigned int prot, pext1, pext2;
				extern unsigned int sv_protocol;
				extern unsigned int sv_protocol_pext1;
				extern unsigned int sv_protocol_pext2;
				extern cvar_t cl_nopext;
				if (sv.active || qcvm == &sv.qcvm)
				{	//server or client+server
					prot = sv_protocol;
					pext1 = sv_protocol_pext1;
					pext2 = sv_protocol_pext2;

					//if the server seems to be set up for singleplayer then filter by client settings. otherwise just assume the best.
					if (!isDedicated && svs.maxclients == 1 && cl_nopext.value)
						pext1 = pext2 = 0;
				}
				else if (cls.state == ca_connected)
				{	//client only (or demo)
					prot = cl.protocol;
					pext1 = cl.protocol_pext1;
					pext2 = cl.protocol_pext2;
				}
				else
				{	//menuqc? ooer
					prot = 0;
					pext1 = 0;
					pext2 = 0;
				}
				if (!qcextensions[i].checkextsupported(prot, pext1, pext2))
				{
					if (!pr_checkextension.value)
						Con_Printf("Mod queried extension %s, but not enabled\n", extname);
					G_FLOAT(OFS_RETURN) = false;
					return;
				}
			}

			cvn = va("pr_ext_%s", qcextensions[i].name);
			for (i = 0; cvn[i]; i++)
				if (cvn[i] >= 'A' && cvn[i] <= 'Z')
					cvn[i] = 'a' + (cvn[i]-'A');
			v = Cvar_Create(cvn, "1");
			if (v && !v->value)
			{
				if (!pr_checkextension.value)
					Con_Printf("Mod queried extension %s, but blocked by cvar\n", extname);
				G_FLOAT(OFS_RETURN) = false;
				return;
			}
			if (!pr_checkextension.value)
				Con_Printf("Mod found extension %s\n", extname);
			G_FLOAT(OFS_RETURN) = true;
			return;
		}
	}
	if (!pr_checkextension.value)
		Con_DPrintf("Mod tried extension %s\n", extname);
	G_FLOAT(OFS_RETURN) = false;
}

static void PF_builtinsupported(void)
{
	const char *biname = G_STRING(OFS_PARM0);
	unsigned int i;
	for (i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]); i++)
	{
		if (!strcmp(extensionbuiltins[i].name, biname))
		{
			if (qcvm == &cls.menu_qcvm)
				G_FLOAT(OFS_RETURN) = extensionbuiltins[i].number_menuqc;
			else
				G_FLOAT(OFS_RETURN) = extensionbuiltins[i].number;
			return;
		}
	}
	G_FLOAT(OFS_RETURN) = 0;
}


static void PF_checkbuiltin (void)
{
	func_t funcref = G_INT(OFS_PARM0);
	if ((unsigned int)funcref < (unsigned int)qcvm->progs->numfunctions)
	{
		dfunction_t *fnc = &qcvm->functions[(unsigned int)funcref];
//		const char *funcname = PR_GetString(fnc->s_name);
		int binum = -fnc->first_statement;
		unsigned int i;

		//qc defines the function at least. nothing weird there...
		if (binum > 0 && binum < qcvm->numbuiltins)
		{
			if (qcvm->builtins[binum] == PF_Fixme)
			{
				G_FLOAT(OFS_RETURN) = false;	//the builtin with that number isn't defined.
				if (qcvm == &cls.menu_qcvm)
				{
					for (i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]); i++)
					{
						if (extensionbuiltins[i].number_menuqc == binum)
						{	//but it will be defined if its actually executed.
							if (extensionbuiltins[i].desc && !strncmp(extensionbuiltins[i].desc, "stub.", 5))
								G_FLOAT(OFS_RETURN) = false;	//pretend it won't work if it probably won't be useful
							else if (!extensionbuiltins[i].menufunc)
								G_FLOAT(OFS_RETURN) = false;	//works, but not in this module
							else
								G_FLOAT(OFS_RETURN) = true;
							break;
						}
					}
				}
				else
				{
					for (i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]); i++)
					{
						if (extensionbuiltins[i].number == binum)
						{	//but it will be defined if its actually executed.
							if (extensionbuiltins[i].desc && !strncmp(extensionbuiltins[i].desc, "stub.", 5))
								G_FLOAT(OFS_RETURN) = false;	//pretend it won't work if it probably won't be useful
							else if ((qcvm == &cl.qcvm && !extensionbuiltins[i].csqcfunc)
								 || (qcvm == &sv.qcvm && !extensionbuiltins[i].ssqcfunc))
								G_FLOAT(OFS_RETURN) = false;	//works, but not in this module
							else
								G_FLOAT(OFS_RETURN) = true;
							break;
						}
					}
				}
			}
			else
			{
				G_FLOAT(OFS_RETURN) = true;		//its defined, within the sane range, mapped, everything. all looks good.
				//we should probably go through the available builtins and validate that the qc's name matches what would be expected
				//this is really intended more for builtins defined as #0 though, in such cases, mismatched assumptions are impossible.
			}
		}
		else
			G_FLOAT(OFS_RETURN) = false;	//not a valid builtin (#0 builtins get remapped at load, even if the builtin is activated then)
	}
	else
	{	//not valid somehow.
		G_FLOAT(OFS_RETURN) = false;
	}
}

void PF_Fixme (void)
{
	//interrogate the vm to try to figure out exactly which builtin they just tried to execute.
	dstatement_t *st = &qcvm->statements[qcvm->xstatement];
	eval_t *glob = (eval_t*)&qcvm->globals[st->a];
	if ((unsigned int)glob->function < (unsigned int)qcvm->progs->numfunctions)
	{
		dfunction_t *fnc = &qcvm->functions[(unsigned int)glob->function];
		const char *funcname = PR_GetString(fnc->s_name);
		int binum = -fnc->first_statement;
		unsigned int i;
		if (binum >= 0)
		{
			//find an extension with the matching number
			for (i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]); i++)
			{
				int num = (qcvm == &cls.menu_qcvm?extensionbuiltins[i].number_menuqc:extensionbuiltins[i].number);
				if (num == binum)
				{	//set it up so we're faster next time
					builtin_t bi = NULL;
					if (qcvm == &sv.qcvm)
						bi = extensionbuiltins[i].ssqcfunc;
					else if (qcvm == &cl.qcvm)
						bi = extensionbuiltins[i].csqcfunc;
					else if (qcvm == &cls.menu_qcvm)
						bi = extensionbuiltins[i].menufunc;
					if (!bi)
						continue;

					num = (qcvm == &cls.menu_qcvm?extensionbuiltins[i].documentednumber_menuqc:extensionbuiltins[i].documentednumber);
					if (!pr_checkextension.value || (extensionbuiltins[i].desc && !strncmp(extensionbuiltins[i].desc, "stub.", 5)))
						Con_Warning("Mod is using builtin #%u - %s\n", num, extensionbuiltins[i].name);
					else
						Con_DPrintf2("Mod uses builtin #%u - %s\n", num, extensionbuiltins[i].name);
					qcvm->builtins[binum] = bi;
					qcvm->builtins[binum]();
					return;
				}
			}

			PR_RunError ("unimplemented builtin #%i - %s", binum, funcname);
		}
	}
	PR_RunError ("PF_Fixme: not a builtin...");
}


//called at map end
void PR_ShutdownExtensions(void)
{
	PR_UnzoneAll();
	PF_frikfile_shutdown();
	PF_search_shutdown();
	PF_buf_shutdown();
	tokenize_flush();
	if (qcvm == &cl.qcvm)
		PR_ReloadPics(true);

	pr_ext_warned_particleeffectnum = 0;
}

func_t PR_FindExtFunction(const char *entryname)
{	//depends on 0 being an invalid function,
	dfunction_t *func = ED_FindFunction(entryname);
	if (func)
		return func - qcvm->functions;
	return 0;
}
static void *PR_FindExtGlobal(int type, const char *name)
{
	ddef_t *def = ED_FindGlobal(name);
	if (def && (def->type&~DEF_SAVEGLOBAL) == type && def->ofs < qcvm->progs->numglobals)
		return qcvm->globals + def->ofs;
	return NULL;
}

void PR_AutoCvarChanged(cvar_t *var)
{
	char *n;
	ddef_t *glob;
	qcvm_t *oldqcvm = qcvm;
	PR_SwitchQCVM(NULL);
	if (sv.active)
	{
		PR_SwitchQCVM(&sv.qcvm);
		n = va("autocvar_%s", var->name);
		glob = ED_FindGlobal(n);
		if (glob)
		{
			if (!ED_ParseEpair ((void *)qcvm->globals, glob, var->string, true))
				Con_Warning("EXT: Unable to configure %s\n", n);
		}
		PR_SwitchQCVM(NULL);
	}
	if (cl.qcvm.globals)
	{
		PR_SwitchQCVM(&cl.qcvm);
		n = va("autocvar_%s", var->name);
		glob = ED_FindGlobal(n);
		if (glob)
		{
			if (!ED_ParseEpair ((void *)qcvm->globals, glob, var->string, true))
				Con_Warning("EXT: Unable to configure %s\n", n);
		}
		PR_SwitchQCVM(NULL);
	}
	if (cls.menu_qcvm.globals)
	{
		PR_SwitchQCVM(&cls.menu_qcvm);
		n = va("autocvar_%s", var->name);
		glob = ED_FindGlobal(n);
		if (glob)
		{
			if (!ED_ParseEpair ((void *)qcvm->globals, glob, var->string, true))
				Con_Warning("EXT: Unable to configure %s\n", n);
		}
		PR_SwitchQCVM(NULL);
	}
	PR_SwitchQCVM(oldqcvm);
}

void PR_InitExtensions(void)
{
	size_t i, g,m;
	//this only needs to be done once. because we're evil.
	//it should help slightly with the 'documentation' above at least.
	g = m = sizeof(qcvm->builtins)/sizeof(qcvm->builtins[0]);
	for (i = 0; i < sizeof(extensionbuiltins)/sizeof(extensionbuiltins[0]); i++)
	{
		if (extensionbuiltins[i].documentednumber)
			extensionbuiltins[i].number = extensionbuiltins[i].documentednumber;
		else
			extensionbuiltins[i].number = --g;
		if (extensionbuiltins[i].documentednumber_menuqc)
			extensionbuiltins[i].number_menuqc = extensionbuiltins[i].documentednumber_menuqc;
		else
			extensionbuiltins[i].number_menuqc = --m;
	}
}

//called at map start
void PR_EnableExtensions(ddef_t *pr_globaldefs)
{
	unsigned int i, j;
	unsigned int numautocvars = 0;
	qboolean menuqc = qcvm == &cls.menu_qcvm;

	for (i = qcvm->numbuiltins; i < countof(qcvm->builtins); i++)
		qcvm->builtins[i] = PF_Fixme;
	qcvm->numbuiltins = i;
	if (!pr_checkextension.value && qcvm == &sv.qcvm)
	{
		Con_DPrintf("not enabling qc extensions\n");
		return;
	}

#define QCEXTFUNC(n,t) qcvm->extfuncs.n = PR_FindExtFunction(#n);
	QCEXTFUNCS_COMMON

	//replace standard builtins with new replacement extended ones and selectively populate references to module-specific entrypoints.
	if (qcvm == &cl.qcvm)
	{	//csqc
		for (i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]); i++)
		{
			int num = (extensionbuiltins[i].documentednumber);
			if (num && extensionbuiltins[i].csqcfunc && qcvm->builtins[num] != PF_Fixme)
				qcvm->builtins[num] = extensionbuiltins[i].csqcfunc;
		}

		QCEXTFUNCS_GAME
		QCEXTFUNCS_CS
	}
	else if (qcvm == &sv.qcvm)
	{	//ssqc
		for (i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]); i++)
		{
			int num = (extensionbuiltins[i].documentednumber);
			if (num && extensionbuiltins[i].ssqcfunc && qcvm->builtins[num] != PF_Fixme)
				qcvm->builtins[num] = extensionbuiltins[i].ssqcfunc;
		}

		QCEXTFUNCS_GAME
		QCEXTFUNCS_SV
	}
	else if (qcvm == &cls.menu_qcvm)
	{	//menuqc
		for (i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]); i++)
		{
			int num = (extensionbuiltins[i].documentednumber_menuqc);
			if (num && extensionbuiltins[i].menufunc && qcvm->builtins[num] != PF_Fixme)
				qcvm->builtins[num] = extensionbuiltins[i].menufunc;
		}

		QCEXTFUNCS_MENU
	}
#undef QCEXTFUNC


#define QCEXTGLOBAL_FLOAT(n) qcvm->extglobals.n = PR_FindExtGlobal(ev_float, #n);
#define QCEXTGLOBAL_INT(n) qcvm->extglobals.n = PR_FindExtGlobal(ev_ext_integer, #n);
#define QCEXTGLOBAL_VECTOR(n) qcvm->extglobals.n = PR_FindExtGlobal(ev_vector, #n);
	QCEXTGLOBALS_COMMON
	QCEXTGLOBALS_GAME
	QCEXTGLOBALS_CSQC
#undef QCEXTGLOBAL_FLOAT
#undef QCEXTGLOBAL_INT
#undef QCEXTGLOBAL_VECTOR

	//any #0 functions are remapped to their builtins here, so we don't have to tweak the VM in an obscure potentially-breaking way.
	for (i = 0; i < (unsigned int)qcvm->progs->numfunctions; i++)
	{
		if (qcvm->functions[i].first_statement == 0 && qcvm->functions[i].s_name && !qcvm->functions[i].parm_start && !qcvm->functions[i].locals)
		{
			const char *name = PR_GetString(qcvm->functions[i].s_name);
			for (j = 0; j < sizeof(extensionbuiltins)/sizeof(extensionbuiltins[0]); j++)
			{
				if (!strcmp(extensionbuiltins[j].name, name))
				{	//okay, map it
					qcvm->functions[i].first_statement = -(menuqc?extensionbuiltins[j].number_menuqc:extensionbuiltins[j].number);
					break;
				}
			}
		}
	}

	//autocvars
	for (i = 0; i < (unsigned int)qcvm->progs->numglobaldefs; i++)
	{
		const char *n = PR_GetString(qcvm->globaldefs[i].s_name);
		if (!strncmp(n, "autocvar_", 9))
		{
			//really crappy approach
			cvar_t *var = Cvar_Create(n + 9, PR_UglyValueString (qcvm->globaldefs[i].type, (eval_t*)(qcvm->globals + qcvm->globaldefs[i].ofs)));
			numautocvars++;
			if (!var)
				continue;	//name conflicts with a command?

			if (!ED_ParseEpair ((void *)qcvm->globals, &pr_globaldefs[i], var->string, true))
				Con_Warning("EXT: Unable to configure %s\n", n);
			var->flags |= CVAR_AUTOCVAR;
		}
	}
	if (numautocvars)
		Con_DPrintf2("Found %i autocvars\n", numautocvars);
}

void PR_DumpPlatform_f(void)
{
	char	name[MAX_OSPATH];
	FILE *f;
	const char *outname = NULL;
	unsigned int i, j;
	enum
	{
		SS=1,
		CS=2,
		MN=4,
	};
	unsigned int targs = 0;
	for (i = 1; i < (unsigned)Cmd_Argc(); )
	{
		const char *arg = Cmd_Argv(i++);
		if (!strncmp(arg, "-O", 2))
		{
			if (arg[2])
				outname = arg+2;
			else
				outname = Cmd_Argv(i++);
		}
		else if (!q_strcasecmp(arg, "-Tcs"))
			targs |= CS;
		else if (!q_strcasecmp(arg, "-Tss"))
			targs |= SS;
		else if (!q_strcasecmp(arg, "-Tmenu"))
			targs |= MN;
		else
		{
			Con_Printf("%s: Unknown argument\n", Cmd_Argv(0));
			return;
		}
	}
	if (!outname)
		outname = ((targs==2)?"qscsextensions":"qsextensions");
	if (!targs)
		targs = SS|CS;

	if (strstr(outname, ".."))
		return;
	q_snprintf (name, sizeof(name), "%s/src/%s", com_gamedir, outname);
	COM_AddExtension (name, ".qc", sizeof(name));

	f = fopen (name, "w");
	if (!f)
	{
		Con_Printf("%s: Couldn't write %s\n", Cmd_Argv(0), name);
		return;
	}
	Con_Printf("%s: Writing %s\n", Cmd_Argv(0), name);

	fprintf(f,
		"/*\n"
		"Extensions file for "ENGINE_NAME_AND_VER"\n"
		"This file is auto-generated by %s %s.\n"
		"You will probably need to use FTEQCC to compile this.\n"
		"*/\n"
		,Cmd_Argv(0), Cmd_Args()?Cmd_Args():"with no args");

	fprintf(f,
		"\n\n//This file only supports csqc, so including this file in some other situation is a user error\n"
		"#if defined(QUAKEWORLD) || defined(MENU)\n"
		"#error Mixed up module defs\n"
		"#endif\n"
		);
	if (targs & CS)
	{
		fprintf(f,
			"#if !defined(CSQC) && !defined(SSQC) && !defined(MENU)\n"
				"#define CSQC\n"
				"#ifndef CSQC_SIMPLE\n"	//quakespasm's csqc implementation is simplified, and can do huds+menus, but that's about it.
					"#define CSQC_SIMPLE\n"
				"#endif\n"
			"#endif\n"
			);
	}
	else if (targs & SS)
	{
		fprintf(f,
			"#if !defined(CSQC) && !defined(SSQC) && !defined(MENU)\n"
				"#define SSQC\n"
			"#endif\n"
			);
	}
	else if (targs & MN)
	{
		fprintf(f,
			"#if !defined(CSQC) && !defined(SSQC) && !defined(MENU)\n"
				"#define MENU\n"
			"#endif\n"
			);
	}
	fprintf(f,
		"#ifndef QSSDEP\n"
			"#define QSSDEP(reason) __deprecated(reason)\n"
		"#endif\n"
	);

	fprintf(f, "\n\n//List of advertised extensions\n");
	for (i = 0; i < countof(qcextensions); i++)
		fprintf(f, "//%s\n", qcextensions[i].name);

	fprintf(f, "\n\n//Explicitly flag this stuff as probably-not-referenced, meaning fteqcc will shut up about it and silently strip what it can.\n");
	fprintf(f, "#pragma noref 1\n");

	if (targs & (SS|CS))	//qss uses the same system defs for both ssqc and csqc, as a simplification.
	{	//I really hope that fteqcc's unused variable logic is up to scratch
		fprintf(f, "#if defined(CSQC_SIMPLE) || defined(SSQC)\n");
		fprintf(f, "entity		self,other,world;\n");
		fprintf(f, "float		time,frametime,force_retouch;\n");
		fprintf(f, "string		mapname;\n");
		fprintf(f, "float		deathmatch,coop,teamplay,serverflags,total_secrets,total_monsters,found_secrets,killed_monsters,parm1, parm2, parm3, parm4, parm5, parm6, parm7, parm8, parm9, parm10, parm11, parm12, parm13, parm14, parm15, parm16;\n");
		fprintf(f, "vector		v_forward, v_up, v_right;\n");
		fprintf(f, "float		trace_allsolid,trace_startsolid,trace_fraction;\n");
		fprintf(f, "vector		trace_endpos,trace_plane_normal;\n");
		fprintf(f, "float		trace_plane_dist;\n");
		fprintf(f, "entity		trace_ent;\n");
		fprintf(f, "float		trace_inopen,trace_inwater;\n");
		fprintf(f, "entity		msg_entity;\n");
		fprintf(f, "void() 		main,StartFrame,PlayerPreThink,PlayerPostThink,ClientKill,ClientConnect,PutClientInServer,ClientDisconnect,SetNewParms,SetChangeParms;\n");
		fprintf(f, "void		end_sys_globals;\n\n");
		fprintf(f, ".float		modelindex;\n");
		fprintf(f, ".vector		absmin, absmax;\n");
		fprintf(f, ".float		ltime,movetype,solid;\n");
		fprintf(f, ".vector		origin,oldorigin,velocity,angles,avelocity,punchangle;\n");
		fprintf(f, ".string		classname,model;\n");
		fprintf(f, ".float		frame,skin,effects;\n");
		fprintf(f, ".vector		mins, maxs,size;\n");
		fprintf(f, ".void()		touch,use,think,blocked;\n");
		fprintf(f, ".float		nextthink;\n");
		fprintf(f, ".entity		groundentity;\n");
		fprintf(f, ".float		health,frags,weapon;\n");
		fprintf(f, ".string		weaponmodel;\n");
		fprintf(f, ".float		weaponframe,currentammo,ammo_shells,ammo_nails,ammo_rockets,ammo_cells,items,takedamage;\n");
		fprintf(f, ".entity		chain;\n");
		fprintf(f, ".float		deadflag;\n");
		fprintf(f, ".vector		view_ofs;\n");
		fprintf(f, ".float		button0,button1,button2,impulse,fixangle;\n");
		fprintf(f, ".vector		v_angle;\n");
		fprintf(f, ".float		idealpitch;\n");
		fprintf(f, ".string		netname;\n");
		fprintf(f, ".entity 	enemy;\n");
		fprintf(f, ".float		flags,colormap,team,max_health,teleport_time,armortype,armorvalue,waterlevel,watertype,ideal_yaw,yaw_speed;\n");
		fprintf(f, ".entity		aiment,goalentity;\n");
		fprintf(f, ".float		spawnflags;\n");
		fprintf(f, ".string		target,targetname;\n");
		fprintf(f, ".float		dmg_take,dmg_save;\n");
		fprintf(f, ".entity		dmg_inflictor,owner;\n");
		fprintf(f, ".vector		movedir;\n");
		fprintf(f, ".string		message;\n");
		fprintf(f, ".float		sounds;\n");
		fprintf(f, ".string		noise, noise1, noise2, noise3;\n");
		fprintf(f, "void		end_sys_fields;\n");
		fprintf(f, "#endif\n");
	}
	if (targs & MN)
	{
		fprintf(f, "#if defined(MENU)\n");
		fprintf(f, "entity		self;\n");
		fprintf(f, "void		end_sys_globals;\n\n");
		fprintf(f, "void		end_sys_fields;\n");
		fprintf(f, "#endif\n");
	}

	fprintf(f, "\n\n//Some custom types (that might be redefined as accessors by fteextensions.qc, although we don't define any methods here)\n");
	fprintf(f, "#ifdef _ACCESSORS\n");
	fprintf(f, "accessor strbuf:float;\n");
	fprintf(f, "accessor searchhandle:float;\n");
	fprintf(f, "accessor hashtable:float;\n");
	fprintf(f, "accessor infostring:string;\n");
	fprintf(f, "accessor filestream:float;\n");
	fprintf(f, "#else\n");
	fprintf(f, "#define strbuf float\n");
	fprintf(f, "#define searchhandle float\n");
	fprintf(f, "#define hashtable float\n");
	fprintf(f, "#define infostring string\n");
	fprintf(f, "#define filestream float\n");
	fprintf(f, "#endif\n");


	fprintf(f, "\n\n//Common entry points\n");
#define QCEXTFUNC(n,t) fprintf(f, t " " #n "\n");
	QCEXTFUNCS_COMMON
	if (targs & (SS|CS))
	{
		QCEXTFUNCS_GAME
	}

	if (targs & SS)
	{
		fprintf(f, "\n\n//Serverside entry points\n");
		QCEXTFUNCS_SV
	}
	if (targs & CS)
	{
		fprintf(f, "\n\n//CSQC entry points\n");
		QCEXTFUNCS_CS
	}
	if (targs & MN)
	{
		fprintf(f, "\n\n//MenuQC entry points\n");
		QCEXTFUNCS_MENU
	}
#undef QCEXTFUNC

#define QCEXTGLOBAL_INT(n) fprintf(f, "int " #n ";\n");
#define QCEXTGLOBAL_FLOAT(n) fprintf(f, "float " #n ";\n");
#define QCEXTGLOBAL_VECTOR(n) fprintf(f, "vector " #n ";\n");
	QCEXTGLOBALS_COMMON
	if (targs & (CS|SS))
	{
		QCEXTGLOBALS_GAME
	}
	if (targs & CS)
	{
		QCEXTGLOBALS_CSQC
	}
#undef QCEXTGLOBAL_INT
#undef QCEXTGLOBAL_FLOAT
#undef QCEXTGLOBAL_VECTOR

	fprintf(f, "const float FALSE		= 0;\n");
	fprintf(f, "const float TRUE		= 1;\n");

	if (targs & CS)
	{
		fprintf(f, "const float MASK_ENGINE = %i;\n", MASK_ENGINE);
		fprintf(f, "const float MASK_VIEWMODEL = %i;\n", MASK_VIEWMODEL);

		fprintf(f, "const float GE_MAXENTS = %i;\n", GE_MAXENTS);
		fprintf(f, "const float GE_ACTIVE = %i;\n", GE_ACTIVE);
		fprintf(f, "const float GE_ORIGIN = %i;\n", GE_ORIGIN);
		fprintf(f, "const float GE_FORWARD = %i;\n", GE_FORWARD);
		fprintf(f, "const float GE_RIGHT = %i;\n", GE_RIGHT);
		fprintf(f, "const float GE_UP = %i;\n", GE_UP);
		fprintf(f, "const float GE_SCALE = %i;\n", GE_SCALE);
		fprintf(f, "const float GE_ORIGINANDVECTORS = %i;\n", GE_ORIGINANDVECTORS);
		fprintf(f, "const float GE_ALPHA = %i;\n", GE_ALPHA);
		fprintf(f, "const float GE_COLORMOD = %i;\n", GE_COLORMOD);
		fprintf(f, "const float GE_PANTSCOLOR = %i;\n", GE_PANTSCOLOR);
		fprintf(f, "const float GE_SHIRTCOLOR = %i;\n", GE_SHIRTCOLOR);
		fprintf(f, "const float GE_SKIN = %i;\n", GE_SKIN);
	}
	if (targs & (CS|SS))
	{
		fprintf(f, "const float STAT_HEALTH = 0;		/* Player's health. */\n");
//		fprintf(f, "const float STAT_FRAGS = 1;			/* unused */\n");
		fprintf(f, "const float STAT_WEAPONMODELI = 2;	/* This is the modelindex of the current viewmodel (renamed from the original name 'STAT_WEAPON' due to confusions). */\n");
		fprintf(f, "const float STAT_AMMO = 3;			/* player.currentammo */\n");
		fprintf(f, "const float STAT_ARMOR = 4;\n");
		fprintf(f, "const float STAT_WEAPONFRAME = 5;\n");
		fprintf(f, "const float STAT_SHELLS = 6;\n");
		fprintf(f, "const float STAT_NAILS = 7;\n");
		fprintf(f, "const float STAT_ROCKETS = 8;\n");
		fprintf(f, "const float STAT_CELLS = 9;\n");
		fprintf(f, "const float STAT_ACTIVEWEAPON = 10;	/* player.weapon */\n");
		fprintf(f, "const float STAT_TOTALSECRETS = 11;\n");
		fprintf(f, "const float STAT_TOTALMONSTERS = 12;\n");
		fprintf(f, "const float STAT_FOUNDSECRETS = 13;\n");
		fprintf(f, "const float STAT_KILLEDMONSTERS = 14;\n");
		fprintf(f, "const float STAT_ITEMS = 15;		/* self.items | (self.items2<<23). In order to decode this stat properly, you need to use getstatbits(STAT_ITEMS,0,23) to read self.items, and getstatbits(STAT_ITEMS,23,11) to read self.items2 or getstatbits(STAT_ITEMS,28,4) to read the visible part of serverflags, whichever is applicable. */\n");
		fprintf(f, "const float STAT_VIEWHEIGHT = 16;	/* player.view_ofs_z */\n");
//		fprintf(f, "const float STAT_TIME = 17;\n");
//		fprintf(f, "//const float STAT_MATCHSTARTTIME = 18;\n");
//		fprintf(f, "const float STAT_UNUSED = 19;\n");
		fprintf(f, "const float STAT_VIEW2 = 20;		/* This stat contains the number of the entity in the server's .view2 field. */\n");
		fprintf(f, "const float STAT_VIEWZOOM = 21;		/* Scales fov and sensitiity. Part of DP_VIEWZOOM. */\n");
//		fprintf(f, "const float STAT_UNUSED = 22;\n");
//		fprintf(f, "const float STAT_UNUSED = 23;\n");
//		fprintf(f, "const float STAT_UNUSED = 24;\n");
		fprintf(f, "const float STAT_IDEALPITCH = 25;\n");
		fprintf(f, "const float STAT_PUNCHANGLE_X = 26;\n");
		fprintf(f, "const float STAT_PUNCHANGLE_Y = 27;\n");
		fprintf(f, "const float STAT_PUNCHANGLE_Z = 28;\n");
//		fprintf(f, "const float STAT_PUNCHVECTOR_X = 29;\n");
//		fprintf(f, "const float STAT_PUNCHVECTOR_Y = 30;\n");
//		fprintf(f, "const float STAT_PUNCHVECTOR_Z = 31;\n");

		fprintf(f, "const float SOLID_BBOX = %i;\n", SOLID_BBOX);
		fprintf(f, "const float SOLID_BSP = %i;\n", SOLID_BSP);
		fprintf(f, "const float SOLID_NOT = %i;\n", SOLID_NOT);
		fprintf(f, "const float SOLID_SLIDEBOX = %i;\n", SOLID_SLIDEBOX);
		fprintf(f, "const float SOLID_TRIGGER = %i;\n", SOLID_TRIGGER);

		fprintf(f, "const float MOVETYPE_NONE = %i;\n", MOVETYPE_NONE);
		fprintf(f, "const float MOVETYPE_WALK = %i;\n", MOVETYPE_WALK);
		fprintf(f, "const float MOVETYPE_STEP = %i;\n", MOVETYPE_STEP);
		fprintf(f, "const float MOVETYPE_FLY = %i;\n", MOVETYPE_FLY);
		fprintf(f, "const float MOVETYPE_TOSS = %i;\n", MOVETYPE_TOSS);
		fprintf(f, "const float MOVETYPE_PUSH = %i;\n", MOVETYPE_PUSH);
		fprintf(f, "const float MOVETYPE_NOCLIP = %i;\n", MOVETYPE_NOCLIP);
		fprintf(f, "const float MOVETYPE_FLYMISSILE = %i;\n", MOVETYPE_FLYMISSILE);
		fprintf(f, "const float MOVETYPE_BOUNCE = %i;\n", MOVETYPE_BOUNCE);
		fprintf(f, "const float MOVETYPE_FOLLOW = %i;\n", MOVETYPE_EXT_FOLLOW);

		fprintf(f, "const float CONTENT_EMPTY = %i;\n", CONTENTS_EMPTY);
		fprintf(f, "const float CONTENT_SOLID = %i;\n", CONTENTS_SOLID);
		fprintf(f, "const float CONTENT_WATER = %i;\n", CONTENTS_WATER);
		fprintf(f, "const float CONTENT_SLIME = %i;\n", CONTENTS_SLIME);
		fprintf(f, "const float CONTENT_LAVA = %i;\n", CONTENTS_LAVA);
		fprintf(f, "const float CONTENT_SKY = %i;\n", CONTENTS_SKY);
		fprintf(f, "const float CONTENT_LADDER = %i;\n", CONTENTS_LADDER);

		fprintf(f, "__used var float physics_mode = 2;\n");

		fprintf(f, "const float TE_SPIKE = %i;\n", TE_SPIKE);
		fprintf(f, "const float TE_SUPERSPIKE = %i;\n", TE_SUPERSPIKE);
		fprintf(f, "const float TE_GUNSHOT = %i;\n", TE_GUNSHOT);
		fprintf(f, "const float TE_EXPLOSION = %i;\n", TE_EXPLOSION);
		fprintf(f, "const float TE_TAREXPLOSION = %i;\n", TE_TAREXPLOSION);
		fprintf(f, "const float TE_LIGHTNING1 = %i;\n", TE_LIGHTNING1);
		fprintf(f, "const float TE_LIGHTNING2 = %i;\n", TE_LIGHTNING2);
		fprintf(f, "const float TE_WIZSPIKE = %i;\n", TE_WIZSPIKE);
		fprintf(f, "const float TE_KNIGHTSPIKE = %i;\n", TE_KNIGHTSPIKE);
		fprintf(f, "const float TE_LIGHTNING3 = %i;\n", TE_LIGHTNING3);
		fprintf(f, "const float TE_LAVASPLASH = %i;\n", TE_LAVASPLASH);
		fprintf(f, "const float TE_TELEPORT = %i;\n", TE_TELEPORT);
		fprintf(f, "const float TE_EXPLOSION2 = %i;\n", TE_EXPLOSION2);
		fprintf(f, "const float TE_BEAM = %i;\n", TE_BEAM);


		fprintf(f, "const float MF_ROCKET			= %#x;\n", EF_ROCKET);
		fprintf(f, "const float MF_GRENADE			= %#x;\n", EF_GRENADE);
		fprintf(f, "const float MF_GIB				= %#x;\n", EF_GIB);
		fprintf(f, "const float MF_ROTATE			= %#x;\n", EF_ROTATE);
		fprintf(f, "const float MF_TRACER			= %#x;\n", EF_TRACER);
		fprintf(f, "const float MF_ZOMGIB			= %#x;\n", EF_ZOMGIB);
		fprintf(f, "const float MF_TRACER2			= %#x;\n", EF_TRACER2);
		fprintf(f, "const float MF_TRACER3			= %#x;\n", EF_TRACER3);


		fprintf(f, "const float EF_BRIGHTFIELD = %i;\n", EF_BRIGHTFIELD);
		fprintf(f, "const float EF_MUZZLEFLASH = %i;\n", EF_MUZZLEFLASH);
		fprintf(f, "const float EF_BRIGHTLIGHT = %i;\n", EF_BRIGHTLIGHT);
		fprintf(f, "const float EF_DIMLIGHT = %i;\n", EF_DIMLIGHT);
		fprintf(f, "const float EF_FULLBRIGHT = %i;\n", EF_FULLBRIGHT);
		fprintf(f, "const float EF_NOSHADOW = %i;\n", EF_NOSHADOW);
		fprintf(f, "const float EF_NOMODELFLAGS = %i;\n", EF_NOMODELFLAGS);

		fprintf(f, "const float FL_FLY = %i;\n", FL_FLY);
		fprintf(f, "const float FL_SWIM = %i;\n", FL_SWIM);
		fprintf(f, "const float FL_CLIENT = %i;\n", FL_CLIENT);
		fprintf(f, "const float FL_INWATER = %i;\n", FL_INWATER);
		fprintf(f, "const float FL_MONSTER = %i;\n", FL_MONSTER);
		fprintf(f, "const float FL_GODMODE = %i;\n", FL_GODMODE);
		fprintf(f, "const float FL_NOTARGET = %i;\n", FL_NOTARGET);
		fprintf(f, "const float FL_ITEM = %i;\n", FL_ITEM);
		fprintf(f, "const float FL_ONGROUND = %i;\n", FL_ONGROUND);
		fprintf(f, "const float FL_PARTIALGROUND = %i;\n", FL_PARTIALGROUND);
		fprintf(f, "const float FL_WATERJUMP = %i;\n", FL_WATERJUMP);
		fprintf(f, "const float FL_JUMPRELEASED = %i;\n", FL_JUMPRELEASED);

		fprintf(f, "const float ATTN_NONE = %i;\n", 0);
		fprintf(f, "const float ATTN_NORM = %i;\n", 1);
		fprintf(f, "const float ATTN_IDLE = %i;\n", 2);
		fprintf(f, "const float ATTN_STATIC = %i;\n", 3);

		fprintf(f, "const float CHAN_AUTO = %i;\n", 0);
		fprintf(f, "const float CHAN_WEAPON = %i;\n", 1);
		fprintf(f, "const float CHAN_VOICE = %i;\n", 2);
		fprintf(f, "const float CHAN_ITEM = %i;\n", 3);
		fprintf(f, "const float CHAN_BODY = %i;\n", 4);

		fprintf(f, "const float SPA_POSITION = %i;\n", SPA_POSITION);
		fprintf(f, "const float SPA_S_AXIS = %i;\n", SPA_S_AXIS);
		fprintf(f, "const float SPA_T_AXIS = %i;\n", SPA_T_AXIS);
		fprintf(f, "const float SPA_R_AXIS = %i;\n", SPA_R_AXIS);
		fprintf(f, "const float SPA_TEXCOORDS0 = %i;\n", SPA_TEXCOORDS0);
		fprintf(f, "const float SPA_LIGHTMAP0_TEXCOORDS = %i;\n", SPA_LIGHTMAP0_TEXCOORDS);
		fprintf(f, "const float SPA_LIGHTMAP0_COLOR = %i;\n", SPA_LIGHTMAP0_COLOR);
	}

	if (targs & (CS|MN))
	{
		fprintf(f, "const float IE_KEYDOWN		= %i;\n", CSIE_KEYDOWN);
		fprintf(f, "const float IE_KEYUP		= %i;\n", CSIE_KEYUP);
		fprintf(f, "const float IE_MOUSEDELTA	= %i;\n", CSIE_MOUSEDELTA);
		fprintf(f, "const float IE_MOUSEABS		= %i;\n", CSIE_MOUSEABS);
		fprintf(f, "const float IE_JOYAXIS		= %i;\n", CSIE_JOYAXIS);

		fprintf(f, "const float VF_MIN = %i;", VF_MIN);
		fprintf(f, "const float VF_MIN_X = %i;", VF_MIN_X);
		fprintf(f, "const float VF_MIN_Y = %i;", VF_MIN_Y);
		fprintf(f, "const float VF_SIZE = %i;", VF_SIZE);
		fprintf(f, "const float VF_SIZE_X = %i;", VF_SIZE_X);
		fprintf(f, "const float VF_SIZE_Y = %i;", VF_SIZE_Y);
		fprintf(f, "const float VF_VIEWPORT = %i;", VF_VIEWPORT);
		fprintf(f, "const float VF_FOV = %i;", VF_FOV);
		fprintf(f, "const float VF_FOV_X = %i;", VF_FOV_X);
		fprintf(f, "const float VF_FOV_Y = %i;", VF_FOV_Y);
		fprintf(f, "const float VF_ORIGIN = %i;", VF_ORIGIN);
		fprintf(f, "const float VF_ORIGIN_X = %i;", VF_ORIGIN_X);
		fprintf(f, "const float VF_ORIGIN_Y = %i;", VF_ORIGIN_Y);
		fprintf(f, "const float VF_ORIGIN_Z = %i;", VF_ORIGIN_Z);
		fprintf(f, "const float VF_ANGLES = %i;", VF_ANGLES);
		fprintf(f, "const float VF_ANGLES_X = %i;", VF_ANGLES_X);
		fprintf(f, "const float VF_ANGLES_Y = %i;", VF_ANGLES_Y);
		fprintf(f, "const float VF_ANGLES_Z = %i;", VF_ANGLES_Z);
		fprintf(f, "const float VF_DRAWWORLD = %i;", VF_DRAWWORLD);
		fprintf(f, "const float VF_DRAWENGINESBAR = %i;", VF_DRAWENGINESBAR);
		fprintf(f, "const float VF_DRAWCROSSHAIR = %i;", VF_DRAWCROSSHAIR);
		fprintf(f, "QSSDEP(\"Query only\") const float VF_MINDIST = %i;", VF_MINDIST);
		fprintf(f, "QSSDEP(\"Query only\") const float VF_MAXDIST = %i;", VF_MAXDIST);
		fprintf(f, "const float VF_CL_VIEWANGLES = %i;", VF_CL_VIEWANGLES);
		fprintf(f, "const float VF_CL_VIEWANGLES_X = %i;", VF_CL_VIEWANGLES_X);
		fprintf(f, "const float VF_CL_VIEWANGLES_Y = %i;", VF_CL_VIEWANGLES_Y);
		fprintf(f, "const float VF_CL_VIEWANGLES_Z = %i;", VF_CL_VIEWANGLES_Z);
		fprintf(f, "const float VF_ACTIVESEAT = %i; //stub - must only be set to 0", VF_ACTIVESEAT);
		fprintf(f, "const float VF_AFOV = %i;", VF_AFOV);
		fprintf(f, "const float VF_SCREENVSIZE = %i;", VF_SCREENVSIZE);
		fprintf(f, "const float VF_SCREENPSIZE = %i;", VF_SCREENPSIZE);

		fprintf(f, "const float RF_VIEWMODEL = %i;", RF_VIEWMODEL);
		fprintf(f, "const float RF_EXTERNALMODEL = %i;", RF_EXTERNALMODEL);
		fprintf(f, "const float RF_DEPTHHACK = %i;", RF_DEPTHHACK);
		fprintf(f, "const float RF_ADDITIVE = %i;", RF_ADDITIVE);
		fprintf(f, "const float RF_USEAXIS = %i;", RF_USEAXIS);
		fprintf(f, "const float RF_NOSHADOW = %i;", RF_NOSHADOW);
		fprintf(f, "const float RF_WEIRDFRAMETIMES = %i;", RF_WEIRDFRAMETIMES);

		fprintf(f, "const float PRECACHE_PIC_FROMWAD = %i;", PICFLAG_WAD);
		fprintf(f, "const float PRECACHE_PIC_WRAP = %i;", PICFLAG_WRAP);
		fprintf(f, "const float PRECACHE_PIC_MIPMAP = %i;", PICFLAG_MIPMAP);
		fprintf(f, "const float PRECACHE_PIC_TEST = %i;", PICFLAG_BLOCK);

		fprintf(f, "const float SLIST_HOSTCACHEVIEWCOUNT = %i;", SLIST_HOSTCACHEVIEWCOUNT);
		fprintf(f, "const float SLIST_HOSTCACHETOTALCOUNT = %i;", SLIST_HOSTCACHETOTALCOUNT);
		fprintf(f, "const float SLIST_SORTFIELD = %i;", SLIST_SORTFIELD);
		fprintf(f, "const float SLIST_SORTDESCENDING = %i;", SLIST_SORTDESCENDING);

		fprintf(f, "const float GGDI_GAMEDIR = %i;", GGDI_GAMEDIR);
		//fprintf(f, "const float GGDI_DESCRIPTION = %i;", GGDI_DESCRIPTION);
		//fprintf(f, "const float GGDI_OVERRIDES = %i;", GGDI_OVERRIDES);
		fprintf(f, "const float GGDI_LOADCOMMAND = %i;", GGDI_LOADCOMMAND);
		//fprintf(f, "const float GGDI_ICON = %i;", GGDI_ICON);
		//fprintf(f, "const float GGDI_GAMEDIRLIST = %i;", GGDI_GAMEDIRLIST);
	}
	fprintf(f, "const float STAT_USER = 32;			/* Custom user stats start here (lower values are reserved for engine use). */\n");
	//these can be used for both custom stats and for reflection
	fprintf(f, "const float EV_VOID = %i;\n", ev_void);
	fprintf(f, "const float EV_STRING = %i;\n", ev_string);
	fprintf(f, "const float EV_FLOAT = %i;\n", ev_float);
	fprintf(f, "const float EV_VECTOR = %i;\n", ev_vector);
	fprintf(f, "const float EV_ENTITY = %i;\n", ev_entity);
	fprintf(f, "const float EV_FIELD = %i;\n", ev_field);
	fprintf(f, "const float EV_FUNCTION = %i;\n", ev_function);
	fprintf(f, "const float EV_POINTER = %i;\n", ev_pointer);
	fprintf(f, "const float EV_INTEGER = %i;\n", ev_ext_integer);

#define QCEXTFIELD(n,t) fprintf(f, "%s %s;\n", t, #n);
	//extra fields
	fprintf(f, "\n\n//Supported Extension fields\n");
	QCEXTFIELDS_ALL
	if (targs & (SS|CS)) {QCEXTFIELDS_GAME}
	if (targs & (CS|MN)) {QCEXTFIELDS_CL}
	if (targs & (CS)) {QCEXTFIELDS_CS}
	if (targs & (SS)) {QCEXTFIELDS_SS}
#undef QCEXTFIELD

	if (targs & SS)
	{
		fprintf(f, ".float style;\n");		//not used by the engine, but is used by tools etc.
		fprintf(f, ".float light_lev;\n");	//ditto.

		//extra constants
		fprintf(f, "\n\n//Supported Extension Constants\n");
		fprintf(f, "const float MOVETYPE_FOLLOW	= "STRINGIFY(MOVETYPE_EXT_FOLLOW)";\n");
		fprintf(f, "const float SOLID_CORPSE	= "STRINGIFY(SOLID_EXT_CORPSE)";\n");

		fprintf(f, "const float CLIENTTYPE_DISCONNECT	= "STRINGIFY(0)";\n");
		fprintf(f, "const float CLIENTTYPE_REAL			= "STRINGIFY(1)";\n");
		fprintf(f, "const float CLIENTTYPE_BOT			= "STRINGIFY(2)";\n");
		fprintf(f, "const float CLIENTTYPE_NOTCLIENT	= "STRINGIFY(3)";\n");

		fprintf(f, "const float EF_NOSHADOW			= %#x;\n", EF_NOSHADOW);
		fprintf(f, "const float EF_NOMODELFLAGS		= %#x; /*the standard modelflags from the model are ignored*/\n", EF_NOMODELFLAGS);

		fprintf(f, "const float DAMAGE_AIM = %i;\n", DAMAGE_AIM);
		fprintf(f, "const float DAMAGE_NO = %i;\n", DAMAGE_NO);
		fprintf(f, "const float DAMAGE_YES = %i;\n", DAMAGE_YES);

		fprintf(f, "const float MSG_BROADCAST = %i;\n", MSG_BROADCAST);
		fprintf(f, "const float MSG_ONE = %i;\n", MSG_ONE);
		fprintf(f, "const float MSG_ALL = %i;\n", MSG_ALL);
		fprintf(f, "const float MSG_INIT = %i;\n", MSG_INIT);
		fprintf(f, "const float MSG_MULTICAST = %i;\n", MSG_EXT_MULTICAST);
		fprintf(f, "const float MSG_ENTITY = %i;\n", MSG_EXT_ENTITY);

		fprintf(f, "const float MSG_MULTICAST	= %i;\n", 4);
		fprintf(f, "const float MULTICAST_ALL	= %i;\n", MULTICAST_ALL_U);
	//	fprintf(f, "const float MULTICAST_PHS	= %i;\n", MULTICAST_PHS_U);
		fprintf(f, "const float MULTICAST_PVS	= %i;\n", MULTICAST_PVS_U);
		fprintf(f, "const float MULTICAST_ONE	= %i;\n", MULTICAST_ONE_U);
		fprintf(f, "const float MULTICAST_ALL_R	= %i;\n", MULTICAST_ALL_R);
	//	fprintf(f, "const float MULTICAST_PHS_R	= %i;\n", MULTICAST_PHS_R);
		fprintf(f, "const float MULTICAST_PVS_R	= %i;\n", MULTICAST_PVS_R);
		fprintf(f, "const float MULTICAST_ONE_R	= %i;\n", MULTICAST_ONE_R);
		fprintf(f, "const float MULTICAST_INIT	= %i;\n", MULTICAST_INIT);
	}

	fprintf(f, "const float FILE_READ		= "STRINGIFY(0)";\n");
	fprintf(f, "const float FILE_APPEND		= "STRINGIFY(1)";\n");
	fprintf(f, "const float FILE_WRITE		= "STRINGIFY(2)";\n");

	//this is annoying. builtins from pr_cmds.c are not known here.
	if (targs & (CS|SS))
	{
		const char *conflictprefix = "";//(targs&CS)?"":"//";
		fprintf(f, "\n\n//Vanilla Builtin list (reduced, so as to avoid conflicts)\n");
		fprintf(f, "void(vector) makevectors = #1;\n");
		fprintf(f, "void(entity,vector) setorigin = #2;\n");
		fprintf(f, "void(entity,string) setmodel = #3;\n");
		fprintf(f, "void(entity,vector,vector) setsize = #4;\n");
		fprintf(f, "float() random = #7;\n");
		fprintf(f, "%svoid(entity e, float chan, string samp, float vol, float atten, optional float speedpct, optional float flags, optional float timeofs) sound = #8;\n", conflictprefix);
		fprintf(f, "vector(vector) normalize = #9;\n");
		fprintf(f, "void(string e) error = #10;\n");
		fprintf(f, "void(string n) objerror = #11;\n");
		fprintf(f, "float(vector) vlen = #12;\n");
		fprintf(f, "float(vector fwd) vectoyaw = #13;\n");
		fprintf(f, "entity() spawn = #14;\n");
		fprintf(f, "void(entity e) remove = #15;\n");
		fprintf(f, "void(vector v1, vector v2, float flags, entity ent) traceline = #16;\n");
		if (targs&SS)
			fprintf(f, "entity() checkclient = #17;\n");
		fprintf(f, "entity(entity start, .string fld, string match) find = #18;\n");
		fprintf(f, "string(string s) precache_sound = #19;\n");
		fprintf(f, "string(string s) precache_model = #20;\n");
		if (targs&SS)
			fprintf(f, "%svoid(entity client, string s) stuffcmd = #21;\n", conflictprefix);
		fprintf(f, "%sentity(vector org, float rad, optional .entity chainfield) findradius = #22;\n", conflictprefix);
		if (targs&SS)
		{
			fprintf(f, "%svoid(string s, optional string s2, optional string s3, optional string s4, optional string s5, optional string s6, optional string s7, optional string s8) bprint = #23;\n", conflictprefix);
			fprintf(f, "%svoid(entity pl, string s, optional string s2, optional string s3, optional string s4, optional string s5, optional string s6, optional string s7) sprint = #24;\n", conflictprefix);
		}
		fprintf(f, "void(string,...) dprint = #25;\n");
		fprintf(f, "string(float) ftos = #26;\n");
		fprintf(f, "string(vector) vtos = #27;\n");
		fprintf(f, "%sfloat(float yaw, float dist, optional float settraceglobals) walkmove = #32;\n", conflictprefix);
		fprintf(f, "float() droptofloor = #34;\n");
		fprintf(f, "%svoid(float lightstyle, string stylestring, optional vector rgb) lightstyle = #35;\n", conflictprefix);
		fprintf(f, "float(float n) rint = #36;\n");
		fprintf(f, "float(float n) floor = #37;\n");
		fprintf(f, "float(float n) ceil = #38;\n");
		fprintf(f, "float(entity e) checkbottom = #40;\n");
		fprintf(f, "float(vector point) pointcontents = #41;\n");
		fprintf(f, "float(float n) fabs = #43;\n");
		if (targs&SS)
			fprintf(f, "vector(entity e, float speed) aim = #44;\n");
		fprintf(f, "float(string) cvar = #45;\n");
		fprintf(f, "void(string,...) localcmd = #46;\n");
		fprintf(f, "entity(entity) nextent = #47;\n");
		fprintf(f, "void(vector o, vector d, float color, float count) particle = #48;\n");
		fprintf(f, "void() changeyaw = #49;\n");
		fprintf(f, "%svector(vector fwd, optional vector up) vectoangles = #51;\n", conflictprefix);
		if (targs&SS)
		{
			fprintf(f, "void(float to, float val) WriteByte = #52;\n");
			fprintf(f, "void(float to, float val) WriteChar = #53;\n");
			fprintf(f, "void(float to, float val) WriteShort = #54;\n");
			fprintf(f, "void(float to, float val) WriteLong = #55;\n");
			fprintf(f, "void(float to, float val) WriteCoord = #56;\n");
			fprintf(f, "void(float to, float val) WriteAngle = #57;\n");
			fprintf(f, "void(float to, string val) WriteString = #58;\n");
			fprintf(f, "void(float to, entity val) WriteEntity = #59;\n");
		}
		fprintf(f, "void(float step) movetogoal = #67;\n");
		fprintf(f, "string(string s) precache_file = #68;\n");
		fprintf(f, "void(entity e) makestatic = #69;\n");
		if (targs&SS)
			fprintf(f, "void(string mapname, optional string newmapstartspot) changelevel = #70;\n");
		fprintf(f, "void(string var, string val) cvar_set = #72;\n");
		if (targs&SS)
			fprintf(f, "void(entity ent, string text, optional string text2, optional string text3, optional string text4, optional string text5, optional string text6, optional string text7) centerprint = #73;\n");
		fprintf(f, "void (vector pos, string samp, float vol, float atten) ambientsound = #74;\n");
		fprintf(f, "string(string str) precache_model2 = #75;\n");
		fprintf(f, "string(string str) precache_sound2 = #76;\n");
		fprintf(f, "string(string str) precache_file2 = #77;\n");
		if (targs&SS)
			fprintf(f, "void(entity player) setspawnparms = #78;\n");
	}
	if (targs & MN)
	{
		fprintf(f, "void(string e) error = #2;\n");
		fprintf(f, "void(string n) objerror = #3;\n");
		fprintf(f, "float(vector) vlen = #9;\n");
		fprintf(f, "float(vector fwd) vectoyaw = #10;\n");
		fprintf(f, "vector(vector fwd, optional vector up) vectoangles = #11;\n");
		fprintf(f, "float() random = #12;\n");
		fprintf(f, "void(string,...) localcmd = #13;\n");
		fprintf(f, "float(string) cvar = #14;\n");
		fprintf(f, "void(string var, string val) cvar_set = #15;\n");
		fprintf(f, "void(string,...) dprint = #16;\n");
		fprintf(f, "string(float) ftos = #17;\n");
		fprintf(f, "float(float n) fabs = #18;\n");
		fprintf(f, "string(vector) vtos = #19;\n");
		fprintf(f, "entity() spawn = #22;\n");
		fprintf(f, "void(entity e) remove = #23;\n");
		fprintf(f, "entity(entity start, .string fld, string match) find = #24;\n");
		fprintf(f, "string(string s) precache_file = #28;\n");
		fprintf(f, "string(string s) precache_sound = #29;\n");
		fprintf(f, "float(float n) rint = #34;\n");
		fprintf(f, "float(float n) floor = #35;\n");
		fprintf(f, "float(float n) ceil = #36;\n");
		fprintf(f, "entity(entity) nextent = #37;\n");
		fprintf(f, "float() clientstate = #62;\n");
	}

	for (j = 0; j < 2; j++)
	{
		if (j)
			fprintf(f, "\n\n//Builtin Stubs List (these are present for simpler compatibility, but not properly supported in QuakeSpasm at this time).\n/*\n");
		else
			fprintf(f, "\n\n//Builtin list\n");
		for (i = 0; i < sizeof(extensionbuiltins)/sizeof(extensionbuiltins[0]); i++)
		{
			if ((targs & CS) && extensionbuiltins[i].csqcfunc)
				;
			else if ((targs & SS) && extensionbuiltins[i].ssqcfunc)
				;
			else if ((targs & MN) && extensionbuiltins[i].menufunc)
				;
			else
				continue;

			if (j != (extensionbuiltins[i].desc?!strncmp(extensionbuiltins[i].desc, "stub.", 5):0))
				continue;
			fprintf(f, "%s %s = #%i;", extensionbuiltins[i].typestr, extensionbuiltins[i].name, extensionbuiltins[i].documentednumber);
			if (extensionbuiltins[i].desc && !j)
			{
				const char *line = extensionbuiltins[i].desc;
				const char *term;
				fprintf(f, " /*");
				while(*line)
				{
					fprintf(f, "\n\t\t");
					term = line;
					while(*term && *term != '\n')
						term++;
					fwrite(line, 1, term - line, f);
					if (*term == '\n')
					{
						term++;
					}
					line = term;
				}
				fprintf(f, " */\n\n");
			}
			else
				fprintf(f, "\n");
		}
		if (j)
			fprintf(f, "*/\n");
	}

	if (targs & 2)
	{
		for (i = 0; i < MAX_KEYS; i++)
		{
			const char *k = Key_KeynumToString(i);
			if (!k[0] || !k[1] || k[0] == '<')
				continue;	//skip simple keynames that can be swapped with 'k', and <invalid> keys.
			fprintf(f, "const float K_%s = %i;\n", k, Key_NativeToQC(i));
		}
	}


	fprintf(f, "\n\n//Reset this back to normal.\n");
	fprintf(f, "#pragma noref 0\n");
	fclose(f);
}
