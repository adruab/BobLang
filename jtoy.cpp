/* Copyright (C) 2015 Adrian Bentley
|
| Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated 
| documentation files (the "Software"), to deal in the Software without restriction, including without limitation the 
| rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit 
| persons to whom the Software is furnished to do so, subject to the following conditions:
| 
| The above copyright notice and this permission notice shall be included in all copies or substantial portions of the 
| Software.
| 
| THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE 
| WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
| COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR 
| OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

// BB (adrianb) How much faster would compilation be if I manually declared the needed functions?

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>

#if 0
#include <ffi.h>
#if PLATFORM_OSX
#include <dlfcn.h>
#endif
#endif

#if WIN32
#include <intrin.h>
#endif

#include "llvm-c/Core.h"
#include "llvm-c/Analysis.h"
#include "llvm-c/BitWriter.h"

// Compilation phases:
// Parse to AST, no types
// Perform all #import directives
// Out of order typechecking:
//  1. Flatten AST into type check order list.
//  2. Iterate over list performing type checking, if we find a declaration that is unresolved, push current progress
//		onto the stack and switch to it.
//  3. Continue type checking popping off the progress stack until we're done.
// Generate: global variables then procedures.
//  Running modify procs during type checking will require generating and executing code during type checking.
//  How to keep fast?



// Some basic typedefs

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

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
			fflush(stdout); \
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
#define DIM(a) (int(sizeof(a)/sizeof(a[0])))

#define JOIN2(a,b) a##b
#define JOIN(a,b) JOIN2(a,b)

template <typename T>
T min (T a, T b)
{
	return (b < a) ? b : a;
}

template <typename T>
void Swap(T & t0, T & t1)
{
	T tTemp = t0;
	t0 = t1;
	t1 = tTemp;
}

struct SIntIter
{
	void operator ++() { ++i; }
	bool operator != (const SIntIter & iter) { return i != iter.i; }
	int operator *() { return i; }

	int i;
};

struct SIntRange
{
	SIntIter begin() const { return SIntIter{iMic}; }
	SIntIter end() const { return SIntIter{iMac}; }

	int iMic;
	int iMac;
};

SIntRange IterCount(int c)
{
	return SIntRange{0, c};
}

class CDeferHolderBase
{
};

template <class T>
class CDeferHolder : public CDeferHolderBase
{
public:
	CDeferHolder(const T & tIn): t(tIn) {}
	~CDeferHolder() { t(); }

	T t;
};

// BB (adrianb) Seems bad to overload all uses of unary ++, but doesn't seem to affect
//  ++ on ints or floats at least.

template <class T>
inline CDeferHolder<T> operator ++(const T & t) { return {t}; }

#define defer const CDeferHolderBase & JOIN(_defer, __LINE__) __attribute__((unused)) = ++ [&]

#if 0
void TestDefer()
{
	void * pV1 = malloc(16);
	void * pV2 = malloc(16);

	int n = 5;
	++n;

	float g = 1.5f;
	++g;

	struct S
	{
		int n;

		void operator ++()
		{
			++n;
		}
	};

	auto s = S{n};
	++s;

	const int & b = n;
	int a = -b;
	(void) a;

	defer { printf("Freeing pV1 and pV2; n=%d\n", n); free(pV1); free(pV2); };

	n = 10;
}
#endif



void ShowErrVa(const char * pChzFormat, va_list va)
{
	fflush(stdout);
	vfprintf(stderr, pChzFormat, va);
	fprintf(stderr, "\n");
	fflush(stderr);

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
		"jtoy [options] filename\n"
		"jtoy --run-unit-tests\n"
		"\n"
		"Options:\n"
		"  --print-ast,-p          Print scheme representation of syntax tree\n");
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

// BB (adrianb) If we switch to fixed operators consider representing TOKK as ascii and 
//  having any multi character tokens be 256 or bigger.

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

	TOKK_Literal,
	
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
		
		"Literal",
	};
	CASSERT(DIM(s_mpTokkPchz) == TOKK_Max);
	ASSERT(tokk >= 0 && tokk < TOKK_Max);

	return s_mpTokkPchz[tokk];
}

enum KEYWORD
{
	KEYWORD_Invalid,

	KEYWORD_If,
	KEYWORD_Then, // BB (adrianb) Totally pointless?
	KEYWORD_Else,
	KEYWORD_While,
	KEYWORD_For,

	KEYWORD_Continue,
	KEYWORD_Break,
	KEYWORD_Return,

	KEYWORD_Struct,
	KEYWORD_Enum,

	KEYWORD_Using,
	KEYWORD_Cast,
	KEYWORD_AutoCast,
	KEYWORD_Defer,

	KEYWORD_New,
	KEYWORD_Delete,
	KEYWORD_Remove, // BB (adrianb) This seems pretty terrible to be removing from normal usage...
	KEYWORD_Inline, // BB (adrianb) Is this INLINE_DIRECTIVE in compiler node?
	KEYWORD_NoInline, // BB (adrianb) Is this INLINE_DIRECTIVE in compiler node?
	KEYWORD_PushContext,

	KEYWORD_Null,

	KEYWORD_ImportDirective,
	KEYWORD_RunDirective,
	KEYWORD_CharDirective,
	KEYWORD_ForeignDirective,
	KEYWORD_ForeignLibraryDirective,
	KEYWORD_TypeDirective,
	
	KEYWORD_Max
};

const char * PchzFromKeyword(KEYWORD keyword)
{
	static const char * s_mpKeywordPchz[] =
	{
		"<invalid>",
		"if",
		"then",
		"else",
		"while",
		"for",
		"continue",
		"break",
		"return",
		"struct",
		"enum",
		"using",
		"cast",
		"xx", // BB (adrianb) Not very readable
		"defer",
		"new",
		"delete",
		"remove", // BB (adrianb) add is not a keyword but remove is? Yuck.
		"inline",
		"noinline",
		"push_context",
		"null",

		"#import", // BB (adrianb) Are these really keywords?
		"#run",
		"#char",
		"#foreign",
		"#foreign_library",
		"#type",
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
	fflush(stdout);
	if (errinfo.pChzFile)
	{
		fprintf(stderr, "%s:%d:%d : error: ", errinfo.pChzFile, errinfo.nLine, errinfo.nCol);
	}
	else
	{
		fprintf(stderr, "error: ");
	}

	va_list va;
	va_start(va, pChzFormat);

	vfprintf(stderr, pChzFormat, va);

	if (errinfo.pChzLine)
	{
		fprintf(stderr, "\n");

		char aChzLine[1024];
		char aChzHighlight[DIM(aChzLine)];
		{
			int iChMic = Max(0, errinfo.iChMic - 100);
			const char * pChzLineTrim = errinfo.pChzLine + iChMic;
			int cChMax = errinfo.iChMac + 32 - iChMic;
			int iChOut = 0;
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
	}

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
void Destroy(SArray<T> * pAry)
{
	free(pAry->a);
	ClearStruct(pAry);
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

template <class T, class TOther>
void Append(SArray<T> * pAry, const TOther & t)
{
	*PtAppendNew(pAry) = t;
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



template <class T>
struct SIterPointer
{
	void operator ++() { ++pT; }
	bool operator != (const SIterPointer & iter) { return pT != iter.pT; }
	T * operator *() { return pT; }

	T * pT;
};

template <class T>
struct SPointerRange
{
	SIterPointer<T> begin() const { return {pTMic}; }
	SIterPointer<T> end() const { return {pTMac}; }

	T * pTMic;
	T * pTMac;
};

template <class T>
SPointerRange<T> IterPointer(const SArray<T> & aryT)
{
	return {aryT.a, aryT.a + aryT.c};
}


inline bool FIsPowerOfTwo(s64 n)
{
	return (n & (n - 1)) == 0;
}

struct SPagedAlloc
{
	SArray<uint8_t *> arypB;
	u32 iB;
	u32 cBPage;
};

void Init(SPagedAlloc * pPagealloc, u32 cBPageDefault)
{
	ClearStruct(pPagealloc);
	pPagealloc->cBPage = cBPageDefault;
	pPagealloc->iB = cBPageDefault + 1;
}

void Destroy(SPagedAlloc * pPagealloc)
{
	for (auto pB : pPagealloc->arypB)
	{
		free(pB);
	}

	Destroy(&pPagealloc->arypB);
	ClearStruct(pPagealloc);
}

void * PvAlloc(SPagedAlloc * pPagealloc, size_t cB, size_t cBAlign)
{
	ASSERT(cBAlign <= 16 && FIsPowerOfTwo(cBAlign));
	ASSERT(cB <= pPagealloc->cBPage);
	u32 iB = (pPagealloc->iB + (cBAlign - 1)) & ~(cBAlign - 1);
	u32 iBMac = iB + cB;

	if (iBMac > pPagealloc->cBPage)
	{
		void * pVAlloc = malloc(pPagealloc->cBPage);
		ASSERT((intptr_t(pVAlloc) & 0xf) == 0);
		Append(&pPagealloc->arypB, static_cast<uint8_t *>(pVAlloc));
		iB = 0;
		iBMac = cB;
	}

	pPagealloc->iB = iBMac;

	void * pV = Tail(&pPagealloc->arypB) + iB;
	memset(pV, 0, cB);
	return pV;
}

template <class T>
inline T * PtAlloc(SPagedAlloc * pPagealloc, int cT = 1)
{
	return static_cast<T *>(PvAlloc(pPagealloc, sizeof(T) * cT, alignof(T)));
}


template <class T>
struct SSetNode
{
	u32 hv;
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
void AddImpl(SSet<T> * pSet, u32 hv, const T & t)
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

template <class T>
void Add(SSet<T> * pSet, u32 hv, const T & t)
{
	EnsureCount(pSet, pSet->c + 1);
	AddImpl(pSet, hv, t);
}

template <class T>
void Destroy(SSet<T> * pSet)
{
	free(pSet->aNode);
	ClearStruct(pSet);
}

template <class T, class TOther>
const T * PtLookupImpl(SSet<T> * pSet, u32 hv, const TOther & t)
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

template <class K, class E>
struct SHashNode
{
	u32 hv;
	bool fFull;
	K k;
	E e;
};

template <class K, class E>
struct SHash
{
	SHashNode<K, E> * aNode;
	int c;
	int cMax;
};

template <class K, class E>
void AddImpl(SHash<K, E> * pHash, u32 hv, const K & k, const E & e)
{
	// BB (adrianb) Use hopscotch instead? http://en.wikipedia.org/wiki/Hash_table#Open_addressing

	int cMax = pHash->cMax;
	int iNodeBase = hv % cMax;
	int iNode = iNodeBase;

	ASSERT(pHash->c + 1 <= cMax);

	if (pHash->aNode[iNode].fFull)
	{
		for (int diNode = 0;; ++diNode)
		{
			ASSERT(diNode < cMax);
			iNode = (iNodeBase + diNode) % cMax;
			if (!pHash->aNode[iNode].fFull)
				break;
		}
	}

	auto pNode = &pHash->aNode[iNode];
	pNode->hv = hv;
	pNode->fFull = true;
	pNode->k = k;
	pNode->e = e;

	pHash->c += 1;
}

template <class K, class E>
void EnsureCount(SHash<K, E> * pHash, int c)
{
	static float s_rResize = 0.7f;
	if (c < int(pHash->cMax * s_rResize))
		return;
	
	int cMax = pHash->cMax;
	auto aNode = pHash->aNode;

	pHash->c = 0;
	pHash->cMax = cMax + 256;
	pHash->aNode = static_cast<SHashNode<K, E> *>(calloc(pHash->cMax, sizeof(SHashNode<K, E>)));

	for (int iNode = 0; iNode < cMax; ++iNode)
	{
		if (aNode[iNode].fFull)
		{
			AddImpl(pHash, aNode[iNode].hv, aNode[iNode].k, aNode[iNode].e);
		}
	}

	free(aNode);
}

template <class K, class E>
void Add(SHash<K, E> * pHash, u32 hv, const K & k, const E & e)
{
	EnsureCount(pHash, pHash->c + 1);
	AddImpl(pHash, hv, k, e);
}

template <class K, class E, class KOther>
E * PtLookupImpl(SHash<K, E> * pHash, u32 hv, const KOther & k)
{
	int cMax = pHash->cMax;
	if (cMax == 0)
		return nullptr;
	
	int iNodeBase = hv % cMax;
	for (int diNode = 0;; ++diNode)
	{
		ASSERT(diNode < cMax);
		int iNode = (iNodeBase + diNode) % cMax;
		auto pNode = &pHash->aNode[iNode];
		if (!pNode->fFull)
			return nullptr;

		if (pNode->hv == hv && FIsKeyEqual(pNode->k, k))
			return &pNode->e;
	}
}

template <class T, class E>
void Destroy(SHash<T, E> * pHash)
{
	free(pHash->aNode);
	ClearStruct(pHash);
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

// BB (adrianb) && is higher than != yuck. Requires strcmp everywhere, yuck.
//  Switch to single constant enums?

const SOperator g_aOperator[] =
{
	{}, // Arrow-like operators
	{}, // Assignment-like operators
	{"@:?", nullptr},
	{"", "or xor"},
	{"", "and"},
	{"=<>!", "in notin is isnot not of"},
	{"&", nullptr},
	{"+-|~", nullptr},
	{"*/%", nullptr},
	{"$^", nullptr}
};

const char * g_pChzOperatorAll = "@:?=<>!.&+-|~*/%$^";

const char * PchzOperatorClean(const char * pChz)
{
	// Convert into the world version so we can get correct precedence.

	if (strcmp(pChz, "&&") == 0)
		return "and";
	if (strcmp(pChz, "||") == 0)
		return "or";

	return pChz;
}

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

	// Dot is highest priority and handled specially

	if (pChzOp[0] == '.')
	{
		return DIM(g_aOperator);
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

				if (strncmp(pChzOp, pChzWords, cChWord) == 0 && pChzOp[cChWord] == '\0')
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

inline int NLog2(s64 n)
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

enum LITK
{
	LITK_Bool,
	LITK_Int,
	LITK_Float,
	LITK_String,

	LITK_Max,
};

const char * PchzFromLitk(LITK litk)
{
	static const char * s_mpLitkPchz[] =
	{
		"bool",
		"int",
		"float",
		"string",
	};
	CASSERT(DIM(s_mpLitkPchz) == LITK_Max);
	ASSERT(litk >= 0 && litk < LITK_Max);

	return s_mpLitkPchz[litk];
}

struct SLiteral // tag = lit
{
	LITK litk;
	union
	{
		const char * pChz;
		s64 n;
		double g;
	};
};

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
		} comment;

		SLiteral lit;

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

struct SAst;
struct SAstBlock;
struct SAstProcedure;
struct SType;

struct SModule
{
	const char * pChzFile; // BB (adrianb) Add full path?
	const char * pChzContents;
	bool fAllocContents;
	bool fBuiltIn;
	SAstBlock * pAstblockRoot;

	SArray<SAstProcedure *> arypAstproc;

	// BB (adrianb) Get access to the other modules' symbol tables explicitly ala python.
};

struct STypeId // tag = tid
{
	const SType * pType; // Interred type never set this to a type that hasn't been uniquified
};

inline bool operator == (STypeId tid0, STypeId tid1)
{
	return tid0.pType == tid1.pType;
}

inline bool operator != (STypeId tid0, STypeId tid1)
{
	return tid0.pType != tid1.pType;
}

enum SYMTBLK
{
	SYMTBLK_Scope,
	SYMTBLK_Procedure,
	SYMTBLK_TopLevel,
	SYMTBLK_Struct,

	SYMTBLK_Max,
	SYMTBLK_RegisterAllMic = SYMTBLK_TopLevel
};

struct SAstProcedure;
struct SAstDeclareSingle;
struct SStorage;
struct STypeStruct;
struct SResolveDecl;
struct SProcedure;
struct SGlobal;

struct SSymbolTable
{
	SYMTBLK symtblk;
	SSymbolTable * pSymtParent;
	SArray<SResolveDecl *> arypResdecl; // Declarations, may be finished or unfinished if out of order
	int ipResdeclUsing;

	SAstProcedure * pAstproc; // Function for procedure symbol tables
	STypeStruct * pTypestruct; // Function for procedure symbol tables
};

struct SWorkspace
{
	SPagedAlloc pagealloc;
	SSet<const char *> setpChz;
	SArray<SModule> aryModule;
	int iModuleParse;

	int cOperator;

	// Tokenizing

	const char * pChzCurrent;
	SErrorInfo errinfo;
	bool fBeginLine;
	int cSpace;

	SArray<SToken> aryTokNext;
	bool fInToken;
	int cPeek;

	// Parsing

	SArray<SAst *> arypAstAll;

	// Type checking data

	SSet<STypeId> setTid;			// Unique types

	STypeId tidVoid;				// Commonly used builtin types
	STypeId tidBool;				//  ...
	STypeId tidS8;					//  ...
	STypeId tidS16;					//  ...
	STypeId tidS32;					//  ...
	STypeId tidS64;					//  ...
	STypeId tidU8;					//  ...
	STypeId tidU32;					//  ...
	STypeId tidFloat;				//  ...
	STypeId tidString;				//  ...

	SSymbolTable symtBuiltin; 		// Contains built in types (int, float etc.)
	SSymbolTable symtRoot; 			// Contains all root level declarations from 

	SArray<SSymbolTable *> arypSymtAll; // All symbol tables for destroying

	SArray<STypeStruct *> arypTypestruct; // All structs
	SHash<STypeId, SSymbolTable *> hashTidPsymtStruct; // Symbol tables for out of order struct type checking
	SHash<SAst *, SResolveDecl *> hashPastPresdeclResolved;
};

struct SStringWithLength
{
	const char * pCh;
	int cCh;
};

u32 HvFromKey(const char * pCh, int cCh)
{
	u32 hv = 0;

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

const char * PchzCopy(SWorkspace * pWork, const char * pCh, s64 cCh)
{
	// NOTE (adrianb) Inlining PtLookupImpl to avoid making copy of the string up front.
	//  Have a LookupOther and pass pChz,cBStr?

	u32 hv = HvFromKey(pCh, cCh);
	SStringWithLength strwl = { pCh, int(cCh) };
	const char * const * ppChz = PtLookupImpl(&pWork->setpChz, hv, strwl);
	if (ppChz)
		return *ppChz;

	char * pChz = static_cast<char *>(PvAlloc(&pWork->pagealloc, cCh + 1, 1));
	memcpy(pChz, pCh, cCh);
	pChz[cCh] = '\0';

	Add<const char *>(&pWork->setpChz, hv, pChz);

	return pChz;
}

// BB (adrianb) pWork is just for the paged alloc.  Just malloc instead?

void AddModuleFile(SWorkspace * pWork, const char * pChzFile)
{
	// BB (adrianb) Deal with full paths with working directory?
	// BB (adrianb) Just use _split_path and _make_path?

	char aChzFile[256];
	{
		int cCh = strlen(pChzFile);
		ASSERT(DIM(aChzFile) > cCh + 4);
		memcpy(aChzFile, pChzFile, cCh);
		for (int iCh = 0;; ++iCh)
		{
			if (!(iCh < 10 && iCh < cCh))
			{
				memcpy(aChzFile + cCh, ".jai", 4);
				cCh += 4;
				break;
			}
			
			if (aChzFile[cCh - 1 - iCh] == '.')
				break;
		}

		aChzFile[cCh] = '\0';
	}

	for (const SModule & module : pWork->aryModule)
	{
		if (strcmp(module.pChzFile, aChzFile) == 0)
			return;
	}

	PtAppendNew(&pWork->aryModule)->pChzFile = PchzCopy(pWork, aChzFile, strlen(aChzFile));
}

void StartParseNewFile(SWorkspace * pWork, const char * pChzFile, const char * pChzContents)
{
	ASSERT(pWork->aryTokNext.c == 0);
	ASSERT(!pWork->fInToken && pWork->cPeek == 0);

	ClearStruct(&pWork->errinfo);
	pWork->errinfo.pChzLine = pChzContents;
	pWork->errinfo.pChzFile = pChzFile;
	pWork->errinfo.nLine = 1;
	pWork->errinfo.nCol = 1;
	pWork->pChzCurrent = pChzContents;
	pWork->fBeginLine = true;
	pWork->cSpace = 0;
}

inline char Ch(const SWorkspace * pWork, int iCh = 0)
{
	return pWork->pChzCurrent[iCh]; 
}

inline bool FIsDone(const SWorkspace * pWork)
{
	return Ch(pWork) == '\0';
}

inline bool FIsOperator(const SToken & tok, const char * pChz)
{
	return tok.tokk == TOKK_Operator && strcmp(tok.op.pChz, pChz) == 0;
}

inline void FillInErrInfo(SWorkspace * pWork, SErrorInfo * pErrinfo)
{
	*pErrinfo = pWork->errinfo;
	pErrinfo->iChMic = pWork->pChzCurrent - pWork->errinfo.pChzLine;
	pErrinfo->iChMac = pErrinfo->iChMic + 1;
}

inline SToken * PtokStart(TOKK tokk, SWorkspace * pWork)
{
	ASSERT(!pWork->fInToken);
	pWork->fInToken = true;

	SToken * pTok = PtAppendNew(&pWork->aryTokNext);
	pTok->tokk = tokk;
	pTok->fBeginLine = pWork->fBeginLine;
	pTok->cSpace = pWork->cSpace;
	FillInErrInfo(pWork, &pTok->errinfo);
	return pTok;
}

inline void EndToken(SToken * pTok, SWorkspace * pWork)
{
	ASSERT(pTok == &Tail(&pWork->aryTokNext));
	pWork->fInToken = false;
	if (pWork->errinfo.pChzLine != pTok->errinfo.pChzLine)
	{
		pTok->errinfo.iChMac = 50000;
	}
	else
	{
		pTok->errinfo.iChMac = pWork->pChzCurrent - pWork->errinfo.pChzLine;
	}
}

void AdvanceChar(SWorkspace * pWork)
{
	char ch = *pWork->pChzCurrent;
	if (!ch)
		return;

	// NOTE (adrianb) This should work with utf8 because we're only looking for ascii characters.

	// BB (adrianb) Wrapping character advancement at such a low level is likely very slow.
	//  There's an STB paper on lexing speed which I need to find again.

	pWork->pChzCurrent++;

	if (ch == '\n')
	{
		pWork->errinfo.pChzLine = pWork->pChzCurrent;
		pWork->errinfo.nLine += 1;
		pWork->errinfo.nCol = 1;
		pWork->cSpace = 0;
		pWork->fBeginLine = true;
	}
	else if (ch == '\r')
	{
		// Don't count this as anything
	}
	else if (ch == '\t')
	{
		// Add 1-4 spaces
		int nColPrev = pWork->errinfo.nCol;
		pWork->errinfo.nCol = ((nColPrev + 4) & ~3);
		pWork->cSpace += pWork->errinfo.nCol - nColPrev;
	}
	else if (ch == ' ')
	{
		pWork->errinfo.nCol++;
	}
	else
	{
		pWork->errinfo.nCol++;
		pWork->cSpace = 0;
		pWork->fBeginLine = false;
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

TOKK TokkCheckSimpleToken(SWorkspace * pWork)
{
	char chPeek = Ch(pWork);
	for (int iStok = 0; iStok < DIM(g_aStok); ++iStok)
	{
		if (chPeek == g_aStok[iStok].ch)
		{
			return g_aStok[iStok].tokk;
		}
	}

	return TOKK_Invalid;
}

s64 NParseIntegerBase10(SWorkspace * pWork)
{
	s64 n = 0;
	for (; FIsDigit(Ch(pWork)); AdvanceChar(pWork))
	{
		// BB (adrianb) Check for overflow?

		n = n * 10 + (Ch(pWork) - '0');
	}

	return n;
}

s64 NDigitBase16(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A';
	if (c >= 'a' && c <= 'f')
		return c - 'a';

	return -1;
}

s64 NParseIntegerBase16(SWorkspace * pWork)
{
	s64 n = 0;
	for (;;)
	{
		s64 nDigit = NDigitBase16(Ch(pWork));
		if (nDigit < 0)
			break;

		// BB (adrianb) Check for overflow

		n = n * 16 + nDigit;

		AdvanceChar(pWork);
	}

	return n;
}

s64 NParseIntegerBase8(SWorkspace * pWork)
{
	s64 n = 0;
	for (;;)
	{
		s64 nDigit = NDigitBase16(Ch(pWork));
		if (nDigit < 0)
			break;

		// BB (adrianb) Check for overflow

		n = n * 8 + nDigit;

		AdvanceChar(pWork);
	}

	return n;
}

void TokenizeInt(SWorkspace * pWork)
{
	SToken * pTok = PtokStart(TOKK_Literal, pWork);
	
	// Parse decimal
	// BB (adrianb) Support binary?

	s64 n = 0;

	switch (Ch(pWork))
	{
	case '0':
		{
			AdvanceChar(pWork);
			if (Ch(pWork) == 'x')
			{
				AdvanceChar(pWork);
				n = NParseIntegerBase16(pWork);
			}
			else
			{
				n = NParseIntegerBase8(pWork);
			}
		}
		break;

	default:
		n = NParseIntegerBase10(pWork);
		break;
	}

	pTok->lit.litk = LITK_Int;
	pTok->lit.n = n;

	// BB (adrianb) Provide a way to specific explicit size?

	EndToken(pTok, pWork);
}

void TokenizeFloat(SWorkspace * pWork)
{	
	// Parse decimal
	// BB (adrianb) Is there a safer way to parse a float?

	const char * pChzStart = pWork->pChzCurrent;
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
		AdvanceChar(pWork);

	double g = atof(pChzFloat);

	SToken * pTok = PtokStart(TOKK_Literal, pWork);

	// BB (adrianb) Provide a way to specify explicit type here?

#if 0
	char chNext = char(tolower(Ch(pWork)));
	s64 cBit = 32;
	if (chNext == 'f')
	{
		AdvanceChar(pWork);
		cBit = NParseIntegerBase10(pWork);
		if (cBit != 32 && cBit != 64)
		{
			EndToken(pTok, pWork);
			ShowErr(pTok->errinfo, "Expected 32, or 64 for float literal suffix");
			return;
		}
	}
#endif
	
	pTok->lit.litk = LITK_Float;
	pTok->lit.g = g;

	EndToken(pTok, pWork);
}

void ParseToken(SWorkspace * pWork)
{
	if (FIsDone(pWork))
	{
		SToken * pTok = PtokStart(TOKK_EndOfFile, pWork);
		EndToken(pTok, pWork);
		return;
	}

	// Consume all white space

	while (!FIsDone(pWork))
	{
		// Eat the rest of the line if we're a comment
		
		if (pWork->pChzCurrent[0] == '/' && pWork->pChzCurrent[1] == '/')
		{
			while(!FIsDone(pWork) && Ch(pWork) != '\n')
				AdvanceChar(pWork);
		}

		if (pWork->pChzCurrent[0] == '/' && pWork->pChzCurrent[1] == '*')
		{
			AdvanceChar(pWork);
			int cComment = 1;
			while (!FIsDone(pWork))
			{
				if (pWork->pChzCurrent[0] == '/' && pWork->pChzCurrent[1] == '*')
				{
					AdvanceChar(pWork);
					AdvanceChar(pWork);
					cComment += 1;
					continue;
				}

				if (pWork->pChzCurrent[0] == '*' && pWork->pChzCurrent[1] == '/')
				{
					AdvanceChar(pWork);
					AdvanceChar(pWork);
					cComment -= 1;
					if (cComment == 0)
						break;
					continue;
				}

				AdvanceChar(pWork);
			}
		}

		char ch = Ch(pWork);

		if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ')
			AdvanceChar(pWork);
		else
			break;
	}

	// What token should we start consuming?

	// BB (adrianb) Change to switch statement or table drive ala http://nothings.org/computer/lexing.html? 

	char chStart = Ch(pWork);
	if (chStart == '\0')
	{
	}
	else if (chStart == '#' || chStart == '_' || FIsLetter(chStart))
	{
		SToken * pTok = PtokStart(TOKK_Identifier, pWork);
		
		const char * pChzStart = pWork->pChzCurrent;
		for (; FIsIdent(Ch(pWork)); AdvanceChar(pWork))
		{
		}

		// BB (adrianb) Reuse strings in original source?

		const char * pChzIdent = PchzCopy(pWork, pChzStart, pWork->pChzCurrent - pChzStart);

		const char * pChzOperator = PchzOperatorClean(pChzIdent);
		int nOpLevel = NOperatorLevel(pChzOperator);

		if (KEYWORD keyword = KeywordFromPchz(pChzIdent))
		{
			pTok->tokk = TOKK_Keyword;
			pTok->keyword = keyword;
		}
		else if (strcmp(pChzIdent, "false") == 0 || strcmp(pChzIdent, "true") == 0)
		{
			pTok->tokk = TOKK_Literal;
			pTok->lit.n = (strcmp(pChzIdent, "true") == 0);
			pTok->lit.litk = LITK_Bool;
		}
		else if (nOpLevel >= 0)
		{
			pTok->tokk = TOKK_Operator;
			pTok->op.pChz = pChzOperator;
			pTok->op.nLevel = nOpLevel;
		}
		else
		{
			pTok->ident.pChz = pChzIdent;
		}

		EndToken(pTok, pWork);
	}
	else if (FIsDigit(chStart))
	{
		// Check for floating point literal

		const char * pChz = pWork->pChzCurrent;
		for (; *pChz; ++pChz)
		{
			if (!FIsDigit(*pChz))
				break;
		}

		if (*pChz == '.' && !FIsOperator(pChz[1]))
		{
			TokenizeFloat(pWork);
		}
		else
		{
			TokenizeInt(pWork);
		}
	}
	else if (chStart == '"')
	{
		SToken * pTok = PtokStart(TOKK_Literal, pWork);
		AdvanceChar(pWork);

		// BB (adrianb) Allow 
		char aCh[1024];
		int cCh = 0;

		while (!FIsDone(pWork))
		{
			ASSERT(cCh < DIM(aCh));

			char ch = Ch(pWork);
			if (ch == '"')
				break;
			
			if (ch == '\\')
			{
				AdvanceChar(pWork);

				char chEscape = 0;
				switch (Ch(pWork))
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
				AdvanceChar(pWork);
				continue;
			}

			if (ch == '\n')
			{
				ShowErr(pWork->errinfo, "Unterminated string");
				break;				
			}

			aCh[cCh++] = ch;

			AdvanceChar(pWork);
		}

		pTok->lit.litk = LITK_String;
		pTok->lit.pChz = PchzCopy(pWork, aCh, cCh);
		AdvanceChar(pWork);

		EndToken(pTok, pWork);
	}
	else if (TOKK tokk = TokkCheckSimpleToken(pWork))
	{
		SToken * pTok = PtokStart(tokk, pWork);
		AdvanceChar(pWork);
		EndToken(pTok, pWork);
	}
	else if (FIsOperator(chStart))
	{
		SToken * pTok = PtokStart(TOKK_Operator, pWork);

		const char * pChzStart = pWork->pChzCurrent;
		for (; FIsOperator(Ch(pWork)); AdvanceChar(pWork))
		{
		}

		const char * pChzOp = PchzCopy(pWork, pChzStart, pWork->pChzCurrent - pChzStart);
		pTok->op.pChz = PchzOperatorClean(pChzOp);
		pTok->op.nLevel = NOperatorLevel(pTok->op.pChz);
		ASSERT(pTok->op.nLevel >= 0);

		EndToken(pTok, pWork);
	}
	else
	{
		ShowErr(pWork->errinfo, "Unrecognized character to start token %c", chStart);
	}
}

SToken TokPeek(SWorkspace * pWork, int iTokAhead = 0)
{
	pWork->cPeek += 1;
	ASSERT(pWork->cPeek < 100);

	while (iTokAhead >= pWork->aryTokNext.c)
	{
		ParseToken(pWork);
	}

	return pWork->aryTokNext[iTokAhead];
};

void ConsumeToken(SWorkspace * pWork, int c = 1)
{
	RemoveFront(&pWork->aryTokNext, c);
	pWork->cPeek = 0;
}

bool FTryConsumeToken(SWorkspace * pWork, TOKK tokk, SToken * pTok = nullptr)
{
	SToken tok = TokPeek(pWork);
	if (pTok)
		*pTok = tok;

	if (tok.tokk == tokk)
	{
		ConsumeToken(pWork);
		return true;
	}

	return false;
}

void ConsumeExpectedToken(SWorkspace * pWork, TOKK tokk, SToken * pTok = nullptr)
{
	SToken tok;
	if (!FTryConsumeToken(pWork, tokk, &tok))
	{
		ShowErr(tok.errinfo, "Expected %s found %s", PchzFromTokk(tokk), PchzFromTokk(tok.tokk));
	}

	if (pTok)
		*pTok = tok;
}

bool FTryConsumeLiteral(SWorkspace * pWork, LITK litk, SToken * pTok = nullptr)
{
	SToken tok = TokPeek(pWork);
	if (pTok)
		*pTok = tok;

	if (tok.tokk == TOKK_Literal && tok.lit.litk == litk)
	{
		ConsumeToken(pWork);
		return true;
	}

	return false;
}

void ConsumeExpectedLiteral(SWorkspace * pWork, LITK litk, SToken * pTok = nullptr)
{
	SToken tok = TokPeek(pWork);

	if (tok.tokk != TOKK_Literal)
	{
		ShowErr(tok.errinfo, "Expected %s literal found %s", PchzFromLitk(litk), PchzFromTokk(tok.tokk));
	}
	else if (tok.lit.litk != litk)
	{
		ShowErr(tok.errinfo, "Expected %s literal found %s", PchzFromLitk(litk), PchzFromLitk(tok.lit.litk));	
	}

	ConsumeToken(pWork);
	if (pTok)
		*pTok = tok;
}

bool FTryConsumeOperator(SWorkspace * pWork, const char * pChzOp, SToken * pTok = nullptr)
{
	SToken tok = TokPeek(pWork);
	if (pTok)
		*pTok = tok;

	if (tok.tokk == TOKK_Operator && strcmp(tok.op.pChz, pChzOp) == 0)
	{
		ConsumeToken(pWork);
		return true;
	}

	return false;
}

void ConsumeExpectedOperator(SWorkspace * pWork, const char * pChzOp)
{
	SToken tok;
	if (!FTryConsumeOperator(pWork, pChzOp, &tok))
	{
		ShowErr(tok.errinfo, "Expected operator %s", pChzOp);
	}
}

bool FTryConsumeKeyword(SWorkspace * pWork, KEYWORD keyword, SToken * pTok = nullptr)
{
	SToken tok = TokPeek(pWork);
	if (pTok)
		*pTok = tok;

	if (tok.tokk == TOKK_Keyword && tok.keyword == keyword)
	{
		ConsumeToken(pWork);
		return true;
	}

	return false;
}



#if 0 // in Compiler...
BLOCK     : 1,
LITERAL   : 2, // TODO literals morph into multiple types (e.g. small int, larger int, float).  Probably need an optional actual type in case we cast them.
IDENT     : 3,
PSTATEMENT : 4,

OPERATOR_EXPRESSION : 5,
// 6 is currently unused.
PROCEDURE_CALL  : 7,
ARRAY_SUBSCRIPT : 8,
WHILE     : 9,
IF        : 10,
LOOP_CONTROL : 11, ???
// 12 is currently unused.
REMOVE    : 13, looks like a way to mark that you've removed an entry in a for loop, ew :/
RETURN    : 14,
EACH      : 15, ... aka for blah

TYPE_DEFINITION : 16, #type
TYPE_INSTANTIATION : 17, ??? ...
ENUM : 18,

PROCEDURE : 19,
STRUCT : 20,
COMMA_SEPARATED_ARGUMENTS : 21, ???
EXTRACT : 22, ??? Is this the a,b,c := multifunc()?  With separate definitions for each?
SEQUENCE : 23, ??? a..b?

NEW_OR_DELETE : 24, ? Why is this one thing?

DECLARATION : 25, 

CAST_EXPRESSION : 26,
USING : 27, // for using an external global variable?
DIRECTIVE_INLINE : 28, // So we haven't performed any inlining yet?

DIRECTIVE_IMPORT : 29,
DIRECTIVE_LOAD : 30, auto-load dll?

DIRECTIVE_RUN : 31,
DIRECTIVE_CHECK_CALL : 32, Runs procedure with info from call? Used for printf, but who cares? Any other cases?
DIRECTIVE_ASSERT : 33, ??? Compile time assert?
DIRECTIVE_IF_DEFINED : 34, ??? Is there a way to define symbols from the command line or dynamically?  Or is this just so if you import something you can tell if its there?
DIRECTIVE_BAKE : 35, ???
DIRECTIVE_MODIFY : 36, For restricting 
DIRECTIVE_FOREIGN_LIBRARY : 37, For loading a dll with some functions from it

SIZE_OR_TYPE_INFO : 38, ??? 
CONTEXT_OR_PUSH   : 39, ??? Implicit context demo?

NOTE : 40, ??? annotations is some demo somewhere?
#endif

enum ASTK
{
	ASTK_Invalid = 0,

	// Basic building blocks

	ASTK_Literal,
	ASTK_Null,
	ASTK_UninitializedValue,
	
	// Constructs

	ASTK_Block,
	ASTK_EmptyStatement, // NOTE (adrianb) Allowing empty statements instead of wrapping all statements in a statement node
	ASTK_Identifier,
	ASTK_Operator,

	ASTK_If,
	ASTK_While,
	ASTK_For,
	ASTK_LoopControl,

	ASTK_Using,
	ASTK_Cast,
	ASTK_New,
	ASTK_Delete,
	ASTK_Remove, // yuck, use #remove instead? Not compile time...
	ASTK_Defer,
	ASTK_Inline,
	ASTK_PushContext, // BB (adrianb) unconvinced about this.  Is it thread safe?

	ASTK_ArrayIndex,
	ASTK_Call,
	ASTK_Return,

	// Declarations

	ASTK_DeclareSingle,
	ASTK_DeclareMulti,
	ASTK_AssignMulti,

	ASTK_Struct,
	ASTK_Enum,
	ASTK_Procedure,
	ASTK_TypeDefinition,

	// Type definitions

	ASTK_TypePointer,		// *
	ASTK_TypeArray,			// []
	ASTK_TypeProcedure,		// () or (arg) or (arg) -> ret or (arg, arg...) -> ret, ret... etc.
	ASTK_TypePolymorphic,	// $identifier
	ASTK_TypeVararg,		// ..

	// Directives

	ASTK_ImportDirective,
	ASTK_RunDirective,
	ASTK_ForeignLibraryDirective,
	// TODO a bunch of others?

	ASTK_Max,
};

const char * PchzFromAstk(ASTK astk)
{
	static const char * s_mpAstkPchz[] =
	{
		"Invalid",
		
		"Literal",
		"Null",
		"UninitializedValue",

		"Block",
		"EmptyStatement",
		"Indentifier",
		"Operator",

		"If",
		"While",
		"For",
		"LoopControl",

		"Using",
		"Cast",
		"New",
		"Delete",
		"Remove",
		"Defer",
		"Inline",
		"PushContext",

		"ArrayIndex",
		"Call",
		"Return",

		"DeclareSingle",
		"DeclareMulti",
		"AssignMulti",

		"Struct",
		"Enum",
		"Procedure",
		"TypeDefinition",

		"TypePointer",
		"TypeArray",
		"TypeProcedure",
		"TypePolymorphic",
		"TypeVararg",

		"ImportDirective",
		"RunDirective",
		"ForeignLibraryDirective",
	};

	CASSERT(DIM(s_mpAstkPchz) == ASTK_Max);
	if (astk < 0 || astk >= ASTK_Max)
		return "<invalid>";

	return s_mpAstkPchz[astk];
}

struct SAst
{
	ASTK astk;
	SErrorInfo errinfo;
	STypeId tid;	// Type resolved during typecheck time
};

// BB (adrianb) Could make this generic and shorter?
template <typename T>
T * PastCast(SAst * pAst)
{
	ASSERT(pAst->astk == T::s_astk);
	return static_cast<T *>(pAst);
}

template <typename T>
const T * PastCast(const SAst * pAst)
{
	ASSERT(pAst->astk == T::s_astk);
	return static_cast<const T *>(pAst);
}

struct SAstLiteral : public SAst
{
	static const ASTK s_astk = ASTK_Literal;
	SLiteral lit;
};

struct SAstBlock : public SAst
{
	static const ASTK s_astk = ASTK_Block;

	SArray<SAst *> arypAst;
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
	SAst * pAstLeft; // If nullptr, we're a prefix operator
	SAst * pAstRight;
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

struct SAstLoopControl : public SAst
{
	static const ASTK s_astk = ASTK_LoopControl;
	bool fContinue; // If not continue, then break
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
	bool fIsAuto; 		// Do simple conversions to other types as aggressively as C++
	SAst * pAstType;
	SAst * pAstExpr;
};

// BB (adrianb) Should we reduce the struct variety here?  E.g. all things with 1 expression get a simple ast node?

struct SAstNew : public SAst
{
	static const ASTK s_astk = ASTK_New;
	SAst * pAstType;
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

struct SAstInline : public SAst
{
	static const ASTK s_astk = ASTK_Inline;
	SAst * pAstExpr; // BB (adrianb) Only ever a procedure call?
	// BB (adrianb) Support noinline.
};

struct SAstPushContext : public SAst
{
	static const ASTK s_astk = ASTK_PushContext;
	const char * pChzContext;
	SAstBlock * pAstblock;
};

struct SAstDeclareSingle : public SAst
{
	static const ASTK s_astk = ASTK_DeclareSingle;
	const char * pChzName;
	SAst * pAstType;	// Optional explicit type (though one or more of these must be set)
	SAst * pAstValue;	// Optional initial value
	bool fUsing;
	bool fIsConstant;
};

struct SAstDeclareMulti : public SAst
{
	static const ASTK s_astk = ASTK_DeclareMulti;

	struct SName
	{
		const char * pChzName;
		SErrorInfo errinfo;
	};

	SArray<SName> aryName;
	SAst * pAstType; 	// Optional explicit type, probably should have either type or value
	SAst * pAstValue;	// Value to unpack
	bool fIsConstant;
};

struct SAstAssignMulti : public SAst
{
	static const ASTK s_astk = ASTK_AssignMulti;

	SArray<const char *> arypChzName;
	SAst * pAstValue;	// Value to unpack
};

struct SAstStruct : public SAst
{
	static const ASTK s_astk = ASTK_Struct;

	const char * pChzName;
	SArray<SAst *> arypAstDecl;
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
	SAst * pAstTypeInternal;
	SArray<SValue> aryValue;
};

struct SAstProcedure : public SAst
{
	static const ASTK s_astk = ASTK_Procedure;

	const char * pChzName;
	SArray<SAst *> arypAstDeclArg;
	SArray<SAst *> arypAstDeclRet;

	bool fIsInline;
	bool fIsForeign;
	const char * pChzForeign;

	SAstBlock * pAstblock;
};

struct SAstTypeDefinition : public SAst
{
	static const ASTK s_astk = ASTK_TypeDefinition;
	SAst * pAstType;
};

struct SAstTypePointer : public SAst
{
	static const ASTK s_astk = ASTK_TypePointer;

	SAst * pAstTypeInner;
	bool fSoa;
};

struct SAstTypeArray : public SAst
{
	static const ASTK s_astk = ASTK_TypeArray;

	SAst * pAstSize; 		// Int literal, identifier, etc // BB (adrianb) Extend to statically evaluatable expressions?
	bool fDynamicallySized;	// Size of ..
	bool fSoa;

	SAst * pAstTypeInner;
};

struct SAstTypeProcedure : public SAst
{
	static const ASTK s_astk = ASTK_TypeProcedure;

	SArray<SAst *> arypAstDeclArg;
	SArray<SAst *> arypAstDeclRet;
};

struct SAstTypePolymorphic : public SAst
{
	// BB (adrianb) Just merge this with SAstTypeIdentifier

	static const ASTK s_astk = ASTK_TypePolymorphic;

	const char * pChzName; // Just a name for now
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

struct SAstForeignLibraryDirective : public SAst
{
	static const ASTK s_astk = ASTK_ForeignLibraryDirective;
	const char * pChz;
};

void Destroy(SAst * pAst)
{
	// BB (adrianb) Can we avoid using arrays inside ast values?
	//  E.g. construct a stack array and then point to it in the ast?

	switch (pAst->astk)
	{
	case ASTK_Block: Destroy(&PastCast<SAstBlock>(pAst)->arypAst); break;
	case ASTK_Call: Destroy(&PastCast<SAstCall>(pAst)->arypAstArgs); break;
	case ASTK_Return: Destroy(&PastCast<SAstReturn>(pAst)->arypAstRet); break;
	case ASTK_DeclareMulti: Destroy(&PastCast<SAstDeclareMulti>(pAst)->aryName); break;
	case ASTK_AssignMulti: Destroy(&PastCast<SAstAssignMulti>(pAst)->arypChzName); break;
	case ASTK_Struct: Destroy(&PastCast<SAstStruct>(pAst)->arypAstDecl); break;
	case ASTK_Enum: Destroy(&PastCast<SAstEnum>(pAst)->aryValue); break;
	case ASTK_Procedure:
		Destroy(&PastCast<SAstProcedure>(pAst)->arypAstDeclArg);
		Destroy(&PastCast<SAstProcedure>(pAst)->arypAstDeclRet);
		break;
	case ASTK_TypeProcedure:
		Destroy(&PastCast<SAstTypeProcedure>(pAst)->arypAstDeclArg);
		Destroy(&PastCast<SAstTypeProcedure>(pAst)->arypAstDeclRet);
		break;
	default:
		break;
	}
}

template <typename T>
T * PastCreateManual(SWorkspace * pWork, ASTK astk, const SErrorInfo & errinfo)
{
	T * pAst = static_cast<T *>(PtAlloc<T>(&pWork->pagealloc));
	pAst->astk = astk;
	pAst->errinfo = errinfo;

	Append(&pWork->arypAstAll, pAst);
	return pAst;
}

template <typename T>
T * PastCreate(SWorkspace * pWork, const SErrorInfo & errinfo)
{
	return PastCreateManual<T>(pWork, T::s_astk, errinfo);
}

SAst * PastCreate(SWorkspace * pWork, const SToken & tok)
{
	switch (tok.tokk)
	{
	case TOKK_Identifier:
		{
			SAstIdentifier * pAstident = PastCreate<SAstIdentifier>(pWork, tok.errinfo);
			pAstident->pChz = tok.ident.pChz;
			return pAstident;
		}

	default:
		ASSERT(false);
		return nullptr;
	}
}

SAst * PastParseStatement(SWorkspace * pWork);
SAst * PastTryParseExpression(SWorkspace * pWork);
SAstBlock * PastParseBlock(SWorkspace * pWork);
SAstDeclareSingle * PastdeclParseOptionalName(SWorkspace * pWork);
SAst * PastParseType(SWorkspace * pWork);
SAst * PastTryParsePrimary(SWorkspace * pWork);

SAst * PastTryParseAtom(SWorkspace * pWork)
{
	SToken tok = TokPeek(pWork);

	switch (tok.tokk)
	{
		case TOKK_Identifier:
			{
				ConsumeToken(pWork);
				auto pAstident = PastCreate<SAstIdentifier>(pWork, tok.errinfo);
				pAstident->pChz = tok.ident.pChz;
				return pAstident;
			}

		case TOKK_Literal:
			{
				ConsumeToken(pWork);
				SAstLiteral * pAstlit = PastCreate<SAstLiteral>(pWork, tok.errinfo);
				pAstlit->lit = tok.lit;
				return pAstlit;
			}

		default:
			return nullptr;
	}
}

SAst * PastParseExpression(SWorkspace * pWork)
{
	SToken tok = TokPeek(pWork);
	SAst * pAst = PastTryParseExpression(pWork);
	if (!pAst)
	{
		ShowErr(tok.errinfo, "Expected expression");
	}

	return pAst;
}

SAst * PastParsePrimary(SWorkspace * pWork)
{
	SToken tok = TokPeek(pWork);
	SAst * pAst = PastTryParsePrimary(pWork);
	if (!pAst)
		ShowErr(tok.errinfo, "Expected non-operator expression");
	return pAst;
}

SAst * PastTryParseSimplePrimary(SWorkspace * pWork)
{
	SToken tok = {};

	if (FTryConsumeToken(pWork, TOKK_OpenParen))
	{
		// ( expression )

		SAst * pAst = PastParseExpression(pWork);
		ConsumeExpectedToken(pWork, TOKK_CloseParen);
		return pAst;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_Null, &tok))
	{
		return PastCreateManual<SAst>(pWork, ASTK_Null, tok.errinfo);
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_Cast, &tok) ||
			 FTryConsumeKeyword(pWork, KEYWORD_AutoCast, &tok))
	{
		auto pAstcast = PastCreate<SAstCast>(pWork, tok.errinfo);
		pAstcast->fIsAuto = (tok.keyword == KEYWORD_AutoCast);
		if (!pAstcast->fIsAuto)
		{
			ConsumeExpectedToken(pWork, TOKK_OpenParen);
			pAstcast->pAstType = PastParseType(pWork);
			ConsumeExpectedToken(pWork, TOKK_CloseParen);
		}
		
		pAstcast->pAstExpr = PastParsePrimary(pWork);

		return pAstcast;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_New, &tok))
	{
		auto pAstnew = PastCreate<SAstNew>(pWork, tok.errinfo);
		pAstnew->pAstType = PastParseType(pWork);
		return pAstnew;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_Delete, &tok))
	{
		auto pAstdelete = PastCreate<SAstDelete>(pWork, tok.errinfo);
		pAstdelete->pAstExpr = PastParsePrimary(pWork);
		return pAstdelete;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_Remove, &tok))
	{
		auto pAstremove = PastCreate<SAstRemove>(pWork, tok.errinfo);
		pAstremove->pAstExpr = PastParsePrimary(pWork);
		return pAstremove;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_CharDirective, &tok))
	{
		// BB (adrianb) Make a different type for char? Or explicit non-literal type?
		auto pAstlit = PastCreate<SAstLiteral>(pWork, tok.errinfo);
		
		ConsumeExpectedLiteral(pWork, LITK_String, &tok);
		pAstlit->lit.litk = LITK_Int;
		pAstlit->lit.n = tok.lit.pChz[0]; // BB (adrianb) Handle utf8 conversion to wide here?
		return pAstlit;
	}
	else
	{
		return PastTryParseAtom(pWork);
	}
}

SAst * PastTryParsePrimary(SWorkspace * pWork)
{
	SAst * pAstTop = nullptr;
	SAst ** ppAst = &pAstTop;

	// Parse any number of prefix operators

	SToken tok = {};
	while (FTryConsumeToken(pWork, TOKK_Operator, &tok))
	{
		auto pAstop = PastCreate<SAstOperator>(pWork, tok.errinfo);
		pAstop->pChzOp = tok.op.pChz;
		*ppAst = pAstop;
		ppAst = &pAstop->pAstRight;
	}

	*ppAst = PastTryParseSimplePrimary(pWork);
	
	if (!pAstTop)
		return nullptr;

	for (;;)
	{
		// Parse post operations: function call, array index, .ident

		if (FTryConsumeToken(pWork, TOKK_OpenParen, &tok))
		{
			// stuff(arg[, arg]*)

			// TODO inline, other?
			// BB (adrianb) Try to point the error info at something other than the openning paren?

			auto pAstcall = PastCreate<SAstCall>(pWork, tok.errinfo);
			pAstcall->pAstFunc = *ppAst;
			*ppAst = pAstcall; // Function call has higher precidence so take the last thing and call on that

			if (TokPeek(pWork).tokk != TOKK_CloseParen)
			{
				for (;;)
				{
					Append(&pAstcall->arypAstArgs, PastParseExpression(pWork));
					if (!FTryConsumeToken(pWork, TOKK_Comma))
						break;
				}
			}

			ConsumeExpectedToken(pWork, TOKK_CloseParen);
		}
		else if (FTryConsumeToken(pWork, TOKK_OpenBracket, &tok))
		{
			auto pAstarrayindex = PastCreate<SAstArrayIndex>(pWork, tok.errinfo);
			pAstarrayindex->pAstArray = *ppAst;
			*ppAst = pAstarrayindex;

			pAstarrayindex->pAstIndex = PastParseExpression(pWork);

			ConsumeExpectedToken(pWork, TOKK_CloseBracket);
		}
		else if (FTryConsumeOperator(pWork, ".", &tok))
		{
			SToken tokIdent;
			ConsumeExpectedToken(pWork, TOKK_Identifier, &tokIdent);

			auto pAstident = PastCreate<SAstIdentifier>(pWork, tokIdent.errinfo);
			pAstident->pChz = tokIdent.ident.pChz;

			auto pAstop = PastCreate<SAstOperator>(pWork, tok.errinfo);
			pAstop->pChzOp = tok.op.pChz;
			pAstop->pAstLeft = *ppAst;
			pAstop->pAstRight = pAstident;

			*ppAst = pAstop;
		}
		else
		{
			break;
		}
	}

	return pAstTop;
}

SAst * PastTryParseBinaryOperator(SWorkspace * pWork, int iOperator)
{
	SAst * apAst[32];
	int cpAst = 0;
	SToken aTokOperator[32];
	int cTokOperator = 0;

	// Push a primary on, always starts with one

	for (;;)
	{
		ASSERT(cpAst < DIM(apAst));
		apAst[cpAst++] = PastTryParsePrimary(pWork);

		// See if we should pop an old one

LTryOp:
		SToken tok = TokPeek(pWork);
        int nLevelCur = (cTokOperator > 0) ? aTokOperator[cTokOperator - 1].op.nLevel : -1;
		if (tok.tokk != TOKK_Operator || tok.op.nLevel <= nLevelCur)
		{
			if (cTokOperator > 0)
			{
				ASSERT(cpAst >= 2);
				SAst * pAstRight = apAst[--cpAst];
				SAst * pAstLeft = apAst[--cpAst];
				tok = aTokOperator[--cTokOperator];

				auto pAstop = PastCreate<SAstOperator>(pWork, tok.errinfo);
				pAstop->pChzOp = tok.op.pChz;
				pAstop->pAstLeft = pAstLeft;
				pAstop->pAstRight = pAstRight;
				apAst[cpAst++] = pAstop;

				goto LTryOp;
			}

			break;
		}
		else
		{
			ASSERT(cTokOperator < DIM(aTokOperator));
			ASSERTCHZ(tok.op.nLevel != INT_MAX, "Operator %s doesn't have precedence", tok.op.pChz);
            ConsumeToken(pWork, 1);
			aTokOperator[cTokOperator++] = tok;
		}
	}

	ASSERT(cpAst == 1);
	return apAst[0];
}

SAst * PastTryParseExpression(SWorkspace * pWork)
{
	SToken tok = {};
	if (FTryConsumeKeyword(pWork, KEYWORD_RunDirective, &tok))
	{
		auto pAstrun = PastCreate<SAstRunDirective>(pWork, tok.errinfo);
		SToken tokNext = TokPeek(pWork);
		if (tokNext.tokk == TOKK_OpenBrace)
		{
			pAstrun->pAstExpr = PastParseBlock(pWork);
		}
		else
		{
			pAstrun->pAstExpr = PastParseExpression(pWork);
		}
		return pAstrun;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_Inline, &tok))
	{
		auto pAstinline = PastCreate<SAstInline>(pWork, tok.errinfo);
		pAstinline->pAstExpr = PastParseExpression(pWork);
		return pAstinline;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_Continue, &tok) ||
			 FTryConsumeKeyword(pWork, KEYWORD_Break, &tok))
	{
		auto pAstloopctrl = PastCreate<SAstLoopControl>(pWork, tok.errinfo);
		pAstloopctrl->fContinue = (tok.keyword == KEYWORD_Continue);
		return pAstloopctrl;
	}
	else if (FTryConsumeOperator(pWork, "---", &tok))
	{
		// BB (adrianb) Make operators all manual so we don't have special case here?  
		//  Can't manually define operators then though.

		return PastCreateManual<SAst>(pWork, ASTK_UninitializedValue, tok.errinfo);
	}

	return PastTryParseBinaryOperator(pWork, 0);
}

void ParseReturnValues(SWorkspace * pWork, SArray<SAst *> * parypAstDecl)
{
	// Special casing void so we don't have to check for it later
	SToken tokVoid = TokPeek(pWork);
	if (tokVoid.tokk == TOKK_Identifier && strcmp(tokVoid.ident.pChz, "void") == 0)
	{
		ConsumeToken(pWork);
		return;
	}

	// BB (adrianb) Need to support:
	//  - named return values (doesn't appear to mean anything other than annotation, confusing)
	//  - grouped return values. i.e. (a:A,b:B)->(c:C,d:D). Does this mean you can't specify a return
	//    value that's a function?
	
	for (;;)
	{
		auto pAstdecl = PastdeclParseOptionalName(pWork);
		if (pAstdecl->pChzName != nullptr)
			ShowErr(pAstdecl->errinfo, "Return values should be type only.");
		Append(parypAstDecl, pAstdecl);
		if (!FTryConsumeToken(pWork, TOKK_Comma))
			break;
	}
}

SAst * PastParseType(SWorkspace * pWork)
{
	SToken tok = {};

	if (FTryConsumeOperator(pWork, "..", &tok))
	{
		return PastCreateManual<SAst>(pWork, ASTK_TypeVararg, tok.errinfo);
	}
	else if (FTryConsumeOperator(pWork, "$"))
	{
		// BB (adrianb) Can this have $T.S somehow?

		ConsumeExpectedToken(pWork, TOKK_Identifier, &tok);
		auto pAtypepoly = PastCreate<SAstTypePolymorphic>(pWork, tok.errinfo);
		pAtypepoly->pChzName = tok.ident.pChz;
		return pAtypepoly;
	}
	else if (FTryConsumeOperator(pWork, "*", &tok))
	{
		SAstTypePointer * pAtypepointer = PastCreate<SAstTypePointer>(pWork, tok.errinfo);

		SToken tokSoa = TokPeek(pWork);
		if (tokSoa.tokk == TOKK_Identifier && strcmp(tokSoa.ident.pChz, "SOA") == 0)
		{
			ConsumeToken(pWork);
			pAtypepointer->fSoa = true;
		}

		pAtypepointer->pAstTypeInner = PastParseType(pWork);
		return pAtypepointer;
	}
	else if (FTryConsumeToken(pWork, TOKK_OpenBracket, &tok))
	{
		// BB (adrianb) Would like it to span until close bracket?

		auto pAtypearray = PastCreate<SAstTypeArray>(pWork, tok.errinfo);

		if (TokPeek(pWork).tokk != TOKK_CloseBracket)
		{
			if (FTryConsumeOperator(pWork, ".."))
			{
				pAtypearray->fDynamicallySized = true;
			}
			else
			{
				pAtypearray->pAstSize = PastParseExpression(pWork);
			}
		}
		ConsumeExpectedToken(pWork, TOKK_CloseBracket);

		SToken tokSoa = TokPeek(pWork);
		if (tokSoa.tokk == TOKK_Identifier && strcmp(tokSoa.ident.pChz, "SOA") == 0)
		{
			ConsumeToken(pWork);
			pAtypearray->fSoa = true;
		}

		pAtypearray->pAstTypeInner = PastParseType(pWork);

		return pAtypearray;
	}
	else if (FTryConsumeToken(pWork, TOKK_OpenParen, &tok))
	{
		auto pAtypproc = PastCreate<SAstTypeProcedure>(pWork, tok.errinfo);

		for (;;)
		{
			Append(&pAtypproc->arypAstDeclArg, PastdeclParseOptionalName(pWork));
			if (!FTryConsumeToken(pWork, TOKK_Comma))
				break;
		}
		
		ConsumeExpectedToken(pWork, TOKK_CloseParen);

		if (FTryConsumeOperator(pWork, "->"))
		{
			ParseReturnValues(pWork, &pAtypproc->arypAstDeclRet);
		}

		return pAtypproc;
	}
	else if (FTryConsumeToken(pWork, TOKK_Identifier, &tok))
	{
		// BB (adrianb) Can you have something other than a.b.c once you hit an identifier?

		auto pAstident = PastCreate<SAstIdentifier>(pWork, tok.errinfo);
		pAstident->pChz = tok.ident.pChz;

		// If we have any '.'s, transform a.b.c into (. (. a b) c)

		SAst * pAstType = pAstident;

		while (FTryConsumeOperator(pWork, ".", &tok))
		{
			auto pAstop = PastCreate<SAstOperator>(pWork, tok.errinfo);

			pAstop->pChzOp = tok.op.pChz;
			pAstop->pAstLeft = pAstType;

			ConsumeExpectedToken(pWork, TOKK_Identifier, &tok);
			auto pAstidentRight = PastCreate<SAstIdentifier>(pWork, tok.errinfo);
			pAstidentRight->pChz = tok.ident.pChz;

			pAstop->pAstRight = pAstidentRight;
			pAstType = pAstop;
		}

		return pAstType;
	}
	else if (FTryConsumeOperator(pWork, "..", &tok))
	{
		return PastCreateManual<SAst>(pWork, ASTK_TypeVararg, tok.errinfo);
	}
	
	ShowErr(tok.errinfo, "Unexpected token for type declaration");
	return nullptr;
}

SAstDeclareSingle * PastdeclParseSimple(SWorkspace * pWork)
{
	// BB (adrianb) Makes no sense for structs, procs kinda if we pass context.

	SToken tokIdent = TokPeek(pWork);

	bool fUsing = false;
	if (tokIdent.tokk == TOKK_Keyword && tokIdent.keyword == KEYWORD_Using)
	{
		ConsumeToken(pWork);
		fUsing = true;
		tokIdent = TokPeek(pWork);
	}

	SAstDeclareSingle * pAstdecl = PastCreate<SAstDeclareSingle>(pWork, tokIdent.errinfo);

	SToken tokDefineOp = TokPeek(pWork, 1);

	if (tokIdent.tokk != TOKK_Identifier)
		ShowErr(tokIdent.errinfo, "Expected identifier at beginning of definition");

	if (tokDefineOp.tokk != TOKK_Operator)
		ShowErr(tokIdent.errinfo, "Expected : definition of some sort following identifier for definition");

	ConsumeToken(pWork, 2);

	pAstdecl->pChzName = tokIdent.ident.pChz;
	pAstdecl->fUsing = fUsing;

	const char * pChzColonOp = tokDefineOp.op.pChz;
	if (strcmp(pChzColonOp, ":") == 0)
	{
		// ident : type = value;
		// BB (adrianb) Can we have : = mean infer type too?

		pAstdecl->pAstType = PastParseType(pWork);

		if (FTryConsumeOperator(pWork, "="))
		{
			pAstdecl->pAstValue = PastParseExpression(pWork);
		}
		else if (FTryConsumeOperator(pWork, ":"))
		{
			// BB (adrianb) Does this syntax even make sense?

			pAstdecl->fIsConstant = true;
			pAstdecl->pAstValue = PastParseExpression(pWork);
		}
	}
	else if (strcmp(pChzColonOp, ":=") == 0 ||
			 strcmp(pChzColonOp, "::") == 0)
	{
		// ident := value

		pAstdecl->fIsConstant = strcmp(pChzColonOp, ":=") != 0;

		if (pAstdecl->fIsConstant && FTryConsumeKeyword(pWork, KEYWORD_TypeDirective))
		{
			// BB (adrianb) May need to wrap this in ast node to signify its actually a type.
			
			pAstdecl->pAstValue = PastParseType(pWork);
		}
		else
		{
			pAstdecl->pAstValue = PastParseExpression(pWork);
		}
	}
	else
	{
		ShowErr(tokDefineOp.errinfo, "Unknown define operator");
	}

	return pAstdecl;
}

SAstDeclareSingle * PastdeclParseOptionalName(SWorkspace * pWork)
{
	// If we don't have ident : just check for a type

	SToken tokIdent = TokPeek(pWork);
	SToken tokDefine = TokPeek(pWork, 1);

	if (tokIdent.tokk != TOKK_Identifier || tokDefine.tokk != TOKK_Operator)
	{
		auto pAstdecl = PastCreate<SAstDeclareSingle>(pWork, tokIdent.errinfo);
		pAstdecl->pAstType = PastParseType(pWork);
		return pAstdecl;
	}	

	return PastdeclParseSimple(pWork);
}

SAst * PastParseMultiDeclarationOrAssign(SWorkspace * pWork)
{
	SToken aTokIdent[16] = {};
	int cTokIdent = 0;

	for (;;)
	{
		ASSERT(cTokIdent < DIM(aTokIdent));
		ConsumeExpectedToken(pWork, TOKK_Identifier, &aTokIdent[cTokIdent++]);
		
		if (!FTryConsumeToken(pWork, TOKK_Comma))
			break;
	}

	SToken tok = TokPeek(pWork);
	bool fIsConstant = false;
	if (FTryConsumeOperator(pWork, ":", &tok) || 
		FTryConsumeOperator(pWork, "::", &tok) || 
		FTryConsumeOperator(pWork, ":=", &tok))
	{
		auto pAstdecmul = PastCreate<SAstDeclareMulti>(pWork, tok.errinfo); // BB (adrianb) Want the error info for all the identifiers?

		SAst * pAstType = nullptr;
		bool fHasType = (strcmp(tok.op.pChz, ":") == 0);
		if (fHasType)
		{
			pAstType = PastParseType(pWork);
		}

		fIsConstant = (strcmp(tok.op.pChz, "::") == 0);

		for (int iTok : IterCount(cTokIdent))
		{
			const SToken & tokIdent = aTokIdent[iTok];
			auto pName = PtAppendNew(&pAstdecmul->aryName);
			pName->pChzName = tokIdent.ident.pChz;
			pName->errinfo = tokIdent.errinfo;
		}

		if (!fHasType || FTryConsumeOperator(pWork, "="))
		{
			pAstdecmul->pAstValue = PastParseExpression(pWork);
		}

		return pAstdecmul;
	}
	else if (FTryConsumeOperator(pWork, "=", &tok))
	{
		auto pAstassignmul = PastCreate<SAstAssignMulti>(pWork, tok.errinfo);
		pAstassignmul->pAstValue = PastParseExpression(pWork);
		return pAstassignmul;
	}

	ShowErr(tok.errinfo, "Expected :: or := to do multiple declaration assignment");
	return nullptr;
}

SAst * PastTryParseDeclaration(SWorkspace * pWork)
{
	int iTok = 0;
	SToken tokIdent = TokPeek(pWork);
	if (tokIdent.tokk == TOKK_Keyword && tokIdent.keyword == KEYWORD_Using)
	{
		iTok = 1;
		tokIdent = TokPeek(pWork, 1);
	}

	if (tokIdent.tokk != TOKK_Identifier)
		return nullptr;
	
	SToken tokDefine = TokPeek(pWork, iTok + 1);
	if (tokDefine.tokk == TOKK_Comma)
	{
		// BB (adrianb) Could probably be multi assign too.

		return PastParseMultiDeclarationOrAssign(pWork);
	}
	else if (tokDefine.tokk == TOKK_Operator && *tokDefine.op.pChz == ':') // BB (adrianb) Check for exact set of operators?
	{
		return PastdeclParseSimple(pWork);
	}
	else
		return nullptr;
}

SAst * PastTryParseStructOrProcedureDeclaration(SWorkspace * pWork)
{
	// TODO support using a procedure?

	SToken tokIdent = TokPeek(pWork);
	SToken tokDefineOp = TokPeek(pWork, 1);

	if (tokIdent.tokk == TOKK_Identifier && tokDefineOp.tokk == TOKK_Operator)
	{
		if (strcmp(tokDefineOp.op.pChz, "::") == 0)
		{
			// BB (adrianb) Set these up as declarations too? Naming for procedures is special though in 
			//  that overloading is allowed.

			// ident :: declare-value
			
			// BB (adrianb) Determining if its a procedure seems messy.
			//  Couldn't we have a :: (5 + 3); or b := (5 + 3);  They wouldn't be easily distinguishable from
			//  a procedure definition without looking at the contents or for a block after the close paren.
			//  I suppose we could do that but ew.  Plus there's a #modify and other junk that can go in between.
			//  Requiring a keyword like struct would work but would be heavy.

			SToken tokValue = TokPeek(pWork, 2);
			bool fInline = (tokValue.tokk == TOKK_Keyword && tokValue.keyword == KEYWORD_Inline);
			if (tokValue.tokk == TOKK_OpenParen || (fInline  && TokPeek(pWork, 3).tokk == TOKK_OpenParen)) // procedure
			{
				ConsumeToken(pWork, 3);
				if (fInline)
					ConsumeExpectedToken(pWork, TOKK_OpenParen);

				auto pAstdecl = PastCreate<SAstDeclareSingle>(pWork, tokIdent.errinfo);

				auto pAstproc = PastCreate<SAstProcedure>(pWork, tokIdent.errinfo);
				pAstproc->pChzName = tokIdent.ident.pChz;
				pAstproc->fIsInline = fInline;

				pAstdecl->pChzName = pAstproc->pChzName;
				// BB (adrianb) Only need this if we aren't creating asts for all declarations
				pAstdecl->pAstValue = pAstproc;
				// BB (adrianb) Todo:
				// pAstdecl->fUsing
				pAstdecl->fIsConstant = true; // BB (adrianb) Support lambda case.

				if (TokPeek(pWork).tokk != TOKK_CloseParen)
				{
					for (;;)
					{
						Append(&pAstproc->arypAstDeclArg, PastdeclParseOptionalName(pWork));
						if (!FTryConsumeToken(pWork, TOKK_Comma))
							break;
					}
				}

				ConsumeExpectedToken(pWork, TOKK_CloseParen);

				if (FTryConsumeOperator(pWork, "->"))
				{
					ParseReturnValues(pWork, &pAstproc->arypAstDeclRet);
				}

				if (FTryConsumeKeyword(pWork, KEYWORD_ForeignDirective))
				{
					pAstproc->fIsForeign = true;

					SToken tokStr = {};
					if (FTryConsumeLiteral(pWork, LITK_String, &tokStr))
						pAstproc->pChzForeign = tokStr.lit.pChz;

					ConsumeExpectedToken(pWork, TOKK_Semicolon);
				}
				else
				{
					// BB (adrianb) Can we just have a statement follow or is there always a block?
					pAstproc->pAstblock = PastParseBlock(pWork);

					for (SAst * pAst : pAstproc->arypAstDeclArg)
					{
						auto pAstdecl = PastCast<SAstDeclareSingle>(pAst);
						if (pAstdecl->pChzName == nullptr)
						{
							ShowErr(pAstdecl->errinfo, "No name given to register argument");
						}
					}

					// Register for future consumption at the module level
				
					Append(&pWork->aryModule[pWork->iModuleParse].arypAstproc, pAstproc);
				}

				return pAstdecl;
			}
			else if (tokValue.tokk == TOKK_Keyword && tokValue.keyword == KEYWORD_Struct)
			{
				ConsumeToken(pWork, 3);

				auto pAstdecl = PastCreate<SAstDeclareSingle>(pWork, tokIdent.errinfo);
				auto pAststruct = PastCreate<SAstStruct>(pWork, tokIdent.errinfo);
				pAststruct->pChzName = tokIdent.ident.pChz;

				pAstdecl->pChzName = pAststruct->pChzName;
				pAstdecl->pAstValue = pAststruct;
				pAstdecl->fIsConstant = true;

				// TODO: AOS, SOA.

				ConsumeExpectedToken(pWork, TOKK_OpenBrace);

				while (!FTryConsumeToken(pWork, TOKK_CloseBrace))
				{
					SAst * pAst = PastTryParseStructOrProcedureDeclaration(pWork);
					if (!pAst)
					{
						pAst = PastTryParseDeclaration(pWork);
						if (pAst)
						{
							ConsumeExpectedToken(pWork, TOKK_Semicolon);
						}
						else
						{
							ShowErr(TokPeek(pWork).errinfo, "Expected declaration");
						}
					}
					
					Append(&pAststruct->arypAstDecl, pAst);
				}

				return pAstdecl;
			}
			else if (tokValue.tokk == TOKK_Keyword && tokValue.keyword == KEYWORD_Enum)
			{
				ConsumeToken(pWork, 3);

				auto pAstenum = PastCreate<SAstEnum>(pWork, tokIdent.errinfo);
				pAstenum->pChzName = tokIdent.ident.pChz;

				SToken tokType = TokPeek(pWork);
				if (tokType.tokk != TOKK_OpenBrace)
				{
					pAstenum->pAstTypeInternal = PastParseType(pWork);
				}

				ConsumeExpectedToken(pWork, TOKK_OpenBrace);

				while (TokPeek(pWork).tokk != TOKK_CloseBrace)
				{
					SToken tokIdent;
					ConsumeExpectedToken(pWork, TOKK_Identifier, &tokIdent);

					auto pValue = PtAppendNew(&pAstenum->aryValue);
					pValue->pChzName = tokIdent.ident.pChz;

					if (FTryConsumeOperator(pWork, ":"))
					{
						pValue->pAstValue = PastParseExpression(pWork);
					}

					if (!FTryConsumeToken(pWork, TOKK_Comma))
						break;
				}

				ConsumeExpectedToken(pWork, TOKK_CloseBrace);

				// BB (adrianb) Require a semicolon?

				return pAstenum;
			}
		}
	}

	return nullptr;
}

SAst * PastTryParseStatement(SWorkspace * pWork)
{
	SToken tok = TokPeek(pWork);
	
	if (tok.tokk == TOKK_OpenBrace)
	{
		return PastParseBlock(pWork);
	}

	// BB (adrianb) Should expressionify some of these.

	if (FTryConsumeKeyword(pWork, KEYWORD_If, &tok))
	{
		auto pAstif = PastCreate<SAstIf>(pWork, tok.errinfo);
		pAstif->pAstCondition = PastParseExpression(pWork);

		(void) FTryConsumeKeyword(pWork, KEYWORD_Then);

		pAstif->pAstPass = PastParseStatement(pWork);

		if (FTryConsumeKeyword(pWork, KEYWORD_Else, &tok))
		{
			pAstif->pAstElse = PastParseStatement(pWork);
		}

		return pAstif;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_While, &tok))
	{
		auto pAstwhile = PastCreate<SAstWhile>(pWork, tok.errinfo);
		pAstwhile->pAstCondition = PastParseExpression(pWork);
		pAstwhile->pAstLoop = PastParseStatement(pWork);
		return pAstwhile;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_For, &tok))
	{
		auto pAstfor = PastCreate<SAstFor>(pWork, tok.errinfo);

		// BB (adrianb) Parse *, any other for modifiers?

		SToken tokIdent = TokPeek(pWork);
		SToken tokColon = TokPeek(pWork, 1);
		if (tokIdent.tokk == TOKK_Identifier && FIsOperator(tokColon, ":"))
		{
			ConsumeToken(pWork, 2);
			pAstfor->pAstIter = PastCreate(pWork, tokIdent);
		}

		pAstfor->pAstIterRight = PastParseExpression(pWork);
		pAstfor->pAstLoop = PastParseStatement(pWork);

		return pAstfor;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_Return, &tok))
	{
		auto pAstret = PastCreate<SAstReturn>(pWork, tok.errinfo);

		SAst * pAstRet = PastTryParseExpression(pWork);
		if (pAstRet)
		{
			Append(&pAstret->arypAstRet, pAstRet);
			for (;;)
			{
				if (!FTryConsumeToken(pWork, TOKK_Comma))
					break;

				Append(&pAstret->arypAstRet, PastParseExpression(pWork));
			}
		}
		
		ConsumeExpectedToken(pWork, TOKK_Semicolon);

		return pAstret;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_Defer, &tok))
	{
		auto pAstdefer = PastCreate<SAstDefer>(pWork, tok.errinfo);
		pAstdefer->pAstStmt = PastParseStatement(pWork);
		return pAstdefer;
	}
	else if (FTryConsumeKeyword(pWork, KEYWORD_PushContext, &tok))
	{
		auto pAstpushctx = PastCreate<SAstPushContext>(pWork, tok.errinfo);
		ConsumeExpectedToken(pWork, TOKK_Identifier, &tok);
		pAstpushctx->pChzContext = tok.ident.pChz;
		pAstpushctx->pAstblock = PastParseBlock(pWork);
		return pAstpushctx;
	}

	// TODO Recongize definition with ident(, ident)* followed by a := too.

	// Recognize definition [using] ident[, ident]* (:: or :=) etc.
	// BB (adrianb) Maybe using and , should just be lower priority operators for the parser?

	SAst * pAst = PastTryParseStructOrProcedureDeclaration(pWork);
	if (pAst)
		return pAst; // No semicolon needed

	pAst = PastTryParseDeclaration(pWork);

	// Check for using expr (e.g. using Enum.members) after d
	if (!pAst && FTryConsumeKeyword(pWork, KEYWORD_Using, &tok))
	{
		auto pAstusing = PastCreate<SAstUsing>(pWork, tok.errinfo);
		pAstusing->pAstExpr = PastParseExpression(pWork);
		pAst = pAstusing;
	}

	if (!pAst)
		pAst = PastTryParseExpression(pWork);

	if (pAst)
	{
		ConsumeExpectedToken(pWork, TOKK_Semicolon);
		return pAst;
	}
	
	if (FTryConsumeToken(pWork, TOKK_Semicolon, &tok))
	{
		return PastCreateManual<SAst>(pWork, ASTK_EmptyStatement, tok.errinfo);
	}

	return nullptr;
}

SAst * PastParseStatement(SWorkspace * pWork)
{
	SToken tok = TokPeek(pWork);
	SAst * pAstStmt = PastTryParseStatement(pWork);
	if (!pAstStmt)
		ShowErr(tok.errinfo, "Expected statement");
	return pAstStmt;
}

SAstBlock * PastParseBlock(SWorkspace * pWork)
{
	SToken tokOpen;
	ConsumeExpectedToken(pWork, TOKK_OpenBrace, &tokOpen);

	SAstBlock * pAstblock = PastCreate<SAstBlock>(pWork, tokOpen.errinfo);

	for (;;)
	{
		SToken tok = TokPeek(pWork);
		if (tok.tokk == TOKK_CloseBrace)
			break;

		Append(&pAstblock->arypAst, PastParseStatement(pWork));
	}

	ConsumeExpectedToken(pWork, TOKK_CloseBrace);
	return pAstblock;
}

SAstBlock * PastblockParseRoot(SWorkspace * pWork)
{
	// BB (adrianb) Do we want a root scope?

	SToken tokScope = TokPeek(pWork);
	SAstBlock * pAstblock = PastCreate<SAstBlock>(pWork, tokScope.errinfo);

	for (;;)
	{
		SToken tok = {};
		if (FTryConsumeKeyword(pWork, KEYWORD_ImportDirective, &tok))
		{
			auto pAstimport = PastCreate<SAstImportDirective>(pWork, tokScope.errinfo);
			SToken tokString;
			ConsumeExpectedLiteral(pWork, LITK_String, &tokString);
			//ConsumeExpectedToken(pWork, TOKK_Semicolon);

			pAstimport->pChzImport = tokString.lit.pChz;
			
			// BB (adrianb) Verify this all happens on one line.
			
			Append(&pAstblock->arypAst, pAstimport);
			continue;
		}
		else if (FTryConsumeKeyword(pWork, KEYWORD_ForeignLibraryDirective, &tok))
		{
			auto pAstlib = PastCreate<SAstForeignLibraryDirective>(pWork, tokScope.errinfo);
			SToken tokString;
			ConsumeExpectedLiteral(pWork, LITK_String, &tokString);
			
			pAstlib->pChz = tokString.lit.pChz;
			
			// BB (adrianb) Verify this all happens on one line.
			
			Append(&pAstblock->arypAst, pAstlib);
			continue;
		}
		else if (TokPeek(pWork).tokk == TOKK_EndOfFile)
		{
			break;
		}

		SAst * pAst = PastTryParseStatement(pWork);
		if (!pAst && !FTryConsumeToken(pWork, TOKK_Semicolon))
			break;

		Append(&pAstblock->arypAst, pAst);
	}

	SToken tok = TokPeek(pWork);
	if (tok.tokk != TOKK_EndOfFile)
	{
		ShowErr(tok.errinfo, "Unexpected token %s", PchzFromTokk(tok.tokk));
	}
	else
	{
		ConsumeToken(pWork);
	}

	return pAstblock;
}

using PFNPRINT = void (*)(void * pVCtx, const char * pChzFmt, va_list varg);
struct SPrintFuncImpl
{
	PFNPRINT pfnprint;
	void * pVCtx;

	void operator()(const char * pChzFmt, ...) const
	{
		va_list varg;
		va_start(varg, pChzFmt);

		pfnprint(pVCtx, pChzFmt, varg);

		va_end(varg);
	}
};

template<class T>
void InitPrint(SPrintFuncImpl * pPrint, void (*pfnprint)(T *, const char *, va_list), T * pTCtx)
{
	ClearStruct(pPrint);
	pPrint->pfnprint = reinterpret_cast<PFNPRINT>(pfnprint);
	pPrint->pVCtx = pTCtx;
}

void PrintFile(FILE * pFile, const char * pChzFmt, va_list varg)
{
	vfprintf(pFile, pChzFmt, varg);
}

struct SAstCtx
{
	SPrintFuncImpl print;

	int cScope;
	bool fPrintedAnything;
	bool fPrintType;
};

void PrintEscapedString(const SPrintFuncImpl & print, const char * pChz)
{
	for (; *pChz; ++pChz)
	{
		switch (*pChz)
		{
		case '\n': print("\\n"); break;
		case '\t': print("\\t"); break;
		case '\v': print("\\v"); break;
		case '\r': print("\\r"); break;
		case '\f': print("\\f"); break;
		case '\a': print("\\a"); break;
		case '\\': print("\\\\"); break;
		case '"': print("\\\""); break;

		default:
			print("%c", *pChz);
			break;
		}
	}
}

void PrintSchemeType(SAstCtx * pAcx, const SType * pType);
inline void PrintSchemeType(SAstCtx * pAcx, STypeId tid)
{
	PrintSchemeType(pAcx, tid.pType);
}

void PrintSchemeAst(SAstCtx * pAcx, const SAst * pAst)
{
	if (!pAst)
		return;

	const SPrintFuncImpl & print = pAcx->print;

	// BB (adrianb) Some way to print newlines too?

	if (pAcx->fPrintedAnything)
	{
		print(" ");
	}
	else
	{
		pAcx->fPrintedAnything = true;
	}

	bool fPrintType = pAcx->fPrintType;
	bool fPrintAst = !pAcx->fPrintType;

	// Do all simple nodes with no parens

	if (fPrintAst)
	{
		switch (pAst->astk)
		{
		case ASTK_Literal:
			{
				const SLiteral & lit = PastCast<SAstLiteral>(pAst)->lit;
				switch (lit.litk)
				{
				case LITK_String: { print("\""); PrintEscapedString(print, lit.pChz); print("\""); } break;
				case LITK_Int: print("0x%llx", lit.n); break;
				case LITK_Float: print("%g", lit.g); break;
				case LITK_Bool: print("%s", (lit.n) ? "true" : "false"); break;
				default: ASSERT(false); break;
				}
			}
			break;

		case ASTK_Null:
			print("#null");
			break;

		case ASTK_UninitializedValue:
			print("---");
			break;

		case ASTK_Identifier:
			print("'%s", PastCast<SAstIdentifier>(pAst)->pChz);
			break;

		case ASTK_Operator:
			{
				auto pAstop = PastCast<SAstOperator>(pAst);
				print("(%s", pAstop->pChzOp);
				PrintSchemeAst(pAcx, pAstop->pAstLeft);
				PrintSchemeAst(pAcx, pAstop->pAstRight);
				print(")");
				return;
			}
			break;

		case ASTK_LoopControl:
			print("(%s)", PastCast<SAstLoopControl>(pAst)->fContinue ? "continue" : "break");
			break;

		case ASTK_TypePolymorphic:
			print("$%s", PastCast<SAstTypePolymorphic>(pAst)->pChzName);
			break;

		case ASTK_TypeVararg:
			print("..");
			break;

		case ASTK_ForeignLibraryDirective:
			print("#foreignlibrary");
			break;

		default:
			goto LNested;
		}
	}
	else
	{
		switch (pAst->astk)
		{
		case ASTK_Literal:
			{
				const SLiteral & lit = PastCast<SAstLiteral>(pAst)->lit;
				switch (lit.litk)
				{
				case LITK_String: print("StringLit"); break;
				case LITK_Int: print("IntLit"); break;
				case LITK_Float: print("FloatLit"); break;
				case LITK_Bool: print("BoolLit"); break;
				default: ASSERT(false); break;
				}
			}
			break;

		case ASTK_Null:
		case ASTK_UninitializedValue:
		case ASTK_Identifier:
		case ASTK_LoopControl:
		case ASTK_TypePolymorphic:
		case ASTK_TypeVararg:
		case ASTK_ForeignLibraryDirective:
			pAcx->fPrintedAnything = false;
			PrintSchemeType(pAcx, pAst->tid);
			break;

		case ASTK_Operator:
			{
				auto pAstop = PastCast<SAstOperator>(pAst);
				print("(%s", pAstop->pChzOp);
				PrintSchemeType(pAcx, pAstop->tid);
				PrintSchemeAst(pAcx, pAstop->pAstLeft);
				PrintSchemeAst(pAcx, pAstop->pAstRight);
				print(")");
				return;
			}
			break;

		default:
			goto LNested;
		}
	}

	return;

LNested:
	print("(");
	print("%s", PchzFromAstk(pAst->astk));
	if (fPrintType)
		PrintSchemeType(pAcx, pAst->tid);

	pAcx->cScope += 1;

	switch (pAst->astk)
	{
	case ASTK_Block:
		{
			auto pAstblock = PastCast<SAstBlock>(pAst);
			for (SAst * pAst : pAstblock->arypAst)
			{
				PrintSchemeAst(pAcx, pAst);
			}
		}
		break;

	case ASTK_EmptyStatement:
		break;

	case ASTK_If:
		{
			auto pAstif = PastCast<SAstIf>(pAst);
			PrintSchemeAst(pAcx, pAstif->pAstCondition);
			PrintSchemeAst(pAcx, pAstif->pAstPass);
			if (pAstif->pAstElse)
			{
				PrintSchemeAst(pAcx, pAstif->pAstElse);
			}
		}
		break;

	case ASTK_While:
		{
			auto pAstwhile = PastCast<SAstWhile>(pAst);
			PrintSchemeAst(pAcx, pAstwhile->pAstCondition);
			PrintSchemeAst(pAcx, pAstwhile->pAstLoop);
		}
		break;

	case ASTK_For:
		{
			auto pAstfor = PastCast<SAstFor>(pAst);
			if (pAstfor->pAstIter)
			{
				PrintSchemeAst(pAcx, pAstfor->pAstIter);
			}

			PrintSchemeAst(pAcx, pAstfor->pAstIterRight);
			PrintSchemeAst(pAcx, pAstfor->pAstLoop);
		}
		break;

	case ASTK_Using:
		{
			auto pAstusing = PastCast<SAstUsing>(pAst);
			PrintSchemeAst(pAcx, pAstusing->pAstExpr);
		}
		break;

	case ASTK_Cast:
		{
			auto pAstcast = PastCast<SAstCast>(pAst);
			if (pAstcast->fIsAuto)
				print(" auto"); // or new line bits?
			else if (pAstcast->pAstType == nullptr)
				print(" implicit");
			else
				PrintSchemeAst(pAcx, pAstcast->pAstType);

			PrintSchemeAst(pAcx, pAstcast->pAstExpr);
		}
		break;

	case ASTK_New:
		{
			auto pAstnew = PastCast<SAstNew>(pAst);
			PrintSchemeAst(pAcx, pAstnew->pAstType);
		}
		break;

	case ASTK_Delete:
		{
			PrintSchemeAst(pAcx, PastCast<SAstDelete>(pAst)->pAstExpr);
		}
		break;

	case ASTK_Remove:
		{
			PrintSchemeAst(pAcx, PastCast<SAstRemove>(pAst)->pAstExpr);
		}
		break;

	case ASTK_Defer:
		{
			PrintSchemeAst(pAcx, PastCast<SAstDefer>(pAst)->pAstStmt);
		}
		break;

	case ASTK_Inline:
		{
			PrintSchemeAst(pAcx, PastCast<SAstInline>(pAst)->pAstExpr);
		}
		break;

	case ASTK_PushContext:
		{
			auto pAstpushctx = PastCast<SAstPushContext>(pAst);
			if (fPrintAst)
				print(" %s", pAstpushctx->pChzContext);
			PrintSchemeAst(pAcx, pAstpushctx->pAstblock);
		}
		break;

	case ASTK_ArrayIndex:
		{
			auto pAstarrayindex = PastCast<SAstArrayIndex>(pAst);
			PrintSchemeAst(pAcx, pAstarrayindex->pAstArray);
			PrintSchemeAst(pAcx, pAstarrayindex->pAstIndex);
		}
		break;

	case ASTK_Call:
		{
			auto pAstcall = PastCast<SAstCall>(pAst);
			PrintSchemeAst(pAcx, pAstcall->pAstFunc);
			if (pAstcall->arypAstArgs.c)
			{
				for (auto pAst : pAstcall->arypAstArgs)
				{
					PrintSchemeAst(pAcx, pAst);
				}
			}
		}
		break;

	case ASTK_Return:
		{
			auto pAstreturn = PastCast<SAstReturn>(pAst);
			for (SAst * pAst : pAstreturn->arypAstRet)
			{
				PrintSchemeAst(pAcx, pAst);
			}
		}
		break;

	case ASTK_DeclareSingle:
		{
			auto pAstdecl = PastCast<SAstDeclareSingle>(pAst);
			if (fPrintAst)
			{
				print(" %s", pAstdecl->fIsConstant ? "const" : "var");
			
				if (pAstdecl->fUsing)
					print(" using");
				
				print(" %s", pAstdecl->pChzName ? pAstdecl->pChzName : "<no-name>");
			}

			if (pAstdecl->pAstType)
			{
				PrintSchemeAst(pAcx, pAstdecl->pAstType);
			}
			else
			{
				print(" infer-type");
			}

			// BB (adrianb) Print AST on a new line? if it's got any complexity that'd be ugly.
			if (pAstdecl->pAstValue)
				PrintSchemeAst(pAcx, pAstdecl->pAstValue);
		}
		break;

	case ASTK_DeclareMulti:
		{
			// BB (adrianb) Maybe once right side type is found replace with single declarations 
			//  followed by assign multi?

			auto pAstdecmul = PastCast<SAstDeclareMulti>(pAst);

			if (fPrintAst)
			{
				if (pAstdecmul->fIsConstant)
				{
					print(" constant");
				}

				print(" (names");
				for (const auto & name : pAstdecmul->aryName)
				{
					print(" %s", name.pChzName);
				}
				print(")");
			}

			// BB (adrianb) Newline optional here too?			
			if (pAstdecmul->pAstType)
			{
				PrintSchemeAst(pAcx, pAstdecmul->pAstType);
			}
			else
			{
				print(" infer-type");
			}
			
			if (pAstdecmul->pAstValue)
			{
				PrintSchemeAst(pAcx, pAstdecmul->pAstValue);
			}

			if (pAstdecmul->pAstValue)
				PrintSchemeAst(pAcx, pAstdecmul->pAstValue);
		}
		break;

	case ASTK_AssignMulti:
		{
			auto pAstassignmul = PastCast<SAstAssignMulti>(pAst);
			if (fPrintAst)
			{
				print(" (names");
				for (int ipChz = 0; ipChz < pAstassignmul->arypChzName.c; ++ipChz)
				{
					print(" %s", pAstassignmul->arypChzName[ipChz]);
				}
				print(")");
			}

			PrintSchemeAst(pAcx, pAstassignmul->pAstValue);
		}
		break;

	case ASTK_Struct:
		{
			auto pAststruct = PastCast<SAstStruct>(pAst);
			if (fPrintAst)
				print(" %s", pAststruct->pChzName);
			for (auto pAst : pAststruct->arypAstDecl)
			{
				PrintSchemeAst(pAcx, pAst);
			}
		}
		break;

	case ASTK_Enum:
		{
			auto pAstenum = PastCast<SAstEnum>(pAst);
			if (fPrintAst)
				print(" %s", pAstenum->pChzName);
			if (pAstenum->pAstTypeInternal)
			{
				PrintSchemeAst(pAcx, pAstenum->pAstTypeInternal);
			}
			
			if (fPrintAst)
			{
				for (auto value : pAstenum->aryValue)
				{
					print(" (%s", value.pChzName);
					if (value.pAstValue)
						PrintSchemeAst(pAcx, value.pAstValue);
					print(")");
				}
			}
		}
		break;

	case ASTK_Procedure:
		{
			auto pAstproc = PastCast<SAstProcedure>(pAst);
			if (pAstproc->fIsForeign && fPrintAst)
			{
				print(" (#foreign");
				if (pAstproc->pChzForeign)
				{
					print(" %s)", pAstproc->pChzForeign);
				}
				else
				{
					print(")");
				}
			}

			if (pAstproc->arypAstDeclArg.c)
			{
				print(" (args");
				for (auto pAst : pAstproc->arypAstDeclArg)
				{
					PrintSchemeAst(pAcx, pAst);
				}
				print(")");
			}

			if (pAstproc->arypAstDeclRet.c)
			{
				print(" (returns");
				for (auto pAst : pAstproc->arypAstDeclRet)
				{
					PrintSchemeAst(pAcx, PastCast<SAstDeclareSingle>(pAst)->pAstType);
				}
				print(")");
			}

			PrintSchemeAst(pAcx, pAstproc->pAstblock);
		}
		break;

	case ASTK_TypeProcedure:
		{
			auto pAtypeproc = PastCast<SAstTypeProcedure>(pAst);
			
			if (pAtypeproc->arypAstDeclArg.c)
			{
				print(" (args");
				for (auto pAst : pAtypeproc->arypAstDeclArg)
				{
					PrintSchemeAst(pAcx, pAst);
				}
				print(")");
			}

			if (pAtypeproc->arypAstDeclRet.c)
			{
				print(" (returns");
				for (auto pAst : pAtypeproc->arypAstDeclRet)
				{
					PrintSchemeAst(pAcx, PastCast<SAstDeclareSingle>(pAst)->pAstType);
				}
				print(")");
			}
		}
		break;

	case ASTK_TypePointer:
		{
			PrintSchemeAst(pAcx, PastCast<SAstTypePointer>(pAst)->pAstTypeInner);
		}
		break;

	case ASTK_TypeArray:
		{
			auto pAtypearray = PastCast<SAstTypeArray>(pAst);
			if (pAtypearray->fDynamicallySized)
			{
				print(" dynamic");
			}
			else
			{
				if (pAtypearray->pAstSize)
				{
					print(" (size ");
					PrintSchemeAst(pAcx, pAtypearray->pAstSize);
					print(")");
				}
			}

			PrintSchemeAst(pAcx, PastCast<SAstTypeArray>(pAst)->pAstTypeInner);
		}
		break;

	case ASTK_ImportDirective:
		if (fPrintAst)
			print(" \"%s\"", PastCast<SAstImportDirective>(pAst)->pChzImport);
		break;

	case ASTK_RunDirective:
		PrintSchemeAst(pAcx, PastCast<SAstRunDirective>(pAst)->pAstExpr);
		break;

	default:
		ASSERTCHZ(false, "Unhandled type %s (%d)", PchzFromAstk(pAst->astk), pAst->astk);
		break;
	}
	print(")");
}



void ParseAll(SWorkspace * pWork)
{
	for (pWork->iModuleParse = 0; pWork->iModuleParse < pWork->aryModule.c; ++pWork->iModuleParse)
	{
		SModule * pModule = &pWork->aryModule[pWork->iModuleParse];
		const char * pChzFile = pModule->pChzFile;
	
		if (pModule->pChzContents == nullptr)
		{
			pModule->fAllocContents = true;
			pModule->pChzContents = PchzLoadWholeFile(pChzFile);
		
			if (pModule->pChzContents == nullptr)
			{
				fflush(stdout);
				fprintf(stderr, "Could read file %s\n", pChzFile);
				fflush(stderr);
				exit(-1);
			}
		}
		
		StartParseNewFile(pWork, pChzFile, pModule->pChzContents);
		pModule->pAstblockRoot = PastblockParseRoot(pWork);

#if 0
		printf("\nParsed file %s\n", pChzFile);
		SPrintFunc<FILE> printstdout(PrintFile, stdout);
		PrintSchemeAst(pAcxstdout, pModule->pAstblockRoot);
		printf("\n");
#endif
		
		// Collect and add any imported files

		// NOTE (adrianb) Do not reference pModule after this point as it may have moved.

		auto pAstblock = pModule->pAstblockRoot;
		for (SAst * pAst : pAstblock->arypAst)
		{
			if (pAst->astk != ASTK_ImportDirective)
				continue;

			auto pAstimport = PastCast<SAstImportDirective>(pAst);
			AddModuleFile(pWork, pAstimport->pChzImport);
		}
	}
}



// Type checker needs to be able to suspend in the middle to support things like "func().b.c".
//  So if we flatten out in evaluation order then we should be able to deal with this sort of thing.
//  Also need to do constant flattening in order to support things like types with "[Struct.size] int".
//  Should we put semantics into the tree?  Or make another side structure?

enum TYPEK
{
	TYPEK_Void,
	TYPEK_Bool,
	TYPEK_S8,
	TYPEK_S16,
	TYPEK_S32,
	TYPEK_S64,
	TYPEK_U8,
	TYPEK_U16,
	TYPEK_U32,
	TYPEK_U64,
	TYPEK_Float,
	TYPEK_Double,
	TYPEK_Pointer,
	TYPEK_Procedure,
	TYPEK_Struct,
	TYPEK_String,
	TYPEK_Array,
	TYPEK_Any,
	TYPEK_Enum,
	TYPEK_TypeOf,
	TYPEK_Vararg,
	//TYPEK_PolymorphicVariable, 

	TYPEK_Max,

	TYPEK_IntMic = TYPEK_S8,
	TYPEK_IntMac = TYPEK_U64 + 1,
};

inline bool FIsInt(TYPEK typek)
{
	return typek >= TYPEK_IntMic && typek < TYPEK_IntMac;
}

inline bool FIsFloat(TYPEK typek)
{
	return typek >= TYPEK_Float && typek <= TYPEK_Double;
}

const char * PchzFromTypek(TYPEK typek)
{
	static const char * s_mpTypekPchz[] =
	{
		"void",
		"bool",
		"s8",
		"s16",
		"s32",
		"s64",
		"u8",
		"u16",
		"u32",
		"u64",
		"float",
		"double",
		"pointer",
		"procedure",
		"struct",
		"string",
		"array",
		"any",
		"enum",
		"typeof",
		"vararg",
	};
	CASSERT(DIM(s_mpTypekPchz) == TYPEK_Max);
	ASSERT(typek >= 0 && typek < TYPEK_Max);

	return s_mpTypekPchz[typek];
}

struct SType
{
	TYPEK typek;
	u32 cB;
	bool fSizeComputed;
	u16 cBAlign;
	// BB (adrianb) Story cB and cBAlign?
};

struct STypePointer : public SType
{
	static const TYPEK s_typek = TYPEK_Pointer;
	STypeId tidPointedTo;
	bool fSoa;				// BB (adrianb) Extend this to: -1 means no SOA. 0 means no size limit. >0 is AOSOA of that chunk size.
};

struct STypeProcedure : public SType
{
	static const TYPEK s_typek = TYPEK_Procedure;
	bool fUsesCVararg; // BB (adrianb) So we can't assign a foreign function using vararg to a non-foreign func pointer
	STypeId * aTidArg;
	STypeId * aTidRet;
	int cTidArg;
	int cTidRet;
};

struct STypeStruct : public SType
{
	static const TYPEK s_typek = TYPEK_Struct;
	// BB (adrianb) Don't necessarily need names for full struct equality, but in order to 
	//  resolve func().b.c you need the TYPE of func() to tell you how to resolve .b.

	struct SMember
	{
		SAstDeclareSingle * pAstdecl; // BB (adrianb) A little strange to point to AST in type.

		int iBOffset;
		// TODO annotations
	};

	const char * pChzName; // BB (adrianb) Not required for format equality?
	SMember * aMember;	// BB (adrianb) Filled in after type entered in name slot?
	int cMember;
	int cMemberMax;
};

struct STypeArray : public SType
{
	static const TYPEK s_typek = TYPEK_Array;
	bool fDynamicallySized;	// Size of ..
	s64 cSizeFixed;			// If >= 0 fixed size BB (adrianb) Is this enough to cover static/fixed?

	bool fSoa;				// BB (adrianb) Extend this to: -1 means no SOA. 0 means no size limit. >0 is AOSOA of that chunk size.

	STypeId tidElement;

	STypeStruct * pTypestruct; // Struct representation of this thing (null if fixed size)
};

struct STypeString : public SType
{
	static const TYPEK s_typek = TYPEK_String;
	
	STypeStruct * pTypestruct;
};

struct STypeEnum : public SType
{
	static const TYPEK s_typek = TYPEK_Enum;
	STypeId tidInternal;
	STypeId tidStructForEnum;
};

struct STypeTypeOf : public SType
{
	static const TYPEK s_typek = TYPEK_TypeOf;
	STypeId tid;
};

template <class T>
T * PtypeCast(SType * pType)
{
	ASSERTCHZ(T::s_typek == pType->typek, "Failed to cast type %s to %s", PchzFromTypek(pType->typek), 
			  PchzFromTypek(T::s_typek));
	return static_cast<T *>(pType);
}

template <class T>
const T * PtypeCast(const SType * pType)
{
	ASSERTCHZ(T::s_typek == pType->typek, "Failed to cast type %s to %s", PchzFromTypek(pType->typek), 
			  PchzFromTypek(T::s_typek));
	return static_cast<const T *>(pType);
}

bool FHasStruct(SType * pType)
{
	auto typek = pType->typek;
	return (typek == TYPEK_Struct || typek == TYPEK_String || typek == TYPEK_Array);
}

const STypeStruct * Ptypestruct(const SType * pType)
{
	switch (pType->typek)
	{
	case TYPEK_String: return PtypeCast<STypeString>(pType)->pTypestruct;
	case TYPEK_Array: return PtypeCast<STypeArray>(pType)->pTypestruct;
	default:
		return PtypeCast<STypeStruct>(pType);
	}
}

inline u32 HvFromKey(u64 n)
{
	return u32(n) ^ u32(n >> 32);
}

inline u32 HvFromKey(const STypeId & tid)
{
	return HvFromKey(reinterpret_cast<intptr_t>(tid.pType));
}

inline bool FIsKeyEqual(STypeId tid0, STypeId tid1)
{
	return tid0.pType == tid1.pType;
}

bool FIsBuiltinType(TYPEK typek)
{
	switch (typek)
	{
	case TYPEK_Void:
	case TYPEK_Bool:
	case TYPEK_String:
	case TYPEK_Any:
	case TYPEK_S8:
	case TYPEK_S16:
	case TYPEK_S32:
	case TYPEK_S64:
	case TYPEK_U8:
	case TYPEK_U16:
	case TYPEK_U32:
	case TYPEK_U64:
	case TYPEK_Float:
	case TYPEK_Double:
	case TYPEK_Vararg:
		return true;

	default:
		return false;
	}
}

bool FSigned(TYPEK typek)
{
	switch (typek)
	{
	case TYPEK_S8:
	case TYPEK_S16:
	case TYPEK_S32:
	case TYPEK_S64:
		return true;

	case TYPEK_U8:
	case TYPEK_U16:
	case TYPEK_U32:
	case TYPEK_U64:
		return false;

	default:
		ASSERT(false);
		return false;
	}
}

int CBit(TYPEK typek)
{
	switch (typek)
	{
	case TYPEK_S8:
	case TYPEK_U8:
		return 8;

	case TYPEK_S16:
	case TYPEK_U16:
		return 16;

	case TYPEK_S32:
	case TYPEK_U32:
		return 32;

	case TYPEK_S64:
	case TYPEK_U64:
		return 64;

	case TYPEK_Float:
		return 32;

	case TYPEK_Double:
		return 64;

	default:
		ASSERT(false);
		return 0;
	}
}

struct SStringBuilder
{
	SStringBuilder() : aChz(nullptr), cCh(0), cChMax(0) {}
	explicit SStringBuilder(const char * pChzFmt, ...);
	~SStringBuilder() { delete [] aChz; }

	char * aChz;
	int cCh;
	int cChMax;
};

void PrintV(SStringBuilder * pStrb, const char * pChzFmt, va_list vargs)
{
	auto vargCopy = *vargs;
	int cCh = vsnprintf(pStrb->aChz + pStrb->cCh, pStrb->cChMax - pStrb->cCh, pChzFmt, vargs);
	
	int cChNeeded = pStrb->cCh + cCh + 1;
	if (cChNeeded >= pStrb->cChMax)
	{
		*vargs = vargCopy;
		int cChAlloc = pStrb->cChMax;
		if (cChAlloc == 0)
			cChAlloc = 32;

		while (cChAlloc < cChNeeded)
			cChAlloc *= 2;

		char * aChzNew = static_cast<char *>(realloc(pStrb->aChz, cChAlloc));
		ASSERT(aChzNew);
		pStrb->cChMax = cChAlloc;
		pStrb->aChz = aChzNew;

		int cCh2 = vsnprintf(pStrb->aChz + pStrb->cCh, pStrb->cChMax - pStrb->cCh, pChzFmt, vargs);
		VERIFY(cCh2 == cCh);
		ASSERT(pStrb->cCh + cCh + 1 <= pStrb->cChMax);
	}

	pStrb->cCh += cCh;
}

void Print(SStringBuilder * pStrb, const char * pChzFmt, ...)
{
	va_list vargs;
	va_start(vargs, pChzFmt);

	PrintV(pStrb, pChzFmt, vargs);

	va_end(vargs);
}

SStringBuilder::SStringBuilder(const char * pChzFmt, ...)
: SStringBuilder()
{
	va_list vargs;
	va_start(vargs, pChzFmt);

	PrintV(this, pChzFmt, vargs);

	va_end(vargs);
}

const char * AchzSteal(SStringBuilder * pStrb)
{
	const char * aChz = pStrb->aChz;
	ClearStruct(pStrb);
	return aChz;
}

const char * PchzBaseName(const char * pChzIn)
{
	const char * pChzRet = pChzIn;

	for (const char * pChz = pChzIn; *pChz; ++pChz)
	{
		if (*pChz == '\\' || *pChz == '/')
		{
			pChzRet = pChz + 1;
		}
	}

	return pChzRet;
}

void PatchExt(SStringBuilder * pStrb, const char * pChzExt)
{
	if (char * pChDot = strchr(pStrb->aChz, '.'))
	{
		pStrb->cCh -= strlen(pChDot);
		*pChDot = 0;
	}
	
	Print(pStrb, "%s", pChzExt);
}

void PrintFriendlyTypeRecursive(const SType * pType, SStringBuilder * pStrb)
{
	if (!pType)
	{
		Print(pStrb, "<no-type>");
		return;
	}

	TYPEK typek = pType->typek;
	if (FIsBuiltinType(typek))
	{
		Print(pStrb, "%s", PchzFromTypek(typek));
		return;
	}

	switch (typek)
	{
	case TYPEK_Pointer:
		{
			auto pTypeptr = PtypeCast<STypePointer>(pType);
			Print(pStrb, "* %s", (pTypeptr->fSoa) ? "SOA ": "");
			PrintFriendlyTypeRecursive(pTypeptr->tidPointedTo.pType, pStrb);
			return;
		}

	case TYPEK_Array:
		{
			auto pTypearray = PtypeCast<STypeArray>(pType);

			if (pTypearray->fDynamicallySized)
				Print(pStrb, "[..] ");
			else if (pTypearray->cSizeFixed >= 0)
				Print(pStrb, "[%d] ", pTypearray->cSizeFixed);
			else
				Print(pStrb, "[] ");

			if (pTypearray->fSoa)
				Print(pStrb, "SOA ");

			PrintFriendlyTypeRecursive(pTypearray->tidElement.pType, pStrb);
			return;
		}

	case TYPEK_Procedure:
		{
			auto pTypeproc = PtypeCast<STypeProcedure>(pType);

			Print(pStrb, "(");
			for (int iTid : IterCount(pTypeproc->cTidArg))
			{
				if (iTid > 0)
					Print(pStrb, ", ");
				PrintFriendlyTypeRecursive(pTypeproc->aTidArg[iTid].pType, pStrb);
			}
			Print(pStrb, ")");

			if (pTypeproc->cTidRet > 0)
			{
				Print(pStrb, " -> ");
				for (int iTid : IterCount(pTypeproc->cTidRet))
				{
					if (iTid > 0)
						Print(pStrb, ", ");
					PrintFriendlyTypeRecursive(pTypeproc->aTidRet[iTid].pType, pStrb);
				}
			}
			return;
		}

	case TYPEK_Struct:
		{
			Print(pStrb, "%s", PtypeCast<STypeStruct>(pType)->pChzName);
			return;
		}

	case TYPEK_Enum:
		{
			PrintFriendlyTypeRecursive(PtypeCast<STypeEnum>(pType)->tidStructForEnum.pType, pStrb);
			return;
		}

	case TYPEK_TypeOf:
		{
			Print(pStrb, "#type ");
			PrintFriendlyTypeRecursive(PtypeCast<STypeTypeOf>(pType)->tid.pType, pStrb);
			return;
		}

	default:
		ASSERTCHZ(false, "Can't print type %s(%d)", PchzFromTypek(typek), typek);
		break;
	}
}

struct SStringTemp
{
	explicit SStringTemp(SStringBuilder * pStrb)
	{
		pChz = pStrb->aChz;
		ClearStruct(pStrb);
	}

	~SStringTemp() { delete [] pChz; }
	const char * Pchz() { return pChz; }

	char * pChz;
};

SStringTemp StrPrintType(const SType * pType)
{
	SStringBuilder strb;
	PrintFriendlyTypeRecursive(pType, &strb);

	return SStringTemp(&strb);
}

SStringTemp StrPrintType(STypeId tid)
{
	SStringBuilder strb;
	PrintFriendlyTypeRecursive(tid.pType, &strb);

	return SStringTemp(&strb);
}

void PrintSchemeType(SAstCtx * pAcx, const SType * pType)
{
	const SPrintFuncImpl & print = pAcx->print;

	if (pAcx->fPrintedAnything)
	{
		print(" ");
	}
	else
	{
		pAcx->fPrintedAnything = true;
	}

	if (!pType)
	{
		print("<no-type>");
		return;
	}

	switch (pType->typek)
	{
	case TYPEK_Void:
	case TYPEK_Bool:
	case TYPEK_String:
		print("%s", PchzFromTypek(pType->typek));
		break;

	case TYPEK_S8:
	case TYPEK_S16:
	case TYPEK_S32:
	case TYPEK_S64:
	case TYPEK_U8:
	case TYPEK_U16:
	case TYPEK_U32:
	case TYPEK_U64:
		{
			print("%s%d", (FSigned(pType->typek)) ? "s" : "u", CBit(pType->typek));
		}
		break;

	case TYPEK_Float:
	case TYPEK_Double:
		{
			print("f%d", CBit(pType->typek));
		}
		break;

	case TYPEK_Struct:
		{
			auto pTypestruct = static_cast<const STypeStruct *>(pType);
			print("%s", pTypestruct->pChzName);
			// BB (adrianb) Print member info?
		}
		break;

	case TYPEK_Any:
		{
			print("Any");
		}
		break;

	case TYPEK_Enum:
		{
			// BB (adrianb) Is this the right place to get the name of the enum?

			auto pTypeenum = static_cast<const STypeEnum *>(pType);
			print("%s", static_cast<const STypeStruct *>(pTypeenum->tidStructForEnum.pType)->pChzName);
		}
		break;

	case TYPEK_Vararg:
		{
			print("..");
		}
		break;

	default:
		goto LNested;
	}

	return;

LNested:

	print("(");

	TYPEK typek = pType->typek;
	switch (typek)
	{
	case TYPEK_Pointer:
		{
			auto pTypepointer = static_cast<const STypePointer *>(pType);
			print("*%s", (pTypepointer->fSoa) ? " SOA" : "");
			PrintSchemeType(pAcx, pTypepointer->tidPointedTo);
		}
		break;

	case TYPEK_Array:
		{
			auto pTypearray = static_cast<const STypeArray *>(pType);
			if (pTypearray->fDynamicallySized)
			{
				print("[..]");
			}
			else if (pTypearray->cSizeFixed >= 0)
			{
				print("[%lld]", pTypearray->cSizeFixed);
			}
			else
			{
				print("[]");
			}

			if (pTypearray->fSoa)
				print(" SOA");

			PrintSchemeType(pAcx, pTypearray->tidElement);
		}
		break;

	case TYPEK_Procedure:
		{
			auto pTypeproc = PtypeCast<STypeProcedure>(pType);
			print("Proc");
			for (int iTid = 0; iTid < pTypeproc->cTidArg; ++iTid)
			{
				PrintSchemeType(pAcx, pTypeproc->aTidArg[iTid]);
			}
			
			if (pTypeproc->cTidRet)
			{
				print(" ->");
				for (int iTid = 0; iTid < pTypeproc->cTidRet; ++iTid)
				{
					PrintSchemeType(pAcx, pTypeproc->aTidRet[iTid]);
				}
			}
		}
		break;

	case TYPEK_TypeOf:
		{
			// Distinguish type and not type?
			print("Type");
			PrintSchemeType(pAcx, PtypeCast<STypeTypeOf>(pType)->tid);
		}
		break;

	default:
		ASSERTCHZ(false, "Can't print type %s(%d)", PchzFromTypek(typek), typek);
		break;
	}

	print(")");
}

bool FAreSameType(const SType * pType0, const SType * pType1)
{
	if (pType0->typek != pType1->typek)
		return false;

	if (FIsBuiltinType(pType0->typek))
		return true;

	switch (pType0->typek)
	{
	case TYPEK_Pointer:
		{
			auto pTypepointer0 = static_cast<const STypePointer *>(pType0);
			auto pTypepointer1 = static_cast<const STypePointer *>(pType1);
			return pTypepointer0->fSoa == pTypepointer1->fSoa &&
					pTypepointer0->tidPointedTo == pTypepointer1->tidPointedTo;
		}
		break;

	case TYPEK_Array:
		{
			auto pTypearray0 = static_cast<const STypeArray *>(pType0);
			auto pTypearray1 = static_cast<const STypeArray *>(pType1);

			return pTypearray0->fDynamicallySized == pTypearray1->fDynamicallySized &&
					pTypearray0->cSizeFixed == pTypearray1->cSizeFixed &&
					pTypearray0->fSoa == pTypearray1->fSoa &&
					pTypearray0->tidElement == pTypearray1->tidElement;
		}
		break;

	case TYPEK_Procedure:
		{
			auto pTypeproc0 = static_cast<const STypeProcedure *>(pType0);
			auto pTypeproc1 = static_cast<const STypeProcedure *>(pType1);
			
			if (pTypeproc0->fUsesCVararg != pTypeproc1->fUsesCVararg)
				return false;

			if (pTypeproc0->cTidArg != pTypeproc1->cTidArg)
				return false;

			for (int ipType = 0; ipType < pTypeproc0->cTidArg; ++ipType)
			{
				if (pTypeproc0->aTidArg[ipType] != pTypeproc1->aTidArg[ipType])
					return false;
			}

			if (pTypeproc0->cTidRet != pTypeproc1->cTidRet)
				return false;

			for (int ipType = 0; ipType < pTypeproc0->cTidRet; ++ipType)
			{
				if (pTypeproc0->aTidRet[ipType] != pTypeproc1->aTidRet[ipType])
					return false;
			}

			return true;
		}

	case TYPEK_Struct:
		{
			auto pTypestruct0 = static_cast<const STypeStruct *>(pType0);
			auto pTypestruct1 = static_cast<const STypeStruct *>(pType1);
			
			// BB (adrianb) Not required? Or should this be the ONLY thing required?
			//  Current only because type is registered before it's finished processing
			//  so that we can have recursion on pointers.

			if (strcmp(pTypestruct0->pChzName, pTypestruct1->pChzName) != 0) 
				return false;

#if 0
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
#endif

			return true;
		}

	case TYPEK_Enum:
		{
			auto pTypeenum0 = static_cast<const STypeEnum *>(pType0);
			auto pTypeenum1 = static_cast<const STypeEnum *>(pType1);
			
			if (pTypeenum0->tidInternal != pTypeenum1->tidInternal)
				return false;

			if (pTypeenum0->tidStructForEnum != pTypeenum1->tidStructForEnum)
				return false;

			return true;
		}

	case TYPEK_TypeOf:
		{
			auto pTypeof0 = static_cast<const STypeTypeOf *>(pType0);
			auto pTypeof1 = static_cast<const STypeTypeOf *>(pType1);

			if (pTypeof0->tid != pTypeof1->tid)
				return false;
			
			return true;
		}

	default:
		ASSERTCHZ(false, "Missing clause for typek %d", pType0->typek);
		return false;
	}
}

bool FIsKeyEqual(const STypeId & tid, const SType & type)
{
	return FAreSameType(tid.pType, &type);
}

template <class T>
T * PtClone(SPagedAlloc * pPagealloc, const T * aTIn, int cT = 1)
{
	T * aT = PtAlloc<T>(pPagealloc, cT);
	memcpy(aT, aTIn, cT * sizeof(T));
	return aT;
}

// BB (adrianb) Use 64 bit hash function?

u32 HvFromType(const SType & type)
{
	u32 hv = type.typek;

	// BB (adrianb) If internal types are already unique we could just hash their pointers (inconsistent hash order across runs).
	// BB (adrianb) Better hash mechanism?

	if (FIsBuiltinType(type.typek))
		return hv;

	switch (type.typek)
	{
	case TYPEK_Pointer:
		{
			auto pTypepointer = static_cast<const STypePointer *>(&type);
			hv = hv * 7901 + pTypepointer->fSoa;
			hv = hv * 7901 + HvFromKey(pTypepointer->tidPointedTo);
		}
		break;

	case TYPEK_Array:
		{
			auto pTypearray = static_cast<const STypeArray *>(&type);
			hv = hv * 7901 + pTypearray->fDynamicallySized;
			hv = hv * 7901 + pTypearray->cSizeFixed;
			hv = hv * 7901 + pTypearray->fSoa;
			hv = hv * 7901 + HvFromKey(pTypearray->tidElement);
		}
		break;

	case TYPEK_Procedure:
		{
			auto pTypeproc = static_cast<const STypeProcedure *>(&type);
			
			hv = hv * 7901 + pTypeproc->fUsesCVararg;
			hv = hv * 7901 + pTypeproc->cTidArg;

			for (int iTid = 0; iTid < pTypeproc->cTidArg; ++iTid)
			{
				hv = hv * 7901 + HvFromKey(pTypeproc->aTidArg[iTid]);
			}

			hv = hv * 7901 + pTypeproc->cTidRet;

			for (int iTid = 0; iTid < pTypeproc->cTidRet; ++iTid)
			{
				hv = hv * 7901 + HvFromKey(pTypeproc->aTidRet[iTid]);
			}
		}
		break;

	case TYPEK_Struct:
		{
			auto pTypestruct = static_cast<const STypeStruct *>(&type);
			
			// BB (adrianb) Not required? Or should this be the ONLY thing required?
			hv = hv * 7901 + HvFromKey(pTypestruct->pChzName, INT_MAX);
			#if 0
			hv = hv * 7901 + pTypestruct->cMember;

			for (int iMember = 0; iMember < pTypestruct->cMember; ++iMember)
			{
				auto pMember = &pTypestruct->aMember[iMember];

				hv = hv * 7901 + HvFromKey(pMember->pChzName, INT_MAX); // BB (adrianb) Not required?
				hv = hv * 7901 + HvFromKey(pMember->tid);

				// BB (adrianb) Skipping iBOffset and errinfo.

				hv = hv * 7901 + pMember->fIsConstant;
				hv = hv * 7901 + pMember->fIsImported;
				hv = hv * 7901 + pMember->fIsUsing;
			}
			#endif
		}
		break;

	case TYPEK_Enum:
		{
			auto pTypeenum = static_cast<const STypeEnum *>(&type);
			
			hv = hv * 7901 + HvFromKey(pTypeenum->tidInternal);
			hv = hv * 7901 + HvFromKey(pTypeenum->tidStructForEnum);
		}
		break;

	case TYPEK_TypeOf:
		{
			hv = hv * 7901 + HvFromKey(static_cast<const STypeTypeOf *>(&type)->tid);
		}
		break;

	default:
		ASSERTCHZ(false, "Unknown type %s(%d)", PchzFromTypek(type.typek), type.typek);
		break;
	}

	return hv;
}



// Recurse on AST, resolve declarations/identifiers as you go
//  If you encounter an identifier for an unresolved declaration, pause
//  and do that declaration.  
//  Structs are more complex because they can have type interreferences.
//  E.g. A::c := * B and B::a :: A.d or B::a := * A.
//  So structs can be immediately resolved to type after encountered, but
//  are not fully filled in until finished. Fully filled in is required for
//  indirecting, because using requires it.  Could try to support partial, but
//  that gets really complex really fast.

struct STypeRecurse
{
	// BB (adrianb) We'll need these hooked up for code generation. Annotate the AST values directly?
	SAst ** ppAst;
	SSymbolTable * pSymtParent;
};

struct SDeclaration
{
	const char * pChzName;

	// BB (adrianb) Need this for specific instances of declarations. Better way to store it?

	SAstDeclareSingle * pAstdecl; // Which instance of a procedure is it? Which declaration (do we care)?

	// For out of sync declarations type check in any order, not done until pType is set.

	int iTrecCur;
	SArray<STypeRecurse> aryTrec;
};

struct SResolveDecl
{
	SDeclaration * pDecl; // Declaration to resolve
	SArray<SDeclaration *> arypDeclUsingPath; // Using path, statically allocated
};



template <class T>
T * PtypeClone(SWorkspace * pWork, const T * pTIn, SType ** ppType)
{
	// BB (adrianb) Make sure any allocated memory is copied as well

	T * pT = PtAlloc<T>(&pWork->pagealloc);
	memcpy(pT, pTIn, sizeof(T));
	*ppType = pT;
	return pT;
}

void SetType(SWorkspace * pWork, STypeId tid)
{
	// BB (adrianb) Duplicating Hv calculation for TidEnsure.

	u32 hv = HvFromType(*tid.pType);

	if (PtLookupImpl(&pWork->setTid, hv, *tid.pType))
	{
		ASSERTCHZ(false, "Duplicate type %s added to compiler, compiler needs to detect this earlier.\n", 
				  StrPrintType(tid.pType).Pchz());
	}

	Add(&pWork->setTid, hv, tid);
}

STypeId TidEnsure(SWorkspace * pWork, const SType * const pTypeTry)
{
	u32 hv = HvFromType(*pTypeTry);
	const STypeId * pTid = PtLookupImpl(&pWork->setTid, hv, *pTypeTry);
	
	if (pTid)
		return *pTid;

	// BB (adrianb) Don't need to inter sub-types?

	SType * pType = nullptr;

	switch (pTypeTry->typek)
	{
	case TYPEK_Pointer:
		{
			(void) PtypeClone(pWork, static_cast<const STypePointer *>(pTypeTry), &pType);
		}
		break;

	case TYPEK_String:
		{
			(void) PtypeClone(pWork, static_cast<const STypeString *>(pTypeTry), &pType);
		}
		break;

	case TYPEK_Array:
		{
			(void) PtypeClone(pWork, static_cast<const STypeArray *>(pTypeTry), &pType);
		}
		break;

	case TYPEK_Procedure:
		{
			auto pTypeproc = PtypeClone(pWork, static_cast<const STypeProcedure *>(pTypeTry), &pType);
			pTypeproc->aTidArg = PtClone<STypeId>(&pWork->pagealloc, pTypeproc->aTidArg, pTypeproc->cTidArg);
			pTypeproc->aTidRet = PtClone<STypeId>(&pWork->pagealloc, pTypeproc->aTidRet, pTypeproc->cTidRet);
		}
		break;

	case TYPEK_TypeOf:
		{
			(void) PtypeClone(pWork, static_cast<const STypeTypeOf *>(pTypeTry), &pType);
		}
		break;

	default:
		{
			ASSERTCHZ(FIsBuiltinType(pTypeTry->typek), "Don't support uniquifying typek %s(%d)", 
						PchzFromTypek(pTypeTry->typek), pTypeTry->typek);
			PtypeClone(pWork, pTypeTry, &pType);
		}
		break;
	}

	STypeId tid = { pType };
	SetType(pWork, tid);

	return tid;
}

inline STypeId TidEnsure(SWorkspace * pWork, const SType & typeTry)
{
	return TidEnsure(pWork, &typeTry);
}

struct STypeCheckSwitch // tag = tcswitch
{
	SDeclaration * pDecl;
};

enum RESLK
{
	RESLK_Local,
	RESLK_WithUsing
};

SResolveDecl * PresdeclLookupSimple(SSymbolTable * pSymt, const char * pChzName, RESLK reslk)
{
	for (auto pResdecl : pSymt->arypResdecl)
	{
		if (strcmp(pResdecl->pDecl->pChzName, pChzName) == 0)
			return pResdecl;
	}

	// BB (adrianb) Add or strip using rights when lookup up in parent scopes? For some scopes this might be fine.
	//  Others maybe not.

	if (pSymt->pSymtParent)
		return PresdeclLookupSimple(pSymt->pSymtParent, pChzName, reslk);

	return nullptr;
}

void AddDeclaration(SWorkspace * pWork, SSymbolTable * pSymt, const char * pChzName, SAstDeclareSingle * pAstdecl,
					SDeclaration ** ppDeclRet = nullptr, const SArray<SDeclaration *> & arypDeclUsingPath = {})
{
	// BB (adrianb) Check that we aren't doing this during type checking? Or if we do it's a using decl?
	// BB (adrianb) Need to deal with overloading somehow.

	if (PresdeclLookupSimple(pSymt, pChzName, RESLK_WithUsing))
	{
		ShowErr((pAstdecl) ? pAstdecl->errinfo : SErrorInfo(), "Duplicate symbol found");
	}

	auto pDecl = PtAlloc<SDeclaration>(&pWork->pagealloc);
	pDecl->pChzName = pChzName;
	pDecl->pAstdecl = pAstdecl;

	if (ppDeclRet)
		*ppDeclRet = pDecl;
	
	auto pResdecl = PtAlloc<SResolveDecl>(&pWork->pagealloc);
	pResdecl->pDecl = pDecl;
	pResdecl->arypDeclUsingPath = arypDeclUsingPath;

	Append(&pSymt->arypResdecl, pResdecl);
}

SSymbolTable * PsymtStructLookup(SWorkspace * pWork, STypeId tid);

bool FTryResolveUsing(SWorkspace * pWork, SSymbolTable * pSymt, STypeCheckSwitch * pTcswitch)
{
	if (pSymt->pSymtParent)
	{
		if (!FTryResolveUsing(pWork, pSymt->pSymtParent, pTcswitch))
			return false;
	}
		
	for (; pSymt->ipResdeclUsing < pSymt->arypResdecl.c; ++pSymt->ipResdeclUsing)
	{
		auto pResdecl = pSymt->arypResdecl[pSymt->ipResdeclUsing];
		auto pDecl = pResdecl->pDecl;
		auto pAstdecl = pDecl->pAstdecl;
		if (!pAstdecl->fUsing)
			continue;

		// Wait for this type to be resolved

		if (pAstdecl->tid.pType == nullptr)
		{
			pTcswitch->pDecl = pDecl;
			return false;
		}

		// Add the contents of the symbol table of the struct we are using
		//  Or constant entries in a straight type's namespace

		if (pAstdecl->tid.pType->typek == TYPEK_Struct)
		{
			auto pSymtStruct = PsymtStructLookup(pWork, pAstdecl->tid);

			if (pSymt == pSymtStruct)
			{
				// BB (adrianb) Better error here?
				ShowErr(pAstdecl->errinfo, "Loop in using members.");
			}

			for (auto pResdeclUse : pSymtStruct->arypResdecl)
			{
				// Ignore symbol table retrieved by using, we'll get there eventually

				if (pResdeclUse->arypDeclUsingPath.c > 0)
					continue;

				int cpDeclUsing = pResdecl->arypDeclUsingPath.c + 1;
				auto apDeclUsing = PtAlloc<SDeclaration *>(&pWork->pagealloc, cpDeclUsing);
				for (int ipDeclUsing : IterCount(cpDeclUsing - 1))
				{
					apDeclUsing[ipDeclUsing] = pResdecl->arypDeclUsingPath[ipDeclUsing];
				}

				apDeclUsing[cpDeclUsing - 1] = pResdecl->pDecl;

				const char * pChzName = pResdeclUse->pDecl->pAstdecl->pChzName;
				if (PresdeclLookupSimple(pSymt, pChzName, RESLK_WithUsing))
				{
					// BB (adrianb) Need better error reporting info for both conflicting symbol paths?

					auto pAstdeclRoot = apDeclUsing[0]->pAstdecl;
					ShowErr(pAstdeclRoot->errinfo, "Duplicate symbol %s imported through using declaration", pChzName);
				}

				auto pResdeclUsing = PtAlloc<SResolveDecl>(&pWork->pagealloc);
				pResdeclUsing->pDecl = pResdeclUse->pDecl;
				pResdeclUsing->arypDeclUsingPath = { apDeclUsing, cpDeclUsing };
				
				Append(&pSymt->arypResdecl, pResdeclUsing);
			}
		}
		else
		{
			ShowErr(pAstdecl->errinfo, "Can't apply using to a non-struct type.");
		}
	}

	return true;
}

bool FTryResolveSymbolWithUsing(SWorkspace * pWork, SSymbolTable * pSymt, const char * pChzName, 
								STypeCheckSwitch * pTcswitch, SResolveDecl ** ppResdecl)
{
	// Resolve using declarations all the way up the chain
	if (!FTryResolveUsing(pWork, pSymt, pTcswitch))
		return false;

	auto pResdecl = PresdeclLookupSimple(pSymt, pChzName, RESLK_WithUsing);
	if (pResdecl && pResdecl->pDecl->pAstdecl->tid.pType == nullptr)
	{
		pTcswitch->pDecl = pResdecl->pDecl;
		return false;
	}

	*ppResdecl = pResdecl;
	return true;
}

SSymbolTable * PsymtCreate(SYMTBLK symtblk, SWorkspace * pWork, SSymbolTable * pSymtParent)
{
	SSymbolTable * pSymt = PtAlloc<SSymbolTable>(&pWork->pagealloc);
	pSymt->symtblk = symtblk;
	pSymt->pSymtParent = pSymtParent;
	Append(&pWork->arypSymtAll, pSymt);
	return pSymt;
}

SSymbolTable * PsymtStructLookup(SWorkspace * pWork, STypeId tid)
{
	auto hv = HvFromKey(tid);
	SSymbolTable ** ppSymt = PtLookupImpl(&pWork->hashTidPsymtStruct, hv, tid);
	ASSERT(!ppSymt || *ppSymt);
	return (ppSymt) ? *ppSymt : nullptr;
}

void RegisterStruct(SWorkspace * pWork, STypeId tid, SSymbolTable * pSymt)
{
	ASSERT(pSymt->symtblk == SYMTBLK_Struct);
	auto hv = HvFromKey(tid);
	ASSERT(!PtLookupImpl(&pWork->hashTidPsymtStruct, hv, tid));
	
	Add(&pWork->hashTidPsymtStruct, hv, tid, pSymt);

	auto pTypestruct = const_cast<STypeStruct *>(Ptypestruct(tid.pType));
	Append(&pWork->arypTypestruct, pTypestruct);

#if 0
	if (strcmp(pTypestruct->pChzName, "_StringStruct") == 0)
	{
		ASSERT(pWork->pTypestructString == nullptr);
		pWork->pTypestructString = pTypestruct;
	}
#endif
}

STypeId TidWrap(SWorkspace * pWork, const STypeId & tid)
{
	if (tid.pType->typek == TYPEK_TypeOf)
		return tid;

	STypeTypeOf typeof;
	typeof.typek = TYPEK_TypeOf;
	typeof.tid = tid;

	return TidEnsure(pWork, &typeof);
}

STypeId TidPointer(SWorkspace * pWork, STypeId tid)
{
	STypePointer typeptr = {};
	typeptr.typek = TYPEK_Pointer;
	typeptr.tidPointedTo = tid;

	return TidEnsure(pWork, &typeptr);
}

void AddBuiltinType(SWorkspace * pWork, const char * pChzName, const SType & typeIn, STypeId * pTid = nullptr)
{
	STypeId tid = TidEnsure(pWork, typeIn);
	STypeId tidType = TidWrap(pWork, tid);

	auto pAstdecl = PastCreate<SAstDeclareSingle>(pWork, SErrorInfo());
	pAstdecl->pChzName = pChzName;
	pAstdecl->tid = tidType;
	// BB (adrianb) No type/value ast? Ok?

	auto pSymt = &pWork->symtBuiltin;
	AddDeclaration(pWork, pSymt, pChzName, pAstdecl);

	if (pTid)
		*pTid = tid;
}

struct SStringImage
{
	u8 * pCh;
	s64 cCh;
};

enum FWINIT
{
	FWINIT_IncludeBuiltinModule = 0x1,

	GRFWINIT_None = 0
};

typedef u32 GRFWINIT;

void InitWorkspace(SWorkspace * pWork, GRFWINIT grfwinit)
{
	ClearStruct(pWork);
	pWork->cOperator = DIM(g_aOperator);

	Init(&pWork->pagealloc, 64 * 1024);

	pWork->symtRoot.pSymtParent = &pWork->symtBuiltin;

	Append(&pWork->arypSymtAll, &pWork->symtBuiltin);
	Append(&pWork->arypSymtAll, &pWork->symtRoot);

	pWork->symtRoot.symtblk = SYMTBLK_TopLevel;

	// Initialize built in types

	AddBuiltinType(pWork, "void", SType{TYPEK_Void}, &pWork->tidVoid);
	AddBuiltinType(pWork, "bool", SType{TYPEK_Bool}, &pWork->tidBool);

	AddBuiltinType(pWork, "s64", SType{TYPEK_S64}, &pWork->tidS64);
	AddBuiltinType(pWork, "u64", SType{TYPEK_U64});
	AddBuiltinType(pWork, "s32", SType{TYPEK_S32}, &pWork->tidS32);
	AddBuiltinType(pWork, "u32", SType{TYPEK_U32}, &pWork->tidU32);
	AddBuiltinType(pWork, "s16", SType{TYPEK_S16}, &pWork->tidS16);
	AddBuiltinType(pWork, "u16", SType{TYPEK_U16});
	AddBuiltinType(pWork, "s8", SType{TYPEK_S8}, &pWork->tidS8);
	AddBuiltinType(pWork, "u8", SType{TYPEK_U8}, &pWork->tidU8);

	AddBuiltinType(pWork, "int", SType{TYPEK_S32});
	AddBuiltinType(pWork, "char", SType{TYPEK_U8});

	AddBuiltinType(pWork, "float", SType{TYPEK_Float}, &pWork->tidFloat);
	AddBuiltinType(pWork, "double", SType{TYPEK_Double});
	AddBuiltinType(pWork, "f32", SType{TYPEK_Float});
	AddBuiltinType(pWork, "f64", SType{TYPEK_Double});

	{
		SSymbolTable * pSymtString = PsymtCreate(SYMTBLK_Struct, pWork, nullptr);
		auto pTypestructString = PtAlloc<STypeStruct>(&pWork->pagealloc);
        pSymtString->pTypestruct = pTypestructString;
		pTypestructString->typek = TYPEK_Struct;
		pTypestructString->pChzName = "_string";
		pTypestructString->aMember = PtAlloc<STypeStruct::SMember>(&pWork->pagealloc, 2);
		pTypestructString->cMember = 2;
		pTypestructString->cMemberMax = 2;

		auto pAstdeclPch = PastCreate<SAstDeclareSingle>(pWork, {});
		pAstdeclPch->pChzName = "pCh";
		pAstdeclPch->tid = TidPointer(pWork, pWork->tidU8);
		
		AddDeclaration(pWork, pSymtString, "pCh", pAstdeclPch);

		auto pAstdeclCch = PastCreate<SAstDeclareSingle>(pWork, {});
		pAstdeclCch->pChzName = "cCh";
		pAstdeclCch->tid = pWork->tidU32; // BB (adrianb) Use u64? More than 4 GiB?

		AddDeclaration(pWork, pSymtString, "cCh", pAstdeclCch);

		pTypestructString->aMember[0].pAstdecl = pAstdeclPch;
		pTypestructString->aMember[1].pAstdecl = pAstdeclCch;

		STypeString typestring = {};
		typestring.typek = TYPEK_String;
		typestring.pTypestruct = pTypestructString;

		AddBuiltinType(pWork, "string", typestring, &pWork->tidString);

		RegisterStruct(pWork, pWork->tidString, pSymtString);
	}

	//AddBuiltinType(pWork, "Any", SType{TYPEK_Any});

#if 0
	if (grfwinit & FWINIT_IncludeBuiltinModule)
	{
		SModule * pModule = PtAppendNew(&pWork->aryModule);
		pModule->pChzFile = "builtin-code";
		pModule->pChzContents = 
		"_StringStruct :: struct { pCh : *u8; cCh : s64; };\n";
		pModule->fBuiltIn = true;
	}
#endif
}

void Destroy(SWorkspace * pWork)
{
	// BB (adrianb) Almost all of this junk is to deal with dynamically sized arrays.
	//  If the arrays could somehow be statically sized (or chunked) we could just allocate
	//  them out of page memory and clear only that.

	for (auto pAst : pWork->arypAstAll)
	{
		Destroy(pAst);
	}
	Destroy(&pWork->arypAstAll);

	for (auto pSymt : pWork->arypSymtAll)
	{
		for (auto pResdecl : pSymt->arypResdecl)
		{
			Destroy(&pResdecl->pDecl->aryTrec);
		}
		Destroy(&pSymt->arypResdecl);
	}
	Destroy(&pWork->arypSymtAll);
	Destroy(&pWork->hashTidPsymtStruct);

	for (int iModule = 0; iModule < pWork->aryModule.c; ++iModule)
	{
		SModule * pModule = &pWork->aryModule[iModule];
		if (pModule->fAllocContents)
			free(const_cast<char *>(pModule->pChzContents));
		pModule->pChzContents = nullptr;

		Destroy(&pModule->arypAstproc);
	}
	Destroy(&pWork->aryModule);

	Destroy(&pWork->hashPastPresdeclResolved);
	Destroy(&pWork->arypTypestruct);

	Destroy(&pWork->setpChz);
	Destroy(&pWork->aryTokNext);
	Destroy(&pWork->pagealloc);

	// BB (adrianb) Need to delete all arrays.

	Destroy(&pWork->setTid);
	Destroy(&pWork->pagealloc);
	
	ClearStruct(pWork);
}

// BB (adrianb) Make the compiler/parser a global?

struct SRecurseCtx // tag = recx
{
	SWorkspace * pWork;
	SModule * pModule;
	SArray<STypeRecurse> * paryTrec;
	SSymbolTable * pSymtParent;
};

SRecurseCtx RecxNewScope(const SRecurseCtx & recx)
{
	auto pSymt = PsymtCreate(SYMTBLK_Scope, recx.pWork, recx.pSymtParent);
	pSymt->pSymtParent = recx.pSymtParent;

	SRecurseCtx recxNew = recx;
	recxNew.pSymtParent = pSymt;

	return recxNew;
}

void AppendAst(const SRecurseCtx & recx, SAst ** ppAst)
{
	auto pTrec = PtAppendNew(recx.paryTrec);
	pTrec->ppAst = ppAst;
	pTrec->pSymtParent = recx.pSymtParent;
}

// Register and recurse or just recurse

void RecurseTypeCheck(const SRecurseCtx & recxIn, SAst ** ppAst);

void RecurseProcArgRet(SRecurseCtx * pRecxProc, SAstProcedure * pAstproc)
{
	SSymbolTable * pSymtProc = PsymtCreate(SYMTBLK_Procedure, pRecxProc->pWork, pRecxProc->pSymtParent);
	pSymtProc->pAstproc = pAstproc;
	pRecxProc->pSymtParent = pSymtProc;

	for (SAst ** ppAstDecl : IterPointer(pAstproc->arypAstDeclArg))
	{
		RecurseTypeCheck(*pRecxProc, ppAstDecl);
	}

	// BB (adrianb) Need to make sure return values cannot have names? or their names are ignored?

	for (SAst * pAst : pAstproc->arypAstDeclRet)
	{
		auto pAstdecl = PastCast<SAstDeclareSingle>(pAst);
		ASSERT(pAstdecl->pAstType && pAstdecl->pAstValue == nullptr);

		// Purposely not type checking value or declaration as whole to avoid registering it.
		// BB (adrianb) Keep only the type after parsing? We don't need anything afaik.
		
		RecurseTypeCheck(*pRecxProc, &pAstdecl->pAstType);
	}
}

void RecurseTypeCheckDecl(const SRecurseCtx & recxIn, SAst ** ppAst)
{
	SAst * pAst = *ppAst;

	switch (pAst->astk)
	{
	case ASTK_DeclareSingle:
		{
			auto pAstdecl = PastCast<SAstDeclareSingle>(pAst);
			SRecurseCtx recx = recxIn;

			// Register for out-of-order when a constant, top level, or in a struct.

			if (pAstdecl->pChzName && 
				(pAstdecl->fIsConstant || recx.pSymtParent->symtblk >= SYMTBLK_RegisterAllMic))
			{
				SDeclaration * pDecl;
				AddDeclaration(recx.pWork, recx.pSymtParent, pAstdecl->pChzName, pAstdecl, &pDecl);

				// Procedure handles its own declaration addition

				if (recx.pSymtParent->symtblk != SYMTBLK_Procedure)
					recx.paryTrec = &pDecl->aryTrec;
			}

			if (pAstdecl->pAstType)
				RecurseTypeCheck(recx, &pAstdecl->pAstType);
			
			if (pAstdecl->pAstValue)
			{
				if (pAstdecl->pAstValue->astk == ASTK_Procedure)
				{
					// Recurse differently for procedures to support recursion
					//  Type check: arg/ret, proc, declaration, body

					auto pAstproc = PastCast<SAstProcedure>(pAstdecl->pAstValue);
					SRecurseCtx recxProc = recx;
					RecurseProcArgRet(&recxProc, pAstproc);
					AppendAst(recx, &pAstdecl->pAstValue);
					AppendAst(recx, ppAst);

					if (pAstproc->pAstblock)
						RecurseTypeCheck(recxProc, reinterpret_cast<SAst **>(&pAstproc->pAstblock));
				}
				else
				{
					RecurseTypeCheck(recx, &pAstdecl->pAstValue);
					AppendAst(recx, ppAst);
				}
			}
			else
			{
				AppendAst(recx, ppAst);
			}
		}
		break;

	case ASTK_EmptyStatement:
		break;
			
	case ASTK_ImportDirective:
		if (recxIn.pSymtParent->symtblk == SYMTBLK_TopLevel)
			break;
		// Intentional fallthrough...

	default:
		ShowErr(pAst->errinfo, "Non-declaration cannot be handled in this scope");
		break;
	}
}

void RecurseTypeCheck(const SRecurseCtx & recx, SAst ** ppAst)
{
	// BB (adrianb) Should we have a child AST array which comes in type recursion order?
	//  And the node just knows which is which if it needs to do anything? We could almost
	//  get rid of this function if so. Still need to specify where to create new scopes

	SAst * pAst = *ppAst;

	switch (pAst->astk)
	{
	// BB (adrianb) These should remain literals until usage coercion.
	case ASTK_Literal:
	case ASTK_Null:
		break;

	case ASTK_UninitializedValue:
		return;
	
	case ASTK_Block:
		{
			SRecurseCtx recxInner = RecxNewScope(recx);
			auto pAstblock = PastCast<SAstBlock>(pAst);
			for (int ipAst : IterCount(pAstblock->arypAst.c))
				RecurseTypeCheck(recxInner, &pAstblock->arypAst[ipAst]);
		}
		break;

	case ASTK_EmptyStatement:
	case ASTK_Identifier:
		break;

	case ASTK_Operator:
		{
			auto pAstop = PastCast<SAstOperator>(pAst);

			if (pAstop->pAstLeft)
				RecurseTypeCheck(recx, &pAstop->pAstLeft);

			// . doesn't process its right expression (should be identifier)

			if (strcmp(".", pAstop->pChzOp) != 0)
			{
				RecurseTypeCheck(recx, &pAstop->pAstRight);
			}
		}
		break;

	case ASTK_If:
		{
			auto pAstif = PastCast<SAstIf>(pAst);
			RecurseTypeCheck(recx, &pAstif->pAstCondition);
			RecurseTypeCheck(RecxNewScope(recx), &pAstif->pAstPass);
			if (pAstif->pAstElse)
				RecurseTypeCheck(RecxNewScope(recx), &pAstif->pAstElse);
		}
		break;

	case ASTK_While:
		{
			auto pAstwhile = PastCast<SAstWhile>(pAst);
			RecurseTypeCheck(recx, &pAstwhile->pAstCondition);

			SRecurseCtx recxWhile = RecxNewScope(recx);
			RecurseTypeCheck(recxWhile, &pAstwhile->pAstLoop);
		}
		break;

	case ASTK_For:
		{
			// BB (adrianb) Build the unwrapped structure for this during parse?
			// for [(*) value :] expr: block ->
			// { it/name : type; goto check; incr; if (!check) jump end; block; goto incr; end }

			SRecurseCtx recxFor = RecxNewScope(recx);
			auto pAstfor = PastCast<SAstFor>(pAst);
			// NOTE (adrianb) pAstIter is just a identifier which absorbs the type on the right or so
			RecurseTypeCheck(recx, &pAstfor->pAstIterRight);
			AppendAst(recxFor, ppAst); // Type check for and iterator names and such.

			SRecurseCtx recxLoop = RecxNewScope(recxFor);
			RecurseTypeCheck(recxLoop, &pAstfor->pAstLoop);
			return;
		}

	case ASTK_LoopControl:
		break;

	case ASTK_Using:
		ASSERT(false);
		// BB (adrianb) Should be processed up front with constant declarations?
		return;
		
	case ASTK_Cast:
		{
			auto pAstcast = PastCast<SAstCast>(pAst);
			RecurseTypeCheck(recx, &pAstcast->pAstType);
			RecurseTypeCheck(recx, &pAstcast->pAstExpr);
		}
		break;

	case ASTK_New:
		RecurseTypeCheck(recx, &PastCast<SAstNew>(pAst)->pAstType);
		break;

	case ASTK_Delete:
		RecurseTypeCheck(recx, &PastCast<SAstDelete>(pAst)->pAstExpr);
		break;

	case ASTK_Remove:
		RecurseTypeCheck(recx, &PastCast<SAstRemove>(pAst)->pAstExpr);
		break;

	case ASTK_Defer:
		RecurseTypeCheck(recx, &PastCast<SAstDefer>(pAst)->pAstStmt);
		break;
	
	case ASTK_Inline:
		RecurseTypeCheck(recx, &PastCast<SAstInline>(pAst)->pAstExpr);
		break;
	
	case ASTK_PushContext:
		RecurseTypeCheck(RecxNewScope(recx), reinterpret_cast<SAst **>(&PastCast<SAstPushContext>(pAst)->pAstblock));
		break;

	case ASTK_ArrayIndex:
		{
			auto pAstarrayindex = PastCast<SAstArrayIndex>(pAst);
			RecurseTypeCheck(recx, &pAstarrayindex->pAstArray);
			RecurseTypeCheck(recx, &pAstarrayindex->pAstIndex);
		}
		break;
	
	case ASTK_Call:
		{
			auto pAstcall = PastCast<SAstCall>(pAst);
			RecurseTypeCheck(recx, &pAstcall->pAstFunc);
			for (int ipAst : IterCount(pAstcall->arypAstArgs.c))
				RecurseTypeCheck(recx, &pAstcall->arypAstArgs[ipAst]);
		}
		break;
	
	case ASTK_Return:
		{
			auto pAstret = PastCast<SAstReturn>(pAst);
			for (int ipAst : IterCount(pAstret->arypAstRet.c))
				RecurseTypeCheck(recx, &pAstret->arypAstRet[ipAst]);
		}
		break;

	case ASTK_DeclareSingle:
		{
			RecurseTypeCheckDecl(recx, ppAst);
			return;
		}

#if 0
	case ASTK_DeclareMulti:
		break;

	case ASTK_AssignMulti:
		break;
#endif

	case ASTK_Struct:
		{
			auto pAststruct = PastCast<SAstStruct>(pAst);

			SRecurseCtx recxStruct = recx;

			SSymbolTable * pSymtMembers = PsymtCreate(SYMTBLK_Struct, recx.pWork, recx.pSymtParent);
			recxStruct.pSymtParent = pSymtMembers;

			// Type check the struct right away, will create type but not fill it in
			//  Passing the pSymtMembers to the struct so it can register its type against it.
			// BB (adrianb) Better way to do this.
			
			AppendAst(recxStruct, ppAst);

			for (SAst ** ppAstDecl : IterPointer(pAststruct->arypAstDecl))
			{
				RecurseTypeCheck(recxStruct, ppAstDecl);
			}

			return;
		}

	case ASTK_Enum:
		ASSERT(false);
		break;

	case ASTK_Procedure:
		{
			auto pAstproc = PastCast<SAstProcedure>(pAst);

			// BB (adrianb) Check for any polymorphic-ness and store off for type matching and then type checking after.

			// BB (adrianb) Assumed to be constant for the moment. Lambdas will need to do similar work.

			SRecurseCtx recxProc = recx;

			RecurseProcArgRet(&recxProc, pAstproc);

			// Actually typecheck the function in the passed in context, this will set the type and register
			//  arguments in the procedure symbol table.

			AppendAst(recx, ppAst);

			// Typecheck the body with its own non-reordering symbol table

			if (pAstproc->pAstblock)
			{
				// This will always add a scope which we rely on.

				RecurseTypeCheck(recxProc, reinterpret_cast<SAst **>(&pAstproc->pAstblock));
			}
		}
		return;

#if 0
	case ASTK_TypeDefinition:
		break;
#endif

	case ASTK_TypePointer:
		RecurseTypeCheck(recx, &PastCast<SAstTypePointer>(pAst)->pAstTypeInner);
		break;

	case ASTK_TypeArray:
		{
			auto pAtypearray = PastCast<SAstTypeArray>(pAst);
			RecurseTypeCheck(recx, &pAtypearray->pAstTypeInner);
			if (pAtypearray->pAstSize)
				RecurseTypeCheck(recx, &pAtypearray->pAstSize);
		}
		break;

	case ASTK_TypeProcedure:
		{
			auto pAtypeproc = PastCast<SAstTypeProcedure>(pAst);

			for (SAst * pAst : pAtypeproc->arypAstDeclArg)
			{
				// Purposely not type checking value or declaration as whole to avoid registering it.
				// BB (adrianb) Keep only the type after parsing? We don't need anything afaik.

				// BB (adrianb) Does type need to carry default value?
				
				RecurseTypeCheck(recx, &PastCast<SAstDeclareSingle>(pAst)->pAstType);
			}

			for (SAst * pAst : pAtypeproc->arypAstDeclRet)
			{
				// Purposely not type checking value or declaration as whole to avoid registering it.
				// BB (adrianb) Keep only the type after parsing? We don't need anything afaik.
				
				RecurseTypeCheck(recx, &PastCast<SAstDeclareSingle>(pAst)->pAstType);
			}
		}
		break;

	case ASTK_TypePolymorphic:
		ASSERT(false);
		break;

	case ASTK_TypeVararg:
		break;

	case ASTK_ImportDirective:
		return;

	case ASTK_RunDirective:
		RecurseTypeCheck(recx, &PastCast<SAstRunDirective>(pAst)->pAstExpr);
		break;

	// ASTK_ForeignLibraryDirective

	default:
		ASSERTCHZ(false, "Unhandled typecheck ast %s(%d)", PchzFromAstk(pAst->astk), pAst->astk);
		break;
	}

	AppendAst(recx, ppAst);
}

STypeId TidUnwrap(const SErrorInfo & errinfo, STypeId tid)
{
	if (tid.pType->typek != TYPEK_TypeOf)
	{
		ShowErr(errinfo, "Cannot point to instance of type");
	}

	return static_cast<const STypeTypeOf *>(tid.pType)->tid;
}

STypeId TidWrapPointer(SWorkspace * pWork, STypeId tid) 
{
	STypeTypeOf typeof = {};
	typeof.typek = TYPEK_TypeOf;
	typeof.tid = TidPointer(pWork, tid);
	
	return TidEnsure(pWork, &typeof);
}

STypeId TidInferFromInt(SWorkspace * pWork, s64 n)
{
	// BB (adrianb) Need some way to tell if this should be a u64. Or unsigned types at all.

	CASSERT(TYPEK_U8 - TYPEK_S8 == 4);

	s64 nAbs = (n > 0) ? n : -n;
	if (nAbs < (1ll << 8))
		return pWork->tidS8;
	else if (nAbs < (1ll << 16))
		return pWork->tidS16;
	else if (nAbs < (1ll << 32))
		return pWork->tidS32;
	else
		return pWork->tidS64;
}

STypeId TidFromLiteral(SWorkspace * pWork, const SLiteral & lit)
{
	STypeId tid = {};
	switch (lit.litk)
	{
	case LITK_String: tid = pWork->tidString; break;
	case LITK_Int: tid = TidInferFromInt(pWork, lit.n); break;
	case LITK_Float: tid = pWork->tidFloat; break; // BB (adrianb) Any way to choose double?
	case LITK_Bool: tid = pWork->tidBool; break;
	default: ASSERT(false); break;
	}
	return tid;
}

void TryCoerceLit(SWorkspace * pWork, SAst * pAst)
{
	// BB (adrianb) null -> void *?

	if (pAst->tid.pType == nullptr)
	{
		if (pAst->astk != ASTK_Literal)
		{
			ShowErr(pAst->errinfo, "Can't generate type for a non-literal");
		}

		pAst->tid = TidFromLiteral(pWork, PastCast<SAstLiteral>(pAst)->lit);
	}
}

void Coerce(SWorkspace * pWork, SAst ** ppAst, const STypeId & tid);
void TryCoerceCVararg(SWorkspace * pWork, SAst ** ppAst)
{
	auto pAst = *ppAst;
	STypeId tidSrc = pAst->tid;
	if (tidSrc.pType == nullptr)
	{
		if (pAst->astk != ASTK_Literal)
		{
			ShowErr(pAst->errinfo, "Can't generate type for a non-literal");
		}

		tidSrc = TidFromLiteral(pWork, PastCast<SAstLiteral>(pAst)->lit);
	}

	TYPEK typekSrc = tidSrc.pType->typek;
	if (FIsFloat(typekSrc))
	{
		Coerce(pWork, ppAst, TidEnsure(pWork, SType{TYPEK_Double}));
	}
	else if (FIsInt(typekSrc))
	{
		if (CBit(typekSrc) < 32)
		{
			Coerce(pWork, ppAst, TidEnsure(pWork, SType{FSigned(typekSrc) ? TYPEK_S32 : TYPEK_U32}));
		}
	}

	// If the literal was already big enough, just set it to default type

	if (pAst->tid.pType == nullptr)
	{
		pAst->tid = tidSrc;
	}
}

enum TFN 
{
	TFN_False = 0,
	TFN_True = 1,

	TFN_Nil = -1,
};

TFN TfnCanCoerce(SAst * pAst, TYPEK typek)
{
	if (pAst->astk == ASTK_Literal)
	{
		const SLiteral & lit = PastCast<SAstLiteral>(pAst)->lit;

		switch(lit.litk)
		{
		case LITK_String:
			if (typek == TYPEK_String)
			{
				return TFN_True;
			}
			else if (typek == TYPEK_Pointer)
			{
				// We may allow this for * u8

				return TFN_Nil;
			}
			break;

		case LITK_Int:
			{
				if (typek == TYPEK_S8 || typek == TYPEK_S16 ||
					typek == TYPEK_S32 || typek == TYPEK_S64)
				{
					s64 nPow2 = 1ll << (CBit(typek) - 1);
					return (lit.n >= -nPow2 && lit.n <= nPow2 - 1) ? TFN_True : TFN_False;
				}
				else if (typek == TYPEK_U8 || typek == TYPEK_U16 ||
						 typek == TYPEK_U32 || typek == TYPEK_U64)
				{
					u64 nMax = (1ull << CBit(typek)) - 1;
					return (lit.n >= 0 && u64(lit.n) <= nMax) ? TFN_True : TFN_False;
				}
				else if (typek == TYPEK_Float || typek == TYPEK_Double)
				{
					// Convert int literal to float
					return TFN_True;
				}
			}
			break;

		case LITK_Float:
			{
				// BB (adrianb) Just auto convert to float or double? Should we try and 
				//  detect correct precision or just let user specify it?

				return (typek == TYPEK_Float || typek == TYPEK_Double) ? TFN_True : TFN_False;
			}
			break;

		case LITK_Bool:
			return (typek == TYPEK_Bool) ? TFN_True : TFN_False;

		default:
			break;
		}

		return TFN_False;
	}
	else if (pAst->astk == ASTK_Null)
	{
		return (typek == TYPEK_Pointer) ? TFN_True : TFN_False;
	}
	else if (pAst->astk == ASTK_UninitializedValue)
	{
		// You can always leave something uninitialized
		return TFN_True;
	}

	// Allow coercing to larger integer type

	ASSERT(pAst->tid.pType != nullptr);

	TYPEK typekAst = pAst->tid.pType->typek;
	
	if (FIsInt(typekAst) && FIsInt(typek))
	{
		// A signed type can only fit in an >= signed type

		if (FSigned(typekAst))
			return (FSigned(typek) && CBit(typek) >= CBit(typekAst)) ? TFN_True : TFN_False;
		
		// An unsigned type can fix in a >= unsigned type or > signed type

		bool fCompatible = (!FSigned(typek)) ? CBit(typek) >= CBit(typekAst) : CBit(typek) > CBit(typekAst);
		return (fCompatible) ? TFN_True : TFN_False;
	}

	if (typekAst == TYPEK_Float && typek == TYPEK_Double)
	{
		return TFN_True;
	}

	// Don't know how to deal with this one

	return TFN_Nil;
}

bool FCanCoerce(SAst * pAst, TYPEK typek)
{
	TFN tfn = TfnCanCoerce(pAst, typek);
	if (tfn != TFN_Nil)
		return tfn == TFN_True;

	ASSERT(FIsBuiltinType(typek));

	return pAst->tid.pType && pAst->tid.pType->typek == typek;
}

bool FCanCoerce(SAst * pAst, const STypeId & tidTo)
{
	if (tidTo == pAst->tid)
		return true;

	// BB (adrianb) In most of these cases we can return false after knowing about this too

	TFN tfn = TfnCanCoerce(pAst, tidTo.pType->typek);
	if (tfn != TFN_Nil)
		return tfn == TFN_True;

	auto pTypeTo = tidTo.pType;
	if (pAst->astk == ASTK_Literal)
	{
		const SLiteral & lit = PastCast<SAstLiteral>(pAst)->lit;
		if (lit.litk == LITK_String && pTypeTo->typek == TYPEK_Pointer)
		{
			// Allowing literal to point to char * for easier C interop.

			auto pTypeptr = PtypeCast<STypePointer>(pTypeTo);
			auto pTypePointedTo = pTypeptr->tidPointedTo.pType;
			ASSERT(pTypePointedTo);

			// BB (adrianb) Do I care if it's SOA?

			return (!pTypeptr->fSoa && pTypePointedTo->typek == TYPEK_U8);
		}
	}
	
	if (pTypeTo->typek == TYPEK_Array && pAst->tid.pType->typek == TYPEK_Array)
	{
		auto pTypearrayTo = PtypeCast<STypeArray>(pTypeTo);
		auto pTypearrayFrom = PtypeCast<STypeArray>(pAst->tid.pType);

		return pTypearrayTo->cSizeFixed < 0 && !pTypearrayTo->fDynamicallySized &&
				(pTypearrayFrom->cSizeFixed >= 0 || pTypearrayFrom->fDynamicallySized);
	}

	// BB (adrianb) More conversions?
	//  Non strict enum to builtin type
	// + int to bigger int (same for u).
	// + float to bigger float
	//  Or stronger implicit coercion to bool if you're checking an if statement?

	return false;
}

void Coerce(SWorkspace * pWork, SAst ** ppAst, const STypeId & tid)
{
	auto pAst = *ppAst;
	if (!FCanCoerce(pAst, tid))
	{
		ShowErr(pAst->errinfo, "Cannot convert to type %s", StrPrintType(tid.pType).Pchz());
	}

	auto pTypeTo = tid.pType;

	switch (pAst->astk)
	{
	case ASTK_Literal:
		{
			auto pLit = &PastCast<SAstLiteral>(pAst)->lit;
			if (pLit->litk == LITK_Int && (pTypeTo->typek == TYPEK_Float || pTypeTo->typek == TYPEK_Double))
			{
				pLit->litk = LITK_Float;
				pLit->g = static_cast<double>(pLit->n);
			}

			pAst->tid = tid;
			return;
		}

	case ASTK_Null:
		{
			pAst->tid = tid;
			return;
		}

	case ASTK_UninitializedValue:
		{
			pAst->tid = tid;
			return;
		}

	default:
		break;
	}

	ASSERT(pAst->tid.pType != nullptr);

	if (pAst->tid == tid)
		return;

	// Insert implicit cast

	auto pAstcast = PastCreate<SAstCast>(pWork, pAst->errinfo);
	pAstcast->tid = tid;
	pAstcast->pAstExpr = pAst;

	*ppAst = pAstcast;
}

enum FBOPI
{
	FBOPI_AllIntegers 	= 0x01,
	FBOPI_Bools			= 0x02,
	FBOPI_AllFloats 	= 0x04,
	FBOPI_Pointers	 	= 0x08,
	FBOPI_PointerAndInt	= 0x10,

	FBOPI_AnySame 		= 0x100,

	FBOPI_ReturnBool	= 0x1000,
};

typedef u32 GRFBOPI;



struct SBinaryOperatorInst
{
	const char * pChzOp;
	GRFBOPI grfbopi;
};

static const SBinaryOperatorInst s_aBopi[] =
{
	{ "<", FBOPI_AllIntegers | FBOPI_AllFloats | FBOPI_Pointers | FBOPI_ReturnBool },
	{ ">", FBOPI_AllIntegers | FBOPI_AllFloats | FBOPI_Pointers | FBOPI_ReturnBool },
	{ "<=", FBOPI_AllIntegers | FBOPI_AllFloats | FBOPI_Pointers | FBOPI_ReturnBool },
	{ ">=", FBOPI_AllIntegers | FBOPI_AllFloats | FBOPI_Pointers | FBOPI_ReturnBool },
	{ "==", FBOPI_AllIntegers | FBOPI_Bools | FBOPI_AllFloats | FBOPI_Pointers | FBOPI_ReturnBool }, // Floats for == et al?
	{ "!=", FBOPI_AllIntegers | FBOPI_Bools | FBOPI_AllFloats | FBOPI_Pointers | FBOPI_ReturnBool },

	{ "and", FBOPI_Bools },
	{ "or", FBOPI_Bools },

	{ "+", FBOPI_AllIntegers | FBOPI_AllFloats | FBOPI_PointerAndInt }, // BB (adrianb) Reuse these with += et al?
	{ "-", FBOPI_AllIntegers | FBOPI_AllFloats | FBOPI_PointerAndInt },
	{ "*", FBOPI_AllIntegers | FBOPI_AllFloats },
	{ "/", FBOPI_AllIntegers | FBOPI_AllFloats },
	{ "%", FBOPI_AllIntegers },

	// TODO bitwise operators (what are they?), logical operators (&&, ||?)
};

bool FTryCoerceOperatorArgs(SWorkspace * pWork, SAstOperator * pAstop, GRFBOPI grfbopi, STypeId * pTidRet)
{
	ClearStruct(pTidRet);

	auto pAstLeft = pAstop->pAstLeft;
	auto pAstRight = pAstop->pAstRight;

	if (grfbopi & FBOPI_AllIntegers)
	{
		static const TYPEK s_aTypekInt[] =
		{
			TYPEK_S8,
			TYPEK_U8,
			TYPEK_S16,
			TYPEK_U16,
			TYPEK_S32,
			TYPEK_U32,
			TYPEK_S64,
			TYPEK_U64,
		};
		CASSERT(DIM(s_aTypekInt) == TYPEK_IntMac - TYPEK_IntMic);

		for (TYPEK typek : s_aTypekInt)
		{
			if (FCanCoerce(pAstLeft, typek) && FCanCoerce(pAstRight, typek))
			{
				auto tid = TidEnsure(pWork, SType{typek});
				Coerce(pWork, &pAstop->pAstLeft, tid);
				Coerce(pWork, &pAstop->pAstRight, tid);
				*pTidRet = tid;
				return true;
			}
		}
	}

	if (grfbopi & FBOPI_Bools)
	{
		if (FCanCoerce(pAstLeft, TYPEK_Bool) && FCanCoerce(pAstRight, TYPEK_Bool))
		{
			auto tid = TidEnsure(pWork, SType{TYPEK_Bool});
			Coerce(pWork, &pAstop->pAstLeft, tid);
			Coerce(pWork, &pAstop->pAstRight, tid);
			*pTidRet = tid;
			return true;
		}
	}

	if (grfbopi & FBOPI_AllFloats)
	{
		for (TYPEK typek = TYPEK_Float; typek <= TYPEK_Double; typek = TYPEK(typek + 1))
		{
			if (FCanCoerce(pAstLeft, typek) && FCanCoerce(pAstRight, typek))
			{
				auto tid = TidEnsure(pWork, SType{typek});
				Coerce(pWork, &pAstop->pAstLeft, tid);
				Coerce(pWork, &pAstop->pAstRight, tid);
				*pTidRet = tid;
				return true;
			}
		}
	}

	if (grfbopi & FBOPI_Pointers)
	{
		auto pTypeLeft = pAstLeft->tid.pType;
		auto pTypeRight = pAstRight->tid.pType;
		if (pAstLeft->tid == pAstRight->tid && 
			pTypeLeft && pTypeLeft->typek == TYPEK_Pointer &&
			pTypeRight && pTypeRight->typek == TYPEK_Pointer)
		{
			*pTidRet = pAstLeft->tid;
			return true;
		}
		else if (pAstLeft->astk == ASTK_Null && pTypeRight && pTypeRight->typek == TYPEK_Pointer)
		{
			Coerce(pWork, &pAstop->pAstLeft, pAstRight->tid);
			*pTidRet = pAstRight->tid;
			return true;
		}
		else if (pAstRight->astk == ASTK_Null && pTypeLeft && pTypeLeft->typek == TYPEK_Pointer)
		{
			Coerce(pWork, &pAstop->pAstRight, pAstLeft->tid);
			*pTidRet = pAstLeft->tid;
			return true;
		}
	}

	if (grfbopi & FBOPI_PointerAndInt)
	{
		auto pTypeLeft = pAstLeft->tid.pType;
		auto pTypeRight = pAstRight->tid.pType;
		if (pTypeLeft && pTypeLeft->typek == TYPEK_Pointer && FCanCoerce(pAstRight, TYPEK_S64))
		{
			Coerce(pWork, &pAstop->pAstRight, TidEnsure(pWork, SType{TYPEK_S64}));
			*pTidRet = pAstLeft->tid;
			return true;
		}
		else if (pTypeRight && pTypeRight->typek == TYPEK_Pointer && FCanCoerce(pAstLeft, TYPEK_S64))
		{
			Coerce(pWork, &pAstop->pAstLeft, TidEnsure(pWork, SType{TYPEK_S64}));
			*pTidRet = pAstRight->tid;
			return true;
		}
	}

	return false;
}

void TryCoerceOperator(SWorkspace * pWork, SAstOperator * pAstop, GRFBOPI grfbopi)
{
	if (FTryCoerceOperatorArgs(pWork, pAstop, grfbopi, &pAstop->tid))
	{
		if (grfbopi & FBOPI_ReturnBool)
			pAstop->tid = pWork->tidBool;
	}
}

bool FCanGetAddress(SAst * pAst)
{
	ASTK astk = pAst->astk;
	if (astk == ASTK_Operator)
	{
		// BB (adrianb) Validate that the right type inside is a non-constant? Or do that at generation time?
		auto pAstop = PastCast<SAstOperator>(pAst);
		auto pChzOp = pAstop->pChzOp;
		if (pAstop->pAstLeft && strcmp(pChzOp, ".") == 0)
			return true;

		if (pAstop->pAstLeft == nullptr && strcmp(pChzOp, "<<") == 0)
			return true;
	}

	if (astk == ASTK_Identifier || astk == ASTK_ArrayIndex)
		return true;

	return false;
}

static SBinaryOperatorInst s_aBopiModify[] =
{
	{ "=", FBOPI_AnySame },
	{ "+=", FBOPI_AllIntegers | FBOPI_AllFloats | FBOPI_PointerAndInt },
	{ "-=", FBOPI_AllIntegers | FBOPI_AllFloats | FBOPI_PointerAndInt },
	{ "*=", FBOPI_AllIntegers | FBOPI_AllFloats },
	{ "/=", FBOPI_AllIntegers | FBOPI_AllFloats },
	{ "%=", FBOPI_AllIntegers },
};

bool FTryCoerceOperatorAssign(SWorkspace * pWork, SAstOperator * pAstop, GRFBOPI grfbopi)
{
	auto pAstLeft = pAstop->pAstLeft;
	auto pAstRight = pAstop->pAstRight;

	STypeId tidStore = pAstLeft->tid;
	ASSERT(tidStore.pType != nullptr);
	auto typekStore = tidStore.pType->typek;

	if ((grfbopi & FBOPI_AnySame) != 0 ||
		((grfbopi & FBOPI_AllIntegers) != 0 && FIsInt(typekStore)) ||
		((grfbopi & FBOPI_AllFloats) != 0 && FIsFloat(typekStore)) ||
		((grfbopi & FBOPI_Bools) != 0 && typekStore == TYPEK_Bool))
	{
		if (FCanCoerce(pAstRight, tidStore))
		{
			Coerce(pWork, &pAstop->pAstRight, tidStore);
			return true;
		}

		return false;
	}

	if ((grfbopi & FBOPI_PointerAndInt) != 0 && typekStore == TYPEK_Pointer)
	{
		if (FCanCoerce(pAstRight, TYPEK_S64))
		{
			Coerce(pWork, &pAstop->pAstRight, TidEnsure(pWork, SType{TYPEK_S64}));
			return true;
		}

		return false;
	}

	return false;
}

SResolveDecl * PresdeclResolved(SWorkspace * pWork, SAst * pAstIdent)
{
	auto hv = HvFromKey(reinterpret_cast<u64>(pAstIdent));
	SResolveDecl ** ppResdecl = PtLookupImpl(&pWork->hashPastPresdeclResolved, hv, pAstIdent);
	return (ppResdecl) ? *ppResdecl : nullptr;
}

void RegisterResolved(SWorkspace * pWork, SAst * pAstIdent, SResolveDecl * pResdecl)
{
	ASSERT(PresdeclResolved(pWork, pAstIdent) == nullptr);
	auto hv = HvFromKey(reinterpret_cast<u64>(pAstIdent));
	Add(&pWork->hashPastPresdeclResolved, hv, pAstIdent, pResdecl);
}



void EvalConst(SWorkspace * pWork, SAst * pAst, void * pVRet);



void TypeCheck(SWorkspace * pWork, STypeRecurse * pTrec, STypeCheckSwitch * pTcswitch)
{
	SAst * pAst = *pTrec->ppAst;

	switch (pAst->astk)
	{
	case ASTK_Literal:
	case ASTK_Null:
		// Literals of all kinds infer their type from above
		break;

	case ASTK_UninitializedValue: // Never gets a type
		break;
	
	case ASTK_Block:
	case ASTK_EmptyStatement:
		// BB (adrianb) Try and process any un-processed constant definitions in blocks or just let on 
		//  demand processing do its thing? Maybe we should list those declarations in the list and still
		//  process them as we get to them too?
		pAst->tid = pWork->tidVoid;
		break;

	case ASTK_Identifier:
		{
			// Lookup identifier
			// if found, use type
			// if not, find declaration (error if not found or if already on stack)
			//   set pTcsw to switch to that declaration (pushing the current one on the stack).

			auto pAstident = PastCast<SAstIdentifier>(pAst);
			auto pSymt = pTrec->pSymtParent;

			SResolveDecl * pResdecl;
			if (pSymt->symtblk >= SYMTBLK_RegisterAllMic)
			{
				pResdecl = PresdeclLookupSimple(pSymt, pAstident->pChz, RESLK_Local);
			}
			else if (!FTryResolveSymbolWithUsing(pWork, pSymt, pAstident->pChz, pTcswitch, &pResdecl))
			{
				// We have switched to resolving another declaration
				ASSERT(pTcswitch->pDecl);
				break;
			}

			if (!pResdecl)
			{
				ShowErr(pAst->errinfo, "Couldn't find declaration for identifier");
			}

			STypeId tid = pResdecl->pDecl->pAstdecl->tid;
			if (tid.pType == nullptr)
			{
				pTcswitch->pDecl = pResdecl->pDecl;
				break;
			}
			
			pAstident->tid = tid;

			RegisterResolved(pWork, pAstident, pResdecl);
		}
		break;

	case ASTK_Operator:
		{
			// Coerce arguments (where possible replace constants with literal result?)

			auto pAstop = PastCast<SAstOperator>(pAst);
			auto pChzOp = pAstop->pChzOp;
			if (pAstop->pAstLeft == nullptr)
			{
				if (strcmp(pChzOp, "-") == 0)
				{
					TryCoerceLit(pWork, pAstop->pAstRight);
					auto tid = pAstop->pAstRight->tid;
					auto typek = tid.pType->typek;
					if ((typek >= TYPEK_S8 && typek <= TYPEK_S64) || typek == TYPEK_Float)
					{
						pAstop->tid = tid;
					}
				}
				else if (strcmp(pChzOp, "!") == 0)
				{
					TryCoerceLit(pWork, pAstop->pAstRight);
					auto tid = pAstop->pAstRight->tid;
					auto typek = tid.pType->typek;
					if (typek == TYPEK_Bool)
					{
						pAstop->tid = tid;
					}
				}
				else if (strcmp(pChzOp, "--") == 0 || strcmp(pChzOp, "++") == 0)
				{
					// BB (adrianb) Are prefix inc/dec a thing?

					if (!FCanGetAddress(pAstop->pAstRight))
					{
						ShowErr(pAstop->errinfo, "Cannot modify argument using operator %s, "
								"expected variable or struct member", pChzOp);
					}

					auto tid = pAstop->pAstRight->tid;
					ASSERT(tid.pType != nullptr);
					auto typek = tid.pType->typek;

					// BB (adrianb) Verify declaration is not a constant expression?

					if (!FIsInt(typek))
					{
						ShowErr(pAstop->errinfo, "Cannot operate on variable with type %s", 
								StrPrintType(tid.pType).Pchz());
					}

					pAstop->tid = tid;
				}
				else if (strcmp(pChzOp, "*") == 0)
				{
					if (!FCanGetAddress(pAstop->pAstRight))
					{
						ShowErr(pAstop->errinfo, "Cannot modify argument using operator %s, "
								"expected variable or struct member", pChzOp);
					}

					auto tid = pAstop->pAstRight->tid;
					ASSERT(tid.pType != nullptr);
					
					// BB (adrianb) Verify declaration is not a constant expression?

					pAstop->tid = TidPointer(pWork, tid);
				}
				else if (strcmp(pChzOp, "<<") == 0)
				{
					auto tid = pAstop->pAstRight->tid;
					ASSERT(tid.pType != nullptr);

					if (tid.pType->typek != TYPEK_Pointer)
					{
						ShowErr(pAstop->errinfo, "Cannot indirect non-pointer type %s", StrPrintType(tid).Pchz());
					}
					
					// BB (adrianb) Verify declaration is not a constant expression?

					pAstop->tid = PtypeCast<STypePointer>(tid.pType)->tidPointedTo;
				}
			}
			else
			{
				// Run special case operator comparisons

				if (strcmp(pChzOp, ".") == 0)
				{
					// Lookup struct from type on the left (either struct or pointer) and get member type

					if (pAstop->pAstRight->astk != ASTK_Identifier)
					{
						ShowErr(pAstop->pAstRight->errinfo, "Expected identifier to the right of .");
					}

					STypeId tidStruct = pAstop->pAstLeft->tid;
					if (tidStruct.pType)
					{
						// BB (adrianb) Further null checks?
						if (tidStruct.pType->typek == TYPEK_Pointer)
						{
							tidStruct = PtypeCast<STypePointer>(tidStruct.pType)->tidPointedTo;
						}

						bool fConstantOnly = false;
						if (tidStruct.pType->typek == TYPEK_TypeOf)
						{
							tidStruct = TidUnwrap(pAstop->errinfo, tidStruct);
							fConstantOnly = true;
						}

						// Resolve string to builtin structure type
						// BB (adrianb) Do something similar with enums? Arrays or just manually handle those?
						if (tidStruct.pType->typek == TYPEK_String)
						{
							// BB (adrianb) Use top level symbol table directly?
							auto pResdecl = PresdeclLookupSimple(pTrec->pSymtParent, "_StringStruct", RESLK_Local);
							if (pResdecl)
							{
								if (pResdecl->pDecl->pAstdecl->tid.pType == nullptr)
								{
									pTcswitch->pDecl = pResdecl->pDecl;
									ASSERT(pTcswitch->pDecl);
									break;
								}
								else
								{
									// Remove the typeof around the string structure
									tidStruct = TidUnwrap(pAstop->errinfo, pResdecl->pDecl->pAstdecl->tid);
								}
							}
						}

						auto pAstident = PastCast<SAstIdentifier>(pAstop->pAstRight);
						SSymbolTable * pSymtStruct = PsymtStructLookup(pWork, tidStruct);
						ASSERT(pSymtStruct);
						
						// Resolve under existing symbol table

						if (pSymtStruct)
						{
							SResolveDecl * pResdecl;
							if (!FTryResolveSymbolWithUsing(pWork, pSymtStruct, pAstident->pChz, pTcswitch, &pResdecl))
							{
								// We have switched to resolving another declaration
								ASSERT(pTcswitch->pDecl);
								break;
							}

							if (!pResdecl)
							{
								ShowErr(pAstident->errinfo, "Couldn't find member of %s", 
										StrPrintType(tidStruct).Pchz());
							}

							auto pAstdeclResolved = pResdecl->pDecl->pAstdecl;
							if (fConstantOnly && !pAstdeclResolved->fIsConstant)
							{
								ShowErr(pAstop->errinfo, 
										"Can only reference constants when indirecting through non-instance");
							}

							STypeId tid = pAstdeclResolved->tid;
							ASSERT(tid.pType != nullptr);

							pAstop->pAstRight->tid = tid;
							pAstop->tid = tid;

							// BB (adrianb) We want to store using path with this!?

							RegisterResolved(pWork, pAstident, pResdecl);
						}
					}

					if (pAstop->tid.pType == nullptr)
					{
						ShowErr(pAstop->errinfo, "Expected struct or pointer to struct on the left got: %s", 
								StrPrintType(pAstop->pAstLeft->tid.pType).Pchz());
						break;
					}
				}
				else
				{
					for (const auto & bopi : s_aBopiModify)
					{
						if (strcmp(bopi.pChzOp, pChzOp) != 0)
							continue;

						if (!FCanGetAddress(pAstop->pAstLeft))
						{
							ShowErr(pAstop->pAstLeft->errinfo, "Cannot use assigning operator %s on non-memory location",
									pChzOp);
						}

						if (!FTryCoerceOperatorAssign(pWork, pAstop, bopi.grfbopi))
							goto LOperatorDone;
							
						pAstop->tid = pWork->tidVoid;
						goto LOperatorDone;
					}

					for (const auto & bopi : s_aBopi)
					{
						if (strcmp(bopi.pChzOp, pChzOp) == 0)
						{
							TryCoerceOperator(pWork, pAstop, bopi.grfbopi);
							break;
						}
					}
				}
			}

LOperatorDone:
			if (pAstop->tid.pType == nullptr)
			{
				if (pAstop->pAstLeft == nullptr)
				{
					ShowErr(pAstop->errinfo, "Invalid prefix operator %s given type %s, cannot be typechecked.", 
							pChzOp, StrPrintType(pAstop->pAstRight->tid.pType).Pchz());
				}
				else
				{
					ShowErr(pAstop->errinfo, "Invalid operator %s given types %s and %s, cannot be typechecked.", 
							pChzOp, StrPrintType(pAstop->pAstLeft->tid.pType).Pchz(), 
							StrPrintType(pAstop->pAstRight->tid.pType).Pchz());
				}
			}

			// BB (adrianb) Different for math vs logical operators etc.
		}
		break;

	case ASTK_If:
		{
			auto pAstif = PastCast<SAstIf>(pAst);
			if (FCanCoerce(pAstif->pAstCondition, pWork->tidBool))
			{
				Coerce(pWork, &pAstif->pAstCondition, pWork->tidBool);
			}

			pAst->tid = pWork->tidVoid;
		}
		break;

	case ASTK_While:
		{
			auto pAstwhile = PastCast<SAstWhile>(pAst);
			if (FCanCoerce(pAstwhile->pAstCondition, pWork->tidBool))
			{
				Coerce(pWork, &pAstwhile->pAstCondition, pWork->tidBool);
			}

			pAst->tid = pWork->tidVoid;
		}
		break;

	case ASTK_For:
		ASSERT(false);
		// Add identifier declaration based on iterRight, and any manual for declaration
		pAst->tid = pWork->tidVoid;
		break;

	case ASTK_LoopControl:
	case ASTK_Using:
		pAst->tid = pWork->tidVoid;
		break;

	case ASTK_Cast:
		// BB (adrianb) Check that it is possible?
		ASSERT(false);
		// Use type inside pAstType
		break;

	case ASTK_New:
		pAst->tid = TidPointer(pWork, TidUnwrap(pAst->errinfo, PastCast<SAstNew>(pAst)->pAstType->tid));
		break;

	case ASTK_Delete:
	case ASTK_Remove:
		pAst->tid = pWork->tidVoid;
		break;

	case ASTK_Defer: 
		pAst->tid = pWork->tidVoid;
		break;

	case ASTK_Inline:
		pAst->tid = PastCast<SAstInline>(pAst)->pAstExpr->tid;
		break;

	case ASTK_PushContext:
		pAst->tid = pWork->tidVoid;
		break;

	case ASTK_ArrayIndex:
		{
			auto pAstarrayindex = PastCast<SAstArrayIndex>(pAst);

			Coerce(pWork, &pAstarrayindex->pAstIndex, TidEnsure(pWork, SType{TYPEK_S64}));

			auto pAstArray = pAstarrayindex->pAstArray;
			auto pTypeArray = pAstArray->tid.pType;
			if (pTypeArray)
			{
				if (pTypeArray->typek == TYPEK_Array)
				{
					pAst->tid = PtypeCast<STypeArray>(pTypeArray)->tidElement;
				}
				else if (pTypeArray->typek == TYPEK_Pointer)
				{
					pAst->tid = PtypeCast<STypePointer>(pTypeArray)->tidPointedTo;
				}
			}

			if (pAst->tid.pType == nullptr)
			{
				ShowErr(pAst->errinfo, "Expected array or pointer type but found %s", StrPrintType(pTypeArray).Pchz());
			}
		}
		break;

	case ASTK_Call:
		{
			// Coerce arguments, use return type of pAstFunc

			auto pAstcall = PastCast<SAstCall>(pAst);
			auto pTypeproc = PtypeCast<STypeProcedure>(pAstcall->pAstFunc->tid.pType);

			int cTidArg = pTypeproc->cTidArg;
			bool fVararg = false;
			int cArgRequired = cTidArg;
			if (cArgRequired > 0 && pTypeproc->aTidArg[cTidArg - 1].pType->typek == TYPEK_Vararg)
			{
				fVararg = true;
				cArgRequired -= 1;
			}

			if (cArgRequired > pAstcall->arypAstArgs.c)
			{
				ShowErr(pAst->errinfo, "Too few arguments passed to function");
			}
			else if (cArgRequired > pAstcall->arypAstArgs.c && !fVararg)
			{
				ShowErr(pAst->errinfo, "Too many arguments passed to function");
			}

			// BB (adrianb) Named arguments? Default arguments?

			for (int iArg = 0; iArg < cArgRequired; ++iArg)
			{
				Coerce(pWork, &pAstcall->arypAstArgs[iArg], pTypeproc->aTidArg[iArg]);
			}

			// Make sure all other types are passed as arguments

			for (int ipAst = cArgRequired; ipAst < pAstcall->arypAstArgs.c; ++ipAst)
			{
				// BB (adrianb) For built in functions will need to coerce to Any and build an array?
				//  Anything necessary in type checking to do that?

				// Coerce to larger types for C vararg setup

				if (pTypeproc->fUsesCVararg)
				{
					TryCoerceCVararg(pWork, &pAstcall->arypAstArgs[ipAst]);
				}
				else
				{
					TryCoerceLit(pWork, pAstcall->arypAstArgs[ipAst]);
				}
			}
		
			if (pTypeproc->cTidRet > 0)
			{
				ASSERT(pTypeproc->cTidRet == 1);
				pAst->tid = pTypeproc->aTidRet[0];
			}
			else
			{
				pAst->tid = pWork->tidVoid;
			}
		}
		break;

	case ASTK_Return:
		{
			// Coerce arguments to return type of the function we're in context of
			// Use return type of function

			auto pSymtProc = pTrec->pSymtParent;
			for (; pSymtProc; pSymtProc = pSymtProc->pSymtParent)
			{
				if (pSymtProc->symtblk != SYMTBLK_Scope)
					break;
			}

			if (pSymtProc == nullptr || pSymtProc->symtblk != SYMTBLK_Procedure)
			{
				ShowErr(pAst->errinfo, "Cannot return when not inside a procedure.");
			}

			// Get the type from the procedure and coerce our result to it.

			auto pAstret = PastCast<SAstReturn>(pAst);
			auto pTypeproc = PtypeCast<STypeProcedure>(pSymtProc->pAstproc->tid.pType);

			if (pAstret->arypAstRet.c != pTypeproc->cTidRet)
			{
				// BB (adrianb) Clearer way to say this.
				ShowErr(pAst->errinfo, "Expected number of return values do not match");
			}
			else if (pAstret->arypAstRet.c)
			{
				ASSERT(pTypeproc->cTidRet == 1);
				Coerce(pWork, &pAstret->arypAstRet[0], pTypeproc->aTidRet[0]);
				pAst->tid = pTypeproc->aTidRet[0];
			}
			else
			{
				pAst->tid = pWork->tidVoid;
			}
		}
		break;

	case ASTK_DeclareSingle:
		{
			auto pAstdecl = PastCast<SAstDeclareSingle>(pAst);

			// BB (adrianb) Verify using only with struct or * struct. Or eventually ()->* struct.
			
			if (pAstdecl->pChzName && !pAstdecl->fIsConstant && pTrec->pSymtParent->symtblk < SYMTBLK_RegisterAllMic)
			{
				AddDeclaration(pWork, pTrec->pSymtParent, pAstdecl->pChzName, pAstdecl);
			}
			
			// If we don't have an explicit type, coerce literals to best type to best match 
			//  add name with resolved type to the current scope
			//  coerce right side argument to said type
			// Coerce right side to left

			if (pAstdecl->pAstType)
			{
				pAstdecl->tid = TidUnwrap(pAstdecl->pAstType->errinfo, pAstdecl->pAstType->tid);
			}
			else
			{
				ASSERT(pAstdecl->pAstValue);
				pAstdecl->tid = pAstdecl->pAstValue->tid;
				if (pAstdecl->tid.pType == nullptr)
				{
					auto pAstlit = PastCast<SAstLiteral>(pAstdecl->pAstValue);
					STypeId tid = TidFromLiteral(pWork, pAstlit->lit);

					pAstdecl->tid = tid;
					pAstlit->tid = tid;
				}
			}

			if (pAstdecl->pAstValue)
				Coerce(pWork, &pAstdecl->pAstValue, pAstdecl->tid);

			ASSERT(!pAstdecl->pAstValue || pAstdecl->tid == pAstdecl->pAstValue->tid);
		}
		break;

	case ASTK_DeclareMulti:
		ASSERT(false);
		// Same as declare single except for multiple names and need to unpack tuples
		// BB (adrianb) Need to unpack and coerce the structure. How?
		break;

	case ASTK_AssignMulti:
		ASSERT(false);
		break;

	case ASTK_Struct:
		{
			auto pAststruct = PastCast<SAstStruct>(pAst);

			// NOTE (adrianb) We're making a unique type for each struct we see.
			//  If we want structural equivalence or something this will need to change.
			//  Worse, the concept of uniquifying types at this stage will be broken.

			auto pTypestruct = PtAlloc<STypeStruct>(&pWork->pagealloc);
			pTypestruct->typek = TYPEK_Struct;
			pTypestruct->pChzName = pAststruct->pChzName;

			int cMember = 0;
			for (auto pAstDecl : pAststruct->arypAstDecl)
			{
				auto pAstdecl = PastCast<SAstDeclareSingle>(pAstDecl);
				if (!pAstdecl->fIsConstant)
					++cMember;
			}

			pTypestruct->aMember = PtAlloc<STypeStruct::SMember>(&pWork->pagealloc, cMember);
			pTypestruct->cMemberMax = cMember;

			for (auto pAstDecl : pAststruct->arypAstDecl)
			{
				auto pAstdecl = PastCast<SAstDeclareSingle>(pAstDecl); 
				if (!pAstdecl->fIsConstant)
					pTypestruct->aMember[pTypestruct->cMember++].pAstdecl = pAstdecl;
			}

			STypeId tidStruct = { pTypestruct };

			pAststruct->tid = TidWrap(pWork, tidStruct);

			// Assuming struct is immediately inside a constant declaration

			pTrec->pSymtParent->pTypestruct = pTypestruct;

			RegisterStruct(pWork, tidStruct, pTrec->pSymtParent);
		}
		break;

	// case ASTK_Enum
	case ASTK_Procedure:
		{
			// BB (adrianb) Need this one for lambdas?

			// Take all args and return values and build a procedure type

			// BB (adrianb) Check for any polymorphic-ness and store off for type matching and then type checking after.

			// BB (adrianb) Assumed to be constant for the moment. Lambdas will need to do similar work.

			auto pAstproc = PastCast<SAstProcedure>(pAst);

			int cpAstArg = pAstproc->arypAstDeclArg.c;
			STypeId * aTidArg = nullptr;
			bool fUsesCVararg = false;

			if (pAstproc->fIsForeign && cpAstArg > 0)
			{
				auto pAstArgLastType = PastCast<SAstDeclareSingle>(pAstproc->arypAstDeclArg[cpAstArg - 1])->pAstType;
				if (pAstArgLastType && pAstArgLastType->astk == ASTK_TypeVararg)
				{
					fUsesCVararg = true;
					--cpAstArg;
				}
			}

			if (cpAstArg > 0)
			{
				aTidArg = static_cast<STypeId *>(alloca(sizeof(STypeId) * cpAstArg));
				for (auto ipAst : IterCount(cpAstArg))
				{
					auto pAstArgDecl = pAstproc->arypAstDeclArg[ipAst];
					aTidArg[ipAst] = pAstArgDecl->tid;

					if (ipAst < cpAstArg - 1 && aTidArg[ipAst].pType->typek == TYPEK_Vararg)
					{
						ShowErr(pAstArgDecl->errinfo, "Varargs must be last argument in the function");
					}
				}
			}

			int cpAstRet = pAstproc->arypAstDeclRet.c;
			STypeId * aTidRet = nullptr;

			if (cpAstRet > 0)
			{
				aTidRet = static_cast<STypeId *>(alloca(sizeof(STypeId) * cpAstRet));
				for (auto ipAst : IterCount(cpAstRet))
				{
					// NOTE (adrianb) We only type checked the type (required).

					auto pAstdecl = PastCast<SAstDeclareSingle>(pAstproc->arypAstDeclRet[ipAst]);
					ASSERT(pAstdecl->tid.pType == nullptr);
					aTidRet[ipAst] = TidUnwrap(pAstdecl->pAstType->errinfo, pAstdecl->pAstType->tid);
				}
			}

			STypeProcedure typeproc = {};
			typeproc.typek = TYPEK_Procedure;
			typeproc.fUsesCVararg = fUsesCVararg;
			typeproc.aTidArg = aTidArg;
			typeproc.aTidRet = aTidRet;
			typeproc.cTidArg = cpAstArg;
			typeproc.cTidRet = cpAstRet;

			pAst->tid = TidEnsure(pWork, &typeproc);

			// BB (adrianb) Do I need to register declarations at all?
		}
		break;
	
	case ASTK_TypeDefinition:
		// BB (adrianb) Use a type wrapper around the previous setup?
		ASSERT(false);
		break;

	case ASTK_TypePointer:
		{
			// BB (adrianb) All this wrap/unwrap junk is annoying. Better way to distinguish
			//  type used as value and used as type?

			auto pAtypeptr = PastCast<SAstTypePointer>(pAst);
			pAst->tid = TidWrap(pWork, TidPointer(pWork, TidUnwrap(pAst->errinfo, pAtypeptr->pAstTypeInner->tid)));
		}
		break;

	case ASTK_TypeArray:
		{
			auto pAtypearray = PastCast<SAstTypeArray>(pAst);

			STypeArray typearray = {};
			typearray.typek = TYPEK_Array;
			typearray.cSizeFixed = -1;
			typearray.tidElement = TidUnwrap(pAtypearray->pAstTypeInner->errinfo,
											 pAtypearray->pAstTypeInner->tid);

			if (pAtypearray->pAstSize)
			{
				// BB (adrianb) Specially detect variable sized arrays?
				Coerce(pWork, &pAtypearray->pAstSize, TidEnsure(pWork, {TYPEK_S64}));

				auto pAstSize = pAtypearray->pAstSize;
				ASSERT(pAstSize->tid.pType->typek == TYPEK_S64);

				s64 n = 0;
				EvalConst(pWork, pAtypearray->pAstSize, reinterpret_cast<u8 *>(&n));
				ASSERT(n >= 0);

				typearray.cSizeFixed = n;
			}

			// Manually performing TidEnsure here, assumes that array contains sum total info and we can patch struct
			//  in as needed

			STypeId tidArray = TidEnsure(pWork, typearray);
			pAtypearray->tid = TidWrap(pWork, tidArray);

			auto pTypearray = static_cast<STypeArray *>(const_cast<SType *>(tidArray.pType));

			if (pTypearray->pTypestruct != nullptr)
				return;

			// Setup struct and symbol table, without filling in full AST

			SSymbolTable * pSymtArray = PsymtCreate(SYMTBLK_Struct, pWork, nullptr);

			// BB (adrianb) Not filling in actual full AST.

			auto pAstdeclA = PastCreate<SAstDeclareSingle>(pWork, SErrorInfo{});
			pAstdeclA->tid = TidPointer(pWork, typearray.tidElement);
			pAstdeclA->pChzName = "a";

			AddDeclaration(pWork, pSymtArray, "a", pAstdeclA);

			auto pAstdeclC = PastCreate<SAstDeclareSingle>(pWork, SErrorInfo{});
			pAstdeclC->tid = pWork->tidU32; // BB (adrianb) U64 to support >4GiB [] u8?
			pAstdeclC->pChzName = "c";

			if (typearray.cSizeFixed >= 0)
			{
				pAstdeclC->fIsConstant = true;
				auto pAstlitSize = PastCreate<SAstLiteral>(pWork, SErrorInfo{});
				pAstlitSize->tid = pAstdeclC->tid;
				pAstlitSize->lit.litk = LITK_Int;
				pAstlitSize->lit.n = typearray.cSizeFixed;

				pAstdeclC->pAstValue = pAstlitSize;
			}

			AddDeclaration(pWork, pSymtArray, "c", pAstdeclC);

			auto pTypestruct = PtAlloc<STypeStruct>(&pWork->pagealloc);
			pTypestruct->typek = TYPEK_Struct;
			pTypestruct->pChzName = "_array";

			pTypestruct->cMember = 2;
			pTypestruct->cMemberMax = 2;

			if (typearray.cSizeFixed < 0)
			{
				pTypestruct->aMember = PtAlloc<STypeStruct::SMember>(&pWork->pagealloc, 2);
				pTypestruct->aMember[0].pAstdecl = pAstdeclA;
				pTypestruct->aMember[1].pAstdecl = pAstdeclC;
			}

			pTypearray->pTypestruct = pTypestruct;
			
			// Assuming struct is immediately inside a constant declaration
			// BB (adrianb) Need this?

			pSymtArray->pTypestruct = pTypestruct;

			// Manually RegisterStruct without adding it to list of global structs (anonymous)

			Add(&pWork->hashTidPsymtStruct, HvFromKey(tidArray), tidArray, pSymtArray);

			pAtypearray->tid = TidWrap(pWork, tidArray);
		}
		break;
		
	case ASTK_TypeProcedure:
	case ASTK_TypePolymorphic:
		ShowErr(pAst->errinfo, "NYI");
		break;

	case ASTK_TypeVararg:
		{
			pAst->tid = TidWrap(pWork, TidEnsure(pWork, SType{TYPEK_Vararg}));
		}
		break;

	// case ASTK_ImportDirective:
	
	case ASTK_RunDirective:
		{
			// BB (adrianb) Should we be trying to support just literals here?
			auto pAstrun = PastCast<SAstRunDirective>(pAst);
			pAstrun->tid = pAstrun->pAstExpr->tid;
			// BB (adrianb) Move this to function, how often is it duplicated?
			if (pAstrun->tid.pType == nullptr)
			{
				auto pAstlit = PastCast<SAstLiteral>(pAstrun->pAstExpr);
				STypeId tid = TidFromLiteral(pWork, pAstlit->lit);

				pAstrun->tid = tid;
				pAstlit->tid = tid;
			}
		}
		break;

	// case ASTK_ForeignLibraryDirective:

	default:
		ASSERTCHZ(false, "Could typecheck node of type %s(%d)", PchzFromAstk(pAst->astk), pAst->astk);
		break;
	}

	if (pTcswitch->pDecl == nullptr && pAst->astk != ASTK_Literal && pAst->astk != ASTK_Null && 
		pAst->tid.pType == nullptr)
	{
		ShowErr(pAst->errinfo, "Couldn't compute type");
	}
}

struct STypeCheckEntry
{
	SDeclaration * pDecl;
};

void TypeCheckAll(SWorkspace * pWork)
{
	// Only variable values inside function scopes must be type checked in order. These can be out of order:
	//  1. All top level declarations (includes global variables).
	//  2. All declarations inside a struct (so procedure in struct can know type of later defined type).
	//  3. Constant declarations (e.g. a :: 5) at all scopes.
	// This throws some kinks into how typechecking would generally be done (recurse assuming everything is findable).
	//  1. Resolving an identifer requires either switching or recursing to a new unresolved case.
	//     Solution: we flatten the AST for all recursion so we can easily switch threads of type checking.
	//  2. Function bodies need to be processable after function type is established to allow for recursive functions.
	//     Solution: we establish the type of a function before recursing into the body.
	//  3. Structs members need to be processable after basic struct type is established to allow for recursive 
	//     data structures (e.g. A pointing to A).
	//     Solution: we always create struct types by name (may need name mangling to account for scoping). Seems like
	//      a better solution must exist.

	// Todo: 
	// The using keywork also makes things pretty complicated. If you have a using at any scope when you're 
	//  resolving an identifier you have to trace through to that scope. I think this can lead to cyclical symbol
	//  table references. For constants it's possible this should be ok.
	//  E.g. when resolving D for d definition in "using a : A; A :: struct { d : D; } D : struct {}". D could come 
	//  from inside A but we are processing A. Is the right way to solve this ignoring recursion into any scope we've 
	//  already seen (e.g. you could potentially see constants using from multiple places, or loop back on yourself).
	//  For non-constants this should be an error as it's ambiguous how to reach a particular term.
	
	// Recurse AST, build symbol tables, and register all out of order declarations
			
	for (SModule * pModule : IterPointer(pWork->aryModule))
	{
		SRecurseCtx recx = { pWork, pModule, nullptr, &pWork->symtRoot };
	
		for (auto ppAst : IterPointer(pModule->pAstblockRoot->arypAst))
		{
			RecurseTypeCheckDecl(recx, ppAst);
		}
	}

	// Run type checking in preprecursion order

	SArray<SDeclaration *> arypResolveWait = {};

	// Type check everything, allowing for new symbol tables to be added in flight
    
    for (int ipSymt = 0; ipSymt < pWork->arypSymtAll.c; ++ipSymt)
	{
		auto pSymt = pWork->arypSymtAll[ipSymt];
		for (int ipResdecl : IterCount(pSymt->arypResdecl.c))
		{
			auto pDecl = pSymt->arypResdecl[ipResdecl]->pDecl;
			for (;;)
			{
				// Empty queue of waiting resolves after we've finished this one

				if (pDecl->iTrecCur >= pDecl->aryTrec.c)
				{
					if (arypResolveWait.c == 0)
						break;

					pDecl = Tail(&arypResolveWait);
					Pop(&arypResolveWait);
					continue;
				}

				STypeCheckSwitch tcswitch = {};
				TypeCheck(pWork, &pDecl->aryTrec[pDecl->iTrecCur], &tcswitch);
                
				if (tcswitch.pDecl)
				{
					for (SDeclaration * pResolveWait : arypResolveWait)
					{
						if (pResolveWait == tcswitch.pDecl)
						{
							// BB (adrianb) Display the full contents of the cycle (and where the references were from)?
							ShowErr(pResolveWait->pAstdecl->errinfo, "Cycle asking for symbol resolution");
						}
					}

					Append(&arypResolveWait, pDecl);
					pDecl = tcswitch.pDecl;
				}
				else
				{
					pDecl->iTrecCur++;
				}
			}
		}
	}

	Destroy(&arypResolveWait);
}



void EnsureTypeSize(const SType * pTypeIn);
inline void EnsureTypeSize(STypeId tid)
{
	EnsureTypeSize(tid.pType);
}

u32 CbAlignOf(STypeId tid)
{
	EnsureTypeSize(tid);
	return tid.pType->cBAlign;
}

u32 CbSizeOf(STypeId tid)
{
	EnsureTypeSize(tid);
	return tid.pType->cB;
}

int CbBasicSize(TYPEK typek)
{
	// BB (adrianb) Changes based on 32/64-bitness of pointers.

	switch (typek)
	{
		case TYPEK_Bool: return 1;
		case TYPEK_S8: return 1;
		case TYPEK_S16: return 2;
		case TYPEK_S32: return 4;
		case TYPEK_S64: return 8;
		case TYPEK_U8: return 1;
		case TYPEK_U16: return 2;
		case TYPEK_U32: return 4;
		case TYPEK_U64: return 8;
		case TYPEK_Float: return 4;
		case TYPEK_Double: return 8;
		case TYPEK_Pointer: return 8;
		default: return -1;
	}
}

template <class T>
inline T max(T a, T b)
{
	return a < b ? b : a;
}

inline u32 CbAlign(u32 cB, u32 cBAlign)
{
	return (cB + cBAlign - 1) & ~(cBAlign - 1);
}

void EnsureTypeSize(const SType * pTypeIn)
{
	auto pType = const_cast<SType *>(pTypeIn);
	if (pType->fSizeComputed)
		return;

	pType->fSizeComputed = true;

    int cB = CbBasicSize(pType->typek);
	if (cB >= 0)
	{
		pType->cBAlign = pType->cB = cB;
		return;
	}

	switch (pType->typek)
	{
	case TYPEK_String:
	case TYPEK_Struct:
		{
LStruct:
			auto pTypestruct = Ptypestruct(pType);
			u32 cB = 0;
			u32 cBAlignMax = 1;
			for (int iMember : IterCount(pTypestruct->cMember))
			{
				STypeId tidMember = pTypestruct->aMember[iMember].pAstdecl->tid;
				pTypestruct->aMember[iMember].iBOffset = cB;

				u32 cBAlign = CbAlignOf(tidMember);
				cBAlignMax = max(cBAlignMax, cBAlign);
				cB = CbAlign(cB, cBAlign);
				cB += CbSizeOf(tidMember);
			}

			cB = CbAlign(cB, cBAlignMax);

			pType->cBAlign = cBAlignMax;
			pType->cB = cB;
		}
		break;

	case TYPEK_Array:
		{
			auto pTypearray = PtypeCast<STypeArray>(pType);
			if (pTypearray->cSizeFixed < 0)
				goto LStruct;

			ASSERT(!pTypearray->fSoa);

			pType->cBAlign = CbAlignOf(pTypearray->tidElement);
			pType->cB = pTypearray->cSizeFixed * CbSizeOf(pTypearray->tidElement);
		}
		break;

	//case TYPEK_Array: return 1;
	//case TYPEK_Procedure: return 1;
	//case TYPEK_Any: return 1;
	//case TYPEK_Enum: return 1;
	//case TYPEK_TypeOf: return 1;
	//case TYPEK_Vararg: return 1;

	default:
		ASSERTCHZ(false, "Can't compute size/alignment for type %s", StrPrintType(pType).Pchz());
		break;
	}
}



struct SValue
{
	STypeId tid;
	void * pV;
};

struct SEvalCtx
{
	SWorkspace * pWork;
	SHash<SAstDeclareSingle *, void *> hashPastdeclPv;
	// Hash declaration to memory
	// Stack of memory
	// Stack of call records
};

void Init(SEvalCtx * pEval, SWorkspace * pWork)
{
	pEval->pWork = pWork;
}

void Destroy(SEvalCtx * pEval)
{
	Destroy(&pEval->hashPastdeclPv);
}

using PFNEVALCONSTBINARY = bool (*)(SValue valLeft, SValue valRight, SValue * pValRet);

struct SEvalConstBinaryOperator
{
	const char * pChzOp;

	PFNEVALCONSTBINARY pfnecb;

	// bools? 
};

bool FTryAddValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_S8: *static_cast<s8 *>(pVRet) = *static_cast<s8 *>(pV0) + *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<s16 *>(pVRet) = *static_cast<s16 *>(pV0) + *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<s32 *>(pVRet) = *static_cast<s32 *>(pV0) + *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<s64 *>(pVRet) = *static_cast<s64 *>(pV0) + *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<u8 *>(pVRet) = *static_cast<u8 *>(pV0) + *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<u16 *>(pVRet) = *static_cast<u16 *>(pV0) + *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<u32 *>(pVRet) = *static_cast<u32 *>(pV0) + *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<u64 *>(pVRet) = *static_cast<u64 *>(pV0) + *static_cast<u64 *>(pV1); return true;
	case TYPEK_Float: *static_cast<float *>(pVRet) = *static_cast<float *>(pV0) + *static_cast<float *>(pV1); return true;
	case TYPEK_Double: *static_cast<double *>(pVRet) = *static_cast<double *>(pV0) + *static_cast<double *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator + with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

bool FTrySubValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_S8: *static_cast<s8 *>(pVRet) = *static_cast<s8 *>(pV0) - *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<s16 *>(pVRet) = *static_cast<s16 *>(pV0) - *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<s32 *>(pVRet) = *static_cast<s32 *>(pV0) - *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<s64 *>(pVRet) = *static_cast<s64 *>(pV0) - *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<u8 *>(pVRet) = *static_cast<u8 *>(pV0) - *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<u16 *>(pVRet) = *static_cast<u16 *>(pV0) - *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<u32 *>(pVRet) = *static_cast<u32 *>(pV0) - *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<u64 *>(pVRet) = *static_cast<u64 *>(pV0) - *static_cast<u64 *>(pV1); return true;
	case TYPEK_Float: *static_cast<float *>(pVRet) = *static_cast<float *>(pV0) - *static_cast<float *>(pV1); return true;
	case TYPEK_Double: *static_cast<double *>(pVRet) = *static_cast<double *>(pV0) - *static_cast<double *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator - with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

bool FTryMulValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_S8: *static_cast<s8 *>(pVRet) = *static_cast<s8 *>(pV0) * *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<s16 *>(pVRet) = *static_cast<s16 *>(pV0) * *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<s32 *>(pVRet) = *static_cast<s32 *>(pV0) * *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<s64 *>(pVRet) = *static_cast<s64 *>(pV0) * *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<u8 *>(pVRet) = *static_cast<u8 *>(pV0) * *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<u16 *>(pVRet) = *static_cast<u16 *>(pV0) * *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<u32 *>(pVRet) = *static_cast<u32 *>(pV0) * *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<u64 *>(pVRet) = *static_cast<u64 *>(pV0) * *static_cast<u64 *>(pV1); return true;
	case TYPEK_Float: *static_cast<float *>(pVRet) = *static_cast<float *>(pV0) * *static_cast<float *>(pV1); return true;
	case TYPEK_Double: *static_cast<double *>(pVRet) = *static_cast<double *>(pV0) * *static_cast<double *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator * with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

bool FTryDivValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_S8: *static_cast<s8 *>(pVRet) = *static_cast<s8 *>(pV0) / *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<s16 *>(pVRet) = *static_cast<s16 *>(pV0) / *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<s32 *>(pVRet) = *static_cast<s32 *>(pV0) / *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<s64 *>(pVRet) = *static_cast<s64 *>(pV0) / *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<u8 *>(pVRet) = *static_cast<u8 *>(pV0) / *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<u16 *>(pVRet) = *static_cast<u16 *>(pV0) / *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<u32 *>(pVRet) = *static_cast<u32 *>(pV0) / *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<u64 *>(pVRet) = *static_cast<u64 *>(pV0) / *static_cast<u64 *>(pV1); return true;
	case TYPEK_Float: *static_cast<float *>(pVRet) = *static_cast<float *>(pV0) / *static_cast<float *>(pV1); return true;
	case TYPEK_Double: *static_cast<double *>(pVRet) = *static_cast<double *>(pV0) / *static_cast<double *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator / with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

bool FTryRemValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_S8: *static_cast<s8 *>(pVRet) = *static_cast<s8 *>(pV0) % *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<s16 *>(pVRet) = *static_cast<s16 *>(pV0) % *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<s32 *>(pVRet) = *static_cast<s32 *>(pV0) % *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<s64 *>(pVRet) = *static_cast<s64 *>(pV0) % *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<u8 *>(pVRet) = *static_cast<u8 *>(pV0) % *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<u16 *>(pVRet) = *static_cast<u16 *>(pV0) % *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<u32 *>(pVRet) = *static_cast<u32 *>(pV0) % *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<u64 *>(pVRet) = *static_cast<u64 *>(pV0) % *static_cast<u64 *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator %% with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

bool FTryCmpEqValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_Bool: *static_cast<bool *>(pVRet) = *static_cast<bool *>(pV0) == *static_cast<bool *>(pV1); return true;
	case TYPEK_S8: *static_cast<bool *>(pVRet) = *static_cast<s8 *>(pV0) == *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<bool *>(pVRet) = *static_cast<s16 *>(pV0) == *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<bool *>(pVRet) = *static_cast<s32 *>(pV0) == *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<bool *>(pVRet) = *static_cast<s64 *>(pV0) == *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<bool *>(pVRet) = *static_cast<u8 *>(pV0) == *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<bool *>(pVRet) = *static_cast<u16 *>(pV0) == *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<bool *>(pVRet) = *static_cast<u32 *>(pV0) == *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<bool *>(pVRet) = *static_cast<u64 *>(pV0) == *static_cast<u64 *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator == with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

bool FTryCmpNeqValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_Bool: *static_cast<bool *>(pVRet) = *static_cast<bool *>(pV0) != *static_cast<bool *>(pV1); return true;
	case TYPEK_S8: *static_cast<bool *>(pVRet) = *static_cast<s8 *>(pV0) != *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<bool *>(pVRet) = *static_cast<s16 *>(pV0) != *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<bool *>(pVRet) = *static_cast<s32 *>(pV0) != *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<bool *>(pVRet) = *static_cast<s64 *>(pV0) != *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<bool *>(pVRet) = *static_cast<u8 *>(pV0) != *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<bool *>(pVRet) = *static_cast<u16 *>(pV0) != *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<bool *>(pVRet) = *static_cast<u32 *>(pV0) != *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<bool *>(pVRet) = *static_cast<u64 *>(pV0) != *static_cast<u64 *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator != with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

bool FTryCmpLTValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_S8: *static_cast<bool *>(pVRet) = *static_cast<s8 *>(pV0) < *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<bool *>(pVRet) = *static_cast<s16 *>(pV0) < *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<bool *>(pVRet) = *static_cast<s32 *>(pV0) < *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<bool *>(pVRet) = *static_cast<s64 *>(pV0) < *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<bool *>(pVRet) = *static_cast<u8 *>(pV0) < *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<bool *>(pVRet) = *static_cast<u16 *>(pV0) < *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<bool *>(pVRet) = *static_cast<u32 *>(pV0) < *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<bool *>(pVRet) = *static_cast<u64 *>(pV0) < *static_cast<u64 *>(pV1); return true;
	case TYPEK_Float: *static_cast<bool *>(pVRet) = *static_cast<float *>(pV0) < *static_cast<float *>(pV1); return true;
	case TYPEK_Double: *static_cast<bool *>(pVRet) = *static_cast<double *>(pV0) < *static_cast<double *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator < with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

bool FTryCmpLeqValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_S8: *static_cast<bool *>(pVRet) = *static_cast<s8 *>(pV0) <= *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<bool *>(pVRet) = *static_cast<s16 *>(pV0) <= *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<bool *>(pVRet) = *static_cast<s32 *>(pV0) <= *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<bool *>(pVRet) = *static_cast<s64 *>(pV0) <= *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<bool *>(pVRet) = *static_cast<u8 *>(pV0) <= *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<bool *>(pVRet) = *static_cast<u16 *>(pV0) <= *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<bool *>(pVRet) = *static_cast<u32 *>(pV0) <= *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<bool *>(pVRet) = *static_cast<u64 *>(pV0) <= *static_cast<u64 *>(pV1); return true;
	case TYPEK_Float: *static_cast<bool *>(pVRet) = *static_cast<float *>(pV0) <= *static_cast<float *>(pV1); return true;
	case TYPEK_Double: *static_cast<bool *>(pVRet) = *static_cast<double *>(pV0) <= *static_cast<double *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator <= with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

bool FTryCmpGTValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_S8: *static_cast<bool *>(pVRet) = *static_cast<s8 *>(pV0) > *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<bool *>(pVRet) = *static_cast<s16 *>(pV0) > *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<bool *>(pVRet) = *static_cast<s32 *>(pV0) > *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<bool *>(pVRet) = *static_cast<s64 *>(pV0) > *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<bool *>(pVRet) = *static_cast<u8 *>(pV0) > *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<bool *>(pVRet) = *static_cast<u16 *>(pV0) > *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<bool *>(pVRet) = *static_cast<u32 *>(pV0) > *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<bool *>(pVRet) = *static_cast<u64 *>(pV0) > *static_cast<u64 *>(pV1); return true;
	case TYPEK_Float: *static_cast<bool *>(pVRet) = *static_cast<float *>(pV0) > *static_cast<float *>(pV1); return true;
	case TYPEK_Double: *static_cast<bool *>(pVRet) = *static_cast<double *>(pV0) > *static_cast<double *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator > with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

bool FTryCmpGeqValue(SValue val0, SValue val1, SValue * pValRet)
{
	void * pV0 = val0.pV;
	void * pV1 = val1.pV;
	void * pVRet = pValRet->pV;
	TYPEK typek = val0.tid.pType->typek;
	switch (typek)
	{
	case TYPEK_S8: *static_cast<bool *>(pVRet) = *static_cast<s8 *>(pV0) >= *static_cast<s8 *>(pV1); return true;
	case TYPEK_S16: *static_cast<bool *>(pVRet) = *static_cast<s16 *>(pV0) >= *static_cast<s16 *>(pV1); return true;
	case TYPEK_S32: *static_cast<bool *>(pVRet) = *static_cast<s32 *>(pV0) >= *static_cast<s32 *>(pV1); return true;
	case TYPEK_S64: *static_cast<bool *>(pVRet) = *static_cast<s64 *>(pV0) >= *static_cast<s64 *>(pV1); return true;
	case TYPEK_U8: *static_cast<bool *>(pVRet) = *static_cast<u8 *>(pV0) >= *static_cast<u8 *>(pV1); return true;
	case TYPEK_U16: *static_cast<bool *>(pVRet) = *static_cast<u16 *>(pV0) >= *static_cast<u16 *>(pV1); return true;
	case TYPEK_U32: *static_cast<bool *>(pVRet) = *static_cast<u32 *>(pV0) >= *static_cast<u32 *>(pV1); return true;
	case TYPEK_U64: *static_cast<bool *>(pVRet) = *static_cast<u64 *>(pV0) >= *static_cast<u64 *>(pV1); return true;
	case TYPEK_Float: *static_cast<bool *>(pVRet) = *static_cast<float *>(pV0) >= *static_cast<float *>(pV1); return true;
	case TYPEK_Double: *static_cast<bool *>(pVRet) = *static_cast<double *>(pV0) >= *static_cast<double *>(pV1); return true;
	default: ASSERTCHZ(false, "Can't do operator >= with type %s", StrPrintType(val0.tid).Pchz()); return false;
	}
}

// BB (adrianb) Embed these in normal operator array?
static const SEvalConstBinaryOperator s_aEcbop[] =
{
	// BB (adrianb) Unordered (either arg NaN) vs Ordered. Which to choose?
	{ "<", FTryCmpLTValue },
	{ ">", FTryCmpGTValue },
	{ "<=", FTryCmpLeqValue },
	{ ">=", FTryCmpGeqValue },

	{ "==", FTryCmpEqValue },
	{ "!=", FTryCmpNeqValue },

	{ "+", FTryAddValue },
	{ "-", FTrySubValue },
	{ "*", FTryMulValue },
	{ "/", FTryDivValue },
	{ "%", FTryRemValue },
};

void EvalCode(SEvalCtx * pEval, SAst * pAst, void * pVRet);

bool FTryEvalBinaryOperator(SEvalCtx * pEval, const SEvalConstBinaryOperator & ecbop, SAst * pAstLeft, SAst * pAstRight,
						STypeId tidDst, void * pVDst)
{
	void * pVLeft = alloca(CbSizeOf(pAstLeft->tid));
	void * pVRight = alloca(CbSizeOf(pAstRight->tid));

	EvalCode(pEval, pAstLeft, static_cast<u8 *>(pVLeft));
	EvalCode(pEval, pAstRight, static_cast<u8 *>(pVRight));

	SValue valRet = { tidDst, pVDst };
	return ecbop.pfnecb({pAstLeft->tid, pVLeft}, {pAstRight->tid, pVRight}, &valRet);
}

void EvalConst(SWorkspace * pWork, SAst * pAst, void * pVRet)
{
	// BB (adrianb) We should cache the result so we don't have to recompute the same constant again?
	//  And register single identifiers with the declaring AST instead.

	SEvalCtx eval = {};
	Init(&eval, pWork);
	defer { Destroy(&eval); };

	EvalCode(&eval, pAst, pVRet);
}

void EvalDefaultValue(SWorkspace * pWork, STypeId tid, u8 * pBRet)
{
	TYPEK typek = tid.pType->typek;

	switch (typek)
	{
	case TYPEK_Bool:
	case TYPEK_S8:
	case TYPEK_S16:
	case TYPEK_S32:
	case TYPEK_S64:
	case TYPEK_U8:
	case TYPEK_U16:
	case TYPEK_U32:
	case TYPEK_U64:
	case TYPEK_Float:
	case TYPEK_Double:
	case TYPEK_String:
	case TYPEK_Pointer:
		memset(pBRet, 0, CbSizeOf(tid));
		break;
		
	case TYPEK_Array:
		{
			auto pTypearray = PtypeCast<STypeArray>(tid.pType);
			ASSERT(!pTypearray->fSoa && pTypearray->cSizeFixed >= 0);
			int cBMember = CbSizeOf(pTypearray->tidElement);
			for (int i : IterCount(pTypearray->cSizeFixed))
			{
				EvalDefaultValue(pWork, pTypearray->tidElement, pBRet + i * cBMember);
			}
		}
		break;

	// TYPEK_Procedure
	
	case TYPEK_Struct:
		{
			auto pTypestruct = PtypeCast<STypeStruct>(tid.pType);
			for (int iMember : IterCount(pTypestruct->cMember))
			{
				auto pMember = &pTypestruct->aMember[iMember];
				if (pMember->pAstdecl->pAstValue)
					EvalConst(pWork, pMember->pAstdecl->pAstValue, pBRet + pMember->iBOffset);
				else
					EvalDefaultValue(pWork, pMember->pAstdecl->tid, pBRet + pMember->iBOffset);
			}
		}
		break;

	// TYPEK_Any
	// TYPEK_Enum
	// TYPEK_TypeOf
	// TYPEK_Vararg

	default:
		ASSERTCHZ(false, "Can't set default value for %s(%d)", PchzFromTypek(typek), typek);
		memset(pBRet, 0, CbSizeOf(tid));
		break;
	}
}

void EvalCode(SEvalCtx * pEval, SAst * pAst, void * pVRet)
{
	SWorkspace * pWork = pEval->pWork;

	// BB (adrianb) This is a lot of code to support simple expression constants.
	//  If its not a literal, just replace with implicity wrapping code inside #run?

	ASTK astk = pAst->astk;
	switch (astk)
	{
	case ASTK_Literal:
		{
			const SLiteral & lit = PastCast<SAstLiteral>(pAst)->lit;
			switch (lit.litk)
			{
			case LITK_Bool:
			case LITK_Int:
				switch (pAst->tid.pType->typek)
				{
				case TYPEK_Bool: *static_cast<bool *>(pVRet) = lit.n != 0; return;
				case TYPEK_S8: *static_cast<s8 *>(pVRet) = lit.n; return;
				case TYPEK_S16: *static_cast<s16 *>(pVRet) = lit.n; return;
				case TYPEK_S32: *static_cast<s32 *>(pVRet) = lit.n; return;
				case TYPEK_S64: *static_cast<s64 *>(pVRet) = lit.n; return;
				case TYPEK_U8: *static_cast<u8 *>(pVRet) = lit.n; return;
				case TYPEK_U16: *static_cast<u16 *>(pVRet) = lit.n; return;
				case TYPEK_U32: *static_cast<u32 *>(pVRet) = lit.n; return;
				case TYPEK_U64: *static_cast<u64 *>(pVRet) = lit.n; return;
				default: ASSERT(false); return;
				}

			case LITK_Float:
				// BB (adrianb) Can't use APFloat from C-api. Do I care? I want explicit type anyways.
				switch (pAst->tid.pType->typek)
				{
				case TYPEK_Float: *static_cast<float *>(pVRet) = lit.g; return;
				case TYPEK_Double: *static_cast<double *>(pVRet) = lit.g; return;
				default: ASSERT(false); return;
				}

			case LITK_String:
				switch (pAst->tid.pType->typek)
				{
				case TYPEK_Pointer: *static_cast<const char **>(pVRet) = lit.pChz; return;
				case TYPEK_String: 
				{
					auto pStrimg = static_cast<SStringImage *>(pVRet);
					pStrimg->pCh = reinterpret_cast<u8 *>(const_cast<char *>(lit.pChz));
					pStrimg->cCh = strlen(lit.pChz);
					return;
				}
				default: ASSERT(false); return;
				}

			default:
				ASSERT(false);
				return;
			}
		}
		break;

	case ASTK_Null:
		{
			ASSERT(pAst->tid.pType->typek == TYPEK_Pointer);
			*static_cast<void **>(pVRet) = nullptr;
			return;
		}

	// BB (adrianb) 90% of this is the same as normal generation. Generalize somehow?

	case ASTK_Identifier:
		{
			// This must be a variable if we are evaluating it directly

			auto pResdecl = PresdeclResolved(pWork, pAst);
			auto pAstdecl = pResdecl->pDecl->pAstdecl;
			if (pAstdecl->fIsConstant)
			{
				// BB (adrianb) Does it make sense to evaluate a new constant here?

				EvalCode(pEval, pAstdecl->pAstValue, pVRet);
				return;
			}

			void ** ppV = PtLookupImpl(&pEval->hashPastdeclPv, HvFromKey(reinterpret_cast<u64>(pAstdecl)), pAstdecl);

			if (!ppV)
			{
				ShowErr(pAst->errinfo, "Expected a reference to a constant or variable defined in run code");
				return;
			}

			memcpy(pVRet, static_cast<const void *>(*ppV), CbSizeOf(pAstdecl->tid));
		}

	case ASTK_Operator:
		{
			auto pAstop = PastCast<SAstOperator>(pAst);
			auto pChzOp = pAstop->pChzOp;

			// BB (adrianb) Table drive these or it will get insane!

			if (pAstop->pAstLeft == nullptr)
			{
				auto pAstRight = pAstop->pAstRight;
				auto tidRight = pAstRight->tid;

				if (strcmp(pChzOp, "-") == 0)
				{
					void * pVRight = alloca(CbSizeOf(tidRight));
					EvalCode(pEval, pAstRight, pVRight);

					switch (pAst->tid.pType->typek)
					{
					case TYPEK_S8: *static_cast<s8 *>(pVRet) = - *static_cast<s8 *>(pVRight); return;
					case TYPEK_S16: *static_cast<s16 *>(pVRet) = - *static_cast<s16 *>(pVRight); return;
					case TYPEK_S32: *static_cast<s32 *>(pVRet) = - *static_cast<s32 *>(pVRight); return;
					case TYPEK_S64: *static_cast<s64 *>(pVRet) = - *static_cast<s64 *>(pVRight); return;
					case TYPEK_U8: *static_cast<u8 *>(pVRet) = - *static_cast<u8 *>(pVRight); return;
					case TYPEK_U16: *static_cast<u16 *>(pVRet) = - *static_cast<u16 *>(pVRight); return;
					case TYPEK_U32: *static_cast<u32 *>(pVRet) = - *static_cast<u32 *>(pVRight); return;
					case TYPEK_U64: *static_cast<u64 *>(pVRet) = - *static_cast<u64 *>(pVRight); return;
					case TYPEK_Float: *static_cast<float *>(pVRet) = - *static_cast<float *>(pVRight); return;
					case TYPEK_Double: *static_cast<double *>(pVRet) = - *static_cast<double *>(pVRight); return;
					default: ASSERT(false); return;
					}
				}
				else if (strcmp(pChzOp, "!") == 0)
				{
					void * pVRight = alloca(CbSizeOf(tidRight));
					EvalCode(pEval, pAstRight, pVRight);

					ASSERT(pAst->tid.pType->typek == TYPEK_Bool);
					*static_cast<bool *>(pVRet) = ! *static_cast<bool *>(pVRight); return;
				}
				
				ShowErr(pAstop->errinfo, "Unary operator %s constant with type %s NYI", 
						pChzOp, StrPrintType(tidRight).Pchz());
				return;
			}
			else
			{
				auto pAstLeft = pAstop->pAstLeft;
				auto pAstRight = pAstop->pAstRight;

				for (const auto & ecbop : s_aEcbop)
				{
					if (strcmp(ecbop.pChzOp, pChzOp) == 0)
					{
						if (!FTryEvalBinaryOperator(pEval, ecbop, pAstLeft, pAstRight, pAst->tid, pVRet))
						{
							ShowErr(pAstop->errinfo, "Don't know how to perform constant operator for types %s and %s",
									StrPrintType(pAstLeft->tid).Pchz(), StrPrintType(pAstRight->tid).Pchz());
						}
						return;
					}
				}

				if (strcmp(pChzOp, "and") == 0)
				{
					void * pVLeft = alloca(CbSizeOf(pAstLeft->tid));
					EvalCode(pEval, pAstLeft, pVLeft);
					if (!*static_cast<bool *>(pVLeft))
					{
						*static_cast<bool *>(pVRet) = false;
						return;
					}

					EvalCode(pEval, pAstRight, pVRet);
					return;
				}
				else if (strcmp(pChzOp, "or") == 0)
				{
					void * pVLeft = alloca(CbSizeOf(pAstLeft->tid));
					EvalCode(pEval, pAstLeft, pVLeft);
					if (*static_cast<bool *>(pVLeft))
					{
						*static_cast<bool *>(pVRet) = true;
						return;
					}

					EvalCode(pEval, pAstRight, pVRet);
					return;
				}

				if (strcmp(pChzOp, ".") == 0)
				{
					// BB (adrianb) Support using for this case?
					STypeId tidStruct = pAstLeft->tid;
					if (tidStruct.pType->typek == TYPEK_TypeOf)
					{
						auto pTypetypeof = PtypeCast<STypeTypeOf>(tidStruct.pType);
						auto pSymtStruct = PsymtStructLookup(pWork, pTypetypeof->tid);
						if (pSymtStruct)
						{
							auto pAstident = PastCast<SAstIdentifier>(pAstop->pAstRight);
							// BB (adrianb) Lookup from RegisterResolved instead?
							auto pResdecl = PresdeclResolved(pWork, pAstident);
							ASSERT(pResdecl);

							EvalCode(pEval, pResdecl->pDecl->pAstdecl->pAstValue, pVRet);
							return;
						}
					}
				}

				ShowErr(pAstop->errinfo, "Operator %s constant eval with types %s and %s NYI", 
						pChzOp, StrPrintType(pAstLeft->tid.pType).Pchz(), StrPrintType(pAstRight->tid.pType).Pchz());
				return;
			}
		}
		break;

	case ASTK_Cast:
		{
			auto pAstcast = PastCast<SAstCast>(pAst);

			STypeId tidSrc = pAstcast->pAstExpr->tid;
			STypeId tidDst = pAstcast->tid;

			if (tidSrc == tidDst)
			{
				EvalCode(pEval, pAstcast->pAstExpr, pVRet);
				return;
			}

			void * pVExpr = alloca(CbSizeOf(pAstcast->pAstExpr->tid));
			EvalCode(pEval, pAstcast->pAstExpr, pVExpr);
			
			TYPEK typekSrc = tidSrc.pType->typek;
			TYPEK typekDst = tidDst.pType->typek;

			if (FIsInt(typekSrc))
			{
				s64 n;
				switch (typekSrc)
				{
				case TYPEK_S8: n = *static_cast<s8 *>(pVExpr); break;
				case TYPEK_S16: n = *static_cast<s16 *>(pVExpr); break;
				case TYPEK_S32: n = *static_cast<s32 *>(pVExpr); break;
				case TYPEK_S64: n = *static_cast<s64 *>(pVExpr); break;
				case TYPEK_U8: n = *static_cast<u8 *>(pVExpr); break;
				case TYPEK_U16: n = *static_cast<u16 *>(pVExpr); break;
				case TYPEK_U32: n = *static_cast<u32 *>(pVExpr); break;
				case TYPEK_U64: n = *static_cast<u64 *>(pVExpr); break;
				default: ASSERT(false); n = 0; break;
				}

				switch (typekDst)
				{
				case TYPEK_S8: *static_cast<s8 *>(pVRet) = n; return;
				case TYPEK_S16: *static_cast<s16 *>(pVRet) = n; return;
				case TYPEK_S32: *static_cast<s32 *>(pVRet) = n; return;
				case TYPEK_S64: *static_cast<s64 *>(pVRet) = n; return;
				case TYPEK_U8: *static_cast<u8 *>(pVRet) = n; return;
				case TYPEK_U16: *static_cast<u16 *>(pVRet) = n; return;
				case TYPEK_U32: *static_cast<u32 *>(pVRet) = n; return;
				case TYPEK_U64: *static_cast<u64 *>(pVRet) = n; return;
				case TYPEK_Float: *static_cast<float *>(pVRet) = n; return;
				case TYPEK_Double: *static_cast<double *>(pVRet) = n; return;
				default: ASSERT(false); return;
				}
			}
			else if (FIsFloat(typekSrc))
			{
				double g;
				switch (typekSrc)
				{
				case TYPEK_Float: g = *static_cast<float *>(pVExpr); break;
				case TYPEK_Double: g = *static_cast<double *>(pVExpr); break;
				default: ASSERT(false); g = 0; break;
				}

				switch (typekDst)
				{
				case TYPEK_S8: *static_cast<s8 *>(pVRet) = g; return;
				case TYPEK_S16: *static_cast<s16 *>(pVRet) = g; return;
				case TYPEK_S32: *static_cast<s32 *>(pVRet) = g; return;
				case TYPEK_S64: *static_cast<s64 *>(pVRet) = g; return;
				case TYPEK_U8: *static_cast<u8 *>(pVRet) = g; return;
				case TYPEK_U16: *static_cast<u16 *>(pVRet) = g; return;
				case TYPEK_U32: *static_cast<u32 *>(pVRet) = g; return;
				case TYPEK_U64: *static_cast<u64 *>(pVRet) = g; return;
				case TYPEK_Float: *static_cast<float *>(pVRet) = g; return;
				case TYPEK_Double: *static_cast<double *>(pVRet) = g; return;
				default: ASSERT(false); return;
				}
			}

			ShowErr(pAst->errinfo, "Cannot cast between %s and %s", StrPrintType(tidSrc.pType).Pchz(), 
					StrPrintType(tidDst.pType).Pchz());
			return;
		}

	case ASTK_RunDirective:
		EvalConst(pWork, PastCast<SAstRunDirective>(pAst)->pAstExpr, pVRet);
		return;

	default:
		ShowErr(pAst->errinfo, "Can't evaluate as constant");
		return;
	}
}

struct SGenerateCtx
{
	struct SScope
	{
		int ipAstDeferMic;
		LLVMOpaqueBasicBlock * pLblockLoopContinue;	// BB (adrianb) Could add a loop label here to break out arbitrarily
		LLVMOpaqueBasicBlock * pLblockLoopBreak;
	};

	SWorkspace * pWork;

	SArray<SAst *> arypAstDefer; // Defer instructions per scope
	SArray<SScope> aryScope;	// Scopes
	bool fScopeTerminated;			// Have we returned?
	bool fInProcedure;			// Are we inside a procedure

	// Code generation Move to SGenerateCtx?
	
	LLVMOpaqueBuilder * pLbuilderAlloc;
	LLVMOpaqueBuilder * pLbuilder;
	LLVMOpaqueContext * pLctx;
	LLVMOpaqueModule * pLmod;

	SHash<STypeStruct *, LLVMOpaqueType *> hashPtypestructPltype;
	//SArray<LLVMOpaqueValue *> arypLvalGlobal;

#if 0
	SArray<SGlobal> aryGlobal;
#endif
	SHash<SAstDeclareSingle *, SStorage> hashPastdeclStorage; // 
	SHash<SAstProcedure *, LLVMOpaqueValue *> hashPastprocPlval; // Procedures started
};

void Reset(SGenerateCtx * pGenx)
{
	ASSERT(pGenx->arypAstDefer.c == 0);
	ASSERT(!pGenx->fInProcedure);
	pGenx->fScopeTerminated = false;
}

#if 0
void AddReferenced(SGenerateCtx * pGenx, SAst * pAst)
{
	if (pGenx->parypAstReferenced == nullptr)
		return;

	ASSERT(pAst->astk == ASTK_Procedure || pAst->astk == ASTK_DeclareSingle);
	Append(pGenx->parypAstReferenced, pAst);
}
#endif

const char * PchzBuild(SWorkspace * pWork)
{
	for (const auto & module : pWork->aryModule)
	{
		if (module.fBuiltIn)
			continue;
		return module.pChzFile;
	}

	ASSERT(false);
	return "<Unknown>";
}

void Init(SGenerateCtx * pGenx, SWorkspace * pWork)
{
	pGenx->pWork = pWork;
	pGenx->pLctx = LLVMContextCreate();
	pGenx->pLbuilder = LLVMCreateBuilderInContext(pGenx->pLctx);
	pGenx->pLbuilderAlloc = LLVMCreateBuilderInContext(pGenx->pLctx);
	pGenx->pLmod = LLVMModuleCreateWithNameInContext(PchzBuild(pWork), pGenx->pLctx);

	// BB (adrianb) How do I get this string from arbitrary platform? Also C generates a target data layout.
	//  Apparently the default target triple is wrong for some reason :/.
	{
		auto pChzTriple = "x86_64-apple-macosx10.11.0"; // LLVMGetDefaultTargetTriple();
		LLVMSetTarget(pGenx->pLmod, pChzTriple);
		//LLVMDisposeMessage(pChzTriple);
	}

}

void Destroy(SGenerateCtx * pGenx)
{
	Destroy(&pGenx->arypAstDefer);
	Destroy(&pGenx->aryScope);
	
	Destroy(&pGenx->hashPtypestructPltype);

	Destroy(&pGenx->hashPastprocPlval);
	Destroy(&pGenx->hashPastdeclStorage);

	LLVMDisposeBuilder(pGenx->pLbuilder);
	LLVMDisposeBuilder(pGenx->pLbuilderAlloc);

	if (pGenx->pLmod)
		LLVMDisposeModule(pGenx->pLmod);
	
	if (pGenx->pLctx)
		LLVMContextDispose(pGenx->pLctx);

	ClearStruct(pGenx);
}

struct SScopeCtx
{
	int iScope;
};

SScopeCtx ScopectxPush(SGenerateCtx * pGenx, LLVMOpaqueBasicBlock * pLblockLoopContinue = nullptr, 
						LLVMOpaqueBasicBlock * pLblockLoopBreak = nullptr)
{
	ASSERT((pLblockLoopContinue == nullptr) == (pLblockLoopBreak == nullptr));
	Append(&pGenx->aryScope, SGenerateCtx::SScope{ pGenx->arypAstDefer.c, pLblockLoopContinue, pLblockLoopBreak });
	return { pGenx->aryScope.c - 1 };
}

struct SStorage
{
	LLVMValueRef pLvalPtr; // Address where variable is stored
};

bool FIsKeyEqual(const void * pV0, const void * pV1)
{
	return pV0 == pV1;
}

SStorage * PstorageLookup(SGenerateCtx * pGenx, SAstDeclareSingle * pAstdecl)
{
	auto hv = HvFromKey(reinterpret_cast<u64>(pAstdecl));
	return PtLookupImpl(&pGenx->hashPastdeclStorage, hv, pAstdecl);
}

void RegisterStorage(SGenerateCtx * pGenx, SAstDeclareSingle * pAstdecl, LLVMValueRef pLvalPtr)
{
	ASSERTCHZ(PstorageLookup(pGenx, pAstdecl) == nullptr, 
			  "Found duplicate storage for declaration %s", pAstdecl->pChzName);
	auto hv = HvFromKey(reinterpret_cast<u64>(pAstdecl));
	
	Add(&pGenx->hashPastdeclStorage, hv, pAstdecl, {pLvalPtr});
}

LLVMOpaqueType * PltypeLookupStruct(SGenerateCtx * pGenx, const STypeStruct * pTypestruct)
{
	auto hv = HvFromKey(reinterpret_cast<u64>(pTypestruct));
	auto ppLtype = PtLookupImpl(&pGenx->hashPtypestructPltype, hv, pTypestruct);
	ASSERTCHZ(ppLtype, "Can't find llvm struct for %s", pTypestruct->pChzName);
	return *ppLtype;
}

LLVMOpaqueType * PltypeEnsureStruct(SGenerateCtx * pGenx, const STypeStruct * pTypestruct)
{
	auto hv = HvFromKey(reinterpret_cast<u64>(pTypestruct));
	auto ppLtype = PtLookupImpl(&pGenx->hashPtypestructPltype, hv, pTypestruct);
	ASSERTCHZ(ppLtype, "Can't find llvm struct for %s", pTypestruct->pChzName);
	return *ppLtype;
}

LLVMOpaqueType * PltypeGenerate(SGenerateCtx * pGenx, STypeId tid)
{
	auto pWork = pGenx->pWork;
	auto pType = tid.pType;
	if (pType == nullptr)
		return {};

	// BB (adrianb) Need to branch for foreign function signature vs internal function signature.
	//  E.g. vararg, 

	auto pLctx = pGenx->pLctx;

	TYPEK typek = pType->typek;
	switch (typek)
	{
	case TYPEK_Void: return LLVMVoidTypeInContext(pLctx);
	case TYPEK_Bool: return LLVMInt1TypeInContext(pLctx); // Bool is just int1 in llvm
	case TYPEK_S8: return LLVMInt8TypeInContext(pLctx);
	case TYPEK_S16: return LLVMInt16TypeInContext(pLctx);
	case TYPEK_S32: return LLVMInt32TypeInContext(pLctx);
	case TYPEK_S64: return LLVMInt64TypeInContext(pLctx);
	case TYPEK_U8: return LLVMInt8TypeInContext(pLctx); // Unsigned is not part of LLVM (must sext/zext explicitly)
	case TYPEK_U16: return LLVMInt16TypeInContext(pLctx);
	case TYPEK_U32: return LLVMInt32TypeInContext(pLctx);
	case TYPEK_U64: return LLVMInt64TypeInContext(pLctx);
	case TYPEK_Float: return LLVMFloatTypeInContext(pLctx);
	case TYPEK_Double: return LLVMDoubleTypeInContext(pLctx);
	//case TYPEK_String: return PltypeGenerate(pGenx, STypeId{pWork->pTypestructString});

	case TYPEK_Pointer: 
		{
			auto pTypeptr = PtypeCast<STypePointer>(pType);
			ASSERT(!pTypeptr->fSoa);
			return LLVMPointerType(PltypeGenerate(pGenx, pTypeptr->tidPointedTo), 0);
		}
            
        case TYPEK_String:
        case TYPEK_Struct:
            return PltypeLookupStruct(pGenx, Ptypestruct(pType));

	case TYPEK_Array:
		{
			auto pTypearray = PtypeCast<STypeArray>(pType);
			if (pTypearray->cSizeFixed >= 0)
			{
				ASSERT(!pTypearray->fSoa && !pTypearray->fDynamicallySized);
				return LLVMArrayType(PltypeGenerate(pGenx, pTypearray->tidElement), pTypearray->cSizeFixed);
			}
			else
			{
				auto pLtypePtr = PltypeGenerate(pGenx, TidPointer(pWork, pTypearray->tidElement));
				LLVMOpaqueType * apLtype[] = { pLtypePtr, LLVMInt64TypeInContext(pGenx->pLctx) };
				return LLVMStructTypeInContext(pGenx->pLctx, apLtype, DIM(apLtype), false);
			}
		}

	case TYPEK_Procedure:
		{
			// BB (adrianb) For foreign functions need to support varargs differently.

			auto pTypeproc = PtypeCast<STypeProcedure>(pType);

			STypeId tidRet = pWork->tidVoid;
			if (pTypeproc->cTidRet != 0)
			{
				ASSERT(pTypeproc->cTidRet == 1);
				tidRet = pTypeproc->aTidRet[0];
			}

			int cArg = pTypeproc->cTidArg;
			LLVMTypeRef * aTyperefArg = static_cast<LLVMTypeRef *>(alloca(sizeof(LLVMTypeRef) * cArg));
			for (int iTid : IterCount(cArg))
			{
				aTyperefArg[iTid] = PltypeGenerate(pGenx, pTypeproc->aTidArg[iTid]);
			}

			return LLVMFunctionType(PltypeGenerate(pGenx, tidRet), aTyperefArg, cArg, pTypeproc->fUsesCVararg);
		}

#if 0
	TYPEK_Any,
	TYPEK_Enum,
	TYPEK_TypeOf,
	TYPEK_Vararg,
#endif

	default:
		ASSERTCHZ(false, "Type generation for %s(%d) NYI", PchzFromTypek(typek), typek);
		return LLVMTypeRef{};
	}
}

SGlobal * PglobalAdd(SWorkspace * pWork, SAst * pAstSource, STypeId tid);

#if 0
SProcedure * PprocEnsure(SWorkspace * pWork, SAstProcedure * pAstproc)
{
	auto hv = HvFromKey(reinterpret_cast<u64>(pAstproc));
	auto ppProc = PtLookupImpl(&pWork->hashPastprocPproc, hv, pAstproc);
	if (ppProc)
		return *ppProc;

	auto pProc = PtAlloc<SProcedure>(&pWork->pagealloc);
	pProc->pAstproc = pAstproc;
	auto pGlobal = PglobalAdd(pWork, pAstproc, pAstproc->tid);
	pGlobal->fProcedure = true;
	pGlobal->pProc = pProc;

	pProc->reg = pGlobal->reg;

	Add(&pWork->hashPastprocPproc, hv, pAstproc, pProc);

	return pProc;
}
#endif

LLVMOpaqueValue * PlvalConstS32(SGenerateCtx * pGenx, int n)
{
	return LLVMConstInt(LLVMInt32TypeInContext(pGenx->pLctx), n, true);
}

LLVMOpaqueValue * PlvalConstU32(SGenerateCtx * pGenx, u32 n)
{
	return LLVMConstInt(LLVMInt32TypeInContext(pGenx->pLctx), n, false);
}

LLVMOpaqueValue * PlvalConstU64(SGenerateCtx * pGenx, u64 n)
{
	return LLVMConstInt(LLVMInt64TypeInContext(pGenx->pLctx), n, false);
}

LLVMOpaqueValue * PlvalBuildStruct(SGenerateCtx * pGenx, LLVMOpaqueType * pLtype, LLVMOpaqueValue ** apLval, int cpLval)
{
	// BB (adrianb) Clang treats structs somewhat differently. It builds this by storing values into a stack location.
	//  It initializes constants by storing in a global and calling @llvm.memcpy.p0i8.p0i8.i64 or so.
	//  Similarly struct assignment runs through @llvm.memcpy.p0i8.p0i8.i64.
	// !!! (adrianb) Clang has BIZARRE behavior wrt returning structs:
	//  return a Vector{float x; floaty;} -> return <2 x float>
	//  return a Vector{float x; int y;} -> cast to i64* and return i64!?
	//  return a Vector{float x; float y; int z;} -> return { <float x 2>, i32 }
	//  This is totally arcane even for optimization purposes.

	LLVMOpaqueValue * pLvalStruct = LLVMGetUndef(pLtype);
	ASSERT(LLVMCountStructElementTypes(pLtype) == u32(cpLval));
	for (int ipLval : IterCount(cpLval))
	{
		pLvalStruct = LLVMBuildInsertValue(pGenx->pLbuilder, pLvalStruct, apLval[ipLval], ipLval, "");
	}
	return pLvalStruct;
}

void GetMemberAddress(SGenerateCtx * pGenx, const char * pChzName, 
						LLVMOpaqueValue ** ppLvalPtr, STypeId * pTidPointedTo, SAst * pAstErr)
{
	auto pLbuilder = pGenx->pLbuilder;

	if (pTidPointedTo->pType->typek == TYPEK_Pointer)
	{
		*ppLvalPtr = LLVMBuildLoad(pLbuilder, *ppLvalPtr, "");
		*pTidPointedTo = PtypeCast<STypePointer>(pTidPointedTo->pType)->tidPointedTo;
	}

	const STypeStruct * pTypestruct = Ptypestruct(pTidPointedTo->pType);

	if (pTidPointedTo->pType->typek == TYPEK_Array)
	{
		auto pTypearray = PtypeCast<STypeArray>(pTidPointedTo->pType);
		if (pTypearray->cSizeFixed >= 0)
		{
			ASSERT(strcmp(pChzName, "a") == 0);
			*ppLvalPtr = LLVMBuildStructGEP(pLbuilder, *ppLvalPtr, 0, "");
			*pTidPointedTo = pTypearray->tidElement;
			return;
		}
	}

	EnsureTypeSize(pTypestruct);

	for (int iMember = 0;; ++iMember)
	{
		if (iMember >= pTypestruct->cMember)
            break;

		auto pAstdeclMember = pTypestruct->aMember[iMember].pAstdecl;
		if (strcmp(pChzName, pAstdeclMember->pChzName) != 0)
			continue;

		*ppLvalPtr = LLVMBuildStructGEP(pLbuilder, *ppLvalPtr, iMember, "");
		*pTidPointedTo = pTypestruct->aMember[iMember].pAstdecl->tid;
        return;
	}

	ShowErr(pAstErr->errinfo, "Couldn't find member %s of %s", pChzName, StrPrintType(pTypestruct).Pchz());
	*ppLvalPtr = nullptr;
	*pTidPointedTo = {};
}

LLVMOpaqueValue * PlvalEnsureProcedure(SGenerateCtx * pGenx, SAstProcedure * pAstproc)
{
	auto hv = HvFromKey(reinterpret_cast<u64>(pAstproc));
	LLVMOpaqueValue ** ppLval = PtLookupImpl(&pGenx->hashPastprocPlval, hv, pAstproc);
	if (ppLval)
		return *ppLval;

	ASSERT(pAstproc->tid.pType && pAstproc->tid.pType->typek == TYPEK_Procedure);
	LLVMOpaqueType * pLtypeProc = PltypeGenerate(pGenx, pAstproc->tid);

	auto pLvalProc = LLVMAddFunction(pGenx->pLmod, pAstproc->pChzName, pLtypeProc);
	
	// BB (adrianb) If it comes from a separate module mark as external?
	//  Or just emit all bitcode into one translation unit? Doesn't seem like there's any advantage to
	//  separate modules with typechecking walking all over the place.

	if (pAstproc->fIsForeign)
		LLVMSetLinkage(pLvalProc, LLVMExternalLinkage);

	Add(&pGenx->hashPastprocPlval, hv, pAstproc, pLvalProc);
	return pLvalProc;
}

LLVMOpaqueValue * PlvalGenerateRecursive(SGenerateCtx * pGenx, SAst * pAst);

LLVMOpaqueValue * PlvalGetLoadStoreAddress(SGenerateCtx * pGenx, SAst * pAst)
{
	auto pWork = pGenx->pWork;
	auto pLbuilder = pGenx->pLbuilder;

	ASTK astk = pAst->astk;
	switch (astk)
	{
	case ASTK_Operator:
		{
			auto pAstop = PastCast<SAstOperator>(pAst);
			auto pAstLeft = pAstop->pAstLeft;
			auto pAstRight = pAstop->pAstRight;
			if (pAstLeft == nullptr)
			{
				if (strcmp(pAstop->pChzOp, "<<") == 0)
				{
					return PlvalGenerateRecursive(pGenx, pAstop->pAstRight);
				}
			}
			else
			{
				if (strcmp(pAstop->pChzOp, ".") == 0)
				{
					// Get a pointer to the thing we want to get a member out of

					LLVMOpaqueValue * pLvalPtr;
					STypeId tidPointedTo;
					if (pAstLeft->tid.pType->typek == TYPEK_Pointer)
					{
						pLvalPtr = PlvalGenerateRecursive(pGenx, pAstLeft);
						tidPointedTo = PtypeCast<STypePointer>(pAstLeft->tid.pType)->tidPointedTo;
					}
					else
					{
						ASSERT(pAstLeft->tid.pType->typek != TYPEK_Array || 
								PtypeCast<STypeArray>(pAstLeft->tid.pType)->cSizeFixed < 0);

						pLvalPtr = PlvalGetLoadStoreAddress(pGenx, pAstLeft);
						tidPointedTo = pAstLeft->tid;
					}

					auto pResdecl = PresdeclResolved(pWork, pAstRight);

					for (int ipDecl = 0; ipDecl <= pResdecl->arypDeclUsingPath.c; ++ipDecl)
					{
						auto pDecl = (ipDecl == pResdecl->arypDeclUsingPath.c) ?
										pResdecl->pDecl :
										pResdecl->arypDeclUsingPath[ipDecl];

						GetMemberAddress(pGenx, pDecl->pChzName, &pLvalPtr, &tidPointedTo, pAstop);
					}

					return pLvalPtr;
				}
			}
		}
		break;

	case ASTK_Identifier:
		{
			auto pResdecl = PresdeclResolved(pWork, pAst);
			ASSERT(pResdecl);
			if (pResdecl->arypDeclUsingPath.c > 0)
			{
				// BB (adrianb) This initial lookup and starting at path decl 1 is the only difference
				//  with member declaration lookup.

				auto pDecl0 = pResdecl->arypDeclUsingPath[0];
				auto pAstdecl0 = pDecl0->pAstdecl;
				auto pStorage0 = PstorageLookup(pGenx, pAstdecl0);
				
				auto pLvalPtr  = pStorage0->pLvalPtr;
				auto tidPointedTo = pAstdecl0->tid;

				for (int ipDecl = 1; ipDecl <= pResdecl->arypDeclUsingPath.c; ++ipDecl)
				{
					auto pDecl = (ipDecl == pResdecl->arypDeclUsingPath.c) ?
									pResdecl->pDecl : 
									pResdecl->arypDeclUsingPath[ipDecl];

					GetMemberAddress(pGenx, pDecl->pChzName, &pLvalPtr, &tidPointedTo, pAst);
				}

				return pLvalPtr;
			}
			else
			{
				auto pAstdecl = pResdecl->pDecl->pAstdecl;
				ASSERT(!pAstdecl->fIsConstant);
				auto pStorage = PstorageLookup(pGenx, pAstdecl);
				return pStorage->pLvalPtr;
			}
		}
		break;

	case ASTK_ArrayIndex:
		{
			auto pAstarrayindex = PastCast<SAstArrayIndex>(pAst);

			auto pType = pAstarrayindex->pAstArray->tid.pType;
			auto typek = pType->typek;
			if (typek == TYPEK_Pointer)
			{
				auto pLvalAddr = PlvalGenerateRecursive(pGenx, pAstarrayindex->pAstArray);
				auto pLvalIndex = PlvalGenerateRecursive(pGenx, pAstarrayindex->pAstIndex);
				return LLVMBuildGEP(pLbuilder, pLvalAddr, &pLvalIndex, 1, "");
			}
			else if (typek == TYPEK_Array)
			{
				auto pTypearray = PtypeCast<STypeArray>(pType);

				if (pTypearray->cSizeFixed >= 0)
				{
					auto pLvalArrayAddr = PlvalGetLoadStoreAddress(pGenx, pAstarrayindex->pAstArray);
					auto pLvalIndex = PlvalGenerateRecursive(pGenx, pAstarrayindex->pAstIndex);

					LLVMOpaqueValue * apLvalGEP[] = { PlvalConstS32(pGenx, 0), pLvalIndex };
					return LLVMBuildGEP(pGenx->pLbuilder, pLvalArrayAddr, apLvalGEP, DIM(apLvalGEP), "");
				}
				else
				{
					auto pLvalArrayAddr = PlvalGetLoadStoreAddress(pGenx, pAstarrayindex->pAstArray);
					auto pLvalPtrA = LLVMBuildStructGEP(pLbuilder, pLvalArrayAddr, 0, ""); // getting a from {a,c}
					auto pLvalA = LLVMBuildLoad(pLbuilder, pLvalPtrA, "");

					auto pLvalIndex = PlvalGenerateRecursive(pGenx, pAstarrayindex->pAstIndex);
					return LLVMBuildGEP(pLbuilder, pLvalA, &pLvalIndex, 1, "");
				}
			}
			
			ShowErr(pAst->errinfo, "Don't know how to array index type %s", StrPrintType(pType).Pchz());
			return nullptr;
		}
		break;

	default:
		break;
	}

	ShowErr(pAst->errinfo, "Can't get address to load or store from");
	return {};
}

using PFNGENBINOP = LLVMOpaqueValue * (*)(LLVMOpaqueBuilder * pLbuilder, LLVMOpaqueValue * pLvalLeft, 
											LLVMOpaqueValue * pLvalRight, const char * pChz);

struct SGenerateBinaryOperator
{
	const char * pChzOp;

	PFNGENBINOP pfngbopInt;
	PFNGENBINOP pfngbopIntUnsigned;
	PFNGENBINOP pfngbopFloat;
	PFNGENBINOP pfngbopPointerAndInt;
	PFNGENBINOP pfngbopPointer;

	// bools?
};

template <LLVMIntPredicate lintpred>
LLVMOpaqueValue * GenCmpInt(LLVMOpaqueBuilder * pLbuilder, LLVMOpaqueValue * pLvalLeft, LLVMOpaqueValue * pLvalRight, const char * pChz)
{
	auto pChzLeftType = LLVMPrintTypeToString(LLVMTypeOf(pLvalLeft));
	auto pChzRightType = LLVMPrintTypeToString(LLVMTypeOf(pLvalRight));
	defer { LLVMDisposeMessage(pChzLeftType); LLVMDisposeMessage(pChzRightType); };

	return LLVMBuildICmp(pLbuilder, lintpred, pLvalLeft, pLvalRight, pChz);
}

template <LLVMRealPredicate lrealpred>
LLVMValueRef GenCmpFloat(LLVMOpaqueBuilder * pLbuilder, LLVMOpaqueValue * pLvalLeft, LLVMOpaqueValue * pLvalRight, const char * pChz)
{
	return LLVMBuildFCmp(pLbuilder, lrealpred, pLvalLeft, pLvalRight, pChz);
}

LLVMOpaqueValue * PlvalAddPtrInt(LLVMOpaqueBuilder * pLbuilder, LLVMOpaqueValue * pLvalLeft, LLVMOpaqueValue * pLvalRight, const char * pChz)
{
	LLVMOpaqueValue * apLvalIndex[] = 
	{
		pLvalRight
	};

	return LLVMBuildGEP(pLbuilder, pLvalLeft, apLvalIndex, DIM(apLvalIndex), pChz);
}

LLVMOpaqueValue * PlvalSubPtrInt(LLVMOpaqueBuilder * pLbuilder, LLVMOpaqueValue * pLvalLeft, LLVMOpaqueValue * pLvalRight, const char * pChz)
{
	LLVMOpaqueValue * apLvalIndex[] = 
	{
		LLVMBuildNeg(pLbuilder, pLvalRight, "")
	};

	return LLVMBuildGEP(pLbuilder, pLvalLeft, apLvalIndex, DIM(apLvalIndex), pChz);
}

LLVMOpaqueValue * PlvalSubPtr(LLVMOpaqueBuilder * pLbuilder, LLVMOpaqueValue * pLvalLeft, LLVMOpaqueValue * pLvalRight, const char * pChz)
{
	ASSERT(false);
	return nullptr;
}

static const SGenerateBinaryOperator s_aGbop[] =
{
	// BB (adrianb) Unordered (either arg NaN) vs Ordered. Which to choose?
	// BB (adrianb) Pointer operations right?
	{ "<", GenCmpInt<LLVMIntSLT>, GenCmpInt<LLVMIntULT>, GenCmpFloat<LLVMRealOLT>, nullptr, GenCmpInt<LLVMIntULT> },
	{ ">", GenCmpInt<LLVMIntSGT>, GenCmpInt<LLVMIntUGT>, GenCmpFloat<LLVMRealOGT>, nullptr, GenCmpInt<LLVMIntUGT> },

	{ "==", GenCmpInt<LLVMIntEQ>, nullptr, nullptr, nullptr, GenCmpInt<LLVMIntEQ> },
	{ "!=", GenCmpInt<LLVMIntNE>, nullptr, nullptr, nullptr, GenCmpInt<LLVMIntNE> },

	{ "+", LLVMBuildAdd, nullptr, LLVMBuildFAdd, PlvalAddPtrInt, nullptr },
	{ "-", LLVMBuildSub, nullptr, LLVMBuildFSub, PlvalSubPtrInt, PlvalSubPtr },
	{ "*", LLVMBuildMul, nullptr, LLVMBuildFMul, nullptr, nullptr },
	{ "/", LLVMBuildSDiv, LLVMBuildUDiv, LLVMBuildFDiv, nullptr, nullptr },
	{ "%", LLVMBuildSRem, LLVMBuildURem, nullptr, nullptr, nullptr }, // FRem?
};

LLVMOpaqueValue * PlvalGenerateBinaryOperator(SGenerateCtx * pGenx, const SGenerateBinaryOperator & gbop, 
										   SAst * pAstLeft, LLVMOpaqueValue * pLvalLeft, 
										   SAst * pAstRight, LLVMOpaqueValue * pLvalRight)
{
	auto pLbuilder = pGenx->pLbuilder;

	TYPEK typekLeft = pAstLeft->tid.pType->typek;
	TYPEK typekRight = pAstRight->tid.pType->typek;
	
	if (gbop.pfngbopPointerAndInt && ((typekLeft == TYPEK_Pointer && typekRight == TYPEK_S64) || 
									  (typekRight == TYPEK_Pointer && typekLeft == TYPEK_S64)))
	{
		if (typekRight == TYPEK_Pointer)
			Swap(pLvalLeft, pLvalRight);

		return gbop.pfngbopPointerAndInt(pLbuilder, pLvalLeft, pLvalRight, "");
	}
	else if (gbop.pfngbopPointer && (typekLeft == TYPEK_Pointer && typekRight == TYPEK_Pointer))
	{
		return gbop.pfngbopPointer(pLbuilder, pLvalLeft, pLvalRight, "");
	}
	else if (gbop.pfngbopInt && FIsInt(typekLeft))
	{
		if (gbop.pfngbopIntUnsigned && !FSigned(typekLeft))
		{
			return gbop.pfngbopIntUnsigned(pLbuilder, pLvalLeft, pLvalRight, "");
		}
		else
		{
			return gbop.pfngbopInt(pLbuilder, pLvalLeft, pLvalRight, "");
		}
	}
	else if (gbop.pfngbopFloat && FIsFloat(typekLeft))
	{
		return gbop.pfngbopFloat(pLbuilder, pLvalLeft, pLvalRight, "");
	}

	return nullptr;
}

LLVMOpaqueValue * PlvalGenerateBinaryOperator(SGenerateCtx * pGenx, const SGenerateBinaryOperator & gbop, 
										   SAst * pAstLeft, SAst * pAstRight)
{
	auto pLvalLeft = PlvalGenerateRecursive(pGenx, pAstLeft);
	auto pLvalRight = PlvalGenerateRecursive(pGenx, pAstRight);

	auto pLval = PlvalGenerateBinaryOperator(pGenx, gbop, pAstLeft, pLvalLeft, pAstRight, pLvalRight);

	ASSERTCHZ(pLval, "Operator %s can't be generated with types %s and %s NYI", 
			  gbop.pChzOp, StrPrintType(pAstLeft->tid.pType).Pchz(), StrPrintType(pAstRight->tid.pType).Pchz());

	return pLval;
}

static const SGenerateBinaryOperator s_aGbopAssign[] =
{
	// BB (adrianb) Other types?
	{ "+=", LLVMBuildAdd, nullptr, LLVMBuildFAdd, PlvalAddPtrInt },
	{ "-=", LLVMBuildSub, nullptr, LLVMBuildFSub },
	{ "*=", LLVMBuildMul, nullptr, LLVMBuildFMul },
	{ "/=", LLVMBuildSDiv, LLVMBuildUDiv, LLVMBuildFDiv },
	{ "%=", LLVMBuildSRem, LLVMBuildURem, nullptr }, // FRem?
};

void GenerateBinaryAssignOperator(SGenerateCtx * pGenx, const SGenerateBinaryOperator & gbop, 
									SAst * pAstLeft, SAst * pAstRight)
{
	auto pLbuilder = pGenx->pLbuilder;
	auto pLvalAddr = PlvalGetLoadStoreAddress(pGenx, pAstLeft);
	auto pLvalRight = PlvalGenerateRecursive(pGenx, pAstRight);
	auto pLvalLeft = LLVMBuildLoad(pLbuilder, pLvalAddr, "");

	auto pLvalOp = PlvalGenerateBinaryOperator(pGenx, gbop, pAstLeft, pLvalLeft, pAstRight, pLvalRight);

	ASSERTCHZ(pLvalOp, "Operator %s can't be generated with types %s and %s. NYI?", 
			  gbop.pChzOp, StrPrintType(pAstLeft->tid.pType).Pchz(), StrPrintType(pAstRight->tid.pType).Pchz());

	LLVMBuildStore(pLbuilder, pLvalOp, pLvalAddr);
}

LLVMOpaqueValue * PlvalConst(SGenerateCtx * pGenx, STypeId tid, const void * pV);

LLVMOpaqueValue * PlvalConstStruct(SGenerateCtx * pGenx, const STypeStruct * pTypestruct, LLVMOpaqueType * pLtype, 
									const u8 * pB)
{
	auto cMember = pTypestruct->cMember;
	auto apLvalMember = static_cast<LLVMOpaqueValue **>(alloca(sizeof(LLVMOpaqueValue *) * cMember));

	for (int iMember : IterCount(cMember))
	{
		auto pMember = &pTypestruct->aMember[iMember];
		apLvalMember[iMember] = PlvalConst(pGenx, pMember->pAstdecl->tid, pB + pMember->iBOffset);
	}

	return LLVMConstNamedStruct(pLtype, apLvalMember, cMember);
}

LLVMOpaqueValue * PlvalConstStringPtr(SGenerateCtx * pGenx, const char * pChz)
{
    if (pChz == nullptr)
        return LLVMConstPointerNull(PltypeGenerate(pGenx, TidPointer(pGenx->pWork, pGenx->pWork->tidU8)));

	// Unwrapping LLVMBuildGlobalStringPtr with nothing builder specific so it can be run at global scope

	// IRBuilder::CreateGlobalString

	auto pLvalConstStrData = LLVMConstStringInContext(pGenx->pLctx, pChz, strlen(pChz), false);
	auto pLvalStrGlobal = LLVMAddGlobal(pGenx->pLmod, LLVMTypeOf(pLvalConstStrData), ".str");
	LLVMSetGlobalConstant(pLvalStrGlobal, true);
	LLVMSetLinkage(pLvalStrGlobal, LLVMPrivateLinkage);
	LLVMSetInitializer(pLvalStrGlobal, pLvalConstStrData);
	LLVMSetUnnamedAddr(pLvalStrGlobal, true);

	// Rest of GlobaStringPtr

	auto pLvalZero = LLVMConstInt(LLVMInt32TypeInContext(pGenx->pLctx), 0, false);
	LLVMOpaqueValue * apLval[] = { pLvalZero, pLvalZero };
	return LLVMConstInBoundsGEP(pLvalStrGlobal, apLval, DIM(apLval));
}

LLVMOpaqueValue * PlvalConst(SGenerateCtx * pGenx, STypeId tid, const void * pV)
{
	auto pB = static_cast<const u8 *>(pV);
	TYPEK typek = tid.pType->typek;
	auto pLtype = PltypeGenerate(pGenx, tid);
	switch (typek)
	{
	case TYPEK_Bool: return LLVMConstInt(pLtype, *static_cast<const bool *>(pV), false);
	case TYPEK_S8: return LLVMConstInt(pLtype, *static_cast<const s8 *>(pV), true);
	case TYPEK_S16: return LLVMConstInt(pLtype, *static_cast<const s16 *>(pV), true);
	case TYPEK_S32: return LLVMConstInt(pLtype, *static_cast<const s32 *>(pV), true);
	case TYPEK_S64: return LLVMConstInt(pLtype, *static_cast<const s64 *>(pV), true);
	case TYPEK_U8: return LLVMConstInt(pLtype, *static_cast<const u8 *>(pV), false);
	case TYPEK_U16: return LLVMConstInt(pLtype, *static_cast<const u16 *>(pV), false);
	case TYPEK_U32: return LLVMConstInt(pLtype, *static_cast<const u32 *>(pV), false);
	case TYPEK_U64: return LLVMConstInt(pLtype, *static_cast<const u64 *>(pV), false);
	case TYPEK_Float: return LLVMConstReal(pLtype, *static_cast<const float *>(pV));
	case TYPEK_Double: return LLVMConstReal(pLtype, *static_cast<const double *>(pV));

	// case TYPEK_String: return PlvalConstStruct(pGenx, pGenx->pWork->pTypestructString, pLtype, pB);
	
	case TYPEK_Pointer:
		if (PtypeCast<STypePointer>(tid.pType)->tidPointedTo.pType->typek == TYPEK_U8)
		{
			return PlvalConstStringPtr(pGenx, *static_cast<char * const *>(pV));
		}
		return LLVMConstPointerNull(pLtype);
	
	case TYPEK_Array:
		{
			// BB (adrianb) This could get ridiculous for large arrays. Initialize those with functionssomehow?

			auto pTypearray = PtypeCast<STypeArray>(tid.pType);
			if (pTypearray->cSizeFixed < 0)
				goto LStruct;

			auto cElement = pTypearray->cSizeFixed;
			auto apLvalElement = static_cast<LLVMOpaqueValue **>(alloca(sizeof(LLVMOpaqueValue *) * cElement));

			int cBElement = CbSizeOf(pTypearray->tidElement);
			for (int iElement : IterCount(cElement))
			{
				apLvalElement[iElement] = PlvalConst(pGenx, pTypearray->tidElement, pB + iElement * cBElement);
			}

			return LLVMConstArray(PltypeGenerate(pGenx, pTypearray->tidElement), apLvalElement, cElement);
		}
		break;

	// case TYPEK_Procedure:

	case TYPEK_String:	
	case TYPEK_Struct:
LStruct:
		return PlvalConstStruct(pGenx, Ptypestruct(tid.pType), pLtype, pB);

	// case TYPEK_Any:
	// case TYPEK_Enum:
	// case TYPEK_TypeOf:
	// case TYPEK_Vararg:
	
	default:
		ASSERTCHZ(false, "Can't build constant for %s", StrPrintType(tid).Pchz());
		return nullptr;
	}
}

LLVMOpaqueValue * PlvalLoadConst(SGenerateCtx * pGenx, SAst * pAst)
{
	void * pVValue = alloca(CbSizeOf(pAst->tid));
	EvalConst(pGenx->pWork, pAst, pVValue);
	return PlvalConst(pGenx, pAst->tid, pVValue);
}



void MarkScopeTerminated(SGenerateCtx * pGenx)
{
	pGenx->fScopeTerminated = true;
}

// BB (adrianb) Want a way to detect if we emit any instructions after a return in a basic block so we can error on
//  said expression. Maybe do an expression level detection? Does that make sense?

void Branch(SGenerateCtx * pGenx, LLVMOpaqueBasicBlock * pLblock, SAst * pAst)
{
	// Don't branch if we've returned or it will count as terminator in middle of basic block
	// BB (adrianb) Should we be handling this outside branch?

	if (pGenx->fScopeTerminated)
	{
		pGenx->fScopeTerminated = false;
		return;
	}

	LLVMBuildBr(pGenx->pLbuilder, pLblock);
}

void PopScope(SGenerateCtx * pGenx, const SScopeCtx & scopectx)
{
	ASSERT(scopectx.iScope == pGenx->aryScope.c - 1);
	auto scope = Tail(&pGenx->aryScope);
	while (pGenx->arypAstDefer.c > scope.ipAstDeferMic)
	{
		auto pAstDefer = Tail(&pGenx->arypAstDefer);
		Pop(&pGenx->arypAstDefer);
		(void) PlvalGenerateRecursive(pGenx, pAstDefer);
	}
	Pop(&pGenx->aryScope);
}

void EarlyPopScopes(SGenerateCtx * pGenx, int iScope)
{
	int ipAstMic = (iScope >= 0) ? pGenx->aryScope[iScope].ipAstDeferMic : 0;

	for (int ipAst : IterCount(pGenx->arypAstDefer.c - ipAstMic))
	{
		auto pAstDefer = Tail(&pGenx->arypAstDefer, ipAst);
		(void) PlvalGenerateRecursive(pGenx, pAstDefer);
	}
}

void EarlyReturnPopScopes(SGenerateCtx * pGenx)
{
	EarlyPopScopes(pGenx, -1);
}

LLVMOpaqueValue * PlvalGenerateConstant(SGenerateCtx * pGenx, SAst * pAst)
{
	ASTK astk = pAst->astk;
	switch (astk)
	{
	case ASTK_Literal:
		{
			auto pAstlit = PastCast<SAstLiteral>(pAst);
			const SLiteral & lit = pAstlit->lit;
			if (lit.litk == LITK_String && pAst->tid.pType->typek == TYPEK_Pointer)
			{
				ASSERT(PtypeCast<STypePointer>(pAst->tid.pType)->tidPointedTo.pType->typek == TYPEK_U8);

				// BB (adrianb) String or StringPtr? Do we need to pool these manually?
				// BB (adrianb) LLVMConstString instead?
				return LLVMBuildGlobalStringPtr(pGenx->pLbuilder, lit.pChz, "stringptr");
			}

			return PlvalLoadConst(pGenx, pAst);
		}

	case ASTK_Null:
	case ASTK_Cast:
	case ASTK_Operator:
	case ASTK_Identifier:
	case ASTK_RunDirective:
		return PlvalLoadConst(pGenx, pAst);

	case ASTK_Procedure:
		{
			// Identifier reference to procedure

			//AddReferenced(pGenx, pAst);
			return PlvalEnsureProcedure(pGenx, PastCast<SAstProcedure>(pAst));
		}

	default:
		ShowErr(pAst->errinfo, "Can't generate constant for ast %s(%d)", PchzFromAstk(astk), astk);
		return nullptr;
	}
}

LLVMOpaqueValue * PlvalGenerateDefaultValue(SGenerateCtx * pGenx, STypeId tid, LLVMOpaqueType * pLtype)
{
	auto pVVal = alloca(CbSizeOf(tid));

	EvalDefaultValue(pGenx->pWork, tid, static_cast<u8*>(pVVal));

	return PlvalConst(pGenx, tid, pVVal);

#if 0
	// Generate default value

	TYPEK typek = tid.pType->typek;

	if (FIsInt(typek))
	{
		return LLVMConstInt(pLtype, 0, true);
	}
	else if (FIsFloat(typek))
	{
		return LLVMConstReal(pLtype, 0.0);
	}
	else if (typek == TYPEK_Pointer)
	{
		return LLVMConstNull(pLtype);
	}
	else if (typek == TYPEK_Struct)
	{
		#if 0
		auto pTypestruct = PtypeCast<STypeStruct>(tid.pType);
		int cMember = pTypestruct->cMember;
		auto aReg = static_cast<REG *>(alloca(sizeof(REG) * cMember));
		for (int iMember : IterCount(cMember))
		{
			auto pAstdecl = pTypestruct->aMember[iMember].pAstdecl;
			ASSERT(!pAstdecl->fIsConstant);

			// BB (adrianb) Are values always set to correct type?

			if (pAstdecl->pAstValue)
				aReg[iMember] = PlvalGenerateConstant(pGenx, pAstdecl->pAstValue);
			else
				aReg[iMember] = RegGenerateDefaultValue(pGenx, pAstdecl->tid);
		}

		return LLVMConstStructInContext(pGenx->pGen->lcontext, aReg, cMember, false);
		#endif
	}
	else if (typek == TYPEK_Array)
	{
		#if 0
		auto pTypearray = PtypeCast<STypeArray>(tid.pType);
		ASSERT(!pTypearray->fSoa && pTypearray->cSizeFixed >= 0);
		s64 cElement = pTypearray->cSizeFixed;
		auto aReg = static_cast<REG *>(alloca(sizeof(REG) * cElement));
		for (int iElement : IterCount(cElement))
		{
			aReg[iElement] = RegGenerateDefaultValue(pGenx, pTypearray->tidElement);
		}

		return LLVMConstArray(ltyperefElement, aReg, cElement);
		#endif
	}
	
	ASSERTCHZ(false, "Declaration default value for type %s NYI", StrPrintType(tid.pType).Pchz());
	return {};
#endif
}

LLVMOpaqueValue * PlvalGenerateRecursive(SGenerateCtx * pGenx, SAst * pAst)
{
	auto pWork = pGenx->pWork;
	auto pLbuilder = pGenx->pLbuilder;

	switch (pAst->astk)
	{
	case ASTK_Literal:
	case ASTK_Null:
		return PlvalGenerateConstant(pGenx, pAst);

	// ASTK_UninitializedValue

	case ASTK_Block:
		{
			SScopeCtx scopectx = ScopectxPush(pGenx);
			for (auto pAst : PastCast<SAstBlock>(pAst)->arypAst)
			{
				(void) PlvalGenerateRecursive(pGenx, pAst);
			}
			PopScope(pGenx, scopectx);
			return nullptr;
		}

	case ASTK_EmptyStatement:
		return nullptr;

	case ASTK_Identifier:
		{
			// If we are a constant, just go directly to constant evaluation

			auto pResdecl = PresdeclResolved(pWork, pAst);
			auto pAstdecl = pResdecl->pDecl->pAstdecl;
			if (pAstdecl->fIsConstant)
			{
				return PlvalGenerateConstant(pGenx, pAstdecl->pAstValue);
			}
			else
			{
				auto pLvalAddr = PlvalGetLoadStoreAddress(pGenx, pAst);
				return LLVMBuildLoad(pLbuilder, pLvalAddr, "");
			}
		}
		break;

	case ASTK_Operator:
		{
			auto pAstop = PastCast<SAstOperator>(pAst);
			auto pChzOp = pAstop->pChzOp;

			// BB (adrianb) Table drive these or it will get insane!

			if (pAstop->pAstLeft == nullptr)
			{
				auto pAstRight = pAstop->pAstRight;
				TYPEK typekRight = pAstRight->tid.pType->typek;

				if (strcmp(pChzOp, "-") == 0)
				{
					auto pLval = PlvalGenerateRecursive(pGenx, pAstRight);

					if (FIsInt(typekRight))
						return LLVMBuildNeg(pLbuilder, pLval, "");
					else if (FIsFloat(typekRight))
						return LLVMBuildFNeg(pLbuilder, pLval, "");
				}
				else if (strcmp(pChzOp, "!") == 0)
				{
					auto pLval = PlvalGenerateRecursive(pGenx, pAstRight);
					return LLVMBuildNot(pLbuilder, pLval, "");
				}
				else if (strcmp(pChzOp, "++") == 0)
				{
					ASSERT(FIsInt(typekRight));
					auto pLvalAddr = PlvalGetLoadStoreAddress(pGenx, pAstRight);
					auto pLvalOrig = LLVMBuildLoad(pLbuilder, pLvalAddr, "");
					auto pLvalOne = LLVMConstInt(PltypeGenerate(pGenx, pAstRight->tid), 1, false);
					auto pLvalInc = LLVMBuildAdd(pLbuilder, pLvalOrig, pLvalOne, "");
					LLVMBuildStore(pLbuilder, pLvalInc, pLvalAddr);
					return pLvalInc;
				}
				else if (strcmp(pChzOp, "*") == 0)
				{
					return PlvalGetLoadStoreAddress(pGenx, pAstRight);
				}
				else if (strcmp(pChzOp, "<<") == 0)
				{
					auto pLvalRight = PlvalGenerateRecursive(pGenx, pAstRight);
					return LLVMBuildLoad(pLbuilder, pLvalRight, "");
				}

				ASSERTCHZ(false, "Unary operator %s generation with type %s NYI", 
						  pChzOp, StrPrintType(pAstRight->tid.pType).Pchz());
			}
			else
			{
				auto pAstLeft = pAstop->pAstLeft;
				auto pAstRight = pAstop->pAstRight;

				for (const auto & gbop : s_aGbop)
				{
					if (strcmp(gbop.pChzOp, pChzOp) == 0)
					{
						return PlvalGenerateBinaryOperator(pGenx, gbop, pAstLeft, pAstRight);
					}
				}

				for (const auto & gbop : s_aGbopAssign)
				{
					if (strcmp(gbop.pChzOp, pChzOp) == 0)
					{
						GenerateBinaryAssignOperator(pGenx, gbop, pAstLeft, pAstRight);
						return nullptr;
					}
				}

				bool fIsOrOp = strcmp(pChzOp, "or") == 0;
				if (fIsOrOp || strcmp(pChzOp, "and") == 0)
				{
					// Eval left, branch on it, eval right, branch to done, done phi based on first and 2nd branch

					// BB (adrianb) Could chain ands and ors together for less branching. This is entirely
					//  local which is nice.

					auto pLvalTest = PlvalGenerateRecursive(pGenx, pAstLeft);

					auto pLblockStart = LLVMGetInsertBlock(pLbuilder);
					auto pLvalFunc = LLVMGetBasicBlockParent(pLblockStart);
					auto pLblockRight = LLVMAppendBasicBlockInContext(pGenx->pLctx, pLvalFunc, 
																		(fIsOrOp) ? "orright" : "andright");
					auto pLblockDone = LLVMAppendBasicBlockInContext(pGenx->pLctx, pLvalFunc, 
																		(fIsOrOp) ? "ordone" : "anddone");
					
					if (fIsOrOp)
						LLVMBuildCondBr(pLbuilder, pLvalTest, pLblockDone, pLblockRight);
					else
						LLVMBuildCondBr(pLbuilder, pLvalTest, pLblockRight, pLblockDone);
					
					LLVMPositionBuilderAtEnd(pLbuilder, pLblockRight);

					auto pLvalRight = PlvalGenerateRecursive(pGenx, pAstRight);
                    auto pLblockLast = LLVMGetInsertBlock(pLbuilder);
					Branch(pGenx, pLblockDone, pAst);
					
					LLVMPositionBuilderAtEnd(pLbuilder, pLblockDone);

					ASSERT(pAst->tid.pType->typek == TYPEK_Bool);
					auto pLvalLeftResult = LLVMConstInt(PltypeGenerate(pGenx, pWork->tidBool), fIsOrOp, false);

					auto pLvalPhi = LLVMBuildPhi(pLbuilder, PltypeGenerate(pGenx, pWork->tidBool), "");
					
					LLVMOpaqueValue * apLval [] = { pLvalLeftResult, pLvalRight };
					LLVMOpaqueBasicBlock * apLblock [] = { pLblockStart, pLblockLast };
					CASSERT(DIM(apLval) == DIM(apLblock));

					LLVMAddIncoming(pLvalPhi, apLval, apLblock, DIM(apLval));
					return pLvalPhi;
				}
				else if (strcmp(pChzOp, ".") == 0)
				{
					auto pResdecl = PresdeclResolved(pWork, pAstop->pAstRight);
					auto pAstdecl = pResdecl->pDecl->pAstdecl;

					if (pAstdecl->fIsConstant)
					{
						// BB (adrianb) If there's executable code down this path somewhere should we always be 
						//  executing it? It could be either an identifier or a dot. We could try and generate the dot
						//  case?

						if (pAstop->tid.pType->typek != TYPEK_TypeOf)
						{
							(void) PlvalGetLoadStoreAddress(pGenx, pAstLeft);
						}

						return PlvalGenerateConstant(pGenx, pAstdecl->pAstValue);
					}
					else if (pAstop->pAstLeft->tid.pType->typek == TYPEK_Array &&
								PtypeCast<STypeArray>(pAstop->pAstLeft->tid.pType)->cSizeFixed >= 0)
					{
						ASSERT(pAstop->pAstRight->astk == ASTK_Identifier && 
								strcmp(PastCast<SAstIdentifier>(pAstop->pAstRight)->pChz, "a") == 0);

						auto pLvalArray = PlvalGetLoadStoreAddress(pGenx, pAstop->pAstLeft);
						auto pLvalZero = PlvalConstS32(pGenx, 0);
						LLVMOpaqueValue * apLvalGEP[] = { pLvalZero, pLvalZero };
						return LLVMBuildGEP(pLbuilder, pLvalArray, apLvalGEP, DIM(apLvalGEP), "");
					}
					else
					{
						auto pLvalAddr = PlvalGetLoadStoreAddress(pGenx, pAstop);
						return LLVMBuildLoad(pLbuilder, pLvalAddr, "");
					}
				}

				if (strcmp(pChzOp, "=") == 0)
				{
					auto pLvalAddr = PlvalGetLoadStoreAddress(pGenx, pAstLeft);
					auto pLvalValue = PlvalGenerateRecursive(pGenx, pAstRight);
					LLVMBuildStore(pLbuilder, pLvalValue, pLvalAddr);
					return nullptr;
				}

				ASSERTCHZ(false, "Operator %s generation with types %s and %s NYI", 
						  pChzOp, StrPrintType(pAstLeft->tid.pType).Pchz(), StrPrintType(pAstRight->tid.pType).Pchz());

				return nullptr;
			}
		}
		break;

	case ASTK_If:
		{
			// Construct a test basic block and jump to it, fill into that block
			// Run while loop body and jump to test basic block

			// BB (adrianb) Scope for if pass or else?

			auto pAstif = PastCast<SAstIf>(pAst);
			
			auto pLvalTest = PlvalGenerateRecursive(pGenx, pAstif->pAstCondition);

			// BB (adrianb) Clearer output if we emit blocks first and append branches later.

			auto pLvalFunc = LLVMGetBasicBlockParent(LLVMGetInsertBlock(pLbuilder));
			auto pLblockIfPass = LLVMAppendBasicBlockInContext(pGenx->pLctx, pLvalFunc, "ifpass");
			auto pLblockIfExit = LLVMAppendBasicBlockInContext(pGenx->pLctx, pLvalFunc, "ifexit");

			auto pLblockIfElse = pLblockIfExit;
			if (pAstif->pAstElse)
				pLblockIfElse = LLVMAppendBasicBlockInContext(pGenx->pLctx, pLvalFunc, "ifelse");

			LLVMBuildCondBr(pLbuilder, pLvalTest, pLblockIfPass, pLblockIfElse);
			LLVMPositionBuilderAtEnd(pLbuilder, pLblockIfPass);

			VERIFY(PlvalGenerateRecursive(pGenx, pAstif->pAstPass) == nullptr);
			Branch(pGenx, pLblockIfExit, pAst);
			LLVMPositionBuilderAtEnd(pLbuilder, pLblockIfElse);

			if (pAstif->pAstElse)
			{
				VERIFY(PlvalGenerateRecursive(pGenx, pAstif->pAstElse) == nullptr);
				Branch(pGenx, pLblockIfExit, pAst);
				LLVMPositionBuilderAtEnd(pLbuilder, pLblockIfExit);
			}
			else
			{
				ASSERT(pLblockIfElse == pLblockIfExit);
			}

			// Continue with code after if in the exit basic block
		}
		break;

	case ASTK_While:
		{
			// Construct a test basic block and jump to it, fill into that block
			// Run while loop body and jump to test basic block

			auto pAstwhile = PastCast<SAstWhile>(pAst);

			// BB (adrianb) Pack these into genx so continue/break can jump to necessary spots if need be.

			auto pLvalFunc = LLVMGetBasicBlockParent(LLVMGetInsertBlock(pLbuilder));
			auto pLblockTest = LLVMAppendBasicBlockInContext(pGenx->pLctx, pLvalFunc, "whiletest");
			auto pLblockBody = LLVMAppendBasicBlockInContext(pGenx->pLctx, pLvalFunc, "whilepass");
			auto pLblockExit = LLVMAppendBasicBlockInContext(pGenx->pLctx, pLvalFunc, "whileexit");

			Branch(pGenx, pLblockTest, pAst);
			LLVMPositionBuilderAtEnd(pLbuilder, pLblockTest);

			auto pLvalTest = PlvalGenerateRecursive(pGenx, pAstwhile->pAstCondition);

			LLVMBuildCondBr(pLbuilder, pLvalTest, pLblockBody, pLblockExit);
			LLVMPositionBuilderAtEnd(pLbuilder, pLblockBody);

			SScopeCtx scopectx = ScopectxPush(pGenx, pLblockTest, pLblockExit);
			VERIFY(PlvalGenerateRecursive(pGenx, pAstwhile->pAstLoop) == nullptr);
			PopScope(pGenx, scopectx);

			Branch(pGenx, pLblockTest, pAst);
			LLVMPositionBuilderAtEnd(pLbuilder, pLblockExit);

			// Continue with code after while loop in the exit basic block
		}
		break;

#if 0
	ASTK_For,
#endif

	case ASTK_LoopControl:
		{
			auto pAstloopctrl = PastCast<SAstLoopControl>(pAst);
			for (int iScope = pGenx->aryScope.c - 1; iScope >= 0; --iScope)
			{
				const auto & scope = pGenx->aryScope[iScope];
				auto pLblockLoop = (pAstloopctrl->fContinue) ? scope.pLblockLoopContinue : scope.pLblockLoopBreak;
				if (pLblockLoop)
				{
					EarlyPopScopes(pGenx, iScope);
					LLVMBuildBr(pLbuilder, pLblockLoop);
					MarkScopeTerminated(pGenx);
					break;
				}
			}
		}
		break;

	// ASTK_Using

	case ASTK_Cast:
		{
			auto pAstcast = PastCast<SAstCast>(pAst);

			STypeId tidSrc = pAstcast->pAstExpr->tid;
			STypeId tidDst = pAstcast->tid;

			TYPEK typekSrc = tidSrc.pType->typek;
			TYPEK typekDst = tidDst.pType->typek;

			if (tidSrc == tidDst)
			{
				return PlvalGenerateRecursive(pGenx, pAstcast->pAstExpr);
			}

			if (typekDst == TYPEK_Array && typekSrc == TYPEK_Array)
			{
				auto pTypearraySrc = PtypeCast<STypeArray>(tidSrc.pType);
				
				if (pTypearraySrc->fDynamicallySized)
				{
					ASSERT(false);
					return nullptr;
				}
				else
				{
					// Build nameless array struct type (GetElementPtr(00), const int count or dynamic sized count);

					auto pLtypeArray = PltypeGenerate(pGenx, tidDst);
					auto pLvalPtrArray = PlvalGetLoadStoreAddress(pGenx, pAstcast->pAstExpr);

					LLVMOpaqueValue * apLvalGEP[] = { PlvalConstS32(pGenx, 0), PlvalConstS32(pGenx, 0) };
					auto pLvalA = LLVMBuildGEP(pGenx->pLbuilder, pLvalPtrArray, apLvalGEP, DIM(apLvalGEP), "");
					auto pLvalC = PlvalConstU64(pGenx, pTypearraySrc->cSizeFixed);
					LLVMOpaqueValue * apLvalAry[] = { pLvalA, pLvalC };
					return PlvalBuildStruct(pGenx, pLtypeArray, apLvalAry, DIM(apLvalAry));
				}
			}

			// BB (adrianb) Just do this branch in evaluator?

			auto pLvalExpr = PlvalGenerateRecursive(pGenx, pAstcast->pAstExpr);
			auto pLtypeDst = PltypeGenerate(pGenx, tidDst);

			if (FIsInt(typekSrc))
			{
				if (FIsInt(typekDst))
				{
					// sext or zext when up casting in bitcount

					if (CBit(typekSrc) > CBit(typekDst))
					{
						return LLVMBuildTrunc(pLbuilder, pLvalExpr, pLtypeDst, "");
					}
					else
					{
						// BB (adrianb) Zext is faster than sext. Should we emit zext more often?
						//  Default to unsigned more often?

						if (FSigned(typekSrc))
							return LLVMBuildSExt(pLbuilder, pLvalExpr, pLtypeDst, "");
						else
							return LLVMBuildZExt(pLbuilder, pLvalExpr, pLtypeDst, "");
					}
				}
				else if (FIsFloat(typekDst))
				{
					if (FSigned(typekSrc))
						return LLVMBuildSIToFP(pLbuilder, pLvalExpr, pLtypeDst, "");
					else
						return LLVMBuildUIToFP(pLbuilder, pLvalExpr, pLtypeDst, "");
				}
			}
			else if (FIsFloat(typekSrc))
			{
				if (FIsFloat(typekDst))
				{
					if (CBit(typekSrc) > CBit(typekDst))
						return LLVMBuildFPTrunc(pLbuilder, pLvalExpr, pLtypeDst, "");
					else
						return LLVMBuildFPExt(pLbuilder, pLvalExpr, pLtypeDst, "");
				}
				else if (FIsInt(typekDst))
				{
					if (FSigned(typekDst))
						return LLVMBuildFPToSI(pLbuilder, pLvalExpr, pLtypeDst, "");
					else
						return LLVMBuildFPToUI(pLbuilder, pLvalExpr, pLtypeDst, "");
				}
			}

			ASSERTCHZ(false, "Cannot cast between %s and %s", StrPrintType(tidSrc.pType).Pchz(), 
					  StrPrintType(tidDst.pType).Pchz());
		}
		break;

#if 0
	ASTK_New,
	ASTK_Delete,
	ASTK_Remove, // yuck, use #remove instead? Not compile time...
#endif

	case ASTK_Defer:
		{
			Append(&pGenx->arypAstDefer, PastCast<SAstDefer>(pAst)->pAstStmt);
		}
		break;

#if 0
	ASTK_Inline,
	ASTK_PushContext, // BB (adrianb) unconvinced about this.  Is it thread safe?
#endif

	case ASTK_ArrayIndex:
		{
			auto pLvalAddr = PlvalGetLoadStoreAddress(pGenx, pAst);
			return LLVMBuildLoad(pLbuilder, pLvalAddr, "");
		}
		break;

	case ASTK_Call:
		{
			auto pAstcall = PastCast<SAstCall>(pAst);

			// BB (adrianb) Handle vararg [] Any case. C vararg case.

			// BB (adrianb) Creating extern function:
			// Function *F = Function::Create(FT, Function::ExternalLinkage, Name, TheModule);
			// Do this for all functions? E.g. so out of order will work? Or just start the function
			// and continue filling it later.

			auto pLvalProc = PlvalGenerateRecursive(pGenx, pAstcall->pAstFunc);

			int cArg = pAstcall->arypAstArgs.c;
			auto apLvalArg = static_cast<LLVMOpaqueValue **>(alloca(sizeof(LLVMOpaqueValue *) * cArg));

			for (int iArg : IterCount(cArg))
			{
				apLvalArg[iArg] = PlvalGenerateRecursive(pGenx, pAstcall->arypAstArgs[iArg]);
			}

			return LLVMBuildCall(pLbuilder, pLvalProc, apLvalArg, cArg, "");
		}
		break;

	case ASTK_Return:
		{
			// BB (adrianb) Detect multiple returns. We get a useless llvm error right now.

			auto pAstret = PastCast<SAstReturn>(pAst);

			LLVMOpaqueValue * pLvalRet = nullptr;
			if (pAst->tid.pType->typek != TYPEK_Void)
			{
				ASSERT(pAstret->arypAstRet.c == 1);
				pLvalRet = PlvalGenerateRecursive(pGenx, pAstret->arypAstRet[0]);
			}

			MarkScopeTerminated(pGenx);
			EarlyReturnPopScopes(pGenx);

			// LLVMValueRef LLVMBuildAggregateRet(LLVMBuilderRef, LLVMValueRef *RetVals, unsigned N);

			if (pLvalRet == nullptr)
			{
				(void) LLVMBuildRetVoid(pLbuilder);
			}
			else
			{
				(void) LLVMBuildRet(pLbuilder, pLvalRet);
			}
            
            return nullptr;
		}
		break;

	case ASTK_DeclareSingle:
		{
			auto pAstdecl = PastCast<SAstDeclareSingle>(pAst);

			if (pAstdecl->fIsConstant)
				return nullptr;

			// BB (adrianb) We need to do this in the entry block for all normal variables per
			//  http://llvm.org/docs/Frontend/PerformanceTips.html#use-of-allocas

			auto pLtype = PltypeGenerate(pGenx, pAstdecl->tid);
			auto pLbuilderAlloc = pGenx->pLbuilderAlloc;
			auto pLvalAddr = LLVMBuildAlloca(pLbuilderAlloc, pLtype, pAstdecl->pChzName);

			// BB (adrianb) Struct/array initialization using a copy ala clang?

			LLVMOpaqueValue * pLvalInitial;
			if (pAstdecl->pAstValue)
			{
				// BB (adrianb) Anything special to do if this will evaluate to a constant?

				pLvalInitial = PlvalGenerateRecursive(pGenx, pAstdecl->pAstValue);
			}
			else
			{
				// BB (adrianb) Support ASTK_UninitializedValue. For complex structs run a function to initialize.
				//  E.g. big structs, or with uninitialized members. Also could memcpy from a global like clang does
				//  for C++ structs.

				pLvalInitial = PlvalGenerateDefaultValue(pGenx, pAstdecl->tid, pLtype);
			}

			// BB (adrianb) Return value? Or register it against this AST node?
			
			LLVMBuildStore(pLbuilder, pLvalInitial, pLvalAddr);
			RegisterStorage(pGenx, pAstdecl, pLvalAddr);
		}
		break;

#if 0
	ASTK_DeclareMulti,
	ASTK_AssignMulti,
#endif

	// ASTK_Struct
	// ASTK_Enum

	case ASTK_Procedure:
		{
			auto pAstproc = PastCast<SAstProcedure>(pAst);

			ASSERT(!pGenx->fInProcedure && pGenx->arypAstDefer.c == 0);

			// Start basic block for the procedure

			auto pLvalProc = PlvalEnsureProcedure(pGenx, pAstproc);
			auto pLblockAlloca = LLVMAppendBasicBlockInContext(pGenx->pLctx, pLvalProc, "entry");
			auto pLblockEntry = LLVMAppendBasicBlockInContext(pGenx->pLctx, pLvalProc, "entry");
			
			LLVMPositionBuilderAtEnd(pGenx->pLbuilderAlloc, pLblockAlloca);
			LLVMPositionBuilderAtEnd(pLbuilder, pLblockEntry);

			// Add storage for arguments

			// BB (adrianb) Varargs.
			
			for (u32 iArg : IterCount(LLVMCountParams(pLvalProc)))
            {
            	auto pAstdecl = PastCast<SAstDeclareSingle>(pAstproc->arypAstDeclArg[iArg]);
            	auto pLvalPtr = LLVMBuildAlloca(pGenx->pLbuilderAlloc, PltypeGenerate(pGenx, pAstdecl->tid), pAstdecl->pChzName);
			
				LLVMBuildStore(pLbuilder, LLVMGetParam(pLvalProc, iArg), pLvalPtr);
				RegisterStorage(pGenx, pAstdecl, pLvalPtr);
            }

			// BB (adrianb) Need implicit return?			

			auto pAstblock = pAstproc->pAstblock;
			if (pAstblock)
			{
				SScopeCtx scopectx = ScopectxPush(pGenx);

				(void) PlvalGenerateRecursive(pGenx, pAstblock);

				if (pAstblock->arypAst.c == 0 || Tail(&pAstblock->arypAst)->astk != ASTK_Return)
				{
					if (pAstproc->arypAstDeclRet.c != 0)
					{
						ShowErr(pAstproc->errinfo, "Procedure didn't return a value");
					}

					PopScope(pGenx, scopectx);

					LLVMBuildRetVoid(pLbuilder);
				}
				else
				{
					// Assuming someone else has already returned and performed defer operations, 
					//  just reset scopes here.

					// BB (adrianb) Code might be simpler if we did store return value.

					ASSERT(pGenx->aryScope.c == 1);

					pGenx->aryScope.c = 0;
					pGenx->arypAstDefer.c = 0;
				}
			}

			// Stitch alloc block to entry block
			LLVMBuildBr(pGenx->pLbuilderAlloc, pLblockEntry);

			pGenx->fInProcedure = false;
		}
		break;

	// ASTK_TypeDefinition

	// ASTK_TypePointer
	// ASTK_TypeArray
	// ASTK_TypeProcedure
	// ASTK_TypePolymorphic
	// ASTK_TypeVararg

	// ASTK_ImportDirective

	case ASTK_RunDirective:
		return PlvalGenerateConstant(pGenx, pAst);

	// ASTK_ForeignLibraryDirective

	default:
		ASSERTCHZ(false, "Generate for astk %s(%d) NYI", PchzFromAstk(pAst->astk), pAst->astk);
		break;
	}

	return nullptr;
}

#if 0
SGlobal * PglobalAdd(SWorkspace * pWork, SAst * pAstSource, STypeId tid)
{
	int iGlobal = pWork->aryGlobal.c;
	auto pGlobal = PtAppendNew(&pWork->aryGlobal);
	pGlobal->pAstSource = pAstSource;
	pGlobal->regtAddr = { tid };
	pGlobal->reg = REG((iGlobal + 1) | REG_Global);
	return pGlobal;
}
#endif

void GenerateAll(SGenerateCtx * pGenx)
{
	auto pWork = pGenx->pWork;
	if (pWork->aryModule.c == 0)
		return;

	// Create names for all the structures, including locally defined ones
	// BB (adrianb) Should we name locally defined ones specially to avoid name conflicts?

	for (auto pTypestruct : pWork->arypTypestruct)
	{
		auto pLtypeStruct = LLVMStructCreateNamed(pGenx->pLctx, pTypestruct->pChzName);
		auto hv = HvFromKey(reinterpret_cast<u64>(pTypestruct));
		Add(&pGenx->hashPtypestructPltype, hv, pTypestruct, pLtypeStruct);
	}

	// Fill in all the structures

	for (auto pTypestruct : pWork->arypTypestruct)
	{
		auto pLtypeStruct = PltypeLookupStruct(pGenx, pTypestruct);

		int cMember = pTypestruct->cMember;
		auto apLtype = static_cast<LLVMOpaqueType **>(alloca(sizeof(LLVMOpaqueType *) * cMember));

		for (int iMember : IterCount(cMember))
		{
			auto pAstdecl = pTypestruct->aMember[iMember].pAstdecl;
			apLtype[iMember] = PltypeGenerate(pGenx, pAstdecl->tid);
		}

		LLVMBool fPacked = false; // BB (adrianb) Care about packed?
		LLVMStructSetBody(pLtypeStruct, apLtype, cMember, fPacked);
	}

	// Register and initialize globals
	// BB (adrianb) Do this on demand as they are used?

	for (auto pModule : IterPointer(pWork->aryModule))
	{
		for (auto pAst : pModule->pAstblockRoot->arypAst)
		{
			if (pAst->astk != ASTK_DeclareSingle)
				continue;

			// BB (adrianb) Keep storage for constants when they are things like strings or structs or whatnot? 
			auto pAstdecl = PastCast<SAstDeclareSingle>(pAst);
			if (pAstdecl->fIsConstant)
				continue;

			// BB (adrianb) Need to generate unique name?

			auto pLtype = PltypeGenerate(pGenx, pAstdecl->tid);
			auto pLvalGlobal = LLVMAddGlobal(pGenx->pLmod, pLtype, pAstdecl->pChzName);
			RegisterStorage(pGenx, pAstdecl, pLvalGlobal);

			// BB (adrianb) Need to generate unique name?
			// BB (adrianb) Setup global storage so compile time code can modify?
			// BB (adrianb) If marked as uninitialized just init to zero? Does that happen by default?

			auto pV = alloca(CbSizeOf(pAstdecl->tid));

			if (pAstdecl->pAstValue)
			{
				EvalConst(pGenx->pWork, pAstdecl->pAstValue, pV);
			}
			else
			{
				EvalDefaultValue(pWork, pAstdecl->tid, static_cast<u8 *>(pV));
			}

			// ValGenerateConstant either inline or memory for struct.
			//  Then just need to marshal value into llvm.
			//  #run just copies value to storage.

			// Marshall instance data into LLVM constant value


			LLVMSetInitializer(pLvalGlobal, PlvalConst(pGenx, pAstdecl->tid, pV));

			Reset(pGenx);
		}
	}

	for (auto pModule : IterPointer(pWork->aryModule))
	{
		// Generate code for functions in this module

		for (auto pAstproc : pModule->arypAstproc)
		{
			(void) PlvalGenerateRecursive(pGenx, pAstproc);

			Reset(pGenx);
		}
	}

	// Just create one bitcode file named after the root file
	// BB (adrianb) Is this ok for debugging?

	{
		char * pChzError = nullptr;
		if (LLVMVerifyModule(pGenx->pLmod, LLVMReturnStatusAction, &pChzError))
		{
			SStringBuilder strbLl = SStringBuilder("%s", PchzBaseName(PchzBuild(pWork)));
			PatchExt(&strbLl, ".ll");
			char * pChzErrWrite = nullptr;
			LLVMPrintModuleToFile(pGenx->pLmod, strbLl.aChz, &pChzErrWrite);
			ShowErrRaw("Found error in module %s:\n%s", strbLl.aChz, pChzError);
		}
		LLVMDisposeMessage(pChzError);
	}
}

void PrintToString(SStringBuilder * pStrb, const char * pChzFmt, va_list vargs)
{
	PrintV(pStrb, pChzFmt, vargs);
}

void CompileAndCheckDeclaration(
	const char * pChzTestName, const char * pChzDecl, 
	const char * pChzCode, const char * pChzAst, const char * pChzType,
	GRFWINIT grfwinit = GRFWINIT_None)
{
	// BB (adrianb) Allow getting errors so we can unit test error checking?

	SWorkspace work = {};
	InitWorkspace(&work, grfwinit);

	SModule * pModule = PtAppendNew(&work.aryModule);
	pModule->pChzFile = PchzCopy(&work, pChzTestName, strlen(pChzTestName));
	pModule->pChzContents = pChzCode;

	ParseAll(&work);
	TypeCheckAll(&work);

	SGenerateCtx genx = {};
	Init(&genx, &work);
	GenerateAll(&genx);

	auto pResdecl = PresdeclLookupSimple(&work.symtRoot, pChzDecl, RESLK_WithUsing);
	if (!pResdecl)
	{
		ShowErr(SErrorInfo{pChzTestName}, "Can't find declaration %s", pChzDecl);
	}
	
	auto pAstdecl = pResdecl->pDecl->pAstdecl;
	ASSERT(pAstdecl);
	SStringBuilder strb;
	SAstCtx acx = {};
	InitPrint(&acx.print, PrintToString, &strb);
	PrintSchemeAst(&acx, pAstdecl);

	if (strcmp(strb.aChz, pChzAst) != 0)
	{
		ShowErr(SErrorInfo{pChzTestName},
				"Declaration %s ast doesn't match:\n"
				" Expected \"%s\"\n"
				" Found    \"%s\"",
				pChzDecl, pChzAst, strb.aChz);
	}

	acx.fPrintedAnything = false;
	strb.aChz[0] = '\0';
	strb.cCh = 0;
	acx.fPrintType = true;
	PrintSchemeAst(&acx, pAstdecl);
	if (strcmp(strb.aChz, pChzType) != 0)
	{
		ShowErr(SErrorInfo{pChzTestName}, 
				"Declaration %s types don't match:\n"
				" Expected \"%s\"\n"
				" Found    \"%s\"\n"
				" For expression \"%s\"", 
				pChzDecl, pChzType, strb.aChz, pChzAst);
	}

	Destroy(&genx);
	Destroy(&work);
}

void RunUnitTests()
{
	// DWORD :: int; // int = type int
	//a :: 5; // 5 = int
	//b : int : 5; // b _type_ int :: int  Does explicit type mean anything?
	//LPDWORD :: #type * int; // type ptr int

	CompileAndCheckDeclaration("out_of_order", "a",
		"a := b; b : int : 5;",
		"(DeclareSingle var a infer-type 'b)", 
		"(DeclareSingle s32 infer-type s32)");

	// BB (adrianb) More compact type printing model?

	CompileAndCheckDeclaration("proc-simple", "a",
		"a :: (b : int) { }",
		"(DeclareSingle const a infer-type (Procedure (args (DeclareSingle var b 'int)) (Block)))", 
		"(DeclareSingle (Proc s32) infer-type (Procedure (Proc s32) (args (DeclareSingle s32 (Type s32))) (Block void)))");

	CompileAndCheckDeclaration("operator-lit", "a",
		"a := 5 + 1028;",
		"(DeclareSingle var a infer-type (+ 0x5 0x404))", 
		"(DeclareSingle s16 infer-type (+ s16 IntLit IntLit))");

	CompileAndCheckDeclaration("operator-intflt", "a",
		"a := 6 + 5.0;",
		"(DeclareSingle var a infer-type (+ 6 5))", 
		"(DeclareSingle f32 infer-type (+ f32 FloatLit FloatLit))");

	CompileAndCheckDeclaration("operator-intfltvar", "a",
		"b :: 5.0;"
		"a := 6 + b;",
		"(DeclareSingle var a infer-type (+ 6 'b))", 
		"(DeclareSingle f32 infer-type (+ f32 FloatLit f32))");

	CompileAndCheckDeclaration("operator-fltdbl", "a",
		"b :: 5.0;"
		"c : double : 5.0;"
		"a := b + c;",
		"(DeclareSingle var a infer-type (+ (Cast implicit 'b) 'c))", 
		"(DeclareSingle f64 infer-type (+ f64 (Cast f64 implicit f32) f64))");

	CompileAndCheckDeclaration("operator-bool", "a",
		"b :: true;"
		"c :: false;"
		"a := b != c;",
		"(DeclareSingle var a infer-type (!= 'b 'c))", 
		"(DeclareSingle bool infer-type (!= bool bool bool))");

	CompileAndCheckDeclaration("operator-compare", "a",
		"a := 5 < 6.5;",
		"(DeclareSingle var a infer-type (< 5 6.5))", 
		"(DeclareSingle bool infer-type (< bool FloatLit FloatLit))");

	CompileAndCheckDeclaration("operator-struct", "a",
		"S :: struct { a :: \"6.0\"; }"
		"a :: S.a;",
		"(DeclareSingle const a infer-type (. 'S 'a))", 
		"(DeclareSingle string infer-type (. string (Type S) string))");

	CompileAndCheckDeclaration("operator-pluseq", "Add",
		"Add :: (n : int) -> int { n += 5; return n; } ",
		"(DeclareSingle const Add infer-type (Procedure (args (DeclareSingle var n 'int)) (returns 'int) "
			"(Block (+= 'n 0x5) (Return 'n))))", 
		"(DeclareSingle (Proc s32 -> s32) infer-type (Procedure (Proc s32 -> s32) (args (DeclareSingle s32 (Type s32))) "
			"(returns (Type s32)) (Block void (+= void s32 IntLit) (Return s32 s32))))");
	
	CompileAndCheckDeclaration("operator-logand-precidence", "a",
		"a := 5 != 10 && true;",
		"(DeclareSingle var a infer-type (and (!= 0x5 0xa) true))", 
		"(DeclareSingle bool infer-type (and bool (!= bool IntLit IntLit) BoolLit))");

	CompileAndCheckDeclaration("extern-printf", "printf",
		"printf :: (format : * char, ..) -> int #foreign;",
		"(DeclareSingle const printf infer-type (Procedure (#foreign) (args (DeclareSingle var format (TypePointer 'char))"
		" (DeclareSingle var <no-name> ..)) (returns 'int)))",
		"(DeclareSingle (Proc (* u8) -> s32) infer-type (Procedure (Proc (* u8) -> s32)"
		" (args (DeclareSingle (* u8) (TypePointer (Type (* u8)) (Type u8))) (DeclareSingle .. (Type ..))) (returns (Type s32))))");

	CompileAndCheckDeclaration("global-string", "str",
		"str := \"hello string\";",
		"(DeclareSingle var str infer-type \"hello string\")",
		"(DeclareSingle string infer-type StringLit)");

	// Add support:
	// - Value result JIT

	// Unit tests for:
	// - All operators? Valid coercion cases?
	// - Recursive functions, implicit return from void function

	// BB (adrianb) Unit tests for common errors? How? Error current calls exit, throw instead? Sandbox
	//  allocation entirely within a custom malloc so we can just toss entire heap away? All arrays come
	//  from workspace? E.g. page based arrays? Use global pointer for allocation?
}

int main(int cpChzArg, const char * apChzArg[])
{
	static char s_aChzStdoutBuf[BUFSIZ] = {};
	setvbuf(stdout, s_aChzStdoutBuf, _IOFBF, DIM(s_aChzStdoutBuf));

#if 0
	TestDefer();
#endif

	// Process flags

	bool fDoneUsefulWork = false;
	bool fTraceAst = false;
	bool fTraceTypes = false;
	bool fWriteBitcode = false;
	int ipChz = 1;
	for (; ipChz < cpChzArg; ++ipChz)
	{
		const char * pChzArg = apChzArg[ipChz];
		if (pChzArg[0] != '-')
			break;

		if (strcmp(pChzArg, "-u") == 0 || strcmp(pChzArg, "--run-unit-tests") == 0)
		{
			RunUnitTests();
			fDoneUsefulWork = true;
		}
		else if (strcmp(pChzArg, "-s") == 0 || strcmp(pChzArg, "--print-syntax") == 0)
		{
			fTraceAst = true;
		}
		else if (strcmp(pChzArg, "-t") == 0 || strcmp(pChzArg, "--print-types") == 0)
		{
			fTraceTypes = true;
		}
		else if (strcmp(pChzArg, "-b") == 0 || strcmp(pChzArg, "--write-bitcode") == 0)
		{
			fWriteBitcode = true;
		}
		else
		{
			printf("Unknown option \"%s\", ignoring.\n", pChzArg);
		}
	}

	if (ipChz >= cpChzArg)
	{
		if (!fDoneUsefulWork)
		{
			printf("No file passed.\n");
			ShowHelp();
			return -1;
		}

		return 0;
	}

	const char * pChzFile = apChzArg[ipChz];

	SWorkspace work = {};
	InitWorkspace(&work, FWINIT_IncludeBuiltinModule);
	defer { Destroy(&work); };

	AddModuleFile(&work, pChzFile);
	ParseAll(&work);
	TypeCheckAll(&work);

	SGenerateCtx genx = {};
	Init(&genx, &work);
	defer { Destroy(&genx); };

	GenerateAll(&genx);

	// Link result into an executable
	//  Emit bitcode and link that into an exe with clang
	// BB (adrianb) This is pretty bizarre. I wish there were a library linker to use to avoid file IO.
	// BB (adrianb) Build actual path management.

	{
		// BB (adrianb) Less verbose way of building these using dtors? Also avoid stack craziness?
		//  E.g. string builder class that can destruct itself?

		SStringBuilder strbBc = SStringBuilder("%s", PchzBaseName(PchzBuild(&work))); //"output/";
		PatchExt(&strbBc, ".bc");

		ASSERT(genx.pLmod);
		if (LLVMWriteBitcodeToFile(genx.pLmod, strbBc.aChz) != 0)
		{
			printf("Failed to write bitcode file %s\n", strbBc.aChz);
		}
		else
		{
			SStringBuilder strbExe = SStringBuilder("%s", strbBc.aChz);
			PatchExt(&strbExe, "");

			SStringBuilder strbCmd;
			Print(&strbCmd, "clang -o %s %s", strbExe.aChz, strbBc.aChz);

			printf("Running command: %s\n", strbCmd.aChz);

			FILE * pFileCmdOut = popen(strbCmd.aChz, "r");

			if (pFileCmdOut == nullptr)
			{
				printf("Couldn't link bitcode file %s into executable %s\n", strbBc.aChz, strbExe.aChz);
			}
			else
			{
				char aChzOut[256];
				for (;;)
				{
					if (feof(pFileCmdOut))
						break;

					int cCh = fread(aChzOut, 1, DIM(aChzOut), pFileCmdOut);
					printf("%.*s", cCh, aChzOut);
				}

				auto pcloseresult = pclose(pFileCmdOut);
				auto nExit = WEXITSTATUS(pcloseresult);

				if (nExit < 0)
				{
					printf("Failed to compile %s\n", strbExe.aChz);
				}
				else
				{
					printf("Compiled %s\n", strbExe.aChz);
				}
			}
		}
	}

	if (fTraceAst)
	{
		printf("Tracing AST for all modules\n");
		for (const SModule & module : work.aryModule)
		{
			printf("\nModule %s\n", module.pChzFile);
			SAstCtx acx = {};
			InitPrint(&acx.print, PrintFile, stdout);
			PrintSchemeAst(&acx, module.pAstblockRoot);
			printf("\n");
		}
	}

	if (fTraceTypes)
	{
		printf("Tracing type AST for all modules\n");
		for (const SModule & module : work.aryModule)
		{
			printf("\nModule %s\n", module.pChzFile);
			SAstCtx acx = {};
			acx.fPrintType = true;
			InitPrint(&acx.print, PrintFile, stdout);
			PrintSchemeAst(&acx, module.pAstblockRoot);
			printf("\n");
		}
	}

	if (fWriteBitcode && genx.pLmod)
	{
		// BB (adrianb) Full path management. Write to output directory?

		SStringBuilder strbLl = SStringBuilder("%s", PchzBaseName(PchzBuild(&work)));
		PatchExt(&strbLl, ".ll");

		char * pChzError = nullptr;
		if (LLVMPrintModuleToFile(genx.pLmod, strbLl.aChz, &pChzError) == 0)
		{
			printf("Write out file %s\n", strbLl.aChz);
		}
		else	
		{
			printf("Failed writing out file %s\n  %s\n", strbLl.aChz, pChzError);
		}
		if (pChzError)
			LLVMDisposeMessage(pChzError);
	}

#if 0
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
	#endif

	// BB (adrianb) Use dladdr, dlclose, dlerror, dlopen to open close processes.

	return 0;
}

#if 0
	- Default int literals to s64? or u64?
	- Add dynamic array.
	- Add array literal syntax.
	- Add struct init syntax? E.g. StructName(named parameter arguments)?
	- Add enums.
	- Add unsized integer type (treated as 64 bit, 65?). So that 1 << 8 can morph into something else if need be.
	- Add overloading.
	- #run. Using tree evaluator and global memory allocations.
	- Named parameters? Default values?
	- Dll loading.
		- OSX can do this implicitly: https://developer.apple.com/library/mac/documentation/DeveloperTools/Conceptual/DynamicLibraries/100-Articles/CreatingDynamicLibraries.html
	- Example program to test? OpenGL game?
	- Flesh out operators - bitwise operators
	- using with procedures, pointers.
	- Any/typeinfo. printf replacement
	- SOA support.
	- Inlining.  In generation.
	- overloading.
	- Polymorphic procedures. With modify proc.
	- #bake, #bake_values with procedures?
	- Polymorphic structs. With modify? 
	- Stuff from D: mixins, static if (esp mixins), lazy arguments
	- Closures?
	- implicit context
	- here strings? #string ENDTOKEN\nANYTHING HERE\nENDTOKEN
	- Annotations?
	- Expose typed compiler AST to compile time run procedures ala BANANAS demo. Ouch...
	- Debugger GUI?
	- Conditional compilation.

Problems:
	- Procedure ambiguity
		- Arguments vs multiple return values: (func: (A,B)->B, name: A) {...} 
		- Determine if a definition is intended to use parentheses or be a function definition 
			need to peek 2+ more operators
			- x :: (a + b)
			- y :: (a: int) {}
	- Switch to explicit declarations? var/let, def for const, proc for procedures
		e.g. var x : s32 = 5 or var x = 5;
#endif
