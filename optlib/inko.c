/*
 * Generated by ./misc/optlib2c from optlib/inko.ctags, Don't edit this manually.
 */
#include "general.h"
#include "parse.h"
#include "routines.h"
#include "field.h"
#include "xtag.h"


static void initializeInkoParser (const langType language)
{

	addLanguageRegexTable (language, "toplevel");
	addLanguageRegexTable (language, "object");
	addLanguageRegexTable (language, "trait");
	addLanguageRegexTable (language, "method");
	addLanguageRegexTable (language, "comment");
	addLanguageRegexTable (language, "impl");
	addLanguageRegexTable (language, "let");
	addLanguageRegexTable (language, "sstring");
	addLanguageRegexTable (language, "dstring");
	addLanguageRegexTable (language, "tstring");

	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^'",
	                               "", "", "{tenter=sstring}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^\"",
	                               "", "", "{tenter=dstring}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^`",
	                               "", "", "{tenter=tstring}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^#",
	                               "", "", "{tenter=comment}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^[[:blank:]]*object[[:blank:]]+",
	                               "", "", "{tenter=object}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^[[:blank:]]*trait[[:blank:]]+",
	                               "", "", "{tenter=trait}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^[[:blank:]]*def[[:blank:]]+",
	                               "", "", "{tenter=method}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^[[:blank:]]*impl[[:blank:]]+",
	                               "", "", "{tenter=impl}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^[[:blank:]]*let[[:blank:]]+",
	                               "", "", "{tenter=let}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^\\{",
	                               "", "", "{placeholder}{scope=push}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^\\}",
	                               "", "", "{scope=pop}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^(@[a-zA-Z0-9_]+):",
	                               "\\1", "a", "{scope=ref}", NULL);
	addLanguageTagMultiTableRegex (language, "toplevel",
	                               "^.",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "object",
	                               "^([A-Z][a-zA-Z0-9_?]*)[^{]*",
	                               "\\1", "o", "{scope=push}", NULL);
	addLanguageTagMultiTableRegex (language, "object",
	                               "^\\{",
	                               "", "", "{tleave}", NULL);
	addLanguageTagMultiTableRegex (language, "object",
	                               "^.",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "trait",
	                               "^([A-Z][a-zA-Z0-9_?]*)[^{]*",
	                               "\\1", "t", "{scope=push}", NULL);
	addLanguageTagMultiTableRegex (language, "trait",
	                               "^\\{",
	                               "", "", "{tleave}", NULL);
	addLanguageTagMultiTableRegex (language, "trait",
	                               "^.",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "method",
	                               "^([a-zA-Z0-9_?]+|\\[\\]=?|\\^|&|\\||\\*|\\+|\\-|/|>>|<<|%)",
	                               "\\1", "m", "{scope=push}", NULL);
	addLanguageTagMultiTableRegex (language, "method",
	                               "^\\{|\n",
	                               "", "", "{scope=pop}{tleave}", NULL);
	addLanguageTagMultiTableRegex (language, "method",
	                               "^.",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "comment",
	                               "^\n",
	                               "", "", "{tleave}", NULL);
	addLanguageTagMultiTableRegex (language, "comment",
	                               "^.",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "impl",
	                               "^([A-Z][a-zA-Z0-9_?]*)[[:blank:]]+for[[:blank:]]+([A-Z][a-zA-Z0-9_?]*)[^{]*",
	                               "\\2", "r", "{scope=push}{_field=implements:\\1}", NULL);
	addLanguageTagMultiTableRegex (language, "impl",
	                               "^([A-Z][a-zA-Z0-9_?]*)[^{]*",
	                               "\\1", "r", "{scope=push}", NULL);
	addLanguageTagMultiTableRegex (language, "impl",
	                               "^\\{",
	                               "", "", "{tleave}", NULL);
	addLanguageTagMultiTableRegex (language, "impl",
	                               "^.",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "let",
	                               "^([A-Z][a-zA-Z0-9_]+)",
	                               "\\1", "c", "{scope=ref}", NULL);
	addLanguageTagMultiTableRegex (language, "let",
	                               "^=",
	                               "", "", "{tleave}", NULL);
	addLanguageTagMultiTableRegex (language, "let",
	                               "^.",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "sstring",
	                               "^'",
	                               "", "", "{tleave}", NULL);
	addLanguageTagMultiTableRegex (language, "sstring",
	                               "^\\\\'",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "sstring",
	                               "^.",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "dstring",
	                               "^\"",
	                               "", "", "{tleave}", NULL);
	addLanguageTagMultiTableRegex (language, "dstring",
	                               "^\\\\\"",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "dstring",
	                               "^.",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "tstring",
	                               "^`",
	                               "", "", "{tleave}", NULL);
	addLanguageTagMultiTableRegex (language, "tstring",
	                               "^\\\\`",
	                               "", "", "", NULL);
	addLanguageTagMultiTableRegex (language, "tstring",
	                               "^.",
	                               "", "", "", NULL);
}

extern parserDefinition* InkoParser (void)
{
	static const char *const extensions [] = {
		"inko",
		NULL
	};

	static const char *const aliases [] = {
		NULL
	};

	static const char *const patterns [] = {
		NULL
	};

	static kindDefinition InkoKindTable [] = {
		{
		  true, 'o', "object", "Object definition",
		},
		{
		  true, 'm', "method", "Method definition",
		},
		{
		  true, 't', "trait", "Trait definition",
		},
		{
		  true, 'a', "attribute", "Attribute definition",
		},
		{
		  true, 'c', "constant", "Constant definition",
		},
		{
		  true, 'r', "reopen", "Reopen object",
		},
	};
	static fieldDefinition InkoFieldTable [] = {
		{
		  .enabled     = true,
		  .name        = "implements",
		  .description = "Trait being implemented",
		},
	};

	parserDefinition* const def = parserNew ("Inko");

	def->enabled       = true;
	def->extensions    = extensions;
	def->patterns      = patterns;
	def->aliases       = aliases;
	def->method        = METHOD_NOT_CRAFTED|METHOD_REGEX;
	def->useCork       = CORK_QUEUE;
	def->kindTable     = InkoKindTable;
	def->kindCount     = ARRAY_SIZE(InkoKindTable);
	def->fieldTable    = InkoFieldTable;
	def->fieldCount    = ARRAY_SIZE(InkoFieldTable);
	def->initialize    = initializeInkoParser;

	return def;
}
