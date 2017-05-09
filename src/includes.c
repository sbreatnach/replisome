#include "includes.h"

#include "replisome.h"
#include "reldata.h"

#include "catalog/pg_collation.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/rel.h"


/* forward declarations */

#define cmd_cont(n) dlist_container(InclusionCommand, node, n)
static void cmds_init(InclusionCommands **cmds);
static bool cmds_is_empty(InclusionCommands *cmds);
static void cmds_push(InclusionCommands *cmds, InclusionCommand *cmd);
static InclusionCommand *cmds_tail(InclusionCommands *cmds);
static InclusionCommand *cmd_at_tail(InclusionCommands *cmds, CommandType type);

static void re_compile(regex_t *re, const char *p);
static bool re_match(regex_t *re, const char *s);

static bool jsonb_is_type(Datum jsonb, const char *type);
static char *jsonb_getattr(Datum jsonb, const char *attr);


void
inc_parse_include(DefElem *elem, InclusionCommands **cmds)
{
	Datum jsonb;
	char *t;
	InclusionCommand *cmd;

	cmds_init(cmds);

	jsonb = DirectFunctionCall1(jsonb_in, CStringGetDatum(strVal(elem->arg)));

	if (!jsonb_is_type(jsonb, "object")) {
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"%s\" must be a json object, got \"%s\"",
					 elem->defname, strVal(elem->arg))));
	}

	if ((t = jsonb_getattr(jsonb, "table")) != NULL) {
		cmd = cmd_at_tail(*cmds, CMD_INCLUDE_TABLE);
		cmd->table_name = t;
	}
	else if ((t = jsonb_getattr(jsonb, "tables")) != NULL) {
		cmd = cmd_at_tail(*cmds, CMD_INCLUDE_TABLE_PATTERN);
		re_compile(&cmd->table_re, t);
		pfree(t);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"%s\" not valid: \"%s\"",
					 elem->defname, strVal(elem->arg))));
	}

	pfree(DatumGetPointer(jsonb));
}

void
inc_parse_exclude(DefElem *elem, InclusionCommands **cmds)
{
	InclusionCommand *cmd;

	/* if the first command is an exclude, start including everything */
	cmds_init(cmds);
	if (cmds_is_empty(*cmds))
		cmd_at_tail(*cmds, CMD_INCLUDE_ALL);

	inc_parse_include(elem, cmds);
	cmd = cmds_tail(*cmds);
	switch (cmd->type)
	{
		case CMD_INCLUDE_TABLE:
			cmd->type = CMD_EXCLUDE_TABLE;
			break;

		case CMD_INCLUDE_TABLE_PATTERN:
			cmd->type = CMD_EXCLUDE_TABLE_PATTERN;
			break;

		default:
			Assert(false);
	}
}

/* Return True if a table should be included in the output */
bool
inc_should_emit(InclusionCommands *cmds, Relation relation)
{
	Form_pg_class class_form;
	dlist_iter iter;
	bool rv = false;

	class_form = RelationGetForm(relation);

	/* No command: include everything by default */
	if (cmds == NULL)
		return true;

	dlist_foreach(iter, &(cmds)->head)
	{
		InclusionCommand *cmd = cmd_cont(iter.cur);
		switch (cmd->type)
		{
			case CMD_INCLUDE_ALL:
				rv = true;
				break;

			case CMD_INCLUDE_TABLE:
				if (strcmp(cmd->table_name, NameStr(class_form->relname)) == 0)
					rv = true;
				break;

			case CMD_EXCLUDE_TABLE:
				if (strcmp(cmd->table_name, NameStr(class_form->relname)) == 0)
					rv = false;
				break;

			case CMD_INCLUDE_TABLE_PATTERN:
				if (re_match(&cmd->table_re, NameStr(class_form->relname)))
					rv = true;
				break;

			case CMD_EXCLUDE_TABLE_PATTERN:
				if (re_match(&cmd->table_re, NameStr(class_form->relname)))
					rv = false;
				break;

			default:
				Assert(false);
		}
	}

	elog(DEBUG1, "table \"%s\" matches include commands: %s",
		NameStr(class_form->relname), rv ? "yes" : "no");
	return rv;
}


/* Allocate a list of commands */
static void
cmds_init(InclusionCommands **cmds)
{
	if (*cmds == NULL)
		*cmds = palloc0(sizeof(InclusionCommands));
}


/* Return True if a list of commands is empty */
static bool
cmds_is_empty(InclusionCommands *cmds)
{
	return dlist_is_empty(&cmds->head);
}


/* Add a command at the end of a list of commands */
static void
cmds_push(InclusionCommands *cmds, InclusionCommand *cmd)
{
	dlist_push_tail(&cmds->head, &cmd->node);
}


/* Return the last command of a list */
static InclusionCommand *
cmds_tail(InclusionCommands *cmds)
{
	dlist_node *n = dlist_tail_node(&cmds->head);
	return cmd_cont(n);
}


/* Allocate a new command and add it at the end of a list */
static InclusionCommand *
cmd_at_tail(InclusionCommands *cmds, CommandType type)
{
	InclusionCommand *cmd = palloc0(sizeof(InclusionCommand));
	cmd->type = type;
	cmds_push(cmds, cmd);
	return cmd;
}


/* Compile a regular expression */
static void
re_compile(regex_t *re, const char *p)
{
	pg_wchar *wstr;
	int wlen;
	int r;

	wstr = palloc((strlen(p) + 1) * sizeof(pg_wchar));
	wlen = pg_mb2wchar(p, wstr);

	r = pg_regcomp(re, wstr, wlen, REG_ADVANCED, C_COLLATION_OID);
	if (r)
	{
		char errstr[100];
		pg_regerror(r, re, errstr, sizeof(errstr));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("invalid regular expression \"%s\": %s", p, errstr)));
	}

	pfree(wstr);
}


/* Return true if a regular expression matches a string */
static bool
re_match(regex_t *re, const char *s)
{
	pg_wchar *wstr;
	int wlen;
	int r;

	wstr = palloc((strlen(s) + 1) * sizeof(pg_wchar));
	wlen = pg_mb2wchar(s, wstr);

	r = pg_regexec(re, wstr, wlen, 0, NULL, 0, NULL, 0);
	pfree(wstr);
	if (r == REG_NOMATCH)
		return false;
	if (!r)
		return true;

	{
		char errstr[100];

		/* REG_NOMATCH is not an error, everything else is */
		pg_regerror(r, re, errstr, sizeof(errstr));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("regular expression match for \"%s\" failed: %s",
						s, errstr)));
	}
}


static bool jsonb_is_type(Datum jsonb, const char *type)
{
	char *cjtype;
	Datum jtype;
	bool rv;

	jtype = DirectFunctionCall1(jsonb_typeof, jsonb);
	cjtype = TextDatumGetCString(jtype);
	elog(DEBUG1, "json thing is type %s", cjtype);
	rv = (strcmp(cjtype, type) == 0);
	pfree(cjtype);
	pfree(DatumGetPointer(jtype));
	return rv;
}

static char *jsonb_getattr(Datum jsonb, const char *cattr)
{
	char *rv = NULL;
	Datum attr = CStringGetTextDatum(cattr);

	if (DatumGetBool(DirectFunctionCall2(jsonb_exists, jsonb, attr))) {
		Datum drv = DirectFunctionCall2(jsonb_object_field_text, jsonb, attr);
		rv = TextDatumGetCString(drv);
		elog(DEBUG1, "json attr %s = %s", cattr, rv);
		pfree(DatumGetPointer(drv));
	}
	else {
		elog(DEBUG1, "json attr %s not found", cattr);
	}

	pfree(DatumGetPointer(attr));
	return rv;
}