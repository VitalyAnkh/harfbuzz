/*
 * Copyright © 2022  Google, Inc.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Google Author(s): Garret Rieger
 */

#include "graph.hh"
#include "hb-ot-layout-gsubgpos.hh"
#include "OT/Layout/GSUB/ExtensionSubst.hh"

#ifndef GRAPH_GSUBGPOS_GRAPH_HH
#define GRAPH_GSUBGPOS_GRAPH_HH

namespace graph {

struct Lookup;

struct GSTAR : public OT::GSUBGPOS
{
  static GSTAR* graph_to_gstar (graph_t& graph)
  {
    return (GSTAR*) graph.root ().obj.head;
  }

  const void* get_lookup_list_field_offset () const
  {
    switch (u.version.major) {
    case 1: return &(u.version1.lookupList);
#ifndef HB_NO_BORING_EXPANSION
    case 2: return &(u.version2.lookupList);
#endif
    default: return 0;
    }
  }

  void find_lookups (graph_t& graph,
                     hb_hashmap_t<unsigned, Lookup*>& lookups /* OUT */)
  {
    // TODO: template on types, based on gstar version.
    unsigned lookup_list_idx = graph.index_for_offset (graph.root_idx (),
                                                       get_lookup_list_field_offset());

    const OT::LookupList<SmallTypes>* lookupList =
        (const OT::LookupList<SmallTypes>*) graph.object (lookup_list_idx).head;

    for (unsigned i = 0; i < lookupList->len; i++)
    {
      unsigned lookup_idx = graph.index_for_offset (lookup_list_idx, &(lookupList->arrayZ[i]));
      Lookup* lookup = (Lookup*) graph.object (lookup_idx).head;
      lookups.set (lookup_idx, lookup);
    }
  }
};

struct make_extension_context_t
{
  hb_tag_t table_tag;
  graph_t& graph;
  hb_vector_t<char> buffer;
  GSTAR* gstar;
  hb_hashmap_t<unsigned, graph::Lookup*> lookups;

  make_extension_context_t (hb_tag_t table_tag_,
                            graph_t& graph_)
      : table_tag (table_tag_),
        graph (graph_),
        buffer (),
        gstar (graph::GSTAR::graph_to_gstar (graph_)),
        lookups ()
  {
    gstar->find_lookups (graph, lookups);
    unsigned extension_size = OT::ExtensionFormat1<OT::Layout::GSUB_impl::ExtensionSubst>::static_size;
    buffer.alloc (num_non_ext_subtables () * extension_size);
  }

  bool in_error () const
  {
    return buffer.in_error ();
  }

 private:
  unsigned num_non_ext_subtables ();
};

struct Lookup : public OT::Lookup
{
  unsigned number_of_subtables () const
  {
    return subTable.len;
  }

  bool is_extension (hb_tag_t table_tag) const
  {
    return lookupType == extension_type (table_tag);
  }

  bool make_extension (make_extension_context_t& c,
                       unsigned this_index)
  {
    // TODO: use a context_t?
    unsigned type = lookupType;
    unsigned ext_type = extension_type (c.table_tag);
    if (!ext_type || is_extension (c.table_tag))
    {
      // NOOP
      printf("Already extension (obj %u).\n", this_index);
      return true;
    }

    printf("Promoting lookup type %u (obj %u) to extension.\n",
           type,
           this_index);


    for (unsigned i = 0; i < subTable.len; i++)
    {
      unsigned subtable_index = c.graph.index_for_offset (this_index, &subTable[i]);
      if (!make_subtable_extension (c,
                                    this_index,
                                    subtable_index))
        return false;
    }

    lookupType = ext_type;
    return true;
  }

  bool make_subtable_extension (make_extension_context_t& c,
                                unsigned lookup_index,
                                unsigned subtable_index)
  {
    printf("  Promoting subtable %u in lookup %u to extension.\n", subtable_index, lookup_index);

    unsigned type = lookupType;
    unsigned extension_size = OT::ExtensionFormat1<OT::Layout::GSUB_impl::ExtensionSubst>::static_size;
    unsigned start = c.buffer.length;
    unsigned end = start + extension_size;
    if (!c.buffer.resize (c.buffer.length + extension_size))
      // TODO: resizing potentially invalidates existing head/tail pointers.
      return false;

    OT::ExtensionFormat1<OT::Layout::GSUB_impl::ExtensionSubst>* extension =
        (OT::ExtensionFormat1<OT::Layout::GSUB_impl::ExtensionSubst>*) &c.buffer[start];
    extension->format = 1;
    extension->extensionLookupType = type;
    extension->extensionOffset = 0;

    unsigned type_prime = extension->extensionLookupType;
    printf("Assigned type %d to extension\n", type_prime);

    unsigned ext_index = c.graph.new_node (&c.buffer.arrayZ[start],
                                           &c.buffer.arrayZ[end]);
    if (ext_index == (unsigned) -1) return false;

    auto& lookup_vertex = c.graph.vertices_[lookup_index];
    for (auto& l : lookup_vertex.obj.real_links.writer ())
    {
      if (l.objidx == subtable_index)
      {
        // Change lookup to point at the extension.
        printf("  Changing %d to %d\n", l.objidx, ext_index);
        l.objidx = ext_index;
      }
    }

    // Make extension point at the subtable.
    auto& ext_vertex = c.graph.vertices_[ext_index];
    auto& subtable_vertex = c.graph.vertices_[subtable_index];
    auto* l = ext_vertex.obj.real_links.push ();

    l->width = 4;
    l->objidx = subtable_index;
    l->is_signed = 0;
    l->whence = 0;
    l->position = 4;
    l->bias = 0;

    ext_vertex.parents.push (lookup_index);
    subtable_vertex.remap_parent (lookup_index, ext_index);

    return true;
  }

 private:
  unsigned extension_type (hb_tag_t table_tag) const
  {
    switch (table_tag)
    {
    case HB_OT_TAG_GPOS: return 9;
    case HB_OT_TAG_GSUB: return 7;
    default: return 0;
    }
  }
};

}

#endif  /* GRAPH_GSUBGPOS_GRAPH_HH */