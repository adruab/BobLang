#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <errno.h>

#include <ffi.h>
#if PLATFORM_OSX
#include <dlfcn.h>
#endif

#if WIN32
#include <intrin.h>
#endif

// Compilation phases
// Parse to AST, no types
// Typecheck and generate bytecode, recursing into whatever needs resolving
//  1. When entering a scope, collect all local definitions with no types.
//  2. Choose something to evaluate (e.g. top down).
//  3. While evaluating that resolve types, and possibly code gen for remaining requirements.

#if WIN32
#define BREAK_ALWAYS() __debugbreak()
#define PUSH_MSVC_WARNING_DISABLE(n) \
	__pragma(warning(push)) \
    __pragma(warning(disable:n))
#define POP_MSVC_WARNING()	__pragma(warning(pop))
#else
#define BREAK_ALWAYS() __builtin_trap()
#define PUSH_MSVC_WARNING_DISABLE(n)
#define POP_MSVC_WARNING()
#endif

#define ASSERTCHZ(f, ...) \
    PUSH_MSVC_WARNING_DISABLE(4127) \
    do { \
  		if (!(f)) { \
  			fprintf(stderr, "%s:%d assert failed: ", __FILE__, __LINE__); \
  			fprintf(stderr, __VA_ARGS__); \
  			fprintf(stderr, "\n"); \
  			fflush(stderr); \
  			BREAK_ALWAYS(); \
  		} \
    } while(0) \
    POP_MSVC_WARNING()
#define ASSERT(f) ASSERTCHZ(f, "%s", #f)

#define VERIFY(f) ASSERT(f)    
#define CASSERT(f) static_assert(f, #f " failed")

#define ClearStruct(p) memset(p, 0, sizeof(*p))
#define DIM(a) (sizeof(a)/sizeof(a[0]))


void ShowErrVa(const char * pChzFormat, va_list va)
{
	vfprintf(stderr, pChzFormat, va);
	fprintf(stderr, "\n");

	exit(-1);
}

void ShowErrRaw(const char * pChzFormat, ...)
{
	va_list va;
	va_start(va, pChzFormat);

	ShowErrVa(pChzFormat, va);

	va_end(va);
}

void ShowHelp()
{
	printf(
		"Syntax:\n"
		"jtoy filename\n");
}

// BB (adrianb) Load file in pages?

char * PchzLoadWholeFile(const char * pChzFile)
{
	FILE * pFile = fopen(pChzFile, "rb");
	if (!pFile)
	{
		ShowErrRaw("Can't open file %s (err %d)", pChzFile, errno);
		return nullptr;
	}

	fseek(pFile, 0, SEEK_END);
	size_t cB = ftell(pFile);
	fseek(pFile, 0, SEEK_SET);

	// BB (adrianb) Use a different allocation scheme?
	char * pChzContents = static_cast<char *>(malloc(cB + 1));
	size_t cBRead = fread(pChzContents, 1, cB, pFile);
	if (cB != cBRead)
	{
		ShowErrRaw("Can't read file %s", pChzFile);
		free(pChzContents);
		return nullptr;
	}

	pChzContents[cB] = '\0';

	fclose(pFile);

	return pChzContents;
}

enum TOKK
{
	TOKK_Invalid,
	TOKK_EndOfFile,

	TOKK_Semicolon,
	TOKK_OpenParen,
	TOKK_CloseParen,
	TOKK_OpenBrace,
	TOKK_CloseBrace,
	TOKK_OpenBracket,
	TOKK_CloseBracket,
	TOKK_Comma,

	// AST tokens

	TOKK_Keyword,
	TOKK_Identifier,
	TOKK_Operator, // Includes colon and things like that
	TOKK_Comment,

	TOKK_StringLiteral,
	TOKK_IntLiteral,
	TOKK_BoolLiteral,
	TOKK_FloatLiteral,
	
	TOKK_Max
};

const char * PchzFromTokk(TOKK tokk)
{
	static const char * s_mpTokkPchz[] =
	{
		"Invalid",
		"EndOfFile",

		"Semicolon",
		"OpenParen",
		"CloseParen",
		"OpenBrace",
		"CloseBrace",
		"OpenBracket",
		"CloseBracket",
		"Comma",

		"Keyword",
		"Identifier",
		"Operator",
		"Comment",
		
		"StringLiteral",
		"IntLiteral",
		"BoolLiteral",
		"FloatLiteral",
	};
	CASSERT(DIM(s_mpTokkPchz) == TOKK_Max);
	ASSERT(tokk >= 0 && tokk < TOKK_Max);

	return s_mpTokkPchz[tokk];
}

enum KEYWORD
{
	KEYWORD_Invalid,

	KEYWORD_If,
	KEYWORD_Else,
	KEYWORD_While,
	KEYWORD_For,

	KEYWORD_Struct,
	KEYWORD_Enum,

	KEYWORD_Using,
	KEYWORD_Cast,
	KEYWORD_Defer,

	KEYWORD_New,
	KEYWORD_Delete,
	KEYWORD_Remove, // BB (adrianb) This seems pretty terrible to be removing from normal usage...

	KEYWORD_Return,

	KEYWORD_ImportDirective,
	KEYWORD_RunDirective,
	KEYWORD_CharDirective,
	KEYWORD_ForeignDirective,
	
	KEYWORD_Max
};

const char * PchzFromKeyword(KEYWORD keyword)
{
	static const char * s_mpKeywordPchz[] =
	{
		"<invalid>",
		"if",
		"else",
		"while",
		"for",
		"struct",
		"enum",
		"using",
		"cast",
		"defer",
		"new",
		"delete",
		"remove",
		"return",

		"#import", // BB (adrianb) Are these really keywords?
		"#run",
		"#char",
		"#foreign",
	};
	CASSERT(DIM(s_mpKeywordPchz) == KEYWORD_Max);
	ASSERT(keyword >= 0 && keyword < KEYWORD_Max);

	return s_mpKeywordPchz[keyword];
}

KEYWORD KeywordFromPchz(const char * pChz)
{
	for (int keyword = KEYWORD_Invalid + 1; keyword < KEYWORD_Max; ++keyword)
	{
		if (strcmp(PchzFromKeyword(KEYWORD(keyword)), pChz) == 0)
			return KEYWORD(keyword);
	}

	return KEYWORD_Invalid;
}

template <class T>
T Max(const T & t0, const T & t1)
{
	return (t0 < t1) ? t1 : t0;
}

struct SErrorInfo
{
	const char * pChzLine;
	const char * pChzFile;
	int nLine;
	int nCol;
	int iChMic;
	int iChMac;
};

void ShowErr(const SErrorInfo & errinfo, const char * pChzFormat, ...)
{
	fprintf(stderr, "%s:%d:%d : error: ", errinfo.pChzFile, errinfo.nLine, errinfo.nCol);

	va_list va;
	va_start(va, pChzFormat);

	vfprintf(stderr, pChzFormat, va);
	fprintf(stderr, "\n");

	char aChzLine[1024];
	char aChzHighlight[DIM(aChzLine)];
	{
		int iChMic = Max(0, errinfo.iChMic - 100);
		const char * pChzLineTrim = errinfo.pChzLine + iChMic;
		int cChMax = errinfo.iChMac + 32 - iChMic;
		int iChOut = 0;
		bool fFound = false;
		for (int iCh = 0; iCh < cChMax && iChOut < DIM(aChzLine) - 1; ++iCh)
		{
			char ch = pChzLineTrim[iCh];
			if (ch == '\r' || ch == '\n')
			{
				cChMax = iChOut;
				break;
			}

			int iChAbs = iCh + iChMic;
			char chHighlight = (iChAbs >= errinfo.iChMic && iChAbs < errinfo.iChMac) ? '~' : ' ';

			if (ch == '\t')
			{
				for (int i = iChOut % 4; i < 4 && iChOut < DIM(aChzLine) - 1; ++i)
				{
					aChzLine[iChOut] = ' ';
					aChzHighlight[iChOut] = chHighlight;
					++iChOut;
				}
			}
			else
			{
				aChzLine[iChOut] = ch;
				aChzHighlight[iChOut] = chHighlight;
				++iChOut;
			}
		}

		ASSERT(iChOut < DIM(aChzLine));
		aChzLine[iChOut] = '\0';
		aChzHighlight[iChOut] = '\0';
	}

	fprintf(stderr, "    %s\n    %s\n", aChzLine, aChzHighlight);
	fflush(stderr);

	exit(-1);

	va_end(va);
}

template <class T>
struct SArray
{
	T *	a;
	int c;
	int cMax;

	T & operator [](int i) const
			{ 
				ASSERT(i >= 0 && i < this->c);
				return this->a[i];
			}

	T * begin() const
			{ return a; }
	T * end() const
			{ return a + c; }
};

template <class T>
void Reserve(SArray<T> * pAry, int cMaxNew)
{
	if (cMaxNew <= pAry->cMax)
		return;

	pAry->a = static_cast<T *>(realloc(pAry->a, cMaxNew * sizeof(T)));
	pAry->cMax = cMaxNew;
}

template <class T>
T * PtAppendNew(SArray<T> * pAry, int c = 1)
{
	int cNew = pAry->c + c;
	if (cNew > pAry->cMax)
	{
        int cMaxNew;
        int cMaxOld = pAry->cMax;
        do
        {
	        if (cMaxOld <= 0)
				cMaxNew = 8;
			else if (cNew < 1024)
				cMaxNew = cMaxOld * 2;
			else
				cMaxNew = (cMaxOld * 5) / 4;

			cMaxOld = cMaxNew;
		} while (cMaxNew < cNew);

		// BB (adrianb) Alignment?

		Reserve(pAry, cMaxNew);
	}

	T * pT = pAry->a + pAry->c;
	memset(pT, 0, c * sizeof(T));
	pAry->c += c;

	return pT;
}

template <class T>
void SetSizeAtLeast(SArray<T> * pAry, int c)
{
	if (c > pAry->c)
		(void) PtAppendNew(pAry, c - pAry->c);
}

template <class T>
bool FIsEmpty(const SArray<T> * pAry)
{
	return pAry->c == 0;
}

template <class T>
void Pop(SArray<T> * pAry)
{
	ASSERT(pAry->c > 0);
	--pAry->c;
}

template <class T>
void RemoveFront(SArray<T> * pAry, int c)
{
	ASSERT(c > 0);
	ASSERT(pAry->c >= c);
    int cNew = pAry->c - c;
	for (int i = 0; i < cNew; ++i)
	{
		pAry->a[i] = pAry->a[i + c];
	}

	pAry->c -= c;
}

template <class T>
T & Tail(SArray<T> * pAry, int i = 0)
{
	ASSERT(i >= 0 && i < pAry->c);
	return pAry->a[pAry->c - i - 1];
}

template <class T>
int IFromP(const SArray<T> * pAry, const T * p)
{
	int i = p - pAry->a;
	ASSERT(i >= 0 && i < pAry->c);
	return i;
}

inline bool FIsPowerOfTwo(int64_t n)
{
	return (n & (n - 1)) == 0;
}

struct SPagedAlloc
{
	SArray<uint8_t *> arypB;
	uint32_t iB;
	uint32_t cBPage;
};

void Init(SPagedAlloc * pPagealloc, uint32_t cBPageDefault)
{
	ClearStruct(pPagealloc);
	pPagealloc->cBPage = cBPageDefault;
    pPagealloc->iB = cBPageDefault + 1;
}

void * PvAlloc(SPagedAlloc * pPagealloc, size_t cB, size_t cBAlign)
{
	ASSERT(cBAlign <= 16 && FIsPowerOfTwo(cBAlign));
	ASSERT(cB <= pPagealloc->cBPage);
	uint32_t iB = (pPagealloc->iB + (cBAlign - 1)) & ~(cBAlign - 1);
	uint32_t iBMac = iB + cB;

	if (iBMac > pPagealloc->cBPage)
	{
		void * pVAlloc = malloc(pPagealloc->cBPage);
		ASSERT((intptr_t(pVAlloc) & 0xf) == 0);
		*PtAppendNew(&pPagealloc->arypB) = static_cast<uint8_t *>(pVAlloc);
		iB = 0;
		iBMac = cB;
	}

	pPagealloc->iB = iBMac;

	void * pV = Tail(&pPagealloc->arypB) + iB;
    memset(pV, 0, cB);
    return pV;
}

template <class T>
struct SSetNode
{
	uint32_t hv;
	bool fFull;
	T t;
};

template <class T>
struct SSet
{
	SSetNode<T> * aNode;
	int c;
	int cMax;
};

template <class T>
void AddImpl(SSet<T> * pSet, uint32_t hv, const T & t)
{
	// BB (adrianb) Use hopscotch instead? http://en.wikipedia.org/wiki/Hash_table#Open_addressing

	int cMax = pSet->cMax;
	int iNodeBase = hv % cMax;
	int iNode = iNodeBase;

	ASSERT(pSet->c + 1 <= cMax);

	if (pSet->aNode[iNode].fFull)
	{
		for (int diNode = 0;; ++diNode)
		{
			ASSERT(diNode < cMax);
			iNode = (iNodeBase + diNode) % cMax;
			if (!pSet->aNode[iNode].fFull)
				break;
		}
	}

	auto pNode = &pSet->aNode[iNode];
	pNode->hv = hv;
	pNode->fFull = true;
	pNode->t = t;

	pSet->c += 1;
}

template <class T>
void EnsureCount(SSet<T> * pSet, int c)
{
	static float s_rResize = 0.7f;
	if (c < int(pSet->cMax * s_rResize))
		return;
	
	int cMax = pSet->cMax;
	auto aNode = pSet->aNode;

	pSet->c = 0;
	pSet->cMax = cMax + 256;
	pSet->aNode = static_cast<SSetNode<T> *>(calloc(pSet->cMax, sizeof(SSetNode<T>)));

	for (int iNode = 0; iNode < cMax; ++iNode)
	{
		if (aNode[iNode].fFull)
		{
			AddImpl(pSet, aNode[iNode].hv, aNode[iNode].t);
		}
	}

	free(aNode);
}

template <class T, class TOther>
const T * PtLookupImpl(SSet<T> * pSet, uint32_t hv, const TOther & t)
{
	int cMax = pSet->cMax;
	if (cMax == 0)
		return nullptr;
	
	int iNodeBase = hv % cMax;
	for (int diNode = 0;; ++diNode)
	{
		ASSERT(diNode < cMax);
		int iNode = (iNodeBase + diNode) % cMax;
		auto pNode = &pSet->aNode[iNode];
		if (!pNode->fFull)
			return nullptr;

		if (pNode->hv == hv && FIsKeyEqual(pNode->t, t))
			return &pNode->t;
	}
}

inline bool FIsLetter(char ch)
{
	return (ch >= 'a' && ch <= 'z') ||
			(ch >= 'A' && ch <= 'Z') ||
			(uint8_t(ch) >= 0x80 && uint8_t(ch) <= 0xff);
}

inline bool FIsDigit(char ch)
{
	return (ch >= '0' && ch <= '9');
}

inline bool FIsIdent(char ch)
{
	return ch == '#' || ch == '_' || FIsDigit(ch) || FIsLetter(ch);
}

struct SOperator
{
	const char * pChzOpChar;
	const char * pChzOpSpaceSeparated;
};

const SOperator g_aOperator[] =
{
	{}, // Arrow-like operators
	{}, // Assignment-like operators
	{"@:?"},
	{"", "or xor"},
	{"", "and"},
	{"=<>!", "in notin is isnot not of"},
	{"."},
	{"&"},
	{"+-|~"},
	{"*/%"},
	{"$^"}
};

const char * g_pChzOperatorAll = "@:?=<>!.&+-|~*/%$^";

int NOperatorLevel(const char * pChzOp)
{
	int cChOp = int(strlen(pChzOp));

	// Check for arrow-like

	if (cChOp > 1 && pChzOp[cChOp - 1] == '>')
	{
		return 0;
	}

	// Check for assignment-like operators

	const char * pChzNonAssign = "<>!=~?";
	if (pChzOp[cChOp - 1] == '=' && strchr(pChzNonAssign, pChzOp[0]) == nullptr)
	{
		return 1;
	}

	for (int iOperator = 0; iOperator < DIM(g_aOperator); ++iOperator)
	{
		const SOperator & operatorTest = g_aOperator[iOperator];

		// First letter defines an operator

		if (operatorTest.pChzOpChar && strchr(operatorTest.pChzOpChar, pChzOp[0]) != nullptr)
		{
			return iOperator;
		}

		// Or a list of all operators

		if (operatorTest.pChzOpSpaceSeparated)
		{
			const char * pChzWords = operatorTest.pChzOpSpaceSeparated;
			while (*pChzWords)
			{
				ASSERT(FIsIdent(*pChzWords));
				const char * pChzDelim = strchr(pChzWords, ' ');
				int cChWord = int((pChzDelim) ? pChzDelim - pChzWords : strlen(pChzWords));

				if (strncmp(pChzOp, pChzWords, cChWord) == 0)
					return iOperator;

				pChzWords += cChWord;
				while (*pChzWords == ' ')
					pChzWords++;
			}
		}
	}

	return -1;
}

inline bool FIsOperator(char ch)
{
	return strchr(g_pChzOperatorAll, ch) != nullptr;
}

inline int NLog2(int64_t n)
{
	ASSERT(n != 0);
#if WIN32
	unsigned long iBit = 0;
	_BitScanReverse64(&iBit, n);
	return int(iBit);
#else
	int cZero = __builtin_clz(n);
	return 31 - cZero;
#endif
}

struct SToken
{
	TOKK tokk;
	union
	{
		struct
		{
			const char * pChz;
		} ident;

		struct
		{
			const char * pChz;
		} strlit;

		struct
		{
			const char * pChz;
		} comment;

		struct
		{
			int64_t n; 		// Unsigned stored as signed
			int 	cBit;	// Should be 8, 16, 32, or 64
			bool 	fSigned;// Are we signed or unsigned?
		} intlit;

		struct
		{
			double 	g;
			int 	cBit;	// Should be 32, or 64
		} fltlit;

		KEYWORD keyword;

		struct
		{
			const char * pChz;
			int nLevel;
		} op;
	};

	bool fBeginLine;
	int	cSpace;
	SErrorInfo errinfo;
};

struct SParser
{
	SPagedAlloc pagealloc;
	SSet<const char *> setpChz;

	int cOperator;

	const char * pChzCurrent;
	SErrorInfo errinfo;
	bool fBeginLine;
	int cSpace;

	SArray<SToken> aryTokNext;
	bool fInToken;
	int cPeek;
};

void Init(SParser * pParser)
{
	ClearStruct(pParser);
	pParser->cOperator = DIM(g_aOperator);

	Init(&pParser->pagealloc, 64 * 1024);
}

void StartNewFile(SParser * pParser, const char * pChzFile, const char * pChzContents)
{
	pParser->errinfo.pChzLine = pChzFile;
	pParser->errinfo.pChzFile = pChzFile;
	pParser->errinfo.nLine = 1;
	pParser->errinfo.nCol = 1;
	pParser->pChzCurrent = pChzContents;
	pParser->fBeginLine = true;
}

inline char Ch(const SParser * pParser, int iCh = 0)
{
	return pParser->pChzCurrent[iCh]; 
}

inline bool FIsDone(const SParser * pParser)
{
	return Ch(pParser) == '\0';
}

inline bool FIsOperator(const SToken & tok, const char * pChz)
{
	return tok.tokk == TOKK_Operator && strcmp(tok.op.pChz, pChz) == 0;
}

inline void FillInErrInfo(SParser * pParser, SErrorInfo * pErrinfo)
{
	*pErrinfo = pParser->errinfo;
	pErrinfo->iChMic = pParser->pChzCurrent - pParser->errinfo.pChzLine;
	pErrinfo->iChMac = pErrinfo->iChMic + 1;
}

inline SToken * PtokStart(TOKK tokk, SParser * pParser)
{
	ASSERT(!pParser->fInToken);
	pParser->fInToken = true;

	SToken * pTok = PtAppendNew(&pParser->aryTokNext);
	pTok->tokk = tokk;
	pTok->fBeginLine = pParser->fBeginLine;
	pTok->cSpace = pParser->cSpace;
	FillInErrInfo(pParser, &pTok->errinfo);
	return pTok;
}

inline void EndToken(SToken * pTok, SParser * pParser)
{
	ASSERT(pTok == &Tail(&pParser->aryTokNext));
	pParser->fInToken = false;
	if (pParser->errinfo.pChzLine != pTok->errinfo.pChzLine)
	{
		pTok->errinfo.iChMac = 50000;
	}
	else
	{
		pTok->errinfo.iChMac = pParser->pChzCurrent - pParser->errinfo.pChzLine;
	}
}

struct SStringWithLength
{
	const char * pCh;
	int cCh;
};

uint32_t HvFromKey(const char * pCh, int cCh)
{
	uint32_t hv = 0;

	for (int iCh = 0; iCh < cCh && pCh[iCh]; ++iCh)
	{
		hv = hv * 2339 + pCh[iCh] * 251;
	}

	return hv;
}

bool FIsKeyEqual(const char * pChz, const SStringWithLength & strwl)
{
	return strncmp(pChz, strwl.pCh, strwl.cCh) == 0 && pChz[strwl.cCh] == 0;
}

inline const char * PchzCopy(SParser * pParser, const char * pCh, int64_t cCh)
{
	// NOTE (adrianb) Inlining PtLookupImpl to avoid making copy of the string up front.
	//  Have a LookupOther and pass pChz,cBStr?

	uint32_t hv = HvFromKey(pCh, cCh);
	SStringWithLength strwl = { pCh, int(cCh) };
	const char * const * ppChz = PtLookupImpl(&pParser->setpChz, hv, strwl);
	if (ppChz)
		return *ppChz;

	char * pChz = static_cast<char *>(PvAlloc(&pParser->pagealloc, cCh + 1, 1));
	memcpy(pChz, pCh, cCh);
	pChz[cCh] = '\0';

	EnsureCount(&pParser->setpChz, pParser->setpChz.c + 1);
	AddImpl<const char *>(&pParser->setpChz, hv, pChz);

	return pChz;
}

void AdvanceChar(SParser * pParser)
{
	char ch = *pParser->pChzCurrent;
	if (!ch)
		return;

	// BB (adrianb) Make this work with utf8.

	pParser->pChzCurrent++;

	if (ch == '\n')
	{
		pParser->errinfo.pChzLine = pParser->pChzCurrent;
		pParser->errinfo.nLine += 1;
		pParser->errinfo.nCol = 1;
		pParser->cSpace = 0;
		pParser->fBeginLine = true;
	}
	else if (ch == '\r')
	{
		// Don't count this as anything
	}
	else if (ch == '\t')
	{
		// Add 1-4 spaces
		int nColPrev = pParser->errinfo.nCol;
		pParser->errinfo.nCol = ((nColPrev + 4) & ~3);
		pParser->cSpace += pParser->errinfo.nCol - nColPrev;
	}
	else if (ch == ' ')
	{
		pParser->errinfo.nCol++;
	}
	else
	{
		pParser->errinfo.nCol++;
		pParser->cSpace = 0;
		pParser->fBeginLine = false;
	}
}

struct SSimpleTok
{
	char ch;
	TOKK tokk;
};

const SSimpleTok g_aStok[] =
{
	{ ';', TOKK_Semicolon },
	{ '(', TOKK_OpenParen },
	{ ')', TOKK_CloseParen } ,
	{ '{', TOKK_OpenBrace },
	{ '}', TOKK_CloseBrace } ,
	{ '[', TOKK_OpenBracket },
	{ ']', TOKK_CloseBracket } ,
	{ ',', TOKK_Comma }, 
};

TOKK TokkCheckSimpleToken(SParser * pParser)
{
	char chPeek = Ch(pParser);
	for (int iStok = 0; iStok < DIM(g_aStok); ++iStok)
	{
		if (chPeek == g_aStok[iStok].ch)
		{
			return g_aStok[iStok].tokk;
		}
	}

	return TOKK_Invalid;
}

int64_t NParseIntegerBase10(SParser * pParser)
{
	int64_t n = 0;
	for (; FIsDigit(Ch(pParser)); AdvanceChar(pParser))
	{
		// BB (adrianb) Check for overflow?

		n = n * 10 + (Ch(pParser) - '0');
	}

	return n;
}

int64_t NDigitBase16(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A';
	if (c >= 'a' && c <= 'f')
		return c - 'a';

	return -1;
}

int64_t NParseIntegerBase16(SParser * pParser)
{
	int64_t n = 0;
	for (;;)
	{
		int64_t nDigit = NDigitBase16(Ch(pParser));
		if (nDigit < 0)
			break;

		// BB (adrianb) Check for overflow

		n = n * 16 + nDigit;

		AdvanceChar(pParser);
	}

	return n;
}

int64_t NParseIntegerBase8(SParser * pParser)
{
	int64_t n = 0;
	for (;;)
	{
		int64_t nDigit = NDigitBase16(Ch(pParser));
		if (nDigit < 0)
			break;

		// BB (adrianb) Check for overflow

		n = n * 8 + nDigit;

		AdvanceChar(pParser);
	}

	return n;
}

void TokenizeInt(SParser * pParser)
{
	SToken * pTok = PtokStart(TOKK_IntLiteral, pParser);
	
	// Parse decimal
	// BB (adrianb) Support hex, octal, binary.

	switch (Ch(pParser))
	{
	case '0':
		{
			AdvanceChar(pParser);
			if (Ch(pParser) == 'x')
			{
                AdvanceChar(pParser);
				pTok->intlit.n = NParseIntegerBase16(pParser);
			}
			else
			{
				pTok->intlit.n = NParseIntegerBase8(pParser);
			}
		}
		break;

	default:
		pTok->intlit.n = NParseIntegerBase10(pParser);
		break;
	}

	// BB (adrianb) Do we have any way of specifying these?

	int64_t cBit = 64;
	bool fSigned = true;
	
	pTok->intlit.cBit = int(cBit);
	pTok->intlit.fSigned = fSigned;

	EndToken(pTok, pParser);
}

void TokenizeFloat(SParser * pParser)
{	
	// Parse decimal
	// BB (adrianb) Is there a safer way to parse a float?

	const char * pChzStart = pParser->pChzCurrent;
	const char * pChz = pChzStart;
	for (; *pChz && FIsDigit(*pChz); ++pChz)
	{
	}

	ASSERT(*pChz == '.');
	++pChz;

	for (; *pChz && FIsDigit(*pChz); ++pChz)
	{
	}

	if (*pChz == 'e' || *pChz == 'E')
	{
		++pChz;
		if (*pChz == '-' || *pChz == '+')
			++pChz;

		for (; *pChz && FIsDigit(*pChz); ++pChz)
		{
		}
	}

	int cCh = pChz - pChzStart;
	char * pChzFloat = static_cast<char *>(alloca(cCh + 1));
	memcpy(pChzFloat, pChzStart, cCh);
	pChzFloat[cCh] = '\0';

	for (int iCh = 0; iCh < cCh; ++iCh)
		AdvanceChar(pParser);

	double g = atof(pChzFloat);

	SToken * pTok = PtokStart(TOKK_FloatLiteral, pParser);

	char chNext = char(tolower(Ch(pParser)));
	int64_t cBit = 32;
	if (chNext == 'f')
	{
		AdvanceChar(pParser);
		cBit = NParseIntegerBase10(pParser);
		if (cBit != 32 && cBit != 64)
		{
			EndToken(pTok, pParser);
			ShowErr(pTok->errinfo, "Expected 32, or 64 for float literal suffix");
			return;
		}
	}
	
	pTok->fltlit.g = g;
	pTok->fltlit.cBit = int(cBit);

	EndToken(pTok, pParser);
}

void ParseToken(SParser * pParser)
{
	if (FIsDone(pParser))
	{
		SToken * pTok = PtokStart(TOKK_EndOfFile, pParser);
		EndToken(pTok, pParser);
		return;
	}

	// Consume all white space

	while (!FIsDone(pParser))
	{
		// Eat the rest of the line if we're a comment
		
		if (pParser->pChzCurrent[0] == '/' && pParser->pChzCurrent[1] == '/')
		{
			while(!FIsDone(pParser) && Ch(pParser) != '\n')
				AdvanceChar(pParser);
		}

		if (pParser->pChzCurrent[0] == '/' && pParser->pChzCurrent[1] == '*')
		{
			AdvanceChar(pParser);
			int cComment = 1;
			while (!FIsDone(pParser))
			{
				if (pParser->pChzCurrent[0] == '/' && pParser->pChzCurrent[1] == '*')
				{
					AdvanceChar(pParser);
					AdvanceChar(pParser);
					cComment += 1;
					continue;
				}

				if (pParser->pChzCurrent[0] == '*' && pParser->pChzCurrent[1] == '/')
				{
					AdvanceChar(pParser);
					AdvanceChar(pParser);
					cComment -= 1;
					if (cComment == 0)
						break;
					continue;
				}

				AdvanceChar(pParser);
			}
		}

		char ch = Ch(pParser);

		if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ')
			AdvanceChar(pParser);
		else
			break;
	}

	// What token should we start consuming?

	// BB (adrianb) Deal with utf8 characters (always a letter?)

	char chStart = Ch(pParser);
	if (chStart == '\0')
	{
	}
	else if (chStart == '#' || chStart == '_' || FIsLetter(chStart))
	{
		SToken * pTok = PtokStart(TOKK_Identifier, pParser);
		
		const char * pChzStart = pParser->pChzCurrent;
		for (; FIsIdent(Ch(pParser)); AdvanceChar(pParser))
		{
		}

		// BB (adrianb) Reuse strings in original source?

		const char * pChzIdent = PchzCopy(pParser, pChzStart, pParser->pChzCurrent - pChzStart);

		if (KEYWORD keyword = KeywordFromPchz(pChzIdent))
		{
			pTok->tokk = TOKK_Keyword;
			pTok->keyword = keyword;
		}
		else if (strcmp(pChzIdent, "false") == 0 || strcmp(pChzIdent, "true") == 0)
		{
			pTok->intlit.n = (strcmp(pChzIdent, "true") == 0);
			pTok->intlit.cBit = 1;
			pTok->tokk = TOKK_BoolLiteral;
		}
		else if (int nOpLevel = NOperatorLevel(pChzIdent))
		{
			pTok->op.pChz = pChzIdent;
			pTok->op.nLevel = nOpLevel;
		}
		else
		{
			pTok->ident.pChz = pChzIdent;
		}

		EndToken(pTok, pParser);
	}
	else if (FIsDigit(chStart))
	{
		// Check for floating point literal

		const char * pChz = pParser->pChzCurrent;
		for (; *pChz; ++pChz)
		{
			if (!FIsDigit(*pChz))
				break;
		}

		if (*pChz == '.' && !FIsOperator(pChz[1]))
		{
			TokenizeFloat(pParser);
		}
		else
		{
			TokenizeInt(pParser);
		}
	}
	else if (chStart == '"')
	{
		SToken * pTok = PtokStart(TOKK_StringLiteral, pParser);
		AdvanceChar(pParser);

		// BB (adrianb) Allow 
		char aCh[1024];
		int cCh = 0;

		while (!FIsDone(pParser))
		{
			ASSERT(cCh < DIM(aCh));

			char ch = Ch(pParser);
			if (ch == '"')
				break;
			
			if (ch == '\\')
			{
				AdvanceChar(pParser);

				char chEscape = 0;
				switch (Ch(pParser))
				{
				case 'n': chEscape = '\n'; break;
				case 't': chEscape = '\t'; break;
				case 'v': chEscape = '\v'; break;
				case 'b': chEscape = '\b'; break;
				case 'r': chEscape = '\r'; break;
				case 'f': chEscape = '\f'; break;
				case 'a': chEscape = '\a'; break;
				case '\\': chEscape = '\\'; break;
				case '?': chEscape = '?'; break;
				case '\'': chEscape = '\''; break;
				case '"': chEscape = '"'; break;
				case '0': chEscape = '\0'; break;
				case 'o': ASSERT(false); break;
				case 'x': ASSERT(false); break;
				case 'u': ASSERT(false); break;
				case 'U': ASSERT(false); break;
				}

				aCh[cCh++] = chEscape;
				AdvanceChar(pParser);
				continue;
			}

			if (ch == '\n')
			{
				ShowErr(pParser->errinfo, "Unterminated string");
				break;				
			}

			aCh[cCh++] = ch;

			AdvanceChar(pParser);
		}

		pTok->strlit.pChz = PchzCopy(pParser, aCh, cCh);
		AdvanceChar(pParser);

		EndToken(pTok, pParser);
	}
	else if (chStart == '/' && pParser->pChzCurrent[1] == '/')
	{
		SToken * pTok = PtokStart(TOKK_Comment, pParser);
		AdvanceChar(pParser);
	
		char * pChzComment = nullptr;
		int64_t cBComment = 0;

		for (;;)
		{
			const char * pChzStart = pParser->pChzCurrent;
			for (; !FIsDone(pParser); AdvanceChar(pParser))
			{
				char ch = Ch(pParser);
				if (ch == '\n' || ch == '\r')
					break;
			}

			ASSERT(!pChzComment || pChzComment[cBComment - 1] == '\n');
			int64_t cB = pParser->pChzCurrent - pChzStart + 1;
			pChzComment = static_cast<char *>(realloc(pChzComment, cBComment + cB));
			memcpy(pChzComment + cBComment, pChzStart, cB);
			cBComment += cB;
			pChzComment[cBComment - 1] = '\0';

			// Check if we want to merge this comment with the next line

			int cChNextComment = 0;
			for (const char * pChz = pParser->pChzCurrent; *pChz; ++pChz)
			{
				if (*pChz == ' ' || *pChz == '\t' || *pChz == '\r' || *pChz == '\n' || *pChz == '#')
				{
					++cChNextComment;
					if (*pChz == '#')
						break;
				}
				else
				{
					cChNextComment = 0;
					break;
				}
			}

			// Don't merge with next comment

			if (!cChNextComment)
				break;

			// Skip to next comment

			for (int iCh = 0; iCh < cChNextComment; ++iCh)
			{
				AdvanceChar(pParser);
			}

			pChzComment[cBComment - 1] = '\n';
		}

		pTok->comment.pChz = pChzComment;

		EndToken(pTok, pParser);
	}
	else if (TOKK tokk = TokkCheckSimpleToken(pParser))
	{
		SToken * pTok = PtokStart(tokk, pParser);
		AdvanceChar(pParser);
		EndToken(pTok, pParser);
	}
	else if (FIsOperator(chStart))
	{
		SToken * pTok = PtokStart(TOKK_Operator, pParser);

		const char * pChzStart = pParser->pChzCurrent;
		for (; FIsOperator(Ch(pParser)); AdvanceChar(pParser))
		{
		}

		const char * pChzOp = PchzCopy(pParser, pChzStart, pParser->pChzCurrent - pChzStart);
		pTok->op.pChz = pChzOp;
		pTok->op.nLevel = NOperatorLevel(pChzOp);
		ASSERT(pTok->op.nLevel >= 0);

		EndToken(pTok, pParser);
	}
	else
	{
		ShowErr(pParser->errinfo, "Unrecognized character to start token %c", chStart);
	}
}

SToken TokPeek(SParser * pParser, int iTokAhead = 0)
{
	pParser->cPeek += 1;
	ASSERT(pParser->cPeek < 100);

	while (iTokAhead >= pParser->aryTokNext.c)
	{
		ParseToken(pParser);
	}

	return pParser->aryTokNext[iTokAhead];
};

void ConsumeToken(SParser * pParser, int c = 1)
{
	RemoveFront(&pParser->aryTokNext, c);
	pParser->cPeek = 0;
}

bool FTryConsumeToken(SParser * pParser, TOKK tokk, SToken * pTok = nullptr)
{
	SToken tok = TokPeek(pParser);
	if (pTok)
		*pTok = tok;

	if (tok.tokk == tokk)
	{
		ConsumeToken(pParser);
		return true;
	}

	return false;
}

void ConsumeExpectedToken(SParser * pParser, TOKK tokk, SToken * pTok = nullptr)
{
	SToken tok;
	if (!FTryConsumeToken(pParser, tokk, &tok))
	{
		ShowErr(tok.errinfo, "Expected %s found %s", PchzFromTokk(tokk), PchzFromTokk(tok.tokk));
	}

	if (pTok)
		*pTok = tok;
}

bool FTryConsumeOperator(SParser * pParser, const char * pChzOp, SToken * pTok = nullptr)
{
	SToken tok = TokPeek(pParser);
	if (pTok)
		*pTok = tok;

	if (tok.tokk == TOKK_Operator && strcmp(tok.op.pChz, pChzOp) == 0)
	{
		ConsumeToken(pParser);
		return true;
	}

	return false;
}

void ConsumeExpectedOperator(SParser * pParser, const char * pChzOp)
{
	SToken tok;
	if (!FTryConsumeOperator(pParser, pChzOp, &tok))
	{
		ShowErr(tok.errinfo, "Expected operator %s", pChzOp);
	}
}

bool FTryConsumeKeyword(SParser * pParser, KEYWORD keyword, SToken * pTok = nullptr)
{
	SToken tok = TokPeek(pParser);
	if (pTok)
		*pTok = tok;

	if (tok.tokk == TOKK_Keyword && tok.keyword == keyword)
	{
		ConsumeToken(pParser);
		return true;
	}

	return false;
}



enum ASTTYPEK
{
	ASTTYPEK_Identifier, 	// Could be basic type or struct or similar
	ASTTYPEK_Pointer,		// *
	ASTTYPEK_Array,			// []
	ASTTYPEK_Procedure,		// () or (arg) or (arg) -> ret or (arg, arg...) -> ret, ret... etc.
	ASTTYPEK_Namespace,		// namespace.name

	// !!! Procedure type specification error: (func: (A,B)->B, name: A)
	//  Is B,name the return value or just B and , denotes next argument
};

struct SAstType
{
	ASTTYPEK asttypek;
};

struct SAstTypeIdentifier : public SAstType
{
	const char * pChzName; // Could be basic type struct or similar
};

struct SAstTypePointer : public SAstType
{
	SAstType * pAtypeInner;
	bool fSoa;
};

struct SAst;

struct SAstTypeArray : public SAstType
{
	SAst * pAstSize; 		// Int literal, identifier, namespace qualified value // BB (adrianb) Extend to statically evaluatable expressions?
	bool fDynamicallySized;	// Size of ..
	bool fSoa;

	SAstType * pAtypeInner;
};

struct SAstDeclaration;

struct SAstTypeProcedure : public SAstType
{
	SArray<SAstDeclaration> aryAdeclArg;
	SArray<SAstDeclaration> aryAdeclRet;
};

struct SAstTypeNamespace : public SAstType
{
	const char * pChzNamespace;
	SAstType * pAtypeInner;
};


#if 0 // in Compiler...
BLOCK     : 1,
LITERAL   : 2,
IDENT     : 3,
STATEMENT : 4,

OPERATOR_EXPRESSION : 5,
// 6 is currently unused.
PROCEDURE_CALL  : 7,
ARRAY_SUBSCRIPT : 8,
WHILE     : 9,
IF        : 10,
LOOP_CONTROL : 11, ??? for loop?
// 12 is currently unused.
REMOVE    : 13, looks like a way to mark that you've removed an entry in a for loop, ew :/
RETURN    : 14,
EACH      : 15, ??? for i : ...?

TYPE_DEFINITION : 16, ???
TYPE_INSTANTIATION : 17, ???
ENUM : 18,

PROCEDURE : 19,
STRUCT : 20,
COMMA_SEPARATED_ARGUMENTS : 21, these are embedded in function call and procedure right?
EXTRACT : 22, ??? Is this the a,b,c := multifunc()?
SEQUENCE : 23, ??? a..b?

NEW_OR_DELETE : 24, ? Why is this one thing?

DECLARATION : 25,

CAST_EXPRESSION : 26,
USING : 27,
DIRECTIVE_INLINE : 28, // So we haven't performed any inlining yet?

DIRECTIVE_IMPORT : 29,
DIRECTIVE_LOAD : 30,

DIRECTIVE_RUN : 31,
DIRECTIVE_CHECK_CALL : 32,
DIRECTIVE_ASSERT : 33,
DIRECTIVE_IF_DEFINED : 34,
DIRECTIVE_BAKE : 35,
DIRECTIVE_MODIFY : 36,
DIRECTIVE_FOREIGN_LIBRARY : 37,

SIZE_OR_TYPE_INFO : 38,
CONTEXT_OR_PUSH   : 39,

NOTE : 40,
#endif

enum ASTK
{
	ASTK_Invalid = 0,

	// Basic building blocks

	ASTK_StringLiteral,
	ASTK_IntLiteral,
	ASTK_FloatLiteral,
	ASTK_BoolLiteral,
	
	// Constructs

	ASTK_Block,
	ASTK_EmptyStatement,
	ASTK_Identifier,
	ASTK_Operator,

	ASTK_If,
	ASTK_While,
	ASTK_For,

	ASTK_Using,
	ASTK_Cast,
	ASTK_New,
	ASTK_Delete,
	ASTK_Remove,
	ASTK_Defer,

	ASTK_ArrayIndex,
	ASTK_Call,
	ASTK_Return,

	// Declarations

	ASTK_DeclareSingle,
	ASTK_DeclareMulti,

	ASTK_Struct,
	ASTK_Enum,
	ASTK_Procedure,

	// Directives

	ASTK_ImportDirective,
	ASTK_RunDirective,

	ASTK_Max,
};

const char * PchzFromAstk(ASTK astk)
{
	static const char * s_mpAstkPchz[] =
	{
		"Invalid",
		
		"StringLiteral",
		"IntLiteral",
		"FloatLiteral",
		"BoolLiteral",

		"Block",
		"EmptyStatement",
		"Indentifier",
		"Operator",

		"If",
		"While",
		"For",

		"Using",
		"Cast",
		"New",
		"Delete",
		"Remove",
		"Defer",

		"ArrayIndex",
		"Call",
		"Return",

		"DeclareSingle",
		"DeclareMulti",

		"Struct",
		"Enum",
		"Procedure",

		"ImportDirective",
		"RunDirective",
	};

	CASSERT(DIM(s_mpAstkPchz) == ASTK_Max);
	ASSERT(astk >= 0 && astk < ASTK_Max);

	return s_mpAstkPchz[astk];
}

struct SAst
{
	ASTK astk;
	SErrorInfo errinfo;
};

template <typename T>
T * AstCast(SAst * pAst)
{
	ASSERT(pAst->astk == T::s_astk);
	return static_cast<T *>(pAst);
}

template <typename T>
const T * AstCast(const SAst * pAst)
{
	ASSERT(pAst->astk == T::s_astk);
	return static_cast<const T *>(pAst);
}

struct SAstStringLiteral : public SAst
{
	static const ASTK s_astk = ASTK_StringLiteral;
	const char * pChz;
};

struct SAstIntLiteral : public SAst
{
	static const ASTK s_astk = ASTK_IntLiteral;
	int64_t n;
	int cBit;
	bool fSigned;
};

struct SAstFloatLiteral : public SAst
{
	static const ASTK s_astk = ASTK_FloatLiteral;
	double g;
	int cBit;
};

struct SAstBoolLiteral : public SAst
{
	static const ASTK s_astk = ASTK_BoolLiteral;
	bool f;
};

struct SAstBlock : public SAst
{
	static const ASTK s_astk = ASTK_Block;

	SArray<SAst *> arypAst;
};

struct SAstEmptyStatement : public SAst
{
	static const ASTK s_astk = ASTK_EmptyStatement;
};

struct SAstIdentifier : public SAst
{
	static const ASTK s_astk = ASTK_Identifier;
	const char * pChz;
};

struct SAstOperator : public SAst
{
	static const ASTK s_astk = ASTK_Operator;
	
	const char * pChzOp;
	SAst * pAstBefore; // If nullptr, we're a prefix operator
	SAst * pAstAfter;
};

struct SAstIf : public SAst
{
	static const ASTK s_astk = ASTK_If;
	
	SAst * pAstCondition;
	SAst * pAstPass;
	SAst * pAstElse;
};

struct SAstWhile : public SAst
{
	static const ASTK s_astk = ASTK_While;
	
	SAst * pAstCondition;
	SAst * pAstLoop;
};

struct SAstFor : public SAst
{
	static const ASTK s_astk = ASTK_For;
	
	bool fTakesPointer;
	SAst * pAstIter;
	SAst * pAstIterRight; // i : expr
	SAst * pAstLoop;

	// BB (adrianb) Add declaration for iteration variable? 
};

struct SAstDeclaration // tag = adecl
{
	const char * pChzName;
	SErrorInfo errinfo;
	bool fUsing;
	bool fVariable;
	SAstType * pAtype;
	SAst * pAstValue;

	// TODO Resolved type info?
};

struct SAstArrayIndex : public SAst
{
	static const ASTK s_astk = ASTK_ArrayIndex;

	SAst * pAstArray;
	SAst * pAstIndex;
};

struct SAstCall : public SAst
{
	static const ASTK s_astk = ASTK_Call;

	SAst * pAstFunc;
	SArray<SAst *> arypAstArgs;
};

struct SAstReturn : public SAst
{
	static const ASTK s_astk = ASTK_Return;

	SArray<SAst *> arypAstRet;
};

struct SAstUsing : public SAst
{
	static const ASTK s_astk = ASTK_Using;
	SAst * pAstExpr;
};

struct SAstCast : public SAst
{
	static const ASTK s_astk = ASTK_Cast;
	SAstType * pAtype;
	SAst * pAstExpr;
};

struct SAstNew : public SAst
{
	static const ASTK s_astk = ASTK_New;
	SAstType * pAtype;
	// BB (adrianb) For some reason some cases have call e.g. new Mutex("sound").
	//  AFAICT this doesn't do anything.  In the parse it will run a Call operation by default, which seems wrong.
};

struct SAstDelete : public SAst
{
	static const ASTK s_astk = ASTK_Delete;
	SAst * pAstExpr;
};

struct SAstRemove : public SAst
{
	static const ASTK s_astk = ASTK_Remove;
	SAst * pAstExpr; // BB (adrianb) Only ever an identifier?
};

struct SAstDefer : public SAst
{
	static const ASTK s_astk = ASTK_Defer;
	SAst * pAstStmt;
};

struct SAstDeclareSingle : public SAst
{
	static const ASTK s_astk = ASTK_DeclareSingle;
	SAstDeclaration adecl;
};

struct SAstDeclareMulti : public SAst
{
	static const ASTK s_astk = ASTK_DeclareMulti;

	struct SIdent
	{
		const char * pChzName;
		SErrorInfo errinfo;
	};

	bool fVariable;
	SArray<SIdent> aryIdent;
	SAst * pAstValue;
};

struct SAstStruct : public SAst
{
	static const ASTK s_astk = ASTK_Struct;

	const char * pChzName;
	SArray<SAstDeclaration> aryAdecl;
	SArray<SAst *> arypAstComplexDecl;
};

struct SAstEnum : public SAst
{
	static const ASTK s_astk = ASTK_Enum;

	struct SValue
	{
		const char * pChzName;
		SAst * pAstValue;
	};

	const char * pChzName;
	SAstType * pAtypeInternal;
	SArray<SValue> aryValue;
};

struct SAstProcedure : public SAst
{
	static const ASTK s_astk = ASTK_Procedure;

	const char * pChzName;
	SArray<SAstDeclaration> aryAdeclArg;
	SArray<SAstDeclaration> aryAdeclRet;

	bool fIsForeign;
	const char * pChzForeign;

	SAstBlock * pAstblock;
};

struct SAstImportDirective : public SAst
{
	static const ASTK s_astk = ASTK_ImportDirective;
	const char * pChzImport;
};

struct SAstRunDirective : public SAst
{
	static const ASTK s_astk = ASTK_RunDirective;
	SAst * pAstExpr;
};

template <typename T>
T * PastCreate(const SErrorInfo & errinfo)
{
	T * pAst = static_cast<T *>(calloc(sizeof(T), 1));
	pAst->astk = T::s_astk;
	pAst->errinfo = errinfo;
	return pAst;
}

SAst * PastCreate(const SToken & tok)
{
	switch (tok.tokk)
	{
	case TOKK_Identifier:
		{
			SAstIdentifier * pAstident = PastCreate<SAstIdentifier>(tok.errinfo);
			pAstident->pChz = tok.ident.pChz;
			return pAstident;
		}

	case TOKK_StringLiteral:
		{
			SAstStringLiteral * pAststrlit = PastCreate<SAstStringLiteral>(tok.errinfo);
			pAststrlit->pChz = tok.strlit.pChz;
			return pAststrlit;
		}

	case TOKK_BoolLiteral:
		{
			SAstBoolLiteral * pAstboollit = PastCreate<SAstBoolLiteral>(tok.errinfo);
			pAstboollit->f = tok.intlit.n != 0;
			return pAstboollit;
		}

	case TOKK_IntLiteral:
		{
			SAstIntLiteral * pAstintlit = PastCreate<SAstIntLiteral>(tok.errinfo);
			pAstintlit->n = tok.intlit.n;
			pAstintlit->cBit = tok.intlit.cBit;
			pAstintlit->fSigned = tok.intlit.fSigned;
			return pAstintlit;
		}

	case TOKK_FloatLiteral:
		{
			SAstFloatLiteral * pAstfltlit = PastCreate<SAstFloatLiteral>(tok.errinfo);
			pAstfltlit->g = tok.fltlit.g;
			pAstfltlit->cBit = tok.fltlit.cBit;
			return pAstfltlit;
		}

	default:
		ASSERT(false);
		return nullptr;
	}
}

SAst * PastParseStatement(SParser * pParser);
SAst * PastTryParseExpression(SParser * pParser);
SAstBlock * PastParseBlock(SParser * pParser);
SAstDeclaration AdeclParseOptionalName(SParser * pParser);
SAstType * PatypeParse(SParser * pParser);
SAst * PastTryParsePrimary(SParser * pParser);

SAst * PastTryParseAtom(SParser * pParser)
{
	SToken tok = TokPeek(pParser);

	switch (tok.tokk)
	{
		case TOKK_Identifier:
		case TOKK_StringLiteral:
		case TOKK_IntLiteral:
		case TOKK_BoolLiteral:
		case TOKK_FloatLiteral:
			ConsumeToken(pParser);
			return PastCreate(tok);

		default:
			return nullptr;
	}
}

SAst * PastParseExpression(SParser * pParser)
{
	SToken tok = TokPeek(pParser);
	SAst * pAst = PastTryParseExpression(pParser);
	if (!pAst)
	{
		ShowErr(tok.errinfo, "Expected expression");
	}

	return pAst;
}

SAst * PastParsePrimary(SParser * pParser)
{
	SToken tok = TokPeek(pParser);
	SAst * pAst = PastTryParsePrimary(pParser);
	if (!pAst)
		ShowErr(tok.errinfo, "Expected non-operator expression");
	return pAst;
}

SAst * PastTryParseSimplePrimary(SParser * pParser)
{
	SToken tok = {};

	if (FTryConsumeToken(pParser, TOKK_OpenParen))
	{
		// ( expression )

		SAst * pAst = PastParseExpression(pParser);
		ConsumeExpectedToken(pParser, TOKK_CloseParen);
		return pAst;
	}
	else if (FTryConsumeKeyword(pParser, KEYWORD_Cast, &tok))
	{
		auto pAstcast = PastCreate<SAstCast>(tok.errinfo);
		ConsumeExpectedToken(pParser, TOKK_OpenParen);
		pAstcast->pAtype = PatypeParse(pParser);
		ConsumeExpectedToken(pParser, TOKK_CloseParen);
		SToken tokNext = TokPeek(pParser);
		pAstcast->pAstExpr = PastParsePrimary(pParser);
		return pAstcast;
	}
	else if (FTryConsumeKeyword(pParser, KEYWORD_New, &tok))
	{
		auto pAstnew = PastCreate<SAstNew>(tok.errinfo);
		pAstnew->pAtype = PatypeParse(pParser);
		return pAstnew;
	}
	else if (FTryConsumeKeyword(pParser, KEYWORD_Delete, &tok))
	{
		auto pAstdelete = PastCreate<SAstDelete>(tok.errinfo);
		pAstdelete->pAstExpr = PastParsePrimary(pParser);
		return pAstdelete;
	}
	else if (FTryConsumeKeyword(pParser, KEYWORD_Remove, &tok))
	{
		auto pAstremove = PastCreate<SAstRemove>(tok.errinfo);
		pAstremove->pAstExpr = PastParsePrimary(pParser);
		return pAstremove;
	}
	else if (FTryConsumeKeyword(pParser, KEYWORD_CharDirective, &tok))
	{
		// BB (adrianb) Make a different type for char?
		auto pAstintlit = PastCreate<SAstIntLiteral>(tok.errinfo);
		pAstintlit->cBit = 8;
		pAstintlit->fSigned = true; // BB (adrianb) Should char be signed or unsigned?

		ConsumeExpectedToken(pParser, TOKK_StringLiteral, &tok);
		pAstintlit->n = tok.strlit.pChz[0]; // BB (adrianb) Any need for utf8 shenanigans here?
		return pAstintlit;
	}
	else
	{
		return PastTryParseAtom(pParser);
	}
}

SAst * PastTryParsePrimary(SParser * pParser)
{
	SAst * pAstTop = nullptr;
	SAst ** ppAst = &pAstTop;

	// Parse any number of prefix operators

	SToken tok = {};
	while (FTryConsumeToken(pParser, TOKK_Operator, &tok))
	{
		auto pAstop = PastCreate<SAstOperator>(tok.errinfo);
		pAstop->pChzOp = tok.op.pChz;
		*ppAst = pAstop;
		ppAst = &pAstop->pAstAfter;
	}

	*ppAst = PastTryParseSimplePrimary(pParser);
    
    if (!pAstTop)
        return nullptr;

	// Parse post operations: function call, array index

	if (FTryConsumeToken(pParser, TOKK_OpenParen, &tok))
	{
		// stuff(arg[, arg]*)

		// TODO inline, other?
		// BB (adrianb) Try to point the error info at something other than the openning paren?

		auto pAstcall = PastCreate<SAstCall>(tok.errinfo);
		pAstcall->pAstFunc = *ppAst;
		*ppAst = pAstcall; // Function call has higher precidence so take the last thing and call on that

		if (TokPeek(pParser).tokk != TOKK_CloseParen)
		{
			for (;;)
			{
				*PtAppendNew(&pAstcall->arypAstArgs) = PastParseExpression(pParser);
				if (!FTryConsumeToken(pParser, TOKK_Comma))
					break;
			}
		}

		ConsumeExpectedToken(pParser, TOKK_CloseParen);
	}
	else if (FTryConsumeToken(pParser, TOKK_OpenBracket, &tok))
	{
		auto pAstarrayindex = PastCreate<SAstArrayIndex>(tok.errinfo);
		pAstarrayindex->pAstArray = *ppAst;
		*ppAst = pAstarrayindex;

		pAstarrayindex->pAstIndex = PastParseExpression(pParser);

		ConsumeExpectedToken(pParser, TOKK_CloseBracket);
	}

	return pAstTop;
}

SAst * PastTryParseBinaryOperator(SParser * pParser, int iOperator)
{
	if (iOperator >= pParser->cOperator)
	{
		return PastTryParsePrimary(pParser);
	}

	SAst * pAstLeft = PastTryParseBinaryOperator(pParser, iOperator + 1);
	if (!pAstLeft)
		return nullptr;
	
	// BB (adrianb) Could add right associativity the same way we do with prefix operators.

	for (;;)
	{
		// BB (adrianb) Detect newline here? Other wise *pN = 5\n *pN = 3 will parse to (= (* pN) (* 5 pN) ...

		SToken tok = TokPeek(pParser);
		if (tok.tokk != TOKK_Operator || iOperator != tok.op.nLevel)
			break;

#if 0
		// Don't count operator on next line as a new argument

		if (tok.fBeginLine)
			break;
#endif

		ConsumeToken(pParser);

		SAst * pAstRight = PastTryParseBinaryOperator(pParser, iOperator + 1);
		if (!pAstRight)
		{
			ShowErr(tok.errinfo, "Expected expression to follow operator %s", tok.op.pChz);
			return nullptr;
		}

		auto pAstop = PastCreate<SAstOperator>(tok.errinfo);
		pAstop->pChzOp = tok.op.pChz;
		pAstop->pAstBefore = pAstLeft;
		pAstop->pAstAfter = pAstRight;

		// Left associative

		pAstLeft = pAstop;
	}

	return pAstLeft;
}

SAst * PastTryParseExpression(SParser * pParser)
{
	SToken tok = {};
	if (FTryConsumeKeyword(pParser, KEYWORD_RunDirective, &tok))
	{
		auto pAstrun = PastCreate<SAstRunDirective>(tok.errinfo);
		SToken tokNext = TokPeek(pParser);
		if (tokNext.tokk == TOKK_OpenBrace)
		{
			pAstrun->pAstExpr = PastParseBlock(pParser);
		}
		else
		{
			pAstrun->pAstExpr = PastParseExpression(pParser);
		}
		return pAstrun;
	}

	return PastTryParseBinaryOperator(pParser, 0);
}

template <typename T>
T * PatypeCreate(ASTTYPEK asttypek)
{
	T * pT = static_cast<T *>(calloc(sizeof(T), 1));
	pT->asttypek = asttypek;
	return pT;
}

void ParseReturnValues(SParser * pParser, SArray<SAstDeclaration> * paryAdecl)
{
	// BB (adrianb) Special case void so we don't have to check for it later?

	SToken tokVoid = TokPeek(pParser);
	if (tokVoid.tokk == TOKK_Identifier && strcmp(tokVoid.ident.pChz, "void") == 0)
	{
		ConsumeToken(pParser);
		return;
	}
	
	for (;;)
	{
		*PtAppendNew(paryAdecl) = AdeclParseOptionalName(pParser);
		if (!FTryConsumeToken(pParser, TOKK_Comma))
			break;
	}
}

SAstType * PatypeParse(SParser * pParser)
{
    SToken tok = {};

	if (FTryConsumeOperator(pParser, "*"))
	{
		SAstTypePointer * pAtypepointer = PatypeCreate<SAstTypePointer>(ASTTYPEK_Pointer);

		SToken tokSoa = TokPeek(pParser);
		if (tokSoa.tokk == TOKK_Identifier && strcmp(tokSoa.ident.pChz, "SOA") == 0)
		{
			ConsumeToken(pParser);
			pAtypepointer->fSoa = true;
		}

		pAtypepointer->pAtypeInner = PatypeParse(pParser);
		return pAtypepointer;
	}
	else if (FTryConsumeToken(pParser, TOKK_OpenBracket))
	{
		auto pAtypearray = PatypeCreate<SAstTypeArray>(ASTTYPEK_Array);

		if (TokPeek(pParser).tokk != TOKK_CloseBracket)
		{
			if (FTryConsumeOperator(pParser, ".."))
			{
				pAtypearray->fDynamicallySized = true;
			}
			else
			{
				pAtypearray->pAstSize = PastParseExpression(pParser);
			}
		}
		ConsumeExpectedToken(pParser, TOKK_CloseBracket);

		SToken tokSoa = TokPeek(pParser);
		if (tokSoa.tokk == TOKK_Identifier && strcmp(tokSoa.ident.pChz, "SOA") == 0)
		{
			ConsumeToken(pParser);
			pAtypearray->fSoa = true;
		}

		pAtypearray->pAtypeInner = PatypeParse(pParser);

		return pAtypearray;
	}
	else if (FTryConsumeToken(pParser, TOKK_OpenParen))
	{
		auto pAtypproc = PatypeCreate<SAstTypeProcedure>(ASTTYPEK_Procedure);

		for (;;)
		{
			*PtAppendNew(&pAtypproc->aryAdeclArg) = AdeclParseOptionalName(pParser);
			if (!FTryConsumeToken(pParser, TOKK_Comma))
				break;
		}

		if (FTryConsumeOperator(pParser, "->"))
		{
			ParseReturnValues(pParser, &pAtypproc->aryAdeclRet);
		}

		return pAtypproc;
	}
	else if (FTryConsumeToken(pParser, TOKK_Identifier, &tok))
	{
		if (FTryConsumeOperator(pParser, "."))
		{
			auto pAtypns = PatypeCreate<SAstTypeNamespace>(ASTTYPEK_Namespace);
			pAtypns->pChzNamespace = tok.ident.pChz;
			pAtypns->pAtypeInner = PatypeParse(pParser);
			return pAtypns;
		}
		else
		{
			SAstTypeIdentifier * pAtypeident = PatypeCreate<SAstTypeIdentifier>(ASTTYPEK_Identifier);
			pAtypeident->pChzName = tok.ident.pChz;
			return pAtypeident;
		}
	}
	
	ShowErr(tok.errinfo, "Unexpected token for type declaration");
	return nullptr;
}

SAstDeclaration AdeclParseSimple(SParser * pParser)
{
	SAstDeclaration adecl = {};

	// BB (adrianb) Makes no sense for structs, procs kinda if we pass context.

	SToken tokIdent = TokPeek(pParser);
	bool fUsing = false;
	if (tokIdent.tokk == TOKK_Keyword && tokIdent.keyword == KEYWORD_Using)
	{
		ConsumeToken(pParser);
		adecl.fUsing = true;
		tokIdent = TokPeek(pParser);
	}

	SToken tokDefineOp = TokPeek(pParser, 1);

	if (tokIdent.tokk != TOKK_Identifier)
		ShowErr(tokIdent.errinfo, "Expected identifier at beginning of definition");

	if (tokDefineOp.tokk != TOKK_Operator)
		ShowErr(tokIdent.errinfo, "Expected : definition of some sort following identifier for definition");

	ConsumeToken(pParser, 2);

	adecl.pChzName = tokIdent.ident.pChz;

	const char * pChzColonOp = tokDefineOp.op.pChz;
	if (strcmp(pChzColonOp, ":") == 0)
	{
		// ident : type = value;

		adecl.fVariable = true;
		adecl.pAtype = PatypeParse(pParser);

		if (FTryConsumeOperator(pParser, "="))
		{
			adecl.pAstValue = PastParseExpression(pParser);
		}
		else if (FTryConsumeOperator(pParser, ":"))
		{
			// BB (adrianb) Does this syntax even make sense?

			adecl.fVariable = false;
			adecl.pAstValue = PastParseExpression(pParser);
		}
	}
	else if (strcmp(pChzColonOp, ":=") == 0 ||
			 strcmp(pChzColonOp, "::") == 0)
	{
		// ident := value

		adecl.fVariable = strcmp(pChzColonOp, ":=") == 0;
		adecl.pAstValue = PastParseExpression(pParser);
	}
	else
	{
		ShowErr(tokDefineOp.errinfo, "Unknown define operator");
	}

	return adecl;
}

SAstDeclaration AdeclParseOptionalName(SParser * pParser)
{
	// If we don't have ident : just check for a type

	SToken tokIdent = TokPeek(pParser);
	SToken tokDefine = TokPeek(pParser, 1);

	if (tokIdent.tokk != TOKK_Identifier || tokDefine.tokk != TOKK_Operator)
	{
		SAstDeclaration adecl = {};
		adecl.fVariable = true;
		adecl.pAtype = PatypeParse(pParser);
		return adecl;
	}	

	return AdeclParseSimple(pParser);
}

SAst * PastParseMultiDeclaration(SParser * pParser)
{
	auto pAstdecmul = PastCreate<SAstDeclareMulti>(TokPeek(pParser).errinfo); // BB (adrianb) Want the error info for all the identifiers?

	for (;;)
	{
		SToken tokIdent;
		ConsumeExpectedToken(pParser, TOKK_Identifier, &tokIdent);

		auto pIdent = PtAppendNew(&pAstdecmul->aryIdent);
		pIdent->pChzName = tokIdent.ident.pChz;
		pIdent->errinfo = tokIdent.errinfo;

		if (!FTryConsumeToken(pParser, TOKK_Comma))
			break;
	}

	if (!FTryConsumeOperator(pParser, "::"))
	{
		pAstdecmul->pAstValue = PastParseExpression(pParser);
	}
	else if (!FTryConsumeOperator(pParser, ":="))
	{
		pAstdecmul->fVariable = true;
		pAstdecmul->pAstValue = PastParseExpression(pParser);
	}

	return pAstdecmul;
}

SAst * PastTryParseDeclaration(SParser * pParser)
{
	int iTok = 0;
	SToken tokIdent = TokPeek(pParser);
	if (tokIdent.tokk == TOKK_Keyword && tokIdent.keyword == KEYWORD_Using)
	{
		iTok = 1;
		tokIdent = TokPeek(pParser, 1);
	}

	if (tokIdent.tokk != TOKK_Identifier)
		return nullptr;
	
	SToken tokDefine = TokPeek(pParser, iTok + 1);
	if (tokDefine.tokk == TOKK_Comma)
	{
		return PastParseMultiDeclaration(pParser);
	}
	else if (tokDefine.tokk == TOKK_Operator && *tokDefine.op.pChz == ':') // BB (adrianb) Check for exact set of operators?
	{
		auto pAstdec = PastCreate<SAstDeclareSingle>(tokDefine.errinfo);
		pAstdec->adecl = AdeclParseSimple(pParser);
		return pAstdec;
	}
	else
		return nullptr;
}

SAst * PastTryParseStructOrProcedureDeclaration(SParser * pParser)
{
	// TODO support using a procedure?

	SToken tokIdent = TokPeek(pParser);
	SToken tokDefineOp = TokPeek(pParser, 1);

	if (tokIdent.tokk == TOKK_Identifier && tokDefineOp.tokk == TOKK_Operator)
	{
		if (strcmp(tokDefineOp.op.pChz, "::") == 0)
		{
			// ident :: declare-value

			SToken tokValue = TokPeek(pParser, 2);
			if (tokValue.tokk == TOKK_OpenParen) // procedure
			{
				ConsumeToken(pParser, 3);

				auto pAstproc = PastCreate<SAstProcedure>(tokIdent.errinfo);
				pAstproc->pChzName = tokIdent.ident.pChz;

				if (TokPeek(pParser).tokk != TOKK_CloseParen)
				{
					for (;;)
					{
						*PtAppendNew(&pAstproc->aryAdeclArg) = AdeclParseSimple(pParser);
						if (!FTryConsumeToken(pParser, TOKK_Comma))
							break;
					}
				}

				ConsumeExpectedToken(pParser, TOKK_CloseParen);

				if (FTryConsumeOperator(pParser, "->"))
				{
					ParseReturnValues(pParser, &pAstproc->aryAdeclRet);
				}

				if (FTryConsumeKeyword(pParser, KEYWORD_ForeignDirective))
				{
					pAstproc->fIsForeign = true;

					SToken tokStr = {};
					if (FTryConsumeToken(pParser, TOKK_StringLiteral, &tokStr))
						pAstproc->pChzForeign = tokStr.strlit.pChz;

					ConsumeExpectedToken(pParser, TOKK_Semicolon);
				}
				else
				{
					pAstproc->pAstblock = PastParseBlock(pParser);
				}

				return pAstproc;
			}
			else if (tokValue.tokk == TOKK_Keyword && tokValue.keyword == KEYWORD_Struct)
			{
				ConsumeToken(pParser, 3);

				auto pAststruct = PastCreate<SAstStruct>(tokIdent.errinfo);
				pAststruct->pChzName = tokIdent.ident.pChz;

				// TODO: AOS, SOA.

				ConsumeExpectedToken(pParser, TOKK_OpenBrace);

				while (!FTryConsumeToken(pParser, TOKK_CloseBrace))
				{
					SAst * pAst = PastTryParseStructOrProcedureDeclaration(pParser);
					if (pAst)
					{
						*PtAppendNew(&pAststruct->arypAstComplexDecl) = pAst;
					}
					else
					{
						*PtAppendNew(&pAststruct->aryAdecl) = AdeclParseSimple(pParser);
					}
					ConsumeExpectedToken(pParser, TOKK_Semicolon);
				}

				return pAststruct;
			}
			else if (tokValue.tokk == TOKK_Keyword && tokValue.keyword == KEYWORD_Enum)
			{
				ConsumeToken(pParser, 3);

				auto pAstenum = PastCreate<SAstEnum>(tokIdent.errinfo);
				pAstenum->pChzName = tokIdent.ident.pChz;

				SToken tokType = TokPeek(pParser);
				if (tokType.tokk != TOKK_OpenBrace)
				{
					pAstenum->pAtypeInternal = PatypeParse(pParser);
				}

				ConsumeExpectedToken(pParser, TOKK_OpenBrace);

				while (TokPeek(pParser).tokk != TOKK_CloseBrace)
				{
					SToken tokIdent;
					ConsumeExpectedToken(pParser, TOKK_Identifier, &tokIdent);

					SAstEnum::SValue * pValue = PtAppendNew(&pAstenum->aryValue);
					pValue->pChzName = tokIdent.ident.pChz;

					if (FTryConsumeOperator(pParser, ":"))
					{
						pValue->pAstValue = PastParseExpression(pParser);
					}

					if (!FTryConsumeToken(pParser, TOKK_Comma))
						break;
				}

				ConsumeExpectedToken(pParser, TOKK_CloseBrace);

				// BB (adrianb) Require a semicolon?

				return pAstenum;
			}
		}
	}

	return nullptr;
}

SAst * PastTryParseStatement(SParser * pParser)
{
	SToken tok = TokPeek(pParser);
	
	if (tok.tokk == TOKK_OpenBrace)
	{
		return PastParseBlock(pParser);
	}

	// BB (adrianb) Should expressionify some of these.

	if (FTryConsumeKeyword(pParser, KEYWORD_If, &tok))
	{
		auto pAstif = PastCreate<SAstIf>(tok.errinfo);
		pAstif->pAstCondition = PastParseExpression(pParser);
		pAstif->pAstPass = PastParseStatement(pParser);

		if (FTryConsumeKeyword(pParser, KEYWORD_Else, &tok))
		{
			pAstif->pAstElse = PastParseStatement(pParser);
		}

		return pAstif;
	}
	else if (FTryConsumeKeyword(pParser, KEYWORD_While, &tok))
	{
		auto pAstwhile = PastCreate<SAstWhile>(tok.errinfo);
		pAstwhile->pAstCondition = PastParseExpression(pParser);
		pAstwhile->pAstLoop = PastParseStatement(pParser);
		return pAstwhile;
	}
	else if (FTryConsumeKeyword(pParser, KEYWORD_For, &tok))
	{
		auto pAstfor = PastCreate<SAstFor>(tok.errinfo);

		SToken tokIdent = TokPeek(pParser);
		SToken tokColon = TokPeek(pParser, 1);
		if (tokIdent.tokk == TOKK_Identifier && FIsOperator(tokColon, ":"))
		{
			pAstfor->pAstIter = PastCreate(tokIdent);
		}

		pAstfor->pAstIterRight = PastParseExpression(pParser);
		pAstfor->pAstLoop = PastParseStatement(pParser);

        return pAstfor;
	}
	else if (FTryConsumeKeyword(pParser, KEYWORD_Return, &tok))
	{
		auto pAstret = PastCreate<SAstReturn>(tok.errinfo);

		SAst * pAstRet = PastTryParseExpression(pParser);
		if (pAstRet)
		{
			*PtAppendNew(&pAstret->arypAstRet) = pAstRet;
			for (;;)
			{
				if (!FTryConsumeToken(pParser, TOKK_Comma))
					break;

				*PtAppendNew(&pAstret->arypAstRet) = PastParseExpression(pParser);
			}
		}
		
		ConsumeExpectedToken(pParser, TOKK_Semicolon);

		return pAstret;
	}
	else if (FTryConsumeKeyword(pParser, KEYWORD_Defer, &tok))
	{
		auto pAstdefer = PastCreate<SAstDefer>(tok.errinfo);
		pAstdefer->pAstStmt = PastParseStatement(pParser);
		return pAstdefer;
	}

	// TODO Recongize definition with ident(, ident)* followed by a := too.

	// Recognize definition [using] ident[, ident]* (:: or :=) etc.
	// BB (adrianb) Maybe using and , should just be lower priority operators for the parser?

	SAst * pAst = PastTryParseStructOrProcedureDeclaration(pParser);
	if (pAst)
		return pAst; // No semicolon needed

	pAst = PastTryParseDeclaration(pParser);

	// Check for using expr (e.g. using Enum.members) after d
	if (!pAst && FTryConsumeKeyword(pParser, KEYWORD_Using, &tok))
	{
		auto pAstusing = PastCreate<SAstUsing>(tok.errinfo);
		pAstusing->pAstExpr = PastParseExpression(pParser);
		pAst = pAstusing;
	}

	if (!pAst)
		pAst = PastTryParseExpression(pParser);

	if (pAst)
	{
		ConsumeExpectedToken(pParser, TOKK_Semicolon);
		return pAst;
	}
	
	if (FTryConsumeToken(pParser, TOKK_Semicolon, &tok))
	{
		return PastCreate<SAstEmptyStatement>(tok.errinfo);
	}

	return nullptr;
}

SAst * PastParseStatement(SParser * pParser)
{
	SToken tok = TokPeek(pParser);
	SAst * pAstStmt = PastTryParseStatement(pParser);
	if (!pAstStmt)
		ShowErr(tok.errinfo, "Expected statement");
	return pAstStmt;
}

SAstBlock * PastParseBlock(SParser * pParser)
{
	SToken tokOpen;
	ConsumeExpectedToken(pParser, TOKK_OpenBrace, &tokOpen);

	SAstBlock * pAstblock = PastCreate<SAstBlock>(tokOpen.errinfo);

	for (;;)
	{
		SToken tok = TokPeek(pParser);
		if (tok.tokk == TOKK_CloseBrace)
			break;

		*PtAppendNew(&pAstblock->arypAst) = PastParseStatement(pParser);
	}

	ConsumeExpectedToken(pParser, TOKK_CloseBrace);
	return pAstblock;
}

SAst * PastParseRoot(SParser * pParser)
{
	// BB (adrianb) Do we want a root scope?

	SToken tokScope = TokPeek(pParser);
	SAstBlock * pAstblock = PastCreate<SAstBlock>(tokScope.errinfo);

	for (;;)
	{
		SToken tok = {};
		if (FTryConsumeKeyword(pParser, KEYWORD_ImportDirective, &tok))
		{
			auto pAstimport = PastCreate<SAstImportDirective>(tokScope.errinfo);
			SToken tokString;
			ConsumeExpectedToken(pParser, TOKK_StringLiteral, &tokString);
			ConsumeExpectedToken(pParser, TOKK_Semicolon);

			pAstimport->pChzImport = tokString.strlit.pChz;
			
			// BB (adrianb) Verify this all happens on one line.
			
			*PtAppendNew(&pAstblock->arypAst) = pAstimport;
			continue;
		}
		else if (TokPeek(pParser).tokk == TOKK_EndOfFile)
		{
			break;
		}

		SAst * pAst = PastTryParseStatement(pParser);
		if (!pAst && !FTryConsumeToken(pParser, TOKK_Semicolon))
			break;

		*PtAppendNew(&pAstblock->arypAst) = pAst;
	}

	SToken tok = TokPeek(pParser);
	if (tok.tokk != TOKK_EndOfFile)
	{
		ShowErr(tok.errinfo, "Unexpected token %s", PchzFromTokk(tok.tokk));
	}

	return pAstblock;
}



void PrintEscapedString(const char * pChz)
{
	for (; *pChz; ++pChz)
	{
		switch (*pChz)
		{
		case '\n': printf("\\n"); break;
		case '\t': printf("\\t"); break;
		case '\v': printf("\\v"); break;
		case '\r': printf("\\r"); break;
		case '\f': printf("\\f"); break;
		case '\a': printf("\\a"); break;
		case '\\': printf("\\\\"); break;
		case '"': printf("\\\""); break;

		default:
			printf("%c", *pChz);
			break;
		}
	}
}

void PrintSimpleInlineAst(const SAst * pAst)
{
	switch (pAst->astk)
	{
	case ASTK_StringLiteral:
		PrintEscapedString(AstCast<SAstStringLiteral>(pAst)->pChz);
		break;

	case ASTK_IntLiteral:
		printf("0x%llx", AstCast<SAstIntLiteral>(pAst)->n);
		break;

	case ASTK_FloatLiteral:
		printf("%g", AstCast<SAstFloatLiteral>(pAst)->g);
		break;

	case ASTK_BoolLiteral:
		printf("%s", (AstCast<SAstBoolLiteral>(pAst)->f) ? "true" : "false");
		break;

	case ASTK_Identifier:
		printf("%s", AstCast<SAstIdentifier>(pAst)->pChz);
		break;

	case ASTK_Operator:
		{
			auto pAstop = AstCast<SAstOperator>(pAst);
			if (pAstop->pAstBefore)
				PrintSimpleInlineAst(pAstop->pAstBefore);
			printf("%s", pAstop->pChzOp);
			PrintSimpleInlineAst(pAstop->pAstAfter);
		}
		break;

	default:
		ASSERTCHZ(false, "Unhandled type in inline printer %s (%d)", PchzFromAstk(pAst->astk), pAst->astk);
		break;
	}
}

void PrintType(const SAstType * pAtype)
{
    switch (pAtype->asttypek) 
    {
	case ASTTYPEK_Identifier:
		{
			auto pAtypeident = static_cast<const SAstTypeIdentifier *>(pAtype);
			printf("%s", pAtypeident->pChzName);
		}
		break;

	case ASTTYPEK_Pointer:
		{
			printf("* ");
			PrintType(static_cast<const SAstTypePointer *>(pAtype)->pAtypeInner);
		}
		break;

	case ASTTYPEK_Array:
		{
			auto pAtypearray = static_cast<const SAstTypeArray *>(pAtype);
			if (pAtypearray->fDynamicallySized)
			{
				printf("[..]");
			}
			else
			{
				printf("[");
				if (pAtypearray->pAstSize)
					PrintSimpleInlineAst(pAtypearray->pAstSize);
				printf("] ");
			}
			PrintType(static_cast<const SAstTypeArray *>(pAtype)->pAtypeInner);
		}
		break;

	case ASTTYPEK_Namespace:
		{
			auto pAtypens = static_cast<const SAstTypeNamespace *>(pAtype);
			printf("%s.", pAtypens->pChzNamespace);
			PrintType(pAtypens->pAtypeInner);
		}
		break;

    default:
    	ASSERT(false);
        break;
    }
}

void PrintAst(int cScope, const SAst * pAst);

void PrintDeclaration(int cScopeValue, const SAstDeclaration & adecl)
{
	printf("%*s%s", cScopeValue * 2, "", (adecl.fUsing) ? "using " : "");
	if (adecl.pChzName)
		printf("%s :", adecl.pChzName);

	if (adecl.pAtype)
	{
		if (adecl.pChzName) 
			printf(" ");
		PrintType(adecl.pAtype);
		printf(" ");
	}

	// BB (adrianb) Print AST on a new line? if it's got any complexity that'd be ugly.
	if (adecl.pAstValue)
	{
		printf("%s\n", adecl.fVariable ? "=" : ":");
		PrintAst(cScopeValue + 1, adecl.pAstValue);
	}
	else
	{
		printf("\n");
	}
}

void PrintAst(int cScope, const SAst * pAst)
{
	if (!pAst)
		return;

	int cChSpace = cScope * 2;
	printf("%*s", cChSpace, "");

	printf("%s", PchzFromAstk(pAst->astk));
	switch (pAst->astk)
	{
	case ASTK_StringLiteral:
		{
			// BB (adrianb) Make this work with UTF8.
		
			printf(": \""); 
			PrintEscapedString(AstCast<SAstStringLiteral>(pAst)->pChz);
			printf("\"\n"); 
		}
		break;

	case ASTK_IntLiteral:
		{
			auto pAstintlit = AstCast<SAstIntLiteral>(pAst);
			printf(": 0x%llx (%s%d bits)\n", pAstintlit->n, (pAstintlit->fSigned) ? "signed ": "", pAstintlit->cBit);
		}
		break;

	case ASTK_FloatLiteral:
		{
			auto pAstfltlit = AstCast<SAstFloatLiteral>(pAst);
			printf(": %g (%d bits)\n", pAstfltlit->g, pAstfltlit->cBit);
		}
		break;

	case ASTK_BoolLiteral:
		printf(": %s\n", (AstCast<SAstBoolLiteral>(pAst)->f) ? "true" : "false");
		break;

	case ASTK_Block:
		{
			auto pAstblock = AstCast<SAstBlock>(pAst);
			printf("\n");
			for (SAst * pAst : pAstblock->arypAst)
			{
				PrintAst(cScope + 1, pAst);
			}
		}
		break;

	case ASTK_EmptyStatement:
		printf("\n");
		break;

	case ASTK_Identifier:
		printf(":%s\n", AstCast<SAstIdentifier>(pAst)->pChz);
		break;

	case ASTK_Operator:
		{
			auto pAstop = AstCast<SAstOperator>(pAst);
			printf(": %s\n", pAstop->pChzOp);
			if (pAstop->pAstBefore)
				PrintAst(cScope + 1, pAstop->pAstBefore);
			ASSERT(pAstop->pAstAfter);
			PrintAst(cScope + 1, pAstop->pAstAfter);
		}
		break;

	case ASTK_If:
		{
			auto pAstif = AstCast<SAstIf>(pAst);
			printf("\n%*scondition:\n", cChSpace + 1, "");
			PrintAst(cScope + 1, pAstif->pAstCondition);
			printf("%*spass:\n", cChSpace + 1, "");
			PrintAst(cScope + 1, pAstif->pAstPass);
			if (pAstif->pAstElse)
			{
				printf("%*selse:\n", cChSpace + 1, "");
				PrintAst(cScope + 1, pAstif->pAstElse);
			}
		}
		break;

	case ASTK_While:
		{
			auto pAstwhile = AstCast<SAstWhile>(pAst);
			printf("\n%*scondition:\n", cChSpace + 1, "");
			PrintAst(cScope + 1, pAstwhile->pAstCondition);
			printf("%*sloop:\n", cChSpace + 1, "");
			PrintAst(cScope + 1, pAstwhile->pAstLoop);
		}
		break;

	case ASTK_For:
		{
			auto pAstfor = AstCast<SAstFor>(pAst);
			printf("\n");
			if (pAstfor->pAstIter)
			{
				printf("%*siteration variable:\n", cChSpace + 1, "");
				PrintAst(cScope + 1, pAstfor->pAstIter);
			}

			printf("%*siterator:\n", cChSpace + 1, "");
			PrintAst(cScope + 1, pAstfor->pAstIterRight);
			printf("%*sloop:\n", cChSpace + 1, "");
			PrintAst(cScope + 1, pAstfor->pAstLoop);
		}
		break;

	case ASTK_Using:
		{
			auto pAstusing = AstCast<SAstUsing>(pAst);
			printf("\n");
			PrintAst(cScope + 1, pAstusing->pAstExpr);
		}
		break;

	case ASTK_Cast:
		{
			auto pAstcast = AstCast<SAstCast>(pAst);
			printf(": ");
			PrintType(pAstcast->pAtype);
			printf("\n%*sexpression:\n", cChSpace + 1, "");
			PrintAst(cScope + 1, pAstcast->pAstExpr);
		}
		break;

	case ASTK_New:
		{
			auto pAstnew = AstCast<SAstNew>(pAst);
			printf(": ");
			PrintType(pAstnew->pAtype);
			printf("\n");
		}
		break;

	case ASTK_Delete:
		{
			printf("\n");
			PrintAst(cScope + 1, AstCast<SAstDelete>(pAst)->pAstExpr);
		}
		break;

	case ASTK_Remove:
		{
			printf("\n");
			PrintAst(cScope + 1, AstCast<SAstRemove>(pAst)->pAstExpr);
		}
		break;

	case ASTK_Defer:
		{
			printf("\n");
			PrintAst(cScope + 1, AstCast<SAstDefer>(pAst)->pAstStmt);
		}
		break;

	case ASTK_ArrayIndex:
		{
			auto pAstarrayindex = AstCast<SAstArrayIndex>(pAst);
			printf("\n%*sarray:\n", cChSpace + 1, "");
			PrintAst(cScope + 1, pAstarrayindex->pAstArray);
			printf("%*sindex:\n", cChSpace + 1, "");
			PrintAst(cScope + 1, pAstarrayindex->pAstIndex);
		}
		break;

	case ASTK_Call:
		{
			auto pAstcall = AstCast<SAstCall>(pAst);
			printf("\n%*sfunc:\n", cChSpace + 1, "");
			PrintAst(cScope + 1, pAstcall->pAstFunc);
			if (pAstcall->arypAstArgs.c)
			{
				printf("%*sargs:\n", cChSpace + 1, "");
				for (auto pAst : pAstcall->arypAstArgs)
				{
					PrintAst(cScope + 1, pAst);
				}
			}
		}
		break;

	case ASTK_Return:
		{
			auto pAstreturn = AstCast<SAstReturn>(pAst);
			printf("\n");
			for (SAst * pAst : pAstreturn->arypAstRet)
			{
				PrintAst(cScope + 1, pAst);
			}
		}
		break;

	case ASTK_DeclareSingle:
		{
			printf("\n");
			PrintDeclaration(cScope + 1, AstCast<SAstDeclareSingle>(pAst)->adecl);
		}
		break;

	case ASTK_DeclareMulti:
		{
			auto pAstdeclmul = AstCast<SAstDeclareMulti>(pAst);
			printf(": variables: ");
			for (int iIdent = 0; iIdent < pAstdeclmul->aryIdent.c; ++iIdent)
			{
				printf("%s%s", (iIdent > 0) ? ", " : "", pAstdeclmul->aryIdent[iIdent].pChzName);
			}
			//printf("\n%*sexpression:\n", cChSpace + 1, "");
			printf("\n");
			PrintAst(cScope + 1, pAstdeclmul->pAstValue);
		}
		break;

	case ASTK_Struct:
		{
			auto pAststruct = AstCast<SAstStruct>(pAst);
			printf(": %s\n", pAststruct->pChzName);
			for (auto adecl : pAststruct->aryAdecl)
			{
				PrintDeclaration(cScope + 1, adecl);
			}
		}
		break;

	case ASTK_Enum:
		{
			auto pAstenum = AstCast<SAstEnum>(pAst);
			printf(": %s", pAstenum->pChzName);
			if (pAstenum->pAtypeInternal)
			{
				printf(" ");
				PrintType(pAstenum->pAtypeInternal);
			}
			printf("\n");

			for (auto value : pAstenum->aryValue)
			{
				printf("%*s%s", cChSpace + 2, "", value.pChzName);
				if (value.pAstValue)
				{
					printf(" : ");
					PrintSimpleInlineAst(value.pAstValue);
				}
				printf("\n");
			}
		}
		break;

	case ASTK_Procedure:
		{
			auto pAstproc = AstCast<SAstProcedure>(pAst);
			printf(": %s%s %s\n", pAstproc->pChzName, pAstproc->fIsForeign ? " #foreign" : "", 
				   pAstproc->pChzForeign ? pAstproc->pChzForeign : "");
			if (pAstproc->aryAdeclArg.c)
			{
				printf("%*sargs:\n", cChSpace + 1, "");
				for (int iAdecl = 0; iAdecl < pAstproc->aryAdeclArg.c; ++iAdecl)
				{
					PrintDeclaration(cScope + 1, pAstproc->aryAdeclArg[iAdecl]);
				}
			}

			if (pAstproc->aryAdeclRet.c)
			{
				printf("%*sreturns:\n", cChSpace + 1, "");
				for (int iAdecl = 0; iAdecl < pAstproc->aryAdeclRet.c; ++iAdecl)
				{
					PrintDeclaration(cScope + 1, pAstproc->aryAdeclRet[iAdecl]);
				}
			}

			PrintAst(cScope + 1, pAstproc->pAstblock);
		}
		break;

	case ASTK_ImportDirective:
		printf(": \"%s\"\n", AstCast<SAstImportDirective>(pAst)->pChzImport);
		break;

	case ASTK_RunDirective:
		printf("\n");
		PrintAst(cScope + 1, AstCast<SAstRunDirective>(pAst)->pAstExpr);
		break;

	default:
		ASSERTCHZ(false, "Unhandled type %s (%d)", PchzFromAstk(pAst->astk), pAst->astk);
		break;
	}
}

enum TYPEK
{
	TYPEK_Void,
	TYPEK_Bool,
	TYPEK_Int,
	TYPEK_Float,
	TYPEK_String,
	TYPEK_Pointer,
	TYPEK_Array,
	TYPEK_Procedure,
	TYPEK_Struct,
	TYPEK_Null,
	TYPEK_Any,
	TYPEK_Enum,
	//TYPEK_PolymorphicVariable, 

	TYPEK_Max	
};

const char * PchzFromTypek(TYPEK typek)
{
	static const char * s_mpTypekPchz[] =
	{
		"void",
		"bool",
		"int",
		"float",
		"string",
		"pointer",
		"array",
		"procedure",
		"struct",
		"null",
		"any",
		"enum"
	};
	CASSERT(DIM(s_mpTypekPchz) == TYPEK_Max);
	ASSERT(typek >= 0 && typek < TYPEK_Max);

	return s_mpTypekPchz[typek];
}


struct SType
{
	TYPEK typek;
	// BB (adrianb) Story cB and cBAlign?
};

struct STypeInt : public SType
{
	int cBit;
	bool fSigned;
};

struct STypeFloat : public SType
{
	int cBit;
};

struct STypePointer : public SType
{
	SType * pTypePointedTo;
	bool fSoa;				// BB (adrianb) Extend this to: -1 means no SOA. 0 means no size limit. >0 is AOSOA of that chunk size.
};

struct STypeArray : public SType
{
	bool fDynamicallySized;	// Size of ..
	int64_t cSizeFixed;			// If >= 0 fixed size BB (adrianb) Is this enough to cover static/fixed?

	bool fSoa;				// BB (adrianb) Extend this to: -1 means no SOA. 0 means no size limit. >0 is AOSOA of that chunk size.

    SType * pTypeElement;
};

struct STypeProcedure : public SType
{
	const SType ** apTypeArg;
	const SType ** apTypeRet;
	int cpTypeArg;
	int cpTypeRet;
};

struct STypeStruct : public SType
{
	struct SMember
	{
		const char * pChzName; // BB (adrianb) Not required for format equality.
		const SType * pType;
		int64_t iBOffset;

		bool fIsConstant;
		bool fIsImported;
		bool fIsUsing;

		SErrorInfo errinfo;

		// TODO annotations
	};

	const char * pChzName; // BB (adrianb) Not required for format equality?
	SMember * aMember;	// BB (adrianb) Filled in after type entered in name slot?
	int cMember;
};

struct STypeEnum : public SType
{
	SType * pTypeInternal;
	STypeStruct * pTypestructForEnum;
};

void PrintType(const SType * pType)
{
	switch (pType->typek)
	{
	case TYPEK_Void:
	case TYPEK_Bool:
	case TYPEK_String:
		printf("%s", PchzFromTypek(pType->typek));
		break;

	case TYPEK_Int:
		{
			const STypeInt * pTypeint = static_cast<const STypeInt *>(pType);
			printf("%s%d", (pTypeint->fSigned) ? "int" : "uint", pTypeint->cBit);
		}
		break;

	case TYPEK_Float:
		{
			const STypeFloat * pTypeflt = static_cast<const STypeFloat *>(pType);
			printf("float%d", pTypeflt->cBit);
		}
		break;

	case TYPEK_Pointer:
		{
			auto pTypepointer = static_cast<const STypePointer *>(pType);
			printf("* %s", (pTypepointer->fSoa) ? "SOA " : "");
			PrintType(pTypepointer->pTypePointedTo);
		}
		break;

	case TYPEK_Array:
		{
			auto pTypearray = static_cast<const STypeArray *>(pType);
			if (pTypearray->fDynamicallySized)
			{
				printf("[..] ");
			}
			else if (pTypearray->cSizeFixed >= 0)
			{
				printf("[%lld] ", pTypearray->cSizeFixed);
			}
			else
			{
				printf("[] ");
			}

			if (pTypearray->fSoa)
				printf("SOA ");

			PrintType(pTypearray->pTypeElement);
		}
		break;

	case TYPEK_Procedure:
		{
			auto pTypeproc = static_cast<const STypeProcedure *>(pType);
			printf("(");
			for (int ipType = 0; ipType < pTypeproc->cpTypeArg; ++ipType)
			{
				printf("%s", (ipType > 0) ? ", " : "");
				PrintType(pTypeproc->apTypeArg[ipType]);
			}
			printf(")");
			if (pTypeproc->cpTypeRet)
			{
				printf(" -> ");
				for (int ipType = 0; ipType < pTypeproc->cpTypeRet; ++ipType)
				{
					printf("%s", (ipType > 0) ? ", " : "");
					PrintType(pTypeproc->apTypeRet[ipType]);
				}
			}
		}
		break;

	case TYPEK_Struct:
		{
			auto pTypestruct = static_cast<const STypeStruct *>(pType);
			printf("%s", pTypestruct->pChzName);
			// BB (adrianb) Print member info?
		}
		break;

	case TYPEK_Null:
		{
			printf("null");
		}
		break;

	case TYPEK_Any:
		{
			printf("Any");
		}
		break;

	case TYPEK_Enum:
		{
			// BB (adrianb) Is this the right place to get the name of the enum?

			auto pTypeenum = static_cast<const STypeEnum *>(pType);
			printf("%s", pTypeenum->pTypestructForEnum->pChzName);
		}
		break;

	default:
		ASSERT(false);
		break;
	}
}

bool FAreSameType(const SType * pType0, const SType * pType1)
{
	if (pType0->typek != pType1->typek)
		return false;

	// BB (adrianb) Can we be sure dependent types are fully resolved?
	//  If so just compare pointers for inner types.

	switch (pType0->typek)
	{
	case TYPEK_Void:
	case TYPEK_Bool:
	case TYPEK_String:
	case TYPEK_Any:
	case TYPEK_Null:
		return true;

	case TYPEK_Int:
		{
			auto pTypeint0 = static_cast<const STypeInt *>(pType0);
			auto pTypeint1 = static_cast<const STypeInt *>(pType1);
			return pTypeint0->cBit == pTypeint1->cBit && 
					pTypeint0->fSigned == pTypeint1->fSigned;
		}

	case TYPEK_Float:
		{
			auto pTypeflt0 = static_cast<const STypeFloat *>(pType0);
			auto pTypeflt1 = static_cast<const STypeFloat *>(pType1);
			return pTypeflt0->cBit == pTypeflt1->cBit;
		}

	case TYPEK_Pointer:
		{
			auto pTypepointer0 = static_cast<const STypePointer *>(pType0);
			auto pTypepointer1 = static_cast<const STypePointer *>(pType1);
			return pTypepointer0->fSoa == pTypepointer1->fSoa &&
					FAreSameType(pTypepointer0->pTypePointedTo, pTypepointer1->pTypePointedTo);
		}
		break;

	case TYPEK_Array:
		{
			auto pTypearray0 = static_cast<const STypeArray *>(pType0);
			auto pTypearray1 = static_cast<const STypeArray *>(pType1);

			return pTypearray0->fDynamicallySized == pTypearray1->fDynamicallySized &&
					pTypearray0->cSizeFixed == pTypearray1->cSizeFixed &&
					pTypearray0->fSoa == pTypearray1->fSoa &&
					FAreSameType(pTypearray0->pTypeElement, pTypearray1->pTypeElement);
		}
		break;

	case TYPEK_Procedure:
		{
			auto pTypeproc0 = static_cast<const STypeProcedure *>(pType0);
			auto pTypeproc1 = static_cast<const STypeProcedure *>(pType1);
			
			if (pTypeproc0->cpTypeArg != pTypeproc1->cpTypeArg)
				return false;

			for (int ipType = 0; ipType < pTypeproc0->cpTypeArg; ++ipType)
			{
				if (!FAreSameType(pTypeproc0->apTypeArg[ipType], pTypeproc1->apTypeArg[ipType]))
					return false;
			}

			if (pTypeproc0->cpTypeRet != pTypeproc1->cpTypeRet)
				return false;

			for (int ipType = 0; ipType < pTypeproc0->cpTypeRet; ++ipType)
			{
				if (!FAreSameType(pTypeproc0->apTypeRet[ipType], pTypeproc1->apTypeRet[ipType]))
					return false;
			}

			return true;
		}

	case TYPEK_Struct:
		{
			auto pTypestruct0 = static_cast<const STypeStruct *>(pType0);
			auto pTypestruct1 = static_cast<const STypeStruct *>(pType1);
			
			// BB (adrianb) Not required? Or should this be the ONLY thing required?
			if (strcmp(pTypestruct0->pChzName, pTypestruct1->pChzName) != 0) 
				return false;

			if (pTypestruct0->cMember != pTypestruct1->cMember)
				return false;

			for (int iMember = 0; iMember < pTypestruct0->cMember; ++iMember)
			{
				auto pMember0 = &pTypestruct0->aMember[iMember];
				auto pMember1 = &pTypestruct1->aMember[iMember];

				if (strcmp(pMember0->pChzName, pMember1->pChzName) != 0) // BB (adrianb) Not required?
					return false;

				if (!FAreSameType(pMember0->pType, pMember1->pType))
					return false;

				// BB (adrianb) Skipping iBOffset and errinfo.

				if (pMember0->fIsConstant != pMember1->fIsConstant ||
					pMember0->fIsImported != pMember1->fIsImported ||
					pMember0->fIsUsing != pMember1->fIsUsing)
				{
					return false;
				}
			}

			return true;
		}

	case TYPEK_Enum:
		{
			auto pTypeenum0 = static_cast<const STypeEnum *>(pType0);
			auto pTypeenum1 = static_cast<const STypeEnum *>(pType1);
			
			if (!FAreSameType(pTypeenum0->pTypeInternal, pTypeenum1->pTypeInternal))
				return false;

			if (!FAreSameType(pTypeenum0->pTypestructForEnum, pTypeenum1->pTypestructForEnum))
				return false;

			return true;
		}

	default:
		ASSERTCHZ(false, "Missing clause for typek %d", pType0->typek);
		return false;
	}
}

bool FIsKeyEqual(const SType * pType, const SType & type)
{
	return FAreSameType(pType, &type);
}

template <class T>
T * PtAlloc(SPagedAlloc * pPagealloc, int cT = 1)
{
	return static_cast<T *>(PvAlloc(pPagealloc, sizeof(T) * cT, alignof(T)));
}

template <class T>
T * PtClone(SPagedAlloc * pPagealloc, const T * aTIn, int cT)
{
	T * aT = PtAlloc<T>(pPagealloc, cT);
	memcpy(aT, aTIn, cT * sizeof(T));
	return aT;
}

// BB (adrianb) Use 64 bit hash function?

uint32_t HvFromType(const SType & type)
{
	uint32_t hv = type.typek;

	// BB (adrianb) If internal types are already unique we could just hash their pointers (inconsistent hash order across runs).
	// BB (adrianb) Better hash mechanism?

	switch (type.typek)
	{
	case TYPEK_Void:
	case TYPEK_Bool:
	case TYPEK_String:
	case TYPEK_Any:
	case TYPEK_Null:
		break;

	case TYPEK_Int:
		{
			auto pTypeint = static_cast<const STypeInt *>(&type);
			hv = hv * 7901 + pTypeint->cBit;
			hv = hv * 7901 + pTypeint->fSigned;
		}
		break;

	case TYPEK_Float:
		hv = hv * 7901 + static_cast<const STypeFloat *>(&type)->cBit;
		break;

	case TYPEK_Pointer:
		{
			auto pTypepointer = static_cast<const STypePointer *>(&type);
			hv = hv * 7901 + pTypepointer->fSoa;
			hv = hv * 7901 + HvFromType(*pTypepointer->pTypePointedTo);
		}
		break;

	case TYPEK_Array:
		{
			auto pTypearray = static_cast<const STypeArray *>(&type);
			hv = hv * 7901 + pTypearray->fDynamicallySized;
			hv = hv * 7901 + pTypearray->cSizeFixed;
			hv = hv * 7901 + pTypearray->fSoa;
			hv = hv * 7901 + HvFromType(*pTypearray->pTypeElement);
		}
		break;

	case TYPEK_Procedure:
		{
			auto pTypeproc = static_cast<const STypeProcedure *>(&type);
			
			hv = hv * 7901 + pTypeproc->cpTypeArg;

			for (int ipType = 0; ipType < pTypeproc->cpTypeArg; ++ipType)
			{
				hv = hv * 7901 + HvFromType(*pTypeproc->apTypeArg[ipType]);
			}

			hv = hv * 7901 + pTypeproc->cpTypeRet;

			for (int ipType = 0; ipType < pTypeproc->cpTypeRet; ++ipType)
			{
				hv = hv * 7901 + HvFromType(*pTypeproc->apTypeRet[ipType]);
			}
		}
		break;

	case TYPEK_Struct:
		{
			auto pTypestruct = static_cast<const STypeStruct *>(&type);
			
			// BB (adrianb) Not required? Or should this be the ONLY thing required?
			hv = hv * 7901 + HvFromKey(pTypestruct->pChzName, INT_MAX);
			hv = hv * 7901 + pTypestruct->cMember;

			for (int iMember = 0; iMember < pTypestruct->cMember; ++iMember)
			{
				auto pMember = &pTypestruct->aMember[iMember];

				hv = hv * 7901 + HvFromKey(pMember->pChzName, INT_MAX); // BB (adrianb) Not required?
				hv = hv * 7901 + HvFromType(*pMember->pType);

				// BB (adrianb) Skipping iBOffset and errinfo.

				hv = hv * 7901 + pMember->fIsConstant;
				hv = hv * 7901 + pMember->fIsImported;
				hv = hv * 7901 + pMember->fIsUsing;
			}
		}

	case TYPEK_Enum:
		{
			auto pTypeenum = static_cast<const STypeEnum *>(&type);
			
			hv = hv * 7901 + HvFromType(*pTypeenum->pTypeInternal);
			hv = hv * 7901 + HvFromType(*pTypeenum->pTypestructForEnum);
		}

	default:
		ASSERT(false);
		break;
	}

	return hv;
}

#if 0

enum BCK
{
	BCK_Invalid,
	
	BCK_LoadImmediate,
	BCK_ResetRegisters,	
	BCK_StartScope,
	BCK_EndScope,

	BCK_Store,
	BCK_Load,
	BCK_LoadStackPtr,
	BCK_LoadGlobalPtr,

	BCK_Call,
	BCK_Return,

	BCK_PrintReg,
	BCK_PrintEnd,
	BCK_Cast,
	
	BCK_JumpIf,
	BCK_Jump,

	BCK_Negate,
	BCK_BitwiseNegate,

	BCK_LogicalAnd,
	BCK_LogicalOr,

	BCK_Add,
	BCK_Subtract,
	BCK_Multiply,
	BCK_Divide,
	BCK_Modulo,
	BCK_LessThan,
	BCK_GreaterThan,
	BCK_LessThanOrEqual,
	BCK_GreaterThanOrEqual,
	BCK_CmpEqual,
	BCK_CmpNotEqual,

	BCK_BitwiseAnd,
	BCK_BitwiseOr,
	BCK_BitwiseXor,
	
	BCK_Max
};

enum LABEL
{
	LABEL_Nil = -1
};

struct SFunction;

struct SByteCode
{
	BCK bck;
	SErrorInfo errinfo;
	union
	{
		struct
		{
			int iReg;
		} simple;

		struct
		{
			int iRegRet;
			int iRegArg0;
			int iRegArg1;
		} binary;

		struct
		{
			int iReg;
			LABEL labelTrue;
			LABEL labelFalse;
		} jump;

		struct 
		{
			int iReg;
			const SType * pType;
			union
			{
				const char * pChz;
				int64_t n;
				double g;
			};
		} loadliteral;

		struct 
		{
			int iReg;
			const SType * pType;
			const char * pChzVariable;
			int iB;
		} address;

		struct 
		{
			int iRegAddr;
			int iRegValue;
		} loadstore;

		struct 
		{
			int iBStack;
		} scope;

		struct
		{
			const SFunction * pFunc;
			int * aiRegArg;
			int ciRegArg;
			int iRegRet;
		} call;

		struct
		{
			int iRegRet;
			int iReg;
			const SType * pType;
		} cast;
	};
};

struct SFunction // tag = func
{
	const char * pChzName;
	const SType * pType;
	SArray<SByteCode> aryBytecode;
	SArray<int> aryiBytecodeLabel;
};

enum DECLK
{
	DECLK_Type,
	DECLK_StackVariable,
	DECLK_Function,
	DECLK_GlobalVariable,

	DECLK_Max
};

struct SDeclaration
{
	DECLK declk;
	const char * pChzName;
	const SType * pType;

	union
	{
		struct
		{
			int iB;
		} stack;

		struct
		{
			SFunction * pFunc;
		} function;

		struct
		{
			int iB;
		} global;
	};
};

struct SFuncContext
{
	SFunction * pFuncCur;

	// BB (adrianb) Temporaries used during byte code emission

	int iRegTemp;
	int iBStack;
	int iDeclFuncMic;
};

// BB (adrianb) Combine parse and compile phases and output bytecode directly?
//  Could do a second pass to uniquify types, but then would have use general equality
//  during compile.  
// BB (adrianb) Could flatten structs and functions to global level?  The handling 
//  recursive types would be easy.

struct SCompiler // tag = compiler
{
	SPagedAlloc pagealloc;			// Non-freeing linear allocator
	SSet<const SType *> setpType;	// Type uniquification

	SArray<SDeclaration> aryDecl; 	// Current namespace

	SArray<uint8_t> aryBGlobal;

	SFuncContext funcctx;
};

const SType * PtypeEnsure(SCompiler * pCompiler, const SType & type)
{
	uint32_t hv = HvFromType(type);
    
    const SType * const * ppType = PtLookupImpl(&pCompiler->setpType, hv, type);
    
	if (ppType)
		return *ppType;

	SType * pType = PtAlloc<SType>(&pCompiler->pagealloc);

	*pType = type;
	switch (type.typek)
	{
	case TYPEK_Bool:
	case TYPEK_Int:
	case TYPEK_Float:
	case TYPEK_String:
	case TYPEK_Void:
	case TYPEK_Pointer:
		break;

	case TYPEK_Procedure:
		{
			int cpTypeArg = type.function.cpTypeArg;
			pType->function.apTypeArg = PtClone<const SType *>(&pCompiler->pagealloc, type.function.apTypeArg, cpTypeArg);
			pType->function.apChzArg = PtClone<const char *>(&pCompiler->pagealloc, type.function.apChzArg, cpTypeArg);
		}
		break;

	default:
		ASSERT(false);
		break;
	}

	EnsureCount(&pCompiler->setpType, pCompiler->setpType.c + 1);

	AddImpl(&pCompiler->setpType, hv, static_cast<const SType *>(pType));

	return pType;
}

const SDeclaration * PdeclLookup(SCompiler * pCompiler, const char * pChz)
{
	// BB (adrianb) Use a hash instead?

	for (int iDecl = 0; iDecl < pCompiler->aryDecl.c; ++iDecl)
	{
		const SDeclaration & decl = pCompiler->aryDecl.a[iDecl];

		// Skip any non-globally relevant scopes
		
		if (decl.declk == DECLK_StackVariable && iDecl < pCompiler->funcctx.iDeclFuncMic)
			continue;

		if (strcmp(decl.pChzName, pChz) == 0)
			return &decl;
	}

	return nullptr;
}

const SType * PtypeLookupByName(SCompiler * pCompiler, const char * pChz)
{
	const SDeclaration * pDecl = PdeclLookup(pCompiler, pChz);

	return (pDecl && pDecl->declk == DECLK_Type) ? pDecl->pType : nullptr;
}

const SType * PtypeEnsure(SCompiler * pCompiler, SAst * pAst)
{
	switch (pAst->astk)
	{
	case ASTK_Identifier:
		{
			const SType * pType = PtypeLookupByName(pCompiler, pAst->identifier.pChz);
			if (pType == nullptr)
			{
				ShowErr(pAst->errinfo, "Didn't recognize type named %s", pAst->identifier.pChz);
			}
			return pType;
		}

	case ASTK_FunctionSignature:
		{
			SAst * pAstArgs = pAst->pAstChild;
			SAst * pAstRet = pAstArgs->pAstNext;

			int cArg = CLength(pAstArgs->pAstChild);

			SType type;
			type.typek = TYPEK_Procedure;
			type.function.pTypeRet = PtypeEnsure(pCompiler, pAstRet);
			type.function.cpTypeArg = cArg;
			type.function.apTypeArg = static_cast<const SType **>(alloca(cArg * sizeof(SType *)));
			type.function.apChzArg = static_cast<const char **>(alloca(cArg * sizeof(const char *)));

			int cpType = 0;
			for (SAst * pAstArg = pAstArgs->pAstChild; pAstArg; pAstArg = pAstArg->pAstNext)
			{
				ASSERT(pAstArg->astk == ASTK_List);

				SAst * pAstType = pAstArg->pAstChild;
				SAst * pAstName = pAstType->pAstNext;

				ASSERT(pAstName->astk == ASTK_Identifier);

				type.function.apTypeArg[cpType] = PtypeEnsure(pCompiler, pAstType);
				type.function.apChzArg[cpType] = pAstName->identifier.pChz;
				++cpType;
			}

			return PtypeEnsure(pCompiler, type);
		}

	default:
		ASSERT(false);
		return nullptr;
	}
}

bool FCheckExistingDeclaration(SCompiler * pCompiler, const SErrorInfo & errinfo, const char * pChz)
{
	const SDeclaration * pDeclFound = PdeclLookup(pCompiler, pChz);
	if (pDeclFound)
	{
		ShowErr(errinfo, "Already found something named %s", pChz);
		return true;
	}

	return false;
}

bool FIsCompatibleCast(const SType * pTypeSrc, const SType * pTypeDst)
{
	if (pTypeSrc == pTypeDst)
		return true;

	TYPEK typekSrc = pTypeSrc->typek;
	switch (pTypeDst->typek)
	{
		case TYPEK_Void:
			return true;

		case TYPEK_Bool:
			return (typekSrc == TYPEK_Int || typekSrc == TYPEK_Pointer || typekSrc == TYPEK_String);

		case TYPEK_Int:
			return (typekSrc == TYPEK_Bool || typekSrc == TYPEK_Int || typekSrc == TYPEK_Float);

		case TYPEK_Float:
			return (typekSrc == TYPEK_Int || typekSrc == TYPEK_Float);

		case TYPEK_String:
			return false;

		case TYPEK_Procedure:
			return false; // Compatible functions?

		case TYPEK_Pointer:
			return typekSrc == TYPEK_Pointer; // BB (adrianb) Check type signatures to see if they're compatible?  E.g. functions.

		case TYPEK_Struct:
			return typekSrc == TYPEK_Struct; // Also works.

		default:
			ASSERT(false);
			return false;
	}
}

void RegisterNamedType(SCompiler * pCompiler, const SErrorInfo & errinfo, const char * pChz, const SType * pType)
{
	if (FCheckExistingDeclaration(pCompiler, errinfo, pChz))
		return;

	SDeclaration * pDecl = PtAppendNew(&pCompiler->aryDecl);
	pDecl->declk = DECLK_Type;
	pDecl->pChzName = pChz;
	pDecl->pType = pType;
}

void RegisterBuiltinType(SCompiler * pCompiler, const char * pChz, const SType & type)
{
	const SType * pType = PtypeEnsure(pCompiler, type);

	SErrorInfo errinfo = { "<builtin_types>" };	
	RegisterNamedType(pCompiler, errinfo, pChz, pType);
}

SType TypeBuild(TYPEK typek, int cBit, bool fSigned)
{
	SType type = {};
	type.typek = typek;
	type.intr.cBit = cBit;
	type.intr.fSigned = fSigned;

	return type;
}

SType TypeInt(int cBit)
{
	return TypeBuild(TYPEK_Int, cBit, true);
}

SType TypeUInt(int cBit)
{
	return TypeBuild(TYPEK_Int, cBit, false);
}

SType TypeFloat(int cBit)
{
	SType type = {};
	type.typek = TYPEK_Float;
	type.flt.cBit = cBit;

	return type;
}

void Init(SCompiler * pCompiler)
{
	ClearStruct(pCompiler);

	Init(&pCompiler->pagealloc, 16 * 1024);

	RegisterBuiltinType(pCompiler, "void", TypeBuild(TYPEK_Void, 0, false));
	RegisterBuiltinType(pCompiler, "bool", TypeBuild(TYPEK_Bool, 1, false));
	RegisterBuiltinType(pCompiler, "string", TypeBuild(TYPEK_String, 0, false));

	RegisterBuiltinType(pCompiler, "int8", TypeInt(8));
	RegisterBuiltinType(pCompiler, "int16", TypeInt(16));
	RegisterBuiltinType(pCompiler, "int32", TypeInt(32));
	RegisterBuiltinType(pCompiler, "int64", TypeInt(64));

	RegisterBuiltinType(pCompiler, "uint8", TypeUInt(8));
	RegisterBuiltinType(pCompiler, "uint16", TypeUInt(16));
	RegisterBuiltinType(pCompiler, "uint32", TypeUInt(32));
	RegisterBuiltinType(pCompiler, "uint64", TypeUInt(64));

	RegisterBuiltinType(pCompiler, "float32", TypeFloat(32));
	RegisterBuiltinType(pCompiler, "float", TypeFloat(32));
	RegisterBuiltinType(pCompiler, "float64", TypeFloat(64));
}

int IregAdd(SCompiler * pCompiler)
{
	return pCompiler->funcctx.iRegTemp++;
}

SByteCode * PbytecodeStart(SFunction * pFunc, BCK bck, const SErrorInfo & errinfo)
{
	SByteCode * pBytecode = PtAppendNew(&pFunc->aryBytecode);
	pBytecode->bck = bck;
	pBytecode->errinfo = errinfo;
	return pBytecode;
}

void ResetRegisters(SCompiler * pCompiler)
{
	if (pCompiler->funcctx.iRegTemp <= 0)
		return;

	(void) PbytecodeStart(pCompiler->funcctx.pFuncCur, BCK_ResetRegisters, SErrorInfo());
	pCompiler->funcctx.iRegTemp = 0;
}

LABEL LabelReserve(SFunction * pFunc)
{
	LABEL label = LABEL(pFunc->aryiBytecodeLabel.c);
	*PtAppendNew(&pFunc->aryiBytecodeLabel) = -1;
	return label;
}

void MarkLabel(SFunction * pFunc, LABEL label)
{
	ASSERT(pFunc->aryiBytecodeLabel[label] == -1);
	pFunc->aryiBytecodeLabel[label] = pFunc->aryBytecode.c;
}

void AddOperatorByteCode(SFunction * pFunc, BCK bck, const SErrorInfo & errinfo,
						 int iRegRet, int iRegArg0, int iRegArg1)
{
	SByteCode * pBytecode = PbytecodeStart(pFunc, bck, errinfo);
	pBytecode->binary.iRegRet = iRegRet;
	pBytecode->binary.iRegArg0 = iRegArg0;
	pBytecode->binary.iRegArg1 = iRegArg1;
}

void AddUnaryByteCode(SFunction * pFunc, BCK bck, const SErrorInfo & errinfo, int iRegRet, int iRegArg0)
{
	SByteCode * pBytecode = PbytecodeStart(pFunc, bck, errinfo);
	pBytecode->binary.iRegRet = iRegRet;
	pBytecode->binary.iRegArg0 = iRegArg0; // Just ignoring iRegArg1
}

void AddSimpleByteCode(SFunction * pFunc, const SErrorInfo & errinfo, BCK bck, int iReg)
{
	SByteCode * pBytecode = PbytecodeStart(pFunc, bck, errinfo);
	pBytecode->simple.iReg = iReg;
}

struct SResult
{
	const SType * pType;
	int iReg;
};

const SType * PtypeMakePtr(SCompiler * pCompiler, const SType * pType)
{
	SType type = {};
	type.typek = TYPEK_Pointer;
	type.pointer.pType = pType;

	return PtypeEnsure(pCompiler, type);
}

void EmitAddressOfByteCode(SCompiler * pCompiler, const SDeclaration * pDecl, const SErrorInfo & errinfo, SResult * pResult)
{
	bool fGlobal = pDecl->declk == DECLK_GlobalVariable;
	ASSERT(pDecl->declk == DECLK_StackVariable || fGlobal);

	pResult->iReg = IregAdd(pCompiler);
	pResult->pType = PtypeMakePtr(pCompiler, pDecl->pType);

	SByteCode * pBytecodePtr = PbytecodeStart(pCompiler->funcctx.pFuncCur, fGlobal ? BCK_LoadGlobalPtr : BCK_LoadStackPtr, errinfo);
	pBytecodePtr->address.iReg = pResult->iReg;
	pBytecodePtr->address.pType = pResult->pType;
	pBytecodePtr->address.pChzVariable = pDecl->pChzName;
	pBytecodePtr->address.iB = fGlobal ? pDecl->global.iB : pDecl->stack.iB;
}

struct SBuiltinValue
{
	union
	{
		uint64_t n;
		double g;
		const char * pChz;
	};
};

void ExtractLiteral(SCompiler * pCompiler, SAst * pAst, const SType ** ppType, SBuiltinValue * pValue)
{
	if (pAst->astk == ASTK_StringLiteral)
	{
		*ppType = PtypeLookupByName(pCompiler, "string");
		pValue->pChz = pAst->strlit.pChz;
	}
	else if (pAst->astk == ASTK_BoolLiteral)
	{
		*ppType = PtypeLookupByName(pCompiler, "bool");
		pValue->n = pAst->intlit.n;
	}
	else if (pAst->astk == ASTK_IntLiteral)
	{
		SType typeInt = {};
		typeInt.typek = TYPEK_Int;
		typeInt.intr.cBit = pAst->intlit.cBit;
		typeInt.intr.fSigned = pAst->intlit.fSigned;

		*ppType = PtypeEnsure(pCompiler, typeInt);
		pValue->n = pAst->intlit.n;
	}
	else if (pAst->astk == ASTK_FloatLiteral)
	{
		SType typeFloat = {};
		typeFloat.typek = TYPEK_Float;
		typeFloat.flt.cBit = pAst->fltlit.cBit;

		*ppType = PtypeEnsure(pCompiler, typeFloat);
		pValue->g = pAst->fltlit.g;
	}
	else
	{
		ASSERT(false);
	}
}

const char * PchzErrorFromDeclk(DECLK declk)
{
	static const char * s_mpDeclkPchzError[] =
	{
		"type",
		"stack variable",
		"function",
		"global variable",
	};

	CASSERT(DIM(s_mpDeclkPchzError) == DECLK_Max);
	ASSERT(declk >= 0 && declk < DECLK_Max);

	return s_mpDeclkPchzError[declk];
}

void CompileExpression(SCompiler * pCompiler, SAst * pAst, SResult * pResult);

void CompileAddress(SCompiler * pCompiler, SAst * pAst, SResult * pResult)
{
	if (pAst->astk == ASTK_Identifier)
	{
		const char * pChzAssign = pAst->identifier.pChz;
		const SDeclaration * pDecl = PdeclLookup(pCompiler, pChzAssign);

		if (pDecl->declk != DECLK_StackVariable && pDecl->declk != DECLK_GlobalVariable)
		{
			ShowErr(pAst->errinfo, "Can't assign to %s %s", PchzErrorFromDeclk(pDecl->declk), pChzAssign);
			return;
		}

		EmitAddressOfByteCode(pCompiler, pDecl, pAst->errinfo, pResult);
	}
	else if (pAst->astk == ASTK_Operator && strcmp(pAst->op.pChz, "*") == 0 && CLength(pAst->pAstChild) == 1)
	{
		SResult resultAddr;
		CompileExpression(pCompiler, pAst->pAstChild, pResult);
	}
	else
	{
		ShowErr(pAst->errinfo, "Can't get address for expression (%s).", PchzFromAstk(pAst->astk));
	}
}

void CompileAssignment(SCompiler * pCompiler, const SErrorInfo & errinfo, SAst * pAstDest, const SResult & result)
{
	SResult resultAddr;
	CompileAddress(pCompiler, pAstDest, &resultAddr);

	ASSERT(resultAddr.pType->typek == TYPEK_Pointer);

	if (resultAddr.pType->pointer.pType != result.pType)
	{
		ShowErr(errinfo, "Type mismatch for assignment");
		return;
	}

	SByteCode * pBytecodeStore = PbytecodeStart(pCompiler->funcctx.pFuncCur, BCK_Store, errinfo);
	pBytecodeStore->loadstore.iRegAddr = resultAddr.iReg;
	pBytecodeStore->loadstore.iRegValue = result.iReg;
}

void CompileExpression(SCompiler * pCompiler, SAst * pAst, SResult * pResult)
{
	SFunction * pFuncCur = pCompiler->funcctx.pFuncCur;

	ClearStruct(pResult);

	if (pAst->astk == ASTK_Operator)
	{
		const char * pChzOp = pAst->op.pChz;
		SAst * pAstArg0 = pAst->pAstChild;
		int cArg = CLength(pAstArg0);
		if (cArg == 2)
		{
			if (strcmp(pChzOp, "=") == 0)
			{
				SResult result1;
				CompileExpression(pCompiler, pAstArg0->pAstNext, &result1);
				CompileAssignment(pCompiler, pAst->errinfo, pAstArg0, result1);
				return;
			}

			SResult result0, result1;
			CompileExpression(pCompiler, pAstArg0, &result0);
			CompileExpression(pCompiler, pAstArg0->pAstNext, &result1);

			// BB (adrianb) Need to be able to lookup global operator functions
			//  that take different types?

			if (result0.pType != result1.pType)
			{
				ShowErr(pAstArg0->errinfo, "Expected types to be the same");
				return;
			}

			struct SOpCompile
			{
				const char * pChz;
				BCK bck;
				TYPEK aTypek[3]; // Invalid means use source type
				const char * pChzTypeResult;
			};

			static const SOpCompile s_aOpc[] =
			{
				{ "+", BCK_Add, { TYPEK_Int, TYPEK_Float } },
				{ "-", BCK_Subtract, { TYPEK_Int, TYPEK_Float } },
				{ "*", BCK_Multiply, { TYPEK_Int, TYPEK_Float } },
				{ "/", BCK_Divide, { TYPEK_Int, TYPEK_Float } },
				{ "%", BCK_Modulo, { TYPEK_Int, TYPEK_Float } },
				{ "&", BCK_BitwiseAnd, { TYPEK_Int } },
				{ "|", BCK_BitwiseOr, { TYPEK_Int } },
				{ "^", BCK_BitwiseXor, { TYPEK_Int } },
				{ "&&", BCK_LogicalAnd, { TYPEK_Bool } }, // BB (adrianb) Short circuit?
				{ "||", BCK_LogicalOr, { TYPEK_Bool } },
				{ "<", BCK_LessThan, { TYPEK_Int, TYPEK_Float }, "bool" },
				{ ">", BCK_GreaterThan, { TYPEK_Int, TYPEK_Float }, "bool" },
				{ "<=", BCK_LessThanOrEqual, { TYPEK_Int, TYPEK_Float }, "bool" },
				{ ">=", BCK_GreaterThanOrEqual, { TYPEK_Int, TYPEK_Float }, "bool" },
				{ "==", BCK_CmpEqual, { TYPEK_Int }, "bool" },
				{ "!=", BCK_CmpNotEqual, { TYPEK_Int }, "bool" },
			};

			static const SOpCompile s_aOpcAssign[] =
			{
				{ "+=", BCK_Add, { TYPEK_Int, TYPEK_Float } },
				{ "-=", BCK_Subtract, { TYPEK_Int, TYPEK_Float } },
				{ "*=", BCK_Multiply, { TYPEK_Int, TYPEK_Float } },
				{ "/=", BCK_Divide, { TYPEK_Int, TYPEK_Float } },
				{ "%=", BCK_Modulo, { TYPEK_Int, TYPEK_Float } },
				{ "&=", BCK_BitwiseAnd, { TYPEK_Int} },
				{ "|=", BCK_BitwiseOr, { TYPEK_Int} },
				{ "^=", BCK_BitwiseXor, { TYPEK_Int} },
				{ "&&=", BCK_LogicalAnd, { TYPEK_Bool } }, // BB (adrianb) Need at all with &=?
				{ "||=", BCK_LogicalOr, { TYPEK_Bool } },
			};

			const SOpCompile * pOpc = nullptr;
			bool fAssign = false;
			for (int iPass = 0; iPass < 2; ++iPass)
			{
				int cOpc = DIM(s_aOpc);
				const SOpCompile * aOpc = s_aOpc;
				if (iPass == 1)
				{
					cOpc = DIM(s_aOpcAssign);
					aOpc = s_aOpcAssign;
				}

				for (int iOpc = 0; iOpc < cOpc; ++iOpc)
				{
					if (strcmp(aOpc[iOpc].pChz, pChzOp) != 0)
						continue;
					
					pOpc = &aOpc[iOpc];
					fAssign = (iPass == 1);

					bool fMatchesType = false;
					for (int iTypek = 0; iTypek < DIM(aOpc[iOpc].aTypek); ++iTypek)
					{
						if (aOpc[iOpc].aTypek[iTypek] && aOpc[iOpc].aTypek[iTypek] == result0.pType->typek)
						{
							fMatchesType = true;
							break;
						}
					}

					if (!fMatchesType)
					{
						ShowErr(pAst->errinfo, "Operator %s does not support type %s",
								pChzOp, PchzFromTypek(result0.pType->typek));
						return;
					}

					break;
				}

				if (pOpc)
					break;
			}

			if (!pOpc)
			{
				ShowErr(pAst->errinfo, "Couldn't find operator %s that takes arguments of type %s",
						pChzOp, PchzFromTypek(result0.pType->typek));
				return;
			}

			const SType * pTypeResult = result0.pType;
			if (pOpc->pChzTypeResult)
				pTypeResult = PtypeLookupByName(pCompiler, pOpc->pChzTypeResult);

			int iRegResult = IregAdd(pCompiler);
			AddOperatorByteCode(pFuncCur, pOpc->bck, pAst->errinfo, iRegResult, result0.iReg, result1.iReg);

			pResult->iReg = iRegResult;
			pResult->pType = pTypeResult;

			if (fAssign)
			{
				// BB (adrianb) += and friends should do any evaluation for the l-value once.
												
				CompileAssignment(pCompiler, pAst->errinfo, pAstArg0, *pResult);
			}
		}
		else if (cArg == 1)
		{
			if (strcmp(pChzOp, "&") == 0)
			{
				CompileAddress(pCompiler, pAstArg0, pResult);
				
				return;
			}

			SResult result0 = {};
			CompileExpression(pCompiler, pAstArg0, &result0);

			const char * pChzOp = pAst->op.pChz;
			if (strcmp(pChzOp, "-") == 0)
			{
				if ((result0.pType->typek == TYPEK_Int && result0.pType->intr.fSigned) || 
					result0.pType->typek == TYPEK_Float)
				{
					// Ok
				}
				else
				{
					ShowErr(pAstArg0->errinfo, "Expected float or integral as argument to negate");
				}

				int iRegResult = IregAdd(pCompiler);
				AddUnaryByteCode(pFuncCur, BCK_Negate, pAst->errinfo, iRegResult, result0.iReg);

				pResult->pType = result0.pType;
				pResult->iReg = iRegResult;
			}
			else if (strcmp(pChzOp, "~") == 0)
			{
				if (result0.pType->typek == TYPEK_Int)
				{
					// Ok
				}
				else
				{
					ShowErr(pAstArg0->errinfo, "Expected signed or unsigned integral argument to bitwise negate.");
				}

				int iRegResult = IregAdd(pCompiler);
				AddUnaryByteCode(pFuncCur, BCK_BitwiseNegate, pAst->errinfo, iRegResult, result0.iReg);

				pResult->pType = result0.pType;
				pResult->iReg = iRegResult;
			}
			else if (strcmp(pChzOp, "*") == 0)
			{
				if (result0.pType->typek != TYPEK_Pointer)
				{
					ShowErr(pAstArg0->errinfo, "Cannot indirect a non-pointer %s.", PchzFromTypek(result0.pType->typek));
				}

				int iRegResult = IregAdd(pCompiler);
				AddUnaryByteCode(pFuncCur, BCK_BitwiseNegate, pAst->errinfo, iRegResult, result0.iReg);

				pResult->pType = result0.pType;
				pResult->iReg = iRegResult;
			}
			else
			{
				ShowErr(pAst->errinfo, "Unrecognized operator %s", pAst->op.pChz);
			}
		}
		else
		{
			ASSERT(false);
		}
	}
	else if (pAst->astk == ASTK_StringLiteral || pAst->astk == ASTK_IntLiteral || pAst->astk == ASTK_BoolLiteral || 
			 pAst->astk == ASTK_FloatLiteral)
	{
		SByteCode * pBytecode = PbytecodeStart(pFuncCur, BCK_LoadImmediate, pAst->errinfo);
		pBytecode->loadliteral.iReg = IregAdd(pCompiler);
		
		SBuiltinValue value = {};
		ExtractLiteral(pCompiler, pAst, &pBytecode->loadliteral.pType, &value);
		pBytecode->loadliteral.n = value.n; // Assigning covers all union
		
		ASSERT(pBytecode->loadliteral.pType);

		pResult->iReg = pBytecode->loadliteral.iReg;
		pResult->pType = pBytecode->loadliteral.pType;
	}
	else if (pAst->astk == ASTK_Identifier)
	{
		const char * pChzIdent = pAst->identifier.pChz;

		const SDeclaration * pDecl = PdeclLookup(pCompiler, pChzIdent);
		if (pDecl == nullptr)
		{
			ShowErr(pAst->errinfo, "Couldn't find let/var named %s", pChzIdent);
			return;
		}
		
		if (pDecl->declk == DECLK_StackVariable || pDecl->declk == DECLK_GlobalVariable)
		{
			SResult resultAddr;
			EmitAddressOfByteCode(pCompiler, pDecl, pAst->errinfo, &resultAddr);
            
            ASSERT(resultAddr.pType->typek == TYPEK_Pointer);
            
            pResult->iReg = IregAdd(pCompiler);
            pResult->pType = resultAddr.pType->pointer.pType;

			SByteCode * pBytecode = PbytecodeStart(pFuncCur, BCK_Load, pAst->errinfo);
			pBytecode->loadstore.iRegAddr = resultAddr.iReg;
			pBytecode->loadstore.iRegValue = pResult->iReg;
		}
		else
		{
			ShowErr(pAst->errinfo, "Don't know how to evaluate %s", pChzIdent);
		}
	}
	else if (pAst->astk == ASTK_Call)
	{
		SAst * pAstName = pAst->pAstChild;
		SAst * pAstArgs = pAstName->pAstNext;

		if (pAstName->astk == ASTK_Keyword)
		{
			ASSERT(pAstName->keyword == KEYWORD_Cast);
			ASSERT(CLength(pAstArgs) == 2);

			SAst * pAstValue = pAstArgs;
			const SType * pType = PtypeEnsure(pCompiler, pAstValue->pAstNext);

			SResult result = {};
			CompileExpression(pCompiler, pAstValue, &result);

			if (result.pType == pType)
			{
				*pResult = result;
			}
			else
			{
				if (!FIsCompatibleCast(result.pType, pType))
				{
					ShowErr(pAst->errinfo, "Types are incompatible");
					return;
				}

				SByteCode * pBytecode = PbytecodeStart(pFuncCur, BCK_Cast, pAst->errinfo);
				pBytecode->cast.iRegRet = IregAdd(pCompiler);
				pBytecode->cast.iReg = result.iReg;
				pBytecode->cast.pType = pType;

				pResult->pType = pType;
				pResult->iReg = pBytecode->cast.iRegRet;
			}
		}
		else
		{
			ASSERT(pAstName->astk == ASTK_Identifier);

			const char * pChzFunc = pAstName->identifier.pChz;
			const SDeclaration * pDecl = PdeclLookup(pCompiler, pChzFunc);
	        
	        if (!pDecl)
	        {
	            ShowErr(pAst->errinfo, "Can't find definition for %s", pChzFunc);
	            return;
	        }
			
			if (pDecl->declk != DECLK_Function)
			{
				ShowErr(pAst->errinfo, "Can't call %s, it is not a function", pChzFunc);
				return;
			}

			int cAstArg = CLength(pAstArgs);
			const SFunction * pFunc = pDecl->function.pFunc;
			const SType * pTypeFunc = pFunc->pType;

			if (cAstArg != pTypeFunc->function.cpTypeArg)
			{
				ShowErr(pAst->errinfo, "%s expected %d arguments given %d", pChzFunc, pTypeFunc->function.cpTypeArg, cAstArg);
				return;
			}
        
	        // Evaluate all arguments

	        int * aiRegArg = PtAlloc<int>(&pCompiler->pagealloc, cAstArg);
	        
	        int iArg = 0;
	        for (SAst * pAstArg = pAstArgs; pAstArg; pAstArg = pAstArg->pAstNext)
	        {
	            SResult result;
	            CompileExpression(pCompiler, pAstArg, &result);
	            
	            if (result.pType != pTypeFunc->function.apTypeArg[iArg])
	            {
	                ShowErr(pAstArg->errinfo, "Type mismatch in argument");
	            }
	            
	            aiRegArg[iArg] = result.iReg;
	            ++iArg;
	        }
	        
	        ASSERT(iArg == cAstArg);

			SByteCode * pBytecode = PbytecodeStart(pFuncCur, BCK_Call, pAst->errinfo);
			pBytecode->call.pFunc = pFunc;
			pBytecode->call.aiRegArg = aiRegArg;
			pBytecode->call.ciRegArg = cAstArg;
			pBytecode->call.iRegRet = (pTypeFunc->function.pTypeRet->typek != TYPEK_Void) ? IregAdd(pCompiler) : -1;
	        
	        pResult->pType = pTypeFunc->function.pTypeRet;
	        pResult->iReg = pBytecode->call.iRegRet;
	    }
	}
	else
	{
		ASSERT(false);
	}
}

void CompileStatements(SCompiler * pCompiler, SAst * pAst);

struct SScopeCtx
{
	int iBStack;
	int cDecl;
};

void CompileStartScope(SCompiler * pCompiler, const SErrorInfo & errinfo, SScopeCtx * pScopectx)
{
	pScopectx->iBStack = pCompiler->funcctx.iBStack;
	pScopectx->cDecl = pCompiler->aryDecl.c;

	(void) PbytecodeStart(pCompiler->funcctx.pFuncCur, BCK_StartScope, errinfo);
}

void CompileEndScope(SCompiler * pCompiler, const SScopeCtx & scopectx, const SErrorInfo & errinfo)
{
	SByteCode * pBytecodeEnd = PbytecodeStart(pCompiler->funcctx.pFuncCur, BCK_EndScope, errinfo);
	pBytecodeEnd->scope.iBStack = scopectx.iBStack;

	pCompiler->funcctx.iBStack = scopectx.iBStack;
	pCompiler->aryDecl.c = scopectx.cDecl;
}

SAst * PastClone(SAst * pAstIn)
{
	switch (pAstIn->astk)
	{
	case ASTK_Identifier:
	case ASTK_IntLiteral:
	case ASTK_BoolLiteral:
	case ASTK_StringLiteral:
	case ASTK_FloatLiteral:
		{
			// Copy whole thing except children pointers

			SAst * pAst = PastCreate(ASTK_Invalid, SErrorInfo());
			*pAst = *pAstIn;
			pAst->pAstNext = nullptr;
			ASSERT(pAst->pAstChild == nullptr);

			return pAst;
		}
		break;

	default:
		ASSERT(false);
		break;
	}
}

SFunction * PfuncLookup(SCompiler * pCompiler, const char * pChz)
{
	const SDeclaration * pDecl = PdeclLookup(pCompiler, pChz);
	if (!pDecl)
		return nullptr;

	if (pDecl->declk != DECLK_Function)
		return nullptr;

	return pDecl->function.pFunc;
}

inline int NAlignN(int iB, int cBAlign)
{
	// BB (adrianb) Align up for positive down for negative.  Is this wrong?
	ASSERT(FIsPowerOfTwo(cBAlign));
	return (iB + cBAlign - 1) & ~(cBAlign - 1);
}

int CbComputeSimpleTypeSize(const SType * pType)
{
	switch (pType->typek)
	{
	case TYPEK_Void:
		return 0;

	case TYPEK_Bool:
		ASSERT(pType->intr.cBit == 1);
		return 1;

	case TYPEK_Int:
		return (pType->intr.cBit + 7) / 8;

	case TYPEK_Float:
		return (pType->flt.cBit + 7) / 8;

	case TYPEK_String:
	case TYPEK_Pointer:
		return 8; // BB (adrianb) Different for different architectures...

	default:
		return -1;
	}
}



struct STypeSize
{
	int cB;
	int cBAlign;
};
STypeSize TypesizeEnsure(SCompiler * pCompiler, const SType * pType);

void ComputeTypeSize(SCompiler * pCompiler, const SType * pType)
{
	// BB (adrianb) Push something on to prevent recursion!

	int cBSimple = CbComputeSimpleTypeSize(pType);

	// Try simple size
	
	if (cBSimple >= 0)
	{
		pType->cBAlign = Max(1, cBSimple);
		pType->cB = cBSimple;
		return;
	}
	
	ASSERT(pType->typek == TYPEK_Struct);

	int cB = 0;
	int cBAlignMax = 1;

	for (int iMember = 0; iMember < pType->strct.cMember; ++iMember)
	{
		const SStructMember * pMember = &pType->strct.aMember[iMember];
		ASSERT(pMember->pType);
		STypeSize typesize = TypesizeEnsure(pCompiler, pMember->pType);
		
		cB = NAlignN(cB, typesize.cBAlign) + typesize.cB;
		cBAlignMax = typesize.cBAlign;
	}

	pType->cBAlign = cBAlignMax;
	pType->cB = cB;
}

STypeSize TypesizeEnsure(SCompiler * pCompiler, const SType * pType)
{
	if (pType->cBAlign == 0)
		ComputeTypeSize(pCompiler, pType);
	
	ASSERT(pType->cB >= 0 && pType->cBAlign > 0);

	STypeSize typesize = { pType->cB, pType->cBAlign };
	return typesize;
}

const SDeclaration * PdeclAddStackVariable(SCompiler * pCompiler, const char * pChz, const SType * pType)
{
	SDeclaration * pDecl = PtAppendNew(&pCompiler->aryDecl);
	pDecl->declk = DECLK_StackVariable;
	pDecl->pChzName = pChz;
	pDecl->pType = pType;

	STypeSize typesize = TypesizeEnsure(pCompiler, pType);
	pDecl->stack.iB = NAlignN(pCompiler->funcctx.iBStack, typesize.cBAlign);

	pCompiler->funcctx.iBStack = pDecl->stack.iB + typesize.cB;

	return pDecl;
}

void CompileStatement(SCompiler * pCompiler, SAst * pAst);

void CompileFunction(SCompiler * pCompiler, SAst * pAst)
{
    SAst * pAstName = pAst->pAstChild;
    SAst * pAstSig = pAstName->pAstNext;
    SAst * pAstBody = pAstSig->pAstNext;
    ASSERT(pAstBody->pAstNext == nullptr);
    ASSERT(pAstName->astk == ASTK_Identifier);
    ASSERT(pAstSig->astk == ASTK_FunctionSignature);
    const char * pChzFunc = pAstName->identifier.pChz;
    
    if (FCheckExistingDeclaration(pCompiler, pAst->errinfo, pChzFunc))
        return;
    
    SFunction * pFunc = static_cast<SFunction *>(calloc(1, sizeof(SFunction)));
    pFunc->pChzName = pChzFunc;
    pFunc->pType = PtypeEnsure(pCompiler, pAstSig);
    
    ASSERT(pFunc->pType->typek == TYPEK_Procedure);
    
    SDeclaration decl = {};
    decl.declk = DECLK_Function;
    decl.pChzName = pChzFunc;
    decl.pType = pFunc->pType;
    decl.function.pFunc = pFunc;
    
    *PtAppendNew(&pCompiler->aryDecl) = decl;
    
    // BB (adrianb) Package this up in a struct so we can copy/clear it more easily?
    // BB (adrianb) Temporaries used during byte code emission
    
    ASSERT(pCompiler->funcctx.iRegTemp == 0);
    SFuncContext funcctxPrev = pCompiler->funcctx;
    ClearStruct(&pCompiler->funcctx);
    int cDeclPrev = pCompiler->aryDecl.c;
    
    pCompiler->funcctx.pFuncCur = pFunc;
    pCompiler->funcctx.iDeclFuncMic = cDeclPrev;
    
    const auto & function = pFunc->pType->function;
    for (int ipTypeArg = 0; ipTypeArg < function.cpTypeArg; ++ipTypeArg)
    {
        (void) PdeclAddStackVariable(pCompiler, function.apChzArg[ipTypeArg], function.apTypeArg[ipTypeArg]);
    }
    
    // BB (adrianb) Detect that there is a return value in all branches of code if it is required.
    
    CompileStatement(pCompiler, pAstBody);
    
    ASSERT(pCompiler->funcctx.iRegTemp == 0);
    pCompiler->funcctx = funcctxPrev;
    pCompiler->aryDecl.c = cDeclPrev;
}

void CompileGlobal(SCompiler * pCompiler, SAst * pAst)
{
	SAst * pAstIdent = pAst->pAstChild;
	SAst * pAstType = pAstIdent->pAstNext;
	SAst * pAstValue = pAstType->pAstNext;

	// Infer either zero value or type

	const SType * pTypeExplicit = nullptr;				
	if (pAstType->astk != ASTK_Invalid)
	{
		pTypeExplicit = PtypeEnsure(pCompiler, pAstType);
	}
	else
	{
		// Just get the value from the result

		ASSERT(pAstValue != nullptr);
	}

	const SType * pTypeValue = nullptr;
	SBuiltinValue value = {};
	if (pAstValue)
	{
		if (pAstValue->astk != ASTK_StringLiteral && pAstValue->astk != ASTK_IntLiteral && 
			pAstValue->astk != ASTK_BoolLiteral && pAstValue->astk != ASTK_FloatLiteral)
		{
			ShowErr(pAstValue->errinfo, "Cannot initialize global with %s", PchzFromAstk(pAstValue->astk));
			return;
		}

		ExtractLiteral(pCompiler, pAstValue, &pTypeValue, &value);
	}

	const SType * pType = pTypeValue ? pTypeValue : pTypeExplicit;
	ASSERT(pType);

	if (pTypeExplicit && pTypeValue && pTypeExplicit != pTypeValue)
	{
		ShowErr(pAst->errinfo, "Expected values of the same type");
	}

	STypeSize typesize = TypesizeEnsure(pCompiler, pType);

	const SDeclaration * pDecl;
	SDeclaration * pDeclGlobal = PtAppendNew(&pCompiler->aryDecl);
	pDeclGlobal->declk = DECLK_GlobalVariable;
	pDeclGlobal->pChzName = pAstIdent->identifier.pChz;
	pDeclGlobal->pType = pType;
	pDeclGlobal->global.iB = NAlignN(pCompiler->aryBGlobal.c, typesize.cBAlign);

	SetSizeAtLeast(&pCompiler->aryBGlobal, pDeclGlobal->global.iB + typesize.cB);
	if (pAstValue)
	{
		// BB (adrianb) This will need to change for struct types.
		ASSERT(sizeof(value) >= typesize.cB);
		memcpy(&pCompiler->aryBGlobal[pDeclGlobal->global.iB], &value.n, typesize.cB);
	}
}

void CompileStatement(SCompiler * pCompiler, SAst * pAst)
{
	SFunction * pFuncCur = pCompiler->funcctx.pFuncCur;

	if (pAst->astk == ASTK_Scope)
	{
		SScopeCtx scopectx = {};
		CompileStartScope(pCompiler, pAst->errinfo, &scopectx);
		CompileStatements(pCompiler, pAst->pAstChild);
		CompileEndScope(pCompiler, scopectx, pAst->errinfo);
	}
	else if (pAst->astk == ASTK_Keyword)
	{
		KEYWORD keyword = pAst->keyword;
		if (keyword == KEYWORD_Print)
		{
			// Write out evaluation for printing each child expression with format string?

			for (SAst * pAstArg = pAst->pAstChild; pAstArg; pAstArg = pAstArg->pAstNext)
			{
				SResult result;
				CompileExpression(pCompiler, pAstArg, &result);
				AddSimpleByteCode(pFuncCur, pAstArg->errinfo, BCK_PrintReg, result.iReg);
			}

			AddSimpleByteCode(pFuncCur, pAst->errinfo, BCK_PrintEnd, -1);
		}
		else if (keyword == KEYWORD_If)
		{
			SAst * pAstTest = pAst->pAstChild;

			SResult result;
			CompileExpression(pCompiler, pAstTest, &result);

			if (result.pType->typek != TYPEK_Bool)
			{
				ShowErr(pAstTest->errinfo, "Expected bool for test expression");
			}

			SAst * pAstPass = pAstTest->pAstNext;
			SAst * pAstFail = pAstPass->pAstNext;
			LABEL labelTrue = LabelReserve(pFuncCur);
			LABEL labelEnd = LabelReserve(pFuncCur);
			LABEL labelFalse = (pAstFail) ? LabelReserve(pFuncCur) : labelEnd;

			{
				SByteCode * pBytecode = PbytecodeStart(pFuncCur, BCK_JumpIf, pAst->errinfo);
				pBytecode->jump.iReg = result.iReg;
				pBytecode->jump.labelTrue = labelTrue;
				pBytecode->jump.labelFalse = labelFalse;
			}

			MarkLabel(pFuncCur, labelTrue);
			CompileStatement(pCompiler, pAstPass);

			{
				SByteCode * pBytecode = PbytecodeStart(pFuncCur, BCK_Jump, pAst->errinfo);
				pBytecode->jump.labelTrue = labelEnd;
			}

			if (pAstFail)
			{
				MarkLabel(pFuncCur, labelFalse);
				CompileStatement(pCompiler, pAstFail);
				ASSERT(pAstFail->pAstNext == nullptr);
			}

			MarkLabel(pFuncCur, labelEnd);
		}
		else if (keyword == KEYWORD_While)
		{
			SAst * pAstTest = pAst->pAstChild;

			LABEL labelTest = LabelReserve(pFuncCur);
			MarkLabel(pFuncCur, labelTest);

			SResult result;
			CompileExpression(pCompiler, pAstTest, &result);

			if (result.pType->typek != TYPEK_Bool)
			{
				ShowErr(pAstTest->errinfo, "Expected bool for test expression");
			}

			SAst * pAstPass = pAstTest->pAstNext;			
			LABEL labelTrue = LabelReserve(pFuncCur);
			LABEL labelEnd = LabelReserve(pFuncCur);
			
			{
				SByteCode * pBytecode = PbytecodeStart(pFuncCur, BCK_JumpIf, pAst->errinfo);
				pBytecode->jump.iReg = result.iReg;
				pBytecode->jump.labelTrue = labelTrue;
				pBytecode->jump.labelFalse = labelEnd;
			}

			MarkLabel(pFuncCur, labelTrue);
			CompileStatement(pCompiler, pAstPass);

			{
				SByteCode * pBytecode = PbytecodeStart(pFuncCur, BCK_Jump, pAst->errinfo);
				pBytecode->jump.labelTrue = labelTest;
			}

			MarkLabel(pFuncCur, labelEnd);
		}
		else if (keyword == KEYWORD_For)
		{
			SScopeCtx scopectx = {};
			CompileStartScope(pCompiler, pAst->errinfo, &scopectx);

			SAst * pAstIdent = pAst->pAstChild;
			SAst * pAstStart = pAstIdent->pAstNext;
			SAst * pAstEnd = pAstStart->pAstNext;
			SAst * pAstBody = pAstEnd->pAstNext;
			ASSERT(pAstBody->pAstNext == nullptr);

			// BB (adrianb) Macro instead?

			// var i = start
			// while i < end:
			//  body
			//  i += 1

			SAst * pAstVar = PastCreate(ASTK_Keyword, pAstIdent->errinfo);
			pAstVar->keyword = KEYWORD_Var;
			AppendAst(&pAstVar->pAstChild, PastClone(pAstIdent));
			AppendAst(&pAstVar->pAstChild, PastCreate(ASTK_Invalid, SErrorInfo())); // empty type
			AppendAst(&pAstVar->pAstChild, PastClone(pAstStart));

			CompileStatement(pCompiler, pAstVar);

			SAst * pAstLess = PastCreate(ASTK_Operator, pAst->errinfo);
			pAstLess->op.pChz = "<";

			AppendAst(&pAstLess->pAstChild, PastClone(pAstIdent));
			AppendAst(&pAstLess->pAstChild, PastClone(pAstEnd));

			SAst * pAstOne = PastClone(pAstStart);
			ASSERT(pAstOne->astk == ASTK_IntLiteral);
			pAstOne->intlit.n = 1;

			SAst * pAstInc = PastCreate(ASTK_Operator, pAstIdent->errinfo);
			pAstInc->op.pChz = "+=";

			AppendAst(&pAstInc->pAstChild, PastClone(pAstIdent));
			AppendAst(&pAstInc->pAstChild, pAstOne);

			SAst * pAstWhileScope = PastCreate(ASTK_Scope, pAst->errinfo);
			AppendAst(&pAstWhileScope->pAstChild, pAstBody);
			AppendAst(&pAstWhileScope->pAstChild, pAstInc);

			SAst * pAstWhile = PastCreate(ASTK_Keyword, pAst->errinfo);
			pAstWhile->keyword = KEYWORD_While;
			
			AppendAst(&pAstWhile->pAstChild, pAstLess);
			AppendAst(&pAstWhile->pAstChild, pAstWhileScope);

			CompileStatement(pCompiler, pAstWhile);

			CompileEndScope(pCompiler, scopectx, pAst->errinfo);
		}
		else if (keyword == KEYWORD_Let || keyword == KEYWORD_Var)
		{
			SAst * pAstIdent = pAst->pAstChild;
			SAst * pAstType = pAstIdent->pAstNext;
			SAst * pAstValue = pAstType->pAstNext;

			// Infer either zero value or type

			const SType * pTypeExplicit = nullptr;				
			if (pAstType->astk != ASTK_Invalid)
			{
				pTypeExplicit = PtypeEnsure(pCompiler, pAstType);

				if (pAstValue == nullptr)
				{				
					pAstValue = PastCreate(ASTK_Invalid, pAstType->errinfo);

					if (pTypeExplicit->typek == TYPEK_Int)
					{
						pAstValue->astk = ASTK_IntLiteral;
						pAstValue->intlit.cBit = pTypeExplicit->intr.cBit;
						pAstValue->intlit.fSigned = pTypeExplicit->intr.fSigned;
					}
					else if (pTypeExplicit->typek == TYPEK_Bool)
					{
						pAstValue->astk = ASTK_BoolLiteral;
						pAstValue->intlit.cBit = 1;
					}
					else if (pTypeExplicit->typek == TYPEK_String)
					{
						pAstValue->astk = ASTK_StringLiteral;
					}
					else if (pTypeExplicit->typek == TYPEK_Float)
					{
						pAstValue->astk = ASTK_FloatLiteral;
						pAstValue->fltlit.cBit = pTypeExplicit->flt.cBit;
					}

					ASSERT(pAstValue->astk != ASTK_Invalid);
				}
			}
			else
			{
				// Just get the value from the result

				ASSERT(pAstValue != nullptr);
			}

			ASSERT(pAstValue);

			SResult result;
			CompileExpression(pCompiler, pAstValue, &result);

			if (pTypeExplicit && pTypeExplicit != result.pType)
			{
				ShowErr(pAst->errinfo, "Expected values of the same type");
			}

			const SDeclaration * pDecl = PdeclAddStackVariable(pCompiler, pAstIdent->identifier.pChz, result.pType);

			SResult resultAddr;
			EmitAddressOfByteCode(pCompiler, pDecl, pAstIdent->errinfo, &resultAddr);

			SByteCode * pBytecodeStore = PbytecodeStart(pFuncCur, BCK_Store, pAstIdent->errinfo);
			pBytecodeStore->loadstore.iRegAddr = resultAddr.iReg;
			pBytecodeStore->loadstore.iRegValue = result.iReg;
		}
		else if (keyword == KEYWORD_Func)
		{
            CompileFunction(pCompiler, pAst);
		}
		else if (keyword == KEYWORD_Global)
		{
			CompileGlobal(pCompiler, pAst);
		}
		else if (keyword == KEYWORD_Return)
		{
			ASSERT(pFuncCur->pType->typek == TYPEK_Procedure);
			const SType * pTypeRet = pFuncCur->pType->function.pTypeRet;

			SResult result;
			if (pAst->pAstChild)
			{
				CompileExpression(pCompiler, pAst->pAstChild, &result);
			}
			else
			{
				result.pType = PtypeLookupByName(pCompiler, "void");
				result.iReg = -1;
			}

			if (pTypeRet != result.pType)
			{
				ShowErr(pAst->errinfo, "Return does not match return type of function");
			}

			AddSimpleByteCode(pFuncCur, pAst->errinfo, BCK_Return, result.iReg);
		}
		else
		{
            ShowErr(pAst->errinfo, "Unrecognized keyword %s", PchzFromKeyword(keyword));
		}
	}
	else if (pAst->astk == ASTK_Operator || pAst->astk == ASTK_Identifier || pAst->astk == ASTK_Call)
	{
		SResult result;
		CompileExpression(pCompiler, pAst, &result);
	}
	else
	{
		ASSERT(false);
	}

	ResetRegisters(pCompiler);
}

void CompileStatements(SCompiler * pCompiler, SAst * pAst)
{
	for (; pAst; pAst = pAst->pAstNext)
	{
		CompileStatement(pCompiler, pAst);
	}
}

void CompileRoot(SCompiler * pCompiler, SAst * pAstRoot)
{
	ASSERT(pAstRoot->astk == ASTK_Scope);
    for (SAst * pAst = pAstRoot->pAstChild; pAst; pAst = pAst->pAstNext)
    {
    	// BB (adrianb) Verify some of this during parsing?
        if (pAst->astk == ASTK_Keyword)
        {
        	if (pAst->keyword == KEYWORD_Func)
        	{
        		CompileFunction(pCompiler, pAst);
        	}
        	else if (pAst->keyword == KEYWORD_Global)
        	{
        		CompileGlobal(pCompiler, pAst);
        	}
        	else
        	{
        		ShowErr(pAst->errinfo, "Unsupported keyword %s at root level", PchzFromKeyword(pAst->keyword));
        	}
        }
        else
        {
        	ShowErr(pAst->errinfo, "Unsupported node %s at root level", PchzFromAstk(pAst->astk));
        }
    }
}

struct SRegister
{
	const SType * pType;
	union
	{
		int64_t n;
		const char * pChz;
		double g;
		void * pV;
	};
};

struct SEval
{
	SCompiler * pCompiler;		// Compiler for type registration (abstract out?)
	SArray<SRegister> aryReg;
	SArray<uint8_t> aryBStack;
	SArray<uint8_t> aryBGlobal;
};

void Init(SEval * pEval, SCompiler * pCompiler)
{
	ClearStruct(pEval);
	pEval->pCompiler = pCompiler;

	SetSizeAtLeast(&pEval->aryBGlobal, pCompiler->aryBGlobal.c);
	memcpy(pEval->aryBGlobal.a, pCompiler->aryBGlobal.a, pCompiler->aryBGlobal.c);

	Reserve(&pEval->aryReg, 128);
	Reserve(&pEval->aryBStack, 2 * 1024 * 1024);
}

void PrintRegister(const SRegister & reg)
{
	switch (reg.pType->typek)
	{
	case TYPEK_Bool:
		printf("%lld", reg.n);
		break;

	case TYPEK_Int:
		if (reg.pType->intr.fSigned)
			printf("%lld", reg.n);
		else
			printf("%#llx", reg.n);
		break;

	case TYPEK_String:
		printf("%s", reg.pChz);
		break;
            
    case TYPEK_Float:
    	printf("%g", reg.g);
    	break;

    case TYPEK_Pointer:
    	printf("%p", reg.pV);
    	break;

	default:
		ASSERT(false);
		break;
	}
}

void * PvEnsureStack(SEval * pEval, int iBRel, int iBMic, int cB, int cBAlign)
{
	SArray<uint8_t> * pAryB = &pEval->aryBStack;
	int iB = iBRel + iBMic;
	ASSERT((iB & (cBAlign - 1)) == 0);

	// BB (adrianb) Stack cannot resize because we can keep address into it.
	//  Use virtual mapping to solve?

    ASSERT(pAryB->cMax >= iB + cB);
    SetSizeAtLeast(pAryB, iB + cB);
	return &pAryB->a[iB];
}

SRegister * PregPrepare(SEval * pEval, int iRegRel, int iRegMic)
{
	// BB (adrianb) Evaluate assumes registers are stable.  Can we remove this assumption and 
	//  detect stale pointers more reliably?

	int iReg = iRegRel + iRegMic;
	ASSERT(pEval->aryReg.cMax > iReg);
	SetSizeAtLeast(&pEval->aryReg, iReg + 1);
	SRegister * pReg = &pEval->aryReg[iReg];
	ClearStruct(pReg);
	return pReg;
}

SRegister * Preg(SEval * pEval, int iRegRel, int iRegMic)
{
	return &pEval->aryReg[iRegRel + iRegMic];
}

void StoreVariable(SEval * pEval, void * pVAddr, const SRegister & reg)
{
    // BB (adrianb) Do this directly by type size?

	TYPEK typek = reg.pType->typek;
	int cB = reg.pType->cB;
	if (typek == TYPEK_Int || typek == TYPEK_Bool)
    {
    	if (cB == 8)
        	*static_cast<int64_t *>(pVAddr) = reg.n;
        else if (cB == 4)
        	*static_cast<int32_t *>(pVAddr) = reg.n;
        else if (cB == 2)
        	*static_cast<int16_t *>(pVAddr) = reg.n;
        else if (cB == 1)
        	*static_cast<int8_t *>(pVAddr) = reg.n;
        else
        	ASSERT(false);
    }
    else if (typek == TYPEK_Float)
    {
    	if (cB == 8)
        	*static_cast<double *>(pVAddr) = reg.g;
        else if (cB == 4)
        	*static_cast<float *>(pVAddr) = reg.g;
        else
        	ASSERT(false);
    }
    else if (typek == TYPEK_String)
    {
        *static_cast<const char **>(pVAddr) = reg.pChz;
    }
    else if (typek == TYPEK_Pointer)
    {
    	*static_cast<void **>(pVAddr) = reg.pV;
    }
    else
    {
        ASSERT(false);
    }
}

void LoadVariable(SEval * pEval, const void * pVAddr, SRegister * pReg)
{
    // BB (adrianb) Do this directly by type size?
    
    TYPEK typek = pReg->pType->typek;
    int cB = pReg->pType->cB;
    if (typek == TYPEK_Int || typek == TYPEK_Bool)
    {
        if (cB == 8)
            pReg->n = *static_cast<const int64_t *>(pVAddr);
        else if (cB == 4)
            pReg->n = *static_cast<const int32_t *>(pVAddr);
        else if (cB == 2)
            pReg->n = *static_cast<const int16_t *>(pVAddr);
        else if (cB == 1)
            pReg->n = *static_cast<const int8_t *>(pVAddr);
        else
            ASSERT(false);
    }
    else if (typek == TYPEK_Float)
    {
        if (cB == 8)
            pReg->g = *static_cast<const double *>(pVAddr);
        else if (cB == 4)
            pReg->g = *static_cast<const float *>(pVAddr);
        else
            ASSERT(false);
    }
    else if (typek == TYPEK_String)
    {
        pReg->pChz = *static_cast<const char * const *>(pVAddr);
    }
    else if (typek == TYPEK_Pointer)
    {
        pReg->pV = *static_cast<void * const *>(pVAddr);
    }
    else
    {
        ASSERT(false);
    }
}

void Evaluate(SEval * pEval, const SFunction * pFunc, int iRegMicSuper, int iRegRet, int * aiRegArg)
{
	int iRegMic = pEval->aryReg.c;
	int iBStackMic = pEval->aryBStack.c;
    
    // Store each argument onto the stack
    
    {
    	int iBStack = 0;
	    for (int iArg = 0; iArg < pFunc->pType->function.cpTypeArg; ++iArg)
	    {
	    	const SType * pTypeArg = pFunc->pType->function.apTypeArg[iArg];
	    	ASSERT(pTypeArg->cBAlign > 0);
	    	void * pVStack = PvEnsureStack(pEval, iBStack, iBStackMic, pTypeArg->cB, pTypeArg->cBAlign);
	        StoreVariable(pEval, pVStack, *Preg(pEval, aiRegArg[iArg], iRegMicSuper));
	        iBStack += NAlignN(iBStack, pTypeArg->cBAlign) + pTypeArg->cB;
		}
	}

	SByteCode * aBytecode = pFunc->aryBytecode.a;
	int cBytecode = pFunc->aryBytecode.c;
	for (int iBytecode = 0; iBytecode < cBytecode; ++iBytecode)
	{
		const SByteCode & bytecode = aBytecode[iBytecode];
		BCK bck = bytecode.bck;
		switch (bck)
		{
		case BCK_LoadImmediate:
			{
				SRegister * pReg = PregPrepare(pEval, bytecode.loadliteral.iReg, iRegMic);
				pReg->pType = bytecode.loadliteral.pType;
				if (pReg->pType->typek == TYPEK_Int || pReg->pType->typek == TYPEK_Bool)
					pReg->n = bytecode.loadliteral.n;
				else if (pReg->pType->typek == TYPEK_Float)
					pReg->g = bytecode.loadliteral.g;
				else if (pReg->pType->typek == TYPEK_String)
					pReg->pChz = bytecode.loadliteral.pChz;
				else
					ASSERT(false);
			}
			break;

		case BCK_ResetRegisters:
			{
				pEval->aryReg.c = iRegMic;
			}
			break;

		case BCK_StartScope:
			break;

		case BCK_EndScope:
			{
				pEval->aryBStack.c = bytecode.scope.iBStack + iBStackMic;
			}
			break;

		case BCK_Store:
			{
				const SRegister & regAddr = *Preg(pEval, bytecode.loadstore.iRegAddr, iRegMic);
				const SRegister & regValue = *Preg(pEval, bytecode.loadstore.iRegValue, iRegMic);
				ASSERT(regAddr.pType->typek == TYPEK_Pointer && regAddr.pType->pointer.pType == regValue.pType);
				StoreVariable(pEval, regAddr.pV, regValue);
			}
			break;

		case BCK_Load:
			{   
				const SRegister & regAddr = *Preg(pEval, bytecode.loadstore.iRegAddr, iRegMic);
				SRegister * pReg = PregPrepare(pEval, bytecode.loadstore.iRegValue, iRegMic);
				ASSERT(regAddr.pType->typek == TYPEK_Pointer);

				pReg->pType = regAddr.pType->pointer.pType;
				LoadVariable(pEval, regAddr.pV, pReg);
			}
			break;

		case BCK_LoadGlobalPtr:
			{
				SRegister * pReg = PregPrepare(pEval, bytecode.address.iReg, iRegMic);
				const SType * pType = bytecode.address.pType;
                ASSERT(pType->typek == TYPEK_Pointer);
                const SType * pTypeRef = pType->pointer.pType;
				pReg->pType = pType;
				ASSERT((bytecode.address.iB & (pTypeRef->cBAlign - 1)) == 0);
				pReg->pV = &pEval->aryBGlobal[bytecode.address.iB];
				ASSERT(pEval->aryBGlobal.c >= bytecode.address.iB + pTypeRef->cB);
			}
			break;

		case BCK_LoadStackPtr:
			{   
				SRegister * pReg = PregPrepare(pEval, bytecode.address.iReg, iRegMic);
                const SType * pType = bytecode.address.pType;
                ASSERT(pType->typek == TYPEK_Pointer);
                const SType * pTypeRef = pType->pointer.pType;
				pReg->pType = pType;
				pReg->pV = PvEnsureStack(pEval, bytecode.address.iB, iBStackMic, pTypeRef->cB, pTypeRef->cBAlign);
			}
			break;

		case BCK_PrintReg:
			{
				PrintRegister(*Preg(pEval, bytecode.simple.iReg, iRegMic));
			}
			break;

		case BCK_PrintEnd:
			{
				printf("\n");
			}
			break;

		case BCK_Cast:
			{
				SRegister * pRegRet = PregPrepare(pEval, bytecode.cast.iRegRet, iRegMic);
				const SRegister & reg = *Preg(pEval, bytecode.cast.iReg, iRegMic);
				const SType * pType = bytecode.cast.pType;
				pRegRet->pType = pType;

				switch (pType->typek)
				{
					case TYPEK_Void:
						break;

					case TYPEK_Bool:
						pRegRet->n = (reg.n != 0);
						break;

					case TYPEK_Int:
						if (reg.pType->typek == TYPEK_Bool || reg.pType->typek == TYPEK_Int)
							pRegRet->n = reg.n;
						else if (reg.pType->typek == TYPEK_Float)
							pRegRet->n = int64_t(reg.g);
						else
							ASSERT(false);
						break;

					case TYPEK_Float:
						if (reg.pType->typek == TYPEK_Int)
							pRegRet->g = double(reg.n);
						else if (reg.pType->typek == TYPEK_Float)
							pRegRet->g = reg.g;
						else
							ASSERT(false);
						break;

					case TYPEK_Pointer:
						ASSERT(reg.pType->typek == TYPEK_Pointer);
						pRegRet->pV = reg.pV;
						break;

					default:
						ASSERT(false);
						break;
				}
			}
			break;

		case BCK_Jump:
		case BCK_JumpIf:
			{
				LABEL label = bytecode.jump.labelTrue;
				if (bck == BCK_JumpIf)
				{
					const SRegister & reg = *Preg(pEval, bytecode.jump.iReg, iRegMic);
					ASSERT(reg.pType->typek == TYPEK_Bool);
					if (reg.n == 0)
						label = bytecode.jump.labelFalse;
				}

				iBytecode = pFunc->aryiBytecodeLabel[label] - 1;
			}
			break;

		case BCK_Negate:
		case BCK_BitwiseNegate:
			{
				SRegister * pRegRet = PregPrepare(pEval, bytecode.binary.iRegRet, iRegMic);
				const SRegister & regArg0 = *Preg(pEval, bytecode.binary.iRegArg0, iRegMic);
				pRegRet->pType = regArg0.pType;
				if (bck == BCK_Negate && regArg0.pType->typek == TYPEK_Int && regArg0.pType->intr.fSigned)
					pRegRet->n = -regArg0.n;
				else if (bck == BCK_Negate && regArg0.pType->typek == TYPEK_Float)
					pRegRet->g = -regArg0.g;
				else if (bck == BCK_BitwiseNegate && regArg0.pType->typek == TYPEK_Int)
					pRegRet->n = ~regArg0.n;
				else
					ASSERT(false);
			}
			break;

		case BCK_LogicalAnd:
		case BCK_LogicalOr:
		case BCK_BitwiseAnd:
		case BCK_BitwiseOr:
		case BCK_Add:
		case BCK_Subtract:
		case BCK_Multiply:
		case BCK_Divide:
		case BCK_Modulo:
			{
				SRegister * pRegRet = PregPrepare(pEval, bytecode.binary.iRegRet, iRegMic);
				const SRegister & regArg0 = *Preg(pEval, bytecode.binary.iRegArg0, iRegMic);
				const SRegister & regArg1 = *Preg(pEval, bytecode.binary.iRegArg1, iRegMic);
				TYPEK typekArg0 = regArg0.pType->typek;
				pRegRet->pType = regArg0.pType;
				if (bck == BCK_LogicalAnd || bck == BCK_BitwiseAnd)
					pRegRet->n = regArg0.n & regArg1.n;
				else if (bck == BCK_LogicalOr || bck == BCK_BitwiseOr)
					pRegRet->n = regArg0.n | regArg1.n;
				else if (bck == BCK_Add && typekArg0 == TYPEK_Int)
						pRegRet->n = regArg0.n + regArg1.n;
				else if (bck == BCK_Add && typekArg0 == TYPEK_Float)
					pRegRet->g = regArg0.g + regArg1.g;
				else if (bck == BCK_Subtract && typekArg0 == TYPEK_Int)
						pRegRet->n = regArg0.n - regArg1.n;
				else if (bck == BCK_Subtract && typekArg0 == TYPEK_Float)
					pRegRet->g = regArg0.g - regArg1.g;
				else if (bck == BCK_Multiply && typekArg0 == TYPEK_Int)
						pRegRet->n = regArg0.n * regArg1.n;
				else if (bck == BCK_Multiply && typekArg0 == TYPEK_Float)
					pRegRet->g = regArg0.g * regArg1.g;
				else if (bck == BCK_Divide && typekArg0 == TYPEK_Int)
						pRegRet->n = regArg0.n / regArg1.n;
				else if (bck == BCK_Divide && typekArg0 == TYPEK_Float)
					pRegRet->g = regArg0.g / regArg1.g;
				else if (bck == BCK_Modulo && typekArg0 == TYPEK_Int)
						pRegRet->n = regArg0.n % regArg1.n;
				else if (bck == BCK_Modulo && typekArg0 == TYPEK_Float)
					pRegRet->g = fmod(regArg0.g, regArg1.g);
				else
					ASSERT(false);
			}
			break;
		
		case BCK_LessThan:
		case BCK_GreaterThan:
		case BCK_LessThanOrEqual:
		case BCK_GreaterThanOrEqual:
		case BCK_CmpEqual:
		case BCK_CmpNotEqual:
			{
				const SRegister & regArg0 = *Preg(pEval, bytecode.binary.iRegArg0, iRegMic);
				const SRegister & regArg1 = *Preg(pEval, bytecode.binary.iRegArg1, iRegMic);
				const SType & type0 = *regArg0.pType;
				SRegister * pReg = PregPrepare(pEval, bytecode.binary.iRegRet, iRegMic);
				pReg->pType = PtypeLookupByName(pEval->pCompiler, "bool");

				if (bck == BCK_LessThan && type0.typek == TYPEK_Int && type0.intr.fSigned)
					pReg->n = regArg0.n < regArg1.n;
				else if (bck == BCK_LessThan && type0.typek == TYPEK_Int && !type0.intr.fSigned)
					pReg->n = uint64_t(regArg0.n) < uint64_t(regArg1.n);
				else if (bck == BCK_LessThan && type0.typek == TYPEK_Float)
					pReg->n = regArg0.g < regArg1.g;
				else if (bck == BCK_GreaterThan && type0.typek == TYPEK_Int && type0.intr.fSigned)
					pReg->n = regArg0.n > regArg1.n;
				else if (bck == BCK_GreaterThan && type0.typek == TYPEK_Int && !type0.intr.fSigned)
					pReg->n = uint64_t(regArg0.n) > uint64_t(regArg1.n);
				else if (bck == BCK_GreaterThan && type0.typek == TYPEK_Float)
					pReg->n = regArg0.g > regArg1.g;
				else if (bck == BCK_LessThanOrEqual && type0.typek == TYPEK_Int && type0.intr.fSigned)
					pReg->n = regArg0.n <= regArg1.n;
				else if (bck == BCK_LessThanOrEqual && type0.typek == TYPEK_Int && !type0.intr.fSigned)
					pReg->n = uint64_t(regArg0.n) <= uint64_t(regArg1.n);
				else if (bck == BCK_LessThanOrEqual && type0.typek == TYPEK_Float)
					pReg->n = regArg0.g <= regArg1.g;
				else if (bck == BCK_GreaterThanOrEqual && type0.typek == TYPEK_Int && type0.intr.fSigned)
					pReg->n = regArg0.n >= regArg1.n;
				else if (bck == BCK_GreaterThanOrEqual && type0.typek == TYPEK_Int && !type0.intr.fSigned)
					pReg->n = uint64_t(regArg0.n) >= uint64_t(regArg1.n);
				else if (bck == BCK_GreaterThanOrEqual && type0.typek == TYPEK_Float)
					pReg->n = regArg0.g >= regArg1.g;
				else if (bck == BCK_CmpEqual && type0.typek == TYPEK_Int)
					pReg->n = regArg0.n == regArg1.n;
				else if (bck == BCK_CmpNotEqual && type0.typek == TYPEK_Int)
					pReg->n = regArg0.n != regArg1.n;
				else 
					ASSERT(false);
			}
			break;

		case BCK_Call:
			{
                if (bytecode.call.iRegRet >= 0)
                {
                    (void) PregPrepare(pEval, bytecode.call.iRegRet, iRegMic);
                }
				Evaluate(pEval, bytecode.call.pFunc, iRegMic, bytecode.call.iRegRet, bytecode.call.aiRegArg);
			}
			break;
                
        case BCK_Return:
            {
                if (bytecode.simple.iReg >= 0)
                {
                	SRegister * pReg = Preg(pEval, iRegRet, iRegMicSuper);
                	*pReg = *Preg(pEval, bytecode.simple.iReg, iRegMic);
                }
                else
                {
                	ASSERT(iRegRet < 0);
                }

                // Roll back to state when we entered

                pEval->aryReg.c = iRegMic;
                pEval->aryBStack.c = iBStackMic;

                return;
            }
            break;

		default:
			{
				ShowErr(bytecode.errinfo, "Unknown bytecode %d", bytecode.bck);
				ASSERT(false);
				return;
			}
		}
	}
    
    // Do an implicit return with no value
    
    ASSERT(iRegRet < 0);
        
    // Roll back to state when we entered

    pEval->aryReg.c = iRegMic;
    pEval->aryBStack.c = iBStackMic;
}


#define USE_LLVM WIN32
#if USE_LLVM
// LLVM related code

#pragma warning(push)
#pragma warning(disable:4267)
#pragma warning(disable:4244)
#pragma warning(disable:4512)
#pragma warning(disable:4800)
#pragma warning(disable:4996)
#include "llvm/Analysis/Passes.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/PassManager.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Scalar.h"
#pragma warning(pop)

struct SEvalJit
{
	llvm::LLVMContext * pLlvmctx;
	IRBuilder<> * pBuilder;
	llvm::Module * pModuleOpen;
	SArray<llvm::Module *> arypModule;
	SArray<llvm::ExecutionEngine *> arypEngine;
};

void Init(SEvalJit * pEvaljit)
{
	ClearStruct(pEvaljit);
	pEvaljit->pLlvmctx = &llvm::getGlobalContext();
}

void * PvResolveAddress(SEvalJit * pEvaljit, const char * pChzName)
{
	for (llvm::ExecutionEngine * pEngine : pEvaljit->arypEngine)
	{
		uint64_t nFuncAddr = pEngine->getFunctionAddress(pChzName);
		if (nFuncAddr) 
	  		return reinterpret_cast<void *>(nFuncAddr);
	}
	
	return nullptr;
}

class LlmvMemoryManager : public llvm::SectionMemoryManager 
{
  LlmvMemoryManager(const LlmvMemoryManager &) LLVM_DELETED_FUNCTION;
  void operator=(const LlmvMemoryManager &) LLVM_DELETED_FUNCTION;

public:
  LlmvMemoryManager(SEvalJit * pEvaljit) : m_pEvaljit(pEvaljit) {}
  virtual ~LlmvMemoryManager() {}

  virtual uint64_t getSymbolAddress(const std::string & strName) override;

private:
  SEvalJit * m_pEvaljit;
};

uint64_t LlmvMemoryManager::getSymbolAddress(const std::string & strName) 
{
	uint64_t nFnAddr = SectionMemoryManager::getSymbolAddress(strName);
	if (nFnAddr)
	{
		return nFnAddr;
	}

	uint64_t nHelperFun = (uint64_t)PvResolveAddress(m_pEvaljit, strName.c_str());
	if (!nHelperFun)
	{
		llvm::report_fatal_error("Program used extern function '" + strName +
	    	  		             "' which could not be resolved!");
	}

	return nHelperFun;
}

llvm::Function * PfuncFind(SEvalJit * pEvaljit, const char * pChzName) 
{
	for (int ipModule = 0; ipModule < pEvaljit->arypModule.c; ++ipModule)
	{
		llvm::Module * pModule = pEvaljit->arypModule[ipModule];
		llvm::Function * pFunc = pModule->getFunction(pChzName);
		if (pFunc) 
		{
			if (pModule == pEvaljit->pModuleOpen)
				return pFunc;

			ASSERT(pEvaljit->pModuleOpen != nullptr);

			// This function is in a module that has already been JITed.
			// We need to generate a new prototype for external linkage.

			llvm::Function * pFuncOpen = pEvaljit->pModuleOpen->getFunction(pChzName);
			if (pFuncOpen && !pFuncOpen->empty())
			{
				ShowErr("redefinition of function across modules %s", pChzName);
				return 0;
			}

			// If we don't have a prototype yet, create one.

			if (!pFuncOpen)
				pFuncOpen = llvm::Function::Create(pFunc->getFunctionType(), llvm::Function::ExternalLinkage,
			                      					pChzName, pEvaljit->pModuleOpen);
			return pFuncOpen;
		}
	}

	return nullptr;
}

// BB (adrianb) Use other form?
std::string GenerateUniqueName(const char *root) {
  static int i = 0;
  char s[16];
  sprintf_s(s, "%s%d", root, i++);
  std::string S = s;
  return S;
}

llvm::Module * PmoduleEnsureOpen(SEvalJit * pEvaljit) 
{
	// If we have a Module that hasn't been JITed, use that.

	if (pEvaljit->pModuleOpen)
		return pEvaljit->pModuleOpen;

	// Otherwise create a new Module.

	std::string ModName = GenerateUniqueName("mcjit_module_");
	llvm::Module * pModule = new llvm::Module(ModName, *pEvaljit->pLlvmctx);
	*PtAppendNew(&pEvaljit->arypModule) = pModule;
	pEvaljit->pModuleOpen = pModule;
	return pModule;
}

void * PvResolveFunction(SEvalJit * pEvaljit, llvm::Function * pFunc) 
{
	// See if an existing instance of MCJIT has this function.
	for (int ipEngine = 0; ipEngine < pEvaljit->arypEngine.c; ++ipEngine)
	{
		llvm::ExecutionEngine * pEngine = pEvaljit->arypEngine[ipEngine];
		void * pV = pEngine->getPointerToFunction(pFunc);
		if (pV)
	  		return pV;
	}

	// If we didn't find the function, see if we can generate it.

	if (pEvaljit->pModuleOpen) 
	{
		std::string strErr;
		llvm::ExecutionEngine * pEngine =
		    llvm::EngineBuilder(std::unique_ptr<llvm::Module>(pEvaljit->pModuleOpen))
		        .setErrorStr(&strErr)
		        .setMCJITMemoryManager(std::unique_ptr<LlmvMemoryManager>(
		            new LlmvMemoryManager(pEvaljit)))
		        .create();

		if (!pEngine) 
		{
			ShowErr("Could not create ExecutionEngine: %s\n", strErr.c_str());
			exit(1);
		}

		// Create a function pass manager for this engine
		llvm::FunctionPassManager * pFuncpassm = new llvm::FunctionPassManager(pEvaljit->pModuleOpen);

		// Set up the optimizer pipeline.  Start with registering info about how the
		// target lays out data structures.
		pEvaljit->pModuleOpen->setDataLayout(pEngine->getDataLayout());
		pFuncpassm->add(new llvm::DataLayoutPass());
		// Provide basic AliasAnalysis support for GVN.
		pFuncpassm->add(llvm::createBasicAliasAnalysisPass());
		// Promote allocas to registers.
		pFuncpassm->add(llvm::createPromoteMemoryToRegisterPass());
		// Do simple "peephole" optimizations and bit-twiddling optzns.
		pFuncpassm->add(llvm::createInstructionCombiningPass());
		// Reassociate expressions.
		pFuncpassm->add(llvm::createReassociatePass());
		// Eliminate Common SubExpressions.
		pFuncpassm->add(llvm::createGVNPass());
		// Simplify the control flow graph (deleting unreachable blocks, etc).
		pFuncpassm->add(llvm::createCFGSimplificationPass());
		pFuncpassm->doInitialization();

		// Run the FPM on this function
		for (llvm::Function & func : *pEvaljit->pModuleOpen)
		{
			pFuncpassm->run(func);
		}

		// We don't need this anymore

		delete pFuncpassm;

		pEvaljit->pModuleOpen = nullptr;
		*PtAppendNew(&pEvaljit->arypEngine) = pEngine;
		pEngine->finalizeObject();
		return pEngine->getPointerToFunction(pFunc);
	}

	return nullptr;
}

void Dump(SEvalJit * pEvaljit)
{
  	for (llvm::Module * pModule : pEvaljit->arypModule)
		pModule->dump();
}

struct SVariable
{
	const char * pChzName;
	llvm::Value * pValueAddr;
	int iBStack;
};

SVariable * PvarFind(int iBStack, SVariable * aVar, int cVar)
{
	for (int iVar = 0; iVar < cVar; ++iVar)
	{
		if (aVar[iVar].iBStack == iBStack)
			return &aVar[iVar];
	}

	return nullptr;
}

llvm::Type * PtypeFromType(const SType & type)
{
	if (pType->typek == TYPEK_Int || type.typek == TYPEK_Bool)
	{
		return llvm::getIntNTy(type.cBit);
	}
	else if (type.typek == TYPEK_Float && type.cBit == 32)
	{
		return llvm::getFloatTy();
	}
	else if (type.typek == TYPEK_Float && type.cBit == 64)
	{
		return llvm::getDoubleTy();
	}
	else if (type.typek == TYPEK_String)
	{
		return llvm::getInt8PtrTy();
	}
	else
	{
		ASSERT(false);
		return nullptr;
	}
}

template <class T>
void Sort(SArray<T> * pAry, int (*pfncmp)(const T *, const T *))
{
	qsort(pAry->a, pAry->c, sizeof(T), static_cast<int (*)(const void *, const void *)>(pfncmp));
}

void BuildIr(SEvalJit * pEvaljit, SCompiler * pCompiler)
{
	SByteCode * aBytecode = pCompiler->aryBytecode.a;
	int cBytecode = pCompiler->aryBytecode.c;

	auto pLlvmctx = pEvaljit->m_pLlvmctx;
	auto pBuilder = pEvaljit->pBuilder;
	llvm::Value * apValue[32] = {};
	SVariable aVar[32] = {};
	int cVar = 0;

	struct SLabelLoc
	{
		LABEL label;
		int iBytecode;
	};

	SLabelLoc aLabelloc[64] = {};

	for (int label = 0; label < pCompiler->aryiBytecodeLabel.c; ++label)
	{
		aLabelloc
	}

	for (int iBytecode = 0; iBytecode < cBytecode; ++iBytecode)
	{
		const SByteCode & bytecode = aBytecode[iBytecode];
		BCK bck = bytecode.bck;
		switch (bck)
		{
		case BCK_LoadImmediate:
			{
				const SType & type = bytecode.loadliteral.type;
				llvm::Value * pValue = nullptr;
				if (type.typek == TYPEK_Int || type.typek == TYPEK_UInt || type.typek == TYPEK_Bool)
				{
					pValue = llvm::ConstantInt::get(pLlvmctx, APInt(&bytecode.loadliteral.n, bytecode.loadliteral.cBit));
				}
				else if (type.typek == TYPEK_Float)
				{
					llvm::fltSemantics floatsize = llvm::APFloat::IEEEsingle;
					if (bytecode.loadliteral.cBit == 64)
						floatsize = llvm::APFloat::IEEEdouble;
					else
						ASSERT(false);

					pValue = llvm::ConstantFP::get(pLlvmctx, APFloat(&bytecode.loadliteral.g, floatsize));
				}
				else if (type.typek == TYPEK_String)
				{
					pValue = llvm::ConstantArray::get(pLlvmctx, bytecode.loadliteral.pChz);
				}
				else
				{
					ASSERT(false);
				}

				int iReg = bytecode.loadliteral.iReg;
				ASSERT(iReg < DIM(apValue));
				apValue[iReg] = pValue;
			}
			break;

		case BCK_ResetRegisters:
			{
				ClearStruct(&apValue);
			}
			break;

		case BCK_StartScope:
			break;

		case BCK_EndScope:
			{
				for (; cVar >= 1; --cVar)
				{
					if (aVar[cVar - 1].iBStack < bytecode.stack.iBStack)
						break;
					ClearStruct(&aVar[cVar - 1]);
				}
			}
			break;

		case BCK_StoreVariable:
			{
				llvm::Value * pValue = apValue[bytecode.stack.iReg];
				SVariable * pVar = PvarFind(bytecode.stack.iBStack, aVar, cVar);
				if (iVar < 0)
				{
					pVar = aVar[cVar++];
					pVar->pChzName = bytecode.stack.pChzName;
					pVar->iBStack = bytecode.stack.iBStack;

					auto pFunc = pEvaljit->pFunc;
					llvm::IRBuilder<> irDeclare(&pFunc->getEntryBlock(), pFunc->getEntryBlock().begin());
					pVar->pValueAddr = irDeclare.CreateAlloca(PtypeFromType(bytecode.stack.pType), 0, pVar->pChzName);
				}

				pBuilder->CreateStore(pValue, pVar->pValueAddr);
			}
			break;

		case BCK_LoadVariable:
			{
				SVariable * pVar = PvarFind(bytecode.stack.iBStack, aVar, cVar);
				ASSERT(pVar);

				apValue[bytecode.stack.iReg] = pBuilder->CreateLoad(pVar->pValueAddr, pVar->pChzName);
			}
			break;

		case BCK_PrintReg:
			{
				// BB (adrianb) Call externed function for printing each type
				//PrintRegister(pEval->aryReg[bytecode.simple.iReg + iRegMic]);
			}
			break;

		case BCK_PrintEnd:
			{
				// BB (adrianb) Call externed function for printing newline.
				//printf("\n");
			}
			break;

		case BCK_Jump:

			break;
		case BCK_JumpIf:
			{
				LABEL label = bytecode.jump.labelTrue;
				if (bck == BCK_JumpIf)
				{
					pEval->aryReg[bytecode.jump.iReg]
					ASSERT(.type.typek == TYPEK_Bool);
					if (pEval->aryReg[bytecode.jump.iReg].n == 0)
						label = bytecode.jump.labelFalse;
				}

				iBytecode = pCompiler->aryiBytecodeLabel[label] - 1;
			}
			break;

		case BCK_Negate:
		case BCK_BitwiseNegate:
			{
				SRegister * pRegRet = PregPrepare(pEval, bytecode.binary.iRegRet);
				const SRegister & regArg0 = pEval->aryReg[bytecode.binary.iRegArg0];
				pRegRet->pType = regArg0.type;
				if (bck == BCK_Negate && regArg0.type.typek == TYPEK_Int)
					pRegRet->n = -regArg0.n;
				else if (bck == BCK_Negate && regArg0.type.typek == TYPEK_Float)
					pRegRet->g = -regArg0.g;
				else if (bck == BCK_BitwiseNegate && (regArg0.type.typek == TYPEK_Int || regArg0.type.typek == TYPEK_UInt))
					pRegRet->n = ~regArg0.n;
				else
					ASSERT(false);
			}
			break;

		case BCK_LogicalAnd:
		case BCK_LogicalOr:
		case BCK_BitwiseAnd:
		case BCK_BitwiseOr:
		case BCK_Add:
		case BCK_Subtract:
		case BCK_Multiply:
		case BCK_Divide:
		case BCK_Modulo:
			{
				SRegister * pRegRet = PregPrepare(pEval, bytecode.binary.iRegRet);
				const SRegister & regArg0 = pEval->aryReg[bytecode.binary.iRegArg0];
				const SRegister & regArg1 = pEval->aryReg[bytecode.binary.iRegArg1];
				TYPEK typekArg0 = regArg0.type.typek;
				pRegRet->pType = regArg0.type;
				if (bck == BCK_LogicalAnd || bck == BCK_BitwiseAnd)
					pRegRet->n = regArg0.n & regArg1.n;
				else if (bck == BCK_LogicalOr || bck == BCK_BitwiseOr)
					pRegRet->n = regArg0.n | regArg1.n;
				else if (bck == BCK_Add && typekArg0 == TYPEK_Int)
						pRegRet->n = regArg0.n + regArg1.n;
				else if (bck == BCK_Add && typekArg0 == TYPEK_Float)
					pRegRet->g = regArg0.g + regArg1.g;
				else if (bck == BCK_Subtract && typekArg0 == TYPEK_Int)
						pRegRet->n = regArg0.n - regArg1.n;
				else if (bck == BCK_Subtract && typekArg0 == TYPEK_Float)
					pRegRet->g = regArg0.g - regArg1.g;
				else if (bck == BCK_Multiply && typekArg0 == TYPEK_Int)
						pRegRet->n = regArg0.n * regArg1.n;
				else if (bck == BCK_Multiply && typekArg0 == TYPEK_Float)
					pRegRet->g = regArg0.g * regArg1.g;
				else if (bck == BCK_Divide && typekArg0 == TYPEK_Int)
						pRegRet->n = regArg0.n / regArg1.n;
				else if (bck == BCK_Divide && typekArg0 == TYPEK_Float)
					pRegRet->g = regArg0.g / regArg1.g;
				else if (bck == BCK_Modulo && typekArg0 == TYPEK_Int)
						pRegRet->n = regArg0.n % regArg1.n;
				else if (bck == BCK_Modulo && typekArg0 == TYPEK_Float)
					pRegRet->g = fmod(regArg0.g, regArg1.g);
				else
					ASSERT(false);
			}
			break;
		
		case BCK_LessThan:
		case BCK_GreaterThan:
		case BCK_LessThanOrEqual:
		case BCK_GreaterThanOrEqual:
		case BCK_CmpEqual:
		case BCK_CmpNotEqual:
			{
				int iRegRet = bytecode.binary.iRegRet;
				SetSizeAtLeast(&pEval->aryReg, iRegRet + 1);
				const SRegister & regArg0 = pEval->aryReg[bytecode.binary.iRegArg0];
				const SRegister & regArg1 = pEval->aryReg[bytecode.binary.iRegArg1];
				TYPEK typekArg0 = regArg0.type.typek;
				SRegister * pReg = &pEval->aryReg[iRegRet];
				pReg->pType.typek = TYPEK_Bool;
				pReg->pType.cBit = 1; 
				if (bck == BCK_LessThan && typekArg0 == TYPEK_Int)
					pReg->n = regArg0.n < regArg1.n;
				else if (bck == BCK_LessThan && typekArg0 == TYPEK_UInt)
					pReg->n = uint64_t(regArg0.n) < uint64_t(regArg1.n);
				else if (bck == BCK_LessThan && typekArg0 == TYPEK_Float)
					pReg->n = regArg0.g < regArg1.g;
				else if (bck == BCK_GreaterThan && typekArg0 == TYPEK_Int)
					pReg->n = regArg0.n > regArg1.n;
				else if (bck == BCK_GreaterThan && typekArg0 == TYPEK_UInt)
					pReg->n = uint64_t(regArg0.n) > uint64_t(regArg1.n);
				else if (bck == BCK_GreaterThan && typekArg0 == TYPEK_Float)
					pReg->n = regArg0.g > regArg1.g;
				else if (bck == BCK_LessThanOrEqual && typekArg0 == TYPEK_Int)
					pReg->n = regArg0.n <= regArg1.n;
				else if (bck == BCK_LessThanOrEqual && typekArg0 == TYPEK_UInt)
					pReg->n = uint64_t(regArg0.n) <= uint64_t(regArg1.n);
				else if (bck == BCK_LessThanOrEqual && typekArg0 == TYPEK_Float)
					pReg->n = regArg0.g <= regArg1.g;
				else if (bck == BCK_GreaterThanOrEqual && typekArg0 == TYPEK_Int)
					pReg->n = regArg0.n >= regArg1.n;
				else if (bck == BCK_GreaterThanOrEqual && typekArg0 == TYPEK_UInt)
					pReg->n = uint64_t(regArg0.n) >= uint64_t(regArg1.n);
				else if (bck == BCK_GreaterThanOrEqual && typekArg0 == TYPEK_Float)
					pReg->n = regArg0.g >= regArg1.g;
				else if (bck == BCK_CmpEqual && (typekArg0 == TYPEK_Int || typekArg0 == TYPEK_UInt))
					pReg->n = regArg0.n == regArg1.n;
				else if (bck == BCK_CmpNotEqual && (typekArg0 == TYPEK_Int || typekArg0 == TYPEK_UInt))
					pReg->n = regArg0.n != regArg1.n;
				else 
					ASSERT(false);
			}
			break;

		default:
			{
				ASSERT(false);
				return;
			}
		}
	}
}
#endif

#if 0
static MCJITHelper *JITHelper;
static IRBuilder<> Builder(getGlobalContext());
static std::map<std::string, Value *> NamedValues;

Value *ErrorV(const char *Str) {
  Error(Str);
  return 0;
}

Value *NumberExprAST::Codegen() {
  return ConstantFP::get(getGlobalContext(), APFloat(Val));
}

Value *VariableExprAST::Codegen() {
  // Look this variable up in the function.
  Value *V = NamedValues[Name];
  return V ? V : ErrorV("Unknown variable name");
}

Value *BinaryExprAST::Codegen() {
  Value *L = LHS->Codegen();
  Value *R = RHS->Codegen();
  if (L == 0 || R == 0)
    return 0;

  switch (Op) {
  case '+':
    return Builder.CreateFAdd(L, R, "addtmp");
  case '-':
    return Builder.CreateFSub(L, R, "subtmp");
  case '*':
    return Builder.CreateFMul(L, R, "multmp");
  case '<':
    L = Builder.CreateFCmpULT(L, R, "cmptmp");
    // Convert bool 0/1 to double 0.0 or 1.0
    return Builder.CreateUIToFP(L, Type::getDoubleTy(getGlobalContext()),
                                "booltmp");
  default:
    return ErrorV("invalid binary operator");
  }
}

Value *CallExprAST::Codegen() {
  // Look up the name in the global module table.
  Function *CalleeF = JITHelper->getFunction(Callee);
  if (CalleeF == 0)
    return ErrorV("Unknown function referenced");

  // If argument mismatch error.
  if (CalleeF->arg_size() != Args.size())
    return ErrorV("Incorrect # arguments passed");

  std::vector<Value *> ArgsV;
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgsV.push_back(Args[i]->Codegen());
    if (ArgsV.back() == 0)
      return 0;
  }

  return Builder.CreateCall(CalleeF, ArgsV, "calltmp");
}

Function *PrototypeAST::Codegen() {
  // Make the function type:  double(double,double) etc.
  std::vector<Type *> Doubles(Args.size(),
                              Type::getDoubleTy(getGlobalContext()));
  FunctionType *FT =
      FunctionType::get(Type::getDoubleTy(getGlobalContext()), Doubles, false);

  std::string FnName = MakeLegalFunctionName(Name);

  Module *M = JITHelper->getModuleForNewFunction();

  Function *F = Function::Create(FT, Function::ExternalLinkage, FnName, M);

  // If F conflicted, there was already something named 'Name'.  If it has a
  // body, don't allow redefinition or reextern.
  if (F->getName() != FnName) {
    // Delete the one we just made and get the existing one.
    F->eraseFromParent();
    F = JITHelper->getFunction(Name);
    // If F already has a body, reject this.
    if (!F->empty()) {
      ErrorF("redefinition of function");
      return 0;
    }

    // If F took a different number of args, reject.
    if (F->arg_size() != Args.size()) {
      ErrorF("redefinition of function with different # args");
      return 0;
    }
  }

  // Set names for all arguments.
  unsigned Idx = 0;
  for (Function::arg_iterator AI = F->arg_begin(); Idx != Args.size();
       ++AI, ++Idx) {
    AI->setName(Args[Idx]);

    // Add arguments to variable symbol table.
    NamedValues[Args[Idx]] = AI;
  }

  return F;
}

Function *FunctionAST::Codegen() {
  NamedValues.clear();

  Function *TheFunction = Proto->Codegen();
  if (TheFunction == 0)
    return 0;

  // Create a new basic block to start insertion into.
  BasicBlock *BB = BasicBlock::Create(getGlobalContext(), "entry", TheFunction);
  Builder.SetInsertPoint(BB);

  if (Value *RetVal = Body->Codegen()) {
    // Finish off the function.
    Builder.CreateRet(RetVal);

    // Validate the generated code, checking for consistency.
    verifyFunction(*TheFunction);

    return TheFunction;
  }

  // Error reading body, remove function.
  TheFunction->eraseFromParent();
  return 0;
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Driver
//===----------------------------------------------------------------------===//

static void HandleDefinition() {
  if (FunctionAST *F = ParseDefinition()) {
    if (Function *LF = F->Codegen()) {
      fprintf(stderr, "Read function definition:");
      LF->dump();
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleExtern() {
  if (PrototypeAST *P = ParseExtern()) {
    if (Function *F = P->Codegen()) {
      fprintf(stderr, "Read extern: ");
      F->dump();
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (FunctionAST *F = ParseTopLevelExpr()) {
    if (Function *LF = F->Codegen()) {
      // JIT the function, returning a function pointer.
      void *FPtr = JITHelper->getPointerToFunction(LF);

      // Cast it to the right type (takes no arguments, returns a double) so we
      // can call it as a native function.
      double (*FP)() = (double (*)())(intptr_t)FPtr;
      fprintf(stderr, "Evaluated to %f\n", FP());
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
  while (1) {
    fprintf(stderr, "ready> ");
    switch (CurTok) {
    case tok_eof:
      return;
    case ';':
      getNextToken();
      break; // ignore top-level semicolons.
    case tok_def:
      HandleDefinition();
      break;
    case tok_extern:
      HandleExtern();
      break;
    default:
      HandleTopLevelExpression();
      break;
    }
  }
}
#endif

#endif // USE_LLVM

#define TEST_CALL_CONVENTION 0
#if TEST_CALL_CONVENTION
extern "C"
{
	int AddN(int a, int b)
	{
		return a + b;
	}

	float AddG(float a, float b)
	{
		return a + b;
	}

	int8_t AddS8(int8_t a, int8_t b)
	{
		return a + b;
	}

	int64_t AddS64(int64_t a, int64_t b)
	{
		return a + b;
	}

	uint64_t * AddOffset(uint64_t * pN, int iN)
	{
		return pN + iN;
	}

	struct SSimple
	{
		int a;
		int b;
		bool c;
		float d;
		double e;
	};

	SSimple AddSimple(SSimple s0, SSimple s1)
	{
		SSimple s = {s0.a + s1.a, s0.b + s1.b, s0.c && s1.c, s0.d + s1.d, s0.e + s1.e};
		return s;
	}
}
#endif // TEST_CALL_CONVENTION

int main(int cpChzArg, const char * apChzArg[])
{
#if TEST_CALL_CONVENTION
	{
		int a = AddN(1, 2);
		float b = AddG(2.0f, 3.0f);
		int8_t c = AddS8(5, 6);
		int64_t d = AddS64(20, 30);
		uint64_t * e = AddOffset(nullptr, 16);
		SSimple s1 = {7, 8, true, 4, 5};
		SSimple s2 = AddSimple(s1, s1);

		printf("Stuff %#x %g %#x %#llx %p %#x %g\n", a, b, c, d, e, s2.b, s2.e);
	}
#endif // TEST_CALL_CONVENTION

	if (cpChzArg != 2)
	{
		ShowErrRaw("Invalid command line");
		ShowHelp();
		return -1;
	}

	const char * pChzFile = apChzArg[1];
	const char * pChzContents = PchzLoadWholeFile(pChzFile);
	if (pChzContents == nullptr)
    {
        printf("why couldn't we read the file");
		return -1;
    }

	SParser parser;
	Init(&parser);
	StartNewFile(&parser, pChzFile, pChzContents);
	SAst * pAst = PastParseRoot(&parser);

	PrintAst(0, pAst);

	// Test libffi

	// Because the return value from foo() is smaller than sizeof(long), it
	// must be passed as ffi_arg or ffi_sarg.
	//ffi_arg result;

	// Specify the data type of each argument. Available types are defined
	// in <ffi/ffi.h>.

	double gResult = 0;

	{
		ffi_type *aFfitypeArg[2] = { &ffi_type_double, &ffi_type_double };

		// Prepare the ffi_cif structure.
		ffi_cif cif;
		ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, DIM(aFfitypeArg), &ffi_type_double, aFfitypeArg);
		if (status != FFI_OK)
		{
			printf("ffi error %d", status);
		}

		// Specify the values of each argument.
		double g1 = 2;
		double g2 = 1;

		void *arg_values[2] = { &g1, &g2 };
		ffi_call(&cif, FFI_FN(dlsym(RTLD_DEFAULT, "atan2")), &gResult, arg_values);
	}

	printf("result is %g rad\n", gResult);

	{
		ffi_type *aFfitypeArg[2] = { &ffi_type_pointer, &ffi_type_double };

		// Prepare the ffi_cif structure.
		ffi_cif cif;
		ffi_status status = ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, DIM(aFfitypeArg), &ffi_type_sint, aFfitypeArg);
		if (status != FFI_OK)
		{
			printf("ffi error %d", status);
		}

		// Specify the values of each argument.
		const char * pChzFormat = "Calling printf to print result %g\n";

		void *arg_values[2] = { &pChzFormat, &gResult };
		ffi_arg result;
		ffi_call(&cif, FFI_FN(dlsym(RTLD_DEFAULT, "printf")), &gResult, arg_values);
	}

	// BB (adrianb) Use dladdr, dlclose, dlerror, dlopen to open close processes.

#if 0
	SCompiler compiler;
	Init(&compiler);

	CompileRoot(&compiler, pAst);

	SEval eval;
	Init(&eval, &compiler);

	SFunction * pFuncMain = PfuncLookup(&compiler, "main");
	if (!pFuncMain)
	{
		ShowErrRaw("Couldn't find main function to evaluate");
	}

	Evaluate(&eval, pFuncMain, 0, -1, nullptr);
#endif

#if USE_LLVM
	// Generate LLVM IR

	llvm::InitializeNativeTarget();
	llvm::InitializeNativeTargetAsmPrinter();
	llvm::InitializeNativeTargetAsmParser();

	SEvalJit evaljit;
	Init(&evaljit);

	BuildIr(&evaljit, &scope);

	// Print out all of the generated code.

	Dump(&evaljit);
#endif // USE_LLVM

	return 0;
}

#if 0
	- Type checking.  Translate AST with types ala JBLOW's solution?  Or do this as part of byte code generation?
	- Generate/execute byte code.  Foreign functions work pretty easily with FFI.
	- Generate executable.  Translate to C or to LLVM?
#endif
