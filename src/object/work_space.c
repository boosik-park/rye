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
 * work_space.c - Workspace Manager
 */

#ident "$Id$"

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "memory_alloc.h"
#include "message_catalog.h"
#include "memory_hash.h"
#include "error_manager.h"
#include "oid.h"
#include "work_space.h"
#include "schema_manager.h"
#include "authenticate.h"
#include "object_accessor.h"
#include "locator_cl.h"
#include "storage_common.h"
#include "system_parameter.h"
#include "set_object.h"
#include "object_primitive.h"
#include "class_object.h"
#include "environment_variable.h"
#include "db.h"
#include "transaction_cl.h"
#include "object_template.h"
#include "server_interface.h"

/* this must be the last header file included!!! */
#include "dbval.h"

extern unsigned int db_on_server;

/*
 * need these to get the allocation areas initialized, avoid including
 * the entire file
 */

/*
 * ws_Commit_mops
 *    Linked list of mops to be reset at commit/abort.
 */

MOP ws_Commit_mops = NULL;

/*
 * ws_Mop_table
 *    This is the OID to MOP hash table.  This is public ONLY to allow
 *    some performance related mapping macros to be used by the
 *    transaction manager.
 */

WS_MOP_TABLE_ENTRY *ws_Mop_table = NULL;

/*
 * ws_Mop_table_size
 *    Records the current size of the OID to MOP hash table.
 */

unsigned int ws_Mop_table_size = 0;


/*
 * ws_Resident_classes
 *    This is a global list of resident class objects.
 *    Since the root of the class' resident instance list is kept in
 *    the class_link field of the class MOP, we can't use this field
 *    to chain the list of resident class objects.  Instead keep an object
 *    list.
 */

DB_OBJLIST *ws_Resident_classes = NULL;

/*
 * ws_Stats
 *    Workspace statistics structure.
 *    This contains random information about the state of the workspace.
 */

WS_STATISTICS ws_Stats = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

int ws_Num_dirty_mop = 0;

/*
 * We used to keep a global dirty list here. But for more efficient traversals
 * the dirty list has been reorganized into a dirty list by class. To visit
 * all the dirty objects in this workspace, start with the resident class list
 * and visit each class' dirty list. The dirty flag here is consulted by
 * ws_has_updated to determine if there are dirty objects in this workspace.
 */
static bool Ws_dirty;

/*
 * Null_object
 *    This is used at the terminator for the dirty_link and class_link fieids
 *    of the MOP if they are the last in the list.  This allows us to
 *    determine if the MOP has been added to a class list simply by
 *    checking to see if the field is NULL.  If it is non-NULL, we know it
 *    must be in the list (even if it is at the end of the list).  This
 *    avoids having to keep an extra bit in the MOP structre.
 */

static MOP Null_object;

/*
 * Classname_cache
 *    This is a hash table used to cache the class name to MOP mapping
 *    on the client side.  This avoids repeated calls to the server to
 *    find class OIDs.
 */

static MHT_TABLE *Classname_cache = NULL;

static WS_REPL_FLUSH_ERR *ws_Repl_error_link = NULL;

static WS_REPL_LIST ws_Repl_objs;

static MOP ws_make_mop (OID * oid);
static void ws_free_mop (MOP op);
#if defined (ENABLE_UNUSED_FUNCTION)
static void emergency_remove_dirty (MOP op);
#endif
static int ws_map_dirty_internal (MAPFUNC function, void *args,
				  bool classes_only);
static int add_class_object (MOP class_mop, MOP obj);
static void remove_class_object (MOP class_mop, MOP obj);
static int mark_instance_deleted (MOP op, void *args);
static void ws_clear_internal (void);
#if defined (RYE_DEBUG)
static void ws_print_oid (OID * oid);
static int ws_describe_mop (MOP mop, void *args);
#endif
#if !defined (NDEBUG)
static int ws_check_hash_link (int slot);
#endif
static void ws_insert_mop_on_hash_link (MOP mop, int slot);
static void ws_insert_mop_on_hash_link_with_position (MOP mop, int slot,
						      MOP prev);

/*
 * MEMORY CRISES
 */

/*
 * ws_abort_transaction - callback routine for the qf module that is called
 *                        when storage is exhausted and an allocation can
 *                        not be serviced
 *     return: void
 */
void
ws_abort_transaction (void)
{
  if (db_Disable_modifications)
    {
      if (er_errid () != ER_OUT_OF_VIRTUAL_MEMORY)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
		  1, 0);
	}
    }
  else
    {
      /* might want to keep a chunk of memory in reserve here so we can free it
         in case we need to do any small allocations during the abort process */

      (void) tran_unilaterally_abort ();

      /* couldn't get to the catalog, use hard coded strings */
      fprintf (stdout,
	       "Rye cannot allocate main memory and must halt execution.\n");
      fprintf (stdout, "The current transaction has been aborted.\n");
      fprintf (stdout, "Data integrity has been preserved.\n");
    }
}

/*
 * MOP ALLOCATION AND TABLE MAINTENANCE
 */

/*
 * ws_make_mop - allocates a storage for a new mop
 *    return: MOP structure
 *    oid(in): oid for a new mop
 */
static MOP
ws_make_mop (OID * oid)
{
  MOP op;

  op = (MOP) malloc (sizeof (DB_OBJECT));
  if (op != NULL)
    {
      op->class_mop = NULL;
      op->object = NULL;
      op->class_link = NULL;
      op->dirty_link = NULL;
      op->dirty = 0;
      op->deleted = 0;
      op->pinned = 0;
      op->no_objects = 0;
      op->ws_lock = NULL_LOCK;
      op->hash_link = NULL;
      op->commit_link = NULL;
      OID_SET_NULL (&(op->ws_oid));

      /* this is NULL only for the Null_object hack */
      if (oid != NULL)
	{
	  COPY_OID (WS_OID (op), oid);
	}
      else
	{
	  OID_SET_NULL (WS_OID (op));
	}

      ws_Stats.mops_allocated++;
    }
  else
    {
      /* couldnt' allocate a MOP, mgc should have set an error by now */
      ws_abort_transaction ();
    }

  return (op);
}

/*
 * ws_free_mop - frees a MOP
 *    return: void
 *    op(in/out): MOP pointer
 *
 * Note: This was introduced primarily to handle the new MOP property
 * lists.  MOPS can only really be freed through garbage collection.
 */
static void
ws_free_mop (MOP op)
{
  free (op);
}

#if !defined (NDEBUG)
static int
ws_check_hash_link (int slot)
{
  MOP head, tail;
  MOP p, q;
  int c;

  head = ws_Mop_table[slot].head;
  tail = ws_Mop_table[slot].tail;

  p = head;
  if (p == NULL)
    {
      /* empty list */
      assert (head == NULL && tail == NULL);
    }
  else if (p->hash_link == NULL)
    {
      /* only one node */
      assert (head == p && tail == p);
    }
  else
    {
      /* more than one node */
      for (q = p->hash_link; q; p = q, q = q->hash_link)
	{
	  c = oid_compare (WS_OID (p), WS_OID (q));
	  assert (c <= 0);
	}
      assert (p == tail);
    }

  return NO_ERROR;
}
#endif

static void
ws_insert_mop_on_hash_link (MOP mop, int slot)
{
  MOP p;
  MOP prev = NULL;
  int c;

  /* to find the appropriate position */
  p = ws_Mop_table[slot].tail;
  if (p)
    {
      c = oid_compare (WS_OID (mop), WS_OID (p));

      if (c > 0)
	{
	  /* mop is greater than the tail */
	  p->hash_link = mop;
	  mop->hash_link = NULL;
	  ws_Mop_table[slot].tail = mop;

	  return;
	}

      /* Unfortunately, we have to navigate the list when c == 0,
       * because there can be redundancies of mops which have the same oid,
       * in case of VID.
       * Under 'Create table A -> rollback -> Create table B' scenario,
       * the oid of the mop of table B can be same as that of table A.
       * Because the newest one is located at the head of redundancies
       * in that case, we use the first fit method.
       */
    }

  for (p = ws_Mop_table[slot].head; p != NULL; prev = p, p = p->hash_link)
    {
      c = oid_compare (WS_OID (mop), WS_OID (p));

      if (c <= 0)
	{
	  /* Unfortunately, we have to navigate the list when c == 0 */
	  /* See the above comment */
	  break;
	}
    }

  if (p == NULL)
    {
      /* empty or reach at the tail of the list */
      ws_Mop_table[slot].tail = mop;
    }

  if (prev == NULL)
    {
      mop->hash_link = ws_Mop_table[slot].head;
      ws_Mop_table[slot].head = mop;
    }
  else
    {
      mop->hash_link = prev->hash_link;
      prev->hash_link = mop;
    }
}

static void
ws_insert_mop_on_hash_link_with_position (MOP mop, int slot, MOP prev)
{
  if (prev == NULL)
    {
      if (ws_Mop_table[slot].tail == NULL)
	{
	  /* empty list */
	  ws_Mop_table[slot].tail = mop;
	}
      mop->hash_link = ws_Mop_table[slot].head;
      ws_Mop_table[slot].head = mop;
    }
  else
    {
      if (prev->hash_link == NULL)
	{
	  /* append mop on the tail of the list */
	  ws_Mop_table[slot].tail = mop;
	}
      mop->hash_link = prev->hash_link;
      prev->hash_link = mop;
    }
}

/*
 * ws_mop - given a oid, find or create the corresponding MOP and add it to
 * the workspace object table.
 *    return: MOP
 *    oid(in): oid
 *    class_mop(in): optional class MOP (can be null if not known)
 *
 * Note: If the class argument is NULL, it will be added to the class list
 * when the object is cached.
 */

MOP
ws_mop (OID * oid, MOP class_mop)
{
  MOP mop, new_mop, prev;
  unsigned int slot;
  int c;

  if (OID_ISNULL (oid))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_WS_CANT_INSTALL_NULL_OID,
	      0);
      return NULL;
    }

  /* look for existing entry */
  slot = OID_PSEUDO_KEY (oid);
  if (slot >= ws_Mop_table_size)
    {
      slot = slot % ws_Mop_table_size;
    }

  /* compare with the last mop */
  prev = NULL;
  mop = ws_Mop_table[slot].tail;
  if (mop)
    {
      c = oid_compare (oid, WS_OID (mop));
      if (c > 0)
	{
	  /* 'oid' is greater than the tail,
	   * which means 'oid' does not exist in the list
	   *
	   * NO need to traverse the list!
	   */
	  prev = ws_Mop_table[slot].tail;
	}
      else
	{
	  /* c <= 0 */

	  /* Unfortunately, we have to navigate the list when c == 0 */
	  /* See the comment of ws_insert_mop_on_hash_link() */

	  for (mop = ws_Mop_table[slot].head; mop != NULL;
	       prev = mop, mop = mop->hash_link)
	    {
	      c = oid_compare (oid, WS_OID (mop));
	      if (c == 0)
		{
		  return mop;
		}
	      else if (c < 0)
		{
		  /* find the node which is greater than I */
		  break;
		}
	    }
	}
    }

  /* make a new mop entry */
  new_mop = ws_make_mop (oid);
  if (new_mop == NULL)
    {
      return NULL;
    }

  if (class_mop != NULL)
    {
      if (add_class_object (class_mop, new_mop))
	{
	  ws_free_mop (new_mop);
	  return NULL;
	}
    }

  /* install it into this slot list */
  ws_insert_mop_on_hash_link_with_position (new_mop, slot, prev);
  assert (ws_check_hash_link (slot) == NO_ERROR);

  return new_mop;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * ws_new_mop - optimized version of ws_mop when OID being entered into the
 * workspace is guaranteed to be unique.
 *    return: new MOP
 *    oid(in): object OID
 *    class_mop(in): class mop of object
 *
 * Note:
 *    This happens when temporary OIDs are generated for newly created objects.
 *    It assumes that the MOP must be created and does not bother searching
 *    the hash table collision list looking for duplicates.
 */
MOP
ws_new_mop (OID * oid, MOP class_mop)
{
  MOP mop;
  unsigned int slot;

  mop = NULL;
  if (OID_ISNULL (oid))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_WS_CANT_INSTALL_NULL_OID,
	      0);
      return NULL;
    }

  slot = OID_PSEUDO_KEY (oid);
  if (slot >= ws_Mop_table_size)
    {
      slot = slot % ws_Mop_table_size;
    }

  mop = ws_make_mop (oid);
  if (mop == NULL)
    {
      return NULL;
    }

  if (class_mop != NULL)
    {
      if (add_class_object (class_mop, mop))
	{
	  ws_free_mop (mop);
	  return NULL;
	}
    }

  ws_insert_mop_on_hash_link (mop, slot);
  assert (ws_check_hash_link (slot) == NO_ERROR);

  return (mop);
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * ws_perm_oid_and_class - change the OID of a MOP and recache the class mop
 *			   if it has been changed
 *    return: void
 *    mop(in/out)   : MOP whose OID needs to be changed
 *    newoid(in)    : new OID
 *    new_class_oid : new class OID
 *
 * Note:
 *    This is only called by the transaction locator as OIDs need to be
 *    flushed and must be converted to permanent OIDs before they are given
 *    to the server.
 *
 *    This assumes that the new permanent OID is guaranteed to be
 *    unique and we can avoid searching the hash table collision list
 *    for existing MOPs with this OID.  This makes the conversion faster.
 */
int
ws_perm_oid_and_class (MOP mop, OID * new_oid, OID * new_class_oid)
{
  MOP class_mop = NULL;
  bool relink = false;

  class_mop = ws_class_mop (mop);
  if (!OID_ISTEMP ((OID *) WS_OID (mop)))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_WS_MOP_NOT_TEMPORARY, 0);
      return ER_FAILED;
    }

  if (!OID_EQ (WS_OID (class_mop), new_class_oid))
    {
      /* we also need to disconnect this instance from class_mop and add it
         to new_class_oid */
      remove_class_object (class_mop, mop);
      relink = true;
      class_mop = ws_mop (new_class_oid, NULL);
      if (class_mop == NULL)
	{
	  assert (false);
	  return ER_FAILED;
	}
    }

  /* Make sure that we have the new class in workspace */
  if (class_mop->object == NULL)
    {
      int error = NO_ERROR;
      SM_CLASS *smclass = NULL;
      /* No need to check authorization here */
      error = au_fetch_class_force (class_mop, &smclass, S_LOCK);
      if (error != NO_ERROR)
	{
	  assert (false);
	  return error;
	}
    }
  mop->class_mop = class_mop;
  ws_perm_oid (mop, new_oid);
  if (relink)
    {
      add_class_object (class_mop, mop);
    }
  return NO_ERROR;
}

/*
 * ws_perm_oid - change the OID of a MOP
 *    return: void
 *    mop(in/out): MOP whose OID needs to be changed
 *    newoid(in): new OID
 *
 * Note:
 *    This is only called by the transaction locator as OIDs need to be
 *    flushed and must be converted to permanent OIDs before they are given
 *    to the server.
 *
 *    This assumes that the new permanent OID is guaranteed to be
 *    unique and we can avoid searching the hash table collision list
 *    for existing MOPs with this OID.  This makes the conversion faster.
 */
void
ws_perm_oid (MOP mop, OID * newoid)
{
  MOP mops, prev;
  unsigned int slot;

  if (!OID_ISTEMP ((OID *) WS_OID (mop)))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_WS_MOP_NOT_TEMPORARY, 0);
      return;
    }

  /* find current entry */
  slot = OID_PSEUDO_KEY (WS_OID (mop));
  if (slot >= ws_Mop_table_size)
    {
      slot = slot % ws_Mop_table_size;
    }

  mops = ws_Mop_table[slot].head;
  for (prev = NULL; mops != mop && mops != NULL; mops = mops->hash_link)
    {
      prev = mops;
    }

  if (mops != mop)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_WS_MOP_NOT_FOUND, 0);
      return;
    }

  /* remove the current entry */
  if (prev == NULL)
    {
      ws_Mop_table[slot].head = mop->hash_link;
    }
  else
    {
      prev->hash_link = mop->hash_link;
    }

  if (ws_Mop_table[slot].tail == mop)
    {
      /* I was the tail of the list */
      ws_Mop_table[slot].tail = prev;
    }

  assert (ws_check_hash_link (slot) == NO_ERROR);

  mop->hash_link = NULL;

  /* assign the new oid */
  COPY_OID (WS_OID (mop), newoid);

  /* force the MOP into the table at the new slot position */
  slot = OID_PSEUDO_KEY (newoid);
  if (slot >= ws_Mop_table_size)
    {
      slot = slot % ws_Mop_table_size;
    }

  ws_insert_mop_on_hash_link (mop, slot);
  assert (ws_check_hash_link (slot) == NO_ERROR);
}

/*
 * INTERNAL GARBAGE COLLECTOR
 */

/*
 * ws_disconnect_deleted_instances - called when a class MOP is being garbage
 * collected
 *    return: void
 *    classop(in/out): class MOP
 *
 * Note:
 *    This should only happen if the class has been deleted and is no
 *    longer on the resident class list.
 *    At this point, all instances should have been flushed and decached.
 *    Here we make sure that any instance MOPs connected to this class
 *    get disconnected.
 */
void
ws_disconnect_deleted_instances (MOP classop)
{
  MOP m, next;

  if (classop == sm_Root_class_mop)
    {
      return;
    }

  for (m = classop->class_link, next = NULL;
       m != Null_object && m != NULL; m = next)
    {
      next = m->class_link;

      if (m->object != NULL)
	{
	  /*
	   * there should be no cached object here ! since the class is gone,
	   * we no longer no how to free this. If this becomes a normal case,
	   * we'll have to wait and decache the class AFTER all the instances
	   * have been decached
	   */
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_WS_CORRUPTED, 0);
	  m->object = NULL;
	}

      m->class_link = NULL;
      m->class_mop = NULL;
    }
  classop->class_link = Null_object;

}

/*
 * ws_remove_resident_class - remove a class from the resident class list.
 *    return: void
 *    classop(in/out): class mop
 *
 * Note:
 *    This should ONLY be called if the class has been deleted and all of
 *    the instances have been decached.
 *    Once the class MOP is removed from the resident class list, it will
 *    be subject to garbage collection.
 *    This can be called from the schema manager when it
 *    finishes deleting a class.
 */
void
ws_remove_resident_class (MOP classop)
{
  if (classop != sm_Root_class_mop)
    {
      /* make sure we don't have anyone referencing us */
      ws_disconnect_deleted_instances (classop);
      ml_remove (&ws_Resident_classes, classop);
    }
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * emergency_remove_dirty - reclaim a MOP that is on the dirty list.
 *    return: void
 *    op(in/out): mop that needs to be garbage collected
 *
 * Note:
 *    This should never be called.
 */
static void
emergency_remove_dirty (MOP op)
{
  MOP mop, prev;

  /*
   * make sure we can get to op's class dirty list because without that
   * there is no dirty list from which we can remove op.
   */
  if (op->dirty_link != NULL
      && op->class_mop != NULL && op->class_mop->dirty_link != NULL)
    {
      /* search for op in op's class' dirty list */
      prev = NULL;

      for (mop = op->class_mop->dirty_link;
	   mop != Null_object && mop != op; mop = mop->dirty_link)
	{
	  prev = mop;
	}

      /* remove op from op's class' dirty list */
      if (mop == op)
	{
	  if (prev == NULL)
	    {
	      op->class_mop->dirty_link = op->dirty_link;
	    }
	  else
	    {
	      prev->dirty_link = op->dirty_link;
	    }
	  op->dirty_link = NULL;
	}
    }
}

/*
 * ws_intern_instances - flush and cull all MOPs of given class MOP
 *    return: void
 *    class_mop(in): class MOP
 */
void
ws_intern_instances (MOP class_mop)
{
  if (locator_flush_all_instances (class_mop, DECACHE) != NO_ERROR)
    {
      return;
    }

  ws_filter_dirty ();
}
#endif

/*
 * DIRTY LIST MAINTENANCE
 */

/*
 * The dirty list which used to be a global dirty list is now kept by class.
 * A dirty list (possibly empty) is rooted at each class' dirty_link and is
 * chained through the dirty_link field in the object_pointer.  This makes
 * maintenance of the dirty_list very simple at the expense of an object_pointer
 * that is one word larger.
 *
 * When an object is marked as "clean" it is not immediately removed from the
 * dirty list.  Since we don`t have a doubly linked list, we will need to
 * perform a linear search of the dirty list in order to remove the element.
 * Physicaly altering the dirty list as objects are "cleaned" also has
 * unpleasant side effects for the dirty object iterator function below.
 *
 * Instead, the dirty object iterator will remove objects from the dirty list
 * as it sweeps through them.
 *
 * Note that doing this also requires an extra "dirty bit" in addition to the
 * dirty list link field.
 */

/*
 * ws_dirty - Add an object to the dirty list of its class.
 *    return: void
 *    op(in/out): mop to make dirty
 */
void
ws_dirty (MOP op)
{
  /*
   * don't add the root class to any dirty list. otherwise, later traversals
   * of that dirty list will loop forever.
   */

  if (op == NULL || op == sm_Root_class_mop)
    {
      return;
    }
  WS_SET_DIRTY (op);
  /*
   * add_class_object makes sure each class' dirty list (even an empty one)
   * is always terminated by the magical Null_object. Therefore, this test
   * "op->dirty_link == NULL" makes sure class objects are not added to
   * the Rootclass' dirty list.
   */
  if (op->dirty_link != NULL)
    {
      return;
    }

  if (op->class_mop == NULL)
    {
      /* SERIOUS INTERNAL ERROR */
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_WS_CLASS_NOT_CACHED, 0);
      ws_Stats.uncached_classes++;
    }
  else
    {
      /*
       * add op to op's class' dirty list only if op is not yet there.
       * The preceding "op->dirty_link == NULL" asserts that op is not
       * on any dirty list so we can simply prepend op to op's class'
       * dirty list.
       */
      op->dirty_link = op->class_mop->dirty_link;
      op->class_mop->dirty_link = op;
    }

}

/*
 * ws_clean - clears the dirty bit of a mop
 *    return: void
 *    op(in/out): mop to mark clean
 *
 * Note:
 * Making dirty bit cleared will cause the object to be ignored by the
 * dirty list iterator
 */
void
ws_clean (MOP op)
{
  /*
   * because pinned objects can be in a state of direct modification, we
   * can't reset the dirty bit after a workspace panic flush because this
   * would lose any changes made to the pinned object after the flush
   */

  if (!op->pinned)
    {
      WS_RESET_DIRTY (op);
    }
  else
    {
      ws_Stats.pinned_cleanings++;	/* need to know how often this happens */
    }
}

/*
 * ws_map_dirty_internal - iterate over elements in the dirty list calling map
 * function with the element.
 *    return: map status code
 *    function(in): function to apply to the dirty list elements
 *    args(in): arguments to pass to map function
 *    classes_only(in): flag indicating map over class objects only
 *
 * Note:
 *    As a side effect, non-dirty objects that are still in the dirty list
 *    are removed.  The map function must return WS_MAP_CONTINUE each time
 *    to map over the entire list.  If the map function returns any other
 *    value, the loop will terminate.  The function will return
 *    WS_MAP_SUCCESS if the loop completed or if the map function never
 *    returned WS_MAP_FAIL.  If the map function returns WS_MAP_FAIL, the
 *    loop will terminate and this will be returned from the function.
 */
static int
ws_map_dirty_internal (MAPFUNC function, void *args, bool classes_only)
{
  MOP op, op2, next, prev, class_mop;
  DB_OBJLIST *m;
  int status = WS_MAP_CONTINUE;
  int collected_num_dirty_mop = 0;

  /* traverse the resident classes to get to their dirty lists */
  for (m = ws_Resident_classes;
       m != NULL && status == WS_MAP_CONTINUE && (class_mop = m->op) != NULL;
       m = m->next)
    {

      /* is this a dirty class? */
      if (class_mop->class_mop == sm_Root_class_mop && class_mop->dirty)
	{
	  if (!classes_only)
	    {
	      collected_num_dirty_mop++;
	    }

	  Ws_dirty = true;
	  /* map given function over this dirty class */
	  if (function != NULL)
	    {
	      status = (*function) (class_mop, args);
	    }

	  /* Don't continue if something bad happened */
	  if (status == WS_MAP_FAIL)
	    {
	      break;
	    }
	}

      /* skip over all non-dirty objects at the start of each dirty list */
      for (op = class_mop->dirty_link; op != Null_object && op->dirty == 0;
	   op = next)
	{
	  next = op->dirty_link;
	  op->dirty_link = NULL;
	}
      class_mop->dirty_link = op;

      prev = NULL;
      next = Null_object;

      /* map given function over this class' dirty list */
      for (; op != Null_object && status == WS_MAP_CONTINUE; op = next)
	{

	  /*
	   * if we get here, then op must be dirty. So turn the static dirty
	   * flag on (just in case we've been called from ws_has_updated).
	   * ws_has_updated uses this static flag to check for the presence
	   * of dirty objects.
	   */
	  if (!classes_only)
	    {
	      collected_num_dirty_mop++;
	    }

	  Ws_dirty = true;

	  if (function != NULL)
	    {
	      if (!classes_only)
		{
		  status = (*function) (op, args);
		}

	      else if (op->class_mop == sm_Root_class_mop)
		{
		  status = (*function) (op, args);
		}
	    }

	  /* Don't continue if something bad happened */
	  if (status == WS_MAP_FAIL)
	    {
	      break;
	    }

	  next = op->dirty_link;

	  /* remember the last dirty object in the list */
	  if (op->dirty == 1)
	    {
	      prev = op;
	    }
	  else
	    {
	      op->dirty_link = NULL;	/* remove it from the list */
	    }

	  /* find the next non-dirty object */
	  for (op2 = next; op2 != Null_object && op2->dirty == 0; op2 = next)
	    {
	      next = op2->dirty_link;
	      op2->dirty_link = NULL;
	    }
	  next = op2;

	  /* remove intervening clean objects */
	  if (prev == NULL)
	    {
	      class_mop->dirty_link = next;
	    }
	  else
	    {
	      prev->dirty_link = next;
	    }
	}
    }

  if (status != WS_MAP_FAIL)
    {
      status = WS_MAP_SUCCESS;

      if (!classes_only && ws_Num_dirty_mop != collected_num_dirty_mop)
	{
	  ws_Num_dirty_mop = collected_num_dirty_mop;
	}
    }

  return (status);
}

/*
 * ws_map_dirty - specializations of ws_map_dirty_internal function
 *    return: map status code
 *    function(in): map function
 *    args(in): map function argument
 */
int
ws_map_dirty (MAPFUNC function, void *args)
{
  return (ws_map_dirty_internal (function, args, false));
}

/*
 * ws_filter_dirty - remove any mops that don't have their dirty bit set.
 *    return: void
 */
void
ws_filter_dirty (void)
{
  ws_map_dirty_internal (NULL, NULL, false);
}

/*
 *       	       RESIDENT INSTANCE LIST MAINTENANCE
 */
/*
 * Each class object in the workspace maintains a list of all the instances
 * for that class.  This list is rooted in the class_link field of the class
 * MOP and the instances are chained through their class_link field.
 */

/*
 * add_class_object - Add an instance MOP to the class' resident instance list.
 *    return: NO_ERROR if successful, error code otherwise
 *    class_mop(in/out): class mop
 *    obj(in/out): instance mop
 */
static int
add_class_object (MOP class_mop, MOP obj)
{
  int error = NO_ERROR;

  assert (class_mop != NULL);

  if (class_mop == sm_Root_class_mop)
    {
      /*
       * class MOP, initialize the object list, do this only if it isn't
       * already initialized, this may happen if the workspace is cleared
       * and nothing is cached.  In this case the class_link lists are still
       * valid.  When the class comes back in, we don't want to destroy the
       * previously built instance lists.
       */
      if (obj->class_link == NULL)
	{
	  obj->class_link = Null_object;
	}
      if (obj->dirty_link == NULL)
	{
	  obj->dirty_link = Null_object;
	}
      obj->class_mop = class_mop;

      /* add the class object to the root memory resident class list */
      error = ml_add (&ws_Resident_classes, obj, NULL);
    }
  else
    {
      /* must make sure this gets initialized, should have been done
         already when the class was cached in the clause above */
      if (class_mop->class_link == NULL)
	{
	  class_mop->class_link = Null_object;
	}

      if (obj->class_link == NULL)
	{
	  obj->class_link = class_mop->class_link;
	  class_mop->class_link = obj;
	}
      if (class_mop->object == NULL)
	{
	  error = ER_WS_CLASS_NOT_CACHED;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 0);
	  ws_Stats.uncached_classes++;
	}
      else
	{
	  if ((obj->class_mop != NULL) && (obj->class_mop != class_mop))
	    {
	      error = ER_WS_CHANGING_OBJECT_CLASS;
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 0);
	      ws_Stats.ignored_class_assignments++;
	    }
	  else
	    {
	      obj->class_mop = class_mop;
	    }
	}
    }
  return error;
}

/*
 * remove_class_object - Remove an instance from a class' resident instance
 * list.
 *    return:void
 *    class_mop(in/out): class mop
 *    obj(in): instance mop
 */
static void
remove_class_object (MOP class_mop, MOP obj)
{
  MOP o, prev;

  if (class_mop->class_link == NULL)
    {
      return;
    }

  for (o = class_mop->class_link, prev = NULL;
       o != Null_object && o != obj; o = o->class_link)
    {
      if (o != obj)
	{
	  prev = o;
	}
    }

  if (o == Null_object)
    {
      return;
    }

  if (prev == NULL)
    {
      class_mop->class_link = o->class_link;
    }
  else
    {
      prev->class_link = o->class_link;
    }
  o->class_link = NULL;
  o->class_mop = NULL;

}

#if defined (ENABLE_UNUSED_FUNCTION)	/* TODO - */
/*
 * ws_set_class - set the class of an instance mop.
 *    return: void
 *    inst(in/out): instance mop
 *    class_mop(in/out): class mop
 *
 * Note:
 *    This will make sure the MOP is tagged with the class and that the
 *    instance is added to the class' resident instance list.
 */
void
ws_set_class (MOP inst, MOP class_mop)
{
  if (inst->class_mop != class_mop)
    {
      (void) add_class_object (class_mop, inst);
    }
}
#endif

/*
 * ws_map_class_dirty - iterate over all of the dirty instances of a class and
 * calls supplied function.
 *    return: WS_MAP_SUCCESS or WS_MAP_FAIL
 *    class_op(in/out): class of a mop to iterate over
 *    function(in): map function
 *    args(in): map function argument
 *
 * Note:
 * The mapping (calling the map function) will continue as long as the
 * map function returns WS_MAP_CONTINUE
 */
int
ws_map_class_dirty (MOP class_op, MAPFUNC function, void *args)
{
  MOP op, op2, next, prev;
  DB_OBJLIST *l;
  int status = WS_MAP_CONTINUE;

  if (class_op == sm_Root_class_mop)
    {
      /* rootclass, must map through dirty resident class list */
      for (l = ws_Resident_classes; l != NULL && status == WS_MAP_CONTINUE;
	   l = l->next)
	{
	  /* should we be ignoring deleted class MOPs ? */
	  if (l->op && l->op->dirty && function != NULL)
	    {
	      status = (*function) (l->op, args);
	    }
	}
    }
  else if (class_op->class_mop == sm_Root_class_mop)
    {				/* normal class */
      /* skip over all non-dirty objects at the start of dirty list */
      for (op = class_op->dirty_link, next = Null_object;
	   op != Null_object && op->dirty == 0; op = next)
	{
	  next = op->dirty_link;
	  op->dirty_link = NULL;
	}
      class_op->dirty_link = op;

      prev = NULL;
      next = Null_object;

      /* map given function over this class' dirty list */
      for (; op != Null_object && status == WS_MAP_CONTINUE; op = next)
	{

	  /* what if it is deleted ? */
	  if (function != NULL)
	    {
	      status = (*function) (op, args);
	    }

	  /* Don't continue if something bad happened */
	  if (status == WS_MAP_FAIL)
	    {
	      break;
	    }

	  next = op->dirty_link;

	  /* remember the last dirty object in the list */
	  if (op->dirty == 1)
	    {
	      prev = op;
	    }
	  else
	    {
	      op->dirty_link = NULL;	/* remove it from the list */
	    }

	  /* find the next non-dirty object */
	  for (op2 = next; op2 != Null_object && op2->dirty == 0; op2 = next)
	    {
	      next = op2->dirty_link;
	      op2->dirty_link = NULL;
	    }
	  next = op2;

	  /* remove intervening clean objects */
	  if (prev == NULL)
	    {
	      class_op->dirty_link = next;
	    }
	  else
	    {
	      prev->dirty_link = next;
	    }
	}
    }
  /* else we got an object MOP, don't do anything */

  if (status != WS_MAP_FAIL)
    {
      status = WS_MAP_SUCCESS;
    }

  return (status);
}

/*
 * ws_map_class - iterates over all of the resident instances of a class
 * and calls the supplied function.
 *    return: WS_MAP_SUCCESS or WS_MAP_FAIL
 *    class_op(in): class of interest
 *    function(in): map function
 *    args(in): map function argument
 *
 * Note:
 * The map will continue as long as the map function returns WS_MAP_CONTINUE.
 */
int
ws_map_class (MOP class_op, MAPFUNC function, void *args)
{
  MOP op;
  DB_OBJLIST *l;
  int status = WS_MAP_CONTINUE;

  if (class_op == sm_Root_class_mop)
    {
      /* rootclass, must map through resident class list */
      for (l = ws_Resident_classes; l != NULL && status == WS_MAP_CONTINUE;
	   l = l->next)
	{
	  /* should we be ignoring deleted class MOPs ? */
	  status = (*function) (l->op, args);
	}
    }
  else if (class_op->class_mop == sm_Root_class_mop)
    {
      /* normal class */
      if (class_op->class_link != NULL)
	{
	  for (op = class_op->class_link;
	       op != Null_object && status == WS_MAP_CONTINUE;
	       op = op->class_link)
	    {
#if 1				/* TODO - trace */
	      assert (false);
#endif
	      /*
	       * should we only call the function if the object has been
	       * loaded ? what if it is deleted ?
	       */
	      status = (*function) (op, args);
	    }
	}
    }
  /* else we got an object MOP, don't do anything */

  if (status != WS_MAP_FAIL)
    {
      status = WS_MAP_SUCCESS;
    }

  return (status);
}

/*
 * mark_instance_deleted - mark a mop as deleted
 *    return: WS_MAP_CONTINUE
 *    op(in/out): mop of interest
 *    args(in): not used
 *
 */
static int
mark_instance_deleted (MOP op, UNUSED_ARG void *args)
{
#if 1				/* TODO - trace */
  assert (false);
#endif

  WS_SET_DELETED (op);
  if (op->pinned)
    {
/*  er_set(ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_WS_PIN_VIOLATION, 0); */
      op->pinned = 0;
    }

  return (WS_MAP_CONTINUE);
}

/*
 * ws_mark_instances_deleted - mark class mops as deleted
 *    return: void
 *    class_op(in): mop class of interest
 *
 * Note:
 *    This is called by the schema manager when a class is deleted.  It will
 *    loop through all of the MOPs for instances of this class and mark
 *    them as deleted.  This makes it more efficient to detect deleted
 *    objects in the upper layers.  This may be something the locator should
 *    do when locator_remove_class is called ?
 */
void
ws_mark_instances_deleted (MOP class_op)
{
  ws_map_class (class_op, mark_instance_deleted, NULL);
}

/*
 * CLASS NAME CACHE
 */

/*
 * ws_add_classname - caches a classname in the workspace classname table.
 *    return: void
 *    classobj(in): pointer to class strucure
 *    classmop(in): mop for this class
 *    cl_name(in): class name
 *
 * Note:
 *    It should be called by ws_cache when a class is given to the workspace.
 */
void
ws_add_classname (MOBJ classobj, MOP classmop, const char *cl_name)
{
  MOP current;
  SM_CLASS *class_;

  class_ = (SM_CLASS *) classobj;

  if (class_ == NULL || classmop == NULL)
    {
      return;
    }

  current = (MOP) mht_get (Classname_cache, class_->header.name);

  if (current == NULL)
    {
      mht_put (Classname_cache, cl_name, classmop);
    }
  else
    {
      if (current != classmop)
	{
	  mht_rem (Classname_cache, class_->header.name, NULL, NULL);
	  mht_put (Classname_cache, cl_name, classmop);
	}
    }
}

/*
 * ws_drop_classname - remove a classname from the workspace cache.
 *    return: void
 *    classobj(in): pointer to class strucutre
 *
 * Note:
 * It should be called by ws_cache or whenever the name
 * needs to be removed.
 */
void
ws_drop_classname (MOBJ classobj)
{
  SM_CLASS *class_;

  class_ = (SM_CLASS *) classobj;
  if (class_ != NULL)
    {
      mht_rem (Classname_cache, class_->header.name, NULL, NULL);
    }
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * ws_reset_classname_cache - clear the class name cache since this must be
 * reverified for the next transaction.
 *    return: void
 *
 * Note:
 *    This is called whenever a transaction is committed or aborted.
 *    We might consider must NULLing out the current class pointers and
 *    leaving the key strings in the table so we can avoid reallocating
 *    them again the next time names are cached.
 */
void
ws_reset_classname_cache (void)
{
  if (Classname_cache != NULL)
    {
      /*
       * don't need to map over entries because the name strings
       * are part of the class structure
       */
      (void) mht_clear (Classname_cache, NULL, NULL);
    }
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * ws_find_class - search in the workspace classname cache for the MOP of
 * a class.
 *    return: class pointer
 *    name(in): class name to search
 *
 * Note:
 *    This avoids going to the server each time a name to OID mapping needs
 *    to be made.  The cache will remain valid for the duration of the
 *    current transaction.  This should be called by locator_find_class to
 *    check the schema before calling the server.
 */
MOP
ws_find_class (const char *name)
{
  MOP class_mop = NULL;

  assert (name != NULL);

  if (Classname_cache != NULL && name != NULL)
    {
      class_mop = (MOP) mht_get (Classname_cache, name);
    }

  return class_mop;
}

/*
 * MAIN INITIALIZATION AND SHUTDOWN
 */

/*
 * ws_init - initialize workspace
 *    return: NO_ERROR if successful, error code otherwise
 *
 * Note: This function should be called once early in the database
 *    initialization phase.
 */
int
ws_init (void)
{
  unsigned int i;
  size_t allocsize;

  /*
   * this function needs to be left active after a database shutdown,
   * (in case of server crash).  Because of this, it must be able
   * to restart itself if initialized twice
   */
  if (ws_Mop_table != NULL)
    {
      ws_final ();
    }

  /*
   * area must have been called earlier.
   * These need to all be returning errors !
   */
  set_area_init ();		/* set reference */

  /* build the MOP table */
  ws_Mop_table_size = prm_get_integer_value (PRM_ID_WS_HASHTABLE_SIZE);
  allocsize = sizeof (WS_MOP_TABLE_ENTRY) * ws_Mop_table_size;
  ws_Mop_table = (WS_MOP_TABLE_ENTRY *) malloc (allocsize);

  if (ws_Mop_table == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
	      1, allocsize);
      return ER_OUT_OF_VIRTUAL_MEMORY;
    }

  for (i = 0; i < ws_Mop_table_size; i++)
    {
      ws_Mop_table[i].head = NULL;
      ws_Mop_table[i].tail = NULL;
    }

  /* create the internal Null object mop */
  Null_object = ws_make_mop (NULL);
  if (Null_object == NULL)
    {
      return (er_errid ());
    }

  /* start with nothing dirty */
  Ws_dirty = false;

  /* build the classname cache */
  Classname_cache = mht_create ("Workspace class name cache",
				256, mht_1strhash,
				mht_compare_strings_are_equal);

  if (Classname_cache == NULL)
    {
      return ER_OUT_OF_VIRTUAL_MEMORY;
    }

  /* Can't have any resident classes yet */
  ws_Resident_classes = NULL;

  return NO_ERROR;
}

/*
 * ws_final - Close the workspace and release all allocated storage.
 *    return: void
 *
 * Note: Must only be called prior to closing the database.
 */
void
ws_final (void)
{
  MOP mop, next;
  unsigned int slot;

  if (prm_get_bool_value (PRM_ID_WS_MEMORY_REPORT))
    {
      /* this is for debugging only */
      fprintf (stdout,
	       "*** Database client statistics before shutdown ***\n");
      ws_dump (stdout);
      /*
       * Check for dangling allocations in the workspace.
       * First decache everything, must do this before the
       * MOP tables are destroyed.
       */
      ws_clear_internal ();
    }
  ws_clear_all_repl_errors_of_error_link ();
  ws_clear ();

  /* destroy the classname cache */
  if (Classname_cache != NULL)
    {
      mht_destroy (Classname_cache);
      Classname_cache = NULL;
    }

  /* destroy list of resident classes */
  ml_free (ws_Resident_classes);
  ws_Resident_classes = NULL;

  /* destroy the MOP table */
  if (ws_Mop_table != NULL)
    {
      for (slot = 0; slot < ws_Mop_table_size; slot++)
	{
	  for (mop = ws_Mop_table[slot].head, next = NULL; mop != NULL;
	       mop = next)
	    {
	      next = mop->hash_link;
	      ws_free_mop (mop);
	    }
	}
      ws_free_mop (Null_object);
      free_and_init (ws_Mop_table);
    }

  /* clean up misc globals */
  ws_Mop_table = NULL;
  ws_Mop_table_size = 0;
  Null_object = NULL;
  Ws_dirty = false;
}

/*
 * ws_clear_internal - Debugging function that clears all locks.
 *    return: void
 *
 * Note: Used to make sure objects are flushed correctly.
 */
static void
ws_clear_internal (void)
{
  MOP mop;
  unsigned int slot;

  for (slot = 0; slot < ws_Mop_table_size; slot++)
    {
      for (mop = ws_Mop_table[slot].head; mop != NULL; mop = mop->hash_link)
	{
	  mop->ws_lock = NULL_LOCK;
	  mop->deleted = 0;
	}
    }
  ws_Commit_mops = NULL;
  ws_filter_dirty ();
}

/*
 * ws_clear - ws_clear_internal wrapper. see comments of ws_clear_internal
 *    return: void
 *
 */
void
ws_clear (void)
{
  ws_clear_internal ();
}

/*
 * ws_has_updated - see if there are any dirty objects in the workspace
 *    return: true if updated, false otherwise
 */
bool
ws_has_updated (void)
{
  /*
   * We used to be able to test the global dirty list (Dirty_objects) for
   * the presence of workspace updates. Now, we have to be a bit sneaky. To
   * do the same test, we set this static dirty flag to false and let the
   * ws_filter_dirty traversal turn this dirty flag on if it finds any
   * dirty objects in the workspace.
   */
  Ws_dirty = false;

  /*
   * wouldn't need to filter the whole list but this seems like
   * a reasonable time to do this
   */
  ws_filter_dirty ();

  return (Ws_dirty);
}

/*
 * MOP CACHING AND DECACHING
 */

/*
 * ws_cache - sets the object content of a mop
 *    return: void
 *    obj(in): memory representation of object
 *    mop(in): mop of the object
 *    class_mop(in): class of the object
 *
 * Note:
 *    First, we must check for any existing contents and free them.
 *    Note that when a class is decached, all instances of that class must
 *    also be decached because the class definition may have changed.
 *    We force this here but it really has to be checked by the transaction
 *    manager since the dirty instances must be flushed.
 */
void
ws_cache (MOBJ obj, MOP mop, MOP class_mop)
{
  assert (!(ws_get_lock (mop) > NULL_LOCK));

  /* third clause applies if the sm_Root_class_mop is still being initialized */
  if ((class_mop == sm_Root_class_mop)
      || (mop->class_mop == sm_Root_class_mop) || (mop == class_mop))
    {

      /* caching a class */
      if ((mop->object != NULL) && (mop->object != (MOBJ) (&sm_Root_class)))
	{
	  /* remove information for existing class */
	  ws_drop_classname ((MOBJ) mop->object);
	  classobj_free_class ((SM_CLASS *) mop->object);
	}
      mop->object = obj;
      mop->class_mop = class_mop;

      /*
       * must always call this when caching a class because we don't know
       * if there are any objects on disk
       */
      ws_class_has_object_dependencies (mop);

      if (obj != (MOBJ) (&sm_Root_class))
	{
	  /* this initializes the class_link list and adds it to the
	     list of resident classes */
	  if (add_class_object (class_mop, mop))
	    {
	      goto abort_it;
	    }

	  /* add to the classname cache */
	  ws_add_classname (obj, mop, ((SM_CLASS *) obj)->header.name);
	}
    }
  else
    {
      if (mop->object != NULL)
	{
	  /* free the current contents */
	  if (mop->class_mop == NULL || mop->class_mop->object == NULL)
	    {
	      /* SERIOUS INTERNAL ERROR */
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		      ER_WS_CLASS_NOT_CACHED, 0);
	      ws_Stats.uncached_classes++;
	      goto abort_it;
	    }
	  else
	    {
	      obj_free_memory ((SM_CLASS *) mop->class_mop->object,
			       (MOBJ) mop->object);
	      mop->object = NULL;
	    }
	}

      mop->object = obj;
      ws_class_has_object_dependencies (class_mop);

      if (mop->class_mop != class_mop)
	{
	  if (mop->class_mop != NULL)
	    {
	      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE,
		      ER_WS_CHANGING_OBJECT_CLASS, 0);
	      if (mop->class_link != NULL)
		{
		  remove_class_object (mop->class_mop, mop);
		}
	    }
	  if (add_class_object (class_mop, mop))
	    {
	      goto abort_it;
	    }
	}
    }

  return;

abort_it:
  /*
   * NULL the MOP since we're in an unknown state, this function
   * should be returning an error
   */
  mop->object = NULL;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * ws_cache_dirty - caches an object and also marks it as dirty
 *    return: void
 *    obj(in): memory representation of object
 *    op(in): mop of object
 *    class(in): class of object
 */
void
ws_cache_dirty (MOBJ obj, MOP op, MOP class_)
{
  ws_cache (obj, op, class_);
  ws_dirty (op);
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * ws_cache_with_oid - first find or create a MOP for the object's OID and
 * then cache the object.
 *    return: mop of cached object
 *    obj(in): memory representation of object
 *    oid(in): object identifier
 *    class(in): class of object
 *
 */
MOP
ws_cache_with_oid (MOBJ obj, OID * oid, MOP class_)
{
  MOP mop;

  mop = ws_mop (oid, class_);
  if (mop != NULL)
    {
      ws_cache (obj, mop, class_);
    }

  return (mop);
}

/*
 *  MOP ACCESSOR FUNCTIONS
 */

/*
 * ws_identifier() - This function returns the permanent object identifier of
 *                   the given object.
 * return : Pointer to object identifier
 * mop(in):
 * Note: This function should not be used if the object can be a
 *       non-referable instance as it will return a reference to the object;
 *       use db_identifier () instead to perform the needed check.
 */
OID *
ws_identifier (MOP mop)
{
  OID *oid = NULL;

  if (mop == NULL || WS_MARKED_DELETED (mop))
    {
      goto end;
    }

  oid = ws_oid (mop);

  if (OID_ISTEMP (oid))
    {
      assert (false);
      oid = (OID *) & oid_Null_oid;
    }

end:
  return oid;
}

/*
 * These provide access shells for the fields in the MOP structure.  These
 * are simple enough that callers should change to use the corresponding
 * macros.
 */

/*
 * ws_oid - oid field accessor
 *    return: pointer to oid structure
 *    mop(in): object pointer
 */
OID *
ws_oid (MOP mop)
{
  if (mop)
    {
      return (WS_OID (mop));
    }

  return NULL;
}

/*
 * ws_class_mop - class accessor
 *    return: pointer to the class mop of an object
 *    mop(in): object mop
 */
MOP
ws_class_mop (MOP mop)
{
#if 0				/* TODO - trace */
  assert (mop == sm_Root_class_mop
	  || ws_mop_compare (mop, sm_Root_class_mop) == 0
	  || mop->class_mop != NULL);
#endif

  return (mop->class_mop);
}

/*
 * ws_get_lock - lock field accessor
 *    return: lock field of a mop
 *    mop(in): object pointer
 */
LOCK
ws_get_lock (MOP mop)
{
  return (mop->ws_lock);
}

/*
 * ws_set_lock - lock field setter
 *    return: void
 *    mop(in): object pointer
 *    lock(in): lock type
 */
void
ws_set_lock (MOP mop, LOCK lock)
{
  if (mop != NULL)
    {
      WS_SET_LOCK (mop, lock);
    }
}

/*
 * ws_pin - sets the pin flag for a MOP
 *    return: previous pin flag value
 *    mop(in/out): object pointer
 *    pin(in): pin flag value
 *
 * Note:
 *    Pinning a MOP will make sure that it is not decached
 *    (garbage collected) for the duration of the transaction.
 *    The pin flag will be cleared with the other mop flags when a
 *    transaction is aborted or committed.
 *    It is OK to call this for a MOP that has no current contents.  This
 *    would happen in the case where we have just prefetched some objects
 *    and are attempting to load and cache all of them.  Since a panic
 *    can ocurr during the loading of one of the prefetched objects, we
 *    must make sure that the original object we were attempting to fetch
 *    is not swapped out as part of the panic.  To prevent this, we pin
 *    the mop before it is cached.
 */
int
ws_pin (MOP mop, int pin)
{
  int old = 0;

  /* We don't deal with MOPs on the server */
  if (db_on_server)
    {
      return old;
    }

  if (mop != NULL)
    {
      if (mop->class_mop != sm_Root_class_mop)
	{
	  old = mop->pinned;
	  mop->pinned = pin;
	}
      /* else, its a class MOP, they're implicitly pinned */
    }

  return (old);
}

/*
 * ws_pin_instance_and_class - pin object and the class of the object
 *    return: void
 *    obj(in/out): object pointer
 *    opin(out): previous pin flag value of a object
 *    cpin(out): previous pin flag value of a class of the object
 */
void
ws_pin_instance_and_class (MOP obj, int *opin, int *cpin)
{
  if (obj->class_mop != NULL && obj->class_mop != sm_Root_class_mop)
    {
      *opin = obj->pinned;
      obj->pinned = 1;
      if (obj->class_mop == NULL)
	{
	  *cpin = 0;
	}
      else
	{
	  *cpin = obj->class_mop->pinned;
	  obj->class_mop->pinned = 1;
	}
    }
  else
    {
      /* classes have no explicit pinning */
      *opin = 0;
      *cpin = 0;
    }
}

/*
 * ws_restore_pin - resotre pin flag of a object and its class object
 *    return: void
 *    obj(in/out): object pointer
 *    opin(in): class pin flag value to set
 *    cpin(in): object pin flag value to set
 */
void
ws_restore_pin (MOP obj, int opin, int cpin)
{
  obj->pinned = opin;
  if (obj->class_mop != NULL)
    {
      obj->class_mop->pinned = cpin;
    }
}

/*
 * ws_mark_deleted
 *
 * arguments:
 *
 * returns/side-effects:
 *
 * description:
 *    This marks an object as deleted.  It will also add the object to the
 *    dirty list if it isn't already there.  The object will be flushed
 *    to disk at the end of the transaction.
 */

/*
 * ws_mark_deleted - marks an object as deleted
 *    return: void
 *    mop(in): object pointer
 *
 */
void
ws_mark_deleted (MOP mop)
{
  ws_dirty (mop);

  WS_SET_DELETED (mop);

  /* should be unpinning before deleting */
  if (mop->pinned)
    {
/*    er_set(ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_WS_PIN_VIOLATION, 0); */
      mop->pinned = 0;
    }

}

/*
 * MISC UTILITY FUNCTIONS
 */

/*
 * ws_find -
 *    return: mop status code (WS_FIND_MOP_DELETED, WS_FIND_MOP_NOTDELETED)
 *    mop(in): object pointer
 *    obj(out): return pointer to memory representation of object
 *
 * Note:
 *    This is used to access the memory representation of an object.
 *    The memory representation is returned through the supplied pointer
 *    as long as the mop is not marked as deleted.
 */
int
ws_find (MOP mop, MOBJ * obj)
{
  int status = WS_FIND_MOP_NOTDELETED;

  *obj = NULL;
  if (mop && !mop->deleted)
    {
      *obj = (MOBJ) mop->object;
    }
  else
    {
      status = WS_FIND_MOP_DELETED;
    }

  return (status);
}

/*
 * ws_mop_compare - compare MOPs
 *    return: 0 if equal, non-zero if not equal
 *    mop1(in): object pointer
 *    mop2(in): object pointer
 *
 * Note:
 *    Currently, MOPs with the same OID will always be exactly the same
 *    structure so comparison with '==' in C is acceptable.  It has been
 *    discussed that this may not be acceptable for future use so this
 *    function will compare based on the OIDs.
 */
int
ws_mop_compare (MOP mop1, MOP mop2)
{
  return (oid_compare (WS_OID (mop1), WS_OID (mop2)));
}

/*
 * ws_class_has_object_dependencies - set no_object fields to 0
 *    return: void
 *    class(out): class mop
 *
 * Note:
 *    This controls a flag that is used to optimize the generation of
 *    represetations by the schema manager when a class is altered.
 *    A bit is kept in the class MOP that is set whenever we are sure
 *    that no object have been stored in the database that are dependent on
 *    the current class representation.  This bit is set after a
 *    representation is installed but is cleared whenever a class is
 *    cached, an object of the class is cached, an object of the class is
 *    updated, or an object of the class is created.  It will also be cleared
 *    at the end of a transaction by ws_clear_hints.
 */
void
ws_class_has_object_dependencies (MOP class_)
{
  if (class_ != NULL)
    {
      class_->no_objects = 0;
    }
}

/*
 * ws_class_has_cached_objects - check if there are cached redident instance
 *    return: non zero if there are cached redident instances
 *    class(in): class to examin
 */
int
ws_class_has_cached_objects (MOP class_)
{
  MOP obj;
  int cached = 0;

  for (obj = class_->class_link; obj != Null_object && !cached;
       obj = obj->class_link)
    {
      if (obj->object != NULL)
	{
	  cached = 1;
	}
    }

  assert (cached == 0);

  return (cached);
}

#if defined (RYE_DEBUG)
/*
 * ws_map - map over all MOPs currently in the workspace.
 *    return: WS_MAP_ status code
 *    function(in): mapping function to alpply to the mops
 *    args(in): map function argument
 *
 * Note:
 *    The loop will continue as long as the mapping function returns
 *    WS_MAP_CONTINUE.
 */
int
ws_map (MAPFUNC function, void *args)
{
  MOP mop;
  unsigned int slot;
  int status = WS_MAP_CONTINUE;

  if (ws_Mop_table != NULL)
    {
      for (slot = 0; slot < ws_Mop_table_size && status == WS_MAP_CONTINUE;
	   slot++)
	{
	  for (mop = ws_Mop_table[slot].head;
	       mop != NULL && status == WS_MAP_CONTINUE; mop = mop->hash_link)
	    {
	      status = (*(function)) (mop, args);
	      if (status == WS_MAP_CONTINUE_ON_ERROR)
		{
		  num_ws_continue_on_error++;
		  stauts = WS_MAP_CONTINUE;
		}
	    }
	}
    }
  if (status != WS_MAP_FAIL)
    {
      status = WS_MAP_CONTINUE_ON_ERROR;
    }

  return (status);
}
#endif

/*
 * TRANSACTION MANAGEMENT SUPPORT
 */

/*
 * ws_clear_hints - clear all of the hint bits in the MOP.
 *    return: void
 *    mop(in): object pointer
 *    leave_pinned(in): flag to keep from modifying pinned field
 *
 * Note:
 *    This is called by the transaction manager to clear all of the hint
 *    bits in the MOP.  This is guaranteed to be called at the end of a
 *    transaction commit.  Note that we always clear the no_objects field
 *    for classes because once they are commited to a database, we must
 *    assume that other users have access to the current representation and
 *    can create instances with that represenatation.
 */
void
ws_clear_hints (MOP mop, bool leave_pinned)
{
  /*
   * Don't decache non-updatable view objects because they cannot be
   * recreated.  Let garbage collection eventually decache them.
   */

  mop->ws_lock = NULL_LOCK;
  mop->deleted = 0;
  WS_RESET_DIRTY (mop);
  mop->no_objects = 0;

  if (!leave_pinned)
    {
      mop->pinned = 0;
    }
}

/*
 * ws_clear_all_hints - reset all hint flags in the mops after a transaction
 * has been committeed.
 *    return: void
 *
 * Note:
 *    Called by the transaction manager to reset all hint flags in the mops
 *    after a transaction has been committeed.  Also reset the
 *    authorization cache.
 */
void
ws_clear_all_hints (void)
{
  MOP mop;
  MOP next;

  au_reset_authorization_caches ();

  mop = ws_Commit_mops;
  while (mop)
    {
      ws_clear_hints (mop, false);
      next = mop->commit_link;
      mop->commit_link = NULL;	/* remove mop from commit link (it's done) */

      if (next == mop)
	{
	  mop = NULL;
	}
      else
	{
	  mop = next;
	}
    }
  ws_Commit_mops = NULL;
  ws_Num_dirty_mop = 0;
}

/*
 * ws_abort_mops - called by the transaction manager when a transaction is
 * aborted
 *    return: void
 *    only_unpinned(in): flag whether is it safe to abort pinned mops
 *
 */
void
ws_abort_mops (bool only_unpinned)
{
  MOP mop;
  MOP next;

  au_reset_authorization_caches ();

  mop = ws_Commit_mops;
  while (mop)
    {
      next = mop->commit_link;
      mop->commit_link = NULL;	/* remove mop from commit link (it's done) */

      /* clear all hint fields including the lock */
      ws_clear_hints (mop, only_unpinned);

      if (next == mop)
	{
	  mop = NULL;
	}
      else
	{
	  mop = next;
	}
    }

  if (!only_unpinned)
    {
      ws_Commit_mops = NULL;
      ws_Num_dirty_mop = 0;
    }
}

/*
 * STRING UTILITIES
 */

/*
 * ws_copy_string - copies a string storage allocated whthin the workspace
 *    return: copied string
 *    str(in): string to copy
 */
char *
ws_copy_string (const char *str)
{
  char *copy;

  if (str == NULL)
    {
      return NULL;
    }

  copy = (char *) db_ws_alloc (strlen (str) + 1);
  if (copy == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
	      ER_OUT_OF_VIRTUAL_MEMORY, 1, strlen (str + 1) + 1);

      return NULL;
    }

  strcpy ((char *) copy, (char *) str);

  return copy;
}

/*
 * ws_free_string - frees a string that was allocated by ws_copy_string.
 *    return: void
 *    str(out): workspace string to free
 */
void
ws_free_string (const char *str)
{
  char *s;

  if (str != NULL)
    {
      s = (char *) str;		/* avoid compiler warnings */
      db_ws_free (s);
    }
}

/*
 * DEBUG FUNCTIONS
 */

#if defined (RYE_DEBUG)
/*
 * ws_print_oid - print oid to standard out
 *    return: void
 *    oid(in): oid to print
 */
static void
ws_print_oid (OID * oid)
{
  fprintf (stdout, "%d/%d/%d",
	   (int) oid->volid, (int) oid->pageid, (int) oid->slotid);
}

/*
 * ws_describe_mop - print MOP information
 *    return: void
 *    mop(in): object pointer to describe
 *    args(in): not used
 */
static int
ws_describe_mop (MOP mop, void *args)
{
  ws_print_oid (WS_OID (mop));
  fprintf (stdout, " ");
  if (ws_mop_compare (mop, sm_Root_class_mop) == 0)
    {
      fprintf (stdout, "Root class ");
    }
  else
    {
      if (mop->class_mop == NULL)
	{
	  fprintf (stdout, "class MOP not available\n");
	}
      else
	{
	  if (ws_mop_compare (mop->class_mop, sm_Root_class_mop) == 0)
	    {
	      fprintf (stdout, "class ");
	      if (mop->object == NULL)
		{
		  fprintf (stdout, "not cached ");
		}
	      else
		{
		  fprintf (stdout, "%s ", sm_class_name (mop));
		}
	    }
	  else
	    {
	      fprintf (stdout, "instance of ");
	      if (mop->class_mop->object == NULL)
		{
		  fprintf (stdout, "uncached class ");
		}
	      else
		{
		  fprintf (stdout, "%s ", sm_class_name (mop->class_mop));
		}
	    }
	}
    }
  if (mop->dirty)
    {
      fprintf (stdout, " dirty");
    }
  if (mop->deleted)
    {
      fprintf (stdout, " deleted");
    }
  if (mop->pinned)
    {
      fprintf (stdout, " pinned");
    }
  if (mop->no_objects)
    {
      fprintf (stdout, " no_objects");
    }

  fprintf (stdout, "\n");
  return (WS_MAP_CONTINUE);
}

/*
 * ws_dump_mops - print information of all mops
 *    return: void
 */
void
ws_dump_mops (void)
{
  fprintf (stdout, "WORKSPACE MOP TABLE:\n\n");
  (void) ws_map (ws_describe_mop, NULL);
  fprintf (stdout, "\n");
}
#endif

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * ws_makemop - find or create a MOP whose OID contains the indicated
 * identifiers
 *    return: mop found or created
 *    volid(in): volumn id
 *    pageid(in): page id
 *    slotid(in): slot id
 *
 * description:
 *    This will build (or find) a MOP whose OID contains the indicated
 *    identifiers.  This is intended as a debugging functions to get a
 *    handle on an object pointer given the three OID numbers.
 *
 * Note:
 *    This is intended as a debugging functions to get a
 *    handle on an object pointer given the three OID numbers.
 */
MOP
ws_makemop (int volid, int pageid, int slotid)
{
  OID oid;
  MOP mop;

  oid.volid = volid;
  oid.pageid = pageid;
  oid.slotid = slotid;
  mop = ws_mop (&oid, NULL);

  return (mop);
}

/*
 * ws_count_mops - count the number of mops in the workspace
 *    return: mop count in the workspace
 */
int
ws_count_mops (void)
{
  MOP mop;
  unsigned int slot, count;

  count = 0;
  for (slot = 0; slot < ws_Mop_table_size; slot++)
    {
      for (mop = ws_Mop_table[slot].head; mop != NULL; mop = mop->hash_link)
	{
	  count++;
	}
    }
  return (count);
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * WORKSPACE STATISTICS
 */

/*
 * ws_dump - print worksapce information to FILE output
 *    return: void
 *    fpp(in): FILE * to print the workspace information
 */
void
ws_dump (FILE * fpp)
{
  int mops, root, unknown, classes, cached_classes, instances,
    cached_instances;
  int count, actual, decached, weird;
  unsigned int slot;
  int classtotal, insttotal, size, isize, icount, deleted;
  MOP mop, inst;
  DB_OBJLIST *m;

  /* get mop totals */
  mops = root = unknown = classes = cached_classes = instances =
    cached_instances = 0;
  weird = 0;
  for (slot = 0; slot < ws_Mop_table_size; slot++)
    {
      for (mop = ws_Mop_table[slot].head; mop != NULL; mop = mop->hash_link)
	{
	  mops++;

	  if (mop == sm_Root_class_mop)
	    {
	      continue;
	    }

	  if (mop->class_mop == NULL)
	    {
#if 1				/* TODO - trace */
	      assert (false);
#endif

	      unknown++;
	      if (mop->object != NULL)
		{
		  weird++;
		}
	    }
	  else if (mop->class_mop == sm_Root_class_mop)
	    {
	      classes++;
	      if (mop->object != NULL)
		{
		  cached_classes++;
		}
	    }
	  else
	    {
	      instances++;
	      if (mop->object != NULL)
		{
		  cached_instances++;
		}
	    }

	}
    }

  fprintf (fpp, "%d mops in the workspace (including one rootclass mop)\n",
	   mops);
  fprintf (fpp, "%d class mops (%d cached, %d uncached)\n", classes,
	   cached_classes, classes - cached_classes);
  fprintf (fpp, "%d instance mops (%d cached, %d uncached)\n", instances,
	   cached_instances, instances - cached_instances);

  fprintf (fpp, "%d unknown mops\n", unknown);
  if (weird)
    {
      fprintf (fpp, "*** %d unknown mops with cached objects\n", weird);
    }
  fprintf (fpp, "%d attempts to clean pinned mops\n",
	   ws_Stats.pinned_cleanings);

  /* gc stats */
  fprintf (fpp, "%d MOPs allocated, %d freed\n",
	   ws_Stats.mops_allocated, ws_Stats.mops_freed);

  /* misc stats */
  fprintf (fpp,
	   "%d dirty list emergencies, %d uncached classes, %d corruptions\n",
	   ws_Stats.dirty_list_emergencies, ws_Stats.uncached_classes,
	   ws_Stats.corruptions);
  fprintf (fpp, "%d ignored class assignments\n",
	   ws_Stats.ignored_class_assignments);


  fprintf (fpp,
	   "%d total set mops allocated, %d total set mops freed\n",
	   ws_Stats.set_mops_allocated, ws_Stats.set_mops_freed);

  /* dirty stats */
  count = actual = 0;
  for (m = ws_Resident_classes; m != NULL; m = m->next)
    {
      for (mop = m->op->dirty_link; mop != Null_object; mop = mop->dirty_link)
	{
	  count++;
	  if (mop->dirty)
	    {
	      actual++;
	    }
	}
    }
  fprintf (fpp, "%d dirty objects, %d clean objects in dirty list\n",
	   actual, count - actual);

  /* get class totals */
  fprintf (fpp, "RESIDENT INSTANCE TOTALS: \n");
  count = classtotal = insttotal = deleted = 0;
  for (m = ws_Resident_classes; m != NULL; m = m->next)
    {
      mop = m->op;
      if (mop->deleted)
	{
	  deleted++;
	}
      else
	{
	  count++;
	  if (mop != sm_Root_class_mop && mop->object != NULL)
	    {
	      size = classobj_class_size ((SM_CLASS *) mop->object);
	      classtotal += size;
	      icount = isize = decached = 0;
	      for (inst = mop->class_link; inst != Null_object;
		   inst = inst->class_link)
		{
		  icount++;
		  if (inst->object != NULL)
		    {
		      isize +=
			sm_object_size_quick ((SM_CLASS *) mop->object,
					      (MOBJ) inst->object);
		    }
		  else
		    {
		      decached++;
		    }
		}
	      fprintf (fpp,
		       "  %-20s : %d instances, %d decached, %d bytes used\n",
		       sm_classobj_name ((MOBJ) mop->object), icount,
		       decached, isize);
	      insttotal += isize;
	    }
	}
    }
  if (deleted)
    {
      fprintf (fpp, "*** %d deleted MOPs in the resident class list \n",
	       deleted);
    }

  /* just to make sure */
  if (count != cached_classes)
    {
      fprintf (fpp,
	       "*** Mops claiming to be classes %d, resident class list length %d\n",
	       cached_classes, count);
    }

  fprintf (fpp, "Total bytes for class storage     %d\n", classtotal);
  fprintf (fpp, "Total bytes for instance storage  %d\n", insttotal);
  fprintf (fpp, "Total bytes for object storage    %d\n",
	   classtotal + insttotal);
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * ws_has_dirty_objects - check if object has any dirty instance
 *    return: nonzero iff op has any dirty instances
 *    op(in): object pointer
 *    isvirt(out): 1 iff op is a proxy of vclass
 */
int
ws_has_dirty_objects (MOP op, int *isvirt)
{
  *isvirt = (op && !op->deleted && op->object
	     && (((SM_CLASS *) (op->object))->class_type == SM_VCLASS_CT));

  return (op && !op->deleted && op->object && op->dirty_link
	  && op->dirty_link != Null_object);
}
#endif

/*
 * ws_need_flush - check if workspace has dirty mop
 *    return: 1 | 0
 */
bool
ws_need_flush (void)
{
  return (ws_Num_dirty_mop > 0);
}


/*
 * LIST UTILITIES
 */
/*
 * These operations assume a structure with a single link field at the top.
 *
 * struct link {
 *   struct link *next;
 * };
 *
 */


/*
 * ws_list_append - append element to the end of a list
 *    return: none
 *    root(in/out): pointer to pointer to list head
 *    element(in): element to add
 */
void
ws_list_append (DB_LIST ** root, DB_LIST * element)
{
  DB_LIST *el;

  for (el = *root; (el != NULL) && (el->next != NULL); el = el->next)
    {
      ;
    }

  if (el == NULL)
    {
      *root = element;
    }
  else
    {
      el->next = element;
    }
}

/*
 * ws_list_remove - Removes an element from a list if it exists.
 *    return: non-zero if the element was removed
 *    root(): pointer to pointer to list head
 *    element(): element to remove
 */
int
ws_list_remove (DB_LIST ** root, DB_LIST * element)
{
  DB_LIST *el, *prev;
  int removed;

  removed = 0;
  for (el = *root, prev = NULL; el != NULL && el != element; el = el->next)
    {
      prev = el;
    }

  if (el != element)
    {
      return removed;
    }
  if (prev == NULL)
    {
      *root = element->next;
    }
  else
    {
      prev->next = element->next;
    }
  removed = 1;

  return (removed);
}

/*
 * ws_list_length - return the number of elements in a list
 *    return: length of the list (zero if empty)
 *    list(in): list to examine
 */
int
ws_list_length (DB_LIST * list)
{
  DB_LIST *el;
  int length = 0;

  for (el = list; el != NULL; el = el->next)
    {
      length++;
    }

  return (length);
}

/*
 * ws_list_free - apply (free) function over the elements of a list
 *    return: none
 *    list(in): list to free
 *    function(in): function to perform the freeing of elements
 */
void
ws_list_free (DB_LIST * list, LFREEER function)
{
  DB_LIST *link, *next;

  for (link = list, next = NULL; link != NULL; link = next)
    {
      next = link->next;
      (*function) (link);
    }
}


/*
 * ws_list_total - maps a function over the elements of a list and totals up
 * the integers returned by the mapping function.
 *    return: total of all calls to mapping function
 *    list(in): list to examine
 *    function(in): function to call on list elements
 */
int
ws_list_total (DB_LIST * list, LTOTALER function)
{
  DB_LIST *el;
  int total = 0;

  for (el = list; el != NULL; el = el->next)
    {
      total += (*function) (el);
    }

  return (total);
}


/*
 * ws_list_copy - Copies a list by calling a copier function for each element.
 *    return: new list
 *    src(in): list to copy
 *    copier(in): function to copy the elements
 *    freeer(in): function to free the elements
 */
DB_LIST *
ws_list_copy (DB_LIST * src, LCOPIER copier, LFREEER freeer)
{
  DB_LIST *list, *last, *new_;

  list = last = NULL;
  for (; src != NULL; src = src->next)
    {
      new_ = (DB_LIST *) (*copier) (src);
      if (new_ == NULL)
	{
	  goto memory_error;
	}

      new_->next = NULL;
      if (list == NULL)
	{
	  list = new_;
	}
      else
	{
	  last->next = new_;
	}
      last = new_;
    }
  return (list);

memory_error:
  if (freeer != NULL)
    {
      ws_list_free (list, freeer);
    }
  return NULL;
}


/*
 * ws_list_nconc - concatenate list2 to list1
 *    return: list pointer
 *    list1(out): first list
 *    list2(in): list to concatenate
 * Note:
 *    If list1 was NULL, it returns a pointer to list2.
 */
DB_LIST *
ws_list_nconc (DB_LIST * list1, DB_LIST * list2)
{
  DB_LIST *el, *result;

  if (list1 == NULL)
    {
      result = list2;
    }
  else
    {
      result = list1;
      for (el = list1; el->next != NULL; el = el->next)
	{
	  ;
	}
      el->next = list2;
    }
  return (result);
}

/*
 * NAMED LIST UTILITIES
 */
/*
 * These utilities assume elements with a link field and a name.
 * struct named_link {
 *   struct named_link *next;
 *   const char *name;
 * }
 */

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * nlist_remove - Removes a named element from a list.
 *    return: removed element (if found), NULL otherwise
 *    root(in/out): pointer to pointer to list head
 *    name(in): name of entry to remove
 *    fcn(in): compare function
 * Note:
 *    If an element with the given name was found it is removed and returned.
 *    If an element was not found, NULL is returned.
 */
DB_NAMELIST *
nlist_remove (DB_NAMELIST ** root, const char *name, NLSEARCHER fcn)
{
  DB_NAMELIST *el, *prev, *found;

  if (fcn == NULL)
    {
      fcn = (NLSEARCHER) strcmp;
    }

  found = NULL;

  for (el = *root, prev = NULL; el != NULL && found == NULL; el = el->next)
    {
      if ((el->name == name) ||
	  ((el->name != NULL) && (name != NULL)
	   && (*fcn) (el->name, name) == 0))
	{
	  found = el;
	}
      else
	{
	  prev = el;
	}
    }
  if (found != NULL)
    {
      if (prev == NULL)
	{
	  *root = found->next;
	}
      else
	{
	  prev->next = found->next;
	}
    }

  return (found);
}

/*
 * nlist_append - appends an element to a namelist if it does not exist.
 *    return: NO_ERROR if the element was added , error code otherwise
 *    list(in/out): pointer to pointer to list head
 *    name(in): entry name to append
 *    fcn(in): compare function
 *    added_ptr(out): set to 1 if added
 */
int
nlist_append (DB_NAMELIST ** list, const char *name, NLSEARCHER fcn,
	      int *added_ptr)
{
  DB_NAMELIST *el, *found, *last, *new_;
  int status = 0;

  if (fcn == NULL)
    {
      fcn = (NLSEARCHER) strcmp;
    }

  if (name == NULL)
    {
      goto error;
    }

  found = NULL;
  last = NULL;

  for (el = *list; el != NULL && found == NULL; el = el->next)
    {
      if ((el->name == name) ||
	  ((el->name != NULL) && (name != NULL)
	   && (*fcn) (el->name, name) == 0))
	{
	  found = el;
	}
      last = el;
    }
  if (found != NULL)
    {
      goto error;
    }
  new_ = (DB_NAMELIST *) db_ws_alloc (sizeof (DB_NAMELIST));

  if (new_ == NULL)
    {
      return er_errid ();
    }

  new_->name = ws_copy_string (name);

  if (new_->name == NULL)
    {
      db_ws_free (new_);
      return er_errid ();
    }

  new_->next = NULL;

  if (last == NULL)
    {
      *list = new_;
    }
  else
    {
      last->next = new_;
    }
  status = 1;

error:
  if (added_ptr != NULL)
    {
      *added_ptr = status;
    }
  return NO_ERROR;
}

/*
 * nlist_find_or_append - searches for a name or appends the element.
 *    return: error code
 *    list(in/out): pointer to pointer to list head
 *    name(in): name of element to add
 *    fcn(in): compare funciont
 *    position(out): position of element if found or inserted
 */
int
nlist_find_or_append (DB_NAMELIST ** list, const char *name,
		      NLSEARCHER fcn, int *position)
{
  DB_NAMELIST *el, *found, *last, *new_;
  int psn = -1;

  if (fcn == NULL)
    {
      fcn = (NLSEARCHER) strcmp;
    }

  if (name != NULL)
    {
      found = last = NULL;
      for (el = *list, psn = 0; el != NULL && found == NULL; el = el->next)
	{
	  if ((el->name == name) ||
	      ((el->name != NULL) && (*fcn) (el->name, name) == 0))
	    {
	      found = el;
	    }
	  else
	    {
	      psn++;
	    }
	  last = el;
	}
      if (found == NULL)
	{
	  new_ = (DB_NAMELIST *) db_ws_alloc (sizeof (DB_NAMELIST));
	  if (new_ == NULL)
	    {
	      return er_errid ();
	    }

	  new_->name = ws_copy_string (name);
	  if (new_->name == NULL)
	    {
	      db_ws_free (new_);
	      return er_errid ();
	    }

	  new_->next = NULL;
	  if (last == NULL)
	    {
	      *list = new_;
	    }
	  else
	    {
	      last->next = new_;
	    }
	}
    }
  *position = psn;
  return NO_ERROR;
}
#endif /* ENABLE_UNUSED_FUNCTION */


/*
 * nlist_free - frees a name list
 *    return: none
 *    list(in/out): list to free
 */
void
nlist_free (DB_NAMELIST * list)
{
  DB_NAMELIST *el, *next;

  for (el = list, next = NULL; el != NULL; el = next)
    {
      next = el->next;
      db_ws_free ((char *) el->name);
      db_ws_free (el);
    }
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * nlist_copy - makes a copy of a named list
 *    return: new namelist
 *    list(in): namelist to copy
 */
DB_NAMELIST *
nlist_copy (DB_NAMELIST * list)
{
  DB_NAMELIST *first, *last, *el, *new_;

  first = last = NULL;
  for (el = list; el != NULL; el = el->next)
    {
      new_ = (DB_NAMELIST *) db_ws_alloc (sizeof (DB_NAMELIST));
      if (new_ == NULL)
	{
	  goto memory_error;
	}

      new_->name = ws_copy_string (el->name);
      if (new_->name == NULL)
	{
	  db_ws_free (new_);
	  goto memory_error;
	}

      new_->next = NULL;
      if (first == NULL)
	{
	  first = new_;
	}
      else
	{
	  last->next = new_;
	}
      last = new_;
    }
  return first;

memory_error:
  nlist_free (first);
  return NULL;
}
#endif

/*
 * nlist_filter - remove all elements with the given name from a list
 * and return a list of the removed elements.
 *    return: filtered list of elements
 *    root(in/out): pointer to pointer to list head
 *    name(in): name of elements to filter
 *    fcn(in): compare function
 */
DB_NAMELIST *
nlist_filter (DB_NAMELIST ** root, const char *name, NLSEARCHER fcn)
{
  DB_NAMELIST *el, *prev, *next, *head, *filter;

  if (fcn == NULL)
    {
      fcn = (NLSEARCHER) strcmp;
    }

  filter = NULL;
  head = *root;

  for (el = head, prev = NULL, next = NULL; el != NULL; el = next)
    {
      next = el->next;
      if ((el->name == name) ||
	  ((el->name != NULL) && (name != NULL)
	   && (*fcn) (el->name, name) == 0))
	{
	  if (prev == NULL)
	    {
	      head = next;
	    }
	  else
	    {
	      prev->next = next;
	    }
	  el->next = filter;
	  filter = el;
	}
      else
	{
	  prev = el;
	}
    }

  *root = head;
  return (filter);
}

/*
 * MOP LIST UTILITIES
 */
/*
 * These utilities operate on a list of MOP links.
 * This is such a common operation for the workspace and schema manager that
 * it merits its own optimized implementation.
 *
 */


/*
 * ml_find - searches a list for the given mop.
 *    return: non-zero if mop was in the list
 *    list(in): list to search
 *    mop(in): mop we're looking for
 */
int
ml_find (DB_OBJLIST * list, MOP mop)
{
  DB_OBJLIST *l;
  int found;

  found = 0;
  for (l = list; l != NULL && found == 0; l = l->next)
    {
      if (l->op == mop)
	found = 1;
    }
  return (found);
}


/*
 * ml_add - Adds a MOP to the list if it isn't already present.
 *    return: NO_ERROR or error code
 *    list(in/out): pointer to pointer to list head
 *    mop(in): mop to add to the list
 *    added_ptr(out): set to 1 if added
 * Note:
 *    There is no guarentee where the MOP will be added in the list although
 *    currently it will push it at the head of the list.  Use ml_append
 *    if you must ensure ordering.
 */
int
ml_add (DB_OBJLIST ** list, MOP mop, int *added_ptr)
{
  DB_OBJLIST *l, *found, *new_;
  int added;

  added = 0;
  if (mop == NULL)
    {
      goto error;
    }

  for (l = *list, found = NULL; l != NULL && found == NULL; l = l->next)
    {
      if (l->op == mop)
	{
	  found = l;
	}
    }
  /* since we can get the end of list easily, may want to append here */
  if (found != NULL)
    {
      goto error;
    }

  new_ = (DB_OBJLIST *) db_ws_alloc (sizeof (DB_OBJLIST));
  if (new_ == NULL)
    {
      return er_errid ();
    }
  new_->op = mop;
  new_->next = *list;
  *list = new_;
  added = 1;

error:
  if (added_ptr != NULL)
    {
      *added_ptr = added;
    }
  return NO_ERROR;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * ml_append - Appends a MOP to the list if it isn't already present.
 *    return: NO_ERROR or error code
 *    list(in/out): pointer to pointer to list head
 *    mop(in): mop to add
 *    added_ptr(out): set to 1 if added
 */
int
ml_append (DB_OBJLIST ** list, MOP mop, int *added_ptr)
{
  DB_OBJLIST *l, *found, *new_, *last;
  int added;

  added = 0;
  if (mop == NULL)
    {
      goto error;
    }

  last = NULL;
  for (l = *list, found = NULL; l != NULL && found == NULL; l = l->next)
    {
      if (l->op == mop)
	{
	  found = l;
	}
      last = l;
    }
  /* since we can get the end of list easily, may want to append here */

  if (found != NULL)
    {
      goto error;
    }

  new_ = (DB_OBJLIST *) db_ws_alloc (sizeof (DB_OBJLIST));
  if (new_ == NULL)
    {
      return er_errid ();
    }
  new_->op = mop;
  new_->next = NULL;
  if (last == NULL)
    {
      *list = new_;
    }
  else
    {
      last->next = new_;
    }
  added = 1;

error:

  if (added_ptr != NULL)
    {
      *added_ptr = added;
    }
  return NO_ERROR;
}
#endif

/*
 * ml_remove - removes a mop from a mop list if it is found.
 *    return: non-zero if mop was removed
 *    list(in/out): pointer to pointer to list head
 *    mop(in): mop to remove from the list
 */
int
ml_remove (DB_OBJLIST ** list, MOP mop)
{
  DB_OBJLIST *l, *found, *prev;
  int deleted;

  deleted = 0;
  for (l = *list, found = NULL, prev = NULL; l != NULL && found == NULL;
       l = l->next)
    {
      if (l->op == mop)
	{
	  found = l;
	}
      else
	{
	  prev = l;
	}
    }
  if (found != NULL)
    {
      if (prev == NULL)
	{
	  *list = found->next;
	}
      else
	{
	  prev->next = found->next;
	}
      db_ws_free (found);
      deleted = 1;
    }
  return (deleted);
}


/*
 * ml_free - free a list of MOPs.
 *    return: none
 *    list(in/out): list to free
 */
void
ml_free (DB_OBJLIST * list)
{
  DB_OBJLIST *l, *next;

  for (l = list, next = NULL; l != NULL; l = next)
    {
      next = l->next;
      db_ws_free (l);
    }
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * ml_copy - copy a list of mops.
 *    return: new list
 *    list(in): list to copy
 */
DB_OBJLIST *
ml_copy (DB_OBJLIST * list)
{
  DB_OBJLIST *l, *new_, *first, *last;

  first = last = NULL;
  for (l = list; l != NULL; l = l->next)
    {
      new_ = (DB_OBJLIST *) db_ws_alloc (sizeof (DB_OBJLIST));
      if (new_ == NULL)
	{
	  goto memory_error;
	}

      new_->next = NULL;
      new_->op = l->op;
      if (first == NULL)
	{
	  first = new_;
	}
      else
	{
	  last->next = new_;
	}
      last = new_;
    }
  return (first);

memory_error:
  ml_free (first);
  return NULL;
}

/*
 * ml_size - This calculates the number of bytes of memory required for the
 * storage of a MOP list.
 *    return: memory size of list
 *    list(in): list to examine
 */
int
ml_size (DB_OBJLIST * list)
{
  int size = 0;

  size = ws_list_length ((DB_LIST *) list) * sizeof (DB_OBJLIST);

  return (size);
}
#endif

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * ml_filter - maps a function over the mops in a list selectively removing
 * elements based on the results of the filter function.
 *    return: void
 *    list(in/out): pointer to pointer to mop list
 *    filter(in): filter function
 *    args(in): args to pass to filter function
 * Note:
 *    If the filter function returns zero, the mop will be removed.
 */
void
ml_filter (DB_OBJLIST ** list, MOPFILTER filter, void *args)
{
  DB_OBJLIST *l, *prev, *next;
  int keep;

  prev = NULL;
  next = NULL;

  for (l = *list; l != NULL; l = next)
    {
      next = l->next;
      keep = (*filter) (l->op, args);
      if (keep)
	{
	  prev = l;
	}
      else
	{
	  if (prev != NULL)
	    {
	      prev->next = next;
	    }
	  else
	    {
	      *list = next;
	    }
	}
    }
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * DB_OBJLIST AREA ALLOCATION
 */


/*
 * ml_ext_alloc_link - This is used to allocate a mop list link for return to
 * the application layer.
 *    return: new mop list link
 * Note:
 *    These links must be allocated in areas outside the workspace
 *    so they serve as roots to the garabage collector.
 */
DB_OBJLIST *
ml_ext_alloc_link (void)
{
  return ((DB_OBJLIST *) malloc (sizeof (DB_OBJLIST)));
}


/*
 * ml_ext_free_link - frees a mop list link that was allocated with
 * ml_ext_alloc_link.
 *    return: void
 *    link(in/out): link to free
 */
void
ml_ext_free_link (DB_OBJLIST * link)
{
  if (link != NULL)
    {
      link->op = NULL;		/* this is important */
      free_and_init (link);
    }
}


/*
 * ml_ext_free - frees a complete list of links allocated with the
 * ml_ext_alloc_link function.
 *    return: void
 *    list(in/out): list to free
 */
void
ml_ext_free (DB_OBJLIST * list)
{
  DB_OBJLIST *l, *next;

  if (list == NULL)
    {
      return;
    }

  for (l = list, next = NULL; l != NULL; l = next)
    {
      next = l->next;
      ml_ext_free_link (l);
    }
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * ml_ext_copy - Like ml_copy except that it allocates the mop list links using
 * ml_ext_alloc_link so they can be returned to the application level.
 *    return: new mop list
 *    list(in): list to copy
 */
DB_OBJLIST *
ml_ext_copy (DB_OBJLIST * list)
{
  DB_OBJLIST *l, *new_, *first, *last;

  first = NULL;
  last = NULL;

  for (l = list; l != NULL; l = l->next)
    {
      new_ = ml_ext_alloc_link ();
      if (new_ == NULL)
	{
	  goto memory_error;
	}
      new_->next = NULL;
      new_->op = l->op;
      if (first == NULL)
	{
	  first = new_;
	}
      else
	{
	  last->next = new_;
	}
      last = new_;
    }
  return (first);

memory_error:
  ml_ext_free (first);
  return NULL;
}
#endif

#if 0
/*
 * ml_ext_add - same as ml_add except that it allocates a mop in the external
 * area.
 *    return: NO_ERROR or error code
 *    list(in/out): pointer to pointer to list head
 *    mop(in): mop to add to the list
 *    added_ptr(out): set to 1 if added
 */
int
ml_ext_add (DB_OBJLIST ** list, MOP mop, int *added_ptr)
{
  DB_OBJLIST *l, *found, *new_;
  int added;

  added = 0;
  if (mop == NULL)
    {
      goto error;
    }
  for (l = *list, found = NULL; l != NULL && found == NULL; l = l->next)
    {
      if (l->op == mop)
	{
	  found = l;
	}
    }
  /* since we can get the end of list easily, may want to append here */
  if (found == NULL)
    {
      new_ = (DB_OBJLIST *) malloc (sizeof (DB_OBJLIST));
      if (new_ == NULL)
	{
	  return er_errid ();
	}

      new_->op = mop;
      new_->next = *list;
      *list = new_;
      added = 1;
    }

error:
  if (added_ptr != NULL)
    {
      *added_ptr = added;
    }
  return NO_ERROR;
}
#endif

/*
 * ws_set_repl_error_into_error_link() -
 *    return: void
 *    mop(in):
 */
void
ws_set_repl_error_into_error_link (LC_COPYAREA_ONEOBJ * obj,
				   char *content_ptr)
{
  WS_REPL_FLUSH_ERR *flush_err;
  char *ptr;

  flush_err = (WS_REPL_FLUSH_ERR *) malloc (sizeof (WS_REPL_FLUSH_ERR));
  if (flush_err == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
	      1, sizeof (WS_REPL_FLUSH_ERR));
      return;
    }

  flush_err->class_oid = obj->class_oid;
  flush_err->operation = obj->operation;

  ptr = content_ptr;
  ptr = or_unpack_db_idxkey (ptr, &flush_err->key);
  assert (ptr != NULL);
  ptr = or_unpack_int (ptr, &flush_err->error_code);
  ptr = or_unpack_string_alloc (ptr, &flush_err->error_msg);

  flush_err->error_link = ws_Repl_error_link;
  ws_Repl_error_link = flush_err;

  return;
}

/*
 * ws_get_error_from_error_link() -
 *    return: void
 */
WS_REPL_FLUSH_ERR *
ws_get_repl_error_from_error_link (void)
{
  WS_REPL_FLUSH_ERR *flush_err;

  flush_err = ws_Repl_error_link;
  if (flush_err == NULL)
    {
      return NULL;
    }

  ws_Repl_error_link = flush_err->error_link;
  flush_err->error_link = NULL;

  return flush_err;
}

/*
 * ws_clear_all_errors_of_error_link() -
 *    return: number of error
 */
int
ws_clear_all_repl_errors_of_error_link (void)
{
  WS_REPL_FLUSH_ERR *flush_err, *next;
  int error_count;

  error_count = 0;
  for (flush_err = ws_Repl_error_link; flush_err; flush_err = next)
    {
      next = flush_err->error_link;
      ws_free_repl_flush_error (flush_err);
      error_count++;
    }
  ws_Repl_error_link = NULL;

  return error_count;
}

/*
 * ws_free_repl_flush_error() -
 *    return: void
 */
void
ws_free_repl_flush_error (WS_REPL_FLUSH_ERR * flush_err)
{
  assert (flush_err != NULL);

  if (flush_err->error_msg != NULL)
    {
      free_and_init (flush_err->error_msg);
    }

  db_idxkey_clear (&flush_err->key);

  free_and_init (flush_err);

  return;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * ws_init_repl_objs() -
 *    free_func (in): function for freeing data in recdes
 *    return: void
 */
void
ws_init_repl_objs (void)
{
  ws_Repl_objs.head = NULL;
  ws_Repl_objs.tail = NULL;
  ws_Repl_objs.num_items = 0;
}
#endif

/*
 * ws_get_repl_obj_from_list() -
 *    return: repl object
 */
WS_REPL_OBJ *
ws_get_repl_obj_from_list (void)
{
  WS_REPL_OBJ *repl_obj;

  repl_obj = ws_Repl_objs.head;
  if (repl_obj != NULL)
    {
      ws_Repl_objs.head = repl_obj->next;
      if (ws_Repl_objs.head == NULL)
	{
	  ws_Repl_objs.tail = NULL;
	}

      ws_Repl_objs.num_items--;
    }

  assert (ws_Repl_objs.num_items >= 0);

  return repl_obj;
}

/*
 * ws_free_repl_obj() -
 *    return:
 */
void
ws_free_repl_obj (WS_REPL_OBJ * obj)
{
  (void) db_idxkey_clear (&obj->key);

  if (obj->recdes != NULL)
    {
      if (obj->recdes->data != NULL)
	{
	  free_and_init (obj->recdes->data);
	}
      free_and_init (obj->recdes);
    }

  free_and_init (obj);

  return;
}

/*
 * ws_clear_all_repl_objs() -
 *    return:
 */
void
ws_clear_all_repl_objs (void)
{
  WS_REPL_OBJ *obj, *next_obj;

  obj = ws_Repl_objs.head;
  while (obj != NULL)
    {
      next_obj = obj->next;
      ws_free_repl_obj (obj);

      obj = next_obj;
    }

  ws_Repl_objs.head = NULL;
  ws_Repl_objs.tail = NULL;
  ws_Repl_objs.num_items = 0;

  return;
}

/*
 * ws_add_to_repl_obj_list() - create a repl object and add it
 *  class_name(in):
 *  key(in):
 *  recdes(in):
 *  operation(in):
 *
 *    return:
 */
int
ws_add_to_repl_obj_list (const char *class_name, DB_IDXKEY * key,
			 RECDES * recdes, int operation)
{
  WS_REPL_OBJ *repl_obj = NULL;
  int error = NO_ERROR;

  if (class_name == NULL || key == NULL || operation == LC_FETCH)
    {
      assert (false);

      error = ER_GENERIC_ERROR;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error,
	      1, "Invalid Arguments");
      return error;
    }

  repl_obj = (WS_REPL_OBJ *) malloc (sizeof (WS_REPL_OBJ));
  if (repl_obj == NULL)
    {
      error = ER_OUT_OF_VIRTUAL_MEMORY;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error,
	      1, sizeof (WS_REPL_OBJ));
      return error;
    }

  DB_IDXKEY_MAKE_NULL (&repl_obj->key);

  strncpy (repl_obj->class_name, class_name, sizeof (repl_obj->class_name));
  error = db_idxkey_clone (key, &repl_obj->key);
  if (error != NO_ERROR)
    {
      return error;
    }

  repl_obj->recdes = recdes;
  repl_obj->operation = operation;
  repl_obj->next = NULL;

  if (ws_Repl_objs.tail == NULL)
    {
      ws_Repl_objs.head = repl_obj;
      ws_Repl_objs.tail = repl_obj;
    }
  else
    {
      ws_Repl_objs.tail->next = repl_obj;
      ws_Repl_objs.tail = repl_obj;
    }
  ws_Repl_objs.num_items++;

  return NO_ERROR;
}
