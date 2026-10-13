/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Resource wrappers for ID3D11Buffer and ID3D11Texture{1,2,3}D, plus
 * the per-resource Map shadow-staging rename ring.  Most COM interfaces
 * use a plain npt_com_base; buffer and texture families need aux state
 * for staging shmem.
 *
 * Map staging: each resource owns a rename ring of slots.  Slot 0 is
 * allocated eagerly on first ensure_shmem (sync MAP_RESOURCE always lands
 * on slot 0); additional slots grow lazily when the wrap-around slot is
 * still in flight, up to a cap that depends on the slot size.  At the
 * cap, rotate falls back to blocking on pending_seqno.  Slots
 * sub-allocate from the per-device map_pool rather than getting one
 * shmem each, which scales linearly with N on the host.
 */

#ifndef NPT_RESOURCE_H
#define NPT_RESOURCE_H

#include "npt_com.h"
#include "npt_renderer.h"

struct npt_d3d11_buffer;
struct npt_d3d11_texture;

/* ==========================================================================
 * Map shadow-staging rename ring
 * ========================================================================== */

#define NPT_D3D_MAP_SLOT_INIT 1u
/* Largest per-ring cap; the cap of a ring (max_slots) depends on its slot
 * size so small, frequently discarded buffers can run further ahead of
 * the host than large ones (see npt_d3d_map_ring_alloc_shmem). */
#define NPT_D3D_MAP_SLOT_MAX  32u

struct npt_d3d_map_slot {
   /* Ref into the per-device map_pool's current shmem (NULL until
    * lazily allocated).  Held until slot release / ring fini so the
    * pool's old shmem stays alive across pool growth. */
   struct npt_renderer_shmem *shmem;
   /* Mirrors shmem->res_id; 0 if shmem == NULL. */
   uint32_t shmem_res_id;
   /* Byte offset of this slot within shmem (slots cohabit a pool). */
   uint32_t offset;
   /* Next rotate must wait until host head passes this. */
   uint32_t pending_seqno;
   /* Ring the Unmap was submitted on; NULL = dev->ring. */
   struct npt_ring *pending_ring;
   bool in_flight;
};

/* One open Map: the slot it was given and what its Unmap sends.  Several
 * contexts can map one resource at once, and one context several
 * subresources of a texture, so each Unmap closes its own Map's record. */
struct npt_d3d_map_record {
   const void *context;     /* the mapping context's key */
   uint32_t subresource;
   uint32_t slot;
   /* 0: a sync Map, whose host map_state the Unmap reuses; else the host
    * replays Map+memcpy+Unmap with these flags. */
   uint32_t access_flags;
   uint32_t byte_size;
   /* NO_OVERWRITE on host-shared storage: holds no slot, and the Unmap
    * sends nothing. */
   bool in_place;
};

enum npt_d3d_map_slot_choice {
   NPT_D3D_MAP_SLOT_ZERO,      /* sync MAP_RESOURCE */
   NPT_D3D_MAP_SLOT_CURRENT,   /* WRITE_NO_OVERWRITE */
   NPT_D3D_MAP_SLOT_NEXT,      /* WRITE_DISCARD */
   /* Sync MAP_RESOURCE of one of several subresources: slot 0 unless
    * another open Map holds it, then a free slot as for NEXT. */
   NPT_D3D_MAP_SLOT_SYNC,
};

struct npt_d3d_map_ring {
   struct npt_com_base *com;
   /* Deferred contexts on other threads map the same resource, so slot
    * allocation, the choice of the next slot and the in-flight bookkeeping
    * are serialized here.  A slot's shmem, res_id and offset never change
    * once it is allocated, and a caller only names a slot it got back from
    * a locked call (or slot 0 after alloc_shmem), so the accessors below
    * read them without the lock. */
   mtx_t lock;
   /* Set with the first shmem; aligned_slot_size and max_slots are fixed
    * from then on, so a Map that finds it set skips the lock. */
   _Atomic bool ready;
   uint32_t aligned_slot_size;
   uint32_t max_slots;      /* growth cap, <= NPT_D3D_MAP_SLOT_MAX */
   uint32_t active_count;
   uint32_t current_slot;
   /* max_slots entries, allocated with the first shmem; NULL until then. */
   struct npt_d3d_map_slot *slots;
   /* Open Maps: record_count of record_cap entries. */
   struct npt_d3d_map_record *records;
   uint32_t record_count, record_cap;
};

/* Slot size depends on per-resource sizing (byte_width / row_pitch *
 * rows * d), so the alloc is left to the caller's ensure_map_shmem. */
void npt_d3d_map_ring_init(struct npt_d3d_map_ring *r,
                           struct npt_com_base *com);

/* Records aligned_slot_size and ensures NPT_D3D_MAP_SLOT_INIT slots
 * are allocated.  Idempotent on the same size; logs and rejects size
 * drift (buffer/texture descs are immutable). */
bool npt_d3d_map_ring_alloc_shmem(struct npt_d3d_map_ring *r,
                                  uint32_t aligned_slot_size);

/* Drains in-flight slots, drops shmem refs, zeroes the ring. */
void npt_d3d_map_ring_fini(struct npt_d3d_map_ring *r);

static inline uint32_t
npt_d3d_map_ring_slot_offset(const struct npt_d3d_map_ring *r, uint32_t slot)
{
   if (!r->slots || slot >= r->max_slots) return 0;
   return r->slots[slot].offset;
}

static inline void *
npt_d3d_map_ring_slot_ptr(const struct npt_d3d_map_ring *r, uint32_t slot)
{
   if (!r->slots || slot >= r->max_slots || !r->slots[slot].shmem) return NULL;
   return (uint8_t *)r->slots[slot].shmem->mmap_ptr + r->slots[slot].offset;
}

static inline uint32_t
npt_d3d_map_ring_slot_res_id(const struct npt_d3d_map_ring *r, uint32_t slot)
{
   if (!r->slots || slot >= r->max_slots) return 0;
   return r->slots[slot].shmem_res_id;
}

/* Records *rec as the open Map of (rec->context, rec->subresource),
 * replacing any earlier one, with the slot choice picks stored in
 * rec->slot.  NEXT takes the slot after the current one unless it is in
 * flight or another open Map holds it; then it grows up to max_slots
 * before blocking on pending_seqno.  The block runs outside the lock:
 * the host may reach that seqno only after another thread submits work
 * (a fence it waits on), and that thread may be about to Map this
 * resource.  False on allocation failure, with nothing recorded. */
bool npt_d3d_map_ring_open(struct npt_d3d_map_ring *r,
                           struct npt_d3d_map_record *rec,
                           enum npt_d3d_map_slot_choice choice);

/* Records the byte size the open Map of (context, subresource) moves,
 * once a sync Map has learned it. */
void npt_d3d_map_ring_set_byte_size(struct npt_d3d_map_ring *r,
                                    const void *context, uint32_t subresource,
                                    uint32_t byte_size);

/* Copies the open Map of (context, subresource) into *out; false if
 * there is none. */
bool npt_d3d_map_ring_find(struct npt_d3d_map_ring *r, const void *context,
                           uint32_t subresource,
                           struct npt_d3d_map_record *out);

/* Removes the open Map of (context, subresource).  With a ring, its
 * Unmap was submitted there at seqno, and the slot goes in flight in the
 * same step that drops the Map's hold on it, so no other Map is handed
 * the slot before the host has copied out of it. */
void npt_d3d_map_ring_close(struct npt_d3d_map_ring *r, const void *context,
                            uint32_t subresource, struct npt_ring *ring,
                            uint32_t seqno);

/* Orders `ring` after the host's create of the slot's shmem; call before
 * a command on `ring` names the slot. */
void npt_d3d_map_ring_slot_visible(const struct npt_d3d_map_ring *r,
                                   uint32_t slot, struct npt_ring *ring);

/* Block until the host has consumed the Unmap last submitted from slot,
 * for a caller about to write a slot NEXT did not choose. */
void npt_d3d_map_ring_wait_slot(struct npt_d3d_map_ring *r, uint32_t slot);

/* ==========================================================================
 * RESOURCE_UPDATE transfers (UpdateSubresource, initial data,
 * WriteToSubresource)
 * ========================================================================== */

/* Largest payload one RESOURCE_UPDATE carries.  The ring's upload buffer
 * grows to fit the biggest payload it has seen and never shrinks, so a
 * larger transfer is sent as several bands of the same region. */
#define NPT_RESOURCE_UPDATE_CHUNK_BYTES (64u << 20)

/* How to cut a transfer into bands.  `extent` is the destination region
 * in texels (bytes for a buffer) whether or not the caller sends a box;
 * `rows` is rows of memory per slice and `row_texels` the texel rows one
 * row of memory covers (4 under block compression). */
struct npt_update_split {
   D3D11_BOX extent;
   uint32_t rows;
   uint32_t row_texels;
   bool buffer;
};

/* Sends `full` bytes (reserved for the host) of which `copy` are read
 * from `data` (context_id as for npt_dispatch_resource_update), as one
 * RESOURCE_UPDATE when it fits the chunk size and
 * as bands of `split` otherwise.  A NULL `split` marks a region with no
 * band layout (planar formats); an oversized one is then refused. */
bool npt_resource_update(struct npt_ring *ring, uint64_t context_id,
                         uint64_t resource_id,
                         uint32_t subresource, const D3D11_BOX *box,
                         uint32_t row_pitch, uint32_t depth_pitch,
                         const void *data, uint64_t full, uint64_t copy,
                         const struct npt_update_split *split);

/* Texel rows covered by one row of memory: 4 for block-compressed
 * formats, else 1. */
uint32_t npt_dxgi_format_row_texels(DXGI_FORMAT fmt);

/* ==========================================================================
 * ID3D11Buffer wrapper
 * ========================================================================== */

/* NULL on non-buffer resource (used by ctx Map/Unmap). */
struct npt_d3d11_buffer *npt_d3d11_buffer_cast(void *resource);

void npt_d3d11_buffer_set_desc(struct npt_d3d11_buffer *b,
                               const D3D11_BUFFER_DESC *desc);
/* False until set_desc has run; caller then falls back to the wire. */
bool npt_d3d11_buffer_fill_desc(struct npt_d3d11_buffer *b,
                                D3D11_BUFFER_DESC *out);
uint32_t npt_d3d11_buffer_get_byte_width(struct npt_d3d11_buffer *b);

bool npt_d3d11_buffer_ensure_map_shmem(struct npt_d3d11_buffer *b);

/* Host-shared storage (NPT_CAPSET_CAP_D3D11_EXTERNAL_BUFFERS).  The map
 * ring's NO_OVERWRITE Map is free, but its Unmap has the host copy the
 * whole buffer, since D3D11 never says which bytes were written; an app
 * appending small chunks to a large ring buffer pays that copy on every
 * append.  So an eligible buffer that sees a NO_OVERWRITE Map on the
 * immediate context switches: the host gets guest shmems (cookies) to
 * rename onto, the immediate context's DISCARD becomes a sync Map whose
 * reply names the shmem it landed on, and its NO_OVERWRITE writes that
 * shmem in place with no host traffic at all.  A buffer that only ever
 * DISCARDs keeps the map ring, whose DISCARD needs no reply.
 *
 * Only the immediate context takes this path, and it alone reads or
 * writes the claim, the current cookie and the slots.  A deferred
 * context's DISCARD renames the host buffer when its command list
 * executes, not when it is recorded, so once a deferred context maps the
 * buffer a shmem written in place can stop being the buffer's storage.
 * The buffer then leaves the path at the immediate context's next
 * DISCARD: before it, a NO_OVERWRITE sent to the map ring would write a
 * slot missing the bytes written in place.  It also leaves when a
 * DISCARD finds every shmem in use and no more may be added, where the
 * host copy is cheaper than a round trip on every DISCARD. */
#define NPT_D3D11_EXTERNAL_BUFFER_MIN_BYTES (1u << 20)
#define NPT_D3D11_EXTERNAL_SLOT_MAX 8u
void npt_d3d11_buffer_set_external_eligible(struct npt_d3d11_buffer *b);
bool npt_d3d11_buffer_external_eligible(const struct npt_d3d11_buffer *b);
/* A deferred context mapped the buffer; any thread. */
void npt_d3d11_buffer_retire_external(struct npt_d3d11_buffer *b);
bool npt_d3d11_buffer_external_retiring(const struct npt_d3d11_buffer *b);
/* Leave the path for good: the map ring serves every later Map. */
void npt_d3d11_buffer_drop_external(struct npt_d3d11_buffer *b);
/* True on the first call only: that NO_OVERWRITE switches the buffer. */
bool npt_d3d11_buffer_claim_external(struct npt_d3d11_buffer *b);
bool npt_d3d11_buffer_external_claimed(const struct npt_d3d11_buffer *b);
/* Cookie of the shmem holding the current contents, or
 * NPT_EXTERNAL_COOKIE_NONE when they are on the map ring. */
uint32_t npt_d3d11_buffer_external_current(const struct npt_d3d11_buffer *b);
void npt_d3d11_buffer_set_external_current(struct npt_d3d11_buffer *b,
                                           uint32_t cookie);
/* One more shmem for the host to rename onto; false at the buffer's
 * slot cap or on any failure, after which it keeps whatever it has. */
bool npt_d3d11_buffer_add_external_slot(struct npt_d3d11_buffer *b,
                                        struct npt_ring *ring);
void *npt_d3d11_buffer_external_slot_ptr(const struct npt_d3d11_buffer *b,
                                         uint32_t cookie);
/* The open Map is a NO_OVERWRITE on the current shmem: its Unmap sends
 * nothing, since the host never saw the Map. */
void npt_d3d11_buffer_wait_slot(struct npt_d3d11_buffer *b, uint32_t slot);

/* Contexts from CreateDeferredContext{,1,2,3} are tagged deferred before
 * they reach the app; every other context wrapper is the device's
 * immediate context.  A context with no tag storage is neither. */
void npt_d3d11_context_set_deferred(void *context);
bool npt_d3d11_context_is_immediate(void *context);
/* Slot 0's res_id (sync MAP_RESOURCE always lands on slot 0). */
uint32_t npt_d3d11_buffer_get_map_shmem_res_id(struct npt_d3d11_buffer *b);
/* Per-slot res_id for async Unmap on the rename ring. */
uint32_t npt_d3d11_buffer_get_slot_shmem_res_id(struct npt_d3d11_buffer *b,
                                                uint32_t slot);

/* A context's open Map of the buffer (npt_d3d_map_ring_open/find/close,
 * subresource 0). */
bool npt_d3d11_buffer_map_open(struct npt_d3d11_buffer *b, const void *context,
                               enum npt_d3d_map_slot_choice choice,
                               uint32_t access_flags, bool in_place,
                               uint32_t *out_slot);
bool npt_d3d11_buffer_map_find(struct npt_d3d11_buffer *b,
                               const void *context,
                               struct npt_d3d_map_record *out);
void npt_d3d11_buffer_map_close(struct npt_d3d11_buffer *b,
                                const void *context, struct npt_ring *ring,
                                uint32_t seqno);

/* Byte offset of a slot in its (pool-shared) shmem. */
uint32_t npt_d3d11_buffer_slot_offset(const struct npt_d3d11_buffer *b,
                                      uint32_t slot);
void npt_d3d11_buffer_slot_visible(const struct npt_d3d11_buffer *b,
                                   uint32_t slot, struct npt_ring *ring);
void *npt_d3d11_buffer_slot_ptr(const struct npt_d3d11_buffer *b,
                                uint32_t slot);

/* ==========================================================================
 * ID3D11Texture{1,2,3}D wrapper
 * ========================================================================== */

/* NULL on non-texture resource. */
struct npt_d3d11_texture *npt_d3d11_texture_cast(void *resource);

/*
 * Dimension-agnostic flattening of {1D,2D,2D1,3D,3D1}_DESC; unused
 * fields collapse to neutral values (e.g. 1D leaves height/depth at
 * 1, non-*1 tiers leave texture_layout at UNDEFINED).
 */
struct npt_d3d11_texture_desc {
   uint32_t width;
   uint32_t height;
   uint32_t depth;
   uint32_t mip_levels;
   uint32_t array_size;
   DXGI_FORMAT format;
   uint32_t sample_count;
   uint32_t sample_quality;
   uint32_t usage;
   uint32_t bind_flags;
   uint32_t cpu_access_flags;
   uint32_t misc_flags;
   D3D11_TEXTURE_LAYOUT texture_layout;
};

/* The desc's mip count with MipLevels 0 resolved to the full chain. */
uint32_t npt_d3d11_texture_desc_mip_levels(const struct npt_d3d11_texture_desc *d);

/* Must be called before any Map(); otherwise dimensions are 0 and
 * the cached bytes-per-pixel is 0. */
void npt_d3d11_texture_set_desc(struct npt_d3d11_texture *t,
                                const struct npt_d3d11_texture_desc *d);

void npt_d3d11_texture_fill_desc1d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE1D_DESC *out);
void npt_d3d11_texture_fill_desc2d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE2D_DESC *out);
void npt_d3d11_texture_fill_desc2d1(const struct npt_d3d11_texture *t,
                                    D3D11_TEXTURE2D_DESC1 *out);
void npt_d3d11_texture_fill_desc3d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE3D_DESC *out);
void npt_d3d11_texture_fill_desc3d1(const struct npt_d3d11_texture *t,
                                    D3D11_TEXTURE3D_DESC1 *out);

/* False for textures that arrived via swapchain GetBuffer /
 * OpenSharedResource; GetDesc falls back to the sync round-trip. */
bool npt_d3d11_texture_has_desc(const struct npt_d3d11_texture *t);

/* Mappable = STAGING with CPU access, or DYNAMIC with CPU_ACCESS_WRITE,
 * in a format whose CPU layout (row bytes and rows) is known. */
bool npt_d3d11_texture_is_mappable(const struct npt_d3d11_texture *t);

bool npt_d3d11_texture_ensure_map_shmem(struct npt_d3d11_texture *t);
/* Per-slot res_id for async Unmap on the rename ring. */
uint32_t npt_d3d11_texture_get_slot_shmem_res_id(const struct npt_d3d11_texture *t,
                                                 uint32_t slot);
/* Total bytes currently allocated across active slots (informational). */
uint32_t npt_d3d11_texture_get_shmem_size(const struct npt_d3d11_texture *t);
uint32_t npt_d3d11_texture_get_slot_size(const struct npt_d3d11_texture *t);

uint32_t npt_d3d11_texture_slot_offset(const struct npt_d3d11_texture *t,
                                       uint32_t slot);
void npt_d3d11_texture_slot_visible(const struct npt_d3d11_texture *t,
                                    uint32_t slot, struct npt_ring *ring);
void *npt_d3d11_texture_slot_ptr(const struct npt_d3d11_texture *t,
                                 uint32_t slot);

/* A context's open Map of one subresource
 * (npt_d3d_map_ring_open/find/close).  Sync Maps land on slot 0; the
 * cached-pitch fast path rotates. */
bool npt_d3d11_texture_map_open(struct npt_d3d11_texture *t,
                                const void *context, uint32_t subresource,
                                enum npt_d3d_map_slot_choice choice,
                                uint32_t access_flags, uint32_t byte_size,
                                uint32_t *out_slot);
bool npt_d3d11_texture_map_find(struct npt_d3d11_texture *t,
                                const void *context, uint32_t subresource,
                                struct npt_d3d_map_record *out);
void npt_d3d11_texture_map_set_byte_size(struct npt_d3d11_texture *t,
                                         const void *context,
                                         uint32_t subresource,
                                         uint32_t byte_size);
void npt_d3d11_texture_map_close(struct npt_d3d11_texture *t,
                                 const void *context, uint32_t subresource,
                                 struct npt_ring *ring, uint32_t seqno);

/* 0 = uncached: next Map must take the sync path to learn the
 * host's pitches.  Once cached, Map returns a shmem pointer with no
 * wire trip and Unmap fires async. */
uint32_t npt_d3d11_texture_get_cached_row_pitch(const struct npt_d3d11_texture *t);
uint32_t npt_d3d11_texture_get_cached_depth_pitch(const struct npt_d3d11_texture *t);
void npt_d3d11_texture_set_cached_pitches(struct npt_d3d11_texture *t,
                                          uint32_t row_pitch, uint32_t depth_pitch);

uint32_t npt_d3d11_texture_get_subresource_byte_size(const struct npt_d3d11_texture *t,
                                                     uint32_t subresource,
                                                     uint32_t row_pitch,
                                                     uint32_t depth_pitch);

/* Rows of memory (block rows / luma+chroma rows) and depth slices of
 * one subresource; both 0 when the desc is unknown. */
void npt_d3d11_texture_get_map_extent(const struct npt_d3d11_texture *t,
                                      uint32_t subresource,
                                      uint32_t *out_rows,
                                      uint32_t *out_depth);

/* Returns the full-pitch footprint of a no-box UpdateSubresource and
 * stores the D3D-guaranteed readable extent of its source in
 * *out_copy_size; both 0 when the transfer cannot be sized.  *out_split
 * gets the subresource's band layout, and `split_ok` says whether it has
 * one (false for planar formats). */
uint64_t npt_d3d11_texture_get_subresource_update_sizes(const struct npt_d3d11_texture *t,
                                                        uint32_t subresource,
                                                        uint32_t row_pitch,
                                                        uint32_t depth_pitch,
                                                        uint64_t *out_copy_size,
                                                        struct npt_update_split *out_split,
                                                        bool *split_ok);

void npt_d3d11_texture_get_mip_dimensions(const struct npt_d3d11_texture *t,
                                          uint32_t subresource,
                                          uint32_t *out_height,
                                          uint32_t *out_depth);

/* 0 = unknown / unsupported format. */
uint32_t npt_d3d11_texture_get_bytes_per_pixel(const struct npt_d3d11_texture *t);
uint32_t npt_dxgi_format_bytes_per_pixel(DXGI_FORMAT fmt);

/* DXGI_FORMAT_UNKNOWN when set_desc has not run. */
DXGI_FORMAT npt_d3d11_texture_get_format(const struct npt_d3d11_texture *t);

/* Rows of memory spanned by texel_rows rows of texels: a row pitch addresses
 * one row of memory, and the block-compressed families pack four texel rows
 * into one row of 4x4 blocks.  Valid for a partial (box) region. */
uint32_t npt_dxgi_format_block_rows(DXGI_FORMAT fmt, uint32_t texel_rows);
uint32_t npt_dxgi_format_row_bytes(DXGI_FORMAT fmt, uint32_t texel_width);

/* Rows of memory in one whole 2D slice of a mip.  Adds the chroma-plane rows
 * that the planar YUV formats store after the luma plane, so this is valid
 * only for a whole subresource. */
uint32_t npt_dxgi_format_subresource_rows(DXGI_FORMAT fmt, uint32_t height);

/* ==========================================================================
 * Aux blob storage
 * ========================================================================== */

/* Per-buffer storage attached to the COM wrapper (npt_com_register_family
 * gets sizeof(struct npt_d3d11_buffer_aux); aux_init wires destroy in). */
struct npt_d3d11_buffer_aux {
   struct npt_com_base *com;
   uint32_t byte_width;
   /* App's create desc; GetDesc answers guest-side (the desc is
    * immutable per buffer, each wire fetch a sync round trip).  Set
    * before the wrapper escapes CreateBuffer, like the texture desc. */
   D3D11_BUFFER_DESC desc;
   bool has_desc;
   struct npt_d3d_map_ring map_ring;
   /* Host-shared rename storage, indexed by cookie.  eligible and
    * retiring are read on every context's thread; the rest belongs to the
    * immediate context.  A slot is stored before the bind that lets the
    * host hand it out (cleared if the bind fails). */
   struct npt_renderer_shmem *external_slots[NPT_D3D11_EXTERNAL_SLOT_MAX];
   uint32_t external_reserved;
   _Atomic bool external_eligible;
   _Atomic bool external_retiring;
   bool external_claimed;
   uint32_t external_current;
};

void npt_d3d11_buffer_aux_init(struct npt_com_base *com,
                               struct npt_device *dev, uint64_t host_id);

/* Per-texture storage.  Cached desc, the host's map pitches and the
 * rename ring shared with the buffer aux. */
struct npt_d3d11_texture_aux {
   struct npt_com_base *com;
   uint32_t width, height, depth;
   uint32_t mip_levels, array_size;
   DXGI_FORMAT format;
   uint32_t bytes_per_pixel;
   bool has_desc;
   uint32_t usage, cpu_access_flags, bind_flags, misc_flags;
   uint32_t sample_count, sample_quality;
   D3D11_TEXTURE_LAYOUT texture_layout;
   /* Pitches from the first sync MAP_RESOURCE; reused by the
    * WRITE_DISCARD / WRITE_NO_OVERWRITE fast path. */
   uint32_t cached_row_pitch, cached_depth_pitch;
   struct npt_d3d_map_ring map_ring;
};

void npt_d3d11_texture_aux_init(struct npt_com_base *com,
                                struct npt_device *dev, uint64_t host_id);

#endif /* NPT_RESOURCE_H */
