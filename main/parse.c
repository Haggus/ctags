/*
*   Copyright (c) 1996-2003, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains functions for managing input languages and
*   dispatching files to the appropriate language parser.
*/

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

/* TODO: This definition should be removed. */
#define OPTION_WRITE
#include "options_p.h"

#include <string.h>

#include "ctags.h"
#include "debug.h"
#include "entry_p.h"
#include "field_p.h"
#include "flags_p.h"
#include "htable.h"
#include "keyword.h"
#include "lxpath_p.h"
#include "main_p.h"
#include "param.h"
#include "param_p.h"
#include "parse_p.h"
#include "parsers_p.h"
#include "promise.h"
#include "promise_p.h"
#include "ptag_p.h"
#include "ptrarray.h"
#include "read.h"
#include "read_p.h"
#include "routines.h"
#include "routines_p.h"
#include "subparser.h"
#include "subparser_p.h"
#include "trace.h"
#include "trashbox.h"
#include "trashbox_p.h"
#include "vstring.h"
#ifdef HAVE_ICONV
# include "mbcs_p.h"
#endif
#include "xtag_p.h"

/*
 * DATA TYPES
 */
enum specType {
	SPEC_NONE,
	SPEC_NAME,
	SPEC_ALIAS = SPEC_NAME,
	SPEC_EXTENSION,
	SPEC_PATTERN,
};
const char *specTypeName [] = {
	"none", "name", "extension", "pattern"
};

typedef struct {
	langType lang;
	const char* spec;
	enum specType specType;
}  parserCandidate;

typedef struct sParserObject {
	parserDefinition *def;

	kindDefinition* fileKind;

	stringList* currentPatterns;   /* current list of file name patterns */
	stringList* currentExtensions; /* current list of extensions */
	stringList* currentAliases;    /* current list of aliases */

	unsigned int initialized:1;    /* initialize() is called or not */
	unsigned int dontEmit:1;	   /* run but don't emit tags
									  (a subparser requests run this parser.) */
	unsigned int pseudoTagPrinted:1;   /* pseudo tags about this parser
										  is emitted or not. */

	unsigned int anonymousIdentiferId; /* managed by anon* functions */

	struct slaveControlBlock *slaveControlBlock;
	struct kindControlBlock  *kindControlBlock;
	struct lregexControlBlock *lregexControlBlock;
} parserObject;

/*
 * FUNCTION PROTOTYPES
 */

static void lazyInitialize (langType language);
static void addParserPseudoTags (langType language);
static void installKeywordTable (const langType language);
static void installTagRegexTable (const langType language);
static void installTagXpathTable (const langType language);
static void anonResetMaybe (parserObject *parser);
static void setupAnon (void);
static void teardownAnon (void);

/*
*   DATA DEFINITIONS
*/
static parserDefinition *CTagsSelfTestParser (void);
static parserDefinitionFunc* BuiltInParsers[] = {
	CTagsSelfTestParser,
	PARSER_LIST,
	XML_PARSER_LIST
#ifdef HAVE_LIBXML
	,
#endif
	YAML_PARSER_LIST
#ifdef HAVE_LIBYAML
	,
#endif
};
static parserObject* LanguageTable = NULL;
static unsigned int LanguageCount = 0;
static hashTable* LanguageHTable = NULL;
static kindDefinition defaultFileKind = {
	.enabled     = false,
	.letter      = KIND_FILE_DEFAULT,
	.name        = KIND_FILE_DEFAULT_LONG,
	.description = KIND_FILE_DEFAULT_LONG,
};

/*
*   FUNCTION DEFINITIONS
*/

static bool isLanguageNameChar(int c)
{
	if (isgraph(c))
	{
		if (c == '\'' || c == '"' || c == ';')
			return false;
		return true;
	}
	else
		return false;
}

extern unsigned int countParsers (void)
{
	return LanguageCount;
}

extern int makeSimpleTag (
		const vString* const name, const int kindIndex)
{
	return makeSimpleRefTag (name, kindIndex, ROLE_INDEX_DEFINITION);
}

extern int makeSimpleRefTag (const vString* const name, const int kindIndex,
			     int roleIndex)
{
	int r = CORK_NIL;

	Assert (roleIndex < (int)countInputLanguageRoles(kindIndex));

	/* do not check for kind being disabled - that happens later in makeTagEntry() */
	if (name != NULL  &&  vStringLength (name) > 0)
	{
	    tagEntryInfo e;
	    initRefTagEntry (&e, vStringValue (name), kindIndex, roleIndex);

	    r = makeTagEntry (&e);
	}
	return r;
}

extern bool isLanguageEnabled (const langType language)
{
	const parserDefinition* const lang = LanguageTable [language].def;

	if (!lang->enabled)
		return false;

	if ((lang->kindTable == NULL) &&
		(!(lang->method & METHOD_REGEX)) &&
		(!(lang->method & METHOD_XPATH)))
		return false;
	else
		return true;
}

extern bool isLanguageVisible (const langType language)
{
	const parserDefinition* const lang = LanguageTable [language].def;

	return !lang->invisible;
}

/*
*   parserDescription mapping management
*/

extern parserDefinition* parserNew (const char* name)
{
	parserDefinition* result = xCalloc (1, parserDefinition);
	result->name = eStrdup (name);

	result->enabled = true;
	return result;
}

extern bool doesLanguageAllowNullTag (const langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	return LanguageTable [language].def->allowNullTag;
}

extern bool doesLanguageRequestAutomaticFQTag (const langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	return LanguageTable [language].def->requestAutomaticFQTag;
}

extern const char *getLanguageName (const langType language)
{
	const char* result;
	if (language == LANG_IGNORE)
		result = "unknown";
	else
	{
		Assert (0 <= language  &&  language < (int) LanguageCount);
		result = LanguageTable [language].def->name;
	}
	return result;
}

extern const char *getLanguageKindName (const langType language, const int kindIndex)
{
	kindDefinition* kdef = getLanguageKind (language, kindIndex);
	return kdef->name;
}

static kindDefinition kindGhost = {
	.letter = KIND_GHOST,
	.name = KIND_GHOST_LONG,
	.description = KIND_GHOST_LONG,
};

extern int defineLanguageKind (const langType language, kindDefinition *def,
							   freeKindDefFunc freeKindDef)
{
	return defineKind (LanguageTable [language].kindControlBlock, def, freeKindDef);
}

extern unsigned int countLanguageKinds (const langType language)
{
	return countKinds (LanguageTable [language].kindControlBlock);
}

extern unsigned int countLanguageRoles (const langType language, int kindIndex)
{
	return countRoles (LanguageTable [language].kindControlBlock, kindIndex);
}

extern kindDefinition* getLanguageKind (const langType language, int kindIndex)
{
	kindDefinition* kdef;

	Assert (0 <= language  &&  language < (int) LanguageCount);

	switch (kindIndex)
	{
	case KIND_FILE_INDEX:
		kdef = LanguageTable [language].fileKind;
		break;
	case KIND_GHOST_INDEX:
		kdef = &kindGhost;
		break;
	default:
		Assert (kindIndex >= 0);
		kdef = getKind (LanguageTable [language].kindControlBlock, kindIndex);
	}
	return kdef;
}

extern kindDefinition* getLanguageKindForLetter (const langType language, char kindLetter)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	if (kindLetter == LanguageTable [language].fileKind->letter)
		return LanguageTable [language].fileKind;
	else if (kindLetter == KIND_GHOST)
		return &kindGhost;
	else
		return getKindForLetter (LanguageTable [language].kindControlBlock, kindLetter);
}

extern kindDefinition* getLanguageKindForName (const langType language, const char *kindName)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	Assert (kindName);

	if (strcmp(kindName, LanguageTable [language].fileKind->name) == 0)
		return LanguageTable [language].fileKind;
	else if (strcmp(kindName, KIND_GHOST_LONG) == 0)
		return &kindGhost;
	else
		return getKindForName (LanguageTable [language].kindControlBlock, kindName);
}

extern roleDefinition* getLanguageRole(const langType language, int kindIndex, int roleIndex)
{
	return getRole (LanguageTable [language].kindControlBlock, kindIndex, roleIndex);
}

extern roleDefinition* getLanguageRoleForName (const langType language, int kindIndex,
											   const char *roleName)
{
	return getRoleForName (LanguageTable [language].kindControlBlock, kindIndex, roleName);
}

extern langType getNamedLanguage (const char *const name, size_t len)
{
	langType result = LANG_IGNORE;
	unsigned int i;
	Assert (name != NULL);

	if (len == 0)
	{
		parserDefinition *def = (parserDefinition *)hashTableGetItem (LanguageHTable, name);
		if (def)
			result = def->id;
	}
	else
		for (i = 0  ;  i < LanguageCount  &&  result == LANG_IGNORE  ;  ++i)
		{
			const parserDefinition* const lang = LanguageTable [i].def;
			Assert (lang->name);
			vString* vstr = vStringNewInit (name);
			vStringTruncate (vstr, len);

			if (strcasecmp (vStringValue (vstr), lang->name) == 0)
				result = i;
			vStringDelete (vstr);
		}
	return result;
}

static langType getNameOrAliasesLanguageAndSpec (const char *const key, langType start_index,
						 const char **const spec, enum specType *specType)
{
	langType result = LANG_IGNORE;
	unsigned int i;


	if (start_index == LANG_AUTO)
	        start_index = 0;
	else if (start_index == LANG_IGNORE || start_index >= (int) LanguageCount)
		return result;

	for (i = start_index  ;  i < LanguageCount  &&  result == LANG_IGNORE  ;  ++i)
	{
		const parserObject* const parser = LanguageTable + i;
		stringList* const aliases = parser->currentAliases;
		vString* tmp;

		/* isLanguageEnabled is not used here.
		   It calls initializeParser which takes
		   cost. */
		if (! parser->def->enabled)
			continue;

		if (parser->def->name != NULL && strcasecmp (key, parser->def->name) == 0)
		{
			result = i;
			*spec = parser->def->name;
			*specType = SPEC_NAME;
		}
		else if (aliases != NULL  &&  (tmp = stringListFileFinds (aliases, key)))
		{
			result = i;
			*spec = vStringValue(tmp);
			*specType = SPEC_ALIAS;
		}
	}
	return result;
}

extern langType getLanguageForCommand (const char *const command, langType startFrom)
{
	const char *const tmp_command = baseFilename (command);
	char *tmp_spec;
	enum specType tmp_specType;

	return getNameOrAliasesLanguageAndSpec (tmp_command, startFrom,
											(const char **const)&tmp_spec,
											&tmp_specType);
}

static langType getPatternLanguageAndSpec (const char *const baseName, langType start_index,
					   const char **const spec, enum specType *specType)
{
	langType result = LANG_IGNORE;
	unsigned int i;

	if (start_index == LANG_AUTO)
	        start_index = 0;
	else if (start_index == LANG_IGNORE || start_index >= (int) LanguageCount)
		return result;

	*spec = NULL;
	for (i = start_index  ;  i < LanguageCount  &&  result == LANG_IGNORE  ;  ++i)
	{
		parserObject *parser = LanguageTable + i;
		stringList* const ptrns = parser->currentPatterns;
		vString* tmp;

		/* isLanguageEnabled is not used here.
		   It calls initializeParser which takes
		   cost. */
		if (! parser->def->enabled)
			continue;

		if (ptrns != NULL && (tmp = stringListFileFinds (ptrns, baseName)))
		{
			result = i;
			*spec = vStringValue(tmp);
			*specType = SPEC_PATTERN;
			goto found;
		}
	}

	for (i = start_index  ;  i < LanguageCount  &&  result == LANG_IGNORE  ;  ++i)
	{
		parserObject *parser = LanguageTable + i;
		stringList* const exts = parser->currentExtensions;
		vString* tmp;

		/* isLanguageEnabled is not used here.
		   It calls initializeParser which takes
		   cost. */
		if (! parser->def->enabled)
			continue;

		if (exts != NULL && (tmp = stringListExtensionFinds (exts,
								     fileExtension (baseName))))
		{
			result = i;
			*spec = vStringValue(tmp);
			*specType = SPEC_EXTENSION;
			goto found;
		}
	}
found:
	return result;
}

extern langType getLanguageForFilename (const char *const filename, langType startFrom)
{
	const char *const tmp_filename = baseFilename (filename);
	char *tmp_spec;
	enum specType tmp_specType;

	return getPatternLanguageAndSpec (tmp_filename, startFrom,
									  (const char **const)&tmp_spec,
									  &tmp_specType);
}

const char *scopeSeparatorFor (langType language, int kindIndex, int parentKindIndex)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);

	parserObject *parser = LanguageTable + language;
	struct kindControlBlock *kcb = parser->kindControlBlock;

	const scopeSeparator *sep = getScopeSeparator (kcb, kindIndex, parentKindIndex);
	return sep? sep->separator: NULL;
}

static parserCandidate* parserCandidateNew(unsigned int count CTAGS_ATTR_UNUSED)
{
	parserCandidate* candidates;
	unsigned int i;

	candidates= xMalloc(LanguageCount, parserCandidate);
	for (i = 0; i < LanguageCount; i++)
	{
		candidates[i].lang = LANG_IGNORE;
		candidates[i].spec = NULL;
		candidates[i].specType = SPEC_NONE;
	}
	return candidates;
}

/* If multiple parsers are found, return LANG_AUTO */
static unsigned int nominateLanguageCandidates (const char *const key, parserCandidate** candidates)
{
	unsigned int count;
	langType i;
	const char* spec = NULL;
	enum specType specType = SPEC_NONE;

	*candidates = parserCandidateNew(LanguageCount);

	for (count = 0, i = LANG_AUTO; i != LANG_IGNORE; )
	{
		i = getNameOrAliasesLanguageAndSpec (key, i, &spec, &specType);
		if (i != LANG_IGNORE)
		{
			(*candidates)[count].lang = i++;
			(*candidates)[count].spec = spec;
			(*candidates)[count++].specType = specType;
		}
	}

	return count;
}

static unsigned int
nominateLanguageCandidatesForPattern(const char *const baseName, parserCandidate** candidates)
{
	unsigned int count;
	langType i;
	const char* spec;
	enum specType specType = SPEC_NONE;

	*candidates = parserCandidateNew(LanguageCount);

	for (count = 0, i = LANG_AUTO; i != LANG_IGNORE; )
	{
		i = getPatternLanguageAndSpec (baseName, i, &spec, &specType);
		if (i != LANG_IGNORE)
		{
			(*candidates)[count].lang = i++;
			(*candidates)[count].spec = spec;
			(*candidates)[count++].specType = specType;
		}
	}
	return count;
}

static vString* extractEmacsModeAtFirstLine(MIO* input);

/*  The name of the language interpreter, either directly or as the argument
 *  to "env".
 */
static vString* determineInterpreter (const char* const cmd)
{
	vString* const interpreter = vStringNew ();
	const char* p = cmd;
	do
	{
		vStringClear (interpreter);
		for ( ;  isspace ((int) *p)  ;  ++p)
			;  /* no-op */
		for ( ;  *p != '\0'  &&  ! isspace ((int) *p)  ;  ++p)
			vStringPut (interpreter, (int) *p);
	} while (strcmp (vStringValue (interpreter), "env") == 0);
	return interpreter;
}

static vString* extractInterpreter (MIO* input)
{
	vString* const vLine = vStringNew ();
	const char* const line = readLineRaw (vLine, input);
	vString* interpreter = NULL;

	if (line != NULL  &&  line [0] == '#'  &&  line [1] == '!')
	{
		/* "48.2.4.1 Specifying File Variables" of Emacs info:
		   ---------------------------------------------------
		   In shell scripts, the first line is used to
		   identify the script interpreter, so you
		   cannot put any local variables there.  To
		   accommodate this, Emacs looks for local
		   variable specifications in the _second_
		   line if the first line specifies an
		   interpreter.  */

		interpreter = extractEmacsModeAtFirstLine(input);
		if (!interpreter)
		{
			const char* const lastSlash = strrchr (line, '/');
			const char *const cmd = lastSlash != NULL ? lastSlash+1 : line+2;
			interpreter = determineInterpreter (cmd);
		}
	}
	vStringDelete (vLine);
	return interpreter;
}

static vString* determineEmacsModeAtFirstLine (const char* const line)
{
	vString* mode = vStringNew ();

	const char* p = strstr(line, "-*-");
	if (p == NULL)
		goto out;
	p += strlen("-*-");

	for ( ;  isspace ((int) *p)  ;  ++p)
		;  /* no-op */

	if (strncasecmp(p, "mode:", strlen("mode:")) == 0)
	{
		/* -*- mode: MODE; -*- */
		p += strlen("mode:");
		for ( ;  isspace ((int) *p)  ;  ++p)
			;  /* no-op */
		for ( ;  *p != '\0'  &&  isLanguageNameChar ((int) *p)  ;  ++p)
			vStringPut (mode, (int) *p);
	}
	else
	{
		/* -*- MODE -*- */
		const char* end = strstr (p, "-*-");

		if (end == NULL)
			goto out;

		for ( ;  p < end &&  isLanguageNameChar ((int) *p)  ;  ++p)
			vStringPut (mode, (int) *p);

		for ( ;  isspace ((int) *p)  ;  ++p)
			;  /* no-op */
		if (strncmp(p, "-*-", strlen("-*-")) != 0)
			vStringClear (mode);
	}

	vStringLower (mode);

out:
	return mode;

}

static vString* extractEmacsModeAtFirstLine(MIO* input)
{
	vString* const vLine = vStringNew ();
	const char* const line = readLineRaw (vLine, input);
	vString* mode = NULL;
	if (line != NULL)
		mode = determineEmacsModeAtFirstLine (line);
	vStringDelete (vLine);

	if (mode && (vStringLength(mode) == 0))
	{
		vStringDelete(mode);
		mode = NULL;
	}
	return mode;
}

static vString* determineEmacsModeAtEOF (MIO* const fp)
{
	vString* const vLine = vStringNew ();
	const char* line;
	bool headerFound = false;
	const char* p;
	vString* mode = vStringNew ();

	while ((line = readLineRaw (vLine, fp)) != NULL)
	{
		if (headerFound && ((p = strstr (line, "mode:")) != NULL))
		{
			vStringClear (mode);
			headerFound = false;

			p += strlen ("mode:");
			for ( ;  isspace ((int) *p)  ;  ++p)
				;  /* no-op */
			for ( ;  *p != '\0'  &&  isLanguageNameChar ((int) *p)  ;  ++p)
				vStringPut (mode, (int) *p);
		}
		else if (headerFound && (p = strstr(line, "End:")))
			headerFound = false;
		else if (strstr (line, "Local Variables:"))
			headerFound = true;
	}
	vStringDelete (vLine);
	return mode;
}

static vString* extractEmacsModeLanguageAtEOF (MIO* input)
{
	vString* mode;

	/* "48.2.4.1 Specifying File Variables" of Emacs info:
	   ---------------------------------------------------
	   you can define file local variables using a "local
	   variables list" near the end of the file.  The start of the
	   local variables list should be no more than 3000 characters
	   from the end of the file, */
	mio_seek(input, -3000, SEEK_END);

	mode = determineEmacsModeAtEOF (input);
	if (mode && (vStringLength (mode) == 0))
	{
		vStringDelete (mode);
		mode = NULL;
	}

	return mode;
}

static vString* determineVimFileType (const char *const modeline)
{
	/* considerable combinations:
	   --------------------------
	   ... filetype=
	   ... ft= */

	unsigned int i;
	const char* p;

	const char* const filetype_prefix[] = {"filetype=", "ft="};
	vString* const filetype = vStringNew ();

	for (i = 0; i < ARRAY_SIZE(filetype_prefix); i++)
	{
		if ((p = strrstr(modeline, filetype_prefix[i])) == NULL)
			continue;

		p += strlen(filetype_prefix[i]);
		for ( ;  *p != '\0'  &&  isalnum ((int) *p)  ;  ++p)
			vStringPut (filetype, (int) *p);
		break;
	}
	return filetype;
}

static vString* extractVimFileType(MIO* input)
{
	/* http://vimdoc.sourceforge.net/htmldoc/options.html#modeline

	   [text]{white}{vi:|vim:|ex:}[white]se[t] {options}:[text]
	   options=> filetype=TYPE or ft=TYPE

	   'modelines' 'mls'	number	(default 5)
			global
			{not in Vi}
	    If 'modeline' is on 'modelines' gives the number of lines that is
	    checked for set commands. */

	vString* filetype = NULL;
#define RING_SIZE 5
	vString* ring[RING_SIZE];
	int i, j;
	unsigned int k;
	const char* const prefix[] = {
		"vim:", "vi:", "ex:"
	};

	for (i = 0; i < RING_SIZE; i++)
		ring[i] = vStringNew ();

	i = 0;
	while ((readLineRaw (ring[i++], input)) != NULL)
		if (i == RING_SIZE)
			i = 0;

	j = i;
	do
	{
		const char* p;

		j--;
		if (j < 0)
			j = RING_SIZE - 1;

		for (k = 0; k < ARRAY_SIZE(prefix); k++)
			if ((p = strstr (vStringValue (ring[j]), prefix[k])) != NULL)
			{
				p += strlen(prefix[k]);
				for ( ;  isspace ((int) *p)  ;  ++p)
					;  /* no-op */
				filetype = determineVimFileType(p);
				break;
			}
	} while (((i == RING_SIZE)? (j != RING_SIZE - 1): (j != i)) && (!filetype));

	for (i = RING_SIZE - 1; i >= 0; i--)
		vStringDelete (ring[i]);
#undef RING_SIZE

	if (filetype && (vStringLength (filetype) == 0))
	{
		vStringDelete (filetype);
		filetype = NULL;
	}
	return filetype;

	/* TODO:
	   [text]{white}{vi:|vim:|ex:}[white]{options} */
}

static vString* extractMarkGeneric (MIO* input,
									vString * (* determiner)(const char *const, void *),
									void *data)
{
	vString* const vLine = vStringNew ();
	const char* const line = readLineRaw (vLine, input);
	vString* mode = NULL;

	if (line)
		mode = determiner (line, data);

	vStringDelete (vLine);
	return mode;
}

static vString* determineZshAutoloadTag (const char *const modeline,
										 void *data CTAGS_ATTR_UNUSED)
{
	/* See "Autoloaded files" in zsh info.
	   -------------------------------------
	   #compdef ...
	   #autoload [ OPTIONS ] */

	if (((strncmp (modeline, "#compdef", 8) == 0) && isspace (*(modeline + 8)))
	    || ((strncmp (modeline, "#autoload", 9) == 0)
		&& (isspace (*(modeline + 9)) || *(modeline + 9) == '\0')))
		return vStringNewInit ("zsh");
	else
		return NULL;
}

static vString* extractZshAutoloadTag(MIO* input)
{
	return extractMarkGeneric (input, determineZshAutoloadTag, NULL);
}

static vString* determinePHPMark(const char *const modeline,
		void *data CTAGS_ATTR_UNUSED)
{
	if (strncmp (modeline, "<?php", 5) == 0)
		return vStringNewInit ("php");
	else
		return NULL;
}

static vString* extractPHPMark(MIO* input)
{
	return extractMarkGeneric (input, determinePHPMark, NULL);
}


struct getLangCtx {
    const char *fileName;
    MIO        *input;
    bool     err;
};

#define GLC_FOPEN_IF_NECESSARY0(_glc_, _label_) do {        \
    if (!(_glc_)->input) {                                  \
	    (_glc_)->input = getMio((_glc_)->fileName, "rb", false);	\
        if (!(_glc_)->input) {                              \
            (_glc_)->err = true;                            \
            goto _label_;                                   \
        }                                                   \
    }                                                       \
} while (0)                                                 \

#define GLC_FOPEN_IF_NECESSARY(_glc_, _label_, _doesParserRequireMemoryStream_) \
	do {								\
		if (!(_glc_)->input)					\
			GLC_FOPEN_IF_NECESSARY0 (_glc_, _label_);	\
		if ((_doesParserRequireMemoryStream_) &&		\
		    (mio_memory_get_data((_glc_)->input, NULL) == NULL)) \
		{							\
			MIO *tmp_ = (_glc_)->input;			\
			(_glc_)->input = mio_new_mio (tmp_, 0, -1);	\
			mio_free (tmp_);				\
			if (!(_glc_)->input) {				\
				(_glc_)->err = true;			\
				goto _label_;				\
			}						\
		}							\
	} while (0)

#define GLC_FCLOSE(_glc_) do {                              \
    if ((_glc_)->input) {                                   \
        mio_free((_glc_)->input);                             \
        (_glc_)->input = NULL;                              \
    }                                                       \
} while (0)

static const struct taster {
	vString* (* taste) (MIO *);
        const char     *msg;
} eager_tasters[] = {
        {
		.taste  = extractInterpreter,
		.msg    = "interpreter",
        },
	{
		.taste  = extractZshAutoloadTag,
		.msg    = "zsh autoload tag",
	},
        {
		.taste  = extractEmacsModeAtFirstLine,
		.msg    = "emacs mode at the first line",
        },
        {
		.taste  = extractEmacsModeLanguageAtEOF,
		.msg    = "emacs mode at the EOF",
        },
        {
		.taste  = extractVimFileType,
		.msg    = "vim modeline",
        },
		{
		.taste  = extractPHPMark,
		.msg    = "PHP marker",
		}
};
static langType tasteLanguage (struct getLangCtx *glc, const struct taster *const tasters, int n_tasters,
			      langType *fallback);

/* If all the candidates have the same specialized language selector, return
 * it.  Otherwise, return NULL.
 */
static bool
hasTheSameSelector (langType lang, selectLanguage candidate_selector)
{
	selectLanguage *selector;

	selector = LanguageTable[ lang ].def->selectLanguage;
	if (selector == NULL)
		return false;

	while (*selector)
	{
		if (*selector == candidate_selector)
			return true;
		selector++;
	}
	return false;
}

static selectLanguage
commonSelector (const parserCandidate *candidates, int n_candidates)
{
    Assert (n_candidates > 1);
    selectLanguage *selector;
    int i;

    selector = LanguageTable[ candidates[0].lang ].def->selectLanguage;
    if (selector == NULL)
	    return NULL;

    while (*selector)
    {
	    for (i = 1; i < n_candidates; ++i)
		    if (! hasTheSameSelector (candidates[i].lang, *selector))
			    break;
	    if (i == n_candidates)
		    return *selector;
	    selector++;
    }
    return NULL;
}


/* Calls the selector and returns the integer value of the parser for the
 * language associated with the string returned by the selector.
 */
static int
pickLanguageBySelection (selectLanguage selector, MIO *input,
						 parserCandidate *candidates,
						 unsigned int nCandidates)
{
	const char *lang;
	langType *cs = xMalloc(nCandidates, langType);
	unsigned int i;

	for (i = 0; i < nCandidates; i++)
		cs[i] = candidates[i].lang;
    lang = selector(input, cs, nCandidates);
	eFree (cs);

    if (lang)
    {
        verbose ("		selection: %s\n", lang);
        return getNamedLanguage(lang, 0);
    }
    else
    {
	verbose ("		no selection\n");
        return LANG_IGNORE;
    }
}

static int compareParsersByName (const void *a, const void* b)
{
	const parserDefinition *const *la = a, *const *lb = b;
	return strcasecmp ((*la)->name, (*lb)->name);
}

static int sortParserCandidatesBySpecType (const void *a, const void *b)
{
	const parserCandidate *ap = a, *bp = b;
	if (ap->specType > bp->specType)
		return -1;
	else if (ap->specType == bp->specType)
	{
		/* qsort, the function calling this function,
		   doesn't do "stable sort". To make the result of
		   sorting predictable, compare the names of parsers
		   when their specType is the same. */
		parserDefinition *la = LanguageTable [ap->lang].def;
		parserDefinition *lb = LanguageTable [bp->lang].def;
		return compareParsersByName (&la, &lb);
	}
	else
		return 1;
}

static unsigned int sortAndFilterParserCandidates (parserCandidate  *candidates,
						   unsigned int n_candidates)
{
	enum specType highestSpecType;
	unsigned int i;
	unsigned int r;

	if (n_candidates < 2)
		return n_candidates;

	qsort (candidates, n_candidates, sizeof(*candidates),
	       sortParserCandidatesBySpecType);

	highestSpecType = candidates [0].specType;
	r = 1;
	for (i = 1; i < n_candidates; i++)
	{
		if (candidates[i].specType == highestSpecType)
			r++;
	}
	return r;
}

static void verboseReportCandidate (const char *header,
				    parserCandidate  *candidates,
				    unsigned int n_candidates)
{
	unsigned int i;
	verbose ("		#%s: %u\n", header, n_candidates);
	for (i = 0; i < n_candidates; i++)
		verbose ("			%u: %s (%s: \"%s\")\n",
			 i,
			 LanguageTable[candidates[i].lang].def->name,
			 specTypeName [candidates[i].specType],
			 candidates[i].spec);
}

static bool doesCandidatesRequireMemoryStream(const parserCandidate *candidates,
						 int n_candidates)
{
	int i;

	for (i = 0; i < n_candidates; i++)
		if (doesParserRequireMemoryStream (candidates[i].lang))
			return true;

	return false;
}

static langType getSpecLanguageCommon (const char *const spec, struct getLangCtx *glc,
				       unsigned int nominate (const char *const, parserCandidate**),
				       langType *fallback)
{
	langType language;
	parserCandidate  *candidates;
	unsigned int n_candidates;

	if (fallback)
		*fallback = LANG_IGNORE;

	n_candidates = (*nominate)(spec, &candidates);
	verboseReportCandidate ("candidates",
				candidates, n_candidates);

	n_candidates = sortAndFilterParserCandidates (candidates, n_candidates);
	verboseReportCandidate ("candidates after sorting and filtering",
				candidates, n_candidates);

	if (n_candidates == 1)
	{
		language = candidates[0].lang;
	}
	else if (n_candidates > 1)
	{
		selectLanguage selector = commonSelector(candidates, n_candidates);
		bool memStreamRequired = doesCandidatesRequireMemoryStream (candidates,
									       n_candidates);

		GLC_FOPEN_IF_NECESSARY(glc, fopen_error, memStreamRequired);
		if (selector) {
			verbose ("	selector: %p\n", selector);
			language = pickLanguageBySelection(selector, glc->input, candidates, n_candidates);
		} else {
			verbose ("	selector: NONE\n");
		fopen_error:
			language = LANG_IGNORE;
		}

		Assert(language != LANG_AUTO);

		if (fallback)
			*fallback = candidates[0].lang;
	}
	else
	{
		language = LANG_IGNORE;
	}

	eFree(candidates);
	candidates = NULL;

	return language;
}

static langType getSpecLanguage (const char *const spec,
                                 struct getLangCtx *glc,
				 langType *fallback)
{
	return getSpecLanguageCommon(spec, glc, nominateLanguageCandidates,
				     fallback);
}

static langType getPatternLanguage (const char *const baseName,
                                    struct getLangCtx *glc,
				    langType *fallback)
{
	return getSpecLanguageCommon(baseName, glc,
				     nominateLanguageCandidatesForPattern,
				     fallback);
}

/* This function tries to figure out language contained in a file by
 * running a series of tests, trying to find some clues in the file.
 */
static langType
tasteLanguage (struct getLangCtx *glc, const struct taster *const tasters, int n_tasters,
	      langType *fallback)
{
    int i;

    if (fallback)
	    *fallback = LANG_IGNORE;
    for (i = 0; i < n_tasters; ++i) {
        langType language;
        vString* spec;

        mio_rewind(glc->input);
	spec = tasters[i].taste(glc->input);

        if (NULL != spec) {
            verbose ("	%s: %s\n", tasters[i].msg, vStringValue (spec));
            language = getSpecLanguage (vStringValue (spec), glc,
					(fallback && (*fallback == LANG_IGNORE))? fallback: NULL);
            vStringDelete (spec);
            if (language != LANG_IGNORE)
                return language;
        }
    }

    return LANG_IGNORE;
}


struct GetLanguageRequest {
	enum { GLR_OPEN, GLR_DISCARD, GLR_REUSE, } type;
	const char *const fileName;
	MIO *mio;
};

static langType
getFileLanguageForRequestInternal (struct GetLanguageRequest *req)
{
	const char *const fileName = req->fileName;
    langType language;

    /* ctags tries variety ways(HINTS) to choose a proper language
       for given fileName. If multiple candidates are chosen in one of
       the hint, a SELECTOR common between the candidate languages
       is called.

       "selection failure" means a selector common between the
       candidates doesn't exist or the common selector returns NULL.

       "hint failure" means the hint finds no candidate or
       "selection failure" occurs though the hint finds multiple
       candidates.

       If a hint chooses multiple candidates, and selection failure is
       occurred, the hint records one of the candidates as FALLBACK for
       the hint. (The candidates are stored in an array. The first
       element of the array is recorded. However, there is no
       specification about the order of elements in the array.)

       If all hints are failed, FALLBACKs of the hints are examined.
       Which fallbacks should be chosen?  `enum hint' defines the order. */
    enum hint {
	    HINT_INTERP,
	    HINT_OTHER,
	    HINT_FILENAME,
	    HINT_TEMPLATE,
	    N_HINTS,
    };
    langType fallback[N_HINTS];
    int i;
    struct getLangCtx glc = {
        .fileName = fileName,
        .input    = (req->type == GLR_REUSE)? mio_ref (req->mio): NULL,
        .err      = false,
    };
    const char* const baseName = baseFilename (fileName);
    char *templateBaseName = NULL;
    fileStatus *fstatus = NULL;

    for (i = 0; i < N_HINTS; i++)
	fallback [i] = LANG_IGNORE;

    verbose ("Get file language for %s\n", fileName);

    verbose ("	pattern: %s\n", baseName);
    language = getPatternLanguage (baseName, &glc,
				   fallback + HINT_FILENAME);
    if (language != LANG_IGNORE || glc.err)
        goto cleanup;

    {
        const char* const tExt = ".in";
        templateBaseName = baseFilenameSansExtensionNew (fileName, tExt);
        if (templateBaseName)
        {
            verbose ("	pattern + template(%s): %s\n", tExt, templateBaseName);
            GLC_FOPEN_IF_NECESSARY(&glc, cleanup, false);
            mio_rewind(glc.input);
            language = getPatternLanguage(templateBaseName, &glc,
					  fallback + HINT_TEMPLATE);
            if (language != LANG_IGNORE)
                goto cleanup;
        }
    }

	/* If the input is already opened, we don't have to verify the existence. */
    if (glc.input || ((fstatus = eStat (fileName)) && fstatus->exists))
    {
	    if ((fstatus && fstatus->isExecutable) || Option.guessLanguageEagerly)
	    {
		    GLC_FOPEN_IF_NECESSARY (&glc, cleanup, false);
		    language = tasteLanguage(&glc, eager_tasters, 1,
					    fallback + HINT_INTERP);
	    }
	    if (language != LANG_IGNORE)
		    goto cleanup;

	    if (Option.guessLanguageEagerly)
	    {
		    GLC_FOPEN_IF_NECESSARY(&glc, cleanup, false);
		    language = tasteLanguage(&glc,
					     eager_tasters + 1,
					     ARRAY_SIZE(eager_tasters) - 1,
					     fallback + HINT_OTHER);
	    }
    }


  cleanup:
	if (req->type == GLR_OPEN && glc.input)
		req->mio = mio_ref (glc.input);
    GLC_FCLOSE(&glc);
    if (fstatus)
	    eStatFree (fstatus);
    if (templateBaseName)
        eFree (templateBaseName);

    for (i = 0;
	 language == LANG_IGNORE && i < N_HINTS;
	 i++)
    {
        language = fallback [i];
	if (language != LANG_IGNORE)
        verbose ("	fallback[hint = %d]: %s\n", i, getLanguageName (language));
    }

    return language;
}

static langType getFileLanguageForRequest (struct GetLanguageRequest *req)
{
	langType l = Option.language;

	if (l == LANG_AUTO)
		return getFileLanguageForRequestInternal(req);
	else if (! isLanguageEnabled (l))
	{
		error (FATAL,
		       "%s parser specified with --language-force is disabled",
		       getLanguageName (l));
		/* For suppressing warnings. */
		return LANG_AUTO;
	}
	else
		return Option.language;
}

extern langType getLanguageForFilenameAndContents (const char *const fileName)
{
	struct GetLanguageRequest req = {
		.type = GLR_DISCARD,
		.fileName = fileName,
	};

	return getFileLanguageForRequest (&req);
}

typedef void (*languageCallback)  (langType language, void* user_data);
static void foreachLanguage(languageCallback callback, void *user_data)
{
	langType result = LANG_IGNORE;

	unsigned int i;
	for (i = 0  ;  i < LanguageCount  &&  result == LANG_IGNORE  ;  ++i)
	{
		const parserDefinition* const lang = LanguageTable [i].def;
		if (lang->name != NULL)
			callback(i, user_data);
	}
}

static void printLanguageMap (const langType language, FILE *fp)
{
	bool first = true;
	unsigned int i;
	parserObject *parser = LanguageTable + language;
	stringList* map = parser->currentPatterns;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	for (i = 0  ;  map != NULL  &&  i < stringListCount (map)  ;  ++i)
	{
		fprintf (fp, "%s(%s)", (first ? "" : " "),
			 vStringValue (stringListItem (map, i)));
		first = false;
	}
	map = parser->currentExtensions;
	for (i = 0  ;  map != NULL  &&  i < stringListCount (map)  ;  ++i)
	{
		fprintf (fp, "%s.%s", (first ? "" : " "),
			 vStringValue (stringListItem (map, i)));
		first = false;
	}
}

extern void installLanguageMapDefault (const langType language)
{
	parserObject* parser;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	if (parser->currentPatterns != NULL)
		stringListDelete (parser->currentPatterns);
	if (parser->currentExtensions != NULL)
		stringListDelete (parser->currentExtensions);

	if (parser->def->patterns == NULL)
		parser->currentPatterns = stringListNew ();
	else
	{
		parser->currentPatterns =
			stringListNewFromArgv (parser->def->patterns);
	}
	if (parser->def->extensions == NULL)
		parser->currentExtensions = stringListNew ();
	else
	{
		parser->currentExtensions =
			stringListNewFromArgv (parser->def->extensions);
	}
	BEGIN_VERBOSE(vfp);
	{
	printLanguageMap (language, vfp);
	putc ('\n', vfp);
	}
	END_VERBOSE();
}

extern void installLanguageMapDefaults (void)
{
	unsigned int i;
	for (i = 0  ;  i < LanguageCount  ;  ++i)
	{
		verbose ("    %s: ", getLanguageName (i));
		installLanguageMapDefault (i);
	}
}

extern void installLanguageAliasesDefault (const langType language)
{
	parserObject* parser;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	if (parser->currentAliases != NULL)
		stringListDelete (parser->currentAliases);

	if (parser->def->aliases == NULL)
		parser->currentAliases = stringListNew ();
	else
	{
		parser->currentAliases =
			stringListNewFromArgv (parser->def->aliases);
	}
	BEGIN_VERBOSE(vfp);
	if (parser->currentAliases != NULL)
		for (unsigned int i = 0  ;  i < stringListCount (parser->currentAliases)  ;  ++i)
			fprintf (vfp, " %s", vStringValue (
						 stringListItem (parser->currentAliases, i)));
	putc ('\n', vfp);
	END_VERBOSE();
}

extern void installLanguageAliasesDefaults (void)
{
	unsigned int i;
	for (i = 0  ;  i < LanguageCount  ;  ++i)
	{
		verbose ("    %s: ", getLanguageName (i));
		installLanguageAliasesDefault (i);
	}
}

extern void clearLanguageMap (const langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	stringListClear ((LanguageTable + language)->currentPatterns);
	stringListClear ((LanguageTable + language)->currentExtensions);
}

extern void clearLanguageAliases (const langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);

	parserObject* parser = (LanguageTable + language);
	if (parser->currentAliases)
		stringListClear (parser->currentAliases);
}

static bool removeLanguagePatternMap1(const langType language, const char *const pattern)
{
	bool result = false;
	stringList* const ptrn = (LanguageTable + language)->currentPatterns;

	if (ptrn != NULL && stringListDeleteItemExtension (ptrn, pattern))
	{
		verbose (" (removed from %s)", getLanguageName (language));
		result = true;
	}
	return result;
}

extern bool removeLanguagePatternMap (const langType language, const char *const pattern)
{
	bool result = false;

	if (language == LANG_AUTO)
	{
		unsigned int i;
		for (i = 0  ;  i < LanguageCount  &&  ! result ;  ++i)
			result = removeLanguagePatternMap1 (i, pattern) || result;
	}
	else
		result = removeLanguagePatternMap1 (language, pattern);
	return result;
}

extern void addLanguagePatternMap (const langType language, const char* ptrn,
				   bool exclusiveInAllLanguages)
{
	vString* const str = vStringNewInit (ptrn);
	parserObject* parser;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	if (exclusiveInAllLanguages)
		removeLanguagePatternMap (LANG_AUTO, ptrn);
	stringListAdd (parser->currentPatterns, str);
}

static bool removeLanguageExtensionMap1 (const langType language, const char *const extension)
{
	bool result = false;
	stringList* const exts = (LanguageTable + language)->currentExtensions;

	if (exts != NULL  &&  stringListDeleteItemExtension (exts, extension))
	{
		verbose (" (removed from %s)", getLanguageName (language));
		result = true;
	}
	return result;
}

extern bool removeLanguageExtensionMap (const langType language, const char *const extension)
{
	bool result = false;

	if (language == LANG_AUTO)
	{
		unsigned int i;
		for (i = 0  ;  i < LanguageCount ;  ++i)
			result = removeLanguageExtensionMap1 (i, extension) || result;
	}
	else
		result = removeLanguageExtensionMap1 (language, extension);
	return result;
}

extern void addLanguageExtensionMap (
		const langType language, const char* extension,
		bool exclusiveInAllLanguages)
{
	vString* const str = vStringNewInit (extension);
	Assert (0 <= language  &&  language < (int) LanguageCount);
	if (exclusiveInAllLanguages)
		removeLanguageExtensionMap (LANG_AUTO, extension);
	stringListAdd ((LanguageTable + language)->currentExtensions, str);
}

extern void addLanguageAlias (const langType language, const char* alias)
{
	vString* const str = vStringNewInit (alias);
	parserObject* parser;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	if (parser->currentAliases == NULL)
		parser->currentAliases = stringListNew ();
	stringListAdd (parser->currentAliases, str);
}

extern void enableLanguage (const langType language, const bool state)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	LanguageTable [language].def->enabled = state;
}

#ifdef DO_TRACING
extern void traceLanguage (langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	LanguageTable [language].def->traced = true;
}
extern bool isLanguageTraced (langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	return LanguageTable [language].def->traced;
}
#endif /* DO_TRACING */

extern void enableLanguages (const bool state)
{
	unsigned int i;
	for (i = 0  ;  i < LanguageCount  ;  ++i)
		enableLanguage (i, state);
}

static void installFieldDefinition (const langType language)
{
	unsigned int i;
	parserDefinition * parser;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable [language].def;

	if (parser->fieldTable != NULL)
	{
		for (i = 0; i < parser->fieldCount; i++)
			defineField (& parser->fieldTable [i], language);
	}
}

static void installXtagDefinition (const langType language)
{
	unsigned int i;
	parserDefinition * parser;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable [language].def;

	if (parser->xtagTable != NULL)
	{
		for (i = 0; i < parser->xtagCount; i++)
			defineXtag (& parser->xtagTable [i], language);
	}
}

static void initializeParserOne (langType lang)
{
	parserObject *const parser = LanguageTable + lang;

	if (parser->initialized)
		goto out;

	verbose ("Initialize parser: %s\n", parser->def->name);
	parser->initialized = true;

	installKeywordTable (lang);
	installTagXpathTable (lang);
	installFieldDefinition     (lang);
	installXtagDefinition      (lang);

	/* regex definitions refers xtag definitions.
	   So installing RegexTable must be after installing
	   xtag definitions. */
	installTagRegexTable (lang);

	if (parser->def->initialize != NULL)
		parser->def->initialize (lang);

	initializeDependencies (parser->def, parser->slaveControlBlock);

	Assert (parser->fileKind != NULL);
	Assert (!doesParserUseKind (parser->kindControlBlock, parser->fileKind->letter));

	return;

 out:
	/* lazyInitialize() installs findRegexTags() to parser->parser.
	   findRegexTags() should be installed to a parser if the parser is
	   optlib based(created by --langdef) and has some regex patterns(defined
	   with --regex-<LANG>). findRegexTags() makes regex matching work.

	   If a parser can be initialized during evaluating options,
	   --fields-<LANG>=+{something}, for an example.
	   If such option is evaluated first, evaluating --regex-<LANG>=...
	   option doesn't cause installing findRegexTags. As the result
	   regex matching doesn't work. lazyInitialize was called only
	   once when --fields-<LANG>=+{something} was evaluated. In the
	   timing ctags had not seen --regex-<LANG>=.... Even though
	   ctags saw --regex-<LANG>=.... after initializing, there
	   was no chance to install findRegexTags() to parser->parser.

	   Following code block gives extra chances to call lazyInitialize)
	   which installs findRegexTags() to parser->parser.	 */
	if (parser->def->initialize == lazyInitialize)
		parser->def->initialize (lang);
}

extern void initializeParser (langType lang)
{
	if (lang == LANG_AUTO)
	{
		unsigned int i;
		for (i = 0; i < countParsers(); i++)
			initializeParserOne (i);
	}
	else
		initializeParserOne (lang);
}

static void linkDependenciesAtInitializeParsing (parserDefinition *const parser)
{
	unsigned int i;
	parserDependency *d;
	langType upper;
	parserObject *upperParser;

	for (i = 0; i < parser->dependencyCount; i++)
	{
		d = parser->dependencies + i;
		upper = getNamedLanguage (d->upperParser, 0);
		upperParser = LanguageTable + upper;

		linkDependencyAtInitializeParsing (d->type, upperParser->def,
										   upperParser->slaveControlBlock,
										   upperParser->kindControlBlock,
										   parser,
										   (LanguageTable + parser->id)->kindControlBlock,
										   d->data);
	}
}

/* Used in both builtin and optlib parsers. */
static void initializeParsingCommon (parserDefinition *def, bool is_builtin)
{
	parserObject *parser;

	if (is_builtin)
		verbose ("%s%s", LanguageCount > 0 ? ", " : "", def->name);
	else
		verbose ("Add optlib parser: %s\n", def->name);

	def->id = LanguageCount++;
	parser = LanguageTable + def->id;
	parser->def = def;

	hashTablePutItem (LanguageHTable, def->name, def);

	parser->fileKind = &defaultFileKind;

	parser->kindControlBlock  = allocKindControlBlock (def);
	parser->slaveControlBlock = allocSlaveControlBlock (def);
	parser->lregexControlBlock = allocLregexControlBlock (def);
}

extern void initializeParsing (void)
{
	unsigned int builtInCount;
	unsigned int i;

	builtInCount = ARRAY_SIZE (BuiltInParsers);
	LanguageTable = xMalloc (builtInCount, parserObject);
	memset(LanguageTable, 0, builtInCount * sizeof (parserObject));

	LanguageHTable = hashTableNew (127,
								   hashCstrcasehash,
								   hashCstrcaseeq,
								   NULL,
								   NULL);
	DEFAULT_TRASH_BOX(LanguageHTable, hashTableDelete);

	verbose ("Installing parsers: ");
	for (i = 0  ;  i < builtInCount  ;  ++i)
	{
		parserDefinition* const def = (*BuiltInParsers [i]) ();
		if (def != NULL)
		{
			bool accepted = false;
			if (def->name == NULL  ||  def->name[0] == '\0')
				error (FATAL, "parser definition must contain name\n");
			else if (def->method & METHOD_NOT_CRAFTED)
			{
				def->parser = findRegexTags;
				accepted = true;
			}
			else if ((!def->invisible) && (((!!def->parser) + (!!def->parser2)) != 1))
				error (FATAL,
		"%s parser definition must define one and only one parsing routine\n",
					   def->name);
			else
				accepted = true;
			if (accepted)
				initializeParsingCommon (def, true);
		}
	}
	verbose ("\n");

	for (i = 0; i < builtInCount  ;  ++i)
		linkDependenciesAtInitializeParsing (LanguageTable [i].def);
}

extern void freeParserResources (void)
{
	unsigned int i;
	for (i = 0  ;  i < LanguageCount  ;  ++i)
	{
		parserObject* const parser = LanguageTable + i;

		if (parser->def->finalize)
			(parser->def->finalize)((langType)i, (bool)parser->initialized);

		freeLregexControlBlock (parser->lregexControlBlock);
		freeKindControlBlock (parser->kindControlBlock);
		parser->kindControlBlock = NULL;

		finalizeDependencies (parser->def, parser->slaveControlBlock);
		freeSlaveControlBlock (parser->slaveControlBlock);
		parser->slaveControlBlock = NULL;

		freeList (&parser->currentPatterns);
		freeList (&parser->currentExtensions);
		freeList (&parser->currentAliases);

		eFree (parser->def->name);
		parser->def->name = NULL;
		eFree (parser->def);
		parser->def = NULL;
	}
	if (LanguageTable != NULL)
		eFree (LanguageTable);
	LanguageTable = NULL;
	LanguageCount = 0;
}

static void doNothing (void)
{
}

static void optlibRunBaseParser (void)
{
	scheduleRunningBaseparser (0);
}

static bool optlibIsDedicatedSubparser (parserDefinition* def)
{
	return (def->dependencies
			&& (def->dependencies->type == DEPTYPE_SUBPARSER)
			&& ((subparser *)def->dependencies->data)->direction & SUBPARSER_SUB_RUNS_BASE);
}

static void lazyInitialize (langType language)
{
	parserDefinition* def;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	def = LanguageTable [language].def;

	def->parser = doNothing;

	if (def->method & METHOD_REGEX)
	{
		if (optlibIsDedicatedSubparser (def))
			def->parser = optlibRunBaseParser;
		else
			def->parser = findRegexTags;
	}
}

extern void enableDefaultFileKind (bool state)
{
	defaultFileKind.enabled = state;
}

/*
*   Option parsing
*/
struct preLangDefFlagData
{
	char *base;
	subparserRunDirection direction;
	bool autoFQTag;
};

static void pre_lang_def_flag_base_long (const char* const optflag, const char* const param, void* data)
{
	struct preLangDefFlagData * flag_data = data;
	langType base;

	if (param[0] == '\0')
	{
		error (WARNING, "No base parser specified for \"%s\" flag of --langdef option", optflag);
		return;
	}

	base = getNamedLanguage (param, 0);
	if (base == LANG_IGNORE)
	{
		error (WARNING, "Unknown language(%s) is specified for \"%s\" flag of --langdef option",
			   param, optflag);
		return;

	}

	flag_data->base = eStrdup(param);
}

#define LANGDEF_FLAG_DEDICATED "dedicated"
#define LANGDEF_FLAG_SHARED    "shared"
#define LANGDEF_FLAG_BIDIR     "bidirectional"
static void pre_lang_def_flag_direction_long (const char* const optflag, const char* const param CTAGS_ATTR_UNUSED, void* data)
{
	struct preLangDefFlagData * flag_data = data;

	if (strcmp(optflag, LANGDEF_FLAG_DEDICATED) == 0)
		flag_data->direction = SUBPARSER_SUB_RUNS_BASE;
	else if (strcmp(optflag, LANGDEF_FLAG_SHARED) == 0)
		flag_data->direction = SUBPARSER_BASE_RUNS_SUB;
	else if (strcmp(optflag, LANGDEF_FLAG_BIDIR) == 0)
		flag_data->direction = SUBPARSER_BI_DIRECTION;
	else
		AssertNotReached ();
}

static void pre_lang_def_flag_autoFQTag_long (const char* const optflag,
											  const char* const param CTAGS_ATTR_UNUSED,
											  void* data)
{
	struct preLangDefFlagData * flag_data = data;
	flag_data->autoFQTag = true;
}

static flagDefinition PreLangDefFlagDef [] = {
	{ '\0',  "base", NULL, pre_lang_def_flag_base_long,
	  "BASEPARSER", "utilize as a base parser"},
	{ '\0',  LANGDEF_FLAG_DEDICATED,  NULL,
	  pre_lang_def_flag_direction_long,
	  NULL, "make the base parser dedicated to this subparser"},
	{ '\0',  LANGDEF_FLAG_SHARED,     NULL,
	  pre_lang_def_flag_direction_long,
	  NULL, "share the base parser with the other subparsers"
	},
	{ '\0',  LANGDEF_FLAG_BIDIR,      NULL,
	  pre_lang_def_flag_direction_long,
	  NULL, "utilize the base parser both 'dedicated' and 'shared' way"
	},
	{ '\0',  "_autoFQTag", NULL, pre_lang_def_flag_autoFQTag_long,
	  NULL, "make full qualified tags automatically based on scope information"},
};

static void optlibFreeDep (langType lang, bool initialized CTAGS_ATTR_UNUSED)
{
	parserDefinition * pdef = LanguageTable [lang].def;

	if (pdef->dependencyCount == 1)
	{
		parserDependency *dep = pdef->dependencies;

		eFree ((char *)dep->upperParser); /* Dirty cast */
		dep->upperParser = NULL;
		eFree (dep->data);
		dep->data = NULL;
		eFree (dep);
		pdef->dependencies = NULL;
	}
}

static parserDefinition* OptlibParser(const char *name, const char *base,
									  subparserRunDirection direction)
{
	parserDefinition *def;

	def = parserNew (name);
	def->initialize        = lazyInitialize;
	def->method            = METHOD_NOT_CRAFTED;
	if (base)
	{
		subparser *sub = xCalloc (1, subparser);
		parserDependency *dep = xCalloc (1, parserDependency);

		sub->direction = direction;
		dep->type = DEPTYPE_SUBPARSER;
		dep->upperParser = eStrdup (base);
		dep->data = sub;
		def->dependencies = dep;
		def->dependencyCount = 1;
		def->finalize = optlibFreeDep;
	}

	return def;
}

extern void processLanguageDefineOption (
		const char *const option, const char *const parameter)
{
	if (parameter [0] == '\0')
		error (WARNING, "No language specified for \"%s\" option", option);
	else if (getNamedLanguage (parameter, 0) != LANG_IGNORE)
		error (WARNING, "Language \"%s\" already defined", parameter);
	else
	{
		char *name;
		char *flags;
		parserDefinition*  def;

		flags = strchr (parameter, LONG_FLAGS_OPEN);
		if (flags)
			name = eStrndup (parameter, flags - parameter);
		else
			name = eStrdup (parameter);

		LanguageTable = xRealloc (LanguageTable, LanguageCount + 1, parserObject);
		memset (LanguageTable + LanguageCount, 0, sizeof(parserObject));

		struct preLangDefFlagData data = {
			.base = NULL,
			.direction = SUBPARSER_UNKNOWN_DIRECTION,
			.autoFQTag = false,
		};
		flagsEval (flags, PreLangDefFlagDef, ARRAY_SIZE (PreLangDefFlagDef), &data);

		if (data.base == NULL && data.direction != SUBPARSER_UNKNOWN_DIRECTION)
			error (WARNING, "Ignore the direction of subparser because \"{base=}\" is not given");

		if (data.base && data.direction == SUBPARSER_UNKNOWN_DIRECTION)
			data.direction = SUBPARSER_BASE_RUNS_SUB;

		def = OptlibParser (name, data.base, data.direction);
		if (data.base)
			eFree (data.base);

		def->requestAutomaticFQTag = data.autoFQTag;

		initializeParsingCommon (def, false);
		linkDependenciesAtInitializeParsing (def);

		LanguageTable [def->id].currentPatterns = stringListNew ();
		LanguageTable [def->id].currentExtensions = stringListNew ();

		eFree (name);
	}
}

extern bool isLanguageKindEnabled (const langType language, int kindIndex)
{
	kindDefinition * kdef = getLanguageKind (language, kindIndex);
	return kdef->enabled;
}

extern bool isLanguageRoleEnabled (const langType language, int kindIndex, int roleIndex)
{
	return isRoleEnabled(LanguageTable [language].kindControlBlock,
						 kindIndex, roleIndex);
}

extern bool isLanguageKindRefOnly (const langType language, int kindIndex)
{
	kindDefinition * def =  getLanguageKind(language, kindIndex);
	return def->referenceOnly;
}

static void resetLanguageKinds (const langType language, const bool mode)
{
	const parserObject* parser;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;

	{
		unsigned int i;
		struct kindControlBlock *kcb = parser->kindControlBlock;

		for (i = 0  ;  i < countKinds (kcb)  ;  ++i)
		{
			kindDefinition *kdef = getKind (kcb, i);
			enableKind (kdef, mode);
		}
	}
}

static bool enableLanguageKindForLetter (
		const langType language, const int kind, const bool mode)
{
	bool result = false;
	kindDefinition* const def = getLanguageKindForLetter (language, kind);
	if (def != NULL)
	{
		enableKind (def, mode);
		result = true;
	}
	return result;
}

static bool enableLanguageKindForName (
	const langType language, const char * const name, const bool mode)
{
	bool result = false;
	kindDefinition* const def = getLanguageKindForName (language, name);
	if (def != NULL)
	{
		enableKind (def, mode);
		result = true;
	}
	return result;
}

static void processLangKindDefinition (
		const langType language, const char *const option,
		const char *const parameter)
{
	const char *p = parameter;
	bool mode = true;
	int c;
	static vString *longName;
	bool inLongName = false;
	const char *k;
	bool r;

	Assert (0 <= language  &&  language < (int) LanguageCount);

	initializeParser (language);
	if (*p == '*')
	{
		resetLanguageKinds (language, true);
		p++;
	}
	else if (*p != '+'  &&  *p != '-')
		resetLanguageKinds (language, false);

	longName = vStringNewOrClearWithAutoRelease (longName);

	while ((c = *p++) != '\0')
	{
		switch (c)
		{
		case '+':
			if (inLongName)
				vStringPut (longName, c);
			else
				mode = true;
			break;
		case '-':
			if (inLongName)
				vStringPut (longName, c);
			else
				mode = false;
			break;
		case '{':
			if (inLongName)
				error(FATAL,
				      "unexpected character in kind specification: \'%c\'",
				      c);
			inLongName = true;
			break;
		case '}':
			if (!inLongName)
				error(FATAL,
				      "unexpected character in kind specification: \'%c\'",
				      c);
			k = vStringValue (longName);
			r = enableLanguageKindForName (language, k, mode);
			if (! r)
				error (WARNING, "Unsupported kind: '%s' for --%s option",
				       k, option);

			inLongName = false;
			vStringClear (longName);
			break;
		default:
			if (inLongName)
				vStringPut (longName, c);
			else
			{
				r = enableLanguageKindForLetter (language, c, mode);
				if (! r)
					error (WARNING, "Unsupported kind: '%c' for --%s option",
					       c, option);
			}
			break;
		}
	}
}

static void freeKdef (kindDefinition *kdef)
{
	eFree (kdef->name);
	eFree (kdef->description);
	eFree (kdef);
}

static char *extractDescriptionAndFlags(const char *input, const char **flags)
{
	vString *vdesc = vStringNew();
	bool escaped = false;

	if (flags)
		*flags = NULL;

	while (*input != '\0')
	{
		if (escaped)
		{
			vStringPut (vdesc, *input);
			escaped = false;

		}
		else if (*input == '\\')
			escaped = true;
		else if (*input == LONG_FLAGS_OPEN)
		{
			if (flags)
				*flags = input;
			break;
		}
		else
			vStringPut (vdesc, *input);
		input++;
	}
	return vStringDeleteUnwrap(vdesc);
}

static void pre_kind_def_flag_refonly_long (const char* const optflag,
											const char* const param, void* data)
{
	kindDefinition *kdef = data;
	kdef->referenceOnly = true;
}

static flagDefinition PreKindDefFlagDef [] = {
	{ '\0', "_refonly", NULL, pre_kind_def_flag_refonly_long,
	  NULL, "use this kind reference tags only"},
};

static bool processLangDefineKind(const langType language,
								  const char *const option,
								  const char *const parameter)
{
	parserObject *parser;

	kindDefinition *kdef;
	int letter;
	const char * p = parameter;
	char *name;
	char *description;
	const char *tmp_start;
	const char *tmp_end;
	size_t tmp_len;
	const char *flags;


	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;

	Assert (p);

	if (p[0] == '\0')
		error (FATAL, "no kind definition specified in \"--%s\" option", option);

	letter = p[0];
	if (letter == ',')
		error (FATAL, "no kind letter specified in \"--%s\" option", option);
	if (!isalnum (letter))
		error (FATAL, "the kind letter give in \"--%s\" option is not an alphabet or a number", option);
	else if (letter == KIND_FILE_DEFAULT)
		error (FATAL, "the kind letter `F' in \"--%s\" option is reserved for \"file\" kind", option);
	else if (getKindForLetter (parser->kindControlBlock, letter))
	{
		error (WARNING, "the kind for letter `%c' specified in \"--%s\" option is already defined.",
			   letter, option);
		return true;
	}

	if (p[1] != ',')
		error (FATAL, "wrong kind definition in \"--%s\" option: no comma after letter", option);

	p += 2;
	if (p[0] == '\0')
		error (FATAL, "no kind name specified in \"--%s\" option", option);
	tmp_end = strchr (p, ',');
	if (!tmp_end)
		error (FATAL, "no kind description specified in \"--%s\" option", option);

	tmp_start = p;
	while (p != tmp_end)
	{
		if (!isalnum (*p))
			error (FATAL, "unacceptable char as part of kind name in \"--%s\" option", option);
		p++;
	}

	if (tmp_end == tmp_start)
		error (FATAL, "the kind name in \"--%s\" option is empty", option);

	tmp_len = tmp_end - tmp_start;
	if (strncmp (tmp_start, KIND_FILE_DEFAULT_LONG, tmp_len) == 0)
		error (FATAL,
			   "the kind name " KIND_FILE_DEFAULT_LONG " in \"--%s\" option is reserved",
			   option);

	name = eStrndup (tmp_start, tmp_len);
	if (getKindForName (parser->kindControlBlock, name))
	{
		error (WARNING, "the kind for name `%s' specified in \"--%s\" option is already defined.",
			   name, option);
		eFree (name);
		return true;
	}

	p++;
	if (p [0] == '\0' || p [0] == LONG_FLAGS_OPEN)
		error (FATAL, "found an empty kind description in \"--%s\" option", option);

	description = extractDescriptionAndFlags (p, &flags);

	kdef = xCalloc (1, kindDefinition);
	kdef->enabled = true;
	kdef->letter = letter;
	kdef->name = name;
	kdef->description = description;
	if (flags)
		flagsEval (flags, PreKindDefFlagDef, ARRAY_SIZE (PreKindDefFlagDef), kdef);

	defineKind (parser->kindControlBlock, kdef, freeKdef);
	return true;
}

static void freeRdef (roleDefinition *rdef)
{
	eFree (rdef->name);
	eFree (rdef->description);
	eFree (rdef);
}

static bool processLangDefineRole(const langType language,
								  const char *const option,
								  const char *const parameter)
{
	parserObject *parser;

	kindDefinition *kdef;
	roleDefinition *rdef;
	int kletter;
	const char * p = parameter;
	char *name;
	char *description;
	const char *tmp_start;
	const char *tmp_end;
	const char *flags;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;

	Assert (p);

	if (p[0] == '\0')
		error (FATAL, "no role definition specified in \"--%s\" option", option);

	kletter = p[0];
	if (kletter == '.')
		error (FATAL, "no kind letter specified in \"--%s\" option", option);
	if (!isalnum (kletter))
		error (FATAL, "the kind letter give in \"--%s\" option is not an alphabet or a number", option);
	else if (kletter == KIND_FILE_DEFAULT)
		error (FATAL, "the kind letter `F' in \"--%s\" option is reserved for \"file\" kind and no role can be attached to", option);

	kdef = getKindForLetter (parser->kindControlBlock, kletter);

	if (kdef == NULL)
	{
		error (WARNING, "the kind for letter `%c' specified in \"--%s\" option is not defined.",
			   kletter, option);
		return true;
	}

	if (p[1] != '.')
		error (FATAL, "wrong role definition in \"--%s\" option: no period after kind letter `%c'",
			   option, kletter);

	p += 2;
	if (p[0] == '\0')
		error (FATAL, "no role name specified in \"--%s\" option", option);
	tmp_end = strchr (p, ',');
	if (!tmp_end)
		error (FATAL, "no role description specified in \"--%s\" option", option);

	tmp_start = p;
	while (p != tmp_end)
	{
		if (!isalnum (*p))
			error (FATAL, "unacceptable char as part of role name in \"--%s\" option", option);
		p++;
	}

	if (tmp_end == tmp_start)
		error (FATAL, "the role name in \"--%s\" option is empty", option);

	name = eStrndup (tmp_start, tmp_end - tmp_start);
	if (getRoleForName (parser->kindControlBlock, kdef->id, name))
	{
		error (WARNING, "the role for name `%s' specified in \"--%s\" option is already defined.",
			   name, option);
		eFree (name);
		return true;
	}

	p++;
	if (p [0] == '\0' || p [0] == LONG_FLAGS_OPEN)
		error (FATAL, "found an empty role description in \"--%s\" option", option);

	description = extractDescriptionAndFlags (p, &flags);

	rdef = xCalloc (1, roleDefinition);
	rdef->enabled = true;
	rdef->name = name;
	rdef->description = description;

	if (flags)
		flagsEval (flags, NULL, 0, rdef);

	defineRole (parser->kindControlBlock, kdef->id, rdef, freeRdef);

	return true;
}

extern bool processKinddefOption (const char *const option, const char * const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "kinddef-");
	if (language == LANG_IGNORE)
		return false;

	return processLangDefineKind (language, option, parameter);
}

extern bool processRoledefOption (const char *const option, const char * const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "_roledef-");
	if (language == LANG_IGNORE)
		return false;

	return processLangDefineRole (language, option, parameter);
}

struct langKindDefinitionStruct {
	const char *const option;
	const char *const parameter;
};
static void processLangKindDefinitionEach(
	langType lang, void* user_data)
{
	struct langKindDefinitionStruct *arg = user_data;
	processLangKindDefinition (lang, arg->option, arg->parameter);
}

extern bool processKindsOption (
		const char *const option, const char *const parameter)
{
#define PREFIX "kinds-"
#define PREFIX_LEN strlen(PREFIX)

	bool handled = false;
	struct langKindDefinitionStruct arg = {
		.option = option,
		.parameter = parameter,
	};
	langType language;

	const char* const dash = strchr (option, '-');
	if (dash != NULL  &&
		(strcmp (dash + 1, "kinds") == 0  ||  strcmp (dash + 1, "types") == 0))
	{
		size_t len = dash - option;

		if ((len == 3) && (strncmp (option, RSV_LANG_ALL, len) == 0))
			foreachLanguage(processLangKindDefinitionEach, &arg);
		else
		{
			language = getNamedLanguage (option, len);
			if (language == LANG_IGNORE)
			{
				char *langName = eStrndup (option, len);
				error (WARNING, "Unknown language \"%s\" in \"%s\" option", langName, option);
				eFree (langName);
			}
			else
				processLangKindDefinition (language, option, parameter);
		}
		handled = true;
	}
	else if ( strncmp (option, PREFIX, PREFIX_LEN) == 0 )
	{
		const char* lang;

		lang = option + PREFIX_LEN;
		if (lang[0] == '\0')
			error (WARNING, "No language given in \"%s\" option", option);
		else if (strcmp (lang, RSV_LANG_ALL) == 0)
			foreachLanguage(processLangKindDefinitionEach, &arg);
		else
		{
			language = getNamedLanguage (lang, 0);
			if (language == LANG_IGNORE)
				error (WARNING, "Unknown language \"%s\" in \"%s\" option", lang, option);
			else
				processLangKindDefinition (language, option, parameter);
		}
		handled = true;
	}
	return handled;
#undef PREFIX
#undef PREFIX_LEN
}

extern void printLanguageRoles (const langType language, const char* kindspecs,
								bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable *table = roleColprintTableNew();
	parserObject *parser;

	initializeParser (language);

	if (language == LANG_AUTO)
	{
		for (unsigned int i = 0  ;  i < LanguageCount  ;  ++i)
		{
			if (!isLanguageVisible (i))
				continue;

			parser = LanguageTable + i;
			roleColprintAddRoles (table, parser->kindControlBlock, kindspecs);
		}
	}
	else
	{
		parser = LanguageTable + language;
		roleColprintAddRoles (table, parser->kindControlBlock, kindspecs);
	}

	roleColprintTablePrint (table, (language != LANG_AUTO),
							withListHeader, machinable, fp);
	colprintTableDelete (table);
}

static void printKinds (langType language, bool indent,
						struct colprintTable * table)
{
	const parserObject *parser;
	struct kindControlBlock *kcb;
	Assert (0 <= language  &&  language < (int) LanguageCount);

	initializeParser (language);
	parser = LanguageTable + language;
	kcb = parser->kindControlBlock;

	if (table)
		kindColprintAddLanguageLines (table, kcb);
	else
	{
		for (unsigned int i = 0  ;  i < countKinds(kcb)  ;  ++i)
			printKind (getKind(kcb, i), indent);
	}
}

extern void printLanguageKinds (const langType language, bool allKindFields,
								bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable * table = NULL;

	if (allKindFields)
		table = kindColprintTableNew ();

	if (language == LANG_AUTO)
	{
		for (unsigned int i = 0  ;  i < LanguageCount  ;  ++i)
		{
			const parserDefinition* const lang = LanguageTable [i].def;

			if (lang->invisible)
				continue;

			if (!table)
				printf ("%s%s\n", lang->name, isLanguageEnabled (i) ? "" : " [disabled]");
			printKinds (i, true, table);
		}
	}
	else
		printKinds (language, false, table);

	if (allKindFields)
	{
		kindColprintTablePrint(table, (language == LANG_AUTO)? 0: 1,
							   withListHeader, machinable, fp);
		colprintTableDelete (table);
	}
}

static void printParameters (struct colprintTable *table, langType language)
{
	const parserDefinition* lang;
	Assert (0 <= language  &&  language < (int) LanguageCount);

	initializeParser (language);
	lang = LanguageTable [language].def;
	if (lang->parameterHandlerTable != NULL)
	{
		for (unsigned int i = 0; i < lang->parameterHandlerCount; ++i)
			paramColprintAddParameter(table, language, lang->parameterHandlerTable + i);
	}

}

extern void printLanguageParameters (const langType language,
									 bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable *table =  paramColprintTableNew();

	if (language == LANG_AUTO)
	{
		for (unsigned int i = 0; i < LanguageCount ; ++i)
		{
			const parserDefinition* const lang = LanguageTable [i].def;

			if (lang->invisible)
				continue;

			printParameters (table, i);
		}
	}
	else
		printParameters (table, language);

	paramColprintTablePrint (table, (language != LANG_AUTO),
							 withListHeader, machinable, fp);
	colprintTableDelete (table);
}

static void processLangAliasOption (const langType language,
				    const char *const parameter)
{
	const char* alias;
	const parserObject * parser;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;

	if (parameter[0] == '\0')
	{
		clearLanguageAliases (language);
		verbose ("clear aliases for %s\n", parser->def->name);
	}
	else if (strcmp (parameter, RSV_LANGMAP_DEFAULT) == 0)
	{
		installLanguageAliasesDefault (language);
		verbose ("reset aliases for %s\n", parser->def->name);
	}
	else if (parameter[0] == '+')
	{
		alias = parameter + 1;
		addLanguageAlias(language, alias);
		verbose ("add an alias %s to %s\n", alias, parser->def->name);
	}
	else if (parameter[0] == '-')
	{
		if (parser->currentAliases)
		{
			alias = parameter + 1;
			if (stringListDeleteItemExtension (parser->currentAliases, alias))
			{
				verbose ("remove an alias %s from %s\n", alias, parser->def->name);
			}
		}
	}
	else
	{
		alias = parameter;
		clearLanguageAliases (language);
		addLanguageAlias(language, alias);
		verbose ("set alias %s to %s\n", alias, parser->def->name);
	}

}

extern bool processAliasOption (
		const char *const option, const char *const parameter)
{
	langType language;

	Assert (parameter);

#define PREFIX "alias-"
	if (strcmp (option, "alias-" RSV_LANG_ALL) == 0)
	{
		if ((parameter[0] == '\0')
			|| (strcmp (parameter, RSV_LANGMAP_DEFAULT) == 0))
		{
			for (unsigned int i = 0; i < LanguageCount; i++)
			{
				clearLanguageAliases (i);
				verbose ("clear aliases for %s\n", getLanguageName(i));
			}

			if (parameter[0] != '\0')
			{
				verbose ("  Installing default language aliases:\n");
				installLanguageAliasesDefaults ();
			}
		}
		else
		{
			error (WARNING, "Use \"%s\" option for reset (\"default\") or clearing (\"\")", option);
			return false;
		}
		return true;
	}

	language = getLanguageComponentInOption (option, "alias-");
	if (language == LANG_IGNORE)
		return false;
#undef PREFIX

	processLangAliasOption (language, parameter);
	return true;
}

static void printMaps (const langType language, langmapType type)
{
	const parserObject* parser;
	unsigned int i;

	parser = LanguageTable + language;
	printf ("%-8s", parser->def->name);
	if (parser->currentPatterns != NULL && (type & LMAP_PATTERN))
		for (i = 0  ;  i < stringListCount (parser->currentPatterns)  ;  ++i)
			printf (" %s", vStringValue (
						stringListItem (parser->currentPatterns, i)));
	if (parser->currentExtensions != NULL && (type & LMAP_EXTENSION))
		for (i = 0  ;  i < stringListCount (parser->currentExtensions)  ;  ++i)
			printf (" *.%s", vStringValue (
						stringListItem (parser->currentExtensions, i)));
	putchar ('\n');
}

static struct colprintTable *mapColprintTableNew (langmapType type)
{
	if ((type & LMAP_ALL) == LMAP_ALL)
		return colprintTableNew ("L:LANGUAGE", "L:TYPE", "L:MAP", NULL);
	else if (type & LMAP_PATTERN)
		return colprintTableNew ("L:LANGUAGE", "L:PATTERN", NULL);
	else if (type & LMAP_EXTENSION)
		return colprintTableNew ("L:LANGUAGE", "L:EXTENSION", NULL);
	else
	{
		AssertNotReached ();
		return NULL;
	}
}

static void mapColprintAddLanguage (struct colprintTable * table,
									langmapType type,
									const parserObject* parser)
{
	struct colprintLine * line;
	unsigned int count;
	unsigned int i;

	if ((type & LMAP_PATTERN) && (0 < (count = stringListCount (parser->currentPatterns))))
	{
		for (i = 0; i < count; i++)
		{
			line = colprintTableGetNewLine (table);
			vString *pattern = stringListItem (parser->currentPatterns, i);

			colprintLineAppendColumnCString (line, parser->def->name);
			if (type & LMAP_EXTENSION)
				colprintLineAppendColumnCString (line, "pattern");
			colprintLineAppendColumnVString (line, pattern);
		}
	}

	if ((type & LMAP_EXTENSION) && (0 < (count = stringListCount (parser->currentExtensions))))
	{
		for (i = 0; i < count; i++)
		{
			line = colprintTableGetNewLine (table);
			vString *extension = stringListItem (parser->currentExtensions, i);

			colprintLineAppendColumnCString (line, parser->def->name);
			if (type & LMAP_PATTERN)
				colprintLineAppendColumnCString (line, "extension");
			colprintLineAppendColumnVString (line, extension);
		}
	}
}

extern void printLanguageMaps (const langType language, langmapType type,
							   bool withListHeader, bool machinable, FILE *fp)
{
	/* DON'T SORT THE LIST

	   The order of listing should be equal to the order of matching
	   for the parser selection. */

	struct colprintTable * table = NULL;
	if (type & LMAP_TABLE_OUTPUT)
		table = mapColprintTableNew(type);

	if (language == LANG_AUTO)
	{
		for (unsigned int i = 0  ;  i < LanguageCount  ;  ++i)
		{
			if (!isLanguageVisible (i))
				continue;

			if (type & LMAP_TABLE_OUTPUT)
			{
				const parserObject* parser = LanguageTable + i;

				mapColprintAddLanguage (table, type, parser);
			}
			else
				printMaps (i, type);
		}
	}
	else
	{
		Assert (0 <= language  &&  language < (int) LanguageCount);

		if (type & LMAP_TABLE_OUTPUT)
		{
			const parserObject* parser = LanguageTable + language;

			mapColprintAddLanguage (table, type, parser);
		}
		else
			printMaps (language, type);
	}


	if (type & LMAP_TABLE_OUTPUT)
	{
		colprintTablePrint (table, (language == LANG_AUTO)? 0: 1,
							withListHeader, machinable, fp);
		colprintTableDelete (table);
	}
}

static struct colprintTable *aliasColprintTableNew (void)
{
	return colprintTableNew ("L:LANGUAGE", "L:ALIAS", NULL);
}

static void aliasColprintAddLanguage (struct colprintTable * table,
									  const parserObject* parser)
{
	unsigned int count;

	if (parser->currentAliases && (0 < (count = stringListCount (parser->currentAliases))))
	{
		for (unsigned int i = 0; i < count; i++)
		{
			struct colprintLine * line = colprintTableGetNewLine (table);
			vString *alias = stringListItem (parser->currentAliases, i);;

			colprintLineAppendColumnCString (line, parser->def->name);
			colprintLineAppendColumnVString (line, alias);
		}
	}
}

extern void printLanguageAliases (const langType language,
								  bool withListHeader, bool machinable, FILE *fp)
{
	/* DON'T SORT THE LIST

	   The order of listing should be equal to the order of matching
	   for the parser selection. */

	struct colprintTable * table = aliasColprintTableNew();
	const parserObject* parser;

	if (language == LANG_AUTO)
	{
		for (unsigned int i = 0; i < LanguageCount; ++i)
		{
			parser = LanguageTable + i;
			if (parser->def->invisible)
				continue;

			aliasColprintAddLanguage (table, parser);
		}
	}
	else
	{
		Assert (0 <= language  &&  language < (int) LanguageCount);
		parser = LanguageTable + language;
		aliasColprintAddLanguage (table, parser);
	}

	colprintTablePrint (table, (language == LANG_AUTO)? 0: 1,
						withListHeader, machinable, fp);
	colprintTableDelete (table);
}

static void printLanguage (const langType language, parserDefinition** ltable)
{
	const parserDefinition* lang;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = ltable [language];

	if (lang->invisible)
		return;

	if (lang->kindTable != NULL  ||  (lang->method & METHOD_REGEX))
		printf ("%s%s\n", lang->name, isLanguageEnabled (lang->id) ? "" : " [disabled]");
}

extern void printLanguageList (void)
{
	unsigned int i;
	parserDefinition **ltable;

	ltable = xMalloc (LanguageCount, parserDefinition*);
	for (i = 0 ; i < LanguageCount ; ++i)
		ltable[i] = LanguageTable[i].def;
	qsort (ltable, LanguageCount, sizeof (parserDefinition*), compareParsersByName);

	for (i = 0  ;  i < LanguageCount  ;  ++i)
		printLanguage (i, ltable);

	eFree (ltable);
}

static void xtagDefinitionDestroy (xtagDefinition *xdef)
{
	eFree ((void *)xdef->name);
	eFree ((void *)xdef->description);
	eFree (xdef);
}

static bool processLangDefineExtra (const langType language,
									const char *const option,
									const char *const parameter)
{
	xtagDefinition *xdef;
	const char * p = parameter;
	const char *name_end;
	const char *desc;
	const char *flags;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	Assert (p);

	if (p[0] == '\0')
		error (FATAL, "no extra definition specified in \"--%s\" option", option);

	name_end = strchr (p, ',');
	if (!name_end)
		error (FATAL, "no extra description specified in \"--%s\" option", option);
	else if (name_end == p)
		error (FATAL, "the extra name in \"--%s\" option is empty", option);

	for (; p < name_end; p++)
	{
		if (!isalnum (*p))
			error (FATAL, "unacceptable char as part of extra name in \"--%s\" option",
				   option);
	}

	p++;
	if (p [0] == '\0' || p [0] == LONG_FLAGS_OPEN)
		error (FATAL, "extra description in \"--%s\" option is empty", option);

	desc = extractDescriptionAndFlags (p, &flags);

	xdef = xCalloc (1, xtagDefinition);
	xdef->enabled = false;
	xdef->letter = NUL_XTAG_LETTER;
	xdef->name = eStrndup (parameter, name_end - parameter);
	xdef->description = desc;
	xdef->isEnabled = NULL;
	DEFAULT_TRASH_BOX(xdef, xtagDefinitionDestroy);

	if (flags)
		flagsEval (flags, NULL, 0, xdef);

	defineXtag (xdef, language);

	return true;
}

extern bool processExtradefOption (const char *const option, const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "_" "extradef-");
	if (language == LANG_IGNORE)
		return false;

	return processLangDefineExtra (language, option, parameter);
}

static void fieldDefinitionDestroy (fieldDefinition *fdef)
{
	eFree ((void *)fdef->description);
	eFree ((void *)fdef->name);
	eFree (fdef);
}

static bool processLangDefineField (const langType language,
									const char *const option,
									const char *const parameter)
{
	fieldDefinition *fdef;
	const char * p = parameter;
	const char *name_end;
	const char *desc;
	const char *flags;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	Assert (p);

	if (p[0] == '\0')
		error (FATAL, "no field definition specified in \"--%s\" option", option);

	name_end = strchr (p, ',');
	if (!name_end)
		error (FATAL, "no field description specified in \"--%s\" option", option);
	else if (name_end == p)
		error (FATAL, "the field name in \"--%s\" option is empty", option);

	for (; p < name_end; p++)
	{
		if (!isalpha (*p))
			error (FATAL, "unacceptable char as part of field name in \"--%s\" option",
				   option);
	}

	p++;
	if (p [0] == '\0' || p [0] == LONG_FLAGS_OPEN)
		error (FATAL, "field description in \"--%s\" option is empty", option);

	desc = extractDescriptionAndFlags (p, &flags);

	fdef = xCalloc (1, fieldDefinition);
	fdef->enabled = false;
	fdef->letter = NUL_FIELD_LETTER;
	fdef->name = eStrndup(parameter, name_end - parameter);
	fdef->description = desc;
	fdef->isValueAvailable = NULL;
	fdef->dataType = FIELDTYPE_STRING; /* TODO */
	fdef->ftype = FIELD_UNKNOWN;
	DEFAULT_TRASH_BOX(fdef, fieldDefinitionDestroy);

	if (flags)
		flagsEval (flags, NULL, 0, fdef);

	defineField (fdef, language);

	return true;
}

extern bool processFielddefOption (const char *const option, const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "_fielddef-");
	if (language == LANG_IGNORE)
		return false;

	return processLangDefineField (language, option, parameter);
}

/*
*   File parsing
*/

static rescanReason createTagsForFile (const langType language,
				       const unsigned int passCount)
{
	parserDefinition *const lang = LanguageTable [language].def;
	rescanReason rescan = RESCAN_NONE;

	resetInputFile (language);

	Assert (lang->parser || lang->parser2);

	notifyLanguageRegexInputStart (language);
	notifyInputStart ();

	if (lang->parser != NULL)
		lang->parser ();
	else if (lang->parser2 != NULL)
		rescan = lang->parser2 (passCount);

	notifyInputEnd ();
	notifyLanguageRegexInputEnd (language);

	return rescan;
}

extern void notifyLanguageRegexInputStart (langType language)
{
	notifyRegexInputStart((LanguageTable + language)->lregexControlBlock);
}

extern void notifyLanguageRegexInputEnd (langType language)
{
	notifyRegexInputEnd((LanguageTable + language)->lregexControlBlock);
}

static bool doesParserUseCork (parserDefinition *parser)
{
	subparser *tmp;
	bool r = false;

	if (parser->useCork)
		return true;

	if (hasLanguageScopeActionInRegex (parser->id)
	    || parser->requestAutomaticFQTag)
	{
		parser->useCork = true;
		return true;
	}

	pushLanguage (parser->id);
	foreachSubparser(tmp, true)
	{
		langType t = getSubparserLanguage (tmp);
		if (doesParserUseCork (LanguageTable[t].def))
		{
			r = true;
			break;
		}
	}
	popLanguage ();
	return r;
}

static void setupLanguageSubparsersInUse (const langType language)
{
	subparser *tmp;

	setupSubparsersInUse ((LanguageTable + language)->slaveControlBlock);
	foreachSubparser(tmp, true)
	{
		langType t = getSubparserLanguage (tmp);
		enterSubparser (tmp);
		setupLanguageSubparsersInUse(t);
		leaveSubparser ();
	}
}

static subparser* teardownLanguageSubparsersInUse (const langType language)
{
	subparser *tmp;

	foreachSubparser(tmp, true)
	{
		langType t = getSubparserLanguage (tmp);
		enterSubparser (tmp);
		teardownLanguageSubparsersInUse(t);
		leaveSubparser ();
	}
	return teardownSubparsersInUse ((LanguageTable + language)->slaveControlBlock);
}

static bool createTagsWithFallback1 (const langType language,
									 langType *exclusive_subparser)
{
	bool tagFileResized = false;
	unsigned long numTags	= numTagsAdded ();
	MIOPos tagfpos;
	int lastPromise = getLastPromise ();
	unsigned int passCount = 0;
	rescanReason whyRescan;
	parserObject *parser;
	bool useCork;

	initializeParser (language);
	parser = &(LanguageTable [language]);

	setupLanguageSubparsersInUse (language);

	useCork = doesParserUseCork(parser->def);
	if (useCork)
		corkTagFile();

	addParserPseudoTags (language);
	tagFilePosition (&tagfpos);

	anonResetMaybe (parser);

	while ( ( whyRescan =
		  createTagsForFile (language, ++passCount) )
		!= RESCAN_NONE)
	{
		if (useCork)
		{
			uncorkTagFile();
			corkTagFile();
		}


		if (whyRescan == RESCAN_FAILED)
		{
			/*  Restore prior state of tag file.
			*/
			setTagFilePosition (&tagfpos);
			setNumTagsAdded (numTags);
			tagFileResized = true;
			breakPromisesAfter(lastPromise);
		}
		else if (whyRescan == RESCAN_APPEND)
		{
			tagFilePosition (&tagfpos);
			numTags = numTagsAdded ();
			lastPromise = getLastPromise ();
		}
	}

	/* Force filling allLines buffer and kick the multiline regex parser */
	if (hasLanguageMultilineRegexPatterns (language))
		while (readLineFromInputFile () != NULL)
			; /* Do nothing */

	if (useCork)
		uncorkTagFile();

	{
		subparser *s = teardownLanguageSubparsersInUse (language);
		if (exclusive_subparser && s)
			*exclusive_subparser = getSubparserLanguage (s);
	}

	return tagFileResized;
}

extern bool runParserInNarrowedInputStream (const langType language,
					       unsigned long startLine, long startCharOffset,
					       unsigned long endLine, long endCharOffset,
					       unsigned long sourceLineOffset,
					       int promise)
{
	bool tagFileResized;

	verbose ("runParserInNarrowedInputStream: %s; "
			 "file: %s, "
			 "start(line: %lu, offset: %ld, srcline: %lu)"
			 " - "
			 "end(line: %lu, offset: %ld)\n",
			 getLanguageName (language),
			 getInputFileName (),
			 startLine, startCharOffset, sourceLineOffset,
			 endLine, endCharOffset);

	pushNarrowedInputStream (
				 startLine, startCharOffset,
				 endLine, endCharOffset,
				 sourceLineOffset,
				 promise);
	tagFileResized = createTagsWithFallback1 (language, NULL);
	popNarrowedInputStream  ();
	return tagFileResized;

}

static bool createTagsWithFallback (
	const char *const fileName, const langType language,
	MIO *mio)
{
	langType exclusive_subparser = LANG_IGNORE;
	bool tagFileResized = false;

	Assert (0 <= language  &&  language < (int) LanguageCount);

	if (!openInputFile (fileName, language, mio))
		return false;

	tagFileResized = createTagsWithFallback1 (language,
											  &exclusive_subparser);
	tagFileResized = forcePromises()? true: tagFileResized;

	pushLanguage ((exclusive_subparser == LANG_IGNORE)
				  ? language
				  : exclusive_subparser);
	makeFileTag (fileName);
	popLanguage ();
	closeInputFile ();

	return tagFileResized;
}

static void printGuessedParser (const char* const fileName, langType language)
{
	const char *parserName;

	if (language == LANG_IGNORE)
	{
		Option.printLanguage = ((int)true) + 1;
		parserName = RSV_NONE;
	}
	else
		parserName = LanguageTable [language].def->name;

	printf("%s: %s\n", fileName, parserName);
}

#ifdef HAVE_ICONV
static char **EncodingMap;
static unsigned int EncodingMapMax;

static void addLanguageEncoding (const langType language,
									const char *const encoding)
{
	if (language > EncodingMapMax || EncodingMapMax == 0)
	{
		int i;
		int istart = (EncodingMapMax == 0)? 0: EncodingMapMax + 1;
		EncodingMap = xRealloc (EncodingMap, (language + 1), char*);
		for (i = istart;  i <= language  ;  ++i)
		{
			EncodingMap [i] = NULL;
		}
		EncodingMapMax = language;
	}
	if (EncodingMap [language])
		eFree (EncodingMap [language]);
	EncodingMap [language] = eStrdup(encoding);
	if (!Option.outputEncoding)
		Option.outputEncoding = eStrdup("UTF-8");
}

extern bool processLanguageEncodingOption (const char *const option, const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "input-encoding-");
	if (language == LANG_IGNORE)
		return false;

	addLanguageEncoding (language, parameter);
	return true;
}

extern void freeEncodingResources (void)
{
	if (EncodingMap)
	{
		unsigned int i;
		for (i = 0  ;  i <= EncodingMapMax  ; ++i)
		{
			if (EncodingMap [i])
				eFree (EncodingMap [i]);
		}
		eFree (EncodingMap);
	}
	if (Option.inputEncoding)
		eFree (Option.inputEncoding);
	if (Option.outputEncoding)
		eFree (Option.outputEncoding);
}

extern const char *getLanguageEncoding (const langType language)
{
	if (EncodingMap && language <= EncodingMapMax && EncodingMap [language])
		return EncodingMap[language];
	else
		return Option.inputEncoding;
}
#endif

static void addParserPseudoTags (langType language)
{
	parserObject *parser = LanguageTable + language;
	if (!parser->pseudoTagPrinted)
	{
		makePtagIfEnabled (PTAG_KIND_DESCRIPTION, &language);
		makePtagIfEnabled (PTAG_KIND_SEPARATOR, &language);

		parser->pseudoTagPrinted = 1;
	}
}

extern bool doesParserRequireMemoryStream (const langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parserDefinition *const lang = LanguageTable [language].def;
	unsigned int i;

	if (lang->tagXpathTableCount > 0
		|| lang->useMemoryStreamInput)
	{
		verbose ("%s requires a memory stream for input\n", lang->name);
		return true;
	}

	for (i = 0; i < lang->dependencyCount; i++)
	{
		parserDependency *d = lang->dependencies + i;
		if (d->type == DEPTYPE_SUBPARSER &&
			((subparser *)(d->data))->direction & SUBPARSER_SUB_RUNS_BASE)
		{
			langType baseParser;
			baseParser = getNamedLanguage (d->upperParser, 0);
			if (doesParserRequireMemoryStream(baseParser))
			{
				verbose ("%s/%s requires a memory stream for input\n", lang->name,
						 LanguageTable[baseParser].def->name);
				return true;
			}
		}
	}

	return false;
}

extern bool parseFile (const char *const fileName)
{
	TRACE_ENTER_TEXT("Parsing file %s",fileName);
	bool bRet = parseFileWithMio (fileName, NULL);
	TRACE_LEAVE();
	return bRet;
}

extern bool parseFileWithMio (const char *const fileName, MIO *mio)
{
	bool tagFileResized = false;
	langType language;
	struct GetLanguageRequest req = {
		.type = mio? GLR_REUSE: GLR_OPEN,
		.fileName = fileName,
		.mio = mio,
	};

	language = getFileLanguageForRequest (&req);
	Assert (language != LANG_AUTO);

	if (Option.printLanguage)
	{
		printGuessedParser (fileName, language);
		return tagFileResized;
	}

	if (language == LANG_IGNORE)
		verbose ("ignoring %s (unknown language/language disabled)\n",
			 fileName);
	else
	{
		Assert(isLanguageEnabled (language));

		if (Option.filter && ! Option.interactive)
			openTagFile ();

#ifdef HAVE_ICONV
		/* TODO: checkUTF8BOM can be used to update the encodings. */
		openConverter (getLanguageEncoding (language), Option.outputEncoding);
#endif

		setupWriter ();

		setupAnon ();

		initParserTrashBox ();

		tagFileResized = createTagsWithFallback (fileName, language, req.mio);

		finiParserTrashBox ();

		teardownAnon ();

		tagFileResized = teardownWriter (getSourceFileTagPath())? true: tagFileResized;

		if (Option.filter && ! Option.interactive)
			closeTagFile (tagFileResized);
		addTotals (1, 0L, 0L);

#ifdef HAVE_ICONV
		closeConverter ();
#endif
		if (req.type == GLR_OPEN && req.mio)
			mio_free (req.mio);
		return tagFileResized;
	}

	if (req.type == GLR_OPEN && req.mio)
		mio_free (req.mio);

	return tagFileResized;
}

static void matchLanguageMultilineRegexCommon (const langType language,
											   bool (* func) (struct lregexControlBlock *, const vString* const),
											   const vString* const allLines)
{
	subparser *tmp;

	func ((LanguageTable + language)->lregexControlBlock, allLines);
	foreachSubparser(tmp, true)
	{
		langType t = getSubparserLanguage (tmp);
		enterSubparser (tmp);
		matchLanguageMultilineRegexCommon (t, func, allLines);
		leaveSubparser ();
	}
}

extern void matchLanguageMultilineRegex (const langType language,
										 const vString* const allLines)
{
	matchLanguageMultilineRegexCommon(language, matchMultilineRegex, allLines);
}

extern void matchLanguageMultitableRegex (const langType language,
										  const vString* const allLines)
{
	matchLanguageMultilineRegexCommon(language, matchMultitableRegex, allLines);
}

extern void processLanguageMultitableExtendingOption (langType language, const char *const parameter)
{
	const char* src;
	char* dist;
	const char *tmp;

	tmp = strchr(parameter, '+');

	if (!tmp)
		error (FATAL, "no separator(+) found: %s", parameter);

	if (tmp == parameter)
		error (FATAL, "the name of source table is empty in table extending: %s", parameter);

	src = tmp + 1;
	if (!*src)
		error (FATAL, "the name of dist table is empty in table extending: %s", parameter);

	dist = eStrndup(parameter, tmp  - parameter);
	extendRegexTable(((LanguageTable + language)->lregexControlBlock), src, dist);
	eFree (dist);
}

static bool lregexQueryParserAndSubparsers (const langType language, bool (* predicate) (struct lregexControlBlock *))
{
	bool r;
	subparser *tmp;

	r = predicate ((LanguageTable + language)->lregexControlBlock);
	if (!r)
	{
		foreachSubparser(tmp, true)
		{
			langType t = getSubparserLanguage (tmp);
			enterSubparser (tmp);
			r = lregexQueryParserAndSubparsers (t, predicate);
			leaveSubparser ();

			if (r)
				break;
		}
	}

	return r;
}

extern bool hasLanguageMultilineRegexPatterns (const langType language)
{
	return lregexQueryParserAndSubparsers (language, regexNeedsMultilineBuffer);
}


extern void addLanguageCallbackRegex (const langType language, const char *const regex, const char *const flags,
									  const regexCallback callback, bool *disabled, void *userData)
{
	addCallbackRegex ((LanguageTable +language)->lregexControlBlock, regex, flags, callback, disabled, userData);
}

extern bool hasLanguageScopeActionInRegex (const langType language)
{
	bool hasScopeAction;

	pushLanguage (language);
	hasScopeAction = lregexQueryParserAndSubparsers (language, hasScopeActionInRegex);
	popLanguage ();

	return hasScopeAction;
}

extern void matchLanguageRegex (const langType language, const vString* const line)
{
	subparser *tmp;

	matchRegex ((LanguageTable + language)->lregexControlBlock, line);
	foreachSubparser(tmp, true)
	{
		langType t = getSubparserLanguage (tmp);
		enterSubparser (tmp);
		matchLanguageRegex (t, line);
		leaveSubparser ();
	}
}

extern bool processLanguageRegexOption (langType language,
										enum regexParserType regptype,
										const char *const parameter)
{
	processTagRegexOption ((LanguageTable +language)->lregexControlBlock,
						   regptype, parameter);

	return true;
}

extern bool processTabledefOption (const char *const option, const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "_tabledef-");
	if (language == LANG_IGNORE)
		return false;

	if (parameter == NULL || parameter[0] == '\0')
		error (FATAL, "A parameter is needed after \"%s\" option", option);

	addRegexTable((LanguageTable +language)->lregexControlBlock, parameter);
	return true;
}

extern void useRegexMethod (const langType language)
{
	parserDefinition* lang;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;
	lang->method |= METHOD_REGEX;
}

static void useXpathMethod (const langType language)
{
	parserDefinition* lang;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;
	lang->method |= METHOD_XPATH;
}

static void installTagRegexTable (const langType language)
{
	parserObject* parser;
	parserDefinition* lang;
	unsigned int i;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	lang = parser->def;


	if (lang->tagRegexTable != NULL)
	{
	    for (i = 0; i < lang->tagRegexCount; ++i)
		{
			if (lang->tagRegexTable [i].mline)
				addTagMultiLineRegex (parser->lregexControlBlock,
									  lang->tagRegexTable [i].regex,
									  lang->tagRegexTable [i].name,
									  lang->tagRegexTable [i].kinds,
									  lang->tagRegexTable [i].flags,
									  (lang->tagRegexTable [i].disabled));
			else
				addTagRegex (parser->lregexControlBlock,
							 lang->tagRegexTable [i].regex,
							 lang->tagRegexTable [i].name,
							 lang->tagRegexTable [i].kinds,
							 lang->tagRegexTable [i].flags,
							 (lang->tagRegexTable [i].disabled));
		}
	}
}

static void installKeywordTable (const langType language)
{
	parserDefinition* lang;
	unsigned int i;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;

	if (lang->keywordTable != NULL)
	{
		for (i = 0; i < lang->keywordCount; ++i)
			addKeyword (lang->keywordTable [i].name,
				    language,
				    lang->keywordTable [i].id);
	}
}

static void installTagXpathTable (const langType language)
{
	parserDefinition* lang;
	unsigned int i, j;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;

	if (lang->tagXpathTableTable != NULL)
	{
		for (i = 0; i < lang->tagXpathTableCount; ++i)
			for (j = 0; j < lang->tagXpathTableTable[i].count; ++j)
				addTagXpath (language, lang->tagXpathTableTable[i].table + j);
		useXpathMethod (language);
	}
}

extern unsigned int getXpathFileSpecCount (const langType language)
{
	parserDefinition* lang;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;

	return lang->xpathFileSpecCount;
}

extern xpathFileSpec* getXpathFileSpec (const langType language, unsigned int nth)
{
	parserDefinition* lang;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;

	Assert (nth < lang->xpathFileSpecCount);
	return lang->xpathFileSpecs + nth;
}

extern bool makeKindSeparatorsPseudoTags (const langType language,
					     const ptagDesc *pdesc)
{
	parserObject* parser;
	parserDefinition* lang;
	struct kindControlBlock *kcb;
	kindDefinition *kind;
	unsigned int kindCount;
	unsigned int i, j;

	bool r = false;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	lang = parser->def;
	kcb = parser->kindControlBlock;
	kindCount = countKinds(kcb);

	if (kindCount == 0)
		return r;

	for (i = 0; i < kindCount; ++i)
	{
		static vString *sepval;

		if (!sepval)
			sepval = vStringNew ();

		kind = getKind (kcb, i);
		for (j = 0; j < kind->separatorCount; ++j)
		{
			char name[5] = {[0] = '/', [3] = '/', [4] = '\0'};
			const kindDefinition *upperKind;
			const scopeSeparator *sep;

			sep = kind->separators + j;

			if (sep->parentKindIndex == KIND_WILDCARD_INDEX)
			{
				name[1] = KIND_WILDCARD;
				name[2] = kind->letter;
			}
			else if (sep->parentKindIndex == KIND_GHOST_INDEX)
			{
				/* This is root separator: no upper item is here. */
				name[1] = kind->letter;
				name[2] = name[3];
				name[3] = '\0';
			}
			else
			{
				upperKind = getLanguageKind (language,
							    sep->parentKindIndex);
				if (!upperKind)
					continue;

				name[1] = upperKind->letter;
				name[2] = kind->letter;
			}


			vStringClear (sepval);
			vStringCatSWithEscaping (sepval, sep->separator);

			r = writePseudoTag (pdesc, vStringValue (sepval),
					    name, lang->name) || r;
		}
	}

	return r;
}

struct makeKindDescriptionPseudoTagData {
	const char* langName;
	const ptagDesc *pdesc;
	bool written;
};

static bool makeKindDescriptionPseudoTag (kindDefinition *kind,
					     void *user_data)
{
	struct makeKindDescriptionPseudoTagData *data = user_data;
	vString *letter_and_name;
	vString *description;
	const char *d;

	letter_and_name = vStringNew ();
	description = vStringNew ();

	vStringPut (letter_and_name, kind -> letter);
	vStringPut (letter_and_name, ',');
	vStringCatS (letter_and_name, kind -> name);

	d = kind->description? kind->description: kind->name;
	vStringPut (description, '/');
	vStringCatSWithEscapingAsPattern (description, d);
	vStringPut (description, '/');
	data->written |=  writePseudoTag (data->pdesc, vStringValue (letter_and_name),
					  vStringValue (description),
					  data->langName);

	vStringDelete (description);
	vStringDelete (letter_and_name);

	return false;
}

extern bool makeKindDescriptionsPseudoTags (const langType language,
					    const ptagDesc *pdesc)
{
	parserObject *parser;
	struct kindControlBlock *kcb;
	parserDefinition* lang;
	kindDefinition *kind;
	unsigned int kindCount, i;
	struct makeKindDescriptionPseudoTagData data;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	kcb = parser->kindControlBlock;
	lang = parser->def;

	kindCount = countKinds(kcb);

	data.langName = lang->name;
	data.pdesc = pdesc;
	data.written = false;

	for (i = 0; i < kindCount; ++i)
	{
		kind = getKind (kcb, i);
		makeKindDescriptionPseudoTag (kind, &data);
	}

	return data.written;
}

/*
*   Copyright (c) 2016, Szymon Tomasz Stefanek
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   Anonymous name generator
*/
static ptrArray *parsersUsedInCurrentInput;

static void setupAnon (void)
{
	parsersUsedInCurrentInput = ptrArrayNew (NULL);
}

static void teardownAnon (void)
{
	ptrArrayDelete (parsersUsedInCurrentInput);
}

static void anonResetMaybe (parserObject *parser)
{
	if (ptrArrayHas (parsersUsedInCurrentInput, parser))
		return;

	parser -> anonymousIdentiferId = 0;
	ptrArrayAdd (parsersUsedInCurrentInput, parser);
}

static unsigned int anonHash(const unsigned char *str)
{
	unsigned int hash = 5381;
	int c;

	while((c = *str++))
		hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

	return hash ;
}

extern void anonHashString (const char *filename, char buf[9])
{
	sprintf(buf, "%08x", anonHash((const unsigned char *)filename));
}


extern void anonGenerate (vString *buffer, const char *prefix, int kind)
{
	parserObject* parser = LanguageTable + getInputLanguage ();
	parser -> anonymousIdentiferId ++;

	char szNum[32];
	char buf [9];

	vStringCopyS(buffer, prefix);

	anonHashString (getInputFileName(), buf);
	sprintf(szNum,"%s%02x%02x",buf,parser -> anonymousIdentiferId, kind);
	vStringCatS(buffer,szNum);
}


extern void applyParameter (const langType language, const char *name, const char *args)
{
	parserDefinition* parser;


	Assert (0 <= language  &&  language < (int) LanguageCount);

	initializeParserOne (language);
	parser = LanguageTable [language].def;

	if (parser->parameterHandlerTable)
	{
		unsigned int i;

		for (i = 0; i < parser->parameterHandlerCount; i++)
		{
			if (strcmp (parser->parameterHandlerTable [i].name, name) == 0)
			{
				parser->parameterHandlerTable [i].handleParameter (language, name, args);
				return;
			}
		}
	}

	error (FATAL, "no such parameter in %s: %s", parser->name, name);
}

extern subparser *getNextSubparser(subparser *last,
								   bool includingNoneCraftedParser)
{
	langType lang = getInputLanguage ();
	parserObject *parser = LanguageTable + lang;
	subparser *r;
	langType t;

	if (last == NULL)
		r = getFirstSubparser(parser->slaveControlBlock);
	else
		r = last->next;

	if (r == NULL)
		return r;

	t = getSubparserLanguage(r);
	if (isLanguageEnabled (t) &&
		(includingNoneCraftedParser
		 || ((((LanguageTable + t)->def->method) && METHOD_NOT_CRAFTED) == 0)))
		return r;
	else
		return getNextSubparser (r, includingNoneCraftedParser);
}

extern slaveParser *getNextSlaveParser(slaveParser *last)
{
	langType lang = getInputLanguage ();
	parserObject *parser = LanguageTable + lang;
	slaveParser *r;

	if (last == NULL)
		r = getFirstSlaveParser(parser->slaveControlBlock);
	else
		r = last->next;

	return r;
}

extern void scheduleRunningBaseparser (int dependencyIndex)
{
	langType current = getInputLanguage ();
	parserDefinition *current_parser = LanguageTable [current].def;
	parserDependency *dep = NULL;

	if (dependencyIndex == RUN_DEFAULT_SUBPARSERS)
	{
		for (unsigned int i = 0; i < current_parser->dependencyCount; ++i)
			if (current_parser->dependencies[i].type == DEPTYPE_SUBPARSER)
			{
				dep = current_parser->dependencies + i;
				break;
			}
	}
	else
		dep = current_parser->dependencies + dependencyIndex;

	if (dep == NULL)
		return;

	const char *base_name = dep->upperParser;
	langType base = getNamedLanguage (base_name, 0);
	parserObject *base_parser = LanguageTable + base;

	if (dependencyIndex == RUN_DEFAULT_SUBPARSERS)
		useDefaultSubparsers(base_parser->slaveControlBlock);
	else
		useSpecifiedSubparser (base_parser->slaveControlBlock,
							   dep->data);

	if (!isLanguageEnabled (base))
	{
		enableLanguage (base, true);
		base_parser->dontEmit = true;
		verbose ("force enable \"%s\" as base parser\n", base_parser->def->name);
	}

	{
		subparser *tmp;

		verbose ("scheduleRunningBaseparser %s with subparsers: ", base_name);
		pushLanguage (base);
		foreachSubparser(tmp, true)
		{
			langType t = getSubparserLanguage (tmp);
			verbose ("%s ", getLanguageName (t));
		}
		popLanguage ();
		verbose ("\n");
	}


	makePromise(base_name, THIN_STREAM_SPEC);
}

extern bool isParserMarkedNoEmission (void)
{
	langType lang = getInputLanguage();
	parserObject *parser = LanguageTable + lang;

	return parser->dontEmit;
}


extern subparser* getSubparserRunningBaseparser (void)
{
	langType current = getInputLanguage ();
	parserObject *current_parser = LanguageTable + current;
	subparser *s = getFirstSubparser (current_parser->slaveControlBlock);

	if (s && s->schedulingBaseparserExplicitly)
		return s;
	else
		return NULL;
}

extern void printLanguageSubparsers (const langType language,
									 bool withListHeader, bool machinable, FILE *fp)
{
	for (int i = 0; i < (int) LanguageCount; i++)
		initializeParserOne (i);

	struct colprintTable * table = subparserColprintTableNew();
	parserObject *parser;

	if (language == LANG_AUTO)
	{
		for (int i = 0; i < (int) LanguageCount; i++)
		{
			parser = LanguageTable + i;
			if (parser->def->invisible)
				continue;

			subparserColprintAddSubparsers (table,
											parser->slaveControlBlock);
		}
	}
	else
	{
		parser = (LanguageTable + language);
		subparserColprintAddSubparsers (table,
										parser->slaveControlBlock);
	}

	subparserColprintTablePrint (table,
								 withListHeader, machinable,
								 fp);
	colprintTableDelete (table);
}

extern void printLangdefFlags (bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable * table;

	table = flagsColprintTableNew ();

	flagsColprintAddDefinitions (table, PreLangDefFlagDef, ARRAY_SIZE (PreLangDefFlagDef));

	flagsColprintTablePrint (table, withListHeader, machinable, fp);
	colprintTableDelete(table);
}

extern void printKinddefFlags (bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable * table;

	table = flagsColprintTableNew ();

	flagsColprintAddDefinitions (table, PreKindDefFlagDef, ARRAY_SIZE (PreKindDefFlagDef));

	flagsColprintTablePrint (table, withListHeader, machinable, fp);
	colprintTableDelete(table);
}

extern void printLanguageMultitableStatistics (langType language, FILE *vfp)
{
	parserObject* const parser = LanguageTable + language;
	printMultitableStatistics (parser->lregexControlBlock,
							   vfp);
}

extern void addLanguageRegexTable (const langType language, const char *name)
{
	parserObject* const parser = LanguageTable + language;
	addRegexTable (parser->lregexControlBlock, name);
}

extern void addLanguageTagMultiTableRegex(const langType language,
										  const char* const table_name,
										  const char* const regex,
										  const char* const name, const char* const kinds, const char* const flags,
										  bool *disabled)
{
	parserObject* const parser = LanguageTable + language;
	addTagMultiTableRegex (parser->lregexControlBlock, table_name, regex,
						   name, kinds, flags, disabled);
}

/*
 * A parser for CTagsSelfTest (CTST)
 */
#define SELF_TEST_PARSER "CTagsSelfTest"
#if defined(DEBUG) && defined(HAVE_SECCOMP)
extern void getppid(void);
#endif

typedef enum {
	K_BROKEN,
	K_NO_LETTER,
	K_NO_LONG_NAME,
	K_NOTHING_SPECIAL,
	K_GUEST_BEGINNING,
	K_GUEST_END,
#if defined(DEBUG) && defined(HAVE_SECCOMP)
	K_CALL_GETPPID,
#endif
	K_DISABLED,
	K_ENABLED,
	K_ROLES,
	K_ROLES_DISABLED,
	KIND_COUNT
} CTST_Kind;

typedef enum {
	R_BROKEN_REF,
} CTST_BrokenRole;

static roleDefinition CTST_BrokenRoles [] = {
	{true, "broken", "broken" },
};

typedef enum {
	R_DISABLED_KIND_DISABLED_ROLE,
	R_DISABLED_KIND_ENABLED_ROLE,
} CTST_DisabledKindRole;

static roleDefinition CTST_DisabledKindRoles [] = {
	{ false, "disabled", "disbaled role attached to disabled kind" },
	{ true,  "enabled",  "enabled role attached to disabled kind"  },
};

typedef enum {
	R_ENABLED_KIND_DISABLED_ROLE,
	R_ENABLED_KIND_ENABLED_ROLE,
} CTST_EnabledKindRole;

static roleDefinition CTST_EnabledKindRoles [] = {
	{ false, "disabled", "disbaled role attached to enabled kind" },
	{ true,  "enabled",  "enabled role attached to enabled kind"  },
};

typedef enum {
	R_ROLES_KIND_A_ROLE,
	R_ROLES_KIND_B_ROLE,
	R_ROLES_KIND_C_ROLE,
	R_ROLES_KIND_D_ROLE,
} CTST_RolesKindRole;

static roleDefinition CTST_RolesKindRoles [] = {
	{ true,  "a", "A role" },
	{ true,  "b", "B role" },
	{ false, "c", "C role" },
	{ true,  "d", "D role"  },
};

typedef enum {
	R_ROLES_DISABLED_KIND_A_ROLE,
	R_ROLES_DISABLED_KIND_B_ROLE,
} CTST_RolesDisableKindRole;


static roleDefinition CTST_RolesDisabledKindRoles [] = {
	{ true,  "A", "A role" },
	{ true,  "B", "B role" },
};

static kindDefinition CTST_Kinds[KIND_COUNT] = {
	/* `a' is reserved for kinddef testing */
	{true, 'b', "broken tag", "name with unwanted characters",
	 .referenceOnly = false, ATTACH_ROLES (CTST_BrokenRoles) },
	{true, KIND_NULL, "no letter", "kind with no letter"
	 /* use '@' when testing. */
	},
	{true, 'L', NULL, "kind with no long name" },
	{true, 'N', "nothingSpecial", "emit a normal tag" },
	{true, 'B', NULL, "beginning of an area for a guest" },
	{true, 'E', NULL, "end of an area for a guest" },
#if defined(DEBUG) && defined(HAVE_SECCOMP)
	{true, 'P', "callGetPPid", "trigger calling getppid(2) that seccomp sandbox disallows"},
#endif
	{false,'d', "disabled", "a kind disabled by default",
	 .referenceOnly = false, ATTACH_ROLES (CTST_DisabledKindRoles)},
	{true, 'e', "enabled", "a kind enabled by default",
	 .referenceOnly = false, ATTACH_ROLES (CTST_EnabledKindRoles)},
	{true, 'r', "roles", "emit a tag with multi roles",
	 .referenceOnly = true, ATTACH_ROLES (CTST_RolesKindRoles)},
	{false, 'R', "rolesDisabled", "emit a tag with multi roles(disabled by default)",
	 .referenceOnly = true, ATTACH_ROLES (CTST_RolesDisabledKindRoles)},
};

static void createCTSTTags (void)
{
	int i;
	const unsigned char *line;
	tagEntryInfo e;

	unsigned long lb = 0;
	unsigned long le = 0;

	int found_enabled_disabled[2] = {0, 0};

	TRACE_ENTER_TEXT("Parsing starts");

	while ((line = readLineFromInputFile ()) != NULL)
	{
		int c = line[0];

		for (i = 0; i < KIND_COUNT; i++)
			if ((c == CTST_Kinds[i].letter && i != K_NO_LETTER)
				|| (c == '@' && i == K_NO_LETTER))
			{
				switch (i)
				{
					case K_BROKEN:
						initTagEntry (&e, "one\nof\rbroken\tname", i);
						e.extensionFields.scopeKindIndex = K_BROKEN;
						e.extensionFields.scopeName = "\\Broken\tContext";
						makeTagEntry (&e);
						break;
					case K_NO_LETTER:
						initTagEntry (&e, "abnormal kindDefinition testing (no letter)", i);
						makeTagEntry (&e);
						break;
					case K_NO_LONG_NAME:
						initTagEntry (&e, "abnormal kindDefinition testing (no long name)", i);
						makeTagEntry (&e);
						break;
					case K_NOTHING_SPECIAL:
						if (!lb)
						{
							initTagEntry (&e, "NOTHING_SPECIAL", i);
							makeTagEntry (&e);
						}
						break;
					case K_GUEST_BEGINNING:
						lb = getInputLineNumber ();
						break;
					case K_GUEST_END:
						le = getInputLineNumber ();
						makePromise (SELF_TEST_PARSER, lb + 1, 0, le, 0, lb + 1);
						break;
#if defined(DEBUG) && defined(HAVE_SECCOMP)
				    case K_CALL_GETPPID:
						getppid();
						break;
#endif
				    case K_DISABLED:
				    case K_ENABLED:
						{
							int role;
							char *name;
							if (found_enabled_disabled[i == K_DISABLED]++ == 0)
							{
								role = ROLE_INDEX_DEFINITION;
								name = (i == K_DISABLED)
									? "disable-kind-no-role"
									: "enabled-kind-no-role";
							}
							else if (found_enabled_disabled[i == K_DISABLED]++ == 1)
							{
								role = (i == K_DISABLED)
									? R_DISABLED_KIND_DISABLED_ROLE
									: R_ENABLED_KIND_DISABLED_ROLE;
								name = (i == K_DISABLED)
									? "disable-kind-disabled-role"
									: "enabled-kind-disabled-role";
							}
							else
							{
								role = (i == K_DISABLED)
									? R_DISABLED_KIND_ENABLED_ROLE
									: R_ENABLED_KIND_ENABLED_ROLE;
								name = (i == K_DISABLED)
									? "disable-kind-enabled-role"
									: "enabled-kind-enabled-role";
							}
							initRefTagEntry (&e, name, i, role);
							makeTagEntry (&e);
							break;
						}
					case K_ROLES:
					{
						char *name = "multiRolesTarget";
						int qindex;
						tagEntryInfo *qe;

						initTagEntry (&e, name, i);
						assignRole(&e, R_ROLES_KIND_A_ROLE);
						assignRole(&e, R_ROLES_KIND_C_ROLE);
						assignRole(&e, R_ROLES_KIND_D_ROLE);
						qindex = makeTagEntry (&e);
						qe = getEntryInCorkQueue (qindex);
						assignRole(qe, R_ROLES_KIND_B_ROLE);
						break;
					}
					case K_ROLES_DISABLED:
					{
						char *name = "multiRolesDisabledTarget";

						initRefTagEntry (&e, name, i, R_ROLES_DISABLED_KIND_A_ROLE);
						makeTagEntry (&e);
						initRefTagEntry (&e, name, i, R_ROLES_DISABLED_KIND_B_ROLE);
						makeTagEntry (&e);
						break;
					}
				}
			}
	}

	TRACE_LEAVE();
}

static parserDefinition *CTagsSelfTestParser (void)
{
	static const char *const extensions[] = { NULL };
	parserDefinition *const def = parserNew (SELF_TEST_PARSER);
	def->extensions = extensions;
	def->kindTable = CTST_Kinds;
	def->kindCount = KIND_COUNT;
	def->parser = createCTSTTags;
	def->invisible = true;
	def->useMemoryStreamInput = true;
	def->useCork = true;
	return def;
}
