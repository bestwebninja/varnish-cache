/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2013 Varnish Software AS
 * All rights reserved.
 *
 * Author: Martin Blix Grydeland <martin@varnish-software.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "vas.h"
#include "vsb.h"
#include "miniobj.h"
#include "vapi/vsl.h"

#include "vxp.h"

static void vxp_expr_or(struct vxp *vxp, struct vex **pvex);

static void
vxp_expr_tag(struct vxp *vxp, struct vex_tag **ptag)
{

	/* XXX: Tag wildcards */
	AN(ptag);
	AZ(*ptag);
	if (vxp->t->tok != VAL) {
		VSB_printf(vxp->sb, "Expected VSL tag got '%.*s' ", PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	}
	ALLOC_OBJ(*ptag, VEX_TAG_MAGIC);
	AN(*ptag);
	(*ptag)->tag = VSL_Name2Tag(vxp->t->dec, -1);
	if ((*ptag)->tag == -1) {
		VSB_printf(vxp->sb, "Could not match '%.*s' to any tag ",
		    PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	} else if ((*ptag)->tag == -2) {
		VSB_printf(vxp->sb, "'%.*s' matches multiple tags ",
		    PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	}
	vxp_NextToken(vxp);

	/* XXX: Tag limiting operators ([], {}) */
}

static void
vxp_expr_num(struct vxp *vxp, struct vex_val **pval)
{
	char *endptr;

	AN(pval);
	AZ(*pval);
	if (vxp->t->tok != VAL) {
		VSB_printf(vxp->sb, "Expected number got '%.*s' ", PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	}
	AN(vxp->t->dec);
	ALLOC_OBJ(*pval, VEX_VAL_MAGIC);
	AN(*pval);
	if (strchr(vxp->t->dec, '.')) {
		(*pval)->type = VEX_FLOAT;
		(*pval)->val_float = strtod(vxp->t->dec, &endptr);
		while (isspace(*endptr))
			endptr++;
		if (*endptr != '\0') {
			VSB_printf(vxp->sb, "Floating point parse error ");
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
	} else {
		(*pval)->type = VEX_INT;
		(*pval)->val_int = strtoll(vxp->t->dec, &endptr, 0);
		while (isspace(*endptr))
			endptr++;
		if (*endptr != '\0') {
			VSB_printf(vxp->sb, "Integer parse error ");
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
	}
	vxp_NextToken(vxp);
}

static void
vxp_expr_str(struct vxp *vxp, struct vex_val **pval)
{

	AN(pval);
	AZ(*pval);
	if (vxp->t->tok != VAL) {
		VSB_printf(vxp->sb, "Expected string got '%.*s' ", PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	}
	AN(vxp->t->dec);
	ALLOC_OBJ(*pval, VEX_VAL_MAGIC);
	AN(*pval);
	(*pval)->type = VEX_STRING;
	(*pval)->val_string = strdup(vxp->t->dec);
	AN((*pval)->val_string);
	vxp_NextToken(vxp);
}

static void
vxp_expr_regex(struct vxp *vxp, struct vex_val **pval)
{
	const char *errptr;
	int erroff;

	/* XXX: Caseless option */

	AN(pval);
	AZ(*pval);
	if (vxp->t->tok != VAL) {
		VSB_printf(vxp->sb, "Expected regular expression got '%.*s' ",
		    PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	}
	AN(vxp->t->dec);
	ALLOC_OBJ(*pval, VEX_VAL_MAGIC);
	AN(*pval);
	(*pval)->type = VEX_REGEX;
	(*pval)->val_string = strdup(vxp->t->dec);
	(*pval)->val_regex = VRE_compile(vxp->t->dec, 0, &errptr, &erroff);
	if ((*pval)->val_regex == NULL) {
		AN(errptr);
		VSB_printf(vxp->sb, "Regular expression error: %s ", errptr);
		vxp_ErrWhere(vxp, vxp->t, erroff);
		return;
	}
	vxp_NextToken(vxp);
}

/*
 * SYNTAX:
 *   expr_cmp:
 *     tag
 *     tag <operator> num|str|regex
 */

static void
vxp_expr_cmp(struct vxp *vxp, struct vex **pvex)
{

	AN(pvex);
	AZ(*pvex);
	ALLOC_OBJ(*pvex, VEX_MAGIC);
	AN(*pvex);
	vxp_expr_tag(vxp, &(*pvex)->tag);
	ERRCHK(vxp);

	/* Test operator */
	switch (vxp->t->tok) {

	/* Single tag expressions don't take any more tokens */
	case EOI:
	case T_AND:
	case T_OR:
	case ')':
		return;

	/* Valid operators */
	case T_EQ:		/* == */
	case T_NEQ:		/* != */
	case T_SEQ:		/* eq */
	case T_SNEQ:		/* ne */
	case '~':		/* ~ */
	case T_NOMATCH:		/* !~ */
		(*pvex)->tok = vxp->t->tok;
		break;

	/* Error */
	default:
		VSB_printf(vxp->sb, "Expected operator got '%.*s' ",
		    PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	}
	vxp_NextToken(vxp);
	ERRCHK(vxp);

	/* Value */
	switch((*pvex)->tok) {
	case '\0':
		WRONG("Missing token");
	case T_EQ:		/* == */
	case T_GEQ:		/* >= */
	case T_LEQ:		/* <= */
	case T_NEQ:		/* != */
		vxp_expr_num(vxp, &(*pvex)->val);
		break;
	case T_SEQ:		/* eq */
	case T_SNEQ:		/* ne */
		vxp_expr_str(vxp, &(*pvex)->val);
		break;
	case '~':		/* ~ */
	case T_NOMATCH:		/* !~ */
		vxp_expr_regex(vxp, &(*pvex)->val);
		break;
	default:
		INCOMPL();
	}
}

/*
 * SYNTAX:
 *   expr_group:
 *     '(' expr_or ')'
 *     expr_not
 */

static void
vxp_expr_group(struct vxp *vxp, struct vex **pvex)
{

	AN(pvex);
	AZ(*pvex);

	if (vxp->t->tok == '(') {
		SkipToken(vxp, '(');
		vxp_expr_or(vxp, pvex);
		ERRCHK(vxp);
		SkipToken(vxp, ')');
		return;
	}

	vxp_expr_cmp(vxp, pvex);
}

/*
 * SYNTAX:
 *   expr_not:
 *     '!' expr_group
 *     expr_group
 */

static void
vxp_expr_not(struct vxp *vxp, struct vex **pvex)
{

	AN(pvex);
	AZ(*pvex);

	if (vxp->t->tok == '!') {
		ALLOC_OBJ(*pvex, VEX_MAGIC);
		AN(*pvex);
		(*pvex)->tok = vxp->t->tok;
		vxp_NextToken(vxp);
		vxp_expr_group(vxp, &(*pvex)->a);
		return;
	}

	vxp_expr_group(vxp, pvex);
	return;
}

/*
 * SYNTAX:
 *   expr_and:
 *     expr_not { 'and' expr_not }*
 */

static void
vxp_expr_and(struct vxp *vxp, struct vex **pvex)
{
	struct vex *a;

	AN(pvex);
	AZ(*pvex);
	vxp_expr_not(vxp, pvex);
	ERRCHK(vxp);
	while (vxp->t->tok == T_AND) {
		a = *pvex;
		ALLOC_OBJ(*pvex, VEX_MAGIC);
		AN(*pvex);
		(*pvex)->tok = vxp->t->tok;
		(*pvex)->a = a;
		vxp_NextToken(vxp);
		ERRCHK(vxp);
		vxp_expr_not(vxp, &(*pvex)->b);
		ERRCHK(vxp);
	}
}

/*
 * SYNTAX:
 *   expr_or:
 *     expr_and { 'or' expr_and }*
 */

static void
vxp_expr_or(struct vxp *vxp, struct vex **pvex)
{
	struct vex *a;

	AN(pvex);
	AZ(*pvex);
	vxp_expr_and(vxp, pvex);
	ERRCHK(vxp);
	while (vxp->t->tok == T_OR) {
		a = *pvex;
		ALLOC_OBJ(*pvex, VEX_MAGIC);
		AN(*pvex);
		(*pvex)->tok = vxp->t->tok;
		(*pvex)->a = a;
		vxp_NextToken(vxp);
		ERRCHK(vxp);
		vxp_expr_and(vxp, &(*pvex)->b);
		ERRCHK(vxp);
	}
}

/*
 * SYNTAX:
 *   expr:
 *     expr_or EOI
 */

static void
vxp_expr(struct vxp *vxp, struct vex **pvex)
{
	vxp_expr_or(vxp, pvex);
	ERRCHK(vxp);
	ExpectErr(vxp, EOI);
}

/*
 * Build a struct vex tree from the token list in vxp
 */

struct vex *
vxp_Parse(struct vxp *vxp)
{
	struct vex *vex = NULL;

	vxp->t = VTAILQ_FIRST(&vxp->tokens);
	if (vxp->t == NULL)
		return (NULL);

	vxp_expr(vxp, &vex);

	if (vxp->err) {
		if (vex)
			vex_Free(&vex);
		AZ(vex);
		return (NULL);
	}

	return (vex);
}

/*
 * Free a struct vex tree
 */

void
vex_Free(struct vex **pvex)
{

	if ((*pvex)->tag != NULL)
		FREE_OBJ((*pvex)->tag);
	if ((*pvex)->val != NULL) {
		if ((*pvex)->val->val_string)
			free((*pvex)->val->val_string);
		if ((*pvex)->val->val_regex)
			VRE_free(&(*pvex)->val->val_regex);
		FREE_OBJ((*pvex)->val);
	}
	if ((*pvex)->a != NULL) {
		vex_Free(&(*pvex)->a);
		AZ((*pvex)->a);
	}
	if ((*pvex)->b != NULL) {
		vex_Free(&(*pvex)->b);
		AZ((*pvex)->b);
	}
	FREE_OBJ(*pvex);
	*pvex = NULL;
}

#ifdef VXP_DEBUG

static void
vex_print_val(const struct vex_val *val)
{

	CHECK_OBJ_NOTNULL(val, VEX_VAL_MAGIC);
	switch (val->type) {
	case VEX_INT:
		fprintf(stderr, "INT=%jd", (intmax_t)val->val_int);
		break;
	case VEX_FLOAT:
		fprintf(stderr, "FLOAT=%f", val->val_float);
		break;
	case VEX_STRING:
		AN(val->val_string);
		fprintf(stderr, "STRING='%s'", val->val_string);
		break;
	case VEX_REGEX:
		AN(val->val_string);
		AN(val->val_regex);
		fprintf(stderr, "REGEX='%s'", val->val_string);
		break;
	default:
		WRONG("value type");
		break;
	}
}

static void
vex_print(const struct vex *vex, int indent)
{
	CHECK_OBJ_NOTNULL(vex, VEX_MAGIC);

	fprintf(stderr, "%*s%s", indent, "", vxp_tnames[vex->tok]);
	if (vex->tag != NULL) {
		CHECK_OBJ_NOTNULL(vex->tag, VEX_TAG_MAGIC);
		fprintf(stderr, " tag=%s", VSL_tags[vex->tag->tag]);
	}
	if (vex->val != NULL) {
		fprintf(stderr, " ");
		vex_print_val(vex->val);
	}
	fprintf(stderr, "\n");
	if (vex->a != NULL)
		vex_print(vex->a, indent + 2);
	if (vex->b != NULL)
		vex_print(vex->b, indent + 2);
}

void
vex_PrintTree(const struct vex *vex)
{

	CHECK_OBJ_NOTNULL(vex, VEX_MAGIC);
	fprintf(stderr, "VEX tree:\n");
	vex_print(vex, 2);
}

#endif /* VXP_DEBUG */
