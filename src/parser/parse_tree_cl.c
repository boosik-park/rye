/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * parse_tree_cl.c - Parser module for the client
 */

#ident "$Id$"

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <float.h>
#include <assert.h>
#include <math.h>

#include "porting.h"
#include "parser.h"
#include "parser_message.h"
#include "misc_string.h"
#include "rsql_grammar_scan.h"
#include "memory_alloc.h"
#include "language_support.h"
#include "object_print.h"
#include "optimizer.h"
#include "system_parameter.h"
#include "set_object.h"

#define SAFENUM(node, field)    ((node) ? (node)->field : -1)
#define PT_MEMB_BUF_SIZE        100
#define PT_MEMB_PRINTABLE_BUF_SIZE    512
#define PT_MEMB_ERR_BUF_SIZE    256
#define MAX_STRING_SEGMENT_LENGTH 254

typedef struct pt_lambda_arg PT_LAMBDA_ARG;
struct pt_lambda_arg
{
  PT_NODE *name;
  PT_NODE *tree;
  int type;			/* 1: reduce_equality_terms, 0: o/w */
  int replace_num;
  bool loc_check;
  bool dont_replace;
};

typedef struct pt_walk_arg PT_WALK_ARG;
struct pt_walk_arg
{
  PT_NODE_WALK_FUNCTION pre_function;
  void *pre_argument;
  PT_NODE_WALK_FUNCTION post_function;
  void *post_argument;
  int continue_walk;
};

typedef struct pt_string_block PT_STRING_BLOCK;
struct pt_string_block
{
  char *body;
  int length;
  int size;
};

PARSER_INIT_NODE_FUNC *pt_init_f = NULL;
PARSER_PRINT_NODE_FUNC *pt_print_f = NULL;
PARSER_APPLY_NODE_FUNC *pt_apply_f = NULL;
PARSER_CONTEXT *parent_parser = NULL;

static void strcat_with_realloc (PT_STRING_BLOCK * sb, const char *tail);
static PT_NODE *pt_lambda_check_reduce_eq (PARSER_CONTEXT * parser,
					   PT_NODE * tree_or_name,
					   void *void_arg,
					   int *continue_walk);
static PT_NODE *pt_lambda_node (PARSER_CONTEXT * parser,
				PT_NODE * tree_or_name, void *void_arg,
				int *continue_walk);
static PT_NODE *copy_node_in_tree_pre (PARSER_CONTEXT * parser,
				       PT_NODE * old_node, void *arg,
				       int *continue_walk);
static PT_NODE *free_node_in_tree_pre (PARSER_CONTEXT * parser,
				       PT_NODE * node, void *arg,
				       int *continue_walk);
static PT_NODE *free_node_in_tree_post (PARSER_CONTEXT * parser,
					PT_NODE * node, void *arg,
					int *continue_walk);
static PT_NODE *pt_walk_private (PARSER_CONTEXT * parser, PT_NODE * node,
				 void *void_arg);

static PARSER_VARCHAR *pt_append_quoted_string (const PARSER_CONTEXT * parser,
						PARSER_VARCHAR * buf,
						const char *str,
						size_t str_length);
static PARSER_VARCHAR *pt_append_string_prefix (const PARSER_CONTEXT * parser,
						PARSER_VARCHAR * buf,
						const PT_NODE * value);

static void pt_init_apply_f (void);
static void pt_init_init_f (void);
static void pt_init_print_f (void);

/*
 * Note :
 * When adding new functions, be sure to add to ALL 4 function types and
 * ALL 4 function vectors.  (apply, init, print, tree_print
 */

static PT_NODE *pt_apply_get_opt_lvl (PARSER_CONTEXT * parser, PT_NODE * p,
				      PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_get_xaction (PARSER_CONTEXT * parser, PT_NODE * p,
				      PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_isolation_lvl (PARSER_CONTEXT * parser, PT_NODE * p,
					PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_savepoint (PARSER_CONTEXT * parser, PT_NODE * p,
				    PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_set_opt_lvl (PARSER_CONTEXT * parser, PT_NODE * p,
				      PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_set_sys_params (PARSER_CONTEXT * parser, PT_NODE * p,
					 PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_timeout (PARSER_CONTEXT * parser, PT_NODE * p,
				  PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_alter_index (PARSER_CONTEXT * parser, PT_NODE * p,
				      PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_alter (PARSER_CONTEXT * parser, PT_NODE * p,
				PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_alter_user (PARSER_CONTEXT * parser, PT_NODE * p,
				     PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_attr_def (PARSER_CONTEXT * parser, PT_NODE * p,
				   PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_attr_ordering (PARSER_CONTEXT * parser, PT_NODE * p,
					PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_auth_cmd (PARSER_CONTEXT * parser, PT_NODE * p,
				   PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_commit_work (PARSER_CONTEXT * parser, PT_NODE * p,
				      PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_constraint (PARSER_CONTEXT * parser, PT_NODE * p,
				     PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_create_entity (PARSER_CONTEXT * parser, PT_NODE * p,
					PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_create_index (PARSER_CONTEXT * parser, PT_NODE * p,
				       PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_create_user (PARSER_CONTEXT * parser, PT_NODE * p,
				      PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_data_default (PARSER_CONTEXT * parser, PT_NODE * p,
				       PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_datatype (PARSER_CONTEXT * parser, PT_NODE * p,
				   PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_delete (PARSER_CONTEXT * parser, PT_NODE * p,
				 PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_difference (PARSER_CONTEXT * parser, PT_NODE * p,
				     PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_dot (PARSER_CONTEXT * parser, PT_NODE * p,
			      PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_drop_index (PARSER_CONTEXT * parser, PT_NODE * p,
				     PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_drop (PARSER_CONTEXT * parser, PT_NODE * p,
			       PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_drop_user (PARSER_CONTEXT * parser, PT_NODE * p,
				    PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_error_msg (PARSER_CONTEXT * parser, PT_NODE * p,
				    PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_expr (PARSER_CONTEXT * parser, PT_NODE * p,
			       PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_function (PARSER_CONTEXT * parser, PT_NODE * p,
				   PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_grant (PARSER_CONTEXT * parser, PT_NODE * p,
				PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_host_var (PARSER_CONTEXT * parser, PT_NODE * p,
				   PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_insert (PARSER_CONTEXT * parser, PT_NODE * p,
				 PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_intersection (PARSER_CONTEXT * parser, PT_NODE * p,
				       PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_name (PARSER_CONTEXT * parser, PT_NODE * p,
			       PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_node_list (PARSER_CONTEXT * parser, PT_NODE * p,
				    PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_pointer (PARSER_CONTEXT * parser, PT_NODE * p,
				  PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_rename (PARSER_CONTEXT * parser, PT_NODE * p,
				 PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_revoke (PARSER_CONTEXT * parser, PT_NODE * p,
				 PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_rollback_work (PARSER_CONTEXT * parser, PT_NODE * p,
					PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_select (PARSER_CONTEXT * parser, PT_NODE * p,
				 PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_sort_spec (PARSER_CONTEXT * parser, PT_NODE * p,
				    PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_spec (PARSER_CONTEXT * parser, PT_NODE * p,
			       PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_table_option (PARSER_CONTEXT * parser, PT_NODE * p,
				       PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_union_stmt (PARSER_CONTEXT * parser, PT_NODE * p,
				     PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_update (PARSER_CONTEXT * parser, PT_NODE * p,
				 PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_update_stats (PARSER_CONTEXT * parser, PT_NODE * p,
				       PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_value (PARSER_CONTEXT * parser, PT_NODE * p,
				PT_NODE_FUNCTION g, void *arg);
static PT_NODE *pt_apply_query_trace (PARSER_CONTEXT * parser, PT_NODE * p,
				      PT_NODE_FUNCTION g, void *arg);

static PARSER_APPLY_NODE_FUNC pt_apply_func_array[PT_NODE_NUMBER];


static PT_NODE *pt_init_get_opt_lvl (PT_NODE * p);
static PT_NODE *pt_init_get_xaction (PT_NODE * p);
static PT_NODE *pt_init_isolation_lvl (PT_NODE * p);
static PT_NODE *pt_init_savepoint (PT_NODE * p);
static PT_NODE *pt_init_set_opt_lvl (PT_NODE * p);
static PT_NODE *pt_init_set_sys_params (PT_NODE * p);
static PT_NODE *pt_init_timeout (PT_NODE * p);
static PT_NODE *pt_init_alter_index (PT_NODE * p);
static PT_NODE *pt_init_alter (PT_NODE * p);
static PT_NODE *pt_init_alter_user (PT_NODE * p);
static PT_NODE *pt_init_attr_def (PT_NODE * p);
static PT_NODE *pt_init_attr_ordering (PT_NODE * p);
static PT_NODE *pt_init_auth_cmd (PT_NODE * p);
static PT_NODE *pt_init_commit_work (PT_NODE * p);
static PT_NODE *pt_init_constraint (PT_NODE * node);
static PT_NODE *pt_init_create_entity (PT_NODE * p);
static PT_NODE *pt_init_create_index (PT_NODE * p);
static PT_NODE *pt_init_create_user (PT_NODE * p);
static PT_NODE *pt_init_data_default (PT_NODE * p);
static PT_NODE *pt_init_datatype (PT_NODE * p);
static PT_NODE *pt_init_delete (PT_NODE * p);
static PT_NODE *pt_init_difference (PT_NODE * p);
static PT_NODE *pt_init_dot (PT_NODE * p);
static PT_NODE *pt_init_drop_index (PT_NODE * p);
static PT_NODE *pt_init_drop (PT_NODE * p);
static PT_NODE *pt_init_drop_user (PT_NODE * p);
static PT_NODE *pt_init_error_msg (PT_NODE * p);
static PT_NODE *pt_init_expr (PT_NODE * p);
static PT_NODE *pt_init_function (PT_NODE * p);
static PT_NODE *pt_init_grant (PT_NODE * p);
static PT_NODE *pt_init_host_var (PT_NODE * p);
static PT_NODE *pt_init_insert (PT_NODE * p);
static PT_NODE *pt_init_intersection (PT_NODE * p);
static PT_NODE *pt_init_name (PT_NODE * p);
static PT_NODE *pt_init_node_list (PT_NODE * p);
static PT_NODE *pt_init_pointer (PT_NODE * node);
static PT_NODE *pt_init_rename (PT_NODE * p);
static PT_NODE *pt_init_revoke (PT_NODE * p);
static PT_NODE *pt_init_rollback_work (PT_NODE * p);
static PT_NODE *pt_init_select (PT_NODE * p);
static PT_NODE *pt_init_sort_spec (PT_NODE * p);
static PT_NODE *pt_init_spec (PT_NODE * p);
static PT_NODE *pt_init_table_option (PT_NODE * p);
static PT_NODE *pt_init_union_stmt (PT_NODE * p);
static PT_NODE *pt_init_update_stats (PT_NODE * p);
static PT_NODE *pt_init_update (PT_NODE * p);
static PT_NODE *pt_init_value (PT_NODE * p);
static PT_NODE *pt_init_query_trace (PT_NODE * p);

static PARSER_INIT_NODE_FUNC pt_init_func_array[PT_NODE_NUMBER];

static PARSER_VARCHAR *pt_print_alter_index (PARSER_CONTEXT * parser,
					     PT_NODE * p);
static PARSER_VARCHAR *pt_print_alter (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_alter_user (PARSER_CONTEXT * parser,
					    PT_NODE * p);
static PARSER_VARCHAR *pt_print_attr_def (PARSER_CONTEXT * parser,
					  PT_NODE * p);
static PARSER_VARCHAR *pt_print_attr_ordering (PARSER_CONTEXT * parser,
					       PT_NODE * p);
static PARSER_VARCHAR *pt_print_auth_cmd (PARSER_CONTEXT * parser,
					  PT_NODE * p);
static PARSER_VARCHAR *pt_print_commit_work (PARSER_CONTEXT * parser,
					     PT_NODE * p);
static PARSER_VARCHAR *pt_print_constraint (PARSER_CONTEXT * parser,
					    PT_NODE * p);
static PARSER_VARCHAR *pt_print_col_def_constraint (PARSER_CONTEXT * parser,
						    PT_NODE * p);
static PARSER_VARCHAR *pt_print_create_entity (PARSER_CONTEXT * parser,
					       PT_NODE * p);
static PARSER_VARCHAR *pt_print_create_index (PARSER_CONTEXT * parser,
					      PT_NODE * p);
static PARSER_VARCHAR *pt_print_create_user (PARSER_CONTEXT * parser,
					     PT_NODE * p);
static PARSER_VARCHAR *pt_print_data_default (PARSER_CONTEXT * parser,
					      PT_NODE * p);
static PARSER_VARCHAR *pt_print_datatype (PARSER_CONTEXT * parser,
					  PT_NODE * p);
static PARSER_VARCHAR *pt_print_delete (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_difference (PARSER_CONTEXT * parser,
					    PT_NODE * p);
static PARSER_VARCHAR *pt_print_dot (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_drop_index (PARSER_CONTEXT * parser,
					    PT_NODE * p);
static PARSER_VARCHAR *pt_print_drop (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_drop_user (PARSER_CONTEXT * parser,
					   PT_NODE * p);
static PARSER_VARCHAR *pt_print_error_msg (PARSER_CONTEXT * parser,
					   PT_NODE * p);
static PARSER_VARCHAR *pt_print_expr (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_function (PARSER_CONTEXT * parser,
					  PT_NODE * p);
static PARSER_VARCHAR *pt_print_get_opt_lvl (PARSER_CONTEXT * parser,
					     PT_NODE * p);
static PARSER_VARCHAR *pt_print_get_xaction (PARSER_CONTEXT * parser,
					     PT_NODE * p);
static PARSER_VARCHAR *pt_print_grant (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_host_var (PARSER_CONTEXT * parser,
					  PT_NODE * p);
static PARSER_VARCHAR *pt_print_insert (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_intersection (PARSER_CONTEXT * parser,
					      PT_NODE * p);
static PARSER_VARCHAR *pt_print_isolation_lvl (PARSER_CONTEXT * parser,
					       PT_NODE * p);
static PARSER_VARCHAR *pt_print_name (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_node_list (PARSER_CONTEXT * parser,
					   PT_NODE * p);
static PARSER_VARCHAR *pt_print_pointer (PARSER_CONTEXT * parser,
					 PT_NODE * p);
static PARSER_VARCHAR *pt_print_rename (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_revoke (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_rollback_work (PARSER_CONTEXT * parser,
					       PT_NODE * p);
static PARSER_VARCHAR *pt_print_savepoint (PARSER_CONTEXT * parser,
					   PT_NODE * p);
static PARSER_VARCHAR *pt_print_select (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_set_opt_lvl (PARSER_CONTEXT * parser,
					     PT_NODE * p);
static PARSER_VARCHAR *pt_print_set_sys_params (PARSER_CONTEXT * parser,
						PT_NODE * p);
static PARSER_VARCHAR *pt_print_sort_spec (PARSER_CONTEXT * parser,
					   PT_NODE * p);
static PARSER_VARCHAR *pt_print_spec (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_table_option (PARSER_CONTEXT * parser,
					      PT_NODE * p);
static PARSER_VARCHAR *pt_print_timeout (PARSER_CONTEXT * parser,
					 PT_NODE * p);
static PARSER_VARCHAR *pt_print_union_stmt (PARSER_CONTEXT * parser,
					    PT_NODE * p);
static PARSER_VARCHAR *pt_print_update (PARSER_CONTEXT * parser, PT_NODE * p);
static PARSER_VARCHAR *pt_print_update_stats (PARSER_CONTEXT * parser,
					      PT_NODE * p);
static PARSER_VARCHAR *pt_print_value (PARSER_CONTEXT * parser, PT_NODE * p);

static PARSER_VARCHAR *pt_print_index_columns (PARSER_CONTEXT * parser,
					       PT_NODE * p);

static PARSER_VARCHAR *pt_print_query_trace (PARSER_CONTEXT * parser,
					     PT_NODE * p);

static PARSER_PRINT_NODE_FUNC pt_print_func_array[PT_NODE_NUMBER];

extern char *g_query_string;
extern int g_query_string_len;

static PT_QUERY_INFO *pt_init_query_info (PT_QUERY_INFO * query);

/*
 * strcat_with_realloc () -
 *   return:
 *   PT_STRING_BLOCK(in/out):
 *   tail(in):
 */
static void
strcat_with_realloc (PT_STRING_BLOCK * sb, const char *tail)
{
  char *cp = sb->body;
  int margin = 32;

  if (sb->size - sb->length < strlen (tail) + margin)
    {
      sb->size = (sb->size + strlen (tail) + margin) * 2;
      sb->body = realloc (sb->body, sb->size);
      cp = sb->body;
    }

  strcat (cp, tail);
  sb->length = sb->length + strlen (tail);
}

/*
 * pt_lambda_check_reduce_eq () -
 *   return:
 *   parser(in):
 *   tree_or_name(in/out):
 *   void_arg(in):
 *   continue_walk(in/out):
 */
static PT_NODE *
pt_lambda_check_reduce_eq (PARSER_CONTEXT * parser,
			   PT_NODE * tree_or_name,
			   void *void_arg, int *continue_walk)
{
  PT_LAMBDA_ARG *lambda_arg = (PT_LAMBDA_ARG *) void_arg;
  PT_NODE *arg1, *tree, *name;

  if (!tree_or_name)
    {
      return tree_or_name;
    }

  switch (tree_or_name->node_type)
    {
    case PT_DOT_:
      arg1 = tree_or_name->info.dot.arg1;
      if (arg1 && arg1->node_type == PT_NAME)
	{
	  PT_NAME_INFO_SET_FLAG (arg1, PT_NAME_INFO_DOT_SPEC);
	}
      break;
    case PT_EXPR:
      tree = lambda_arg->tree;
      name = lambda_arg->name;

      /* check for variable string type */
      if (tree->type_enum == PT_TYPE_VARCHAR
	  || tree->type_enum == PT_TYPE_VARBIT)
	{
	  switch (tree_or_name->info.expr.op)
	    {
	    case PT_BIT_LENGTH:
	    case PT_OCTET_LENGTH:
	    case PT_CHAR_LENGTH:
	      *continue_walk = PT_LIST_WALK;	/* don't dive into */
	      break;
	    case PT_CAST:
	      if (PT_HAS_COLLATION (name->type_enum)
		  && tree_or_name->info.expr.op == PT_CAST
		  && PT_HAS_COLLATION (tree_or_name->type_enum)
		  && pt_name_equal (parser, name,
				    tree_or_name->info.expr.arg1))
		{
		  int cast_coll = LANG_SYS_COLLATION;
		  int name_coll = LANG_SYS_COLLATION;

		  if (tree_or_name->data_type != NULL)
		    {
		      cast_coll =
			tree_or_name->data_type->info.data_type.collation_id;
		    }

		  if (cast_coll != name_coll)
		    {
		      /* predicate evaluates with different collation */
		      *continue_walk = PT_LIST_WALK;	/* don't dive into */
		    }
		}
	    default:
	      break;
	    }
	}
      break;
    default:
      break;
    }

  return tree_or_name;
}


/*
 * pt_lambda_node () - applies the lambda test to the node passed to it,
 * 	and conditionally substitutes a copy of its corresponding tree
 *   return:
 *   parser(in):
 *   tree_or_name(in/out):
 *   void_arg(in/out):
 *   continue_walk(in/out):
 */
static PT_NODE *
pt_lambda_node (PARSER_CONTEXT * parser,
		PT_NODE * tree_or_name, void *void_arg, int *continue_walk)
{
  PT_LAMBDA_ARG *lambda_arg = (PT_LAMBDA_ARG *) void_arg;
  PT_NODE *name_node, *lambda_name, *result, *next, *temp;

  *continue_walk = PT_CONTINUE_WALK;

  if (!tree_or_name)
    {
      return tree_or_name;
    }

  if (tree_or_name->node_type == PT_FUNCTION)
    {
      switch (tree_or_name->info.function.function_type)
	{
	case F_SEQUENCE:
#if 0				/* TODO - */
	  assert (false);	/* should not reach here */
#endif
	  if (lambda_arg->replace_num > 0)
	    {
	      /* check normal func data_type
	         1: reduce_equality_terms - check normal func data_type */
	      if (lambda_arg->type == 1)
		{
		  /* at here, do clear. later is updated in pt_semantic_type */
		  tree_or_name->type_enum = PT_TYPE_NONE;
		}

	      lambda_arg->replace_num = 0;
	    }
	  break;
	default:
	  break;
	}

      return tree_or_name;
    }

  name_node = tree_or_name;
  lambda_name = lambda_arg->name;
  while (name_node->node_type == PT_DOT_ && lambda_name->node_type == PT_DOT_)
    {
      name_node = name_node->info.dot.arg2;
      lambda_name = lambda_name->info.dot.arg2;
    }

  /* change orderby_num() to groupby_num() */
  if (tree_or_name->node_type == PT_EXPR
      && tree_or_name->info.expr.op == PT_ORDERBY_NUM)
    {
      if (lambda_name->node_type == PT_EXPR
	  && lambda_name->info.expr.op == PT_ORDERBY_NUM)
	{			/* found match */
	  /* replace 'tree_or_name' node with 'lambda_arg->tree' */
	  next = tree_or_name->next;
	  result = parser_copy_tree_list (parser, lambda_arg->tree);
	  parser_free_node (parser, tree_or_name);
	  for (temp = result; temp->next; temp = temp->next)
	    {
	      ;
	    }
	  temp->next = next;

	  lambda_arg->replace_num++;

	  return result;
	}
    }

  if (name_node->node_type != PT_NAME || lambda_name->node_type != PT_NAME)
    {
      return tree_or_name;
    }

  if (PT_NAME_INFO_IS_FLAGED (name_node, PT_NAME_INFO_DOT_SPEC))
    {
      /* never rewrites a path expression (e.g, oid = ? and oid.i = 1) */
      return tree_or_name;
    }

  if (lambda_arg->loc_check == true
      && (name_node->info.name.location == 0
	  || (name_node->info.name.location !=
	      lambda_name->info.name.location)))
    {
      /* WHERE condition or different ON location */
      return tree_or_name;
    }

  if (pt_name_equal (parser, name_node, lambda_name))
    {
      if (lambda_arg->dont_replace)
	{			/* don't replace, only marking */
	  temp = pt_get_end_path_node (tree_or_name);
	  if (temp->node_type == PT_NAME)
	    {
	      PT_NAME_INFO_SET_FLAG (temp, PT_NAME_INFO_CONSTANT);
	    }

	  return tree_or_name;
	}

      /* replace 'tree_or_name' node with 'lambda_arg->tree' */
      next = tree_or_name->next;
      result = parser_copy_tree_list (parser, lambda_arg->tree);
      parser_free_node (parser, tree_or_name);
      for (temp = result; temp->next; temp = temp->next)
	{
	  ;
	}
      temp->next = next;

      lambda_arg->replace_num++;
    }
  else
    {				/* did not match */
      result = tree_or_name;
    }

  return result;
}


/*
 * pt_copy_node () - copies exactly a node passed to it, and returns
 * 	a pointer to the copy. It is eligible for a walk "pre" function
 *   return:
 *   parser(in):
 *   old_node(in):
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
copy_node_in_tree_pre (PARSER_CONTEXT * parser,
		       PT_NODE * old_node, UNUSED_ARG void *arg,
		       UNUSED_ARG int *continue_walk)
{
  PT_NODE *new_node;

  new_node = parser_new_node (parser, old_node->node_type);
  if (new_node == NULL)
    {
      PT_INTERNAL_ERROR (parser, "allocate new node");
      return NULL;
    }

  *new_node = *old_node;

  /* if node is copied from another parser context, deepcopy string contents */
  if (old_node->parser_id != parser->id)
    {
      if (new_node->node_type == PT_NAME)
	{
	  new_node->info.name.original =
	    pt_append_string (parser, NULL, old_node->info.name.original);
	  new_node->info.name.resolved =
	    pt_append_string (parser, NULL, old_node->info.name.resolved);
	}
      else if (new_node->node_type == PT_VALUE)
	{
	  if (new_node->info.value.text)
	    {
	      new_node->info.value.text =
		pt_append_string (parser, NULL, old_node->info.value.text);
	    }
	}
    }

  /* if we are operating in a context of db_values, copy it too */
  if (new_node->node_type == PT_VALUE
      && new_node->info.value.db_value_is_in_workspace
      && new_node->info.value.db_value_is_initialized)
    {
      DB_MAKE_NULL (&new_node->info.value.db_value);
      if (db_value_clone (&old_node->info.value.db_value,
			  &new_node->info.value.db_value) < 0)
	{
	  PT_ERRORc (parser, new_node, er_msg ());
	}
      else
	{
	  new_node->info.value.db_value_is_in_workspace = 1;
	}
    }

  new_node->parser_id = parser->id;

  return new_node;
}


/*
 * pt_walk_private () - implements the higher order tree walk routine parser_walk_tree
 *   return:
 *   parser(in):
 *   node(in/out):
 *   void_arg(in/out):
 */
static PT_NODE *
pt_walk_private (PARSER_CONTEXT * parser, PT_NODE * node, void *void_arg)
{
  PT_WALK_ARG *walk = (PT_WALK_ARG *) void_arg;
  PT_NODE_TYPE node_type;
  PARSER_APPLY_NODE_FUNC apply;
  int save_continue;

  if (node && walk->pre_function)
    {
      node = (*walk->pre_function) (parser,
				    node, walk->pre_argument,
				    &(walk->continue_walk));
    }

  if (node)
    {
      if (walk->continue_walk != PT_STOP_WALK)
	{
	  /* walking leaves may write over this. */
	  save_continue = walk->continue_walk;

	  /* visit sub-trees */
	  if (save_continue == PT_CONTINUE_WALK
	      || save_continue == PT_LEAF_WALK)
	    {
	      /* this is an optimization to remove a procedure call
	       * per node from the recursion path. It is the same as
	       * calling pt_apply.
	       */
	      node_type = node->node_type;

	      if (node_type >= PT_LAST_NODE_NUMBER
		  || !(apply = pt_apply_f[node_type]))
		{
		  return NULL;
		}

	      (*apply) (parser, node, pt_walk_private, walk);

	      if (node->data_type)
		{
		  node->data_type = pt_walk_private (parser,
						     node->data_type, walk);
		}
	    }

	  /* visit rest of list
	     first, follow 'or_next' list */
	  if (node->or_next
	      && (save_continue == PT_CONTINUE_WALK
		  || save_continue == PT_LEAF_WALK
		  || save_continue == PT_LIST_WALK))
	    {
	      node->or_next = pt_walk_private (parser, node->or_next, walk);
	    }

	  /* then, follow 'next' list */
	  if (node->next
	      && (save_continue == PT_CONTINUE_WALK
		  || save_continue == PT_LIST_WALK))
	    {
	      node->next = pt_walk_private (parser, node->next, walk);
	    }

	  if (walk->continue_walk != PT_STOP_WALK)
	    {
	      walk->continue_walk = save_continue;
	    }
	}

      /* and visit this node again */
      if (walk->post_function)
	{
	  node = (*walk->post_function) (parser, node,
					 walk->post_argument,
					 &(walk->continue_walk));
	}
    }

  return node;
}


/*
 * parser_walk_leaves () - like parser_walk_tree, but begins at the leaves of
 *                     the node passed in
 *   return:
 *   parser(in):
 *   node(in):
 *   pre_function(in):
 *   pre_argument(in):
 *   post_function(in):
 *   post_argument(in):
 */
PT_NODE *
parser_walk_leaves (PARSER_CONTEXT * parser,
		    PT_NODE * node,
		    PT_NODE_WALK_FUNCTION pre_function,
		    void *pre_argument,
		    PT_NODE_WALK_FUNCTION post_function, void *post_argument)
{
  PARSER_APPLY_NODE_FUNC apply;
  PT_NODE_TYPE node_type;
  PT_WALK_ARG walk_argument;
  PT_NODE *walk;

  walk_argument.continue_walk = PT_CONTINUE_WALK;
  walk_argument.pre_function = pre_function;
  walk_argument.pre_argument = pre_argument;
  walk_argument.post_function = post_function;
  walk_argument.post_argument = post_argument;

  for (walk = node; walk; walk = walk->or_next)
    {
      node_type = walk->node_type;

      if (node_type >= PT_LAST_NODE_NUMBER
	  || !(apply = pt_apply_f[node_type]))
	{
	  return NULL;
	}

      (*apply) (parser, walk, pt_walk_private, &walk_argument);

      if (walk->data_type)
	{
	  walk->data_type = pt_walk_private (parser,
					     walk->data_type, &walk_argument);
	}
    }

  return node;
}


/*
 * parser_walk_tree () - walks a tree and applies pre and post visit routines
 *              to each node in the tree. A pre function may prune
 *              the search by returning a false (0) in the continue argument
 *   return:
 *   parser(in):
 *   node(in):
 *   pre_function(in):
 *   pre_argument(in):
 *   post_function(in):
 *   post_argument(in):
 */
PT_NODE *
parser_walk_tree (PARSER_CONTEXT * parser,
		  PT_NODE * node,
		  PT_NODE_WALK_FUNCTION pre_function,
		  void *pre_argument,
		  PT_NODE_WALK_FUNCTION post_function, void *post_argument)
{
  PT_WALK_ARG walk_argument;

  walk_argument.continue_walk = PT_CONTINUE_WALK;
  walk_argument.pre_function = pre_function;
  walk_argument.pre_argument = pre_argument;
  walk_argument.post_function = post_function;
  walk_argument.post_argument = post_argument;

  return pt_walk_private (parser, node, &walk_argument);
}


/*
 * pt_continue_walk () - Re-enabled the tree walk after a portion was "pruned"
 *   return:
 *   parser(in):
 *   tree(in):
 *   arg(in):
 *   continue_walk(in/out):
 */
PT_NODE *
pt_continue_walk (UNUSED_ARG PARSER_CONTEXT * parser, PT_NODE * tree,
		  UNUSED_ARG void *arg, int *continue_walk)
{
  *continue_walk = PT_CONTINUE_WALK;
  return tree;
}

/*
 * pt_lambda_with_arg () - walks a tree and modifies it in place to replace
 * 	                   name nodes with copies of a corresponding tree
 *   return:
 *   parser(in):
 *   tree_with_names(in):
 *   name_node(in):
 *   corresponding_tree(in):
 *   loc_check(in):
 *   type(in):
 *   dont_replace(in):
 */
PT_NODE *
pt_lambda_with_arg (PARSER_CONTEXT * parser, PT_NODE * tree_with_names,
		    PT_NODE * name_node, PT_NODE * corresponding_tree,
		    bool loc_check, int type, bool dont_replace)
{
  PT_LAMBDA_ARG lambda_arg;
  PT_NODE *tree;
  int save_paren_type = 0;
  bool arg_ok;

  arg_ok = false;

  if (name_node->node_type == PT_NAME || name_node->node_type == PT_DOT_)
    {
      arg_ok = true;
    }
  else
    if (name_node->node_type == PT_EXPR
	&& name_node->info.expr.op == PT_ORDERBY_NUM)
    {
      if (corresponding_tree)
	{
	  if ((corresponding_tree->node_type == PT_FUNCTION
	       && corresponding_tree->info.function.function_type ==
	       PT_GROUPBY_NUM)
	      || (corresponding_tree->node_type == PT_EXPR
		  && corresponding_tree->info.expr.op == PT_INST_NUM))
	    {
	      /* change orderby_num() to groupby_num() */
	      /* change orderby_num() to inst_num() */
	      arg_ok = true;
	    }
	}
    }

  if (arg_ok != true)
    {
      PT_INTERNAL_ERROR (parser, "lambda");
      return tree_with_names;
    }

  lambda_arg.type = type;
  lambda_arg.name = name_node;
  lambda_arg.tree = corresponding_tree;
  lambda_arg.loc_check = loc_check;
  lambda_arg.dont_replace = dont_replace;
  lambda_arg.replace_num = 0;
  if (corresponding_tree && corresponding_tree->node_type == PT_EXPR)
    {
      /* make sure it will print with proper precedance.
         we don't want to replace "name" with "1+2"
         in 4*name, and get 4*1+2. It should be 4*(1+2) instead. */
      save_paren_type = corresponding_tree->info.expr.paren_type;
      corresponding_tree->info.expr.paren_type = 1;
    }

  tree = parser_walk_tree (parser, tree_with_names,
			   ((type) ? pt_lambda_check_reduce_eq : NULL),
			   &lambda_arg, pt_lambda_node, &lambda_arg);

  if (corresponding_tree && corresponding_tree->node_type == PT_EXPR)
    {
      corresponding_tree->info.expr.paren_type = save_paren_type;
    }

  return tree;
}

/*
 * parser_copy_tree () - copies a parse tree without and modifying it
 *   return:
 *   parser(in):
 *   in_tree(in):
 */
PT_NODE *
parser_copy_tree (PARSER_CONTEXT * parser, const PT_NODE * tree)
{
  PT_NODE *copy = NULL;

  if (tree)
    {
      PT_NODE *temp, *save;
      temp = (PT_NODE *) tree;
      save = temp->next;
      temp->next = NULL;
      copy =
	parser_walk_tree (parser, temp, copy_node_in_tree_pre, NULL, NULL,
			  NULL);
      temp->next = save;
    }
  return copy;
}


/*
 * parser_copy_tree_list () - copies a parse tree without and modifing it.
 * 		  It includes the rest of the list pointed to by tree
 *   return:
 *   parser(in):
 *   tree(in):
 */

PT_NODE *
parser_copy_tree_list (PARSER_CONTEXT * parser, PT_NODE * tree)
{
  if (tree)
    {
      tree = parser_walk_tree (parser, tree, copy_node_in_tree_pre,
			       NULL, NULL, NULL);
    }

  return tree;
}

/*
 * pt_point () - points a parse tree node without and modifing it
 *   return:
 *   parser(in):
 *   in_tree(in):
 */
PT_NODE *
pt_point (PARSER_CONTEXT * parser, const PT_NODE * in_tree)
{
  PT_NODE *tree, *pointer;

  if (!in_tree)
    {
      return NULL;
    }

  /* unconst */
  tree = (PT_NODE *) in_tree;

  CAST_POINTER_TO_NODE (tree);

  pointer = parser_new_node (parser, PT_POINTER);
  if (!pointer)
    {
      return NULL;
    }

  /* set original node pointer */
  pointer->info.pointer.node = tree;

  /* set line/column number as that of original node pointer;
   * this is used at error print routine */
  pointer->line_number = tree->line_number;
  pointer->column_number = tree->column_number;

  return pointer;
}


/*
 * pt_point_l () - points a parse tree node without and modifing it.
 * 		   It includes the rest of the list pointed to by tree
 *   return:
 *   parser(in):
 *   in_tree(in):
 */

PT_NODE *
pt_point_l (PARSER_CONTEXT * parser, const PT_NODE * in_tree)
{
  PT_NODE *tree, *node, *pointer, *list;

  if (!in_tree)
    {
      return NULL;
    }

  /* unconst */
  tree = (PT_NODE *) in_tree;

  list = NULL;
  for (node = tree; node; node = node->next)
    {
      pointer = pt_point (parser, node);
      if (!pointer)
	{
	  goto exit_on_error;
	}
      list = parser_append_node (pointer, list);
    }

  return list;

exit_on_error:

  while (list)
    {
      node = list;
      list = list->next;

      node->next = NULL;	/* cut-off link */
      parser_free_tree (parser, node);
    }

  return NULL;
}

/*
 * pt_pointer_stack_push () - push a new PT_POINTER, pointing to node, on a
 *                            stack of similar pointers
 *   returns: stack base
 *   parser(in): parser context
 *   stack(in): base of stack or NULL for new stack
 *   node(in): node to be pointed to by new stack entry
 */
PT_NODE *
pt_pointer_stack_push (PARSER_CONTEXT * parser, PT_NODE * stack,
		       PT_NODE * node)
{
  PT_NODE *new_top = pt_point (parser, node);
  PT_NODE *list = stack;

  if (new_top == NULL)
    {
      PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC,
		 MSGCAT_SEMANTIC_OUT_OF_MEMORY);
      return stack;
    }

  while (list != NULL && list->next != NULL)
    {
      list = list->next;
    }

  if (list)
    {
      list->next = new_top;
      return stack;
    }
  else
    {
      return new_top;
    }
}

/*
 * pt_pointer_stack_pop () - push a new PT_POINTER, pointing to node, on a
 *                            stack of similar pointers
 *   returns: new stack base
 *   parser(in): parser context
 *   stack(in): base of stack
 *   node(out): popped node
 */
PT_NODE *
pt_pointer_stack_pop (PARSER_CONTEXT * parser, PT_NODE * stack,
		      PT_NODE ** node)
{
  PT_NODE *new_top = NULL;
  PT_NODE *list = stack;

  if (stack == NULL)
    {
      PT_INTERNAL_ERROR (parser, "pop operation on empty PT_POINTER stack");
      return NULL;
    }

  while (list != NULL && list->next != NULL)
    {
      new_top = list;
      list = list->next;
    }

  if (node != NULL)
    {
      *node = list->info.pointer.node;
    }
  list->info.pointer.node = NULL;
  parser_free_tree (parser, list);

  if (new_top)
    {
      new_top->next = NULL;
      return stack;
    }
  else
    {
      return NULL;
    }
}


/*
 * free_node_in_tree_pre () - checks a pointer nodes for a recursive walk
 *   return:
 *   parser(in):
 *   node(in/out):
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
free_node_in_tree_pre (UNUSED_ARG PARSER_CONTEXT * parser,
		       PT_NODE * node, UNUSED_ARG void *arg,
		       UNUSED_ARG int *continue_walk)
{
  if (node->node_type == PT_POINTER)
    {
      /* do must not free original node; cut-off link to original node */
      node->info.pointer.node = NULL;
    }
  return node;
}


/*
 * free_node_in_tree_post () - frees a node for a recursive walk
 *   return:
 *   parser(in):
 *   node(in):
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
free_node_in_tree_post (PARSER_CONTEXT * parser, PT_NODE * node,
			UNUSED_ARG void *arg, UNUSED_ARG int *continue_walk)
{
  parser_free_node (parser, node);
  return NULL;
}


/*
 * parser_free_tree () -
 *   return:
 *   parser(in):
 *   tree(in):
 */
void
parser_free_tree (PARSER_CONTEXT * parser, PT_NODE * tree)
{
  (void) parser_walk_tree (parser, tree, free_node_in_tree_pre, NULL,
			   free_node_in_tree_post, NULL);
}


/*
 * pt_internal_error () - report an internal system error
 *   return:
 *   parser(in): the parser context
 *   file(in): source file name
 *   line(in): at which line in the source
 *   what(in): a note about the internal system error
 */

void *
pt_internal_error (PARSER_CONTEXT * parser, const char *file,
		   int line, const char *what)
{
  PT_NODE node;

  node.line_number = 0;
  node.column_number = 0;
  node.buffer_pos = -1;

  if (parser && !pt_has_error (parser))
    {
      pt_frob_error (parser, &node, "System error (%s) in %s (line: %d)",
		     what, file, line);
      parser->has_internal_error = 1;
    }

  return NULL;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * pt_void_internal_error () - wrapper for pt_internal_error
 *   return:
 *   parser(in):
 *   file(in):
 *   line(in):
 *   what(in):
 */
void
pt_void_internal_error (PARSER_CONTEXT * parser, const char *file,
			int line, const char *what)
{
  pt_internal_error (parser, file, line, what);
}

/*
 * fgetin() - get input from users file
 *   return: -1 on EOF
 *   p(in):
 */
static int
fgetin (PARSER_CONTEXT * p)
{
  int c;

  c = fgetc (p->file);

  if (c == EOF)
    {
      return -1;
    }
  else
    {
      return c;
    }
}
#endif

/*
 * buffgetin() - get input from users buffer
 *   return: -1 on end
 *   p(in):
 */
static int
buffgetin (PARSER_CONTEXT * p)
{
  int c;

  c = *((const unsigned char *) p->buffer);

  if (!c)
    {
      c = -1;
    }
  else
    {
      (p->buffer)++;
    }

  return c;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * binarygetin() -
 *   return:
 *   p(in):
 */
static int
binarygetin (PARSER_CONTEXT * p)
{
  int c = -1;

  if (p->input_buffer_position >= p->input_buffer_length)
    {
      c = -1;
    }
  else
    {
      c = (const unsigned char) p->buffer[p->input_buffer_position++];
    }

  return c;
}
#endif

/*
 * pt_push() -  push a node onto this parser's stack
 *   return:  1 if all OK, 0 otherwise
 *   parser(in/out): the parser context
 *   node(in): a PT_NODE
 */
int
pt_push (PARSER_CONTEXT * parser, PT_NODE * node)
{
#define INITIAL_EXTENT 512
#define DELTA          512

  int new_siz, old_siz;
  PT_NODE **new_stk;

  if (!parser || !node)
    {
      return 0;
    }

  /* make sure there is space in the node_stack */
  if (parser->stack_top + 1 >= parser->stack_size)
    {
      /* expand the node_stack */
      old_siz = parser->stack_size;
      new_siz = (old_siz <= 0) ? INITIAL_EXTENT : old_siz + DELTA;
      new_stk =
	(PT_NODE **) parser_alloc (parser, new_siz * sizeof (PT_NODE *));
      if (!new_stk)
	{
	  return 0;
	}
      parser->stack_size = new_siz;

      /* copy contents of old node_stack to the new node_stack */
      if (parser->node_stack)
	{
	  memmove (new_stk, parser->node_stack,
		   parser->stack_top * sizeof (PT_NODE *));
	}

      /* the old node_stack will be freed later by parser_free_parser */
      parser->node_stack = new_stk;
    }

  /* push new node onto the stack */
  parser->node_stack[parser->stack_top++] = node;
  return 1;
}

/*
 * pt_pop() -  pop and return node from top of stack
 *   return:  the top node on the stack or NULL
 *   parser(in): the parser context
 */
PT_NODE *
pt_pop (PARSER_CONTEXT * parser)
{
  if (!parser || !parser->node_stack)
    {
      return NULL;
    }

  /* guard against stack underflow */
  if (parser->stack_top <= 0)
    {
      return NULL;
    }

  return parser->node_stack[--parser->stack_top];
}

/*
 * pt_top() -  return top of stack
 *   return:  the top node on the stack or NULL
 *   parser(in): the parser context
 */
PT_NODE *
pt_top (PARSER_CONTEXT * parser)
{
  if (!parser || !parser->node_stack)
    {
      return NULL;
    }

  /* guard against stack underflow */
  if (parser->stack_top <= 0)
    {
      return NULL;
    }

  return parser->node_stack[parser->stack_top - 1];
}

/*
 * parser_parse_string_use_sys_charset () - Parses a string and generates
 *					    parse tree. String constants will
 *					    use system charset if no charset
 *					    is specified.
 *
 * return      : Parse tree.
 * parser (in) : Parser context.
 * buffer (in) : Query string.
 *
 * NOTE: This function should be used instead of parser_parse_string () if the
 *	 query string may contain string constants.
 */
PT_NODE *
parser_parse_string_use_sys_charset (PARSER_CONTEXT * parser,
				     const char *buffer)
{
  PT_NODE *result = NULL;

  lang_set_parser_use_client_charset (false);
  result = parser_parse_string (parser, buffer);
  lang_set_parser_use_client_charset (true);

  return result;
}

/*
 * parser_parse_string() - reset and initialize the parser
 *   return:
 *   parser(in/out): the parser context
 *   buffer(in):
 */
PT_NODE *
parser_parse_string (PARSER_CONTEXT * parser, const char *buffer)
{
  PT_NODE *tree;

  if (!parser)
    {
      return 0;
    }
  parser->buffer = buffer;
  parser->original_buffer = buffer;

  parser->next_byte = buffgetin;
  parser->next_char = dbcs_get_next;
  parser->casecmp = intl_identifier_casecmp;

  if (prm_get_bool_value (PRM_ID_QUERY_TRACE) == true)
    {
      parser->query_trace = true;
    }
  else
    {
      parser->query_trace = false;
    }
#if defined (ENABLE_UNUSED_FUNCTION)
  parser->num_plan_trace = 0;
#endif

  /* reset parser node stack and line/column info */
  parser->stack_top = 0;
  parser->line = 1;
  parser->column = 0;

  /* set up an environment for longjump to return to if there is an out
   * of memory error in pt_memory.c. DO NOT RETURN unless PT_CLEAR_JMP_ENV
   * is called to clear the environment.
   */
  PT_SET_JMP_ENV (parser);

  tree = parser_main (parser);

  PT_CLEAR_JMP_ENV (parser);

  return tree;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * pt_init_one_statement_parser() -
 *   return:
 *   parser(in/out):
 *   file(in):
 */
PT_NODE **
pt_init_one_statement_parser (PARSER_CONTEXT * parser, FILE * file)
{
  if (!parser)
    {
      return 0;
    }
  parser->file = file;
  parser->next_byte = fgetin;
  parser->next_char = dbcs_get_next;
  parser->casecmp = intl_identifier_casecmp;

  /* reset parser node stack and line/column info */
  parser->stack_top = 0;
  parser->line = 1;
  parser->column = 0;

  {
    parser_output_host_index = parser_input_host_index = 0;
    this_parser = parser;
    dbcs_start_input ();
  }

  return 0;
}
#endif

/*
 * pt_record_error() - creates a new PT_ZZ_ERROR_MSG node appends it
 *                     to parser->error_msgs
 *   return: none
 *   parser(in/out): pointer to parser structure
 *   stmt_no(in): source statement where error was detected
 *   line_no(in): source line number where error was detected
 *   col_no(in): source column number where error was detected
 *   msg(in): a helpful explanation of the error
 */
void
pt_record_error (PARSER_CONTEXT * parser, int line_no,
		 int col_no, const char *msg, const char *context)
{
  char *context_copy;
  char buf[MAX_PRINT_ERROR_CONTEXT_LENGTH + 1];
  PT_NODE *node;

  node = parser_new_node (parser, PT_ZZ_ERROR_MSG);
  if (node == NULL)
    {
      PT_INTERNAL_ERROR (parser, "allocate new node");
      return;
    }

  node->line_number = line_no;
  node->column_number = col_no;
  node->info.error_msg.error_message = NULL;
  if (context != NULL)
    {
      char *before_context_str = msgcat_message (MSGCAT_CATALOG_RYE,
						 MSGCAT_SET_PARSER_SYNTAX,
						 MSGCAT_SYNTAX_BEFORE_CONTEXT);
      char *before_end_of_stmt_str =
	msgcat_message (MSGCAT_CATALOG_RYE, MSGCAT_SET_PARSER_SYNTAX,
			MSGCAT_SYNTAX_BEFORE_END_OF_STMT);
      /* size of constant string "before ' '\n" to be printed along with the
         actual context - do not count format parameter "%1$s", of size 4 */
      int before_context_len = strlen (before_context_str) - 4;
      int context_len = strlen (context);
      int end_of_statement = 0;
      int str_len = 0;
      char *s = NULL;

      if (context_len > MAX_PRINT_ERROR_CONTEXT_LENGTH)
	{
	  context_len = MAX_PRINT_ERROR_CONTEXT_LENGTH;
	  memset (buf, 0, MAX_PRINT_ERROR_CONTEXT_LENGTH + 1);
	  memcpy (buf, context, MAX_PRINT_ERROR_CONTEXT_LENGTH - 3);
	  strcpy (buf + MAX_PRINT_ERROR_CONTEXT_LENGTH - 3, "...");
	  context_copy = buf;
	}
      else
	{
	  context_copy = (char *) context;
	}

      if ((context_len == 0) || ((context_len == 1) && (*context_copy <= 32)))
	{
	  end_of_statement = 1;
	  /* size of constant string "before END OF STATEMENT\n" */
	  str_len = strlen (before_end_of_stmt_str);
	}
      else
	{
	  str_len = context_len + before_context_len;
	}

      /* parser_allocate_string_buffer() returns the start pointer of
       * the string buffer. It is guaranteed that the length of
       * the buffer 's' is equal to 'str_len + 1'.
       */
      s = parser_allocate_string_buffer (parser, str_len, sizeof (char));
      if (s == NULL)
	{
	  PT_INTERNAL_ERROR (parser, "insufficient memory");
	  return;
	}

      if (end_of_statement == 0)
	{
	  /* snprintf will assign the NULL-terminator('\0') to s[str_len]. */
	  snprintf (s, str_len + 1, before_context_str, context_copy);
	  if (s[str_len - 3] == '\n')
	    {
	      s[str_len - 3] = ' ';
	    }
	}
      else
	{
	  strcpy (s, before_end_of_stmt_str);
	}
      node->info.error_msg.error_message = s;
    }
  node->info.error_msg.error_message =
    pt_append_string (parser, node->info.error_msg.error_message, msg);

  if (pt_has_error (parser))
    {
      parser_append_node (node, parser->error_msgs);
    }
  else
    {
      parser->error_msgs = node;
    }
}

/*
 * pt_frob_warning() - creates a new PT_ZZ_ERROR_MSG node appends it
 *                     to parser->error_msgs
 *   return: none
 *   parser(in/out): pointer to parser structure
 *   stmt(in): pointer to a PT_NODE with interesting line and column info in it
 *   fmt(in): printf-style format string
 *
 * Note :
 *   helper function for PT_WARNING macro
 */

void
pt_frob_warning (PARSER_CONTEXT * parser,
		 const PT_NODE * stmt, const char *fmt, ...)
{
  va_list ap;
  char *old_buf = parser->error_buffer;

  va_start (ap, fmt);
  vasprintf (&parser->error_buffer, fmt, ap);
  va_end (ap);

  if (old_buf && parser->error_buffer != old_buf)
    {
      free (old_buf);
    }
  pt_record_warning (parser, SAFENUM (stmt, line_number),
		     SAFENUM (stmt, column_number), parser->error_buffer);
}

/*
 * pt_frob_error() - creates a new PT_ZZ_ERROR_MSG node appends it
 *                   to parser->error_msgs
 *   return: none
 *   parser(in/out): pointer to parser structure
 *   stmt(in): pointer to a PT_NODE with interesting line and column info in it
 *   fmt(in): printf-style format string
 *
 * Note :
 *   helper function for PT_ERROR macro
 */

void
pt_frob_error (PARSER_CONTEXT * parser,
	       const PT_NODE * stmt, const char *fmt, ...)
{
  va_list ap;
  const char *context = NULL;
  char *old_buf = parser->error_buffer;

  va_start (ap, fmt);
  vasprintf (&parser->error_buffer, fmt, ap);
  va_end (ap);

  if (old_buf && parser->error_buffer != old_buf)
    {
      free (old_buf);
    }

  if (parser->original_buffer != NULL
      && stmt != NULL && stmt->buffer_pos != -1)
    {
      if (strlen (parser->original_buffer) <= stmt->buffer_pos)
	{
	  /* node probably copied from another parser context */
	  context = NULL;
	}
      else
	{
	  context = parser->original_buffer + stmt->buffer_pos;
	}
    }

  pt_record_error (parser,
		   SAFENUM (stmt, line_number),
		   SAFENUM (stmt, column_number), parser->error_buffer,
		   context);
}

/*
 * pt_get_errors() -  returns PT_NODE list or NULL
 *   return:  PT_NODE list if any or NULL
 *   parser(in): parser context used in query compilation
 */
PT_NODE *
pt_get_errors (PARSER_CONTEXT * parser)
{
  if (parser == NULL)
    {
      return NULL;
    }

  return parser->error_msgs;
}

/*
 * pt_get_next_error() -  yield next query compilation error
 *   return:  PT_NODE pointer if there are more errors,
 *          NULL otherwise.
 *   errors(in): iterator of query compilation errors
 *   stmt_no(out): source statement where error was detected
 *   line_no(out): source line number where error was detected
 *   col_no(out): source column number where error was detected
 *   e_msg(out): an explanation of the error
 */
PT_NODE *
pt_get_next_error (PT_NODE * errors,
		   int *line_no, int *col_no, const char **msg)
{
  if (!errors || errors->node_type != PT_ZZ_ERROR_MSG)
    {
      return NULL;
    }
  else
    {
      *line_no = errors->line_number;
      *col_no = errors->column_number;
      *msg = errors->info.error_msg.error_message;

      return errors->next;
    }
}

/*
 * parser_new_node() -
 *   return:
 *   parser(in):
 *   n(in):
 */
PT_NODE *
parser_new_node (PARSER_CONTEXT * parser, PT_NODE_TYPE node_type)
{
  PT_NODE *node;

  node = parser_create_node (parser);
  if (node)
    {
      node->node_type = node_type;
      pt_parser_line_col (node);
      parser_init_node (node);

      node->sql_user_text = g_query_string;
      node->sql_user_text_len = g_query_string_len;
    }

  return node;
}

/*
 * parser_init_node() - initialize a node
 *   return:
 *   node(in/out):
 */
PT_NODE *
parser_init_node (PT_NODE * node)
{
  if (node)
    {
      PARSER_INIT_NODE_FUNC f;

      assert (node->node_type < PT_LAST_NODE_NUMBER);

      /* don't write over node_type, parser_id, line or column */
      node->next = NULL;
      node->or_next = NULL;
      node->etc = NULL;
      node->spec_ident = 0;
      node->type_enum = PT_TYPE_NONE;
      node->data_type = NULL;
      node->xasl_id = NULL;
      node->alias_print = NULL;
      node->recompile = 0;
      node->si_datetime = 0;
      node->use_plan_cache = 0;
      node->is_hidden_column = 0;
      node->is_paren = 0;
      node->with_rollup = 0;
      node->do_not_fold = 0;
      node->is_cnf_start = 0;
      node->buffer_pos = -1;
      node->do_not_replace_orderby = 0;
      node->is_server_query_ended = 0;
      node->is_null_node = 0;

      /* initialize  node info field */
      memset (&(node->info), 0, sizeof (node->info));

      f = pt_init_f[node->node_type];
      node = f (node);
    }

  return node;
}

/*
 * pt_print_bytes_alias() -
 *   return:
 *   parser(in):
 *   node(in):
 */
PARSER_VARCHAR *
pt_print_bytes_alias (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  if (!node)
    {
      return NULL;
    }

  if (node->alias_print)
    {
      return pt_append_name (parser, NULL, node->alias_print);
    }
  else
    {
      return pt_print_bytes (parser, node);
    }
}

/*
 * pt_print_bytes() -
 *   return:
 *   parser(in):
 *   node(in):
 */
PARSER_VARCHAR *
pt_print_bytes (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  PT_NODE_TYPE t;
  PARSER_PRINT_NODE_FUNC f;
  PARSER_VARCHAR *result = NULL;

  if (node == NULL)
    {
      return NULL;
    }

  CAST_POINTER_TO_NODE (node);

  if (node->is_cnf_start && !parser->is_in_and_list)
    {
      return pt_print_and_list (parser, node);
    }

  t = node->node_type;

  if (t >= PT_LAST_NODE_NUMBER || !(f = pt_print_f[t]))
    {
      return NULL;
    }

  /* avoid recursion */
  if (parser->is_in_and_list)
    {
      parser->is_in_and_list = 0;
      result = f (parser, (PT_NODE *) node);
      parser->is_in_and_list = 1;
      return result;
    }
  else
    {
      return f (parser, (PT_NODE *) node);
    }
}


/*
 * pt_print_bytes_l() - PRINT equivalent text for lists
 *   return:
 *   parser(in):
 *   p(in):
 */
PARSER_VARCHAR *
pt_print_bytes_l (PARSER_CONTEXT * parser, const PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r, *prev;
  PT_STRING_BLOCK sb;

  sb.body = NULL;
  sb.length = 0;
  sb.size = 1024;

  if (!p)
    {
      return NULL;
    }

  prev = pt_print_bytes (parser, p);

  if (p->is_cnf_start)
    {
      return prev;
    }

  sb.body = malloc (sb.size);
  if (sb.body == NULL)
    {
      return NULL;
    }

  sb.body[0] = 0;
  if (prev)
    {
      strcat_with_realloc (&sb, (const char *) prev->bytes);
    }

  while (p->next)
    {				/* print in the original order ... */
      p = p->next;
      r = pt_print_bytes (parser, p);
      if (r)
	{
	  if (prev)
	    {
	      strcat_with_realloc (&sb, ", ");
	    }

	  strcat_with_realloc (&sb, (const char *) r->bytes);
	  prev = r;
	}
    }

  if (sb.length > 0)
    {
      q = pt_append_nulstring (parser, q, sb.body);
    }

  free (sb.body);

  return q;
}

/*
 * pt_print_bytes_spec_list() -
 *   return:
 *   parser(in):
 *   p(in):
 */
PARSER_VARCHAR *
pt_print_bytes_spec_list (PARSER_CONTEXT * parser, const PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r;

  if (!p)
    {
      return 0;
    }

  q = pt_print_bytes (parser, p);

  while (p->next)
    {				/* print in the original order ... */
      p = p->next;
      r = pt_print_bytes (parser, p);

      if (p->node_type == PT_SPEC)
	{
	  switch (p->info.spec.join_type)
	    {
	    case PT_JOIN_NONE:
	      q = pt_append_bytes (parser, q, ", ", 2);
	      break;
	    case PT_JOIN_CROSS:
	      /*case PT_JOIN_NATURAL: -- does not support */
	    case PT_JOIN_INNER:
	    case PT_JOIN_LEFT_OUTER:
	    case PT_JOIN_RIGHT_OUTER:
	    case PT_JOIN_FULL_OUTER:
	      break;
	      /*case PT_JOIN_UNION: -- does not support */
	    default:
	      q = pt_append_bytes (parser, q, ", ", 2);
	      break;
	    }
	}
      else
	{
	  q = pt_append_bytes (parser, q, ", ", 2);
	}

      q = pt_append_varchar (parser, q, r);
    }
  return q;
}

/*
 * pt_print_node_value () -
 *   return: const sql string customized
 *   parser(in):
 *   val(in):
 */
PARSER_VARCHAR *
pt_print_node_value (PARSER_CONTEXT * parser, const PT_NODE * val)
{
  PARSER_VARCHAR *q = NULL;
  DB_VALUE *db_val;

  if (!(val->node_type == PT_VALUE || val->node_type == PT_HOST_VAR))
    {
      return NULL;
    }

  db_val = pt_value_to_db (parser, (PT_NODE *) val);
  if (db_val == NULL)
    {
      return NULL;
    }

  q = pt_print_db_value (parser, db_val);

  return q;
}

/*
 * pt_print_db_value () -
 *   return: const sql string customized
 *   parser(in):
 *   val(in):
 */
PARSER_VARCHAR *
pt_print_db_value (PARSER_CONTEXT * parser, const struct db_value * val)
{
  PARSER_VARCHAR *temp = NULL, *result = NULL, *elem;
  int i, size = 0;
  DB_VALUE element;
  int error = NO_ERROR;
  PT_NODE foo;
  unsigned int save_custom = parser->custom_print;

  if (val == NULL)
    {
      return NULL;
    }

  memset (&foo, 0, sizeof (foo));

  /* set custom_print here so describe_data() will know to pad bit
   * strings to full bytes */
  parser->custom_print = parser->custom_print | PT_PAD_BYTE;

  switch (DB_VALUE_TYPE (val))
    {
    case DB_TYPE_SEQUENCE:
      temp = pt_append_nulstring (parser, temp, "{");

      size = db_set_size (db_get_set ((DB_VALUE *) val));
      if (size > 0)
	{
	  error = db_set_get (db_get_set ((DB_VALUE *) val), 0, &element);
	  elem = describe_value (parser, NULL, &element);
	  temp = pt_append_varchar (parser, temp, elem);
	  for (i = 1; i < size; i++)
	    {
	      error = db_set_get (db_get_set ((DB_VALUE *) val), i, &element);
	      temp = pt_append_nulstring (parser, temp, ", ");
	      elem = describe_value (parser, NULL, &element);
	      temp = pt_append_varchar (parser, temp, elem);
	    }
	}
      temp = pt_append_nulstring (parser, temp, "}");
      result = temp;
      break;

    case DB_TYPE_OBJECT:
      /* no printable representation!, should not get here */
      result = pt_append_nulstring (parser, NULL, "NULL");
      break;

    case DB_TYPE_VARBIT:
      /* rsql & everyone else get X'some_hex_string' */
      result = describe_value (parser, NULL, val);
      break;

    case DB_TYPE_DATE:
      /* rsql & everyone else want DATE'mm/dd/yyyy' */
      result = describe_value (parser, NULL, val);
      break;

    case DB_TYPE_TIME:
      /* rsql & everyone else get time 'hh:mi:ss' */
      result = describe_value (parser, NULL, val);
      break;

    case DB_TYPE_DATETIME:
      /* everyone else gets rsql's utime format */
      result = describe_value (parser, NULL, val);
      break;

    default:
      result = describe_value (parser, NULL, val);
      break;
    }
  /* restore custom print */
  parser->custom_print = save_custom;
  return result;
}

/*
 * pt_print_alias() -
 *   return:
 *   parser(in):
 *   node(in):
 */
char *
pt_print_alias (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  PARSER_VARCHAR *string;

  string = pt_print_bytes_alias (parser, node);
  if (string)
    {
      return (char *) string->bytes;
    }
  return NULL;
}

/*
 * parser_print_tree() -
 *   return:
 *   parser(in):
 *   node(in):
 */
char *
parser_print_tree (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  PARSER_VARCHAR *string;

  string = pt_print_bytes (parser, node);
  if (string)
    {
#if 0
      if ((parser->custom_print & PT_PRINT_DIFFERENT_SYSTEM_PARAMETERS) != 0)
	{
	  char *str = sysprm_print_parameters_for_qry_string ();
	  string = pt_append_nulstring (parser, string, "?");
	  string = pt_append_nulstring (parser, string, str);
	  free_and_init (str);
	}
#endif
      return (char *) string->bytes;
    }
  return NULL;
}

/*
 * parser_print_tree_with_quotes() -
 *   return:
 *   parser(in):
 *   node(in):
 */
char *
parser_print_tree_with_quotes (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  PARSER_VARCHAR *string;
  unsigned int save_custom;

  save_custom = parser->custom_print;
  parser->custom_print |= PT_PRINT_QUOTES;

  string = pt_print_bytes (parser, node);
  parser->custom_print = save_custom;

  if (string)
    {
      return (char *) string->bytes;
    }

  return NULL;
}

/*
 * parser_print_tree_list() -
 *   return:
 *   parser(in):
 *   node(in):
 */
char *
parser_print_tree_list (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  PARSER_VARCHAR *string;

  string = pt_print_bytes_l (parser, node);
  if (string)
    {
      return (char *) string->bytes;
    }
  return NULL;
}


/*
 * pt_print_and_list() - PRINT equivalent text for CNF predicate lists
 *   return:
 *   parser(in):
 *   p(in):
 */
PARSER_VARCHAR *
pt_print_and_list (PARSER_CONTEXT * parser, const PT_NODE * p)
{
  PARSER_VARCHAR *q = NULL, *r1;
  const PT_NODE *n;

  if (!p)
    {
      return NULL;
    }

  parser->is_in_and_list = 1;

  for (n = p; n; n = n->next)
    {				/* print in the original order ... */
      r1 = pt_print_bytes (parser, n);
      if (n->node_type == PT_EXPR && !n->info.expr.paren_type && n->or_next)
	{
	  /* found non-parenthesis OR */
	  q = pt_append_nulstring (parser, q, "(");
	  q = pt_append_varchar (parser, q, r1);
	  q = pt_append_nulstring (parser, q, ")");
	}
      else
	{
	  q = pt_append_varchar (parser, q, r1);
	}

      if (n->next)
	{
	  q = pt_append_nulstring (parser, q, " and ");
	}
    }

  parser->is_in_and_list = 0;

  return q;
}


/*
 * pt_print_query_spec_no_list() - prints query specifications
 *                                 with NA placeholders
 *   return:
 *   parser(in):
 *   node(in):
 */
char *
pt_print_query_spec_no_list (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  unsigned int save_custom = parser->custom_print;
  char *result;

  parser->custom_print |= PT_SUPPRESS_SELECT_LIST | PT_SUPPRESS_INTO
    | PT_CHARSET_COLLATE_FULL;
  result = parser_print_tree_with_quotes (parser, node);
  parser->custom_print = save_custom;

  return result;
}

/*
 * pt_short_print() - Short print (for error messages)
 *   return:
 *   parser(in):
 *   node(in):
 */
char *
pt_short_print (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  char *str;
  str = parser_print_tree (parser, node);
  if (str == NULL)
    {
      return NULL;
    }

  if (strlen (str) > 64)
    {
      strcpy (str + 60, "...");
    }

  return str;
}


/*
 * pt_short_print_l() -
 *   return:
 *   parser(in):
 *   node(in):
 */
char *
pt_short_print_l (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  char *str;
  str = parser_print_tree_list (parser, node);
  if (str == NULL)
    {
      return NULL;
    }

  if (strlen (str) > 64)
    {
      strcpy (str + 60, "...");
    }

  return str;
}

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * pt_show_node_type() -
 *   return: English name of the node type (for debugging)
 *   node(in):
 */
const char *
pt_show_node_type (PT_NODE * node)
{
  if (!node)
    {
      return "null_pointer";
    }
  switch (node->node_type)
    {
    case PT_ALTER:
      return "ALTER";
    case PT_ALTER_INDEX:
      return "ALTER_INDEX";
    case PT_ALTER_USER:
      return "ALTER_USER";
    case PT_ATTR_DEF:
      return "ATTR_DEF";
    case PT_ATTR_ORDERING:
      return "ATTR_ORDERING";
    case PT_AUTH_CMD:
      return "AUTH CMD";
    case PT_COMMIT_WORK:
      return "COMMIT_WORK";
    case PT_CREATE_ENTITY:
      return "CREATE_ENTITY";
    case PT_CREATE_INDEX:
      return "CREATE_INDEX";
    case PT_CREATE_USER:
      return "CREATE_USER";
    case PT_DATA_DEFAULT:
      return "DATA_DEFAULT";
    case PT_DATA_TYPE:
      return "DATA_TYPE";
    case PT_DELETE:
      return "DELETE";
    case PT_DIFFERENCE:
      return "DIFFERENCE";
    case PT_DOT_:
      return "DOT_";
    case PT_DROP:
      return "DROP";
    case PT_DROP_INDEX:
      return "DROP_INDEX";
    case PT_DROP_USER:
      return "DROP_USER";
    case PT_EXPR:
      return "EXPR";
    case PT_FUNCTION:
      return "FUNCTION";
    case PT_GET_OPT_LVL:
      return "GET OPT LEVEL";
    case PT_GET_XACTION:
      return "GET TRANSACTION";
    case PT_GRANT:
      return "GRANT";
    case PT_HOST_VAR:
      return "HOST_VAR";
    case PT_INSERT:
      return "INSERT";
    case PT_INTERSECTION:
      return "INTERSECTION";
    case PT_ISOLATION_LVL:
      return "ISOLATION LEVEL";
    case PT_NAME:
      return "NAME";
    case PT_RENAME:
      return "RENAME";
    case PT_REVOKE:
      return "REVOKE";
    case PT_ROLLBACK_WORK:
      return "ROLLBACK_WORK";
    case PT_SAVEPOINT:
      return "SAVEPOINT";
    case PT_SELECT:
      return "SELECT";
    case PT_SET_OPT_LVL:
      return "SET OPT LVL";
    case PT_SET_SYS_PARAMS:
      return "SET SYSTEM PARAMETERS";
    case PT_SORT_SPEC:
      return "SORT_SPEC";
    case PT_SPEC:
      return "SPEC";
    case PT_TIMEOUT:
      return "TIMEOUT";
    case PT_UNION:
      return "UNION";
    case PT_UPDATE:
      return "UPDATE";
    case PT_UPDATE_STATS:
      return "UPDATE_STATS";
    case PT_VALUE:
      return "VALUE";
    case PT_CONSTRAINT:
      return "CONSTRAINT";
    case PT_NODE_LIST:
      return "NODE_LIST";
    default:
      return "NODE: type unknown";
    }
}
#endif

/*
 * parser_append_node() -
 *   return:
 *   node(in/out):
 *   list(in):
 */
PT_NODE *
parser_append_node (PT_NODE * node, PT_NODE * list)
{
  if (list)
    {
      PT_NODE *tail;
      tail = list;
      while (tail->next)
	{
	  tail = tail->next;
	}
      tail->next = node;
    }
  else
    {
      list = node;
    }
  return list;
}


/*
 * parser_append_node_or() -
 *   return:
 *   node(in/out):
 *   list(in):
 */
PT_NODE *
parser_append_node_or (PT_NODE * node, PT_NODE * list)
{
  if (list)
    {
      PT_NODE *tail;
      tail = list;
      while (tail->or_next)
	{
	  tail = tail->or_next;
	}
      tail->or_next = node;
    }
  else
    {
      list = node;
    }
  return list;
}


/*
 * pt_length_of_list() -
 *   return:
 *   list(in):
 */
int
pt_length_of_list (const PT_NODE * list)
{
  int len;
  for (len = 0; list; len++)
    {
      list = list->next;
    }
  return len;
}


/*
 * pt_length_of_select_list() -
 *   return:
 *   list(in):
 *   hidden_col(in):
 */
int
pt_length_of_select_list (PT_NODE * list, int hidden_col)
{
  int len;

  if (hidden_col == INCLUDE_HIDDEN_COLUMNS)
    {
      for (len = 0; list; len++)
	{
	  list = list->next;
	}
      return len;
    }
  else
    {				/* EXCLUDE_HIDDEN_COLUMNS */
      for (len = 0; list; list = list->next)
	{
	  if (list->is_hidden_column)
	    {
	      /* skip hidden column */
	      continue;
	    }
	  len++;
	}
      return len;
    }
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * pt_get_node_from_list - get node from list, based on index
 *  return: the node at position specified by index or NULL
 *  list(in): node list
 *  index(in): index of requested node
 */
PT_NODE *
pt_get_node_from_list (PT_NODE * list, int index)
{
  if (list == NULL || index < 0)
    {
      return NULL;
    }

  while (list && index > 0)
    {
      list = list->next;
      index--;
    }

  return list;
}
#endif

/*
 * pt_show_misc_type() - English name of the node type (for debugging)
 *   return:
 *   c(in):
 */
const char *
pt_show_misc_type (PT_MISC_TYPE p)
{
  switch (p)
    {
    case PT_MISC_DUMMY:
      return "";
    case PT_INVALIDATE_XACTION:
      return "invalidate transaction";
    case PT_PRINT:
      return "print";
    case PT_ALL:
      return "all";
#if 0				/* unused */
    case PT_ONLY:
      return "only";
#endif
    case PT_DISTINCT:
      return "distinct";
    case PT_DEFAULT:
      return "default";
    case PT_ASC:
      return "asc";
    case PT_DESC:
      return "desc";
    case PT_GRANT_OPTION:
      return "with grant option";
    case PT_NO_GRANT_OPTION:
      return "with no grant option";
    case PT_CLASS:
      return "table";
    case PT_VCLASS:
      return "view";
    case PT_NORMAL:
      return "";
    case PT_IS_SUBQUERY:
    case PT_IS_VALUE:
      return "";
    case PT_ATTRIBUTE:
      return "attribute";
    case PT_NO_ISOLATION_LEVEL:
      return "no isolation level";
    case PT_SERIALIZABLE:
      return "serializable";
    case PT_REPEATABLE_READ:
      return "repeatable read";
    case PT_READ_COMMITTED:
      return "read committed";
    case PT_READ_UNCOMMITTED:
      return "read uncommitted";
    case PT_ISOLATION_LEVEL:
      return "isolation level";
    case PT_LOCK_TIMEOUT:
      return "lock timeout";
    case PT_CHAR_STRING:
      return "";
    case PT_BIT_STRING:
      return "";
    case PT_HEX_STRING:
      return "";
    case PT_MATCH_REGULAR:
      return "";
    case PT_MATCH_PARTIAL:
      return "match partial";
    case PT_YEAR:
      return "year";
    case PT_MONTH:
      return "month";
    case PT_DAY:
      return "day";
    case PT_HOUR:
      return "hour";
    case PT_MINUTE:
      return "minute";
    case PT_SECOND:
      return "second";
    case PT_MILLISECOND:
      return "millisecond";
    case PT_SIMPLE_CASE:
      return "simple case";
    case PT_SEARCHED_CASE:
      return "searched case";
    case PT_OPT_LVL:
      return "level";
    case PT_OPT_COST:
      return "cost";
    case PT_NOPUT:
      return "";
    case PT_INPUT:
      return "";
    case PT_OUTPUT:
      return "out";
    case PT_INPUTOUTPUT:
      return "inout";
    case PT_TRACE_ON:
      return "on";
    case PT_TRACE_OFF:
      return "off";
    case PT_TRACE_FORMAT_TEXT:
      return "text";
    case PT_TRACE_FORMAT_JSON:
      return "json";
    default:
      return "MISC_TYPE: type unknown";
    }
}

/*
 * pt_show_binopcode() -
 *   return:
 *   n(in):
 */
const char *
pt_show_binopcode (PT_OP_TYPE n)
{
  switch (n)
    {
    case PT_FUNCTION_HOLDER:
      assert (false);
      return "";
    case PT_AND:
      return " and ";
    case PT_OR:
      return " or ";
    case PT_NOT:
      return " not ";
    case PT_XOR:
      return " xor ";
    case PT_BETWEEN:
      return " between ";
    case PT_NOT_BETWEEN:
      return " not between ";
    case PT_BETWEEN_AND:
      return " and ";
    case PT_BETWEEN_GE_LE:
      return " ge_le ";
    case PT_BETWEEN_GE_LT:
      return " ge_lt ";
    case PT_BETWEEN_GT_LE:
      return " gt_le ";
    case PT_BETWEEN_GT_LT:
      return " gt_lt ";
    case PT_BETWEEN_EQ_NA:
      return " = ";
    case PT_BETWEEN_INF_LE:
      return " inf_le ";
    case PT_BETWEEN_INF_LT:
      return " inf_lt ";
    case PT_BETWEEN_GE_INF:
      return " ge_inf ";
    case PT_BETWEEN_GT_INF:
      return " gt_inf ";
    case PT_RANGE:
      return " range ";
    case PT_LIKE:
      return " like ";
    case PT_NOT_LIKE:
      return " not like ";
    case PT_LIKE_ESCAPE:
      return " escape ";
    case PT_RLIKE:
      return " regexp ";
    case PT_NOT_RLIKE:
      return " not regexp ";
    case PT_RLIKE_BINARY:
      return " regexp binary ";
    case PT_NOT_RLIKE_BINARY:
      return " not regexp binary ";
    case PT_IS_IN:
      return " in ";
    case PT_IS:
      return " is ";
    case PT_IS_NOT:
      return " is not ";
    case PT_IS_NOT_IN:
      return " not in ";
    case PT_IS_NULL:
      return " is null ";
    case PT_IS_NOT_NULL:
      return " is not null ";
    case PT_EXISTS:
      return " exists ";
    case PT_EQ:
      return "=";
    case PT_NULLSAFE_EQ:
      return "<=>";
    case PT_NE:
      return "<>";
    case PT_GE:
      return ">=";
    case PT_GT:
      return ">";
    case PT_LT:
      return "<";
    case PT_LE:
      return "<=";
    case PT_BIT_NOT:
      return "~";
    case PT_BIT_AND:
      return "&";
    case PT_BIT_OR:
      return "|";
    case PT_BIT_XOR:
      return "^";
    case PT_BITSHIFT_LEFT:
      return "<<";
    case PT_BITSHIFT_RIGHT:
      return ">>";
    case PT_DIV:
      return " div ";
    case PT_MOD:
      return " mod ";
    case PT_BIT_COUNT:
      return "bit_count ";
    case PT_PLUS:
      return "+";
    case PT_MINUS:
      return "-";
    case PT_TIMES:
      return "*";
    case PT_DIVIDE:
      return "/";
    case PT_UNARY_MINUS:
      return "-";
    case PT_ASSIGN:
      return "=";
    case PT_MODULUS:
      return "mod ";
    case PT_RAND:
      return "rand ";
    case PT_DRAND:
      return "drand ";
    case PT_RANDOM:
      return "random ";
    case PT_DRANDOM:
      return "drandom ";
    case PT_FLOOR:
      return "floor ";
    case PT_CEIL:
      return "ceil ";
    case PT_SIGN:
      return "sign ";
    case PT_POWER:
      return "power ";
    case PT_ROUND:
      return "round ";
    case PT_LOG:
      return "log ";
    case PT_EXP:
      return "exp ";
    case PT_SQRT:
      return "sqrt ";
    case PT_TRUNC:
      return "trunc ";
    case PT_ABS:
      return "abs ";
    case PT_CHR:
      return "chr ";
    case PT_INSTR:
      return "instr ";
    case PT_LEAST:
      return "least ";
    case PT_GREATEST:
      return "greatest ";
    case PT_POSITION:
      return "position ";
    case PT_FINDINSET:
      return "find_in_set ";
    case PT_SUBSTRING:
      return "substring ";
    case PT_SUBSTRING_INDEX:
      return "substring_index ";
    case PT_OCTET_LENGTH:
      return "octet_length ";
    case PT_BIT_LENGTH:
      return "bit_length ";
    case PT_CHAR_LENGTH:
      return "char_length ";
    case PT_IF:
      return "if ";
    case PT_IFNULL:
      return "ifnull ";
    case PT_ISNULL:
      return "isnull ";
    case PT_DEGREES:
      return "degrees ";
    case PT_RADIANS:
      return "radians ";
    case PT_PI:
      return "pi ";
    case PT_ACOS:
      return "acos ";
    case PT_ASIN:
      return "asin ";
    case PT_ATAN:
      return "atan ";
    case PT_ATAN2:
      return "atan2 ";
    case PT_SIN:
      return "sin ";
    case PT_COS:
      return "cos ";
    case PT_COT:
      return "cot ";
    case PT_TAN:
      return "tan ";
    case PT_LN:
      return "ln ";
    case PT_LOG2:
      return "log2 ";
    case PT_LOG10:
      return "log10 ";
    case PT_FORMAT:
      return "format ";
    case PT_DATE_ADD:
      return "date_add ";
    case PT_ADDDATE:
      return "adddate ";
    case PT_DATE_SUB:
      return "date_sub ";
    case PT_SUBDATE:
      return "subdate ";
    case PT_DATE_FORMAT:
      return "date_format ";
    case PT_STR_TO_DATE:
      return "str_to_date ";
    case PT_TIME_FORMAT:
      return "time_format ";
    case PT_DATEF:
      return "date ";
    case PT_TIMEF:
      return "time ";
    case PT_DATEDIFF:
      return "datediff ";
    case PT_TIMEDIFF:
      return "timediff ";
    case PT_CONCAT:
      return "concat ";
    case PT_CONCAT_WS:
      return "concat_ws ";
    case PT_FIELD:
      return "field ";
    case PT_LEFT:
      return "left ";
    case PT_RIGHT:
      return "right ";
    case PT_LOCATE:
      return "locate ";
    case PT_MID:
      return "mid ";
    case PT_STRCMP:
      return "strcmp ";
    case PT_REVERSE:
      return "reverse ";
    case PT_LIKE_LOWER_BOUND:
      return "like_match_lower_bound ";
    case PT_LIKE_UPPER_BOUND:
      return "like_match_upper_bound ";
    case PT_LOWER:
      return "lower ";
    case PT_UPPER:
      return "upper ";
    case PT_HEX:
      return "hex ";
    case PT_ASCII:
      return "ascii ";
    case PT_CONV:
      return "conv ";
    case PT_MD5:
      return "md5 ";
    case PT_SHA_ONE:
      return "sha1 ";
    case PT_SHA_TWO:
      return "sha2 ";
    case PT_TO_BASE64:
      return "to_base64 ";
    case PT_FROM_BASE64:
      return "from_base64 ";
    case PT_BIN:
      return "bin ";
    case PT_TRIM:
      return "trim ";
    case PT_LTRIM:
      return "ltrim ";
    case PT_RTRIM:
      return "rtrim ";
    case PT_LPAD:
      return "lpad ";
    case PT_RPAD:
      return "rpad ";
    case PT_REPEAT:
      return "repeat ";
    case PT_SPACE:
      return "space ";
    case PT_REPLACE:
      return "replace ";
    case PT_TRANSLATE:
      return "translate ";
    case PT_LAST_DAY:
      return "last_day ";
    case PT_MONTHS_BETWEEN:
      return "months_between ";
    case PT_SYS_DATE:
      return "sys_date ";
    case PT_SYS_TIME:
      return "sys_time ";
    case PT_SYS_DATETIME:
      return "sys_datetime ";
    case PT_UTC_TIME:
      return "utc_time ";
    case PT_UTC_DATE:
      return "utc_date ";
    case PT_TO_CHAR:
      return "to_char ";
    case PT_TO_DATE:
      return "to_date ";
    case PT_TO_TIME:
      return "to_time ";
    case PT_YEARF:
      return "year ";
    case PT_MONTHF:
      return "month ";
    case PT_DAYF:
      return "day ";
    case PT_DAYOFMONTH:
      return "dayofmonth ";
    case PT_HOURF:
      return "hour ";
    case PT_MINUTEF:
      return "minute ";
    case PT_SECONDF:
      return "second ";
    case PT_UNIX_TIMESTAMP:
      return "unix_timestamp ";
    case PT_FROM_UNIXTIME:
      return "from_unixtime ";
    case PT_QUARTERF:
      return "quarter ";
    case PT_WEEKDAY:
      return "weekday ";
    case PT_DAYOFWEEK:
      return "dayofweek ";
    case PT_DAYOFYEAR:
      return "dayofyear ";
    case PT_TODAYS:
      return "to_days ";
    case PT_FROMDAYS:
      return "from_days ";
    case PT_TIMETOSEC:
      return "time_to_sec ";
    case PT_SECTOTIME:
      return "sec_to_time ";
    case PT_MAKEDATE:
      return "makedate ";
    case PT_MAKETIME:
      return "maketime ";
    case PT_WEEKF:
      return "week ";
    case PT_SCHEMA:
      return "schema ";
    case PT_DATABASE:
      return "database ";
    case PT_VERSION:
      return "version ";
    case PT_TO_DATETIME:
      return "to_datetime ";
    case PT_TO_NUMBER:
      return "to_number ";
    case PT_EXTRACT:
      return "extract ";
    case PT_CAST:
      return "cast ";
    case PT_CASE:
      return "case ";
    case PT_INST_NUM:
      return "inst_num ";
    case PT_ROWNUM:
      return "rownum ";
    case PT_ORDERBY_NUM:
      return "orderby_num";
    case PT_CURRENT_USER:
      return "current_user ";
    case PT_HA_STATUS:
      return "ha_status ";
    case PT_SHARD_GROUPID:
      return "shard_groupid ";
    case PT_SHARD_LOCKNAME:
      return "shard_lockname ";
    case PT_SHARD_NODEID:
      return "shard_nodeid ";
    case PT_EXPLAIN:
      return "explain ";
    case PT_STRCAT:
      return "||";
    case PT_NULLIF:
      return "nullif ";
    case PT_COALESCE:
      return "coalesce ";
    case PT_NVL:
      return "nvl ";
    case PT_NVL2:
      return "nvl2 ";
    case PT_DECODE:
      return "decode ";
    case PT_USER:
      return "user ";
    case PT_DEFAULTF:
      return "default ";
    case PT_LIST_DBS:
      return "list_dbs ";
    case PT_TYPEOF:
      return "typeof ";
    case PT_INDEX_CARDINALITY:
      return "index_cardinality ";
    case PT_INET_ATON:
      return "inet_aton ";
    case PT_INET_NTOA:
      return "inet_ntoa ";
    case PT_WIDTH_BUCKET:
      return "width_bucket";
    case PT_TRACE_STATS:
      return "trace_stats";
    case PT_INDEX_PREFIX:
      return "index_prefix ";
    default:
      return "unknown opcode";
    }
}


/*
 * pt_show_function() -
 *   return:
 *   c(in):
 */
const char *
pt_show_function (FUNC_TYPE c)
{
  switch (c)
    {
    case PT_MIN:
      return "min";
    case PT_MAX:
      return "max";
    case PT_SUM:
      return "sum";
    case PT_AVG:
      return "avg";
    case PT_STDDEV:
      return "stddev";
    case PT_STDDEV_POP:
      return "stddev_pop";
    case PT_STDDEV_SAMP:
      return "stddev_samp";
    case PT_VARIANCE:
      return "variance";
    case PT_VAR_POP:
      return "var_pop";
    case PT_VAR_SAMP:
      return "var_samp";
    case PT_COUNT:
      return "count";
    case PT_COUNT_STAR:
      return "count";
    case PT_GROUPBY_NUM:
      return "groupby_num";
    case PT_GROUP_CONCAT:
      return "group_concat";

    case F_SEQUENCE:
#if 0				/* TODO - */
      assert (false);		/* should not reach here */
#endif
      return "sequence";

    case F_INSERT_SUBSTRING:
      return "insert";
    case F_ELT:
      return "elt";
    default:
      return "unknown function";
    }
}

/*
 * pt_show_priv() -
 *   return:
 *   t(in):
 */
const char *
pt_show_priv (PT_PRIV_TYPE t)
{
  switch (t)
    {
    case PT_NO_PRIV:
      return "no";
    case PT_ADD_PRIV:
      return "add";
    case PT_ALL_PRIV:
      return "all";
    case PT_ALTER_PRIV:
      return "alter";
    case PT_DELETE_PRIV:
      return "delete";
    case PT_DROP_PRIV:
      return "drop";
    case PT_INSERT_PRIV:
      return "insert";
    case PT_REFERENCES_PRIV:
      return "references";
    case PT_SELECT_PRIV:
      return "select";
    case PT_UPDATE_PRIV:
      return "update";
    default:
      return "unknown privilege";
    }

}

/*
 * pt_show_type_enum() -
 *   return:
 *   t(in):
 */
const char *
pt_show_type_enum (PT_TYPE_ENUM t)
{
  if (t <= PT_TYPE_NONE || t >= PT_TYPE_MAX)
    {
      return "unknown data type";
    }

  switch (t)
    {
    case PT_TYPE_NONE:
      return "none";
      /* treat PT_TYPE__LOGICAL as PT_TYPE_INTEGER */
    case PT_TYPE_LOGICAL:
    case PT_TYPE_INTEGER:
      return "integer";
    case PT_TYPE_BIGINT:
      return "bigint";
    case PT_TYPE_NUMERIC:
      return "numeric";
    case PT_TYPE_DOUBLE:
      return "double";
    case PT_TYPE_DATE:
      return "date";
    case PT_TYPE_TIME:
      return "time";
    case PT_TYPE_DATETIME:
      return "datetime";
    case PT_TYPE_VARCHAR:
      return "varchar";
    case PT_TYPE_VARBIT:
      return "varbinary";

    case PT_TYPE_MAYBE:
      return "uncertain";

    case PT_TYPE_NULL:
      return "null";
    case PT_TYPE_STAR:
      return "*";

    case PT_TYPE_OBJECT:
      return "object";
    case PT_TYPE_SEQUENCE:
      return "sequence";
    case PT_TYPE_RESULTSET:
      return "cursor";
    case PT_TYPE_COMPOUND:
      return "unknown";

    case PT_TYPE_MAX:
    default:
      return "unknown";
    }
  return "unknown";		/* make the compiler happy */
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * pt_show_alter() -
 *   return:
 *   c(in):
 */
const char *
pt_show_alter (PT_ALTER_CODE c)
{
  switch (c)
    {
    case PT_ADD_ATTR_MTHD:
      return "ADD ATTR/MTHD";
    case PT_DROP_ATTR_MTHD:
      return "DROP ATTR/MTHD";
    case PT_RENAME_ATTR_MTHD:
      return "RENAME ATTR/MTHD";
    case PT_DROP_CONSTRAINT:
      return "DROP CONSTRAINT";
    case PT_RENAME_ENTITY:
      return "RENAME ENTITY";
    case PT_DROP_INDEX_CLAUSE:
      return "DROP INDEX";
    default:
      return "unknown alter code";
    }
}
#endif


/*
 * pt_gather_constraints() - Moves explicit and synthesized constraints from
 *          the attribute definition list out into the constraint_list member
 *          and into the create_index member
 *   return: pointer to modified node or NULL
 *   parser(in): pointer to parser structure
 *   node(in): pointer to CREATE_ENTITY or ALTER node to be modified
 */

PT_NODE *
pt_gather_constraints (PARSER_CONTEXT * parser, PT_NODE * node)
{
  PT_NODE **constraint_list_p = NULL;
  PT_NODE **attr_list_p = NULL;
  PT_NODE *next = NULL, *tmp = NULL;

  switch (node->node_type)
    {
    case PT_CREATE_ENTITY:
      constraint_list_p = &node->info.create_entity.constraint_list;
      attr_list_p = &node->info.create_entity.attr_def_list;
      break;

    case PT_ALTER:
      if (node->info.alter.code == PT_ADD_ATTR_MTHD ||
	  node->info.alter.code == PT_CHANGE_ATTR)
	{
	  constraint_list_p = &node->info.alter.constraint_list;
	  attr_list_p =
	    &node->info.alter.alter_clause.attr_mthd.attr_def_list;
	}
      break;

    default:
      PT_INTERNAL_ERROR (parser, "bad node type");
      goto error;
    }

  if (attr_list_p != NULL)
    {
      next = *attr_list_p;
      while (next)
	{
	  switch (next->node_type)
	    {
	    case PT_CONSTRAINT:
	    case PT_CREATE_INDEX:
	      /*
	       * We need to cut this entry out of the attr_def list and
	       * append it to the constraint list.  This uses the
	       * standard indirect update technique for modifying a
	       * singly-linked list.
	       */
	      tmp = next;
	      *attr_list_p = next = tmp->next;
	      tmp->next = NULL;
	      *constraint_list_p =
		parser_append_node (tmp, *constraint_list_p);
	      break;

	    default:
	      attr_list_p = &next->next;
	      next = next->next;
	      break;
	    }
	}
    }

  return node;

error:
  return NULL;
}

/*
 * pt_get_subquery_list() - simple implementation of pt_get_select_list
 *   return:
 *   node(in):
 */
PT_NODE *
pt_get_subquery_list (PT_NODE * node)
{
  PT_NODE *col;

  switch (node->node_type)
    {
    case PT_SELECT:
      node = node->info.query.q.select.list;

      if (node && node->node_type == PT_VALUE
	  && node->type_enum == PT_TYPE_STAR)
	{
	  /* found "*" */
	  node = NULL;
	}

      for (col = node; col; col = col->next)
	{
	  if (col->node_type == PT_NAME && col->type_enum == PT_TYPE_STAR)
	    {
	      /* found "classname.*" */
	      node = NULL;
	      break;
	    }
	}

      break;

    case PT_UNION:
    case PT_INTERSECTION:
    case PT_DIFFERENCE:
      node = pt_get_subquery_list (node->info.query.q.union_.arg1);
      break;

    default:
      node = NULL;
      break;
    }

  return node;
}

/*
 * pt_get_expression_count() -
 *   return:
 *   node(in):
 */
int
pt_get_expression_count (PT_NODE * node)
{
  int count;
  PT_NODE *list;

  count = -1;
  if (node)
    {
      if (node->node_type == PT_VALUE
	  && PT_IS_COLLECTION_TYPE (node->type_enum))
	{
	  count = pt_length_of_list (node->info.value.data_value.set);
	}
      else if (PT_IS_QUERY_NODE_TYPE (node->node_type))
	{
	  list = pt_get_subquery_list (node);
	  if (!list)
	    {
	      /* in case of error or found "*" */
	      return count;
	    }
	  else if (list->next == NULL)
	    {			/* single column */
	      if (PT_IS_COLLECTION_TYPE (list->type_enum))
		{
		  if (list->node_type == PT_VALUE)
		    {
		      count =
			pt_length_of_list (list->info.value.data_value.set);
		    }
		  else if (list->node_type == PT_FUNCTION)
		    {
		      count =
			pt_length_of_list (list->info.function.arg_list);
		    }
		}
	      else
		{
		  count = 1;
		}
	    }
	  else
	    {
	      count = pt_length_of_select_list (list, EXCLUDE_HIDDEN_COLUMNS);
	    }
	}
      else
	{
	  count = 0;
	  for (list = node; list; list = list->next)
	    {
	      count++;
	    }
	}
    }
  return count;
}


/*
 * pt_select_list_to_one_col() -
 *   return:
 *   parser(in):
 *   node(in):
 *   do_one(in):
 */
void
pt_select_list_to_one_col (PARSER_CONTEXT * parser, PT_NODE * node,
			   bool do_one)
{
  PT_NODE *val, *col, *list, *next;
  bool do_rewrite;

  if (!node)
    {
      return;
    }

  switch (node->node_type)
    {
    case PT_SELECT:
      list = node->info.query.q.select.list;
      if (do_one == true)
	{
	  do_rewrite = false;	/* init */
	  if (node->info.query.orderby_for)
	    {
	      do_rewrite = true;	/* give up */
	    }
	  else
	    {
	      for (col = list; col && do_rewrite != true; col = col->next)
		{
		  /* check orderby_num() exists */
		  if (col->node_type == PT_EXPR
		      && col->info.expr.op == PT_ORDERBY_NUM)
		    {
		      do_rewrite = true;	/* break */
		    }
		}
	    }

	  /* change node as select of query-derived table */
	  if (do_rewrite)
	    {
	      PT_NODE *derived, *from, *range_var, *spec;
	      int i;
	      char buf[20];

	      /* reset single tuple mark and move to derived */
	      node->info.query.single_tuple = 0;
	      derived = parser_copy_tree (parser, node);
	      parser_init_node (node);

	      /* new range var */
	      from = derived->info.query.q.select.from;
	      if ((range_var = from->info.spec.range_var) == NULL)
		{
		  /* set line number to range name */
		  range_var = pt_name (parser, "av3491");
		}

	      spec = parser_new_node (parser, PT_SPEC);
	      if (spec == NULL)
		{
		  PT_INTERNAL_ERROR (parser, "allocate new node");
		  return;
		}

	      spec->info.spec.derived_table = derived;
	      spec->info.spec.derived_table_type = PT_IS_SUBQUERY;
	      spec->info.spec.range_var = range_var;
	      /* new as attr list */
	      for (spec->info.spec.as_attr_list = NULL, i = 1, col = list;
		   col; i++, col = col->next)
		{
		  PT_NODE *att;

		  sprintf (buf, "av_%d", i);

		  att = pt_name (parser, pt_append_string (parser, NULL,
							   buf));
		  if (att)
		    {
		      PT_NAME_INFO_SET_FLAG (att,
					     PT_NAME_GENERATED_DERIVED_SPEC);
		    }
		  spec->info.spec.as_attr_list =
		    parser_append_node (att, spec->info.spec.as_attr_list);
		}

	      node->info.query.q.select.list =
		parser_copy_tree_list (parser, spec->info.spec.as_attr_list);
	      node->info.query.q.select.from = spec;
	    }
	  else
	    {
	      /* remove unnecessary ORDER BY clause */
	      if (node->info.query.order_by)
		{
		  parser_free_tree (parser, node->info.query.order_by);
		  node->info.query.order_by = NULL;
		}
	    }

	  /* create parentheses expr set value */
	  val = parser_new_node (parser, PT_VALUE);
	  if (val)
	    {
	      val->info.value.data_value.set = node->info.query.q.select.list;
	      val->type_enum = PT_TYPE_SEQUENCE;
	    }
	  node->info.query.q.select.list = val;
	}
      else
	{
	  if (pt_length_of_select_list (list, EXCLUDE_HIDDEN_COLUMNS) == 1
	      && PT_IS_COLLECTION_TYPE (list->type_enum))
	    {			/* one column */
	      col = list;
	      next = list->next;	/* save hidden column */
	      col->next = NULL;

	      if (list->node_type == PT_VALUE)
		{
		  list = col->info.value.data_value.set;
		  col->info.value.data_value.set = NULL;
		  parser_free_tree (parser, col);
		}
	      else if (list->node_type == PT_FUNCTION)
		{
		  list = col->info.function.arg_list;
		  col->info.function.arg_list = NULL;
		  parser_free_tree (parser, col);
		}
	      else
		{
		  list = col;
		}

	      /* restore hidden columns */
	      node->info.query.q.select.list =
		parser_append_node (next, list);
	    }
	}
      break;
    case PT_UNION:
    case PT_INTERSECTION:
    case PT_DIFFERENCE:
      pt_select_list_to_one_col (parser, node->info.query.q.union_.arg1,
				 do_one);
      pt_select_list_to_one_col (parser, node->info.query.q.union_.arg2,
				 do_one);
      break;
    default:
      break;
    }

  return;
}


/*
 * pt_check_set_count_set() -
 *   return: 1 for noerror, 0 for error
 *   parser(in):
 *   arg1(in):
 *   arg2(in):
 */
int
pt_check_set_count_set (PARSER_CONTEXT * parser, PT_NODE * arg1,
			PT_NODE * arg2)
{
  PT_NODE *e1, *e2;
  bool e1_is_expr_set, e2_is_expr_set;
  int e1_cnt, e2_cnt, rc;

  rc = 1;			/* set as NO_ERROR */

  if (arg1->node_type != PT_VALUE || !PT_IS_COLLECTION_TYPE (arg1->type_enum)
      || arg2->node_type != PT_VALUE
      || !PT_IS_COLLECTION_TYPE (arg2->type_enum))
    {
      return rc;		/* give up */
    }

  /* get elements */
  e1 = arg1->info.value.data_value.set;
  e2 = arg2->info.value.data_value.set;
  for (; e1 && e2; e1 = e1->next, e2 = e2->next)
    {
      e1_is_expr_set = e2_is_expr_set = false;	/* init */
      if (e1->node_type == PT_VALUE && PT_IS_COLLECTION_TYPE (e1->type_enum))
	{
	  e1_is_expr_set = true;
	}
      if (e2->node_type == PT_VALUE && PT_IS_COLLECTION_TYPE (e2->type_enum))
	{
	  e2_is_expr_set = true;
	}

      if (e1_is_expr_set == e2_is_expr_set)
	{
	  if (e1_is_expr_set == true)
	    {
	      /* do recursion */
	      if (!pt_check_set_count_set (parser, e1, e2))
		{
		  rc = 0;	/* error */
		}
	    }
	  else
	    {
	      if (!rc)
		{
		  /* already check this expression */
		  continue;
		}

	      /* expression number check */
	      e1_cnt = pt_get_expression_count (e1);
	      e2_cnt = pt_get_expression_count (e2);
	      if (e1_cnt > 0 && e2_cnt > 0 && e1_cnt != e2_cnt)
		{
		  PT_ERRORmf2 (parser, e2, MSGCAT_SET_PARSER_SEMANTIC,
			       MSGCAT_SEMANTIC_ATT_CNT_COL_CNT_NE, e1_cnt,
			       e2_cnt);
		  rc = 0;	/* error */
		}
	    }
	}
      else
	{
	  /* unknown error */
	  PT_ERROR (parser, e2, "check syntax at = or <>.");
	  rc = 0;		/* error */
	}
    }				/* for */

  if ((e1 || e2) && rc)
    {
      /* unknown error */
      PT_ERROR (parser, e2, "check syntax at = or <>.");
      rc = 0;			/* error */
    }

  return rc;
}


/*
 * pt_rewrite_set_eq_set() -
 *   return:
 *   parser(in):
 *   exp(in):
 */
PT_NODE *
pt_rewrite_set_eq_set (PARSER_CONTEXT * parser, PT_NODE * exp)
{
  PT_NODE *p = NULL, *rhs = NULL;
  PT_NODE *arg1, *arg2, *e1, *e2, *e1_next, *e2_next, *lhs, *tmp;
  bool e1_is_expr_set, e2_is_expr_set;

  if (exp == NULL)
    {
      return NULL;
    }

  arg1 = exp->info.expr.arg1;
  arg2 = exp->info.expr.arg2;

  if (arg1->node_type != PT_VALUE || !PT_IS_COLLECTION_TYPE (arg1->type_enum)
      || arg2->node_type != PT_VALUE
      || !PT_IS_COLLECTION_TYPE (arg2->type_enum))
    {
      return exp;		/* give up */
    }

  /* get elements and cut-off link */
  e1 = arg1->info.value.data_value.set;
  arg1->info.value.data_value.set = NULL;

  e2 = arg2->info.value.data_value.set;
  arg2->info.value.data_value.set = NULL;

  /* save and cut-off link */
  e1_next = e1->next;
  e1->next = NULL;

  e2_next = e2->next;
  e2->next = NULL;

  e1_is_expr_set = e2_is_expr_set = false;	/* init */
  if (e1->node_type == PT_VALUE && PT_IS_COLLECTION_TYPE (e1->type_enum))
    {
      e1_is_expr_set = true;
    }
  if (e2->node_type == PT_VALUE && PT_IS_COLLECTION_TYPE (e2->type_enum))
    {
      e2_is_expr_set = true;
    }

  if (e1_is_expr_set == e2_is_expr_set)
    {
      if (e1_is_expr_set == true)
	{
	  /* create temporary expr */
	  tmp = parser_copy_tree (parser, exp);
	  tmp->info.expr.arg1->info.value.data_value.set =
	    e1->info.value.data_value.set;
	  e1->info.value.data_value.set = NULL;
	  tmp->info.expr.arg2->info.value.data_value.set =
	    e2->info.value.data_value.set;
	  e2->info.value.data_value.set = NULL;

	  /* do recursion */
	  p = pt_rewrite_set_eq_set (parser, tmp);

	  /* free old elements */
	  parser_free_tree (parser, e1);
	  parser_free_tree (parser, e2);
	}
      else
	{
	  /* create new root node of predicate tree */
	  p = parser_new_node (parser, PT_EXPR);
	  if (p == NULL)
	    {
	      PT_INTERNAL_ERROR (parser, "allocate new node");
	      return NULL;
	    }

	  p->info.expr.op = PT_EQ;
	  p->info.expr.arg1 = e1;
	  p->info.expr.arg2 = e2;
	}
    }
  else
    {				/* error */
      PT_ERRORf (parser, exp, "check syntax at %s",
		 pt_show_binopcode (PT_EQ));
      /* free old elements */
      parser_free_tree (parser, e1);
      parser_free_tree (parser, e2);
    }

  pt_push (parser, p);

  /* create child nodes */
  for (e1 = e1_next, e2 = e2_next; e1 && e2; e1 = e1_next, e2 = e2_next)
    {
      /* save and cut-off link */
      e1_next = e1->next;
      e1->next = NULL;

      e2_next = e2->next;
      e2->next = NULL;

      lhs = pt_pop (parser);

      /* create '=' expr node */
      e1_is_expr_set = e2_is_expr_set = false;	/* init */
      if (e1->node_type == PT_VALUE && PT_IS_COLLECTION_TYPE (e1->type_enum))
	{
	  e1_is_expr_set = true;
	}
      if (e2->node_type == PT_VALUE && PT_IS_COLLECTION_TYPE (e2->type_enum))
	{
	  e2_is_expr_set = true;
	}

      if (e1_is_expr_set == e2_is_expr_set)
	{
	  if (e1_is_expr_set == true)
	    {
	      /* create temporary expr */
	      tmp = parser_copy_tree (parser, exp);
	      tmp->info.expr.arg1->info.value.data_value.set =
		e1->info.value.data_value.set;
	      e1->info.value.data_value.set = NULL;
	      tmp->info.expr.arg2->info.value.data_value.set =
		e2->info.value.data_value.set;
	      e2->info.value.data_value.set = NULL;

	      /* do recursion */
	      rhs = pt_rewrite_set_eq_set (parser, tmp);

	      /* free old elements */
	      parser_free_tree (parser, e1);
	      parser_free_tree (parser, e2);
	    }
	  else
	    {
	      /* create new child node */
	      rhs = parser_new_node (parser, PT_EXPR);
	      if (rhs == NULL)
		{
		  PT_INTERNAL_ERROR (parser, "allocate new node");
		  return NULL;
		}
	      rhs->info.expr.op = PT_EQ;
	      rhs->info.expr.arg1 = e1;
	      rhs->info.expr.arg2 = e2;
	    }
	}
      else
	{			/* error */
	  PT_ERRORf (parser, exp, "check syntax at %s",
		     pt_show_binopcode (PT_EQ));
	  /* free old elements */
	  parser_free_tree (parser, e1);
	  parser_free_tree (parser, e2);
	}

      /* create 'and' node */
      p = parser_new_node (parser, PT_EXPR);
      if (p == NULL)
	{
	  PT_INTERNAL_ERROR (parser, "allocate new node");
	  return NULL;
	}

      p->info.expr.op = PT_AND;
      p->info.expr.arg1 = lhs;
      p->info.expr.arg2 = rhs;
      p->info.expr.arg3 = NULL;

      pt_push (parser, p);
    }

  /* expression count check */
  if (e1 || e2)
    {
      PT_ERRORf (parser, exp,
		 "check syntax at %s, different number of elements in expression.",
		 pt_show_binopcode (PT_EQ));
    }

  p = pt_pop (parser);

  if (p == NULL)
    {
      return NULL;
    }

  /* bound with parentheses */
  p->info.expr.paren_type = 1;

  /* free old exp, arg1, arg2 node */
  if (exp)
    {
      arg1->info.value.data_value.set = NULL;
      arg2->info.value.data_value.set = NULL;
      parser_free_tree (parser, exp);
    }

  return p;
}


/*
 * pt_init_apply_f () - initialize function vector(called by parser_walk_tree...)
 *   return: none
 */
static void
pt_init_apply_f (void)
{
  pt_apply_func_array[PT_ALTER] = pt_apply_alter;
  pt_apply_func_array[PT_ALTER_INDEX] = pt_apply_alter_index;
  pt_apply_func_array[PT_ALTER_USER] = pt_apply_alter_user;
  pt_apply_func_array[PT_ATTR_DEF] = pt_apply_attr_def;
  pt_apply_func_array[PT_ATTR_ORDERING] = pt_apply_attr_ordering;
  pt_apply_func_array[PT_AUTH_CMD] = pt_apply_auth_cmd;
  pt_apply_func_array[PT_COMMIT_WORK] = pt_apply_commit_work;
  pt_apply_func_array[PT_CREATE_ENTITY] = pt_apply_create_entity;
  pt_apply_func_array[PT_CREATE_INDEX] = pt_apply_create_index;
  pt_apply_func_array[PT_CREATE_USER] = pt_apply_create_user;
  pt_apply_func_array[PT_DATA_DEFAULT] = pt_apply_data_default;
  pt_apply_func_array[PT_DATA_TYPE] = pt_apply_datatype;
  pt_apply_func_array[PT_DELETE] = pt_apply_delete;
  pt_apply_func_array[PT_DIFFERENCE] = pt_apply_difference;
  pt_apply_func_array[PT_DOT_] = pt_apply_dot;
  pt_apply_func_array[PT_DROP] = pt_apply_drop;
  pt_apply_func_array[PT_DROP_INDEX] = pt_apply_drop_index;
  pt_apply_func_array[PT_DROP_USER] = pt_apply_drop_user;
  pt_apply_func_array[PT_SPEC] = pt_apply_spec;
  pt_apply_func_array[PT_EXPR] = pt_apply_expr;
  pt_apply_func_array[PT_FUNCTION] = pt_apply_function;
  pt_apply_func_array[PT_GET_OPT_LVL] = pt_apply_get_opt_lvl;
  pt_apply_func_array[PT_GET_XACTION] = pt_apply_get_xaction;
  pt_apply_func_array[PT_GRANT] = pt_apply_grant;
  pt_apply_func_array[PT_HOST_VAR] = pt_apply_host_var;
  pt_apply_func_array[PT_INSERT] = pt_apply_insert;
  pt_apply_func_array[PT_INTERSECTION] = pt_apply_intersection;
  pt_apply_func_array[PT_ISOLATION_LVL] = pt_apply_isolation_lvl;
  pt_apply_func_array[PT_NAME] = pt_apply_name;
  pt_apply_func_array[PT_RENAME] = pt_apply_rename;
  pt_apply_func_array[PT_REVOKE] = pt_apply_revoke;
  pt_apply_func_array[PT_ROLLBACK_WORK] = pt_apply_rollback_work;
  pt_apply_func_array[PT_SAVEPOINT] = pt_apply_savepoint;
  pt_apply_func_array[PT_SELECT] = pt_apply_select;
  pt_apply_func_array[PT_SET_OPT_LVL] = pt_apply_set_opt_lvl;
  pt_apply_func_array[PT_SET_SYS_PARAMS] = pt_apply_set_sys_params;
  pt_apply_func_array[PT_SORT_SPEC] = pt_apply_sort_spec;
  pt_apply_func_array[PT_TIMEOUT] = pt_apply_timeout;
  pt_apply_func_array[PT_UNION] = pt_apply_union_stmt;
  pt_apply_func_array[PT_UPDATE] = pt_apply_update;
  pt_apply_func_array[PT_UPDATE_STATS] = pt_apply_update_stats;
  pt_apply_func_array[PT_VALUE] = pt_apply_value;
  pt_apply_func_array[PT_ZZ_ERROR_MSG] = pt_apply_error_msg;
  pt_apply_func_array[PT_CONSTRAINT] = pt_apply_constraint;
  pt_apply_func_array[PT_POINTER] = pt_apply_pointer;
  pt_apply_func_array[PT_NODE_LIST] = pt_apply_node_list;
  pt_apply_func_array[PT_TABLE_OPTION] = pt_apply_table_option;
  pt_apply_func_array[PT_QUERY_TRACE] = pt_apply_query_trace;

  pt_apply_f = pt_apply_func_array;
}

/*
 * pt_init_init_f () - initialize function vector(called by parser_new_node...)
 *   return: none
 */
static void
pt_init_init_f (void)
{
  pt_init_func_array[PT_ALTER] = pt_init_alter;
  pt_init_func_array[PT_ALTER_INDEX] = pt_init_alter_index;
  pt_init_func_array[PT_ALTER_USER] = pt_init_alter_user;
  pt_init_func_array[PT_ATTR_DEF] = pt_init_attr_def;
  pt_init_func_array[PT_ATTR_ORDERING] = pt_init_attr_ordering;
  pt_init_func_array[PT_AUTH_CMD] = pt_init_auth_cmd;
  pt_init_func_array[PT_COMMIT_WORK] = pt_init_commit_work;
  pt_init_func_array[PT_CREATE_ENTITY] = pt_init_create_entity;
  pt_init_func_array[PT_CREATE_INDEX] = pt_init_create_index;
  pt_init_func_array[PT_CREATE_USER] = pt_init_create_user;
  pt_init_func_array[PT_DATA_DEFAULT] = pt_init_data_default;
  pt_init_func_array[PT_DATA_TYPE] = pt_init_datatype;
  pt_init_func_array[PT_DELETE] = pt_init_delete;
  pt_init_func_array[PT_DIFFERENCE] = pt_init_difference;
  pt_init_func_array[PT_DOT_] = pt_init_dot;
  pt_init_func_array[PT_DROP] = pt_init_drop;
  pt_init_func_array[PT_DROP_INDEX] = pt_init_drop_index;
  pt_init_func_array[PT_DROP_USER] = pt_init_drop_user;
  pt_init_func_array[PT_SPEC] = pt_init_spec;
  pt_init_func_array[PT_EXPR] = pt_init_expr;
  pt_init_func_array[PT_FUNCTION] = pt_init_function;
  pt_init_func_array[PT_GET_OPT_LVL] = pt_init_get_opt_lvl;
  pt_init_func_array[PT_GET_XACTION] = pt_init_get_xaction;
  pt_init_func_array[PT_GRANT] = pt_init_grant;
  pt_init_func_array[PT_HOST_VAR] = pt_init_host_var;
  pt_init_func_array[PT_INSERT] = pt_init_insert;
  pt_init_func_array[PT_INTERSECTION] = pt_init_intersection;
  pt_init_func_array[PT_ISOLATION_LVL] = pt_init_isolation_lvl;
  pt_init_func_array[PT_NAME] = pt_init_name;
  pt_init_func_array[PT_RENAME] = pt_init_rename;
  pt_init_func_array[PT_REVOKE] = pt_init_revoke;
  pt_init_func_array[PT_ROLLBACK_WORK] = pt_init_rollback_work;
  pt_init_func_array[PT_SAVEPOINT] = pt_init_savepoint;
  pt_init_func_array[PT_SELECT] = pt_init_select;
  pt_init_func_array[PT_SET_OPT_LVL] = pt_init_set_opt_lvl;
  pt_init_func_array[PT_SET_SYS_PARAMS] = pt_init_set_sys_params;
  pt_init_func_array[PT_SORT_SPEC] = pt_init_sort_spec;
  pt_init_func_array[PT_TIMEOUT] = pt_init_timeout;
  pt_init_func_array[PT_UNION] = pt_init_union_stmt;
  pt_init_func_array[PT_UPDATE] = pt_init_update;
  pt_init_func_array[PT_UPDATE_STATS] = pt_init_update_stats;
  pt_init_func_array[PT_VALUE] = pt_init_value;
  pt_init_func_array[PT_ZZ_ERROR_MSG] = pt_init_error_msg;
  pt_init_func_array[PT_CONSTRAINT] = pt_init_constraint;
  pt_init_func_array[PT_POINTER] = pt_init_pointer;

  pt_init_func_array[PT_NODE_LIST] = pt_init_node_list;
  pt_init_func_array[PT_TABLE_OPTION] = pt_init_table_option;
  pt_init_func_array[PT_QUERY_TRACE] = pt_init_query_trace;

  pt_init_f = pt_init_func_array;
}

/*
 * pt_init_print_f () - initialize function vector(called by pt_tree_print...)
 *   return: none
 */
static void
pt_init_print_f (void)
{
  pt_print_func_array[PT_ALTER] = pt_print_alter;
  pt_print_func_array[PT_ALTER_INDEX] = pt_print_alter_index;
  pt_print_func_array[PT_ALTER_USER] = pt_print_alter_user;
  pt_print_func_array[PT_ATTR_DEF] = pt_print_attr_def;
  pt_print_func_array[PT_ATTR_ORDERING] = pt_print_attr_ordering;
  pt_print_func_array[PT_AUTH_CMD] = pt_print_auth_cmd;
  pt_print_func_array[PT_COMMIT_WORK] = pt_print_commit_work;
  pt_print_func_array[PT_CREATE_ENTITY] = pt_print_create_entity;
  pt_print_func_array[PT_CREATE_INDEX] = pt_print_create_index;
  pt_print_func_array[PT_CREATE_USER] = pt_print_create_user;
  pt_print_func_array[PT_DATA_DEFAULT] = pt_print_data_default;
  pt_print_func_array[PT_DATA_TYPE] = pt_print_datatype;
  pt_print_func_array[PT_DELETE] = pt_print_delete;
  pt_print_func_array[PT_DIFFERENCE] = pt_print_difference;
  pt_print_func_array[PT_DOT_] = pt_print_dot;
  pt_print_func_array[PT_DROP] = pt_print_drop;
  pt_print_func_array[PT_DROP_INDEX] = pt_print_drop_index;
  pt_print_func_array[PT_DROP_USER] = pt_print_drop_user;
  pt_print_func_array[PT_SPEC] = pt_print_spec;
  pt_print_func_array[PT_EXPR] = pt_print_expr;
  pt_print_func_array[PT_FUNCTION] = pt_print_function;
  pt_print_func_array[PT_GET_OPT_LVL] = pt_print_get_opt_lvl;
  pt_print_func_array[PT_GET_XACTION] = pt_print_get_xaction;
  pt_print_func_array[PT_GRANT] = pt_print_grant;
  pt_print_func_array[PT_HOST_VAR] = pt_print_host_var;
  pt_print_func_array[PT_INSERT] = pt_print_insert;
  pt_print_func_array[PT_INTERSECTION] = pt_print_intersection;
  pt_print_func_array[PT_ISOLATION_LVL] = pt_print_isolation_lvl;
  pt_print_func_array[PT_NAME] = pt_print_name;
  pt_print_func_array[PT_RENAME] = pt_print_rename;
  pt_print_func_array[PT_REVOKE] = pt_print_revoke;
  pt_print_func_array[PT_ROLLBACK_WORK] = pt_print_rollback_work;
  pt_print_func_array[PT_SAVEPOINT] = pt_print_savepoint;
  pt_print_func_array[PT_SELECT] = pt_print_select;
  pt_print_func_array[PT_SET_OPT_LVL] = pt_print_set_opt_lvl;
  pt_print_func_array[PT_SET_SYS_PARAMS] = pt_print_set_sys_params;
  pt_print_func_array[PT_SORT_SPEC] = pt_print_sort_spec;
  pt_print_func_array[PT_TIMEOUT] = pt_print_timeout;
  pt_print_func_array[PT_UNION] = pt_print_union_stmt;
  pt_print_func_array[PT_UPDATE] = pt_print_update;
  pt_print_func_array[PT_UPDATE_STATS] = pt_print_update_stats;
  pt_print_func_array[PT_VALUE] = pt_print_value;
  pt_print_func_array[PT_ZZ_ERROR_MSG] = pt_print_error_msg;
  pt_print_func_array[PT_CONSTRAINT] = pt_print_constraint;
  pt_print_func_array[PT_POINTER] = pt_print_pointer;
  pt_print_func_array[PT_NODE_LIST] = pt_print_node_list;
  pt_print_func_array[PT_TABLE_OPTION] = pt_print_table_option;
  pt_print_func_array[PT_QUERY_TRACE] = pt_print_query_trace;

  pt_print_f = pt_print_func_array;
}

/*
 * pt_append_name () - if the given string is not a keyword and has no
 *  non-alpha characters, append it. Otherwise, append it within double quotes
 *   return:
 *   parser(in):
 *   string(out):
 *   name(in):
 */
PARSER_VARCHAR *
pt_append_name (const PARSER_CONTEXT * parser, PARSER_VARCHAR * string,
		const char *name)
{
  if ((!(parser->custom_print & PT_SUPPRESS_QUOTES)
       && (pt_is_keyword (name)
	   || lang_check_identifier (name, strlen (name)) != true))
      || parser->custom_print & PT_PRINT_QUOTES)
    {
      string = pt_append_nulstring (parser, string, "[");
      string = pt_append_nulstring (parser, string, name);
      string = pt_append_nulstring (parser, string, "]");
    }
  else
    {
      string = pt_append_nulstring (parser, string, name);
    }
  return string;
}


/*
 * pt_append_quoted_string () - Quote and append a string,
 *                              breaking it into pieces if necessary
 *   return:
 *   parser(in):
 *   buf(out):
 *   str(in):
 *   str_length(in):
 *
 * Note :
 * Keep track of how many characters we've written out, and break
 * the string into juxtaposed string lits if we exceed some
 * maximum.  This is a concession to parsers that have smallish
 * token accumulation buffers.
 * MAX_STRING_SEGMENT_LENGTH is the maximum number of characters
 * that will be put between two single quotes.
 *
 */
static PARSER_VARCHAR *
pt_append_quoted_string (const PARSER_CONTEXT * parser, PARSER_VARCHAR * buf,
			 const char *str, size_t str_length)
{
  size_t i;
  size_t out_length;

  out_length = 0;
  buf = pt_append_nulstring (parser, buf, "'");
  if (str)
    {
      for (i = 0; i < str_length; i++)
	{
	  if (str[i] == '\'')
	    {
	      buf = pt_append_bytes (parser, buf, "'", 1);
	      out_length++;
	    }
	  buf = pt_append_bytes (parser, buf, &str[i], 1);
	  out_length++;

	  if (out_length >= MAX_STRING_SEGMENT_LENGTH)
	    {
	      buf = pt_append_nulstring (parser, buf, "' '");
	      out_length = 0;
	    }
	}
    }
  buf = pt_append_nulstring (parser, buf, "'");

  return buf;
}

/*
 * pt_append_string_prefix () - Print out any necessary string prefix modifier
 *                              (e.g., 'B' or 'X')
 *   return:
 *   parser(in):
 *   buf(out):
 *   value(in):
 */
static PARSER_VARCHAR *
pt_append_string_prefix (const PARSER_CONTEXT * parser, PARSER_VARCHAR * buf,
			 const PT_NODE * value)
{
  char prefix[2];

  if (value->info.value.string_type != ' ')
    {
      prefix[0] = value->info.value.string_type;
      prefix[1] = '\0';
      buf = pt_append_nulstring (parser, buf, prefix);
    }

  return buf;
}

/*
 * pt_apply_alter () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_alter (PARSER_CONTEXT * parser, PT_NODE * p,
		PT_NODE_FUNCTION g, void *arg)
{
  p->info.alter.entity_name = g (parser, p->info.alter.entity_name, arg);

  switch (p->info.alter.code)
    {
    case PT_ADD_ATTR_MTHD:
    case PT_DROP_ATTR_MTHD:
    case PT_CHANGE_ATTR:
      p->info.alter.alter_clause.attr_mthd.attr_def_list =
	g (parser, p->info.alter.alter_clause.attr_mthd.attr_def_list, arg);
      p->info.alter.alter_clause.attr_mthd.attr_old_name =
	g (parser, p->info.alter.alter_clause.attr_mthd.attr_old_name, arg);
      p->info.alter.alter_clause.attr_mthd.attr_mthd_name_list =
	g (parser, p->info.alter.alter_clause.attr_mthd.attr_mthd_name_list,
	   arg);
      break;
    case PT_RENAME_ATTR_MTHD:
    case PT_RENAME_ENTITY:
      p->info.alter.alter_clause.rename.old_name = g (parser,
						      p->info.
						      alter.alter_clause.
						      rename.old_name, arg);
      p->info.alter.alter_clause.rename.new_name =
	g (parser, p->info.alter.alter_clause.rename.new_name, arg);
      break;
    default:
      break;
    }
  p->info.alter.constraint_list =
    g (parser, p->info.alter.constraint_list, arg);
  return p;
}

/*
 * pt_init_alter () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_alter (PT_NODE * p)
{
  p->info.alter.constraint_list = NULL;
  return p;
}

/*
 * pt_print_alter_one_clause () -
 *   return:
 *   parser(in):
 *   p(in):
 */

static PARSER_VARCHAR *
pt_print_alter_one_clause (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = NULL, *r1 = NULL, *r2 = NULL;
  PT_NODE *names = NULL, *attrs = NULL;
  bool close_parenthesis = false;

  switch (p->info.alter.code)
    {
    case PT_CHANGE_OWNER:
      r1 =
	pt_print_bytes_l (parser, p->info.alter.alter_clause.user.user_name);
      q = pt_append_nulstring (parser, q, " owner to ");
      q = pt_append_varchar (parser, q, r1);
      break;
    case PT_ADD_ATTR_MTHD:
      q = pt_append_nulstring (parser, q, " add ");
      close_parenthesis = false;
      attrs = p->info.alter.alter_clause.attr_mthd.attr_def_list;
      if (attrs)
	{
	  r1 = pt_print_bytes_l (parser,
				 p->info.alter.alter_clause.
				 attr_mthd.attr_def_list);
	  q = pt_append_nulstring (parser, q, "attribute (");
	  close_parenthesis = true;
	  q = pt_append_varchar (parser, q, r1);
	}

      if (p->info.alter.constraint_list)
	{
	  r1 = pt_print_bytes_l (parser, p->info.alter.constraint_list);
	  if (r1)
	    {
	      if (close_parenthesis)
		{
		  q = pt_append_nulstring (parser, q, ", ");
		}
	      else
		{
		  q = pt_append_nulstring (parser, q, "(");
		  close_parenthesis = true;
		}
	      q = pt_append_varchar (parser, q, r1);
	    }
	}

      if (close_parenthesis)
	{
	  q = pt_append_nulstring (parser, q, ")");
	}

      break;
    case PT_DROP_ATTR_MTHD:
      q = pt_append_nulstring (parser, q, " drop ");
      names = p->info.alter.alter_clause.attr_mthd.attr_mthd_name_list;
      while (names)
	{
	  r1 = pt_print_bytes (parser, names);
	  q = pt_append_varchar (parser, q, r1);
	  names = names->next;
	  if (names != NULL)
	    {
	      q = pt_append_nulstring (parser, q, ", ");
	    }
	}
      break;
    case PT_CHANGE_ATTR:
      {
	/* only one attibute per alter clause should be allowed :
	 * <attr_old_name> and <attr_def_list> should have at most one element*/
	if (p->info.alter.alter_clause.attr_mthd.attr_old_name != NULL)
	  {
	    q = pt_append_nulstring (parser, q, " change");
	    names = p->info.alter.alter_clause.attr_mthd.attr_old_name;
	  }
	else
	  {
	    q = pt_append_nulstring (parser, q, " modify");
	    names = NULL;
	  }

	attrs = p->info.alter.alter_clause.attr_mthd.attr_def_list;
	assert (attrs != NULL);
	q = pt_append_nulstring (parser, q, " attribute ");

	if (names != NULL)
	  {
	    assert (names->next == NULL);
	    r2 = pt_print_bytes (parser, names);
	    q = pt_append_varchar (parser, q, r2);
	    q = pt_append_nulstring (parser, q, " ");
	  }

	assert (attrs->next == NULL);

	/* ordering is last in <CHANGE> syntax context, suppress in this
	 * print */
	if (attrs->info.attr_def.ordering_info != NULL)
	  {
	    parser->custom_print |= PT_SUPPRESS_ORDERING;
	  }

	assert (attrs->info.attr_def.attr_type != PT_CLASS);
	r1 = pt_print_bytes (parser, attrs);
	q = pt_append_varchar (parser, q, r1);
	q = pt_append_nulstring (parser, q, " ");

	if (attrs->info.attr_def.ordering_info != NULL)
	  {
	    parser->custom_print &= ~PT_SUPPRESS_ORDERING;
	  }

	if (p->info.alter.constraint_list != NULL)
	  {
	    PT_NODE *c_node = p->info.alter.constraint_list;

	    r1 = pt_print_col_def_constraint (parser, c_node);

	    while (c_node->next != NULL)
	      {			/* print in the original order ... */
		c_node = c_node->next;
		r2 = pt_print_col_def_constraint (parser, c_node);
		if (r2 != NULL)
		  {
		    r1 = pt_append_varchar (parser, r1, r2);
		  }
	      }
	    if (r1)
	      {
		assert (attrs != NULL);
		q = pt_append_varchar (parser, q, r1);
		q = pt_append_nulstring (parser, q, " ");
	      }
	  }

	if (attrs->info.attr_def.ordering_info != NULL)
	  {
	    r1 = pt_print_bytes (parser, attrs->info.attr_def.ordering_info);
	    q = pt_append_varchar (parser, q, r1);
	    q = pt_append_nulstring (parser, q, " ");
	  }
      }
      break;

    case PT_CHANGE_PK:
      q = pt_append_nulstring (parser, q, " change primary key ");
      r1 =
	pt_print_bytes (parser, p->info.alter.alter_clause.chg_pk.index_name);
      q = pt_append_varchar (parser, q, r1);
      break;

    case PT_RENAME_ENTITY:
      q = pt_append_nulstring (parser, q, " rename to ");
      r1 =
	pt_print_bytes (parser, p->info.alter.alter_clause.rename.new_name);
      q = pt_append_varchar (parser, q, r1);
      break;
    case PT_RENAME_ATTR_MTHD:
      q = pt_append_nulstring (parser, q, " rename ");
      q = pt_append_nulstring (parser, q,
			       pt_show_misc_type (p->info.alter.
						  alter_clause.rename.
						  element_type));
      q = pt_append_nulstring (parser, q, " ");

      switch (p->info.alter.alter_clause.rename.element_type)
	{
	default:
	  break;
	case PT_ATTRIBUTE:
	  r1 = pt_print_bytes (parser,
			       p->info.alter.alter_clause.rename.old_name);
	  q = pt_append_nulstring (parser, q,
				   pt_show_misc_type (p->info.
						      alter.alter_clause.
						      rename.meta));
	  q = pt_append_nulstring (parser, q, " ");
	  q = pt_append_varchar (parser, q, r1);
	  break;
	}
      r1 = pt_print_bytes (parser,
			   p->info.alter.alter_clause.rename.new_name);
      q = pt_append_nulstring (parser, q, " as ");
      q = pt_append_varchar (parser, q, r1);
      break;
    case PT_DROP_CONSTRAINT:
      if (p->info.alter.constraint_list)
	{
	  r1 = pt_print_bytes_l (parser, p->info.alter.constraint_list);
	  q = pt_append_nulstring (parser, q, " drop constraint ");
	  q = pt_append_varchar (parser, q, r1);
	}
      break;
    case PT_DROP_INDEX_CLAUSE:
      if (p->info.alter.constraint_list)
	{
	  r1 = pt_print_bytes_l (parser, p->info.alter.constraint_list);
	  q = pt_append_nulstring (parser, q, " drop ");
	  if (p->info.alter.alter_clause.index.unique)
	    {
	      q = pt_append_nulstring (parser, q, "unique ");
	    }
	  q = pt_append_nulstring (parser, q, "index ");
	  q = pt_append_varchar (parser, q, r1);
	}
      break;
    default:
      break;
    }
  return q;
}

/*
 * pt_print_alter () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_alter (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = NULL, *r1 = NULL;
  PT_NODE *crt_clause = NULL;

  /* ALTER VCLASS XYZ ... */
  r1 = pt_print_bytes (parser, p->info.alter.entity_name);
  q = pt_append_nulstring (parser, q, "alter ");
  q = pt_append_nulstring (parser, q,
			   pt_show_misc_type (p->info.alter.entity_type));
  q = pt_append_nulstring (parser, q, " ");
  q = pt_append_varchar (parser, q, r1);

  for (crt_clause = p; crt_clause != NULL; crt_clause = crt_clause->next)
    {
      r1 = pt_print_alter_one_clause (parser, crt_clause);
      q = pt_append_varchar (parser, q, r1);
      if (crt_clause->next != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	}
    }
  return q;
}

/* ALTER_INDEX */
/*
 * pt_apply_alter_index () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_alter_index (PARSER_CONTEXT * parser, PT_NODE * p,
		      PT_NODE_FUNCTION g, void *arg)
{
  p->info.index.indexed_class = g (parser, p->info.index.indexed_class, arg);
  p->info.index.column_names = g (parser, p->info.index.column_names, arg);

  return p;
}

/*
 * pt_init_alter_index () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_alter_index (PT_NODE * p)
{
  p->info.index.indexed_class = p->info.index.column_names = NULL;

  return p;
}

/*
 * pt_print_alter_index () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_alter_index (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0, *r1, *r2;
  unsigned int saved_cp = parser->custom_print;

  parser->custom_print |= PT_SUPPRESS_RESOLVED;

  r1 = pt_print_bytes (parser, p->info.index.indexed_class);
  r2 = pt_print_index_columns (parser, p);

  parser->custom_print = saved_cp;

  b = pt_append_nulstring (parser, b, "alter");
  assert (p->info.index.unique == false);	/* unused at ALTER */
  b = pt_append_nulstring (parser, b, " index ");
  if (p->info.index.index_name)
    {
      const char *index_name = p->info.index.index_name->info.name.original;
      b = pt_append_bytes (parser, b, index_name, strlen (index_name));
    }

  if (r1 != NULL)
    {
      b = pt_append_nulstring (parser, b, " on ");
      b = pt_append_varchar (parser, b, r1);

      if (r2 != NULL)
	{
	  b = pt_append_nulstring (parser, b, " (");
	  b = pt_append_varchar (parser, b, r2);
	  b = pt_append_nulstring (parser, b, ")");
	}
    }

  b = pt_append_nulstring (parser, b, " rebuild");

  return b;
}

/* ALTER_USER */
/*
 * pt_apply_alter_user () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_alter_user (PARSER_CONTEXT * parser, PT_NODE * p,
		     PT_NODE_FUNCTION g, void *arg)
{
  p->info.alter_user.user_name =
    g (parser, p->info.alter_user.user_name, arg);
  p->info.alter_user.password = g (parser, p->info.alter_user.password, arg);
  return p;
}

/*
 * pt_init_alter_user () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_alter_user (PT_NODE * p)
{
  p->info.alter_user.user_name = p->info.alter_user.password = NULL;
  p->info.alter_user.is_encrypted = false;

  return p;
}


/*
 * pt_print_alter_user () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_alter_user (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0, *r1;

  r1 = pt_print_bytes (parser, p->info.alter_user.user_name);
  b = pt_append_nulstring (parser, b, "alter user ");
  b = pt_append_varchar (parser, b, r1);

  r1 = pt_print_bytes (parser, p->info.alter_user.password);
  b = pt_append_nulstring (parser, b, " password ");
  b = pt_append_varchar (parser, b, r1);

  if (p->info.alter_user.is_encrypted == true)
    {
      b = pt_append_nulstring (parser, b, " encrypt");
    }

  return b;
}


/* ATTR_DEF */
/*
 * pt_apply_attr_def () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_attr_def (PARSER_CONTEXT * parser, PT_NODE * p,
		   PT_NODE_FUNCTION g, void *arg)
{
  p->info.attr_def.attr_name = g (parser, p->info.attr_def.attr_name, arg);
  p->info.attr_def.data_default =
    g (parser, p->info.attr_def.data_default, arg);
  p->info.attr_def.ordering_info =
    g (parser, p->info.attr_def.ordering_info, arg);
  return p;
}

/*
 * pt_init_attr_def () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_attr_def (PT_NODE * p)
{
  p->info.attr_def.attr_name = 0;
  p->info.attr_def.data_default = 0;
  p->info.attr_def.attr_type = PT_NORMAL;
  return p;
}

/*
 * pt_print_attr_def () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_attr_def (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;
  char s[PT_MEMB_BUF_SIZE];

  r1 = pt_print_bytes (parser, p->info.attr_def.attr_name);
  q = pt_append_varchar (parser, q, r1);
  q = pt_append_nulstring (parser, q, " ");

  switch (p->type_enum)
    {
    case PT_TYPE_OBJECT:
      if (p->data_type)
	{
	  r1 = pt_print_bytes (parser, p->data_type);
	  q = pt_append_varchar (parser, q, r1);
	}
      else
	{
	  q = pt_append_nulstring (parser, q, "object");
	}
      break;
    case PT_TYPE_NUMERIC:
      q = pt_append_nulstring (parser, q, pt_show_type_enum (p->type_enum));
      if (p->data_type)
	{
	  /* only show non-default parameter */
	  if (p->data_type->info.data_type.precision !=
	      DB_DEFAULT_NUMERIC_PRECISION
	      || p->data_type->info.data_type.dec_scale !=
	      DB_DEFAULT_NUMERIC_SCALE)
	    {
	      sprintf (s, "(%d,%d)",
		       p->data_type->info.data_type.precision,
		       p->data_type->info.data_type.dec_scale);
	      q = pt_append_nulstring (parser, q, s);
	    }
	}
      break;
    case PT_TYPE_VARCHAR:
    case PT_TYPE_VARBIT:
      q = pt_append_nulstring (parser, q, pt_show_type_enum (p->type_enum));
      if (p->data_type)
	{
	  bool show_precision;
	  int precision;

	  precision = p->data_type->info.data_type.precision;

	  /* variable data type:
	     only show non-maximum(i.e., default) parameter */
	  show_precision = (precision == TP_FLOATING_PRECISION_VALUE)
	    ? (false)
	    : (p->type_enum == PT_TYPE_VARCHAR)
	    ? (precision != DB_MAX_VARCHAR_PRECISION)
	    : (p->type_enum == PT_TYPE_VARBIT)
	    ? (precision != DB_MAX_VARBIT_PRECISION) : (precision != 7);

	  if (show_precision == true)
	    {
	      sprintf (s, "(%d)", precision);
	      q = pt_append_nulstring (parser, q, s);
	    }
	}
      break;
    case PT_TYPE_INTEGER:
    case PT_TYPE_BIGINT:
    case PT_TYPE_DOUBLE:
    case PT_TYPE_DATE:
    case PT_TYPE_TIME:
    case PT_TYPE_DATETIME:
      q = pt_append_nulstring (parser, q, pt_show_type_enum (p->type_enum));
      break;
    case PT_TYPE_NONE:
      /* no type is a blank attr def, as in view creation */
      break;
    default:
      q = pt_append_nulstring (parser, q, pt_show_type_enum (p->type_enum));
      if (p->data_type)
	{
	  r1 = pt_print_bytes_l (parser, p->data_type);
	  q = pt_append_nulstring (parser, q, "(");
	  q = pt_append_varchar (parser, q, r1);
	  q = pt_append_nulstring (parser, q, ")");
	}
      break;
    }

  /* collation must be the first to be printed after type, precision */
  if (PT_HAS_COLLATION (p->type_enum) && p->data_type != NULL
      && (p->data_type->info.data_type.has_coll_spec))
    {
      sprintf (s, " collate %s",
	       lang_get_collation_name (p->data_type->info.data_type.
					collation_id));
      q = pt_append_nulstring (parser, q, s);
    }

  if (p->info.attr_def.data_default)
    {
      r1 = pt_print_bytes (parser, p->info.attr_def.data_default);
      q = pt_append_varchar (parser, q, r1);
    }

  /*  The constraint information is no longer available in the attribute
     branch of the parse tree.  For now we'll just comment this section out.
     If we really want to print out this information, we'll have to search
     the constraint branch of the parse tree to get it.
     if (p->info.attr_def.constrain_unique)
     q=pt_append_nulstring(parser, q, " unique ");
   */

  if (p->info.attr_def.constrain_not_null)
    {
      q = pt_append_nulstring (parser, q, " not null ");
    }

  if (!(parser->custom_print & PT_SUPPRESS_ORDERING) &&
      p->info.attr_def.ordering_info)
    {
      r1 = pt_print_bytes (parser, p->info.attr_def.ordering_info);
      q = pt_append_nulstring (parser, q, " ");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, " ");
    }
  return q;
}

/* ATTR_ORDERING */
/*
 * pt_apply_attr_ordering () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_attr_ordering (PARSER_CONTEXT * parser, PT_NODE * p,
			PT_NODE_FUNCTION g, void *arg)
{
  p->info.attr_ordering.after = g (parser, p->info.attr_ordering.after, arg);
  return p;
}

/*
 * pt_init_attr_ordering () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_attr_ordering (PT_NODE * p)
{
  p->info.attr_ordering.after = NULL;
  p->info.attr_ordering.first = false;
  return p;
}

/*
 * pt_print_attr_ordering () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_attr_ordering (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = NULL, *r1 = NULL;

  if (p->info.attr_ordering.first)
    {
      q = pt_append_nulstring (parser, q, "first");
    }
  else
    {
      r1 = pt_print_bytes (parser, p->info.attr_ordering.after);
      q = pt_append_nulstring (parser, q, "after ");
      q = pt_append_varchar (parser, q, r1);
    }

  return q;
}

/* AUTH_CMD */
/*
 * pt_apply_auth_cmd () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_auth_cmd (PARSER_CONTEXT * parser, PT_NODE * p,
		   PT_NODE_FUNCTION g, void *arg)
{
  p->info.auth_cmd.attr_mthd_list
    = g (parser, p->info.auth_cmd.attr_mthd_list, arg);
  return p;
}

/*
 * pt_init_auth_cmd () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_auth_cmd (PT_NODE * p)
{
  p->info.auth_cmd.auth_cmd = PT_NO_PRIV;
  p->info.auth_cmd.attr_mthd_list = 0;
  return (p);
}

/*
 * pt_print_auth_cmd () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_auth_cmd (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;
  q = pt_append_nulstring (parser, q,
			   pt_show_priv (p->info.auth_cmd.auth_cmd));

  if (p->info.auth_cmd.attr_mthd_list)
    {
      r1 = pt_print_bytes_l (parser, p->info.auth_cmd.attr_mthd_list);
      q = pt_append_nulstring (parser, q, "(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
    }
  return q;
}

/* COMMIT_WORK */
/*
 * pt_apply_commit_work () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_commit_work (UNUSED_ARG PARSER_CONTEXT * parser, PT_NODE * p,
		      UNUSED_ARG PT_NODE_FUNCTION g, UNUSED_ARG void *arg)
{
  return p;
}

/*
 * pt_init_commit_work () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_commit_work (PT_NODE * p)
{
  return (p);
}

/*
 * pt_print_commit_work () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_commit_work (PARSER_CONTEXT * parser, UNUSED_ARG PT_NODE * p)
{
  PARSER_VARCHAR *q = NULL;

  q = pt_append_nulstring (parser, q, "commit work");

  return q;
}


/* CREATE_ENTITY */
/*
 * pt_apply_create_entity () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_create_entity (PARSER_CONTEXT * parser, PT_NODE * p,
			PT_NODE_FUNCTION g, void *arg)
{
  p->info.create_entity.entity_name
    = g (parser, p->info.create_entity.entity_name, arg);
  p->info.create_entity.attr_def_list
    = g (parser, p->info.create_entity.attr_def_list, arg);
  p->info.create_entity.as_query_list
    = g (parser, p->info.create_entity.as_query_list, arg);
  p->info.create_entity.update
    = g (parser, p->info.create_entity.update, arg);
  p->info.create_entity.constraint_list
    = g (parser, p->info.create_entity.constraint_list, arg);
  p->info.create_entity.internal_stmts
    = g (parser, p->info.create_entity.internal_stmts, arg);
  p->info.create_entity.create_like =
    g (parser, p->info.create_entity.create_like, arg);
  return p;
}

/*
 * pt_init_create_entity () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_create_entity (PT_NODE * p)
{
  p->info.create_entity.entity_name = 0;
  p->info.create_entity.entity_type = (PT_MISC_TYPE) 0;
  p->info.create_entity.attr_def_list = 0;
  p->info.create_entity.as_query_list = 0;
  p->info.create_entity.update = 0;
  p->info.create_entity.constraint_list = 0;
  p->info.create_entity.create_like = 0;
  p->info.create_entity.or_replace = 0;
  p->info.create_entity.is_shard = 0;
  return p;
}

/*
 * pt_print_create_entity () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_create_entity (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;
  unsigned int save_custom;
  PT_MISC_TYPE e_type;

  e_type = p->info.create_entity.entity_type;

  r1 = pt_print_bytes (parser, p->info.create_entity.entity_name);
  q = pt_append_nulstring (parser, q, "create ");
  if (p->info.create_entity.or_replace)
    {
      assert (e_type == PT_VCLASS);
      q = pt_append_nulstring (parser, q, "or replace ");
    }

  if (p->info.create_entity.or_replace)
    {
      assert (p->info.create_entity.is_shard == 0);
    }
  else
    {
      if (e_type == PT_CLASS)
	{
	  if (p->info.create_entity.is_shard)
	    {
	      q = pt_append_nulstring (parser, q, "shard ");
	    }
	  else
	    {
	      q = pt_append_nulstring (parser, q, "global ");
	    }

	}
    }

  q = pt_append_nulstring (parser, q, pt_show_misc_type (e_type));
  q = pt_append_nulstring (parser, q, " ");
  q = pt_append_varchar (parser, q, r1);

  if (p->info.create_entity.create_like)
    {
      r1 = pt_print_bytes (parser, p->info.create_entity.create_like);
      q = pt_append_nulstring (parser, q, " like ");
      q = pt_append_varchar (parser, q, r1);
      return q;
    }

  if (p->info.create_entity.attr_def_list
      || p->info.create_entity.constraint_list)
    {
      PT_NODE *constraint;

      save_custom = parser->custom_print;
      parser->custom_print |= PT_SUPPRESS_RESOLVED;
      r1 = pt_print_bytes_l (parser, p->info.create_entity.attr_def_list);
      parser->custom_print = save_custom;

      q = pt_append_nulstring (parser, q, " ( ");
      q = pt_append_varchar (parser, q, r1);

      /* Don't print out not-null constraints */
      constraint = p->info.create_entity.constraint_list;
      while (constraint
	     && (constraint->info.constraint.type == PT_CONSTRAIN_NULL
		 || constraint->info.constraint.type ==
		 PT_CONSTRAIN_NOT_NULL))
	{
	  constraint = constraint->next;
	}
      if (p->info.create_entity.attr_def_list && constraint)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	}

      if (constraint)
	{
	  r1 = pt_print_bytes (parser, constraint);
	  q = pt_append_varchar (parser, q, r1);

	  constraint = constraint->next;
	  while (constraint)
	    {
	      /* keep skipping NOT_NULL constraints */
	      while (constraint
		     && (constraint->info.constraint.type == PT_CONSTRAIN_NULL
			 || constraint->info.constraint.type ==
			 PT_CONSTRAIN_NOT_NULL))
		{
		  constraint = constraint->next;
		}
	      if (constraint)
		{
		  /* Have a list */
		  r1 = pt_print_bytes (parser, constraint);
		  q = pt_append_bytes (parser, q, ", ", 2);
		  q = pt_append_varchar (parser, q, r1);

		  constraint = constraint->next;
		}
	    }
	}

      q = pt_append_nulstring (parser, q, " ) ");
    }

  if (p->info.create_entity.table_option_list)
    {
      r1 = pt_print_bytes_l (parser, p->info.create_entity.table_option_list);
      q = pt_append_nulstring (parser, q, " ");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, " ");
    }

  if (p->info.create_entity.as_query_list)
    {
      save_custom = parser->custom_print;
      parser->custom_print |= PT_PRINT_ALIAS;

      r1 = pt_print_bytes_l (parser, p->info.create_entity.as_query_list);
      q = pt_append_nulstring (parser, q, " as ");
      q = pt_append_varchar (parser, q, r1);

      parser->custom_print = save_custom;
    }

  /* this is out of date */
  if (p->info.create_entity.update)
    {
      r1 = pt_print_bytes_l (parser, p->info.create_entity.update);
      q = pt_append_nulstring (parser, q, " update ");
      q = pt_append_varchar (parser, q, r1);
    }

  if (p->info.create_entity.entity_type == PT_VCLASS)
    {
      /* the ';' is not strictly speaking ANSI */
      q = pt_append_nulstring (parser, q, ";");
    }

  return q;
}


/* CREATE_INDEX */
/*
 * pt_apply_create_index () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_create_index (PARSER_CONTEXT * parser, PT_NODE * p,
		       PT_NODE_FUNCTION g, void *arg)
{
  p->info.index.indexed_class = g (parser, p->info.index.indexed_class, arg);
  p->info.index.column_names = g (parser, p->info.index.column_names, arg);
  p->info.index.index_name = g (parser, p->info.index.index_name, arg);
  return p;
}

/*
 * pt_init_create_index () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_create_index (PT_NODE * p)
{
  p->info.index.indexed_class = NULL;
  p->info.index.column_names = NULL;
  return p;
}


/*
 * pt_print_create_index () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_create_index (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0, *r1 = 0, *r2 = 0;
  unsigned int saved_cp = parser->custom_print;

  parser->custom_print |= PT_SUPPRESS_RESOLVED;

  if (!(parser->custom_print & PT_SUPPRESS_INDEX))
    {
      r1 = pt_print_bytes (parser, p->info.index.indexed_class);

      b = pt_append_nulstring (parser, b, "create");
    }
  if (p->info.index.unique)
    {
      b = pt_append_nulstring (parser, b, " unique");
    }
  b = pt_append_nulstring (parser, b, " index");
  if (p->info.index.index_name)
    {
      const char *index_name = p->info.index.index_name->info.name.original;
      b = pt_append_nulstring (parser, b, " [");
      b = pt_append_bytes (parser, b, index_name, strlen (index_name));
      b = pt_append_nulstring (parser, b, "]");
    }

  if (!(parser->custom_print & PT_SUPPRESS_INDEX))
    {
      b = pt_append_nulstring (parser, b, " on ");
      b = pt_append_varchar (parser, b, r1);
    }

  r2 = pt_print_index_columns (parser, p);

  b = pt_append_nulstring (parser, b, " (");
  b = pt_append_varchar (parser, b, r2);
  b = pt_append_nulstring (parser, b, ") ");

  parser->custom_print = saved_cp;

  return b;
}

/* CREATE_USER */
/*
 * pt_apply_create_user () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_create_user (PARSER_CONTEXT * parser, PT_NODE * p,
		      PT_NODE_FUNCTION g, void *arg)
{
  p->info.create_user.user_name =
    g (parser, p->info.create_user.user_name, arg);
  p->info.create_user.password =
    g (parser, p->info.create_user.password, arg);
  return p;
}

/*
 * pt_init_create_user () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_create_user (PT_NODE * p)
{
  p->info.create_user.user_name = p->info.create_user.password = NULL;
  p->info.create_user.is_encrypted = false;
  return p;
}


/*
 * pt_print_create_user () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_create_user (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0, *r1;

  r1 = pt_print_bytes (parser, p->info.create_user.user_name);
  b = pt_append_nulstring (parser, b, "create user ");
  b = pt_append_varchar (parser, b, r1);

  if (p->info.create_user.password)
    {
      r1 = pt_print_bytes (parser, p->info.create_user.password);
      b = pt_append_nulstring (parser, b, " password ");
      b = pt_append_varchar (parser, b, r1);
    }

  if (p->info.create_user.is_encrypted == true)
    {
      b = pt_append_nulstring (parser, b, " encrypt");
    }

  return b;
}

/* TABLE OPTION */
/*
 * pt_apply_table_option () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_table_option (PARSER_CONTEXT * parser, PT_NODE * p,
		       PT_NODE_FUNCTION g, void *arg)
{
  p->info.table_option.val = g (parser, p->info.table_option.val, arg);
  return p;
}

/*
 * pt_init_table_option () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_table_option (PT_NODE * p)
{
  p->info.table_option.option = 0;
  p->info.table_option.val = NULL;
  return p;
}

/*
 * pt_print_table_option () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_table_option (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = NULL, *r1 = NULL;

  switch (p->info.table_option.option)
    {
    case PT_TABLE_OPTION_SHARD_KEY:
      q = pt_append_nulstring (parser, q, "shard by ");
      break;
    default:
      assert (false);
      break;
    }

  if (p->info.table_option.val != NULL)
    {
      r1 = pt_print_bytes_l (parser, p->info.table_option.val);
      q = pt_append_varchar (parser, q, r1);
    }

  return q;
}


/* DATA_DEFAULT */
/*
 * pt_apply_data_default () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_data_default (PARSER_CONTEXT * parser, PT_NODE * p,
		       PT_NODE_FUNCTION g, void *arg)
{
  p->info.data_default.default_value
    = g (parser, p->info.data_default.default_value, arg);
  return p;
}

/*
 * pt_init_data_default () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_data_default (PT_NODE * p)
{
  p->info.data_default.default_value = 0;
  p->info.data_default.default_expr = DB_DEFAULT_NONE;
  return p;
}


/*
 * pt_print_data_default () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_data_default (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;

  q = pt_append_nulstring (parser, q, " default ");

  r1 = pt_print_bytes (parser, p->info.data_default.default_value);
  if (p->info.data_default.default_value
      && PT_IS_QUERY_NODE_TYPE (p->info.data_default.default_value->
				node_type))
    {
      q = pt_append_nulstring (parser, q, "(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
    }
  else
    {
      q = pt_append_varchar (parser, q, r1);
    }

  return q;
}


/* DATA_TYPE */
/*
 * pt_apply_datatype () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_datatype (PARSER_CONTEXT * parser, PT_NODE * p,
		   PT_NODE_FUNCTION g, void *arg)
{
  p->info.data_type.entity = g (parser, p->info.data_type.entity, arg);
  p->info.data_type.virt_data_type =
    g (parser, p->info.data_type.virt_data_type, arg);
  return p;
}

/*
 * pt_init_datatype () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_datatype (PT_NODE * p)
{
  p->info.data_type.entity = 0;
  p->info.data_type.precision = 0;
  p->info.data_type.dec_scale = 0;
  p->info.data_type.collation_id = LANG_COERCIBLE_COLL;
  return p;
}

/*
 * pt_print_datatype () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_datatype (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;
  char buf[PT_MEMB_BUF_SIZE];
  bool show_collation = false;

  switch (p->type_enum)
    {
    case PT_TYPE_OBJECT:
      r1 = pt_print_bytes (parser, p->info.data_type.entity);
      if (p->info.data_type.entity)
	{
	  q = pt_append_varchar (parser, q, r1);
	}
      else
	{
	  q = pt_append_nulstring (parser, q, "object");
	}
      break;

    case PT_TYPE_NUMERIC:
      q = pt_append_nulstring (parser, q, pt_show_type_enum (p->type_enum));
      /* only show non-default datatype */
      if (p->info.data_type.precision != DB_DEFAULT_NUMERIC_PRECISION
	  || p->info.data_type.dec_scale != DB_DEFAULT_NUMERIC_SCALE)
	{
	  sprintf (buf, "(%d,%d)",
		   p->info.data_type.precision, p->info.data_type.dec_scale);
	  q = pt_append_nulstring (parser, q, buf);
	}
      break;
    case PT_TYPE_VARCHAR:
      show_collation = true;
    case PT_TYPE_VARBIT:
      {
	bool show_precision;
	int precision;

	q = pt_append_nulstring (parser, q, pt_show_type_enum (p->type_enum));

	precision = p->info.data_type.precision;

	/* variable data type:
	 * only show non-maximum(i.e., default) parameter
	 */
	show_precision = (precision == TP_FLOATING_PRECISION_VALUE)
	  ? (false)
	  : (p->type_enum == PT_TYPE_VARCHAR)
	  ? (precision != DB_MAX_VARCHAR_PRECISION)
	  : (p->type_enum == PT_TYPE_VARBIT)
	  ? (precision != DB_MAX_VARBIT_PRECISION) : (precision != 7);

	if (show_precision == true)
	  {
	    sprintf (buf, "(%d)", precision);
	    q = pt_append_nulstring (parser, q, buf);
	  }
      }
      break;
    case PT_TYPE_DOUBLE:
      q = pt_append_nulstring (parser, q, pt_show_type_enum (p->type_enum));
      break;

    default:
      q = pt_append_nulstring (parser, q, pt_show_type_enum (p->type_enum));
      if (p->data_type)
	{
	  r1 = pt_print_bytes_l (parser, p->data_type);
	  q = pt_append_nulstring (parser, q, "(");
	  q = pt_append_varchar (parser, q, r1);
	  q = pt_append_nulstring (parser, q, ")");
	}
    }

  if (show_collation
      && (p->info.data_type.collation_id != LANG_SYS_COLLATION
	  && p->info.data_type.collation_id != LANG_COERCIBLE_COLL))
    {
      sprintf (buf, " collate %s",
	       lang_get_collation_name (p->info.data_type.collation_id));
      q = pt_append_nulstring (parser, q, buf);
    }

  return q;
}


/* DELETE */
/*
 * pt_apply_delete () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_delete (PARSER_CONTEXT * parser, PT_NODE * p,
		 PT_NODE_FUNCTION g, void *arg)
{
  p->info.delete_.target_classes =
    g (parser, p->info.delete_.target_classes, arg);
  p->info.delete_.spec = g (parser, p->info.delete_.spec, arg);
  p->info.delete_.search_cond = g (parser, p->info.delete_.search_cond, arg);
  p->info.delete_.using_index = g (parser, p->info.delete_.using_index, arg);
  p->info.delete_.internal_stmts = g (parser,
				      p->info.delete_.internal_stmts, arg);
#if 0				/* TEMPORARY COMMENTED CODE: DO NOT REMOVE ME !!! */
  p->info.delete_.ordered_hint =
    g (parser, p->info.delete_.ordered_hint, arg);
#endif
  p->info.delete_.use_nl_hint = g (parser, p->info.delete_.use_nl_hint, arg);
  p->info.delete_.use_idx_hint =
    g (parser, p->info.delete_.use_idx_hint, arg);
  p->info.delete_.limit = g (parser, p->info.delete_.limit, arg);

  return p;
}

/*
 * pt_init_delete () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_delete (PT_NODE * p)
{
  p->info.delete_.hint = PT_HINT_NONE;
#if 0				/* TEMPORARY COMMENTED CODE: DO NOT REMOVE ME !!! */
  p->info.delete_.ordered_hint = NULL;
#endif
  p->info.delete_.use_nl_hint = NULL;
  p->info.delete_.use_idx_hint = NULL;
  return p;
}

/*
 * pt_print_delete () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_delete (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1, *r2;

  r1 = pt_print_bytes_l (parser, p->info.delete_.target_classes);
  r2 = pt_print_bytes_spec_list (parser, p->info.delete_.spec);

  q = pt_append_nulstring (parser, q, "delete ");
  if (p->info.delete_.hint != PT_HINT_NONE)
    {
      q = pt_append_nulstring (parser, q, "/*+");

      if (p->info.delete_.hint & PT_HINT_ORDERED)
	{
	  /* force join left-to-right */
	  q = pt_append_nulstring (parser, q, " ORDERED");
#if 0				/* TEMPORARY COMMENTED CODE: DO NOT REMOVE ME !!! */
	  if (p->info.delete_.ordered_hint)
	    {
	      r1 = pt_print_bytes_l (parser, p->info.delete_.ordered_hint);
	      q = pt_append_nulstring (parser, q, "(");
	      q = pt_append_varchar (parser, q, r1);
	      q = pt_append_nulstring (parser, q, ") ");
	    }
	  else
#endif
	    {
	      q = pt_append_nulstring (parser, q, " ");
	    }
	}

      if (p->info.delete_.hint & PT_HINT_USE_NL)
	{
	  /* force nl-join */
	  q = pt_append_nulstring (parser, q, " USE_NL");
	  if (p->info.delete_.use_nl_hint)
	    {
	      r1 = pt_print_bytes_l (parser, p->info.delete_.use_nl_hint);
	      q = pt_append_nulstring (parser, q, "(");
	      q = pt_append_varchar (parser, q, r1);
	      q = pt_append_nulstring (parser, q, ") ");
	    }
	  else
	    {
	      q = pt_append_nulstring (parser, q, " ");
	    }
	}

      if (p->info.delete_.hint & PT_HINT_USE_IDX)
	{
	  /* force idx-join */
	  q = pt_append_nulstring (parser, q, " USE_IDX");
	  if (p->info.delete_.use_idx_hint)
	    {
	      r1 = pt_print_bytes_l (parser, p->info.delete_.use_idx_hint);
	      q = pt_append_nulstring (parser, q, "(");
	      q = pt_append_varchar (parser, q, r1);
	      q = pt_append_nulstring (parser, q, ") ");
	    }
	  else
	    {
	      q = pt_append_nulstring (parser, q, " ");
	    }
	}

      if (p->info.delete_.hint & PT_HINT_USE_IDX_DESC)
	{
	  q = pt_append_nulstring (parser, q, " USE_DESC_IDX ");
	}

      if (p->info.delete_.hint & PT_HINT_NO_COVERING_IDX)
	{
	  q = pt_append_nulstring (parser, q, " NO_COVERING_IDX ");
	}

      if (p->info.delete_.hint & PT_HINT_NO_IDX_DESC)
	{
	  q = pt_append_nulstring (parser, q, " NO_DESC_IDX ");
	}

      if (p->info.delete_.hint & PT_HINT_NO_MULTI_RANGE_OPT)
	{
	  q = pt_append_nulstring (parser, q, " NO_MULTI_RANGE_OPT ");
	}

      if (p->info.delete_.hint & PT_HINT_NO_SORT_LIMIT)
	{
	  q = pt_append_nulstring (parser, q, " NO_SORT_LIMIT ");
	}

      q = pt_append_nulstring (parser, q, " */");
    }
  if (r1)
    {
      q = pt_append_nulstring (parser, q, " ");
      q = pt_append_varchar (parser, q, r1);
    }
  q = pt_append_nulstring (parser, q, " from ");
  q = pt_append_varchar (parser, q, r2);

  if (p->info.delete_.search_cond)
    {
      r1 = pt_print_and_list (parser, p->info.delete_.search_cond);
      q = pt_append_nulstring (parser, q, " where ");
      q = pt_append_varchar (parser, q, r1);
    }
  if (p->info.delete_.using_index)
    {
      if (p->info.delete_.using_index->info.name.original == NULL)
	{
	  if (p->info.delete_.using_index->info.name.resolved == NULL)
	    {
	      q = pt_append_nulstring (parser, q, " using index none");
	    }
	  else
	    {
	      if (p->info.delete_.using_index->etc
		  == (void *) PT_IDX_HINT_CLASS_NONE)
		{
		  r1 = pt_print_bytes_l (parser, p->info.delete_.using_index);
		  q = pt_append_nulstring (parser, q, " using index ");
		  q = pt_append_varchar (parser, q, r1);
		}
	      else
		{
		  r1 =
		    pt_print_bytes_l (parser,
				      p->info.delete_.using_index->next);
		  q = pt_append_nulstring (parser, q,
					   " using index all except ");
		  q = pt_append_varchar (parser, q, r1);
		}
	    }
	}
      else
	{
	  r1 = pt_print_bytes_l (parser, p->info.delete_.using_index);
	  q = pt_append_nulstring (parser, q, " using index ");
	  q = pt_append_varchar (parser, q, r1);
	}
    }
  return q;
}


/* DIFFERENCE */
/*
 * pt_apply_difference () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_difference (PARSER_CONTEXT * parser, PT_NODE * p,
		     PT_NODE_FUNCTION g, void *arg)
{
  p->info.query.q.union_.arg1 = g (parser, p->info.query.q.union_.arg1, arg);
  p->info.query.q.union_.arg2 = g (parser, p->info.query.q.union_.arg2, arg);
  p->info.query.order_by = g (parser, p->info.query.order_by, arg);
  p->info.query.orderby_for = g (parser, p->info.query.orderby_for, arg);
  return p;
}

/*
 * pt_init_difference () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_difference (PT_NODE * p)
{
  p->info.query.q.union_.arg1 = NULL;
  p->info.query.q.union_.arg2 = NULL;
  pt_init_query_info (&(p->info.query));
  return p;
}

/*
 * pt_print_difference () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_difference (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = NULL, *r1;

  r1 = pt_print_bytes (parser, p->info.query.q.union_.arg1);
  q = pt_append_nulstring (parser, q, "(");
  q = pt_append_varchar (parser, q, r1);
  q = pt_append_nulstring (parser, q, " except ");

  if (p->info.query.all_distinct == PT_ALL)
    {
      q = pt_append_nulstring (parser, q, "all ");
    }

  r1 = pt_print_bytes (parser, p->info.query.q.union_.arg2);
  q = pt_append_varchar (parser, q, r1);
  q = pt_append_nulstring (parser, q, ")");

  if (p->info.query.order_by)
    {
      r1 = pt_print_bytes_l (parser, p->info.query.order_by);
      q = pt_append_nulstring (parser, q, " order by ");
      q = pt_append_varchar (parser, q, r1);
    }

  if (p->info.query.orderby_for)
    {
      r1 = pt_print_bytes_l (parser, p->info.query.orderby_for);
      q = pt_append_nulstring (parser, q, " for ");
      q = pt_append_varchar (parser, q, r1);
    }

  if (p->info.query.limit && p->info.query.rewrite_limit)
    {
      r1 = pt_print_bytes_l (parser, p->info.query.limit);
      q = pt_append_nulstring (parser, q, " limit ");
      q = pt_append_varchar (parser, q, r1);
    }
  return q;
}


/* DOT */
/*
 * pt_apply_dot () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_dot (PARSER_CONTEXT * parser, PT_NODE * p, PT_NODE_FUNCTION g,
	      void *arg)
{
  p->info.dot.arg1 = g (parser, p->info.dot.arg1, arg);
  p->info.dot.arg2 = g (parser, p->info.dot.arg2, arg);
  return p;
}

/*
 * pt_init_dot () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_dot (PT_NODE * p)
{
  return p;
}

/*
 * pt_print_dot () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_dot (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0, *r1, *r2;

  r1 = pt_print_bytes (parser, p->info.dot.arg1);
  r2 = pt_print_bytes (parser, p->info.dot.arg2);

  b = pt_append_varchar (parser, b, r1);
  if (r2)
    {
      b = pt_append_nulstring (parser, b, ".");
      b = pt_append_varchar (parser, b, r2);
    }

  if ((parser->custom_print & PT_PRINT_ALIAS) && p->alias_print != NULL)
    {
      b = pt_append_nulstring (parser, b, " as [");
      b = pt_append_nulstring (parser, b, p->alias_print);
      b = pt_append_nulstring (parser, b, "]");
    }

  return b;
}


/* DROP_ENTITY  (not ALTER or VIEW ) */
/*
 * pt_apply_drop () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_drop (PARSER_CONTEXT * parser, PT_NODE * p, PT_NODE_FUNCTION g,
	       void *arg)
{
  p->info.drop.spec_list = g (parser, p->info.drop.spec_list, arg);
  p->info.drop.internal_stmts = g (parser, p->info.drop.internal_stmts, arg);
  return p;
}

/*
 * pt_init_drop () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_drop (PT_NODE * p)
{
  p->info.drop.spec_list = 0;
  p->info.drop.if_exists = false;
  return p;
}

/*
 * pt_print_drop () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_drop (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;
  unsigned int save_custom = parser->custom_print;

  parser->custom_print |= PT_SUPPRESS_RESOLVED;
  r1 = pt_print_bytes_l (parser, p->info.drop.spec_list);
  parser->custom_print = save_custom;

  q = pt_append_nulstring (parser, q, "drop ");
  if (p->info.drop.if_exists)
    {
      q = pt_append_nulstring (parser, q, "if exists ");
    }
  q = pt_append_varchar (parser, q, r1);

  return q;
}


/* DROP_INDEX */
/*
 * pt_apply_drop_index () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_drop_index (PARSER_CONTEXT * parser, PT_NODE * p,
		     PT_NODE_FUNCTION g, void *arg)
{
  p->info.index.indexed_class = g (parser, p->info.index.indexed_class, arg);
  p->info.index.column_names = g (parser, p->info.index.column_names, arg);

  return p;
}

/*
 * pt_init_drop_index () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_drop_index (PT_NODE * p)
{
  p->info.index.indexed_class = p->info.index.column_names = NULL;

  return p;
}

/*
 * pt_print_drop_index () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_drop_index (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0, *r1, *r2;
  const char *index_name = NULL;
  unsigned int saved_cp = parser->custom_print;

  parser->custom_print |= PT_SUPPRESS_RESOLVED;

  r1 = pt_print_bytes (parser, p->info.index.indexed_class);
  r2 = pt_print_index_columns (parser, p);

  parser->custom_print = saved_cp;

  b = pt_append_nulstring (parser, b, "drop");
  assert (p->info.index.unique == false);	/* unused at DROP */
  b = pt_append_nulstring (parser, b, " index ");
  if (p->info.index.index_name)
    {
      index_name = p->info.index.index_name->info.name.original;
      b = pt_append_bytes (parser, b, index_name, strlen (index_name));
    }
  if (r1)
    {
      b = pt_append_nulstring (parser, b, (index_name ? " on " : "on "));
      b = pt_append_varchar (parser, b, r1);
      if (r2)
	{
	  b = pt_append_nulstring (parser, b, " (");
	  b = pt_append_varchar (parser, b, r2);
	  b = pt_append_nulstring (parser, b, ") ");
	}
    }

  return b;
}

/* DROP_USER */
/*
 * pt_apply_drop_user () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_drop_user (PARSER_CONTEXT * parser, PT_NODE * p,
		    PT_NODE_FUNCTION g, void *arg)
{
  p->info.drop_user.user_name = g (parser, p->info.drop_user.user_name, arg);
  return p;
}

/*
 * pt_init_drop_user () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_drop_user (PT_NODE * p)
{
  p->info.drop_user.user_name = NULL;
  return p;
}


/*
 * pt_print_drop_user () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_drop_user (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0, *r1;

  r1 = pt_print_bytes (parser, p->info.drop_user.user_name);
  b = pt_append_nulstring (parser, b, "drop user ");
  b = pt_append_varchar (parser, b, r1);

  return b;
}

/* SPEC */
/*
 * pt_apply_spec () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_spec (PARSER_CONTEXT * parser, PT_NODE * p, PT_NODE_FUNCTION g,
	       void *arg)
{
  p->info.spec.entity_name = g (parser, p->info.spec.entity_name, arg);
  p->info.spec.derived_table = g (parser, p->info.spec.derived_table, arg);
  p->info.spec.range_var = g (parser, p->info.spec.range_var, arg);
  p->info.spec.as_attr_list = g (parser, p->info.spec.as_attr_list, arg);
  p->info.spec.referenced_attrs =
    g (parser, p->info.spec.referenced_attrs, arg);
  p->info.spec.flat_entity_list =
    g (parser, p->info.spec.flat_entity_list, arg);
  p->info.spec.on_cond = g (parser, p->info.spec.on_cond, arg);
  /* p->info.spec.using_cond = g(parser, p->info.spec.using_cond, arg);
     -- does not support named columns join */

  return p;
}

/*
 * pt_init_spec () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_spec (PT_NODE * p)
{
  p->info.spec.location = -1;
  p->info.spec.natural = false;
  p->info.spec.join_type = PT_JOIN_NONE;
  p->info.spec.on_cond = NULL;
  p->info.spec.using_cond = NULL;
  p->info.spec.auth_bypass_mask = DB_AUTH_NONE;

  return p;
}

/*
 * pt_print_spec () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_spec (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;
  unsigned int save_custom;

  if (p->info.spec.natural)
    {
      q = pt_append_nulstring (parser, q, " natural ");
    }
  switch (p->info.spec.join_type)
    {
    case PT_JOIN_NONE:
      break;
    case PT_JOIN_CROSS:
      q = pt_append_nulstring (parser, q, " cross join ");
      break;
      /*case PT_JOIN_NATURAL: -- does not support */
    case PT_JOIN_INNER:
      q = pt_append_nulstring (parser, q, " inner join ");
      break;
    case PT_JOIN_LEFT_OUTER:
      q = pt_append_nulstring (parser, q, " left outer join ");
      break;
    case PT_JOIN_RIGHT_OUTER:
      q = pt_append_nulstring (parser, q, " right outer join ");
      break;
    case PT_JOIN_FULL_OUTER:	/* not used */
      q = pt_append_nulstring (parser, q, " full outer join ");
      break;
      /*case PT_JOIN_UNION: -- does not support */
    default:
      break;
    }

  /* check if a sublist */
  if (p->info.spec.entity_name && p->info.spec.entity_name->next)
    {
      save_custom = parser->custom_print;
      parser->custom_print |= PT_SUPPRESS_RESOLVED;
      q = pt_append_nulstring (parser, q, "(");
      r1 = pt_print_bytes_l (parser, p->info.spec.entity_name);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      parser->custom_print = save_custom;
    }
  /* else is a single class entity spec */
  else if (p->info.spec.entity_name)
    {
      save_custom = parser->custom_print;

      r1 = pt_print_bytes (parser, p->info.spec.entity_name);
      q = pt_append_varchar (parser, q, r1);
      parser->custom_print = save_custom;
    }
  else
    {				/* should be a derived table */
      r1 = pt_print_bytes_l (parser, p->info.spec.derived_table);

      if (r1 != NULL)
	{
	  if (p->info.spec.derived_table_type == PT_IS_SUBQUERY
	      && r1->bytes[0] == '(' && r1->bytes[r1->length - 1] == ')')
	    {
	      /* skip unnecessary nested parenthesis of derived-query */
	      q = pt_append_varchar (parser, q, r1);
	    }
	  else
	    {
	      q = pt_append_nulstring (parser, q, "(");
	      q = pt_append_varchar (parser, q, r1);
	      q = pt_append_nulstring (parser, q, ")");
	    }
	}
    }

  if (!(parser->custom_print & PT_SUPPRESS_RESOLVED))
    {
      save_custom = parser->custom_print;
      parser->custom_print &= ~PT_PRINT_ALIAS;
      if (p->info.spec.range_var
	  && p->info.spec.range_var->info.name.original
	  && p->info.spec.range_var->info.name.original[0])
	{
	  r1 = pt_print_bytes (parser, p->info.spec.range_var);
	  q = pt_append_nulstring (parser, q, " ");
	  q = pt_append_varchar (parser, q, r1);
	}
      parser->custom_print = save_custom;
    }
  if (p->info.spec.as_attr_list)
    {
      save_custom = parser->custom_print;
      parser->custom_print |= PT_SUPPRESS_RESOLVED;
      r1 = pt_print_bytes_l (parser, p->info.spec.as_attr_list);
      q = pt_append_nulstring (parser, q, " (");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      parser->custom_print = save_custom;
    }

  if (p->info.spec.on_cond)
    {
      r1 = pt_print_and_list (parser, p->info.spec.on_cond);
      q = pt_append_nulstring (parser, q, " on ");
      q = pt_append_varchar (parser, q, r1);
    }
  if (p->info.spec.using_cond)
    {
      r1 = pt_print_and_list (parser, p->info.spec.using_cond);
      q = pt_append_nulstring (parser, q, " using ");
      q = pt_append_varchar (parser, q, r1);
    }

  return q;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * pt_print_class_name () - prints a class name from an entity_spec for
 * error messages. prints only the first entity_spec and omits its range var
 *   return:
 *   parser(in):
 *   p(in):
 */
PARSER_VARCHAR *
pt_print_class_name (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;

  /* check if a sublist */
  if (p->info.spec.entity_name && p->info.spec.entity_name->next)
    {
      r1 = pt_print_bytes (parser, p->info.spec.entity_name);
      q = pt_append_varchar (parser, q, r1);
    }
  /* else is a single class entity spec */
  else if (p->info.spec.entity_name)
    {
      r1 = pt_print_bytes (parser, p->info.spec.entity_name);
      q = pt_append_varchar (parser, q, r1);
    }
  return q;
}
#endif

/* EXPR */
/*
 * pt_apply_expr () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_expr (PARSER_CONTEXT * parser, PT_NODE * p, PT_NODE_FUNCTION g,
	       void *arg)
{
  p->info.expr.arg1 = g (parser, p->info.expr.arg1, arg);
  p->info.expr.arg2 = g (parser, p->info.expr.arg2, arg);
  p->info.expr.value = g (parser, p->info.expr.value, arg);
  p->info.expr.arg3 = g (parser, p->info.expr.arg3, arg);
  if (p->info.expr.cast_type != NULL)
    {
      /* walk cast type in case it might contain a name */
      p->info.expr.cast_type = g (parser, p->info.expr.cast_type, arg);
    }
  return p;
}

/*
 * pt_init_expr () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_expr (PT_NODE * p)
{
  p->info.expr.flag = 0;
  p->info.expr.location = 0;
  p->info.expr.recursive_type = PT_TYPE_NONE;
  return p;
}

static void
pt_print_range_op (PARSER_CONTEXT * parser, PT_STRING_BLOCK * sb, PT_NODE * t,
		   PARSER_VARCHAR * lhs)
{
  const char *op1 = NULL, *op2 = NULL;
  PARSER_VARCHAR *rhs1 = NULL, *rhs2 = NULL;

  switch (t->info.expr.op)
    {
    case PT_BETWEEN_GE_LE:
      op1 = pt_show_binopcode (PT_GE);
      op2 = pt_show_binopcode (PT_LE);
      break;
    case PT_BETWEEN_GE_LT:
      op1 = pt_show_binopcode (PT_GE);
      op2 = pt_show_binopcode (PT_LT);
      break;
    case PT_BETWEEN_GT_LE:
      op1 = pt_show_binopcode (PT_GT);
      op2 = pt_show_binopcode (PT_LE);
      break;
    case PT_BETWEEN_GT_LT:
      op1 = pt_show_binopcode (PT_GT);
      op2 = pt_show_binopcode (PT_LT);
      break;
    case PT_BETWEEN_EQ_NA:
      op1 = pt_show_binopcode (PT_EQ);
      break;
    case PT_BETWEEN_INF_LE:
      op1 = pt_show_binopcode (PT_LE);
      break;
    case PT_BETWEEN_INF_LT:
      op1 = pt_show_binopcode (PT_LT);
      break;
    case PT_BETWEEN_GT_INF:
      op1 = pt_show_binopcode (PT_GT);
      break;
    case PT_BETWEEN_GE_INF:
      op1 = pt_show_binopcode (PT_GE);
      break;

    default:
      assert (false);
      return;
    }

  rhs1 = pt_print_bytes (parser, t->info.expr.arg1);
  if (op2)
    {
      rhs2 = pt_print_bytes (parser, t->info.expr.arg2);
    }

  if (lhs && rhs1)
    {
      strcat_with_realloc (sb, (const char *) lhs->bytes);
      strcat_with_realloc (sb, (char *) op1);
      strcat_with_realloc (sb, (const char *) rhs1->bytes);

      if (rhs2)
	{
	  strcat_with_realloc (sb, " and ");
	  strcat_with_realloc (sb, (const char *) lhs->bytes);
	  strcat_with_realloc (sb, (char *) op2);
	  strcat_with_realloc (sb, (const char *) rhs2->bytes);
	}
    }
}

/*
 * pt_print_expr () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_expr (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1, *r2, *r3, *r4;
  PT_NODE *t, *or_next;
  int print_from = 0;
  PT_NODE *arg3;
  PT_NODE *between, *between_ge_lt;

  assert_release (p != p->info.expr.arg1);
  assert_release (p != p->info.expr.arg2);
  assert_release (p != p->info.expr.arg3);

  if (p->info.expr.paren_type == 1)
    {
      q = pt_append_nulstring (parser, q, "(");
    }

  switch (p->info.expr.op)
    {
    case PT_FUNCTION_HOLDER:
      /* FUNCTION_HOLDER has a PT_FUNCTION on arg1 */
      q = pt_print_function (parser, p->info.expr.arg1);
      break;
    case PT_UNARY_MINUS:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, "-");
      q = pt_append_varchar (parser, q, r1);
      break;
    case PT_BIT_NOT:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, "~");
      q = pt_append_varchar (parser, q, r1);
      break;
    case PT_BIT_COUNT:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " bit_count(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_NOT:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " not ");
      q = pt_append_varchar (parser, q, r1);
      break;
    case PT_EXISTS:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " exists ");
      q = pt_append_varchar (parser, q, r1);
      break;
    case PT_MODULUS:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " mod(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_RAND:
      q = pt_append_nulstring (parser, q, " rand(");
      if (p->info.expr.arg1 != NULL)
	{
	  r1 = pt_print_bytes (parser, p->info.expr.arg1);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_DRAND:
      q = pt_append_nulstring (parser, q, " drand(");
      if (p->info.expr.arg1 != NULL)
	{
	  r1 = pt_print_bytes (parser, p->info.expr.arg1);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_RANDOM:
      q = pt_append_nulstring (parser, q, " random(");
      if (p->info.expr.arg1 != NULL)
	{
	  r1 = pt_print_bytes (parser, p->info.expr.arg1);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_DRANDOM:
      q = pt_append_nulstring (parser, q, " drandom(");
      if (p->info.expr.arg1 != NULL)
	{
	  r1 = pt_print_bytes (parser, p->info.expr.arg1);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_FLOOR:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " floor(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_CEIL:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " ceil(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_SIGN:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " sign(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_ABS:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " abs(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_POWER:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " power(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_ROUND:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " round(");
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  q = pt_append_varchar (parser, q, r2);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_LOG:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " log(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_EXP:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " exp(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_SQRT:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " sqrt(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_TRUNC:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " trunc(");
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  q = pt_append_varchar (parser, q, r2);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_CHR:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " chr(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_INSTR:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " instr(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r2);
      if (p->info.expr.arg3 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  r3 = pt_print_bytes (parser, p->info.expr.arg3);
	  q = pt_append_varchar (parser, q, r3);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_POSITION:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " position(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, " in ");
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_FINDINSET:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " find_in_set(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_SUBSTRING:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      if (p->info.expr.qualifier == PT_SUBSTR_ORG)
	{
	  q = pt_append_nulstring (parser, q, " substring(");
	  q = pt_append_varchar (parser, q, r1);
	  q = pt_append_nulstring (parser, q, " from ");
	  q = pt_append_varchar (parser, q, r2);
	  if (p->info.expr.arg3)
	    {
	      r1 = pt_print_bytes (parser, p->info.expr.arg3);
	      q = pt_append_nulstring (parser, q, " for ");
	      q = pt_append_varchar (parser, q, r1);
	    }
	  q = pt_append_nulstring (parser, q, ")");
	}
      else if (p->info.expr.qualifier == PT_SUBSTR)
	{
	  q = pt_append_nulstring (parser, q, " substr(");
	  q = pt_append_varchar (parser, q, r1);
	  q = pt_append_nulstring (parser, q, ", ");
	  q = pt_append_varchar (parser, q, r2);
	  if (p->info.expr.arg3)
	    {
	      r1 = pt_print_bytes (parser, p->info.expr.arg3);
	      q = pt_append_nulstring (parser, q, ", ");
	      q = pt_append_varchar (parser, q, r1);
	    }
	  q = pt_append_nulstring (parser, q, ")");
	}
      break;
    case PT_SUBSTRING_INDEX:
      q = pt_append_nulstring (parser, q, "substring_index(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ", ");
      r3 = pt_print_bytes (parser, p->info.expr.arg3);
      q = pt_append_varchar (parser, q, r3);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_OCTET_LENGTH:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " octet_length(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_BIT_LENGTH:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " bit_length(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_CHAR_LENGTH:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " char_length(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_LOWER:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " lower(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_UPPER:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " upper(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_HEX:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " hex(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_ASCII:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " ascii(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_CONV:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      r3 = pt_print_bytes (parser, p->info.expr.arg3);
      q = pt_append_nulstring (parser, q, " conv(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r3);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_BIN:
      q = pt_append_nulstring (parser, q, " bin(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_MD5:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " md5(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_SHA_ONE:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " sha1(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_SHA_TWO:
      q = pt_append_nulstring (parser, q, " sha2(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_TO_BASE64:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " to_base64(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_FROM_BASE64:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " from_base64(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_EXTRACT:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " extract(");
      switch (p->info.expr.qualifier)
	{
	case PT_YEAR:
	  q = pt_append_nulstring (parser, q, "year ");
	  break;
	case PT_MONTH:
	  q = pt_append_nulstring (parser, q, "month ");
	  break;
	case PT_DAY:
	  q = pt_append_nulstring (parser, q, "day ");
	  break;
	case PT_HOUR:
	  q = pt_append_nulstring (parser, q, "hour ");
	  break;
	case PT_MINUTE:
	  q = pt_append_nulstring (parser, q, "minute ");
	  break;
	case PT_SECOND:
	  q = pt_append_nulstring (parser, q, "second ");
	  break;
	case PT_MILLISECOND:
	  q = pt_append_nulstring (parser, q, "millisecond ");
	  break;
	default:
	  break;
	}
      q = pt_append_nulstring (parser, q, " from ");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_TO_NUMBER:
      q = pt_append_nulstring (parser, q, " to_number(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      if (p->info.expr.arg2 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  r1 = pt_print_bytes (parser, p->info.expr.arg2);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_TO_DATE:
    case PT_TO_TIME:
    case PT_TO_DATETIME:
    case PT_TO_CHAR:
      {
	int flags;
	bool has_user_format = false;
	bool has_user_lang = false;
	INTL_LANG lang_id;

	if (p->info.expr.op == PT_TO_DATE)
	  {
	    q = pt_append_nulstring (parser, q, " to_date(");
	  }
	else if (p->info.expr.op == PT_TO_TIME)
	  {
	    q = pt_append_nulstring (parser, q, " to_time(");
	  }
	else if (p->info.expr.op == PT_TO_DATETIME)
	  {
	    q = pt_append_nulstring (parser, q, " to_datetime(");
	  }
	else if (p->info.expr.op == PT_TO_CHAR)
	  {
	    q = pt_append_nulstring (parser, q, " to_char(");
	  }
	else
	  {
	    assert (false);
	  }

	r1 = pt_print_bytes (parser, p->info.expr.arg1);
	q = pt_append_varchar (parser, q, r1);

	flags = p->info.expr.arg3->info.value.data_value.i;
	lang_id = lang_get_lang_id_from_flag (flags, &has_user_format,
					      &has_user_lang);
	if (has_user_format)
	  {
	    const char *lang_name = lang_get_lang_name_from_id (lang_id);

	    q = pt_append_nulstring (parser, q, ", ");
	    r1 = pt_print_bytes (parser, p->info.expr.arg2);
	    q = pt_append_varchar (parser, q, r1);

	    if (lang_name != NULL && has_user_lang)
	      {
		q = pt_append_nulstring (parser, q, ", '");
		q = pt_append_nulstring (parser, q, lang_name);
		q = pt_append_nulstring (parser, q, "'");
	      }
	  }
	q = pt_append_nulstring (parser, q, ")");
      }
      break;

    case PT_SYS_DATE:
      q = pt_append_nulstring (parser, q, " SYS_DATE ");
      break;

    case PT_SYS_TIME:
      q = pt_append_nulstring (parser, q, " SYS_TIME ");
      break;

    case PT_SYS_DATETIME:
      q = pt_append_nulstring (parser, q, " SYS_DATETIME ");
      break;

    case PT_UTC_TIME:
      q = pt_append_nulstring (parser, q, " utc_time() ");
      break;

    case PT_UTC_DATE:
      q = pt_append_nulstring (parser, q, " utc_date() ");
      break;

    case PT_CURRENT_USER:
      q = pt_append_nulstring (parser, q, " CURRENT_USER ");
      break;

    case PT_USER:
      q = pt_append_nulstring (parser, q, " user() ");
      break;

    case PT_MONTHS_BETWEEN:
      q = pt_append_nulstring (parser, q, " months_between(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_ADDDATE:
      q = pt_append_nulstring (parser, q, " adddate(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_SUBDATE:
      q = pt_append_nulstring (parser, q, " subdate(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DATE_ADD:
    case PT_DATE_SUB:
      if (p->info.expr.op == PT_DATE_ADD)
	{
	  q = pt_append_nulstring (parser, q, " date_add(");
	}
      else if (p->info.expr.op == PT_DATE_SUB)
	{
	  q = pt_append_nulstring (parser, q, " date_sub(");
	}

      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", INTERVAL ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);

      switch (p->info.expr.arg3->info.expr.qualifier)
	{
	case PT_MILLISECOND:
	  q = pt_append_nulstring (parser, q, " MILLISECOND");
	  break;

	case PT_SECOND:
	  q = pt_append_nulstring (parser, q, " SECOND");
	  break;

	case PT_MINUTE:
	  q = pt_append_nulstring (parser, q, " MINUTE");
	  break;

	case PT_HOUR:
	  q = pt_append_nulstring (parser, q, " HOUR");
	  break;

	case PT_DAY:
	  q = pt_append_nulstring (parser, q, " DAY");
	  break;

	case PT_WEEK:
	  q = pt_append_nulstring (parser, q, " WEEK");
	  break;

	case PT_MONTH:
	  q = pt_append_nulstring (parser, q, " MONTH");
	  break;

	case PT_QUARTER:
	  q = pt_append_nulstring (parser, q, " QUARTER");
	  break;

	case PT_YEAR:
	  q = pt_append_nulstring (parser, q, " YEAR");
	  break;

	case PT_SECOND_MILLISECOND:
	  q = pt_append_nulstring (parser, q, " SECOND_MILLISECOND");
	  break;

	case PT_MINUTE_MILLISECOND:
	  q = pt_append_nulstring (parser, q, " MINUTE_MILLISECOND");
	  break;

	case PT_MINUTE_SECOND:
	  q = pt_append_nulstring (parser, q, " MINUTE_SECOND");
	  break;

	case PT_HOUR_MILLISECOND:
	  q = pt_append_nulstring (parser, q, " HOUR_MILLISECOND");
	  break;

	case PT_HOUR_SECOND:
	  q = pt_append_nulstring (parser, q, " HOUR_SECOND");
	  break;

	case PT_HOUR_MINUTE:
	  q = pt_append_nulstring (parser, q, " HOUR_MINUTE");
	  break;

	case PT_DAY_MILLISECOND:
	  q = pt_append_nulstring (parser, q, " DAY_MILLISECOND");
	  break;

	case PT_DAY_SECOND:
	  q = pt_append_nulstring (parser, q, " DAY_SECOND");
	  break;

	case PT_DAY_MINUTE:
	  q = pt_append_nulstring (parser, q, " DAY_MINUTE");
	  break;

	case PT_DAY_HOUR:
	  q = pt_append_nulstring (parser, q, " DAY_HOUR");
	  break;

	case PT_YEAR_MONTH:
	  q = pt_append_nulstring (parser, q, " YEAR_MONTH");
	  break;

	default:
	  break;
	}

      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_ATAN:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " atan(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_ATAN2:
      q = pt_append_nulstring (parser, q, " atan2(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_FORMAT:
      q = pt_append_nulstring (parser, q, " format(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DATE_FORMAT:
      q = pt_append_nulstring (parser, q, " date_format(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_STR_TO_DATE:
      q = pt_append_nulstring (parser, q, " str_to_date(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_LAST_DAY:
      q = pt_append_nulstring (parser, q, " last_day(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_TIME_FORMAT:
      q = pt_append_nulstring (parser, q, " time_format(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_UNIX_TIMESTAMP:
      q = pt_append_nulstring (parser, q, " unix_timestamp(");
      if (p->info.expr.arg1)
	{
	  r1 = pt_print_bytes (parser, p->info.expr.arg1);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_YEARF:
      q = pt_append_nulstring (parser, q, " year(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_MONTHF:
      q = pt_append_nulstring (parser, q, " month(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DAYF:
      q = pt_append_nulstring (parser, q, " day(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DAYOFMONTH:
      q = pt_append_nulstring (parser, q, " dayofmonth(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_HOURF:
      q = pt_append_nulstring (parser, q, " hour(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_MINUTEF:
      q = pt_append_nulstring (parser, q, " minute(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_SECONDF:
      q = pt_append_nulstring (parser, q, " second(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_QUARTERF:
      q = pt_append_nulstring (parser, q, " quarter(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_WEEKDAY:
      q = pt_append_nulstring (parser, q, " weekday(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DAYOFWEEK:
      q = pt_append_nulstring (parser, q, " dayofweek(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DAYOFYEAR:
      q = pt_append_nulstring (parser, q, " dayofyear(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_TODAYS:
      q = pt_append_nulstring (parser, q, " to_days(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_FROMDAYS:
      q = pt_append_nulstring (parser, q, " from_days(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_TIMETOSEC:
      q = pt_append_nulstring (parser, q, " time_to_sec(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_SECTOTIME:
      q = pt_append_nulstring (parser, q, " sec_to_time(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_MAKEDATE:
      q = pt_append_nulstring (parser, q, " makedate(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_MAKETIME:
      q = pt_append_nulstring (parser, q, " maketime(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ", ");
      r3 = pt_print_bytes (parser, p->info.expr.arg3);
      q = pt_append_varchar (parser, q, r3);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_WEEKF:
      q = pt_append_nulstring (parser, q, " week(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_REPLACE:
      q = pt_append_nulstring (parser, q, " replace(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);

      if (p->info.expr.arg3 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  r1 = pt_print_bytes (parser, p->info.expr.arg3);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_REPEAT:
      q = pt_append_nulstring (parser, q, " repeat(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_SPACE:
      q = pt_append_nulstring (parser, q, " space(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_TRANSLATE:
      q = pt_append_nulstring (parser, q, " translate(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg3);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_IF:
      q = pt_append_nulstring (parser, q, " if(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg3);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_IFNULL:
      q = pt_append_nulstring (parser, q, " ifnull(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_ISNULL:
      q = pt_append_nulstring (parser, q, " isnull(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_COS:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " cos(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_SIN:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " sin(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_TAN:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " tan(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DATEF:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " date(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_TIMEF:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " time(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DEFAULTF:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " default(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DEGREES:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " degrees(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_RADIANS:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " radians(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_SCHEMA:
      q = pt_append_nulstring (parser, q, " schema()");
      break;

    case PT_DATABASE:
      q = pt_append_nulstring (parser, q, " database()");
      break;

    case PT_VERSION:
      q = pt_append_nulstring (parser, q, " version()");
      break;

    case PT_PI:
      q = pt_append_nulstring (parser, q, " pi()");
      break;

    case PT_COT:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " cot(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_ACOS:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " acos(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_ASIN:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " asin(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_LN:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " ln(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_LOG2:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " log2(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_LOG10:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " log10(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_CONCAT:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, " concat(");
	}
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2)
	{
	  r2 = pt_print_bytes (parser, p->info.expr.arg2);
	  if (r2)
	    {
	      q = pt_append_nulstring (parser, q, ", ");
	      q = pt_append_varchar (parser, q, r2);
	    }
	}
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, ")");
	}
      break;

    case PT_CONCAT_WS:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, " concat_ws(");
	  assert (p->info.expr.arg3 != NULL);
	  assert (p->info.expr.arg3->next == NULL);
	  r3 = pt_print_bytes (parser, p->info.expr.arg3);
	  q = pt_append_varchar (parser, q, r3);
	  q = pt_append_nulstring (parser, q, ", ");
	}
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2)
	{
	  r2 = pt_print_bytes (parser, p->info.expr.arg2);
	  if (r2)
	    {
	      q = pt_append_nulstring (parser, q, ", ");
	      q = pt_append_varchar (parser, q, r2);
	    }
	}
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, ")");
	}
      break;

    case PT_FIELD:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, " field(");
	  r3 = pt_print_bytes (parser, p->info.expr.arg3);
	  q = pt_append_varchar (parser, q, r3);
	  q = pt_append_nulstring (parser, q, ", ");
	}
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 && p->info.expr.arg2->is_hidden_column == 0)
	{
	  r2 = pt_print_bytes (parser, p->info.expr.arg2);
	  if (r2)
	    {
	      q = pt_append_nulstring (parser, q, ", ");
	      q = pt_append_varchar (parser, q, r2);
	    }
	}
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, ")");
	}
      break;

    case PT_LEFT:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " left(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_RIGHT:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " right(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_LOCATE:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_nulstring (parser, q, " locate(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r2);
      if (p->info.expr.arg3)
	{
	  r1 = pt_print_bytes (parser, p->info.expr.arg3);
	  q = pt_append_nulstring (parser, q, ", ");
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_MID:
      q = pt_append_nulstring (parser, q, " mid(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ", ");
      r3 = pt_print_bytes (parser, p->info.expr.arg3);
      q = pt_append_varchar (parser, q, r3);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_STRCMP:
      q = pt_append_nulstring (parser, q, " strcmp(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DATEDIFF:
      q = pt_append_nulstring (parser, q, " datediff(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_TIMEDIFF:
      q = pt_append_nulstring (parser, q, " timediff(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_REVERSE:
      q = pt_append_nulstring (parser, q, " reverse(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_LPAD:
      q = pt_append_nulstring (parser, q, " lpad(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);

      if (p->info.expr.arg3 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  r1 = pt_print_bytes (parser, p->info.expr.arg3);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_RPAD:
      q = pt_append_nulstring (parser, q, " rpad(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);

      if (p->info.expr.arg3 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  r1 = pt_print_bytes (parser, p->info.expr.arg3);
	  q = pt_append_varchar (parser, q, r1);
	}

      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_LTRIM:
      q = pt_append_nulstring (parser, q, " ltrim(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  r1 = pt_print_bytes (parser, p->info.expr.arg2);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_RTRIM:
      q = pt_append_nulstring (parser, q, " rtrim(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  r1 = pt_print_bytes (parser, p->info.expr.arg2);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_LIKE_LOWER_BOUND:
      q = pt_append_nulstring (parser, q, " like_match_lower_bound(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  r1 = pt_print_bytes (parser, p->info.expr.arg2);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_LIKE_UPPER_BOUND:
      q = pt_append_nulstring (parser, q, " like_match_upper_bound(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  r1 = pt_print_bytes (parser, p->info.expr.arg2);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_FROM_UNIXTIME:
      q = pt_append_nulstring (parser, q, " from_unixtime(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 != NULL)
	{
	  q = pt_append_nulstring (parser, q, ", ");
	  r1 = pt_print_bytes (parser, p->info.expr.arg2);
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_TRIM:
      q = pt_append_nulstring (parser, q, " trim(");
      switch (p->info.expr.qualifier)
	{
	case PT_LEADING:
	  q = pt_append_nulstring (parser, q, "leading ");
	  print_from = 1;
	  break;
	case PT_TRAILING:
	  q = pt_append_nulstring (parser, q, "trailing ");
	  print_from = 1;
	  break;
	case PT_BOTH:
	  q = pt_append_nulstring (parser, q, "both ");
	  print_from = 1;
	  break;
	default:
	  break;
	}

      if (p->info.expr.arg2)
	{
	  r1 = pt_print_bytes (parser, p->info.expr.arg2);
	  q = pt_append_varchar (parser, q, r1);
	}
      if (p->info.expr.arg2 || print_from)
	{
	  q = pt_append_nulstring (parser, q, " from ");
	}
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_CAST:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.cast_type);
      q = pt_append_nulstring (parser, q, " cast(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, " as ");
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_CASE:
      switch (p->info.expr.qualifier)
	{
	case PT_SIMPLE_CASE:
	  arg3 = p->info.expr.arg3;

	  assert (arg3->node_type == PT_EXPR || arg3->node_type == PT_VALUE);

	  if (arg3->node_type == PT_EXPR)
	    {
	      r1 = pt_print_bytes (parser, arg3->info.expr.arg1);
	      r2 = pt_print_bytes (parser, arg3->info.expr.arg2);
	    }
	  else
	    {
	      r2 = r1 = pt_print_bytes (parser, arg3);
	    }
	  r3 = pt_print_bytes (parser, p->info.expr.arg1);
	  r4 = (PT_IS_NULL_NODE (p->info.expr.arg2)) ? NULL
	    : pt_print_bytes (parser, p->info.expr.arg2);

	  if (!p->info.expr.continued_case)
	    {
	      q = pt_append_nulstring (parser, q, "case ");
	      q = pt_append_varchar (parser, q, r1);
	    }
	  q = pt_append_nulstring (parser, q, " when ");
	  q = pt_append_varchar (parser, q, r2);
	  q = pt_append_nulstring (parser, q, " then ");
	  q = pt_append_varchar (parser, q, r3);
	  if (r4)
	    {
	      if (p->info.expr.arg2->node_type != PT_EXPR
		  || p->info.expr.arg2->info.expr.op != PT_CASE
		  || !p->info.expr.arg2->info.expr.continued_case)
		q = pt_append_nulstring (parser, q, " else ");
	      q = pt_append_varchar (parser, q, r4);
	    }
	  if (!p->info.expr.continued_case)
	    {
	      q = pt_append_nulstring (parser, q, " end");
	    }
	  break;
	case PT_SEARCHED_CASE:
	  r1 = pt_print_bytes (parser, p->info.expr.arg3);
	  r2 = pt_print_bytes (parser, p->info.expr.arg1);
	  r3 = (p->info.expr.arg2 == NULL
		|| PT_IS_NULL_NODE (p->info.expr.arg2)) ? NULL
	    : pt_print_bytes (parser, p->info.expr.arg2);

	  if (!p->info.expr.continued_case)
	    {
	      q = pt_append_nulstring (parser, q, "case");
	    }
	  q = pt_append_nulstring (parser, q, " when ");
	  q = pt_append_varchar (parser, q, r1);
	  q = pt_append_nulstring (parser, q, " then ");
	  q = pt_append_varchar (parser, q, r2);
	  if (r3)
	    {
	      if (p->info.expr.arg2->node_type != PT_EXPR
		  || p->info.expr.arg2->info.expr.op != PT_CASE
		  || !p->info.expr.arg2->info.expr.continued_case)
		{
		  q = pt_append_nulstring (parser, q, " else ");
		}
	      q = pt_append_varchar (parser, q, r3);
	    }
	  if (!p->info.expr.continued_case)
	    {
	      q = pt_append_nulstring (parser, q, " end");
	    }
	  break;
	default:
	  break;
	}
      break;

    case PT_NULLIF:
      q = pt_append_nulstring (parser, q, "nullif(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_COALESCE:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, "coalesce(");
	}
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 && p->info.expr.arg2->is_hidden_column == 0)
	{
	  r2 = pt_print_bytes (parser, p->info.expr.arg2);
	  if (r2)
	    {
	      q = pt_append_nulstring (parser, q, ", ");
	      q = pt_append_varchar (parser, q, r2);
	    }
	}
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, ")");
	}
      break;

    case PT_NVL:
      q = pt_append_nulstring (parser, q, "nvl(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_NVL2:
      q = pt_append_nulstring (parser, q, "nvl2(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");
      r2 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ", ");
      r3 = pt_print_bytes (parser, p->info.expr.arg3);
      q = pt_append_varchar (parser, q, r3);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_DECODE:
      arg3 = p->info.expr.arg3;

      assert (arg3->node_type == PT_EXPR || arg3->node_type == PT_VALUE);

      if (arg3->node_type == PT_EXPR)
	{
	  r1 = pt_print_bytes (parser, arg3->info.expr.arg1);
	  r2 = pt_print_bytes (parser, arg3->info.expr.arg2);
	}
      else
	{
	  r2 = r1 = pt_print_bytes (parser, arg3);
	}
      r3 = pt_print_bytes (parser, p->info.expr.arg1);
      r4 = ((p->info.expr.arg2 == NULL
	     || PT_IS_NULL_NODE (p->info.expr.arg2))
	    ? NULL : pt_print_bytes (parser, p->info.expr.arg2));
      if (!p->info.expr.continued_case)
	{
	  q = pt_append_nulstring (parser, q, "decode(");
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ", ");
      q = pt_append_varchar (parser, q, r3);
      if (r4)
	{
	  if (p->info.expr.arg2->node_type != PT_EXPR
	      || p->info.expr.arg2->info.expr.op != PT_DECODE
	      || !p->info.expr.arg2->info.expr.continued_case)
	    {
	      q = pt_append_nulstring (parser, q, ", ");
	    }
	  q = pt_append_varchar (parser, q, r4);
	}
      if (!p->info.expr.continued_case)
	{
	  q = pt_append_nulstring (parser, q, ")");
	}
      break;

    case PT_LEAST:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, "least(");
	}
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 && p->info.expr.arg2->is_hidden_column == 0)
	{
	  r2 = pt_print_bytes (parser, p->info.expr.arg2);
	  if (r2)
	    {
	      q = pt_append_nulstring (parser, q, ", ");
	      q = pt_append_varchar (parser, q, r2);
	    }
	}
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, ")");
	}
      break;

    case PT_GREATEST:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, "greatest(");
	}
      q = pt_append_varchar (parser, q, r1);
      if (p->info.expr.arg2 && p->info.expr.arg2->is_hidden_column == 0)
	{
	  r2 = pt_print_bytes (parser, p->info.expr.arg2);
	  if (r2)
	    {
	      q = pt_append_nulstring (parser, q, ", ");
	      q = pt_append_varchar (parser, q, r2);
	    }
	}
      if (p->info.expr.continued_case == 1)
	{
	  q = pt_append_nulstring (parser, q, ")");
	}
      break;

    case PT_BETWEEN_INF_LE:
    case PT_BETWEEN_INF_LT:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, "min");
      q = pt_append_nulstring (parser, q,
			       pt_show_binopcode (p->info.expr.op));
      q = pt_append_varchar (parser, q, r1);
      break;
    case PT_BETWEEN_GE_INF:
    case PT_BETWEEN_GT_INF:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q,
			       pt_show_binopcode (p->info.expr.op));
      q = pt_append_nulstring (parser, q, "max");
      break;

    case PT_INST_NUM:
      q = pt_append_nulstring (parser, q, "inst_num()");
      break;

    case PT_ROWNUM:
      q = pt_append_nulstring (parser, q, "rownum");
      break;

    case PT_ORDERBY_NUM:
      q = pt_append_nulstring (parser, q, "orderby_num()");
      break;

    case PT_LIST_DBS:
      q = pt_append_nulstring (parser, q, " list_dbs() ");
      break;

    case PT_TYPEOF:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " typeof(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_INDEX_CARDINALITY:
      q = pt_append_nulstring (parser, q, " index_cardinality(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg3);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ")");
      break;

    case PT_RANGE:
      if (parser->custom_print & PT_CONVERT_RANGE)
	{
	  PT_STRING_BLOCK sb;
	  sb.length = 0;
	  sb.size = 1024;
	  sb.body = NULL;

	  sb.body = malloc (sb.size);
	  if (sb.body == NULL)
	    {
	      return NULL;
	    }

	  sb.body[0] = 0;

	  r4 = pt_print_bytes (parser, p->info.expr.arg1);

	  if (p->info.expr.arg2 && p->info.expr.arg2->or_next)
	    {
	      strcat_with_realloc (&sb, "(");
	    }

	  for (t = p->info.expr.arg2; t; t = t->or_next)
	    {
	      if (!p->info.expr.paren_type)
		{
		  strcat_with_realloc (&sb, "(");
		}

	      pt_print_range_op (parser, &sb, t, r4);

	      if (!p->info.expr.paren_type)
		{
		  strcat_with_realloc (&sb, ")");
		}

	      if (t->or_next)
		{
		  strcat_with_realloc (&sb, " or ");
		}
	    }
	  if (p->info.expr.arg2 && p->info.expr.arg2->or_next)
	    {
	      strcat_with_realloc (&sb, ")");
	    }

	  q = pt_append_nulstring (parser, q, sb.body);
	  free (sb.body);

	  /* break case PT_RANGE */
	  break;
	}
      /* fall through to default case */
    default:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);

      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q,
			       pt_show_binopcode (p->info.expr.op));
      if (r2 && (r2->bytes[0] == '-')
	  && q && (q->bytes[q->length - 1] == '-'))
	{
	  q = pt_append_nulstring (parser, q, "(");
	  q = pt_append_varchar (parser, q, r2);
	  q = pt_append_nulstring (parser, q, ")");
	}
      if (p->info.expr.op == PT_RANGE)
	{
	  q = pt_append_nulstring (parser, q, "(");
	  q = pt_append_varchar (parser, q, r2);
	  q = pt_append_nulstring (parser, q, ")");
	}
      else
	{
	  q = pt_append_varchar (parser, q, r2);
	}
      break;
    case PT_INET_ATON:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " inet_aton(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_INET_NTOA:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_nulstring (parser, q, " inet_ntoa(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_WIDTH_BUCKET:
      q = pt_append_nulstring (parser, q, "width_bucket(");

      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ", ");

      /* we use PT_BETWEEN and PT_BETWEEN_GE_LT to represent the boundaries */
      between = p->info.expr.arg2;
      if (between == NULL
	  || between->node_type != PT_EXPR
	  || between->info.expr.op != PT_BETWEEN)
	{
	  return NULL;
	}

      between_ge_lt = between->info.expr.arg2;
      if (between_ge_lt == NULL
	  || between_ge_lt->node_type != PT_EXPR
	  || between_ge_lt->info.expr.op != PT_BETWEEN_GE_LT)
	{
	  return NULL;
	}

      r2 = pt_print_bytes (parser, between_ge_lt->info.expr.arg1);
      q = pt_append_varchar (parser, q, r2);
      q = pt_append_nulstring (parser, q, ", ");

      r3 = pt_print_bytes (parser, between_ge_lt->info.expr.arg2);
      q = pt_append_varchar (parser, q, r3);
      q = pt_append_nulstring (parser, q, ", ");

      r4 = pt_print_bytes (parser, p->info.expr.arg3);
      q = pt_append_varchar (parser, q, r4);

      q = pt_append_nulstring (parser, q, ")");

      break;
    case PT_TRACE_STATS:
      q = pt_append_nulstring (parser, q, " trace_stats()");
      break;
    case PT_LIKE_ESCAPE:
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      r2 = pt_print_bytes (parser, p->info.expr.arg2);

      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q,
			       pt_show_binopcode (p->info.expr.op));
      q = pt_append_varchar (parser, q, r2);
      break;
    case PT_INDEX_PREFIX:
      q = pt_append_nulstring (parser, q, " index_prefix(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg2);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ", ");
      r1 = pt_print_bytes (parser, p->info.expr.arg3);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ")");
      break;
    case PT_HA_STATUS:
      q = pt_append_nulstring (parser, q, " ha_status()");
      break;
    case PT_SHARD_GROUPID:
      q = pt_append_nulstring (parser, q, " shard_groupid()");
      break;
    case PT_SHARD_LOCKNAME:
      q = pt_append_nulstring (parser, q, " shard_lockname()");
      break;
    case PT_SHARD_NODEID:
      q = pt_append_nulstring (parser, q, " shard_nodeid()");
      break;
    case PT_EXPLAIN:
      q = pt_append_nulstring (parser, q, " explain(");
      r1 = pt_print_bytes (parser, p->info.expr.arg1);
      q = pt_append_varchar (parser, q, r1);

      q = pt_append_nulstring (parser, q, ")");
      break;
    }

  for (t = p->or_next; t; t = t->or_next)
    {
      or_next = t->or_next;
      t->or_next = NULL;
      r1 = pt_print_bytes (parser, t);
      if (r1)
	{
	  q = pt_append_nulstring (parser, q, " or ");
	  q = pt_append_varchar (parser, q, r1);
	}
      t->or_next = or_next;
    }

  if (p->info.expr.paren_type == 1)
    {
      q = pt_append_nulstring (parser, q, ")");
    }

  if ((parser->custom_print & PT_PRINT_ALIAS) && p->alias_print != NULL)
    {
      q = pt_append_nulstring (parser, q, " as [");
      q = pt_append_nulstring (parser, q, p->alias_print);
      q = pt_append_nulstring (parser, q, "]");
    }

  return q;
}


/* FUNCTION */
/*
 * pt_apply_function () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_function (PARSER_CONTEXT * parser, PT_NODE * p,
		   PT_NODE_FUNCTION g, void *arg)
{
  p->info.function.arg_list = g (parser, p->info.function.arg_list, arg);
  p->info.function.order_by = g (parser, p->info.function.order_by, arg);
  return p;
}

/*
 * pt_init_function () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_function (PT_NODE * p)
{
  p->info.function.function_type = (FUNC_TYPE) 0;
  p->info.function.arg_list = 0;
  p->info.function.all_or_distinct = (PT_MISC_TYPE) 0;
  p->info.function.generic_name = 0;
  p->info.function.order_by = NULL;
  return p;
}

/*
 * pt_print_function () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_function (PARSER_CONTEXT * parser, PT_NODE * p)
{
  FUNC_TYPE code;
  PARSER_VARCHAR *q = 0, *r1;

  code = p->info.function.function_type;
  if (code == PT_GENERIC)
    {
      r1 = pt_print_bytes_l (parser, p->info.function.arg_list);
      q = pt_append_nulstring (parser, q, p->info.function.generic_name);
      q = pt_append_nulstring (parser, q, "(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
    }
  else if (code < PT_TOP_AGG_FUNC)
    {
      q = pt_append_nulstring (parser, q, pt_show_function (code));
      q = pt_append_nulstring (parser, q, "(");

      if (code == PT_COUNT_STAR)
	{
	  q = pt_append_nulstring (parser, q, "*");
	}
      else
	{
	  if (code == PT_GROUP_CONCAT)
	    {
	      r1 = pt_print_bytes (parser, p->info.function.arg_list);
	      if (p->info.function.order_by != NULL)
		{
		  PARSER_VARCHAR *r2;

		  r2 = pt_print_bytes_l (parser, p->info.function.order_by);
		  r1 = pt_append_nulstring (parser, r1, " order by ");
		  r1 = pt_append_varchar (parser, r1, r2);
		}
	      /* SEPARATOR */
	      if (p->info.function.arg_list->next != NULL)
		{
		  PARSER_VARCHAR *r2;
		  /* print separator */
		  r1 = pt_append_nulstring (parser, r1, " separator ");
		  r2 = pt_print_bytes (parser,
				       p->info.function.arg_list->next);
		  r1 = pt_append_varchar (parser, r1, r2);
		}
	    }
	  else
	    {
	      r1 = pt_print_bytes_l (parser, p->info.function.arg_list);
	    }
	  if (p->info.function.all_or_distinct == PT_DISTINCT)
	    {
	      q = pt_append_nulstring (parser, q, "distinct ");
	    }
	  q = pt_append_varchar (parser, q, r1);
	}
      q = pt_append_nulstring (parser, q, ")");
    }
  else if (code == F_SEQUENCE)
    {
#if 0				/* TODO - */
      assert (false);		/* should not reach here */
#endif
      if (p->spec_ident)
	{
	  /* this is tagged as an "in" clause right hand side
	   * Print it as a parenthesized list */
	  r1 = pt_print_bytes_l (parser, p->info.function.arg_list);
	  q = pt_append_nulstring (parser, q, "(");
	  q = pt_append_varchar (parser, q, r1);
	  q = pt_append_nulstring (parser, q, ")");
	}
      else
	{
	  r1 = pt_print_bytes_l (parser, p->info.function.arg_list);
	  if (code != F_SEQUENCE)
	    {
	      q = pt_append_nulstring (parser, q, pt_show_function (code));
	    }
	  q = pt_append_nulstring (parser, q, "{");
	  q = pt_append_varchar (parser, q, r1);
	  q = pt_append_nulstring (parser, q, "}");
	}
    }
  else
    {
      r1 = pt_print_bytes_l (parser, p->info.function.arg_list);
      q = pt_append_nulstring (parser, q, pt_show_function (code));
      q = pt_append_nulstring (parser, q, "(");
      q = pt_append_varchar (parser, q, r1);
      q = pt_append_nulstring (parser, q, ")");
    }

  if ((parser->custom_print & PT_PRINT_ALIAS) && p->alias_print != NULL)
    {
      q = pt_append_nulstring (parser, q, " as [");
      q = pt_append_nulstring (parser, q, p->alias_print);
      q = pt_append_nulstring (parser, q, "]");
    }

  return q;
}

/* GET_OPTIMIZATION_LEVEL */
/*
 * pt_apply_get_opt_lvl () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_get_opt_lvl (PARSER_CONTEXT * parser, PT_NODE * p,
		      PT_NODE_FUNCTION g, void *arg)
{
  p->info.get_opt_lvl.args = g (parser, p->info.get_opt_lvl.args, arg);
  return p;
}

/*
 * pt_init_get_opt_lvl () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_get_opt_lvl (PT_NODE * p)
{
  p->info.get_opt_lvl.option = PT_OPT_LVL;
  return p;
}

/*
 * pt_print_get_opt_lvl () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_get_opt_lvl (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL, *r1;
  PT_MISC_TYPE option;

  option = p->info.get_opt_lvl.option;
  b = pt_append_nulstring (parser, b, "get optimization ");
  b = pt_append_nulstring (parser, b, pt_show_misc_type (option));

  if (p->info.get_opt_lvl.args)
    {
      r1 = pt_print_bytes (parser, p->info.get_opt_lvl.args);
      b = pt_append_nulstring (parser, b, " ");
      b = pt_append_varchar (parser, b, r1);
    }

  return b;
}

/* GET_XACTION */
/*
 * pt_apply_get_xaction () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_get_xaction (UNUSED_ARG PARSER_CONTEXT * parser, PT_NODE * p,
		      UNUSED_ARG PT_NODE_FUNCTION g, UNUSED_ARG void *arg)
{
  return p;
}

/*
 * pt_init_get_xaction () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_get_xaction (PT_NODE * p)
{
  return (p);
}

/*
 * pt_print_get_xaction () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_get_xaction (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL;

  b = pt_append_nulstring (parser, b, "get transaction ");
  b = pt_append_nulstring (parser, b,
			   pt_show_misc_type (p->info.get_xaction.option));

  return b;
}

/* GRANT */
/*
 * pt_apply_grant () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_grant (PARSER_CONTEXT * parser, PT_NODE * p,
		PT_NODE_FUNCTION g, void *arg)
{
  p->info.grant.auth_cmd_list = g (parser, p->info.grant.auth_cmd_list, arg);
  p->info.grant.user_list = g (parser, p->info.grant.user_list, arg);
  p->info.grant.spec_list = g (parser, p->info.grant.spec_list, arg);
  return p;
}

/*
 * pt_init_grant () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_grant (PT_NODE * p)
{
  p->info.grant.auth_cmd_list = 0;
  p->info.grant.user_list = 0;
  p->info.grant.spec_list = 0;
  p->info.grant.grant_option = (PT_MISC_TYPE) 0;

  return (p);
}

/*
 * pt_print_grant () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_grant (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1, *r2, *r3;
  unsigned int save_custom;

  r1 = pt_print_bytes_l (parser, p->info.grant.auth_cmd_list);
  save_custom = parser->custom_print;
  parser->custom_print |= PT_SUPPRESS_RESOLVED;
  r2 = pt_print_bytes_l (parser, p->info.grant.spec_list);
  parser->custom_print = save_custom;
  r3 = pt_print_bytes_l (parser, p->info.grant.user_list);
  q = pt_append_nulstring (parser, q, "grant ");
  q = pt_append_varchar (parser, q, r1);
  q = pt_append_nulstring (parser, q, " on ");
  q = pt_append_varchar (parser, q, r2);
  q = pt_append_nulstring (parser, q, " to ");
  q = pt_append_varchar (parser, q, r3);

  if (p->info.grant.grant_option == PT_GRANT_OPTION)
    {
      q = pt_append_nulstring (parser, q, " with grant option ");
    }

  return q;
}

/* HOST_VAR */
/*
 * pt_apply_host_var () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_host_var (UNUSED_ARG PARSER_CONTEXT * parser, PT_NODE * p,
		   UNUSED_ARG PT_NODE_FUNCTION g, UNUSED_ARG void *arg)
{
  return p;
}

/*
 * pt_init_host_var () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_host_var (PT_NODE * p)
{
  return p;
}

/*
 * pt_print_host_var () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_host_var (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PT_NODE *t, *or_next;
  PARSER_VARCHAR *q = NULL, *r;
  char s[PT_MEMB_BUF_SIZE];

  if (parser->print_db_value)
    {
      if (p->info.host_var.var_type == PT_HOST_IN)
	{
	  PT_NODE *save_error_msgs;

	  /* keep previous error, and print value
	   * if error occurs, reset and go ahead
	   * for example:
	   * curently, it is impossiable to occurs
	   * anyway, add this code for safety
	   */

	  save_error_msgs = parser->error_msgs;
	  parser->error_msgs = NULL;

	  q = (*parser->print_db_value) (parser, p);

	  if (q)
	    {
	      if (pt_has_error (parser))
		{
		  parser_free_tree (parser, parser->error_msgs);
		}
	      parser->error_msgs = save_error_msgs;	/* restore */

	      return q;
	    }
	  if (pt_has_error (parser))
	    {
	      parser_free_tree (parser, parser->error_msgs);
	    }
	  parser->error_msgs = save_error_msgs;	/* restore */
	}
    }

  q = pt_append_nulstring (parser, q, " ");
  q = pt_append_nulstring (parser, q, p->info.host_var.str);
  /* for internal print, print a host variable with its index */
  sprintf (s, ":%d", p->info.host_var.index);
  q = pt_append_nulstring (parser, q, s);
  q = pt_append_nulstring (parser, q, " ");

  for (t = p->or_next; t; t = t->or_next)
    {
      or_next = t->or_next;
      t->or_next = NULL;
      r = pt_print_bytes (parser, t);
      if (r)
	{
	  q = pt_append_nulstring (parser, q, " or ");
	  q = pt_append_varchar (parser, q, r);
	}
      t->or_next = or_next;
    }

  return q;
}

/* INSERT */
/*
 * pt_apply_insert () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_insert (PARSER_CONTEXT * parser, PT_NODE * p,
		 PT_NODE_FUNCTION g, void *arg)
{
  p->info.insert.spec = g (parser, p->info.insert.spec, arg);
  p->info.insert.attr_list = g (parser, p->info.insert.attr_list, arg);
  p->info.insert.value_clauses = g (parser, p->info.insert.value_clauses,
				    arg);
  p->info.insert.odku_assignments =
    g (parser, p->info.insert.odku_assignments, arg);
  p->info.insert.odku_non_null_attrs =
    g (parser, p->info.insert.odku_non_null_attrs, arg);
  p->info.insert.non_null_attrs =
    g (parser, p->info.insert.non_null_attrs, arg);
  return p;
}

/*
 * pt_init_insert () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_insert (PT_NODE * p)
{
  p->info.insert.spec = 0;
  p->info.insert.attr_list = 0;
  p->info.insert.value_clauses = NULL;
  p->info.insert.hint = PT_HINT_NONE;
  p->info.insert.odku_assignments = NULL;
  p->info.insert.do_replace = false;
  p->info.insert.non_null_attrs = NULL;
  p->info.insert.odku_non_null_attrs = NULL;
  p->info.insert.has_uniques = 0;
  return p;
}

/*
 * pt_print_insert () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_insert (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL, *r1, *r2;
  PT_NODE *crt_list = NULL;
  bool is_first_list = true, multiple_values_insert = false;

  r1 = pt_print_bytes (parser, p->info.insert.spec);
  r2 = pt_print_bytes_l (parser, p->info.insert.attr_list);

  if (p->info.insert.do_replace)
    {
      b = pt_append_nulstring (parser, b, "replace ");
    }
  else
    {
      b = pt_append_nulstring (parser, b, "insert ");
    }
  if (p->info.insert.hint != PT_HINT_NONE)
    {
      b = pt_append_nulstring (parser, b, "/*+");

      if (p->info.insert.hint & PT_HINT_FORCE_PAGE_ALLOCATION)
	{
	  b = pt_append_nulstring (parser, b, " FORCE_PAGE_ALLOCATION ");
	}

      b = pt_append_nulstring (parser, b, " */ ");
    }
  b = pt_append_nulstring (parser, b, "into ");
  b = pt_append_varchar (parser, b, r1);
  if (r2)
    {
      b = pt_append_nulstring (parser, b, " (");
      b = pt_append_varchar (parser, b, r2);
      b = pt_append_nulstring (parser, b, ") ");
    }
  else
    {
      b = pt_append_nulstring (parser, b, " ");
    }

  for (crt_list = p->info.insert.value_clauses, is_first_list = true,
       multiple_values_insert = (crt_list != NULL && crt_list->next != NULL);
       crt_list != NULL; crt_list = crt_list->next, is_first_list = false)
    {
      if (!is_first_list)
	{
	  b = pt_append_nulstring (parser, b, ", ");
	}

      switch (crt_list->info.node_list.list_type)
	{
	case PT_IS_DEFAULT_VALUE:
	  if (is_first_list && multiple_values_insert)
	    {
	      b = pt_append_nulstring (parser, b, "values ");
	    }
	  b = pt_append_nulstring (parser, b, "default values");
	  break;

	case PT_IS_VALUE:
	  r1 = pt_print_bytes_l (parser, crt_list->info.node_list.list);
	  if (is_first_list)
	    {
	      b = pt_append_nulstring (parser, b, "values ");
	    }
	  b = pt_append_nulstring (parser, b, "(");
	  b = pt_append_varchar (parser, b, r1);
	  b = pt_append_nulstring (parser, b, ")");
	  break;

	case PT_IS_SUBQUERY:
	  {
	    PT_NODE *ptr_subquery = crt_list->info.node_list.list;

	    if (ptr_subquery != NULL && ptr_subquery->node_type == PT_SELECT)
	      {
		/* TODO why do we change is_subquery? What about PT_UNION and
		   the rest? */
		ptr_subquery->info.query.is_subquery = (PT_MISC_TYPE) 0;
	      }
	    r1 = pt_print_bytes (parser, ptr_subquery);
	    b = pt_append_varchar (parser, b, r1);
	  }
	  break;

	default:
	  assert (false);
	  break;
	}
    }

  if (p->info.insert.odku_assignments)
    {
      r1 = pt_print_bytes_l (parser, p->info.insert.odku_assignments);
      b = pt_append_nulstring (parser, b, " on duplicate key update ");
      b = pt_append_varchar (parser, b, r1);
    }

  return b;
}


/* INTERSECTION */
/*
 * pt_apply_intersection () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_intersection (PARSER_CONTEXT * parser, PT_NODE * p,
		       PT_NODE_FUNCTION g, void *arg)
{
  p->info.query.q.union_.arg1 = g (parser, p->info.query.q.union_.arg1, arg);
  p->info.query.q.union_.arg2 = g (parser, p->info.query.q.union_.arg2, arg);
  p->info.query.order_by = g (parser, p->info.query.order_by, arg);
  p->info.query.orderby_for = g (parser, p->info.query.orderby_for, arg);
  return p;
}

/*
 * pt_init_intersection () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_intersection (PT_NODE * p)
{
  p->info.query.q.union_.arg1 = NULL;
  p->info.query.q.union_.arg2 = NULL;
  pt_init_query_info (&(p->info.query));
  return p;
}

/*
 * pt_print_intersection () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_intersection (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = NULL, *r1, *r2;

  r1 = pt_print_bytes (parser, p->info.query.q.union_.arg1);
  r2 = pt_print_bytes (parser, p->info.query.q.union_.arg2);
  q = pt_append_nulstring (parser, q, "(");
  q = pt_append_varchar (parser, q, r1);
  if (p->info.query.all_distinct == PT_ALL)
    {
      q = pt_append_nulstring (parser, q, " intersect all ");
    }
  else
    {
      q = pt_append_nulstring (parser, q, " intersect ");
    }
  q = pt_append_varchar (parser, q, r2);
  q = pt_append_nulstring (parser, q, ")");

  if (p->info.query.order_by)
    {
      r1 = pt_print_bytes_l (parser, p->info.query.order_by);
      q = pt_append_nulstring (parser, q, " order by ");
      q = pt_append_varchar (parser, q, r1);
    }
  if (p->info.query.orderby_for)
    {
      r1 = pt_print_bytes_l (parser, p->info.query.orderby_for);
      q = pt_append_nulstring (parser, q, " for");
      q = pt_append_varchar (parser, q, r1);
    }
  if (p->info.query.limit && p->info.query.rewrite_limit)
    {
      r1 = pt_print_bytes_l (parser, p->info.query.limit);
      q = pt_append_nulstring (parser, q, " limit ");
      q = pt_append_varchar (parser, q, r1);
    }
  return q;
}

/* ISOLATION_LVL */
/*
 * pt_apply_isolation_lvl () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_isolation_lvl (PARSER_CONTEXT * parser, PT_NODE * p,
			PT_NODE_FUNCTION g, void *arg)
{
  p->info.isolation_lvl.level = g (parser, p->info.isolation_lvl.level, arg);
  return p;
}

/*
 * pt_init_isolation_lvl () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_isolation_lvl (PT_NODE * p)
{
  p->info.isolation_lvl.schema =
    p->info.isolation_lvl.instances = PT_NO_ISOLATION_LEVEL;
  p->info.isolation_lvl.level = NULL;

  return (p);
}

/*
 * pt_print_isolation_lvl () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_isolation_lvl (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL, *r1;

  b = pt_append_nulstring (parser, b, "isolation level ");
  if (p->info.isolation_lvl.schema == PT_SERIALIZABLE
      && p->info.isolation_lvl.instances == PT_SERIALIZABLE)
    {
      b = pt_append_nulstring (parser, b, " serializable ");
    }
  else
    {
      if (p->info.isolation_lvl.schema != PT_NO_ISOLATION_LEVEL)
	{
	  b = pt_append_nulstring (parser, b,
				   pt_show_misc_type (p->info.
						      isolation_lvl.schema));
	  b = pt_append_nulstring (parser, b, " schema");
	}
      if (p->info.isolation_lvl.instances != PT_NO_ISOLATION_LEVEL)
	{
	  if (p->info.isolation_lvl.schema != PT_NO_ISOLATION_LEVEL)
	    {
	      b = pt_append_nulstring (parser, b, ",");
	    }
	  b = pt_append_nulstring (parser, b,
				   pt_show_misc_type (p->info.
						      isolation_lvl.
						      instances));
	  b = pt_append_nulstring (parser, b, " instances ");
	}
    }

  if (p->info.isolation_lvl.level)
    {
      r1 = pt_print_bytes (parser, p->info.isolation_lvl.level);
      b = pt_append_varchar (parser, b, r1);
    }

  return b;
}


/* NAME */
/*
 * pt_apply_name () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_name (PARSER_CONTEXT * parser, PT_NODE * p, PT_NODE_FUNCTION g,
	       void *arg)
{
  p->info.name.default_value = g (parser, p->info.name.default_value, arg);
  p->info.name.indx_key_limit = g (parser, p->info.name.indx_key_limit, arg);
  return p;
}

/*
 * pt_init_name () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_name (PT_NODE * p)
{
  p->info.name.location = 0;
  p->info.name.indx_key_limit = NULL;
  p->info.name.hidden_column = 0;

  return p;
}


/*
 * pt_print_name () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_name (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = NULL, *r1;
  unsigned int save_custom = parser->custom_print;

  parser->custom_print = parser->custom_print | p->info.name.custom_print;

  if (p->info.name.meta_class == PT_OID_ATTR)
    {
      /* print the correlation name, which may be in one of two
         locations, before and after name resolution. */
      if (p->info.name.original && p->info.name.original[0])
	{
	  char *lcase_name;
	  int name_size;

	  name_size = strlen (p->info.name.original);
	  lcase_name = (char *) malloc (name_size + 1);
	  intl_identifier_lower (p->info.name.original, lcase_name);
	  q = pt_append_name (parser, q, lcase_name);
	  free_and_init (lcase_name);
	}
      else if (p->info.name.resolved)
	{
	  q = pt_append_name (parser, q, p->info.name.resolved);
	}
    }
  else
    if (!(parser->custom_print & PT_SUPPRESS_RESOLVED)
	&& (p->info.name.resolved && p->info.name.resolved[0])
	&& p->info.name.meta_class != PT_CLASS
	&& p->info.name.meta_class != PT_HINT_NAME)
    {
      /* Print both resolved name and original name
       * If there is a non-zero length resolved name, print it,
       * followed by ".".
       */
      if ((parser->custom_print & PT_FORCE_ORIGINAL_TABLE_NAME)
	  && (p->info.name.meta_class == PT_NORMAL))
	{
	  /* make sure spec_id points to original table */
	  PT_NODE *original_spec;

	  assert (p->info.name.spec_id);
	  original_spec = (PT_NODE *) p->info.name.spec_id;
	  if (original_spec->info.spec.entity_name
	      && original_spec->info.spec.entity_name->info.name.original)
	    {
	      q =
		pt_append_name (parser, q,
				original_spec->info.spec.entity_name->info.
				name.original);
	    }
	  else
	    {
	      q = pt_append_name (parser, q, p->info.name.resolved);
	    }
	}
      else
	{
	  q = pt_append_name (parser, q, p->info.name.resolved);
	}

      if (p->info.name.original && p->info.name.original[0])
	{
	  q = pt_append_nulstring (parser, q, ".");
	  q = pt_append_name (parser, q, p->info.name.original);
	  if (p->info.name.indx_key_limit)
	    {
	      q = pt_append_nulstring (parser, q, " keylimit ");
	      if (p->info.name.indx_key_limit->next)
		{
		  r1 =
		    pt_print_bytes (parser,
				    p->info.name.indx_key_limit->next);
		  q = pt_append_varchar (parser, q, r1);
		  q = pt_append_nulstring (parser, q, ",");
		}
	      r1 = pt_print_bytes (parser, p->info.name.indx_key_limit);
	      q = pt_append_varchar (parser, q, r1);
	    }
	}
      if (p->info.name.meta_class == PT_INDEX_NAME
	  && p->info.name.original == NULL
	  && p->etc == (void *) PT_IDX_HINT_CLASS_NONE)
	{
	  q = pt_append_nulstring (parser, q, ".");
	  q = pt_append_nulstring (parser, q, "none");
	}
    }
  else
    {
      /* here we print whatever the length */
      if (p->info.name.original)
	{
	  q = pt_append_name (parser, q, p->info.name.original);
	  if (p->info.name.indx_key_limit)
	    {
	      q = pt_append_nulstring (parser, q, " keylimit ");
	      if (p->info.name.indx_key_limit->next)
		{
		  r1 =
		    pt_print_bytes (parser,
				    p->info.name.indx_key_limit->next);
		  q = pt_append_varchar (parser, q, r1);
		  q = pt_append_nulstring (parser, q, ",");
		}
	      r1 = pt_print_bytes (parser, p->info.name.indx_key_limit);
	      q = pt_append_varchar (parser, q, r1);
	    }
	}
      else
	{
	  if (p->info.name.meta_class == PT_INDEX_NAME
	      && p->info.name.resolved && p->info.name.resolved[0]
	      && p->etc == (void *) PT_IDX_HINT_CLASS_NONE)
	    {
	      /* always print resolved for "class_name.NONE" index names */
	      q = pt_append_name (parser, q, p->info.name.resolved);
	      q = pt_append_nulstring (parser, q, ".");
	      q = pt_append_nulstring (parser, q, "none");
	    }
	}
    }

  if (p->type_enum == PT_TYPE_STAR)
    {
      q = pt_append_nulstring (parser, q, ".*");
    }

  if (PT_NAME_INFO_IS_FLAGED (p, PT_NAME_INFO_DESC))
    {
      q = pt_append_nulstring (parser, q, " desc");
    }

  if ((parser->custom_print & PT_PRINT_ALIAS) && p->alias_print != NULL)
    {
      q = pt_append_nulstring (parser, q, " as [");
      q = pt_append_nulstring (parser, q, p->alias_print);
      q = pt_append_nulstring (parser, q, "]");
    }

  parser->custom_print = save_custom;
  return q;
}


/* RENAME */
/*
 * pt_apply_rename () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_rename (PARSER_CONTEXT * parser, PT_NODE * p,
		 PT_NODE_FUNCTION g, void *arg)
{
  p->info.rename.old_name = g (parser, p->info.rename.old_name, arg);
  p->info.rename.in_class = g (parser, p->info.rename.in_class, arg);
  p->info.rename.new_name = g (parser, p->info.rename.new_name, arg);
  return p;
}

/*
 * pt_init_rename () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_rename (PT_NODE * p)
{
  p->info.rename.old_name = 0;
  p->info.rename.in_class = 0;
  p->info.rename.new_name = 0;
  return p;
}

/*
 * pt_print_rename () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_rename (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL;
  PT_NODE *crt_pair = p;

  b = pt_append_nulstring (parser, b, "rename ");
  b = pt_append_nulstring (parser, b,
			   pt_show_misc_type (p->info.rename.entity_type));
  b = pt_append_nulstring (parser, b, " ");
  do
    {
      PARSER_VARCHAR *r1, *r2;

      r1 = pt_print_bytes (parser, crt_pair->info.rename.old_name);
      r2 = pt_print_bytes (parser, crt_pair->info.rename.new_name);
      b = pt_append_varchar (parser, b, r1);
      b = pt_append_nulstring (parser, b, " as ");
      b = pt_append_varchar (parser, b, r2);
      if (crt_pair->next != NULL)
	{
	  b = pt_append_nulstring (parser, b, ", ");
	}
      crt_pair = crt_pair->next;
    }
  while (crt_pair != NULL);
  return b;
}

/* REVOKE */
/*
 * pt_apply_revoke () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_revoke (PARSER_CONTEXT * parser, PT_NODE * p,
		 PT_NODE_FUNCTION g, void *arg)
{
  p->info.revoke.auth_cmd_list =
    g (parser, p->info.revoke.auth_cmd_list, arg);
  p->info.revoke.user_list = g (parser, p->info.revoke.user_list, arg);
  p->info.revoke.spec_list = g (parser, p->info.revoke.spec_list, arg);
  return p;
}

/*
 * pt_init_revoke () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_revoke (PT_NODE * p)
{
  p->info.revoke.auth_cmd_list = 0;
  p->info.revoke.user_list = 0;
  p->info.revoke.spec_list = 0;
  return (p);
}

/*
 * pt_print_revoke () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_revoke (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1, *r2, *r3;
  unsigned int save_custom;

  r1 = pt_print_bytes_l (parser, p->info.revoke.auth_cmd_list);
  save_custom = parser->custom_print;
  parser->custom_print |= PT_SUPPRESS_RESOLVED;
  r2 = pt_print_bytes_l (parser, p->info.revoke.spec_list);
  parser->custom_print = save_custom;
  r3 = pt_print_bytes_l (parser, p->info.revoke.user_list);
  q = pt_append_nulstring (parser, q, "revoke ");
  q = pt_append_varchar (parser, q, r1);
  q = pt_append_nulstring (parser, q, " on ");
  q = pt_append_varchar (parser, q, r2);
  q = pt_append_nulstring (parser, q, " from ");
  q = pt_append_varchar (parser, q, r3);

  return q;
}


/* ROLLBACK_WORK */
/*
 * pt_apply_rollback_work () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_rollback_work (PARSER_CONTEXT * parser, PT_NODE * p,
			PT_NODE_FUNCTION g, void *arg)
{
  p->info.rollback_work.save_name =
    g (parser, p->info.rollback_work.save_name, arg);
  return p;
}

/*
 * pt_init_rollback_work () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_rollback_work (PT_NODE * p)
{
  p->info.rollback_work.save_name = 0;

  return (p);
}

/*
 * pt_print_rollback_work () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_rollback_work (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;

  q = pt_append_nulstring (parser, q, "rollback work");
  if (p->info.rollback_work.save_name)
    {
      r1 = pt_print_bytes (parser, p->info.rollback_work.save_name);
      q = pt_append_nulstring (parser, q, " to savepoint ");
      q = pt_append_varchar (parser, q, r1);
    }

  return q;
}


/* SAVEPOINT */
/*
 * pt_apply_savepoint () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_savepoint (PARSER_CONTEXT * parser, PT_NODE * p,
		    PT_NODE_FUNCTION g, void *arg)
{
  p->info.savepoint.save_name = g (parser, p->info.savepoint.save_name, arg);
  return p;
}

/*
 * pt_init_savepoint () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_savepoint (PT_NODE * p)
{
  p->info.savepoint.save_name = 0;
  return (p);
}

/*
 * pt_print_savepoint () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_savepoint (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL, *r1;

  r1 = pt_print_bytes (parser, p->info.savepoint.save_name);
  b = pt_append_nulstring (parser, b, "savepoint ");
  b = pt_append_varchar (parser, b, r1);

  return b;
}

/* SELECT */
/*
 * pt_apply_select () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_select (PARSER_CONTEXT * parser, PT_NODE * p,
		 PT_NODE_FUNCTION g, void *arg)
{
  p->info.query.q.select.list = g (parser, p->info.query.q.select.list, arg);
  p->info.query.q.select.from = g (parser, p->info.query.q.select.from, arg);
  p->info.query.q.select.where = g (parser, p->info.query.q.select.where,
				    arg);
  p->info.query.q.select.group_by =
    g (parser, p->info.query.q.select.group_by, arg);
  p->info.query.q.select.having =
    g (parser, p->info.query.q.select.having, arg);
  p->info.query.q.select.using_index =
    g (parser, p->info.query.q.select.using_index, arg);
#if 0				/* TEMPORARY COMMENTED CODE: DO NOT REMOVE ME !!! */
  p->info.query.q.select.ordered_hint =
    g (parser, p->info.query.q.select.ordered_hint, arg);
#endif
  p->info.query.q.select.use_nl_hint =
    g (parser, p->info.query.q.select.use_nl_hint, arg);
  p->info.query.q.select.use_idx_hint =
    g (parser, p->info.query.q.select.use_idx_hint, arg);
  p->info.query.order_by = g (parser, p->info.query.order_by, arg);
  p->info.query.orderby_for = g (parser, p->info.query.orderby_for, arg);
  p->info.query.qcache_hint = g (parser, p->info.query.qcache_hint, arg);
  p->info.query.limit = g (parser, p->info.query.limit, arg);
  p->info.query.pk_next = g (parser, p->info.query.pk_next, arg);
  return p;
}

/*
 * pt_init_select () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_select (PT_NODE * p)
{
  p->info.query.q.select.list = 0;
  p->info.query.q.select.list = NULL;
  p->info.query.q.select.from = NULL;
  p->info.query.q.select.where = NULL;
  p->info.query.q.select.group_by = NULL;
  p->info.query.q.select.having = NULL;
  p->info.query.q.select.using_index = 0;
#if 0				/* TEMPORARY COMMENTED CODE: DO NOT REMOVE ME !!! */
  p->info.query.q.select.ordered_hint = NULL;
#endif
  p->info.query.q.select.use_nl_hint = NULL;
  p->info.query.q.select.use_idx_hint = NULL;
  p->info.query.q.select.qo_summary = NULL;
  p->info.query.q.select.hint = PT_HINT_NONE;
  p->info.query.q.select.flag = 0;
  pt_init_query_info (&(p->info.query));
  return p;
}

/*
 * pt_print_select () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_select (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;
  PT_NODE *temp, *where_list;
  bool set_paren = false;	/* init */
  bool toggle_print_alias = false;
  bool is_first_list;
  unsigned int save_custom;
  PT_NODE *from = NULL, *derived_table = NULL;

  from = p->info.query.q.select.from;

  if (p->info.query.is_subquery == PT_IS_SUBQUERY
      || (p->info.query.is_subquery == PT_IS_UNION_SUBQUERY
	  && p->info.query.order_by)
      || (p->info.query.is_subquery == PT_IS_UNION_QUERY
	  && p->info.query.order_by))
    {
      set_paren = true;
    }

  if (set_paren)
    {
      q = pt_append_nulstring (parser, q, "(");
    }

  temp = p->info.query.q.select.list;
  if (temp && temp->node_type == PT_NODE_LIST)	/* values(...),... */
    {
      q = pt_append_nulstring (parser, q, "values ");

      save_custom = parser->custom_print;
      parser->custom_print |= PT_PRINT_ALIAS;

      is_first_list = true;
      for (; temp; temp = temp->next)
	{
	  if (!is_first_list)
	    {
	      q = pt_append_nulstring (parser, q, ",(");
	    }
	  else
	    {
	      q = pt_append_nulstring (parser, q, "(");
	      is_first_list = false;
	    }

	  r1 = pt_print_bytes_l (parser, temp->info.node_list.list);
	  q = pt_append_varchar (parser, q, r1);

	  q = pt_append_nulstring (parser, q, ")");
	}

      parser->custom_print = save_custom;
    }
  else
    {
      q = pt_append_nulstring (parser, q, "select ");

      if (p->info.query.q.select.hint != PT_HINT_NONE
	  || (p->info.query.hint != PT_HINT_NONE
	      && p->info.query.hint != PT_HINT_REEXECUTE))
	{
	  q = pt_append_nulstring (parser, q, "/*+ ");
	  if (p->info.query.hint & PT_HINT_QUERY_CACHE)
	    {
	      /* query cache */
	      q = pt_append_nulstring (parser, q, "QUERY_CACHE");
	      if (p->info.query.qcache_hint)
		{
		  r1 = pt_print_bytes (parser, p->info.query.qcache_hint);
		  q = pt_append_nulstring (parser, q, "(");
		  q = pt_append_varchar (parser, q, r1);
		  q = pt_append_nulstring (parser, q, ") ");
		}
	      else
		{
		  q = pt_append_nulstring (parser, q, " ");
		}
	    }
	  if (p->info.query.q.select.hint & PT_HINT_ORDERED)
	    {
	      /* force join left-to-right */
	      q = pt_append_nulstring (parser, q, "ORDERED");
#if 0				/* TEMPORARY COMMENTED CODE: DO NOT REMOVE ME !!! */
	      if (p->info.query.q.select.ordered_hint)
		{
		  r1 =
		    pt_print_bytes_l (parser,
				      p->info.query.q.select.ordered_hint);
		  q = pt_append_nulstring (parser, q, "(");
		  q = pt_append_varchar (parser, q, r1);
		  q = pt_append_nulstring (parser, q, ") ");
		}
	      else
#endif
		{
		  q = pt_append_nulstring (parser, q, " ");
		}
	    }

#if 0
	  if (p->info.query.q.select.hint & PT_HINT_Y)
	    {
	      /* -- not used */
	      q = pt_append_nulstring (parser, q, "Y ");
	    }
#endif /* 0 */

	  if (p->info.query.q.select.hint & PT_HINT_USE_NL)
	    {
	      /* force nl-join */
	      q = pt_append_nulstring (parser, q, "USE_NL");
	      if (p->info.query.q.select.use_nl_hint)
		{
		  r1 =
		    pt_print_bytes_l (parser,
				      p->info.query.q.select.use_nl_hint);
		  q = pt_append_nulstring (parser, q, "(");
		  q = pt_append_varchar (parser, q, r1);
		  q = pt_append_nulstring (parser, q, ") ");
		}
	      else
		{
		  q = pt_append_nulstring (parser, q, " ");
		}
	    }
	  if (p->info.query.q.select.hint & PT_HINT_USE_IDX)
	    {
	      /* force idx-join */
	      q = pt_append_nulstring (parser, q, "USE_IDX");
	      if (p->info.query.q.select.use_idx_hint)
		{
		  r1 =
		    pt_print_bytes_l (parser,
				      p->info.query.q.select.use_idx_hint);
		  q = pt_append_nulstring (parser, q, "(");
		  q = pt_append_varchar (parser, q, r1);
		  q = pt_append_nulstring (parser, q, ") ");
		}
	      else
		{
		  q = pt_append_nulstring (parser, q, " ");
		}
	    }

	  if (p->info.query.q.select.hint & PT_HINT_USE_IDX_DESC)
	    {
	      q = pt_append_nulstring (parser, q, "USE_DESC_IDX ");
	    }

	  if (p->info.query.q.select.hint & PT_HINT_NO_COVERING_IDX)
	    {
	      q = pt_append_nulstring (parser, q, "NO_COVERING_IDX ");
	    }

	  if (p->info.query.q.select.hint & PT_HINT_NO_IDX_DESC)
	    {
	      q = pt_append_nulstring (parser, q, "NO_DESC_IDX ");
	    }

	  if (p->info.query.q.select.hint & PT_HINT_NO_MULTI_RANGE_OPT)
	    {
	      q = pt_append_nulstring (parser, q, "NO_MULTI_RANGE_OPT ");
	    }

	  if (p->info.query.q.select.hint & PT_HINT_NO_SORT_LIMIT)
	    {
	      q = pt_append_nulstring (parser, q, "NO_SORT_LIMIT ");
	    }

	  q = pt_append_nulstring (parser, q, "*/ ");
	}

      if (p->info.query.all_distinct == PT_ALL)
	{
	  /* left out "all", its the default. */
	}
      else if (p->info.query.all_distinct == PT_DISTINCT)
	{
	  q = pt_append_nulstring (parser, q, "distinct ");
	}

      if (!(parser->custom_print & PT_SUPPRESS_SELECT_LIST) ||
	  p->info.query.is_subquery == PT_IS_SUBQUERY)
	{
	  r1 = pt_print_bytes_l (parser, p->info.query.q.select.list);
	  q = pt_append_varchar (parser, q, r1);
	}
      else
	{
	  temp = p->info.query.q.select.list;
	  while (temp)
	    {
	      q = pt_append_nulstring (parser, q, "NA");
	      if (temp->next)
		{
		  q = pt_append_nulstring (parser, q, ",");
		}
	      temp = temp->next;
	    }
	}

      if (parser->custom_print & PT_PRINT_ALIAS)
	{
	  parser->custom_print ^= PT_PRINT_ALIAS;
	  toggle_print_alias = true;
	}

      from = p->info.query.q.select.from;
      if (from != NULL)
	{
	  /* for derived_table alias should be printed
	   * e.g.
	   *    create table t2(id int primary key)
	   *         as select id from (select count(*) id from t1)
	   */
	  derived_table = from->info.spec.derived_table;
	  if (derived_table != NULL)
	    {
	      save_custom = parser->custom_print;
	      parser->custom_print |= PT_PRINT_ALIAS;
	    }

	  r1 = pt_print_bytes_spec_list (parser, from);

	  if (derived_table != NULL)
	    {
	      parser->custom_print = save_custom;
	    }

	  q = pt_append_nulstring (parser, q, " from ");
	  q = pt_append_varchar (parser, q, r1);
	}

      where_list = p->info.query.q.select.where;

      if (where_list)
	{
	  r1 = pt_print_and_list (parser, where_list);
	  if (r1 != NULL)
	    {
	      q = pt_append_nulstring (parser, q, " where ");
	      q = pt_append_varchar (parser, q, r1);
	    }
	}

      if (p->info.query.q.select.group_by)
	{
	  r1 = pt_print_bytes_l (parser, p->info.query.q.select.group_by);
	  q = pt_append_nulstring (parser, q, " group by ");
	  q = pt_append_varchar (parser, q, r1);
	  if (p->info.query.q.select.group_by->with_rollup)
	    {
	      q = pt_append_nulstring (parser, q, " with rollup");
	    }
	}

      if (p->info.query.q.select.having)
	{
	  r1 = pt_print_and_list (parser, p->info.query.q.select.having);
	  q = pt_append_nulstring (parser, q, " having ");
	  q = pt_append_varchar (parser, q, r1);
	}

      if (p->info.query.q.select.using_index)
	{
	  if (p->info.query.q.select.using_index->info.name.original == NULL)
	    {
	      if (p->info.query.q.select.using_index->info.name.resolved ==
		  NULL)
		{
		  q = pt_append_nulstring (parser, q, " using index none");
		}
	      else
		{
		  assert (p->info.query.q.select.using_index->etc
			  == (void *) PT_IDX_HINT_CLASS_NONE);

		  r1 = pt_print_bytes_l (parser,
					 p->info.query.q.select.using_index);
		  q = pt_append_nulstring (parser, q, " using index ");
		  q = pt_append_varchar (parser, q, r1);
		}
	    }
	  else
	    {
	      r1 =
		pt_print_bytes_l (parser, p->info.query.q.select.using_index);
	      q = pt_append_nulstring (parser, q, " using index ");
	      q = pt_append_varchar (parser, q, r1);
	    }
	}

      if (PT_SELECT_INFO_IS_FLAGED (p, PT_SELECT_INFO_FOR_UPDATE))
	{
	  q = pt_append_nulstring (parser, q, " for update");
	}

      if (p->info.query.order_by)
	{
	  r1 = pt_print_bytes_l (parser, p->info.query.order_by);
	  if (p->info.query.order_siblings)
	    {
	      q = pt_append_nulstring (parser, q, " order siblings by ");
	    }
	  else
	    {
	      q = pt_append_nulstring (parser, q, " order by ");
	    }
	  q = pt_append_varchar (parser, q, r1);
	}

      if (p->info.query.orderby_for)
	{
	  r1 = pt_print_bytes_l (parser, p->info.query.orderby_for);
	  q = pt_append_nulstring (parser, q, " for ");
	  q = pt_append_varchar (parser, q, r1);
	}

      if (p->info.query.limit && p->info.query.rewrite_limit)
	{
	  r1 = pt_print_bytes_l (parser, p->info.query.limit);
	  q = pt_append_nulstring (parser, q, " limit ");
	  q = pt_append_varchar (parser, q, r1);
	}

      if (p->info.query.pk_next)
	{
	  r1 = pt_print_bytes_l (parser, p->info.query.pk_next);
	  q = pt_append_nulstring (parser, q, " primary key next (");
	  q = pt_append_varchar (parser, q, r1);
	  q = pt_append_nulstring (parser, q, ")");
	}

      if (toggle_print_alias == true)
	{
	  parser->custom_print ^= PT_PRINT_ALIAS;
	}
    }

  if (set_paren)
    {
      q = pt_append_nulstring (parser, q, ")");

      if ((parser->custom_print & PT_PRINT_ALIAS) && p->alias_print != NULL)
	{
	  q = pt_append_nulstring (parser, q, " as [");
	  q = pt_append_nulstring (parser, q, p->alias_print);
	  q = pt_append_nulstring (parser, q, "]");
	}
    }

  return q;
}

/* SET_OPTIMIZATION_LEVEL */
/*
 * pt_apply_set_opt_lvl () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_set_opt_lvl (PARSER_CONTEXT * parser, PT_NODE * p,
		      PT_NODE_FUNCTION g, void *arg)
{
  p->info.set_opt_lvl.val = g (parser, p->info.set_opt_lvl.val, arg);
  return p;
}

/*
 * pt_init_set_opt_lvl () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_set_opt_lvl (PT_NODE * p)
{
  p->info.set_opt_lvl.option = PT_OPT_LVL;
  return (p);
}

/*
 * pt_print_set_opt_lvl () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_set_opt_lvl (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL, *r1, *r2 = NULL;
  PT_MISC_TYPE option;

  option = p->info.set_opt_lvl.option;
  r1 = pt_print_bytes (parser, p->info.set_opt_lvl.val);
  if (option == PT_OPT_COST)
    {
      r2 = pt_print_bytes (parser, p->info.set_opt_lvl.val->next);
    }

  b = pt_append_nulstring (parser, b, "set optimization ");
  b = pt_append_nulstring (parser, b, pt_show_misc_type (option));
  b = pt_append_nulstring (parser, b, " ");
  b = pt_append_varchar (parser, b, r1);
  if (option == PT_OPT_COST)
    {
      b = pt_append_nulstring (parser, b, " ");
      b = pt_append_varchar (parser, b, r2);
    }

  return b;
}

/* SET_SYSTEM_PARAMETERS */
/*
 * pt_apply_set_sys_params () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_set_sys_params (PARSER_CONTEXT * parser, PT_NODE * p,
			 PT_NODE_FUNCTION g, void *arg)
{
  p->info.set_sys_params.val = g (parser, p->info.set_sys_params.val, arg);
  return p;
}

/*
 * pt_init_set_sys_params () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_set_sys_params (PT_NODE * p)
{
  p->info.set_sys_params.persist = PT_PERSIST_OFF;
  return (p);
}

/*
 * pt_print_set_sys_params () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_set_sys_params (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL, *r1;

  b = pt_append_nulstring (parser, b, "set ");
  if (p->info.set_sys_params.persist == PT_PERSIST_ON)
    {
      b = pt_append_nulstring (parser, b, "persist ");
    }
  b = pt_append_nulstring (parser, b, "system parameters ");

  if (p->info.set_sys_params.val)
    {
      r1 = pt_print_bytes (parser, p->info.set_sys_params.val);
      b = pt_append_varchar (parser, b, r1);
    }

  return b;
}

/* SORT_SPEC */
/*
 * pt_apply_sort_spec () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_sort_spec (PARSER_CONTEXT * parser, PT_NODE * p,
		    PT_NODE_FUNCTION g, void *arg)
{
  p->info.sort_spec.expr = g (parser, p->info.sort_spec.expr, arg);
  return p;
}

/*
 * pt_init_sort_spec () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_sort_spec (PT_NODE * p)
{
  p->info.sort_spec.expr = 0;
  p->info.sort_spec.pos_descr.pos_no = 0;
  p->info.sort_spec.asc_or_desc = PT_ASC;
  p->info.sort_spec.nulls_first_or_last = PT_NULLS_DEFAULT;
  return p;
}

/*
 * pt_print_sort_spec () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_sort_spec (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = 0, *r1;

  r1 = pt_print_bytes (parser, p->info.sort_spec.expr);
  q = pt_append_varchar (parser, q, r1);
  if (p->info.sort_spec.asc_or_desc == PT_DESC)
    {
      q = pt_append_nulstring (parser, q, " desc ");
    }

  if (p->info.sort_spec.nulls_first_or_last == PT_NULLS_FIRST)
    {
      q = pt_append_nulstring (parser, q, " nulls first ");
    }
  else if (p->info.sort_spec.nulls_first_or_last == PT_NULLS_LAST)
    {
      q = pt_append_nulstring (parser, q, " nulls last ");
    }

  return q;
}

/* TIMEOUT */
/*
 * pt_apply_timeout () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_timeout (PARSER_CONTEXT * parser, PT_NODE * p,
		  PT_NODE_FUNCTION g, void *arg)
{
  p->info.timeout.val = g (parser, p->info.timeout.val, arg);
  return p;
}

/*
 * pt_init_timeout () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_timeout (PT_NODE * p)
{
  return (p);
}

/*
 * pt_print_timeout () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_timeout (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL, *r1;
  PT_NODE *val;

  b = pt_append_nulstring (parser, b, "lock timeout ");
  val = p->info.timeout.val;

  if (val)
    {
      if (val->info.value.data_value.i == -1)
	{
	  b = pt_append_nulstring (parser, b, "infinite");
	}
      else if (val->info.value.data_value.i == 0)
	{
	  b = pt_append_nulstring (parser, b, "off");
	}
      else
	{
	  r1 = pt_print_bytes (parser, p->info.timeout.val);
	  b = pt_append_varchar (parser, b, r1);
	}
    }
  return b;
}

/* UNION_STMT */
/*
 * pt_apply_union_stmt () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_union_stmt (PARSER_CONTEXT * parser, PT_NODE * p,
		     PT_NODE_FUNCTION g, void *arg)
{
  p->info.query.q.union_.arg1 = g (parser, p->info.query.q.union_.arg1, arg);
  p->info.query.q.union_.arg2 = g (parser, p->info.query.q.union_.arg2, arg);
  p->info.query.order_by = g (parser, p->info.query.order_by, arg);
  p->info.query.orderby_for = g (parser, p->info.query.orderby_for, arg);
  return p;
}

/*
 * pt_init_union_stmt () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_union_stmt (PT_NODE * p)
{
  p->info.query.q.union_.arg1 = NULL;
  p->info.query.q.union_.arg2 = NULL;
  pt_init_query_info (&(p->info.query));
  return p;
}

/*
 * pt_print_union_stmt () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_union_stmt (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *q = NULL, *r1, *r2;

  r1 = pt_print_bytes (parser, p->info.query.q.union_.arg1);
  r2 = pt_print_bytes (parser, p->info.query.q.union_.arg2);
  q = pt_append_nulstring (parser, q, "(");
  q = pt_append_varchar (parser, q, r1);
  q = pt_append_nulstring (parser, q, " union ");
  if (p->info.query.all_distinct == PT_ALL)
    {
      q = pt_append_nulstring (parser, q, "all ");
    }
  q = pt_append_varchar (parser, q, r2);
  q = pt_append_nulstring (parser, q, ")");

  if (p->info.query.order_by)
    {
      r1 = pt_print_bytes_l (parser, p->info.query.order_by);
      q = pt_append_nulstring (parser, q, " order by ");
      q = pt_append_varchar (parser, q, r1);
    }
  if (p->info.query.orderby_for)
    {
      r1 = pt_print_bytes_l (parser, p->info.query.orderby_for);
      q = pt_append_nulstring (parser, q, " for ");
      q = pt_append_varchar (parser, q, r1);
    }
  if (p->info.query.limit && p->info.query.rewrite_limit)
    {
      r1 = pt_print_bytes_l (parser, p->info.query.limit);
      q = pt_append_nulstring (parser, q, " limit ");
      q = pt_append_varchar (parser, q, r1);
    }

  return q;
}


/* UPDATE */
/*
 * pt_apply_update () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_update (PARSER_CONTEXT * parser, PT_NODE * p,
		 PT_NODE_FUNCTION g, void *arg)
{
  p->info.update.spec = g (parser, p->info.update.spec, arg);
  p->info.update.assignment = g (parser, p->info.update.assignment, arg);
  p->info.update.search_cond = g (parser, p->info.update.search_cond, arg);
  p->info.update.order_by = g (parser, p->info.update.order_by, arg);
  p->info.update.orderby_for = g (parser, p->info.update.orderby_for, arg);
  p->info.update.using_index = g (parser, p->info.update.using_index, arg);
  p->info.update.internal_stmts = g (parser,
				     p->info.update.internal_stmts, arg);
#if 0				/* TEMPORARY COMMENTED CODE: DO NOT REMOVE ME !!! */
  p->info.update.ordered_hint = g (parser, p->info.update.ordered_hint, arg);
#endif
  p->info.update.use_nl_hint = g (parser, p->info.update.use_nl_hint, arg);
  p->info.update.use_idx_hint = g (parser, p->info.update.use_idx_hint, arg);
  p->info.update.limit = g (parser, p->info.update.limit, arg);

  return p;
}

/*
 * pt_init_update () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_update (PT_NODE * p)
{
  p->info.update.hint = PT_HINT_NONE;
#if 0				/* TEMPORARY COMMENTED CODE: DO NOT REMOVE ME !!! */
  p->info.update.ordered_hint = NULL;
#endif
  p->info.update.use_nl_hint = NULL;
  p->info.update.use_idx_hint = NULL;
  return p;
}

/*
 * pt_print_update () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_update (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL, *r1;

  b = pt_append_nulstring (parser, b, "update ");

  if (p->info.update.hint != PT_HINT_NONE)
    {
      b = pt_append_nulstring (parser, b, "/*+");

      if (p->info.update.hint & PT_HINT_ORDERED)
	{
	  /* force join left-to-right */
	  b = pt_append_nulstring (parser, b, " ORDERED");
#if 0				/* TEMPORARY COMMENTED CODE: DO NOT REMOVE ME !!! */
	  if (p->info.update.ordered_hint)
	    {
	      r1 = pt_print_bytes_l (parser, p->info.update.ordered_hint);
	      b = pt_append_nulstring (parser, b, "(");
	      b = pt_append_varchar (parser, b, r1);
	      b = pt_append_nulstring (parser, b, ") ");
	    }
	  else
#endif
	    {
	      b = pt_append_nulstring (parser, b, " ");
	    }
	}

      if (p->info.update.hint & PT_HINT_USE_NL)
	{
	  /* force nl-join */
	  b = pt_append_nulstring (parser, b, " USE_NL");
	  if (p->info.update.use_nl_hint)
	    {
	      r1 = pt_print_bytes_l (parser, p->info.update.use_nl_hint);
	      b = pt_append_nulstring (parser, b, "(");
	      b = pt_append_varchar (parser, b, r1);
	      b = pt_append_nulstring (parser, b, ") ");
	    }
	  else
	    {
	      b = pt_append_nulstring (parser, b, " ");
	    }
	}

      if (p->info.update.hint & PT_HINT_USE_IDX)
	{
	  /* force idx-join */
	  b = pt_append_nulstring (parser, b, " USE_IDX");
	  if (p->info.update.use_idx_hint)
	    {
	      r1 = pt_print_bytes_l (parser, p->info.update.use_idx_hint);
	      b = pt_append_nulstring (parser, b, "(");
	      b = pt_append_varchar (parser, b, r1);
	      b = pt_append_nulstring (parser, b, ") ");
	    }
	  else
	    {
	      b = pt_append_nulstring (parser, b, " ");
	    }
	}

      if (p->info.update.hint & PT_HINT_USE_IDX_DESC)
	{
	  b = pt_append_nulstring (parser, b, " USE_DESC_IDX ");
	}

      if (p->info.update.hint & PT_HINT_NO_COVERING_IDX)
	{
	  b = pt_append_nulstring (parser, b, " NO_COVERING_IDX ");
	}

      if (p->info.update.hint & PT_HINT_NO_IDX_DESC)
	{
	  b = pt_append_nulstring (parser, b, " NO_DESC_IDX ");
	}

      if (p->info.update.hint & PT_HINT_NO_MULTI_RANGE_OPT)
	{
	  b = pt_append_nulstring (parser, b, " NO_MULTI_RANGE_OPT ");
	}

      if (p->info.update.hint & PT_HINT_NO_SORT_LIMIT)
	{
	  b = pt_append_nulstring (parser, b, " NO_SORT_LIMIT ");
	}

      b = pt_append_nulstring (parser, b, " */");
    }

  /* print the spec list */
  r1 = pt_print_bytes_spec_list (parser, p->info.update.spec);
  b = pt_append_varchar (parser, b, r1);

  r1 = pt_print_bytes_l (parser, p->info.update.assignment);
  b = pt_append_nulstring (parser, b, " set ");
  b = pt_append_varchar (parser, b, r1);

  if (p->info.update.search_cond)
    {
      r1 = pt_print_and_list (parser, p->info.update.search_cond);
      b = pt_append_nulstring (parser, b, " where ");
      b = pt_append_varchar (parser, b, r1);
    }
  if (p->info.update.using_index)
    {
      if (p->info.update.using_index->info.name.original == NULL)
	{
	  if (p->info.update.using_index->info.name.resolved == NULL)
	    {
	      b = pt_append_nulstring (parser, b, " using index none");
	    }
	  else
	    {
	      assert (p->info.update.using_index->etc
		      == (void *) PT_IDX_HINT_CLASS_NONE);

	      r1 = pt_print_bytes_l (parser, p->info.update.using_index);
	      b = pt_append_nulstring (parser, b, " using index ");
	      b = pt_append_varchar (parser, b, r1);
	    }
	}
      else
	{
	  r1 = pt_print_bytes_l (parser, p->info.update.using_index);
	  b = pt_append_nulstring (parser, b, " using index ");
	  b = pt_append_varchar (parser, b, r1);
	}
    }

  if (p->info.update.order_by)
    {
      r1 = pt_print_bytes_l (parser, p->info.update.order_by);
      b = pt_append_nulstring (parser, b, " order by ");
      b = pt_append_varchar (parser, b, r1);
    }

  if (p->info.update.orderby_for)
    {
      r1 = pt_print_bytes_l (parser, p->info.update.orderby_for);
      b = pt_append_nulstring (parser, b, " for ");
      b = pt_append_varchar (parser, b, r1);
    }

  return b;
}


/* UPDATE_STATS */
/*
 * pt_apply_update_stats () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_update_stats (PARSER_CONTEXT * parser, PT_NODE * p,
		       PT_NODE_FUNCTION g, void *arg)
{
  p->info.update_stats.class_list =
    g (parser, p->info.update_stats.class_list, arg);
  return p;
}

/*
 * pt_init_update_stats () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_update_stats (PT_NODE * p)
{
  p->info.update_stats.class_list = NULL;
  return p;
}

/*
 * pt_print_update_stats () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_update_stats (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0, *r1;

  if (p->info.update_stats.update_stats)
    {
      assert (p->info.update_stats.update_stats == 1);
      b = pt_append_nulstring (parser, b, "update statistics on ");
    }
  else
    {
      b = pt_append_nulstring (parser, b, "analyze statistics on ");
    }

  if (p->info.update_stats.all_classes > 0)
    {
      b = pt_append_nulstring (parser, b, "all tables");
    }
  else if (p->info.update_stats.all_classes < 0)
    {
      b = pt_append_nulstring (parser, b, "catalog tables");
    }
  else
    {
      r1 = pt_print_bytes_l (parser, p->info.update_stats.class_list);
      b = pt_append_varchar (parser, b, r1);
    }

  if (p->info.update_stats.with_fullscan)
    {
      assert (p->info.update_stats.with_fullscan == 1);
      b = pt_append_nulstring (parser, b, " with fullscan");
    }

  return b;
}

/* VALUE */
/*
 * pt_apply_value () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_value (PARSER_CONTEXT * parser, PT_NODE * p,
		PT_NODE_FUNCTION g, void *arg)
{
  switch (p->type_enum)
    {
    case PT_TYPE_SEQUENCE:
      p->info.value.data_value.set =
	g (parser, p->info.value.data_value.set, arg);
    default:
      break;
    }
  return p;
}

/*
 * pt_init_value () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_value (PT_NODE * p)
{
  p->info.value.location = 0;
  return p;
}

/*
 * pt_print_value () -
 *   return:
 *   parser(in):
 *   p(in):
 */
/*
 * Note: The cached statements in the XASL cache are identified by their
 *       string representation (parser_print_tree ()). The strings can
 *       sometimes be identical even if constant literals or values types are
 *       different. For example "select 2;" can be same as "select 2;"
 *       regardless of the value's type (2 can be an integer, a float, a
 *       double, etc.). It is necessary to generate unique string
 *       representations of each pair <value, type_of_value>. The easiest fix
 *       is to wrap the literals in casts like this: "select cast (2 as int);"
 *       and "select cast (2 as float)". However, great care must be exercised
 *       when fixing this as the resulting string might be parsed again by our
 *       SQL parser (for example the strings in view definitions are parsed
 *       during view definition translation, see mq_translate ()).
 *       If a type ambiguity does occur, the XASL cache will return query
 *       results with unexpected types to the client.
 *
 *	 Printing charset introducer and COLLATE modifier of values.
 *	 Four flags control the printing of charset and collate for strings:
 *	  - PT_SUPPRESS_CHARSET_PRINT: when printing columns header in results
 *	  - PT_SUPPRESS_COLLATE_PRINT: some string literals should not
 *	    have COLLATE modifier: in LIKE ESCAPE sequence
 *	  - PT_CHARSET_COLLATE_FULL : printing of already compiled statement
 *	    (view definition, index function)
 *	  - PT_CHARSET_COLLATE_USER_ONLY: printing of an uncompiled statement
 *	    (in HA replication); it prints the statement exactly as the user
 *	    input. This is mutually exclusive with PT_CHARSET_COLLATE_FULL.
 */
/* TODO Investigate the scenarios when this function prints ambiguous strings
 *      and fix the issue by either printing different strings or setting the
 *      print_type_ambiguity flag that disables the caching of the statement.
 */
static PARSER_VARCHAR *
pt_print_value (PARSER_CONTEXT * parser, PT_NODE * p)
{
  DB_VALUE *val;
  PARSER_VARCHAR *q = 0, *r1;
  char s[PT_MEMB_PRINTABLE_BUF_SIZE];
  const char *r;

  /* at first, check NULL value */
  if (p->info.value.db_value_is_initialized)
    {
      val = pt_value_to_db (parser, p);
      if (val)
	{
	  if (DB_IS_NULL (val))
	    {
	      q = pt_append_nulstring (parser, q, "null");

	      if ((parser->custom_print & PT_PRINT_ALIAS)
		  && p->alias_print != NULL)
		{
		  q = pt_append_nulstring (parser, q, " as [");
		  q = pt_append_nulstring (parser, q, p->alias_print);
		  q = pt_append_nulstring (parser, q, "]");
		}

	      return q;
	    }
	}
      else
	{
	  assert (false);
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 1, "");
	}
    }

#if 1				/* TODO - */
  if (PT_IS_NULL_NODE (p))
    {
      q = pt_append_nulstring (parser, q, "null");

      if ((parser->custom_print & PT_PRINT_ALIAS) && p->alias_print != NULL)
	{
	  q = pt_append_nulstring (parser, q, " as [");
	  q = pt_append_nulstring (parser, q, p->alias_print);
	  q = pt_append_nulstring (parser, q, "]");
	}

      return q;
    }

#endif

  switch (p->type_enum)
    {
    case PT_TYPE_SEQUENCE:
#if 1				/* TODO - */
    case PT_TYPE_EXPR_SET:
#endif
      if (p->spec_ident)
	{
	  /* this is tagged as an "in" clause right hand side
	   * Print it as a parenthesized list */
	  r1 = pt_print_bytes_l (parser, p->info.value.data_value.set);
	  q = pt_append_nulstring (parser, q, "(");
	  q = pt_append_varchar (parser, q, r1);
	  q = pt_append_nulstring (parser, q, ")");
	}
      else
	{
	  q = pt_append_nulstring (parser, q, "(");

	  if (p->info.value.data_value.set)
	    {
	      r1 = pt_print_bytes_l (parser, p->info.value.data_value.set);
	      q = pt_append_varchar (parser, q, r1);
	    }

	  q = pt_append_nulstring (parser, q, ")");
	}
      break;

    case PT_TYPE_LOGICAL:
    case PT_TYPE_DOUBLE:
    case PT_TYPE_NUMERIC:
    case PT_TYPE_INTEGER:
    case PT_TYPE_BIGINT:
      if ((p->info.value.text != NULL)
	  && !(parser->custom_print & PT_SUPPRESS_BIGINT_CAST))
	{
	  r = p->info.value.text;
	}
      else
	{
	  switch (p->type_enum)
	    {
	    case PT_TYPE_DOUBLE:
	      OBJ_SPRINT_DB_DOUBLE (s, p->info.value.data_value.d);
	      break;
	    case PT_TYPE_NUMERIC:
	      strcpy (s,
		      (const char *) (p->info.value.data_value.str->bytes));
	      break;
	    case PT_TYPE_INTEGER:
	      sprintf (s, "%ld", p->info.value.data_value.i);
	      break;
	    case PT_TYPE_BIGINT:
	      if (parser->custom_print & PT_SUPPRESS_BIGINT_CAST)
		{
		  sprintf (s, "%lld",
			   (long long) p->info.value.data_value.bigint);
		}
	      else
		{
		  sprintf (s, "cast(%lld as BIGINT)",
			   (long long) p->info.value.data_value.bigint);
		}
	      break;
	    case PT_TYPE_LOGICAL:
	      sprintf (s, "%ld <> 0", p->info.value.data_value.i);
	      break;
	    default:
	      s[0] = '\0';
	      break;
	    }
	  r = s;
	}
      q = pt_append_nulstring (parser, q, r);
      break;

    case PT_TYPE_DATE:
      if (p->info.value.text)
	{
	  q = pt_append_nulstring (parser, q, p->info.value.text);
	  break;
	}
      r = (char *) p->info.value.data_value.str->bytes;
      q = pt_append_nulstring (parser, q, "date ");
      q = pt_append_string_prefix (parser, q, p);
      q = pt_append_quoted_string (parser, q, r, ((r) ? strlen (r) : 0));
      break;
    case PT_TYPE_TIME:
      if (p->info.value.text)
	{
	  q = pt_append_nulstring (parser, q, p->info.value.text);
	  break;
	}
      r = (char *) p->info.value.data_value.str->bytes;
      q = pt_append_nulstring (parser, q, "time ");
      q = pt_append_string_prefix (parser, q, p);
      q = pt_append_quoted_string (parser, q, r, ((r) ? strlen (r) : 0));
      break;
    case PT_TYPE_DATETIME:
      if (p->info.value.text)
	{
	  q = pt_append_nulstring (parser, q, p->info.value.text);
	  break;
	}
      r = (char *) p->info.value.data_value.str->bytes;
      q = pt_append_nulstring (parser, q, "datetime ");
      q = pt_append_string_prefix (parser, q, p);
      q = pt_append_quoted_string (parser, q, r, ((r) ? strlen (r) : 0));
      break;

    case PT_TYPE_VARCHAR:	/* have to check for embedded quotes */
    case PT_TYPE_VARBIT:
      if (p->info.value.text)
	{
	  q = pt_append_nulstring (parser, q, p->info.value.text);

	  break;
	}

      r1 = p->info.value.data_value.str;

      q = pt_append_string_prefix (parser, q, p);
      if (r1)
	{
	  q =
	    pt_append_quoted_string (parser, q, (const char *) r1->bytes,
				     r1->length);
	}
      else
	{
	  q = pt_append_nulstring (parser, q, "''");
	}
      break;
    case PT_TYPE_STAR:		/* as in count (*) */
    case PT_TYPE_OBJECT:
      q = pt_append_nulstring (parser, q, pt_show_type_enum (p->type_enum));
      break;
    default:
      assert (false);
      q = pt_append_nulstring (parser, q, "-- Unknown value type --");
      parser->print_type_ambiguity = 1;
      break;
    }

  if ((parser->custom_print & PT_PRINT_ALIAS) && p->alias_print != NULL)
    {
      q = pt_append_nulstring (parser, q, " as [");
      q = pt_append_nulstring (parser, q, p->alias_print);
      q = pt_append_nulstring (parser, q, "]");
    }

  return q;
}


/* ZZ_ERROR_MSG */
/*
 * pt_apply_error_msg () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_error_msg (UNUSED_ARG PARSER_CONTEXT * parser, PT_NODE * p,
		    UNUSED_ARG PT_NODE_FUNCTION g, UNUSED_ARG void *arg)
{
  return p;
}

/*
 * pt_init_error_msg () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_error_msg (PT_NODE * p)
{
  return p;
}

/*
 * pt_print_error_msg () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_error_msg (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0;
  char s[PT_MEMB_ERR_BUF_SIZE];

  sprintf (s, "near line=%d", p->line_number);
  b = pt_append_nulstring (parser, b, s);
  if (p->column_number > 0)
    {
      sprintf (s, ", col=%d", p->column_number);
      b = pt_append_nulstring (parser, b, s);
    }
  b = pt_append_nulstring (parser, b, ": ");
  b = pt_append_nulstring (parser, b, p->info.error_msg.error_message);
  return b;
}


/* CONSTRAINT */
/*
 * pt_apply_constraint () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_constraint (PARSER_CONTEXT * parser, PT_NODE * p,
		     PT_NODE_FUNCTION g, void *arg)
{
  switch (p->info.constraint.type)
    {
    case PT_CONSTRAIN_NULL:
    case PT_CONSTRAIN_UNKNOWN:
      break;

    case PT_CONSTRAIN_PRIMARY_KEY:
      p->info.constraint.un.primary_key.attrs
	= g (parser, p->info.constraint.un.primary_key.attrs, arg);
      break;

    case PT_CONSTRAIN_NOT_NULL:
      p->info.constraint.un.not_null.attr
	= g (parser, p->info.constraint.un.not_null.attr, arg);
      break;

    case PT_CONSTRAIN_UNIQUE:
      p->info.constraint.un.unique.attrs
	= g (parser, p->info.constraint.un.unique.attrs, arg);
      break;

    case PT_CONSTRAIN_INDEX:
      p->info.constraint.un.index.attrs
	= g (parser, p->info.constraint.un.index.attrs, arg);

    case PT_CONSTRAIN_CHECK:
      p->info.constraint.un.check.expr
	= g (parser, p->info.constraint.un.check.expr, arg);
      break;
    }

  return p;
}

/*
 * pt_init_constraint () -
 *   return:
 *   node(in):
 */
static PT_NODE *
pt_init_constraint (PT_NODE * node)
{
  if (node)
    {
      node->info.constraint.type = PT_CONSTRAIN_UNKNOWN;
      node->info.constraint.name = NULL;
      node->info.constraint.deferrable = 0;
      node->info.constraint.initially_deferred = 0;
    }
  return node;
}

/*
 * pt_print_col_def_constraint () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_col_def_constraint (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0, *r1;

  assert (p->node_type == PT_CONSTRAINT);
  assert (p->info.constraint.name == NULL);

  switch (p->info.constraint.type)
    {
    case PT_CONSTRAIN_UNKNOWN:
      assert (false);
      break;

    case PT_CONSTRAIN_PRIMARY_KEY:
      b = pt_append_nulstring (parser, b, "primary key ");
      break;

    case PT_CONSTRAIN_NULL:
      break;
    case PT_CONSTRAIN_NOT_NULL:
      /*
         Print nothing here. It is a duplicate of the "NOT NULL" printed for
         the column constraint.
       */
      break;

    case PT_CONSTRAIN_UNIQUE:
      b = pt_append_nulstring (parser, b, "unique ");
      break;

    case PT_CONSTRAIN_INDEX:
      break;

    case PT_CONSTRAIN_CHECK:
      r1 = pt_print_bytes (parser, p->info.constraint.un.check.expr);
      b = pt_append_nulstring (parser, b, "check(");
      b = pt_append_varchar (parser, b, r1);
      b = pt_append_nulstring (parser, b, ") ");
      break;
    }

  /*
   * "NOT DEFERRABLE INITIALLY IMMEDIATE" is the default, so print
   * nothing in that case.  It's arguably safer to print the explicit
   * info, but it's also likely to run afoul of SQL parsers that don't
   * understand the full SQL2 jazz yet.
   */
  if (p->info.constraint.deferrable)
    {
      b = pt_append_nulstring (parser, b, "deferrable ");
    }

  if (p->info.constraint.initially_deferred)
    {
      b = pt_append_nulstring (parser, b, "initially deferred ");
    }

  return b;
}

/*
 * pt_print_constraint () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_constraint (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = 0, *r1, *r2;

  if (p->info.constraint.name
      && p->info.constraint.type != PT_CONSTRAIN_INDEX)
    {
      r1 = pt_print_bytes (parser, p->info.constraint.name);
      b = pt_append_nulstring (parser, b, "constraint ");
      b = pt_append_varchar (parser, b, r1);
      b = pt_append_nulstring (parser, b, " ");
    }

  switch (p->info.constraint.type)
    {
    case PT_CONSTRAIN_UNKNOWN:
      b = pt_append_nulstring (parser, b, "unknown ");
      break;

    case PT_CONSTRAIN_PRIMARY_KEY:
      r1 = pt_print_bytes_l (parser, p->info.constraint.un.primary_key.attrs);
      b = pt_append_nulstring (parser, b, "primary key (");
      b = pt_append_varchar (parser, b, r1);
      b = pt_append_nulstring (parser, b, ") ");
      break;

    case PT_CONSTRAIN_NULL:
      break;
    case PT_CONSTRAIN_NOT_NULL:
      /*
         Print nothing here. It is a duplicate of the "NOT NULL" printed for
         the column constraint.
       */
      break;

    case PT_CONSTRAIN_UNIQUE:
      r1 = pt_print_bytes_l (parser, p->info.constraint.un.unique.attrs);
      b = pt_append_nulstring (parser, b, "unique(");
      b = pt_append_varchar (parser, b, r1);
      b = pt_append_nulstring (parser, b, ") ");
      break;

    case PT_CONSTRAIN_INDEX:
      r1 = pt_print_bytes (parser, p->info.constraint.name);
      r2 = pt_print_bytes_l (parser, p->info.constraint.un.index.attrs);
      b = pt_append_nulstring (parser, b, "index ");
      b = pt_append_varchar (parser, b, r1);
      b = pt_append_nulstring (parser, b, "(");
      b = pt_append_varchar (parser, b, r2);
      b = pt_append_nulstring (parser, b, ") ");
      break;

    case PT_CONSTRAIN_CHECK:
      r1 = pt_print_bytes (parser, p->info.constraint.un.check.expr);
      b = pt_append_nulstring (parser, b, "check(");
      b = pt_append_varchar (parser, b, r1);
      b = pt_append_nulstring (parser, b, ") ");
      break;
    }

  /*
   * "NOT DEFERRABLE INITIALLY IMMEDIATE" is the default, so print
   * nothing in that case.  It's arguably safer to print the explicit
   * info, but it's also likely to run afoul of SQL parsers that don't
   * understand the full SQL2 jazz yet.
   */
  if (p->info.constraint.deferrable)
    {
      b = pt_append_nulstring (parser, b, "deferrable ");
    }

  if (p->info.constraint.initially_deferred)
    {
      b = pt_append_nulstring (parser, b, "initially deferred ");
    }

  return b;
}

/* POINTER */

/*
 * pt_apply_pointer () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_pointer (PARSER_CONTEXT * parser, PT_NODE * p, PT_NODE_FUNCTION g,
		  void *arg)
{
  p->info.pointer.node = g (parser, p->info.pointer.node, arg);
  return p;
}

/*
 * pt_init_pointer () -
 *   return:
 *   node(in):
 */
static PT_NODE *
pt_init_pointer (PT_NODE * node)
{
  if (node)
    {
      node->info.pointer.node = NULL;
      node->info.pointer.sel = 0;
      node->info.pointer.rank = 0;
    }

  return node;
}

/*
 * pt_print_pointer () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_pointer (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL;

  if (p)
    {
      b = pt_print_bytes_alias (parser, p->info.pointer.node);
    }

  return b;
}

/*
 * pt_apply_node_list () -
 *   return:
 *   parser(in):
 *   p(in):
 *   g(in):
 *   arg(in):
 */
static PT_NODE *
pt_apply_node_list (PARSER_CONTEXT * parser, PT_NODE * p, PT_NODE_FUNCTION g,
		    void *arg)
{
  p->info.node_list.list = g (parser, p->info.node_list.list, arg);
  return p;
}

/*
 * pt_init_node_list () -
 *   return:
 *   p(in):
 */
static PT_NODE *
pt_init_node_list (PT_NODE * p)
{
  p->info.node_list.list_type = (PT_MISC_TYPE) 0;
  p->info.node_list.list = NULL;
  return p;
}

/*
 * pt_print_node_list () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_node_list (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = pt_print_bytes_l (parser, p->info.node_list.list);
  return b;
}

/*
 * pt_print_index_columns () -
 *   return:
 *   parser(in):
 *   p(in):
 */
static PARSER_VARCHAR *
pt_print_index_columns (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL;

  b = pt_print_bytes_l (parser, p->info.index.column_names);

  return b;
}

/*
 * parser_init_func_vectors () -
 *   return:
 *   parser_init_func_vectors(in):
 */
void
parser_init_func_vectors (void)
{
  if (!pt_apply_f)
    {
      pt_init_apply_f ();
    }
  if (!pt_init_f)
    {
      pt_init_init_f ();
    }
  if (!pt_print_f)
    {
      pt_init_print_f ();
    }
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 *   pt_is_const_expr_node () :
 *   return:
 *   node (in):
 */
bool
pt_is_const_expr_node (PT_NODE * node)
{
  if (node == NULL)
    {
      return false;
    }

  switch (node->node_type)
    {
    case PT_VALUE:
    case PT_HOST_VAR:
      return true;

    case PT_NAME:
      return false;

    case PT_EXPR:
      switch (node->info.expr.op)
	{
	case PT_FUNCTION_HOLDER:
	  {
	    int err = NO_ERROR, num_args = 0, i = 0;
	    bool const_function_holder = false;
	    PT_NODE *function = node->info.expr.arg1;
	    PT_NODE *f_arg = function->info.function.arg_list;

	    /* FUNCTION is const if all arguments of FUNCTION are constant */
	    const_function_holder = true;
	    while (f_arg != NULL)
	      {
		if (pt_is_const_expr_node (f_arg) == false)
		  {
		    const_function_holder = false;
		    break;
		  }
		f_arg = f_arg->next;

	      }
	    return const_function_holder;
	  }
	  break;
	case PT_PLUS:
	case PT_MINUS:
	case PT_TIMES:
	case PT_DIV:
	case PT_BIT_AND:
	case PT_BIT_OR:
	case PT_BIT_XOR:
	case PT_BITSHIFT_LEFT:
	case PT_BITSHIFT_RIGHT:
	case PT_DIVIDE:
	case PT_MOD:
	case PT_MODULUS:
	case PT_LEFT:
	case PT_RIGHT:
	case PT_STRCMP:
	case PT_EQ:
	case PT_NE:
	case PT_GE:
	case PT_GT:
	case PT_LT:
	case PT_LE:
	case PT_NULLSAFE_EQ:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.
					    expr.arg2)) ? true : false;
	case PT_UNARY_MINUS:
	case PT_BIT_NOT:
	case PT_BIT_COUNT:
	  return pt_is_const_expr_node (node->info.expr.arg1);
	case PT_PI:
	case PT_DEFAULTF:
	  return true;
	case PT_FLOOR:
	case PT_CEIL:
	case PT_SIGN:
	case PT_ABS:
	case PT_CHR:
	case PT_EXP:
	case PT_SQRT:
	case PT_ACOS:
	case PT_ASIN:
	case PT_ATAN:
	case PT_COS:
	case PT_SIN:
	case PT_TAN:
	case PT_COT:
	case PT_DEGREES:
	case PT_RADIANS:
	case PT_LN:
	case PT_LOG2:
	case PT_LOG10:
	case PT_DATEF:
	case PT_TIMEF:
	  return pt_is_const_expr_node (node->info.expr.arg1);
	case PT_POWER:
	case PT_ROUND:
	case PT_TRUNC:
	case PT_LOG:
	case PT_DATEDIFF:
	case PT_TIMEDIFF:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.
					    expr.arg2)) ? true : false;
	case PT_INSTR:
	case PT_CONV:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.expr.arg2)
		  && pt_is_const_expr_node (node->info.
					    expr.arg3)) ? true : false;
	case PT_POSITION:
	case PT_FINDINSET:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.
					    expr.arg2)) ? true : false;
	case PT_SUBSTRING_INDEX:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.expr.arg2)
		  && pt_is_const_expr_node (node->info.expr.
					    arg3)) ? true : false;
	case PT_SUBSTRING:
	case PT_LOCATE:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.expr.arg2)
		  && (node->info.expr.arg3 ?
		      pt_is_const_expr_node (node->info.
					     expr.arg3) : true)) ? true :
	    false;
	case PT_CHAR_LENGTH:
	case PT_OCTET_LENGTH:
	case PT_BIT_LENGTH:
	case PT_LOWER:
	case PT_UPPER:
	case PT_HEX:
	case PT_ASCII:
	case PT_BIN:
	case PT_MD5:
	case PT_SHA_ONE:
	case PT_REVERSE:
	case PT_TO_BASE64:
	case PT_FROM_BASE64:
	  return pt_is_const_expr_node (node->info.expr.arg1);
	case PT_TRIM:
	case PT_LTRIM:
	case PT_RTRIM:
	case PT_LIKE_LOWER_BOUND:
	case PT_LIKE_UPPER_BOUND:
	case PT_FROM_UNIXTIME:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && (node->info.expr.arg2 ?
		      pt_is_const_expr_node (node->info.
					     expr.arg2) : true)) ? true :
	    false;

	case PT_LPAD:
	case PT_RPAD:
	case PT_REPLACE:
	case PT_TRANSLATE:
	case PT_INDEX_PREFIX:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.expr.arg2)
		  && (node->info.expr.arg3 ?
		      pt_is_const_expr_node (node->info.
					     expr.arg3) : true)) ? true :
	    false;
	case PT_LAST_DAY:
	  return pt_is_const_expr_node (node->info.expr.arg1);
	case PT_UNIX_TIMESTAMP:
	  if (node->info.expr.arg1)
	    {
	      return pt_is_const_expr_node (node->info.expr.arg1);
	    }
	  else
	    {
	      return true;
	    }
	case PT_MONTHS_BETWEEN:
	case PT_TIME_FORMAT:
	  if (node->info.expr.arg2)
	    {
	      return (pt_is_const_expr_node (node->info.expr.arg1)
		      && pt_is_const_expr_node (node->info.
						expr.arg2)) ? true : false;
	    }
	  else
	    {
	      return (pt_is_const_expr_node (node->info.expr.arg1)) ? true :
		false;
	    }
	case PT_YEARF:
	case PT_MONTHF:
	case PT_DAYF:
	case PT_DAYOFMONTH:
	case PT_HOURF:
	case PT_MINUTEF:
	case PT_SECONDF:
	case PT_QUARTERF:
	case PT_WEEKDAY:
	case PT_DAYOFWEEK:
	case PT_DAYOFYEAR:
	case PT_TODAYS:
	case PT_FROMDAYS:
	case PT_TIMETOSEC:
	case PT_SECTOTIME:
	  return (pt_is_const_expr_node (node->info.expr.arg1)) ? true :
	    false;
	case PT_SCHEMA:
	case PT_DATABASE:
	case PT_VERSION:
	  return true;
	case PT_ATAN2:
	case PT_FORMAT:
	case PT_ADDDATE:
	case PT_DATE_ADD:
	case PT_SUBDATE:
	case PT_DATE_SUB:
	case PT_DATE_FORMAT:
	case PT_STR_TO_DATE:
	case PT_REPEAT:
	case PT_MAKEDATE:
	case PT_WEEKF:
	case PT_SHA_TWO:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.
					    expr.arg2)) ? true : false;
	case PT_SYS_DATE:
	case PT_SYS_TIME:
	case PT_SYS_DATETIME:
	case PT_UTC_TIME:
	case PT_UTC_DATE:
	case PT_HA_STATUS:
	case PT_SHARD_GROUPID:
	case PT_SHARD_LOCKNAME:
	case PT_SHARD_NODEID:
	case PT_EXPLAIN:
	case PT_CURRENT_USER:
	case PT_USER:
	case PT_LIST_DBS:
	  return true;
	case PT_TO_CHAR:
	case PT_TO_DATE:
	case PT_TO_TIME:
	case PT_TO_DATETIME:
	case PT_TO_NUMBER:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && (node->info.expr.arg2 ?
		      pt_is_const_expr_node (node->info.
					     expr.arg2) : true)) ? true :
	    false;
	case PT_CAST:
	  return pt_is_const_expr_node (node->info.expr.arg1);
	case PT_CASE:
	case PT_DECODE:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.
					    expr.arg2)) ? true : false;
	case PT_IF:
	  return (pt_is_const_expr_node (node->info.expr.arg2)
		  && pt_is_const_expr_node (node->info.
					    expr.arg3)) ? true : false;
	case PT_IFNULL:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.
					    expr.arg2)) ? true : false;
	case PT_ISNULL:
	  return pt_is_const_expr_node (node->info.expr.arg1);
	case PT_CONCAT:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && (node->info.expr.arg2 ?
		      pt_is_const_expr_node (node->info.
					     expr.arg2) : true)) ? true :
	    false;
	case PT_CONCAT_WS:
	case PT_FIELD:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && (node->info.expr.arg2 ?
		      pt_is_const_expr_node (node->info.expr.arg2) : true)
		  && pt_is_const_expr_node (node->info.
					    expr.arg3)) ? true : false;
	case PT_NULLIF:
	case PT_COALESCE:
	case PT_NVL:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.
					    expr.arg2)) ? true : false;
	case PT_NVL2:
	case PT_MID:
	case PT_MAKETIME:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.expr.arg2)
		  && pt_is_const_expr_node (node->info.
					    expr.arg3)) ? true : false;
	case PT_EXTRACT:
	  return pt_is_const_expr_node (node->info.expr.arg1);
	case PT_LEAST:
	case PT_GREATEST:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.
					    expr.arg2)) ? true : false;
	case PT_INET_ATON:
	case PT_INET_NTOA:
	  return pt_is_const_expr_node (node->info.expr.arg1);
	case PT_WIDTH_BUCKET:
	  return (pt_is_const_expr_node (node->info.expr.arg1)
		  && pt_is_const_expr_node (node->info.expr.arg2)
		  && pt_is_const_expr_node (node->info.expr.arg3));
	default:
	  return false;
	}

    default:
      return false;
    }

  return false;
}
#endif

/*
 * pt_restore_assignment_links - restore assignments links after a call to
 *  get_assignments_lists.
 *   return:
 *   assigns(in): first node of original assignment list
 *   links(in): The links array returned by get_assignment lists
 *   count(in): count of links in links array. This is used in
 *   get_assignments_lists if an error occurs. If this is -1 then just iterate
 *   through assignments list while restoring it, until the next statement is
 *   NULL.
 *
 * Note:
 *  The links array is freed
 */
void
pt_restore_assignment_links (PT_NODE * assigns, PT_NODE ** links, int count)
{
  PT_NODE *lhs = NULL, *rhs = NULL, *att = NULL;
  int links_idx = 0;

  while (assigns && (links_idx < count || count == -1))
    {
      lhs = assigns->info.expr.arg1;
      rhs = assigns->info.expr.arg2;

      for (att = lhs; att && (links_idx < count || count == -1);
	   att = att->next)
	{
	  assert (att->node_type == PT_NAME);
	  att->next = links[links_idx++];
	}

      rhs->next = links[links_idx++];
      assigns = assigns->next;
    }

  /* free links array */
  if (links)
    {
      free_and_init (links);
    }
}

/*
 * pt_get_assignment_lists - Returns corresponding lists of names and
 *			     expressions
 *   return: Error code
 *   parser(in): Parser context
 *   select_names(out):
 *   select_values(out):
 *   assign(in): Parse tree of assignment lists
 *
 * Note:
 */
int
pt_get_assignment_lists (UNUSED_ARG PARSER_CONTEXT * parser,
			 PT_NODE ** select_names, PT_NODE ** select_values,
			 int *no_vals, PT_NODE * assign,
			 PT_NODE *** old_links)
{
#define ASSIGN_LINKS_EXTENT	10

  int error = NO_ERROR;
  int links_chunk = ASSIGN_LINKS_EXTENT, links_alloc = ASSIGN_LINKS_EXTENT;
  int links_idx = 0;

  PT_NODE *lhs, *rhs, *att;
  PT_NODE **links, **new_links;

  links = (PT_NODE **) malloc (links_alloc * sizeof (PT_NODE *));
  if (!links)
    {
      error = ER_OUT_OF_VIRTUAL_MEMORY;
      goto exit_on_error;
    }

  if (!select_names || !select_values || !no_vals)
    {
      /* bullet proofing, should not get here */
#if defined(RYE_DEBUG)
      fprintf (stdout, "system error detected in %s, line %d.\n",
	       __FILE__, __LINE__);
#endif
      error = ER_GENERIC_ERROR;
      goto exit_on_error;
    }

  *select_names = *select_values = NULL;
  *no_vals = 0;

  while (assign)
    {
      if (assign->node_type != PT_EXPR || assign->info.expr.op != PT_ASSIGN
	  || !(lhs = assign->info.expr.arg1)
	  || !(rhs = assign->info.expr.arg2) || lhs->node_type != PT_NAME)
	{
	  /* bullet proofing, should not get here */
#if defined(RYE_DEBUG)
	  fprintf (stdout, "system error detected in %s, line %d.\n",
		   __FILE__, __LINE__);
#endif
	  error = ER_GENERIC_ERROR;
	  goto exit_on_error;
	}

      for (att = lhs; att; att = att->next)
	{
	  if (att->node_type != PT_NAME)
	    {
#if defined(RYE_DEBUG)
	      fprintf (stdout, "system error detected in %s, line %d.\n",
		       __FILE__, __LINE__);
#endif
	      error = ER_GENERIC_ERROR;
	      goto exit_on_error;
	    }

	  /* allocate more space if needed */
	  if (links_idx >= links_alloc)
	    {
	      links_alloc += links_chunk;
	      new_links = (PT_NODE **) realloc (links,
						links_alloc *
						sizeof (PT_NODE *));
	      if (new_links == NULL)
		{
		  error = ER_OUT_OF_VIRTUAL_MEMORY;
		  goto exit_on_error;
		}
	      links = new_links;
	    }

	  links[links_idx++] = att->next;
	  ++(*no_vals);
	}

      /* allocate more space if needed */
      if (links_idx >= links_alloc)
	{
	  links_alloc += links_chunk;
	  new_links = (PT_NODE **) realloc (links,
					    links_alloc * sizeof (PT_NODE *));
	  if (new_links == NULL)
	    {
	      error = ER_OUT_OF_VIRTUAL_MEMORY;
	      goto exit_on_error;
	    }
	  links = new_links;
	}

      links[links_idx++] = rhs->next;

      /* assume evaluation needed. */
      if (*select_names == NULL)
	{
	  *select_names = lhs;
	  *select_values = rhs;
	}
      else
	{
	  parser_append_node (lhs, *select_names);
	  parser_append_node (rhs, *select_values);
	}

      assign = assign->next;
    }
  *old_links = links;
  return error;

exit_on_error:
  if (links != NULL)
    {
      pt_restore_assignment_links (assign, links, links_idx);
    }
  *old_links = NULL;

  return error;
}

/*
 *   pt_sort_spec_list_to_name_node_list () : creates a list of name nodes
 *					      from sort spec nodes list
 *   return: name list if sort spec nodes contain only name nodes,
 *	     NULL otherwise
 *   expr(in): sort spec list
 *   parser(in):
 */
PT_NODE *
pt_sort_spec_list_to_name_node_list (PARSER_CONTEXT * parser,
				     PT_NODE * sort_spec_list)
{
  PT_NODE *name_list = NULL;
  PT_NODE *node = NULL, *name_node = NULL;

  for (node = sort_spec_list; node; node = node->next)
    {
      if (!PT_IS_SORT_SPEC_NODE (node) ||
	  node->info.sort_spec.expr->node_type != PT_NAME)
	{
	  return NULL;
	}
    }

  for (node = sort_spec_list; node; node = node->next)
    {
      name_node = parser_copy_tree (parser, node->info.sort_spec.expr);
      if (node->info.sort_spec.asc_or_desc == PT_DESC)
	{
	  PT_NAME_INFO_SET_FLAG (name_node, PT_NAME_INFO_DESC);
	}

      name_list = parser_append_node (name_node, name_list);
    }

  return name_list;
}

/*
 * pt_apply_query_trace ()
 * return :
 * parser (in) :
 * p (in) :
 * g (in) :
 * arg (in) :
 */
static PT_NODE *
pt_apply_query_trace (UNUSED_ARG PARSER_CONTEXT * parser, PT_NODE * p,
		      UNUSED_ARG PT_NODE_FUNCTION g, UNUSED_ARG void *arg)
{
  return p;
}

/*
 * pt_init_query_trace ()
 * return :
 * p (in) :
 */
static PT_NODE *
pt_init_query_trace (PT_NODE * p)
{
  p->info.trace.on_off = PT_TRACE_OFF;
  p->info.trace.format = PT_TRACE_FORMAT_TEXT;

  return p;
}

/*
 * pt_print_query_trace ()
 * return :
 * parser (in) :
 * p (in) :
 */
static PARSER_VARCHAR *
pt_print_query_trace (PARSER_CONTEXT * parser, PT_NODE * p)
{
  PARSER_VARCHAR *b = NULL;
  PT_MISC_TYPE onoff, format;

  onoff = p->info.trace.on_off;
  format = p->info.trace.format;

  b = pt_append_nulstring (parser, b, "set trace ");
  b = pt_append_nulstring (parser, b, pt_show_misc_type (onoff));

  if (onoff == PT_TRACE_ON)
    {
      b = pt_append_nulstring (parser, b, " output ");
      b = pt_append_nulstring (parser, b, pt_show_misc_type (format));
    }

  return b;
}

/*
 * pt_init_query_info ()
 * return :
 * query (in) :
 */
static PT_QUERY_INFO *
pt_init_query_info (PT_QUERY_INFO * query)
{
  assert (query != NULL);

  if (query != NULL)
    {
      query->order_by = NULL;
      query->orderby_for = NULL;
      query->all_distinct = PT_ALL;
      query->is_subquery = (PT_MISC_TYPE) 0;
      query->is_view_spec = 0;
      query->has_outer_spec = 0;
      query->is_sort_spec = 0;
      query->is_insert_select = 0;
      query->single_tuple = 0;
      query->vspec_as_derived = 0;
      query->reexecute = 0;
      query->do_cache = 0;
      query->do_not_cache = 0;
      query->order_siblings = 0;
      query->rewrite_limit = 0;
      query->hint = PT_HINT_NONE;
      query->qcache_hint = NULL;
      query->upd_del_class_cnt = 0;
    }

  return query;
}
