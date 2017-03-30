/*
  This file is part of t8code.
  t8code is a C library to manage a collection (a forest) of multiple
  connected adaptive space-trees of general element classes in parallel.

  Copyright (C) 2015 the developers

  t8code is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  t8code is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with t8code; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#include <t8_forest/t8_forest_ghost.h>
#include <t8_forest/t8_forest_types.h>
#include <t8_forest/t8_forest_private.h>
#include <t8_forest.h>
#include <t8_cmesh/t8_cmesh_trees.h>
#include <t8_element_cxx.hxx>

/* We want to export the whole implementation to be callable from "C" */
T8_EXTERN_C_BEGIN ();

/* The information stored for the ghost trees */
typedef struct
{
  t8_gloidx_t         global_id;        /* global id of the tree */
  t8_eclass_t         eclass;   /* The trees element class */
  sc_array_t          elements; /* The ghost elements of that tree */
} t8_ghost_tree_t;

/* The data structure stored in the global_tree_to_ghost_tree hash table. */
typedef struct
{
  t8_gloidx_t         global_id;        /* global tree id */
  size_t              index;    /* the index of that global tree in the ghost_trees array. */
} t8_ghost_gtree_hash_t;

/* The data structure stored in the process_offsets array. */
typedef struct
{
  int                 mpirank;  /* rank of the process */
  size_t              tree_index;       /* index of first ghost of this process in ghost_trees */
  size_t              first_element;    /* the index of the first element in the elements array
                                           of the ghost tree. */
} t8_ghost_process_hash_t;

/* The information stored for the remote trees.
 * Each remote process stores an array of these */
typedef struct
{
  t8_locidx_t         global_id;        /* global id of the tree */
  t8_eclass_t         eclass;   /* The trees element class */
  sc_array_t          elements; /* The ghost elements of that tree */
} t8_ghost_remote_tree_t;

typedef struct
{
  int                 remote_rank;      /* The rank of the remote process */
  sc_array_t          remote_trees;     /* Array of the remote trees of this process */
} t8_ghost_remote_t;

/* Compare two ghost_tree entries. We need this function to sort the
 * ghost_trees array by global_id. */
static int
t8_ghost_tree_compare (const void *tree_a, const void *tree_b)
{
  const t8_ghost_tree_t *A = (const t8_ghost_tree_t *) tree_a;
  const t8_ghost_tree_t *B = (const t8_ghost_tree_t *) tree_b;

  if (A->global_id < B->global_id) {
    return -1;
  }
  return A->global_id != B->global_id;
}

/* The hash function for the global tree hash.
 * As hash value we just return the global tree id. */
static unsigned
t8_ghost_gtree_hash_function (const void *ghost_gtree_hash, const void *data)
{
  const t8_ghost_gtree_hash_t *object =
    (const t8_ghost_gtree_hash_t *) ghost_gtree_hash;

  return (unsigned) object->global_id;
}

/* The equal function for two global tree hash objects.
 * Two t8_ghost_gtree_hash_t are considered equal if theit global
 * tree ids are the same.
 */
static int
t8_ghost_gtree_equal_function (const void *ghost_gtreea,
                               const void *ghost_gtreeb, const void *user)
{
  const t8_ghost_gtree_hash_t *objecta =
    (const t8_ghost_gtree_hash_t *) ghost_gtreea;
  const t8_ghost_gtree_hash_t *objectb =
    (const t8_ghost_gtree_hash_t *) ghost_gtreeb;

  /* return true if and only if the global_ids are the same */
  return objecta->global_id == objectb->global_id;
}

/* The hash value for an entry of the process_offsets hash is the
 * processes mpirank. */
static unsigned
t8_ghost_process_hash_function (const void *process_data,
                                const void *user_data)
{
  const t8_ghost_process_hash_t *process =
    (const t8_ghost_process_hash_t *) process_data;

  return process->mpirank;
}

/* The equal function for the process_offsets array.
 * Two entries are the same if their mpiranks are equal. */
static int
t8_ghost_process_equal_function (const void *process_dataa,
                                 const void *process_datab, const void *user)
{
  const t8_ghost_process_hash_t *processa =
    (const t8_ghost_process_hash_t *) process_dataa;
  const t8_ghost_process_hash_t *processb =
    (const t8_ghost_process_hash_t *) process_datab;

  /* If the ranks are the same then the first_element and tree_index entries
   * must equal. */
  T8_ASSERT (processa->mpirank != processb->mpirank
             || (processa->first_element == processb->first_element
                 && processa->tree_index == processb->tree_index));

  return processa->mpirank == processb->mpirank;
}

/* The hash funtion for the remote_ghosts hash table.
 * The hash value for an mpirank is just the rank */
static unsigned
t8_ghost_remote_hash_function (const void *remote_data, const void *user_data)
{
  const t8_ghost_remote_t *remote = (const t8_ghost_remote_t *) remote_data;

  return remote->remote_rank;
}

/* The equal function for the remote hash table.
 * Two entries are the same if they have the same rank. */
static int
t8_ghost_remote_equal_function (const void *remote_dataa,
                                const void *remote_datab, const void *user)
{
  const t8_ghost_remote_t *remotea = (const t8_ghost_remote_t *) remote_dataa;
  const t8_ghost_remote_t *remoteb = (const t8_ghost_remote_t *) remote_datab;

  return remotea->remote_rank == remoteb->remote_rank;
}

void
t8_forest_ghost_init (t8_forest_ghost_t * pghost)
{
  t8_forest_ghost_t   ghost;

  /* Allocate memory for ghost */
  ghost = *pghost = T8_ALLOC_ZERO (t8_forest_ghost_struct_t, 1);
  /* initialize the reference counter */
  t8_refcount_init (&ghost->rc);
  /* Allocate the trees array */
  ghost->ghost_trees = sc_array_new (sizeof (t8_ghost_tree_t));

  /* initialize the global_tree_to_ghost_tree hash table */
  ghost->glo_tree_mempool = sc_mempool_new (sizeof (t8_ghost_gtree_hash_t));
  ghost->global_tree_to_ghost_tree =
    sc_hash_new (t8_ghost_gtree_hash_function, t8_ghost_gtree_equal_function,
                 NULL, NULL);

  /* initialize the process_offset hash table */
  ghost->proc_offset_mempool =
    sc_mempool_new (sizeof (t8_ghost_process_hash_t));
  ghost->process_offsets =
    sc_hash_new (t8_ghost_process_hash_function,
                 t8_ghost_process_equal_function, NULL, NULL);
  /* initialize the processes array */
  ghost->processes = sc_array_new (sizeof (int));
  /* initialize the remote ghosts hash table */
  ghost->remote_ghosts =
    sc_hash_array_new (sizeof (t8_ghost_remote_t),
                       t8_ghost_remote_hash_function,
                       t8_ghost_remote_equal_function, NULL);
  /* initialize the remote processes array */
  ghost->remote_processes = sc_array_new (sizeof (int));
}

/* Given a local tree in a forest add all non-local face neighbor trees
 * to a ghost structure. If the trees already exist in the ghost structure
 * they are not added.
 */
static void
t8_forest_ghost_add_tree (t8_forest_t forest, t8_forest_ghost_t ghost,
                          t8_gloidx_t gtreeid)
{
  t8_cmesh_t          cmesh;
  t8_eclass_t         eclass;
  t8_locidx_t         lctree_id, num_cmesh_local_trees;
  t8_locidx_t         lctreeid;
  sc_mempool_t       *hash_mempool;
  t8_ghost_gtree_hash_t *global_to_index_entry;
  int                 is_inserted;

  T8_ASSERT (t8_forest_is_committed (forest));
  T8_ASSERT (ghost != NULL);

  cmesh = t8_forest_get_cmesh (forest);
  /* Compute the cmesh local id of the tree */
  lctreeid = gtreeid - t8_cmesh_get_first_treeid (cmesh);
  num_cmesh_local_trees = t8_cmesh_get_num_local_trees (cmesh);
  t8_debugf ("[H] Adding global tree %li to ghost, cid %li\n", gtreeid,
             (long) lctreeid);
  /* The tree must be a local tree or ghost tree in the cmesh */
  T8_ASSERT (0 <= lctree_id && lctree_id < num_cmesh_local_trees
             + t8_cmesh_get_num_ghosts (cmesh));

  /* Get the coarse tree and its face-neighbor information */
  if (lctree_id < num_cmesh_local_trees) {
    /* The tree is a local tree in the cmesh */
    eclass = t8_cmesh_get_tree_class (cmesh, lctreeid);
  }
  else {
    /* The tree is a ghost in the cmesh */
    eclass = t8_cmesh_get_ghost_class (cmesh,
                                       lctreeid - cmesh->num_local_trees);
  }

  /* Build a new entry for the global_tree_to_ghost_tree hashtable */
  hash_mempool = ghost->global_tree_to_ghost_tree->allocator;
  global_to_index_entry =
    (t8_ghost_gtree_hash_t *) sc_mempool_alloc (hash_mempool);
  global_to_index_entry->global_id = gtreeid;
  /* Try to add the entry to the array */
  is_inserted = sc_hash_insert_unique (ghost->global_tree_to_ghost_tree,
                                       global_to_index_entry, NULL);
  if (!is_inserted) {
    /* The tree was already added.
     * clean-up and exit */
    sc_mempool_free (hash_mempool, global_to_index_entry);
    return;
  }
  else {
    t8_ghost_tree_t    *ghost_tree;
    /* The tree was not already added. */
    /* Create the entry in the ghost_trees array */
    ghost_tree = (t8_ghost_tree_t *) sc_array_push (ghost->ghost_trees);
    ghost_tree->eclass = eclass;
    ghost_tree->global_id = gtreeid;
    sc_array_init (&ghost_tree->elements, sizeof (t8_element_t *));
    /* Store the array-index of ghost_tree in the hashtable */
    global_to_index_entry->index = ghost->ghost_trees->elem_count - 1;
  }
}

/* Fill the ghost_trees array of a ghost structure with an entry
 * for each ghost tree of the forest.
 * This function does not create the process_offset table yet. */
/* TODO: For the first and last tree we may add more trees than
 *       neccessary, since we add all non-local faceneighbors, and
 *       for these trees not all face-neighbors must contain ghost elements.
 */
static void
t8_forest_ghost_fill_ghost_tree_array (t8_forest_t forest,
                                       t8_forest_ghost_t ghost)
{
  t8_locidx_t         itree, num_local_trees;
  t8_cmesh_t          cmesh;
  t8_ctree_t          ctree;
  t8_locidx_t        *face_neighbors, lneighid, first_ctreeid;
  int                 iface, num_faces;
  t8_ghost_gtree_hash_t global_tree_tgt_search;
  t8_ghost_gtree_hash_t **pglobal_tree_tgt_entry, *global_tree_tgt_entry;
  t8_ghost_tree_t    *ghost_tree;
  size_t              it;

  T8_ASSERT (t8_forest_is_committed (forest));
  T8_ASSERT (ghost != NULL);

  num_local_trees = t8_forest_get_num_local_trees (forest);
  /* If the first tree of the forest is shared with other
   * processes, then it must contain ghost elements */
  if (t8_forest_first_tree_shared (forest)) {
    t8_forest_ghost_add_tree (forest, ghost,
                              t8_forest_get_first_local_tree_id (forest));
  }
  /* If the last tree of the forest is shared with other
   * processes, then it must contain ghost elements */
  if (t8_forest_last_tree_shared (forest)) {
    t8_forest_ghost_add_tree (forest, ghost,
                              t8_forest_get_first_local_tree_id (forest)
                              + num_local_trees - 1);
  }

  cmesh = t8_forest_get_cmesh (forest);
  first_ctreeid = t8_cmesh_get_first_treeid (cmesh);
  /* Iterate over all tree */
  for (itree = 0; itree < num_local_trees; itree++) {
    /* Get a pointer to the coarse mesh tree and its face_neighbors */
    ctree = t8_forest_get_coarse_tree_ext (forest, itree, &face_neighbors,
                                           NULL);
    num_faces = t8_eclass_num_faces[ctree->eclass];
    /* Iterate over all faces of this tree */
    for (iface = 0; iface < num_faces; iface++) {
      /* Compute the (theoretical) forest local id of the neighbor */
      lneighid = t8_forest_cmesh_ltreeid_to_ltreeid (forest,
                                                     face_neighbors[iface]);
      if (lneighid == -1) {
        /* This face neighbor is not a forest local tree.
         * We thus add it to the ghost trees */
        t8_forest_ghost_add_tree (forest, ghost,
                                  ctree->treeid + first_ctreeid);
      }
    }
  }
  /* Now we have added all trees to the array, we have to sort them
   * according to their global_id */
  sc_array_sort (ghost->ghost_trees, t8_ghost_tree_compare);
  /* After sorting, we have to reset the global_tree_to_ghost_tree entries
   * since these store for a global tree id the index in ghost->ghost_trees,
   * which has changed now. */
  for (it = 0; it < ghost->ghost_trees->elem_count; it++) {
    ghost_tree = (t8_ghost_tree_t *) sc_array_index (ghost->ghost_trees, it);
    /* Find the entry belonging to this ghost_tree in the hash table */
    global_tree_tgt_search.global_id = ghost_tree->global_id;
    sc_hash_insert_unique (ghost->global_tree_to_ghost_tree,
                           &global_tree_tgt_search,
                           (void ***) &pglobal_tree_tgt_entry);
    global_tree_tgt_entry = *pglobal_tree_tgt_entry;
    /* Check if the entry that we found was already included and
     * not added to the hash table */
    T8_ASSERT (global_tree_tgt_entry != &global_tree_tgt_search);
    /* Also check for obvious equality */
    T8_ASSERT (global_tree_tgt_entry->global_id == ghost_tree->global_id);
    /* Set the new array index */
    global_tree_tgt_entry->index = it;
  }
}

/* Initialize a t8_ghost_remote_tree_t */
static void
t8_ghost_init_remote_tree (t8_forest_t forest, t8_gloidx_t gtreeid,
                           t8_eclass_t eclass,
                           t8_ghost_remote_tree_t * remote_tree)
{
  t8_eclass_scheme_c *ts;
  t8_locidx_t         local_treeid;

  T8_ASSERT (remote_tree != NULL);

  ts = t8_forest_get_eclass_scheme (forest, eclass);
  local_treeid = gtreeid - t8_forest_get_first_local_tree_id (forest);
  /* Set the entries of the new remote tree */
  remote_tree->global_id = gtreeid;
  remote_tree->eclass = t8_forest_get_eclass (forest, local_treeid);
  /* Initialize the array to store the element */
  sc_array_init (&remote_tree->elements, ts->t8_element_size ());
}

/* Add a new element to the remote hash table (if not already in it).
 * Must be called for elements in linear order */
static void
t8_ghost_add_remote (t8_forest_t forest, t8_forest_ghost_t ghost,
                     int remote_rank, t8_locidx_t ltreeid,
                     t8_element_t * elem)
{
  t8_ghost_remote_t   remote_entry_lookup, *remote_entry;
  t8_ghost_remote_tree_t *remote_tree;
  t8_element_t       *elem_copy;
  t8_eclass_scheme_c *ts;
  t8_eclass_t         eclass;
  sc_array_t         *remote_array;
  size_t              index;
  t8_gloidx_t         gtreeid;
  int                *remote_process_entry;
  int                 level, copy_level;

  /* Get the tree's element class and the scheme */
  eclass = t8_forest_get_tree_class (forest, ltreeid);
  ts = t8_forest_get_eclass_scheme (forest, eclass);
  gtreeid = t8_forest_get_first_local_tree_id (forest) + ltreeid;

  /* Check whether the remote_rank is already present in the remote ghosts
   * array. */
  remote_entry_lookup.remote_rank = remote_rank;
  remote_entry = (t8_ghost_remote_t *)
    sc_hash_array_insert_unique (ghost->remote_ghosts,
                                 (void *) &remote_entry_lookup, &index);
  if (remote_entry != NULL) {
    /* The remote rank was not in the array and was inserted now */
    remote_entry->remote_rank = remote_rank;
    /* Initialize the tree array of the new entry */
    sc_array_init_size (&remote_entry->remote_trees,
                        sizeof (t8_ghost_remote_tree_t), 1);
    /* Get a pointer to the new entry */
    remote_tree = (t8_ghost_remote_tree_t *)
      sc_array_index (&remote_entry->remote_trees, 0);
    /* initialize the remote_tree */
    t8_ghost_init_remote_tree (forest, gtreeid, eclass, remote_tree);
    /* Since the rank is a new remote rank, we also add it to the
     * remote ranks array */
    remote_process_entry = (int *) sc_array_push (ghost->remote_processes);
    *remote_process_entry = remote_rank;
  }
  else {
    /* The remote rank alrady is contained in the remotes array at
     * position index. */
    remote_array = &ghost->remote_ghosts->a;
    remote_entry = (t8_ghost_remote_t *) sc_array_index (remote_array, index);
    T8_ASSERT (remote_entry->remote_rank == remote_rank);
    /* Check whether the tree has already an entry for this process.
     * Since we only add in local tree order the current tree is either
     * the last entry or does not have an entry yet. */
    remote_tree = (t8_ghost_remote_tree_t *)
      sc_array_index (&remote_entry->remote_trees,
                      remote_entry->remote_trees.elem_count - 1);
    if (remote_tree->global_id != gtreeid) {
      /* The tree does not exist in the array. We thus need to add it and
       * initialize it. */
      remote_tree = (t8_ghost_remote_tree_t *)
        sc_array_push (&remote_entry->remote_trees);
      t8_ghost_init_remote_tree (forest, gtreeid, eclass, remote_tree);
    }
  }
  /* remote_tree now points to a valid entry fore the tree.
   * We can add a copy of the element to the elements array
   * if it does not exist already. If it exists it is the last entry in
   * the array. */
  elem_copy = NULL;
  level = ts->t8_element_level (elem);
  if (remote_tree->elements.elem_count > 0) {
    elem_copy =
      (t8_element_t *) sc_array_index (&remote_tree->elements,
                                       remote_tree->elements.elem_count - 1);
    copy_level = ts->t8_element_level (elem_copy);
  }
  /* Check if the element was not contained in the array.
   * If so, we add a copy of elem to the array.
   * Otherwise, we do nothing. */
  if (elem_copy == NULL ||
      level != copy_level ||
      ts->t8_element_get_linear_id (elem_copy, copy_level) !=
      ts->t8_element_get_linear_id (elem, level)) {
    elem_copy = (t8_element_t *) sc_array_push (&remote_tree->elements);
    ts->t8_element_copy (elem, elem_copy);
    t8_debugf ("[H] Adding element %li of tree %li to proc %i\n",
               ts->t8_element_get_linear_id (elem, level),
               gtreeid, remote_rank);
  }
}

/* Create one layer of ghost elements, following the algorithm
 * in p4est: Scalable Algorithms For Parallel Adaptive
 *           Mesh Refinement On Forests of Octrees
 *           C. Burstedde, L. C. Wilcox, O. Ghattas
 */
void
t8_forest_ghost_create (t8_forest_t forest)
{
  t8_forest_ghost_t   ghost;
  t8_element_t       *elem, *neigh, **half_neighbors;
  t8_locidx_t         num_local_trees, num_tree_elems;
  t8_locidx_t         itree, ielem;
  t8_tree_t           tree;
  t8_eclass_t         tree_class, neigh_class;
  t8_gloidx_t         neighbor_tree;
  t8_eclass_scheme_c *ts, *neigh_scheme;

  int                 iface, num_faces;
  int                 num_face_children, max_num_face_children = 0;
  int                 ichild, owner, ret;

  num_local_trees = t8_forest_get_num_local_trees (forest);

  /* Initialize the ghost structure */
  t8_forest_ghost_init (&forest->ghosts);
  ghost = forest->ghosts;
  /* Create all the ghost trees */
  t8_forest_ghost_fill_ghost_tree_array (forest, ghost);

  /* Loop over the trees of the forest */
  for (itree = 0; itree < num_local_trees; itree++) {
    /* Get a pointer to the tree, the class of the tree, the
     * scheme associated to the class and the number of elements in
     * this tree. */
    tree = t8_forest_get_tree (forest, itree);
    tree_class = t8_forest_get_tree_class (forest, itree);
    ts = t8_forest_get_eclass_scheme (forest, tree_class);

    /* Loop over the elements of this tree */
    num_tree_elems = t8_forest_get_tree_element_count (tree);
    for (ielem = 0; ielem < num_tree_elems; ielem++) {
      /* Get the element of the tree */
      elem = t8_forest_get_tree_element (tree, ielem);
      num_faces = ts->t8_element_num_faces (elem);
      for (iface = 0; iface < num_faces; iface++) {
        /* TODO: Check whether the neighbor element is inside the forest,
         *       if not then do not compute the half_neighbors.
         *       This will save computing time. Needs an "element is in forest" function*/

        /* Get the element class of the neighbor tree */
        neigh_class =
          t8_forest_element_neighbor_eclass (forest, itree, elem, iface);
        neigh_scheme = t8_forest_get_eclass_scheme (forest, neigh_class);
        /* Get the number of face children of the element at this face */
        num_face_children = ts->t8_element_num_face_children (elem, iface);
        /* regrow the half_neighbors array if neccessary */
        if (max_num_face_children < num_face_children) {
          half_neighbors = SC_REALLOC (half_neighbors, t8_element_t *,
                                       num_face_children);
          if (max_num_face_children > 0) {
            /* Clean-up memory */
            neigh_scheme->t8_element_destroy (max_num_face_children,
                                              half_neighbors);
          }
          /* Allocate memory for the half size face neighbors */
          neigh_scheme->t8_element_new (num_face_children, half_neighbors);
          max_num_face_children = num_face_children;
        }
        /* Construct each half size neighbor */
        neighbor_tree =
          t8_forest_element_half_face_neighbors (forest, itree, elem,
                                                 half_neighbors, iface,
                                                 num_face_children);
        if (neighbor_tree >= 0) {
          /* If there exist face neighbor elements (we are not at a domain boundary */
          /* Find the owner process of each face_child */
          for (ichild = 0; ichild < num_face_children; ichild++) {
            /* find the owner */
            owner =
              t8_forest_element_find_owner (forest, neighbor_tree,
                                            half_neighbors[ichild],
                                            neigh_class);
            T8_ASSERT (0 <= owner && owner < forest->mpisize);
            if (owner != forest->mpirank) {
              /* Add the element as a remote element */
              t8_ghost_add_remote (forest, ghost, owner, itree, elem);
            }
          }
        }
      }                         /* end face loop */
    }                           /* end element loop */

  }                             /* end tree loop */
  /* Clean-up memory */
  neigh_scheme->t8_element_destroy (max_num_face_children, half_neighbors);
  SC_FREE (half_neighbors);
}

/* Completely destroy a ghost structure */
static void
t8_forest_ghost_reset (t8_forest_ghost_t * pghost)
{
  t8_forest_ghost_t   ghost;
  size_t              it;
  t8_ghost_remote_t  *remote_entry;

  T8_ASSERT (pghost != NULL);
  ghost = *pghost;
  T8_ASSERT (ghost != NULL);
  T8_ASSERT (ghost->rc.refcount == 0);

  /* Clean-up the arrays */
  sc_array_destroy (ghost->ghost_trees);
  sc_array_destroy (ghost->processes);
  sc_array_destroy (ghost->remote_processes);
  /* Clean-up the hashtables */
  sc_hash_destroy (ghost->global_tree_to_ghost_tree);
  sc_hash_destroy (ghost->process_offsets);
  /* Clean-up the remote ghost entries */
  for (it = 0; it < ghost->remote_ghosts->a.elem_count; it++) {
    remote_entry = (t8_ghost_remote_t *)
      sc_array_index (&ghost->remote_ghosts->a, it);
    sc_array_reset (&remote_entry->remote_trees);
  }
  sc_hash_array_destroy (ghost->remote_ghosts);

  /* Clean-up the memory poolf for the data inside
   * the hash tables */
  sc_mempool_destroy (ghost->glo_tree_mempool);
  sc_mempool_destroy (ghost->proc_offset_mempool);

  /* Free the ghost */
  T8_FREE (ghost);
  pghost = NULL;
}

void
t8_forest_ghost_ref (t8_forest_ghost_t ghost)
{
  T8_ASSERT (ghost != NULL);

  t8_refcount_ref (&ghost->rc);
}

void
t8_forest_ghost_unref (t8_forest_ghost_t * pghost)
{
  t8_forest_ghost_t   ghost;

  T8_ASSERT (pghost != NULL);
  ghost = *pghost;
  T8_ASSERT (ghost != NULL);

  if (t8_refcount_unref (&ghost->rc)) {
    t8_forest_ghost_reset (pghost);
  }
}

void
t8_forest_ghost_destroy (t8_forest_ghost_t * pghost)
{
  T8_ASSERT (pghost != NULL && *pghost != NULL &&
             t8_refcount_is_last (&(*pghost)->rc));
  t8_forest_ghost_unref (pghost);
  T8_ASSERT (*pghost == NULL);
}

T8_EXTERN_C_END ();
