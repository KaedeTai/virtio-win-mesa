/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_env.h"
#include "npt_overrides.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "npt_ring.h"

#include <stdatomic.h>

#if defined(__WINE__)
#include "nptunix/npt_unixlib.h"

/* Wine PE build: ids are allocated from a process-wide counter held by
 * the unixlib so d3d11.dll / dxgi.dll (which share the single host
 * context wired up by npt_device_acquire) don't collide. */
uint64_t
npt_com_allocate_next_id(void)
{
   if (npt_unixlib_ensure_init() != 0)
      return 0;
   struct npt_unix_alloc_obj_id_params p = { 0 };
   npt_wine_unix_call(npt_unix_alloc_obj_id, &p);
   return p.result_id;
}
#else
/* Monotonic guest-allocated object id. */
static _Atomic uint64_t g_npt_next_obj_id = 1;

uint64_t
npt_com_allocate_next_id(void)
{
   return atomic_fetch_add_explicit(&g_npt_next_obj_id, 1,
                                    memory_order_relaxed);
}
#endif

#include "neptune-protocol/npt_protocol_client_table.h"
#include "neptune-protocol/npt_protocol_defs.h"

#include <stdlib.h>
#include <string.h>

/*
 * IID -> ctor table.  Open-addressing hash, 256 buckets for the ~190
 * interfaces plus headroom.  Populated at init; lookups are lock-free.
 */
#define NPT_COM_CTOR_TABLE_BUCKETS 256u

struct npt_com_ctor_entry {
   const GUID *iid;        /* NULL marks an empty bucket */
   npt_com_ctor_fn ctor;
   size_t aux_size;
   npt_com_aux_init_fn aux_init;
};

static struct npt_com_ctor_entry g_ctor_table[NPT_COM_CTOR_TABLE_BUCKETS];

static inline uint32_t
npt_com_iid_hash(const GUID *iid)
{
   const uint32_t *w = (const uint32_t *)iid;
   uint32_t h = w[0] ^ w[1] ^ w[2] ^ w[3];
   return (uint32_t)((h * 0x9E3779B1u) & (NPT_COM_CTOR_TABLE_BUCKETS - 1u));
}

static inline bool
npt_com_iid_equal(const GUID *a, const GUID *b)
{
   return memcmp(a, b, sizeof(GUID)) == 0;
}

/* Returns NULL only when the table is full. */
static struct npt_com_ctor_entry *
npt_com_table_lookup_entry_mut(const GUID *iid)
{
   if (!iid)
      return NULL;
   uint32_t h = npt_com_iid_hash(iid);
   for (uint32_t i = 0; i < NPT_COM_CTOR_TABLE_BUCKETS; i++) {
      uint32_t idx = (h + i) & (NPT_COM_CTOR_TABLE_BUCKETS - 1u);
      struct npt_com_ctor_entry *e = &g_ctor_table[idx];
      if (e->iid == NULL)
         return e;
      if (npt_com_iid_equal(e->iid, iid))
         return e;
   }
   return NULL;
}

/* == Memoized answers ===============================================
 * A host-backed QueryInterface and a getter whose answer is fixed for
 * the object's lifetime (the registry's canonical "memo" methods) are
 * synchronous ring round trips, and COM identity rules fix their
 * answer -- so both the verdict and the resulting wrapper are memoized
 * on the source wrapper, keyed by the command (0 for QueryInterface)
 * and the IID.  Repeat calls return the same interface pointer with a
 * fresh public ref and no wire traffic.
 *
 * Each positive entry owns one public ref on the memoized wrapper.
 * The memo flushes when the source's public count drains (a lookup
 * takes its ref under the memo lock, so it cannot race the flush even
 * on a wrapper the wrapper cache handed out again meanwhile) and again
 * from npt_com_destroy for the force-destroy path;
 * there the release is gated on the wrapper cache still knowing the
 * wrapper, because the device-teardown drain frees wrappers in
 * arbitrary order.  An answer that is the source itself is never
 * memoized: its ref would keep the public count from ever draining,
 * and the wrapper's tier already records it. */

struct npt_com_memo_entry {
   struct npt_com_memo_entry *next;
   uint32_t key;
   GUID iid;
   struct npt_com_base *wrapper; /* NULL = host answered E_NOINTERFACE */
};

static mtx_t g_memo_mutex;

typedef uint32_t (NPT_STDMETHODCALLTYPE *npt_com_unknown_ref_fn)(void *);

static uint32_t
npt_com_vtbl_addref(struct npt_com_base *com)
{
   return ((npt_com_unknown_ref_fn)com->lpVtbl[1])(com);
}

static uint32_t
npt_com_vtbl_release(struct npt_com_base *com)
{
   return ((npt_com_unknown_ref_fn)com->lpVtbl[2])(com);
}

static struct npt_com_memo_entry *
npt_com_memo_find_locked(struct npt_com_base *com, uint32_t key,
                         const GUID *riid)
{
   for (struct npt_com_memo_entry *e = com->memo; e; e = e->next)
      if (e->key == key && npt_com_iid_equal(&e->iid, riid))
         return e;
   return NULL;
}

/* Idempotent; releases outside the lock so a memoized wrapper's own
 * teardown (which flushes its memo in turn) cannot self-deadlock. */
static void
npt_com_memo_flush(struct npt_com_base *com)
{
   mtx_lock(&g_memo_mutex);
   struct npt_com_memo_entry *e = com->memo;
   com->memo = NULL;
   mtx_unlock(&g_memo_mutex);
   while (e) {
      struct npt_com_memo_entry *next = e->next;
      if (e->wrapper &&
          npt_device_wrapper_cache_is_live(com->base.device,
                                           e->wrapper->base.id, e->wrapper))
         npt_com_vtbl_release(e->wrapper);
      free(e);
      e = next;
   }
}

void *
npt_com_memo_lookup(void *self, uint32_t key, const GUID *riid)
{
   struct npt_com_base *com = self;
   if (!com || !riid)
      return NULL;
   mtx_lock(&g_memo_mutex);
   struct npt_com_memo_entry *e = npt_com_memo_find_locked(com, key, riid);
   struct npt_com_base *w = e ? e->wrapper : NULL;
   /* A flush detaches the memo under this lock, so the memo's hold
    * keeps w alive until this AddRef. */
   if (w)
      npt_com_vtbl_addref(w);
   mtx_unlock(&g_memo_mutex);
   return w;
}

/* Records `wrapper` (NULL: the host answered E_NOINTERFACE) unless an
 * entry already exists, in which case that one is authoritative. */
void
npt_com_memo_store(void *self, uint32_t key, const GUID *riid, void *wrapper)
{
   struct npt_com_base *com = self;
   if (!com || !riid || wrapper == self)
      return;
   struct npt_com_memo_entry *fresh = malloc(sizeof(*fresh));
   if (!fresh)
      return;
   fresh->key = key;
   fresh->iid = *riid;
   fresh->wrapper = wrapper;
   mtx_lock(&g_memo_mutex);
   if (npt_com_memo_find_locked(com, key, riid)) {
      mtx_unlock(&g_memo_mutex);
      free(fresh);
      return;
   }
   if (wrapper)
      npt_com_vtbl_addref(wrapper); /* the memo's hold */
   fresh->next = com->memo;
   com->memo = fresh;
   mtx_unlock(&g_memo_mutex);
}

/* Override families patch vtbls in place rather than registering a
 * different ctor, so existing entries are left alone. */
void
npt_com_register_default_ctor(const GUID *iid, npt_com_ctor_fn ctor)
{
   if (!iid || !ctor)
      return;
   struct npt_com_ctor_entry *e = npt_com_table_lookup_entry_mut(iid);
   if (!e) {
      npt_log("npt_com: ctor table full, dropping registration");
      return;
   }
   if (e->iid == NULL) {
      e->iid = iid;
      e->ctor = ctor;
   }
}


void
npt_com_register_family(const GUID *const *tier_iids,
                        size_t aux_size,
                        npt_com_aux_init_fn aux_init)
{
   if (!tier_iids)
      return;
   for (const GUID *const *p = tier_iids; *p; p++) {
      struct npt_com_ctor_entry *e = npt_com_table_lookup_entry_mut(*p);
      /* Empty slot means register_family ran before the default-ctor
       * populator -- aux metadata would attach to no specific IID. */
      if (!e || e->iid == NULL) {
         npt_log("npt_com_register_family: no ctor entry for IID "
                 "(register_family must run after default-ctor populator)");
         continue;
      }
      e->aux_size = aux_size;
      e->aux_init = aux_init;
   }
}

uint32_t NPT_STDMETHODCALLTYPE
npt_com_default_addref(void *self)
{
   struct npt_com_base *com = self;
   uint32_t prev = atomic_fetch_add(&com->base.pub_ref, 1);
   if (prev == 0) {
      /* 0->1: take the matching private hold on self.  The parent
       * hold lasts for the wrapper's entire lifetime. */
      atomic_fetch_add(&com->base.priv_ref, 1);
   }
   return prev + 1;
}

static void npt_com_destroy(struct npt_com_base *com);
static void npt_com_destroy_cascade(struct npt_com_base *com);
static struct npt_ring *npt_com_acquire_dc_sc_ring(struct npt_device *dev);
static void npt_com_release_dc_sc_ring(struct npt_device *dev);

uint32_t NPT_STDMETHODCALLTYPE
npt_com_default_release(void *self)
{
   struct npt_com_base *com = self;
   uint32_t prev = atomic_fetch_sub(&com->base.pub_ref, 1);
   const uint32_t cur = prev - 1;
   if (cur == 0) {
      /* Drop the memo's wrapper holds while our own 0->1 priv hold is
       * still in place: a memoized child reaching final destruction may
       * drop its wrapper-lifetime parent or identity hold on us, and
       * this hold keeps the count above zero during the flush. */
      npt_com_memo_flush(com);
      /* 1->0: drop the matching private hold on self. */
      uint32_t priv = atomic_fetch_sub(&com->base.priv_ref, 1) - 1;
      if (priv == 0)
         npt_com_destroy_cascade(com);
   }
   return cur;
}

void
npt_com_force_destroy(struct npt_com_base *com)
{
   if (com)
      npt_com_destroy(com);
}

void *
npt_com_get_or_wrap_or_release(struct npt_device *dev, const GUID *iid,
                               uint64_t host_id,
                               struct npt_com_base *parent_wrapper)
{
   if (!dev || !host_id)
      return NULL;
   void *wrapper = npt_com_get_or_wrap(dev, iid, host_id, parent_wrapper);
   if (!wrapper) {
      /* Host added a ref before returning host_id; drain it now. */
      npt_com_send_release(dev, host_id);
   }
   return wrapper;
}

static void
npt_com_destroy(struct npt_com_base *com)
{
   /* Mark the wrapper dying before anything else: the wrapper cache must
    * not hand it out again while cache_remove is still ahead.  The mark
    * is also the re-entry guard for npt_com_force_destroy, which tears a
    * wrapper down while children may still hold it: the QI memo flush
    * and aux_destroy below release other wrappers, and a child's final
    * destruction drops its wrapper-lifetime parent hold.  With the mark
    * every such decrement stays above zero, so the cascade gate cannot
    * re-enter destruction of this wrapper -- freeing aux/com while this
    * call is still running.  A refcount-driven destroy starts only once
    * no child holds this wrapper. */
   atomic_fetch_add_explicit(&com->base.priv_ref, NPT_PRIV_REF_DYING,
                             memory_order_relaxed);

   /* Backstop for force-destroy, which bypasses default_release; a
    * release-driven destroy already flushed and this is a no-op. */
   npt_com_memo_flush(com);

   /* Order: cache_remove (concurrent lookup misses us), COM_RELEASE,
    * DC/SC ring unref (after ring's batch sees the COM_RELEASE),
    * aux_destroy, free, then drop device refs (the last release may
    * destroy dev). */
   if (com->base.device)
      npt_device_wrapper_cache_remove(com->base.device, com->base.id);
   npt_com_send_release(com->base.device, com->base.id);
   if (com->base.device && com->base.instance_ring) {
      if (com->base.private_instance_ring) {
         struct npt_device *dev = com->base.device;
         mtx_lock(&dev->instance_rings_mutex);
         list_del(&com->base.instance_ring->instance_head);
         mtx_unlock(&dev->instance_rings_mutex);
         npt_ring_destroy(com->base.instance_ring);
      } else {
         npt_com_release_dc_sc_ring(com->base.device);
      }
      com->base.instance_ring = NULL;
      com->base.private_instance_ring = false;
   }
   if (com->aux_destroy)
      com->aux_destroy(com->aux);
   atomic_fetch_sub_explicit(&com->base.priv_ref, NPT_PRIV_REF_DYING,
                             memory_order_relaxed);
   const uint32_t device_refs =
      atomic_load_explicit(&com->base.device_ref_holds, memory_order_relaxed);
   free(com);
   for (uint32_t i = 0; i < device_refs; i++)
      npt_device_release();
}

/* Drop one wrapper-lifetime hold on `held` and propagate final
 * destruction when it was the last. */
static void
npt_com_drop_hold_cascade(struct npt_com_base *held)
{
   if (!held)
      return;
   uint32_t priv = atomic_fetch_sub(&held->base.priv_ref, 1) - 1;
   if (priv == 0 && atomic_load(&held->base.pub_ref) == 0)
      npt_com_destroy_cascade(held);
}

/* A child keeps its parent alive, and another family's view of an
 * object keeps that object's identity alive, for the whole wrapper
 * lifetime, including periods where pub_ref is zero but descendants
 * still hold priv_refs on it.  Drop those holds only after the wrapper
 * is actually destroyed, then propagate final destruction up. */
static void
npt_com_destroy_cascade(struct npt_com_base *com)
{
   struct npt_com_base *parent = com->base.parent;
   struct npt_com_base *identity = com->identity;
   npt_com_destroy(com);
   npt_com_drop_hold_cascade(parent);
   npt_com_drop_hold_cascade(identity);
}

/*
 * Pin DC* / SC* IIDs (single-threaded-per-instance by D3D11/DXGI
 * contract) onto the shared DC/SC ring so host dispatch stays serial.
 * Off when multi_ring_enabled is false (primary is then the only
 * dispatch thread, so there's nothing to protect).
 */
static bool
npt_com_iid_needs_instance_ring(const struct npt_device *dev,
                                const GUID *iid)
{
   if (!dev->multi_ring_enabled)
      return false;
   static const GUID *const dc_sc_iids[] = {
      &NPT_IID_ID3D11DeviceContext,
      &NPT_IID_ID3D11DeviceContext1,
      &NPT_IID_ID3D11DeviceContext2,
      &NPT_IID_ID3D11DeviceContext3,
      &NPT_IID_ID3D11DeviceContext4,
      &NPT_IID_IDXGISwapChain,
      &NPT_IID_IDXGISwapChain1,
      &NPT_IID_IDXGISwapChain2,
      &NPT_IID_IDXGISwapChain3,
      &NPT_IID_IDXGISwapChain4,
      &NPT_IID_IDXGISwapChainMedia,
      /* D3D12 command queues are pinned explicitly by type via
       * npt_com_pin_queue_ring (the generic path can't see the queue
       * desc); command lists deliberately stay on TLS rings for
       * parallel recording. */
   };
   for (size_t i = 0;
        i < sizeof(dc_sc_iids) / sizeof(dc_sc_iids[0]); i++) {
      if (npt_com_iid_equal(iid, dc_sc_iids[i]))
         return true;
   }
   return false;
}

/*
 * Lazily create dev's shared DC/SwapChain ring.  DXGI requires Present
 * on the same thread as the immediate context, and the host D3D
 * library has no internal locks coordinating DC vs SC vs Device, so
 * every DC* and SC* wrapper multiplexes through one ring per device.
 */
static struct npt_ring *
npt_com_acquire_dc_sc_ring(struct npt_device *dev)
{
   /* Fast path only while a holder keeps the ring alive: the last
    * release destroys it under the mutex, so a ref taken from a zero
    * count could land on a freed ring. */
   uint32_t refs =
      atomic_load_explicit(&dev->dc_sc_ring_refs, memory_order_relaxed);
   while (refs) {
      if (atomic_compare_exchange_weak_explicit(&dev->dc_sc_ring_refs, &refs,
                                                refs + 1,
                                                memory_order_acquire,
                                                memory_order_relaxed))
         return atomic_load_explicit(&dev->dc_sc_ring, memory_order_acquire);
   }

   mtx_lock(&dev->instance_rings_mutex);
   struct npt_ring *ring =
      atomic_load_explicit(&dev->dc_sc_ring, memory_order_relaxed);
   if (!ring) {
      const size_t extra_size = sizeof(uint32_t);
      struct npt_ring_layout layout;
      npt_ring_get_layout(NPT_DC_SC_RING_BUFFER_SIZE, extra_size, &layout);
      const uint64_t ring_id = atomic_fetch_add(&dev->next_ring_id, 1);
      ring = npt_ring_create(dev, &layout, ring_id, false /* is_tls_ring */);
      if (ring) {
         list_addtail(&ring->instance_head, &dev->instance_rings);
         atomic_store_explicit(&dev->dc_sc_ring, ring, memory_order_release);
      }
   }
   if (ring)
      atomic_fetch_add_explicit(&dev->dc_sc_ring_refs, 1,
                                memory_order_relaxed);
   mtx_unlock(&dev->instance_rings_mutex);
   return ring;
}

void *
npt_com_family_aux(void *self, void (*aux_destroy)(void *aux))
{
   struct npt_com_base *com = self;
   if (!com || !com->aux)
      return NULL;
   if (com->aux_destroy == aux_destroy)
      return com->aux;
   return NULL;
}

static void npt_com_release_dc_sc_ring(struct npt_device *dev);

struct npt_ring *
npt_com_object_ring_seqno(void *self, uint32_t *out_seqno)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   if (out_seqno)
      *out_seqno = ring ? npt_ring_seqno_now(ring) : 0;
   return ring;
}

void
npt_com_pin_queue_ring(void *self, bool direct)
{
   struct npt_com_base *com = self;
   struct npt_device *dev = com ? com->base.device : NULL;
   if (!dev || !dev->multi_ring_enabled || com->base.instance_ring)
      return;

   if (direct) {
      /* Share the DC/SC ring so submissions serialize with present. */
      com->base.instance_ring = npt_com_acquire_dc_sc_ring(dev);
      return;
   }

   struct npt_ring_layout layout;
   npt_ring_get_layout(NPT_DC_SC_RING_BUFFER_SIZE, sizeof(uint32_t),
                       &layout);
   const uint64_t ring_id = atomic_fetch_add(&dev->next_ring_id, 1);
   struct npt_ring *ring = npt_ring_create(dev, &layout, ring_id,
                                           false /* is_tls_ring */);
   if (!ring)
      return; /* fall back to the caller's TLS ring */

   mtx_lock(&dev->instance_rings_mutex);
   list_addtail(&ring->instance_head, &dev->instance_rings);
   mtx_unlock(&dev->instance_rings_mutex);

   com->base.instance_ring = ring;
   com->base.private_instance_ring = true;
}

static void
npt_com_release_dc_sc_ring(struct npt_device *dev)
{
   if (atomic_fetch_sub_explicit(&dev->dc_sc_ring_refs, 1,
                                 memory_order_acq_rel) != 1)
      return;
   mtx_lock(&dev->instance_rings_mutex);
   struct npt_ring *ring =
      atomic_load_explicit(&dev->dc_sc_ring, memory_order_relaxed);
   /* Re-check refs: another thread may have acquired between our
    * dec and the mutex. */
   if (ring &&
       atomic_load_explicit(&dev->dc_sc_ring_refs, memory_order_relaxed) == 0) {
      list_del(&ring->instance_head);
      atomic_store_explicit(&dev->dc_sc_ring, (struct npt_ring *)NULL,
                            memory_order_release);
      mtx_unlock(&dev->instance_rings_mutex);
      npt_ring_destroy(ring);
      return;
   }
   mtx_unlock(&dev->instance_rings_mutex);
}

/* A wrapper for `id` from `entry`'s constructor with its self hold, aux
 * and instance ring, not yet published to the cache. */
static struct npt_com_base *
npt_com_build(struct npt_device *dev, const struct npt_com_ctor_entry *entry,
              const GUID *ctor_iid, uint64_t id)
{
   struct npt_com_base *com = entry->ctor(dev, id);
   if (!com)
      return NULL;
   /* Mirrors default_addref: priv_ref tracks the live pub_ref hold. */
   atomic_fetch_add(&com->base.priv_ref, 1);
   /* On aux OOM the wrapper is still usable; aux-dependent accessors see
    * NULL. */
   if (entry->aux_size) {
      com->aux = calloc(1, entry->aux_size);
      if (com->aux && entry->aux_init)
         entry->aux_init(com, dev, id);
   }
   if (!com->base.instance_ring &&
       npt_com_iid_needs_instance_ring(dev, ctor_iid))
      com->base.instance_ring = npt_com_acquire_dc_sc_ring(dev);
   return com;
}

/* Undo a wrapper that was built but never published to the cache: the
 * cache holds another wrapper for its id, which owns the host ref. */
static void
npt_com_discard_unpublished(struct npt_device *dev, struct npt_com_base *com)
{
   if (com->base.parent) {
      atomic_fetch_sub(&com->base.parent->base.priv_ref, 1);
      com->base.parent = NULL;
   }
   if (com->identity) {
      atomic_fetch_sub(&com->identity->base.priv_ref, 1);
      com->identity = NULL;
   }
   if (com->base.instance_ring) {
      npt_com_release_dc_sc_ring(dev);
      com->base.instance_ring = NULL;
   }
   atomic_store(&com->base.pub_ref, 0);
   atomic_store(&com->base.priv_ref, 0);
   if (com->aux_destroy)
      com->aux_destroy(com->aux);
   free(com);
}

void *
npt_com_get_or_wrap(struct npt_device *dev, const GUID *iid,
                    uint64_t host_id, struct npt_com_base *parent_wrapper)
{
   if (!dev || !host_id)
      return NULL;

   /*
    * Fast path: cache hit bumps pub_ref via CAS.  Slow path: miss,
    * construct, publish atomically.  Races:
    *  - two ctors race: insert_if_absent returns the loser the winner
    *    so we discard our own.
    *  - cache slot occupied by a dying wrapper (final release done, or
    *    marked NPT_PRIV_REF_DYING): insert_if_absent flags retry_needed;
    *    we yield and re-lookup so the dying wrapper's cache_remove +
    *    COM_RELEASE stay paired.  A pub==0 wrapper still held by its
    *    children is not dying; the cache hands it out again.
    */
   for (uint32_t tries = 0; tries < 8; tries++) {
      void *cached = npt_device_wrapper_cache_lookup(dev, host_id);
      if (cached)
         return cached;

      const struct npt_com_ctor_entry *entry =
         npt_com_table_lookup_entry_mut(iid);
      if (!entry || !entry->ctor) {
         npt_log("npt_com_get_or_wrap: no ctor registered for IID");
         return NULL;
      }

      struct npt_com_base *com = npt_com_build(dev, entry, iid, host_id);
      if (!com)
         return NULL;
      com->base.parent = parent_wrapper;
      if (parent_wrapper)
         atomic_fetch_add(&parent_wrapper->base.priv_ref, 1);

      bool retry = false;
      void *winner =
         npt_device_wrapper_cache_insert_if_absent(dev, host_id, com, &retry);
      if (!winner && !retry)
         return com;
      npt_com_discard_unpublished(dev, com);
      if (winner)
         return winner;
      thrd_yield();
   }

   npt_log("npt_com_get_or_wrap: gave up after 8 retries on host_id contention");
   return NULL;
}

HRESULT
npt_com_default_query_interface_chain(void *self, const GUID *riid,
                                      void **out, const GUID *const *chain)
{
   if (!out)
      return NPT_E_POINTER;
   *out = NULL;
   if (!riid || !self)
      return NPT_E_POINTER;
   for (const GUID *const *p = chain; *p; p++) {
      if (npt_com_iid_equal(*p, riid)) {
         npt_com_default_addref(self);
         *out = self;
         return NPT_S_OK;
      }
   }
   return NPT_E_NOINTERFACE;
}

void
npt_com_send_release(struct npt_device *dev, uint64_t host_id)
{
   if (npt_com_id_is_guest_fab(host_id))
      return;
   npt_ring_send_com_release(dev, host_id);
}

/* On the object's own ring: FIFO behind its Create when both came from
 * this thread, and the host waits for the Create otherwise. */
static HRESULT
npt_com_send_query_interface(void *self, const GUID *iid, uint64_t guest_id,
                             struct npt_wire_object_ref *ref)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   if (!ring)
      return NPT_E_FAIL;
   return npt_dispatch_com_query_interface(ring, npt_com_self_id(self), iid,
                                           guest_id, ref);
}

/* Raise the wrapper's confirmed tier to the tier `iid` or `type` names
 * in its family, when that is higher. */
static void
npt_com_confirm_tier(struct npt_com_base *com, const GUID *iid, uint32_t type)
{
   const struct npt_com_family *fam = com->family;
   if (!fam)
      return;
   for (uint32_t k = 0; k < fam->tier_count; k++) {
      if (!(iid && npt_com_iid_equal(fam->tier_iids[k], iid)) &&
          !(type && (uint32_t)fam->tier_types[k] == type))
         continue;
      uint32_t cur = atomic_load_explicit(&com->tier, memory_order_relaxed);
      while (cur < k &&
             !atomic_compare_exchange_weak(&com->tier, &cur, k))
         ;
   }
}

/* Lower the wrapper's denied tier to `k` when that is lower. */
static void
npt_com_deny_tier(struct npt_com_base *com, uint32_t k)
{
   uint32_t cur = atomic_load_explicit(&com->tier_denied, memory_order_relaxed);
   while (cur > k && !atomic_compare_exchange_weak(&com->tier_denied, &cur, k))
      ;
}

void *
npt_com_wrap_canonical(struct npt_device *dev,
                       const struct npt_wire_object_ref *ref, const GUID *iid)
{
   if (!dev || !ref || !ref->id)
      return NULL;
   const uint64_t id = ref->id;

   for (uint32_t tries = 0; tries < 8; tries++) {
      struct npt_com_base *cached = npt_device_wrapper_cache_lookup(dev, id);
      if (cached) {
         npt_com_confirm_tier(cached, iid, ref->type);
         /* The host counted this answer as one more registration of
          * the id; the live wrapper already owns one.  Nothing on any
          * ring names this registration, so the release needs no
          * ordering. */
         npt_ring_send_com_release_immediate(dev, id);
         return cached;
      }

      /* No live wrapper: the one that held the id is being destroyed,
       * and its release and this registration keep the host's count
       * for the id above zero in either order.  Build the wrapper as
       * the host's type, the most derived it has confirmed, so the
       * family vtable covers every interface the object answers to. */
      const GUID *ctor_iid =
         npt_iid_from_object_type((npt_object_type)ref->type);
      const struct npt_com_ctor_entry *entry =
         ctor_iid ? npt_com_table_lookup_entry_mut(ctor_iid) : NULL;
      if (!entry || !entry->iid || !entry->ctor) {
         ctor_iid = iid;
         entry = iid ? npt_com_table_lookup_entry_mut(iid) : NULL;
      }
      if (!entry || !entry->iid || !entry->ctor) {
         npt_log("npt_com_wrap_canonical: no ctor for type %u",
                 (unsigned)ref->type);
         npt_ring_send_com_release_immediate(dev, id);
         return NULL;
      }

      struct npt_com_base *com = npt_com_build(dev, entry, ctor_iid, id);
      if (!com) {
         npt_ring_send_com_release_immediate(dev, id);
         return NULL;
      }
      npt_com_confirm_tier(com, iid, ref->type);
      if (ref->primary_id && ref->primary_id != id) {
         struct npt_com_base *primary =
            npt_device_wrapper_cache_lookup(dev, ref->primary_id);
         if (primary) {
            atomic_fetch_add(&primary->base.priv_ref, 1);
            com->identity = primary;
            npt_com_vtbl_release(primary);
         }
      }

      bool retry = false;
      void *winner =
         npt_device_wrapper_cache_insert_if_absent(dev, id, com, &retry);
      if (winner) {
         npt_com_discard_unpublished(dev, com);
         npt_com_confirm_tier(winner, iid, ref->type);
         npt_ring_send_com_release_immediate(dev, id);
         return winner;
      }
      if (retry) {
         npt_com_discard_unpublished(dev, com);
         thrd_yield();
         continue;
      }
      /* A wrapper minted from a reply has no parent to ride on: it
       * holds the device the way an entry point's wrapper does. */
      npt_device_acquire();
      atomic_fetch_add_explicit(&com->base.device_ref_holds, 1,
                                memory_order_relaxed);
      return com;
   }

   npt_log("npt_com_wrap_canonical: gave up after 8 retries on id contention");
   npt_ring_send_com_release_immediate(dev, id);
   return NULL;
}

HRESULT
npt_com_query_interface_host(void *self, const GUID *riid, void **ppvObject)
{
   struct npt_com_base *com = self;
   if (!ppvObject)
      return NPT_E_POINTER;
   *ppvObject = NULL;
   if (!riid || !self)
      return NPT_E_POINTER;

   mtx_lock(&g_memo_mutex);
   struct npt_com_memo_entry *e = npt_com_memo_find_locked(com, 0, riid);
   if (e) {
      struct npt_com_base *w = e->wrapper;
      /* A flush detaches the memo under this lock, so the memo's hold
       * keeps w alive until this AddRef. */
      if (w)
         npt_com_vtbl_addref(w);
      mtx_unlock(&g_memo_mutex);
      if (!w)
         return NPT_E_NOINTERFACE;
      *ppvObject = w;
      return NPT_S_OK;
   }
   mtx_unlock(&g_memo_mutex);

   /* An interface the guest has no constructor for cannot be handed
    * out, so the host is not asked to register it. */
   const struct npt_com_ctor_entry *entry = npt_com_table_lookup_entry_mut(riid);
   if (!entry || !entry->iid || !entry->ctor)
      return NPT_E_NOINTERFACE;

   /* One host round trip per (object, IID).  The guest id is the one
    * the host registers the interface under when it is new to the
    * guest; when it already holds the object under another id, the
    * reply names that id instead and the guest id stays unused. */
   struct npt_device *dev = npt_com_self_device(self);
   struct npt_wire_object_ref ref;
   memset(&ref, 0, sizeof(ref));
   HRESULT hr = npt_com_send_query_interface(self, riid,
                                             npt_com_allocate_next_id(), &ref);
   if (hr != NPT_S_OK && hr != NPT_E_NOINTERFACE) {
      /* Transport failure: nothing was learned about the IID. */
      return hr;
   }

   struct npt_com_base *wrapper = NULL;
   if (hr == NPT_S_OK) {
      wrapper = npt_com_wrap_canonical(dev, &ref, riid);
      if (!wrapper)
         return NPT_E_OUTOFMEMORY;
   }

   mtx_lock(&g_memo_mutex);
   struct npt_com_memo_entry *prior = npt_com_memo_find_locked(com, 0, riid);
   if (prior) {
      /* A concurrent probe won; its entry is authoritative. */
      struct npt_com_base *keep = prior->wrapper;
      if (keep)
         npt_com_vtbl_addref(keep);
      mtx_unlock(&g_memo_mutex);
      if (wrapper)
         npt_com_vtbl_release(wrapper);
      if (!keep)
         return NPT_E_NOINTERFACE;
      *ppvObject = keep;
      return NPT_S_OK;
   }
   mtx_unlock(&g_memo_mutex);

   npt_com_memo_store(self, 0, riid, wrapper);
   if (!wrapper)
      return NPT_E_NOINTERFACE;
   *ppvObject = wrapper;
   return NPT_S_OK;
}

HRESULT
npt_com_query_interface_family(void *self, const GUID *riid, void **out)
{
   struct npt_com_base *com = self;
   if (!out)
      return NPT_E_POINTER;
   *out = NULL;
   if (!riid || !self)
      return NPT_E_POINTER;

   if (npt_com_iid_equal(riid, &NPT_IID_IUnknown)) {
      struct npt_com_base *target = com->identity ? com->identity : com;
      npt_com_vtbl_addref(target);
      *out = target;
      return NPT_S_OK;
   }

   const struct npt_com_family *fam = com->family;
   if (fam) {
      for (const GUID *const *p = fam->ancestor_iids; *p; p++) {
         if (npt_com_iid_equal(*p, riid)) {
            npt_com_vtbl_addref(com);
            *out = com;
            return NPT_S_OK;
         }
      }
      for (uint32_t k = 0; k < fam->tier_count; k++) {
         if (!npt_com_iid_equal(fam->tier_iids[k], riid))
            continue;
         if (k <= atomic_load_explicit(&com->tier, memory_order_acquire)) {
            npt_com_vtbl_addref(com);
            *out = com;
            return NPT_S_OK;
         }
         if (k >= atomic_load_explicit(&com->tier_denied,
                                       memory_order_relaxed))
            return NPT_E_NOINTERFACE;
         /* The host confirms the tier by answering this wrapper's own
          * id, which npt_com_wrap_canonical records; refusal is final
          * for the object's lifetime. */
         HRESULT hr = npt_com_query_interface_host(self, riid, out);
         if (hr == NPT_E_NOINTERFACE)
            npt_com_deny_tier(com, k);
         return hr;
      }
   }
   /* Another family's view of an object asks the object's primary
    * wrapper for every interface outside its own family: the primary
    * answers its own family from its tier, and memoizes the object's
    * other views on itself alone.  Memos on the views would hold the
    * primary and each other by public refs that never drain. */
   if (com->identity)
      return npt_com_query_interface_family(com->identity, riid, out);
   return npt_com_query_interface_host(self, riid, out);
}

struct npt_private_data_entry {
   struct npt_private_data_entry *next;
   GUID name;
   /* Non-NULL: an interface, held; else `size` bytes of data follow. */
   void *unknown;
   UINT size;
   uint8_t data[];
};

static void
npt_private_data_entry_free(struct npt_private_data_entry *e)
{
   if (e->unknown)
      ((npt_com_unknown_ref_fn)(*(void ***)e->unknown)[2])(e->unknown);
   free(e);
}

void
npt_private_data_init(struct npt_private_data *pd)
{
   mtx_init(&pd->lock, mtx_plain);
   pd->head = NULL;
}

void
npt_private_data_fini(struct npt_private_data *pd)
{
   struct npt_private_data_entry *e = pd->head;
   pd->head = NULL;
   while (e) {
      struct npt_private_data_entry *next = e->next;
      npt_private_data_entry_free(e);
      e = next;
   }
   mtx_destroy(&pd->lock);
}

/* Replaces `name`'s entry with `fresh` (NULL: removes it); the old entry
 * is freed outside the lock, since releasing its interface may run
 * arbitrary app code. */
static void
npt_private_data_replace(struct npt_private_data *pd, const GUID *name,
                         struct npt_private_data_entry *fresh)
{
   struct npt_private_data_entry *old = NULL;
   mtx_lock(&pd->lock);
   for (struct npt_private_data_entry **link = &pd->head; *link;
        link = &(*link)->next) {
      if (npt_com_iid_equal(&(*link)->name, name)) {
         old = *link;
         *link = old->next;
         break;
      }
   }
   if (fresh) {
      fresh->next = pd->head;
      pd->head = fresh;
   }
   mtx_unlock(&pd->lock);
   if (old)
      npt_private_data_entry_free(old);
}

HRESULT
npt_private_data_set(struct npt_private_data *pd, const GUID *name,
                     UINT size, const void *data)
{
   if (!name)
      return NPT_E_INVALIDARG;
   if (!data || !size) {
      npt_private_data_replace(pd, name, NULL);
      return NPT_S_OK;
   }
   struct npt_private_data_entry *e = malloc(sizeof(*e) + size);
   if (!e)
      return NPT_E_OUTOFMEMORY;
   e->name = *name;
   e->unknown = NULL;
   e->size = size;
   memcpy(e->data, data, size);
   npt_private_data_replace(pd, name, e);
   return NPT_S_OK;
}

HRESULT
npt_private_data_set_interface(struct npt_private_data *pd, const GUID *name,
                               const void *unknown)
{
   if (!name)
      return NPT_E_INVALIDARG;
   if (!unknown) {
      npt_private_data_replace(pd, name, NULL);
      return NPT_S_OK;
   }
   struct npt_private_data_entry *e = malloc(sizeof(*e));
   if (!e)
      return NPT_E_OUTOFMEMORY;
   e->name = *name;
   e->unknown = (void *)unknown;
   e->size = sizeof(void *);
   ((npt_com_unknown_ref_fn)(*(void ***)e->unknown)[1])(e->unknown);
   npt_private_data_replace(pd, name, e);
   return NPT_S_OK;
}

HRESULT
npt_private_data_get(struct npt_private_data *pd, const GUID *name,
                     UINT *size, void *data)
{
   if (!name || !size)
      return NPT_E_INVALIDARG;
   HRESULT hr = NPT_S_OK;
   void *unknown = NULL;
   mtx_lock(&pd->lock);
   struct npt_private_data_entry *e = pd->head;
   while (e && !npt_com_iid_equal(&e->name, name))
      e = e->next;
   if (!e) {
      *size = 0;
      hr = NPT_DXGI_ERROR_NOT_FOUND;
   } else if (!data) {
      *size = e->size;
   } else if (*size < e->size) {
      *size = e->size;
      hr = NPT_DXGI_ERROR_MORE_DATA;
   } else {
      *size = e->size;
      if (e->unknown) {
         unknown = e->unknown;
         memcpy(data, &unknown, sizeof(unknown));
         ((npt_com_unknown_ref_fn)(*(void ***)unknown)[1])(unknown);
      } else {
         memcpy(data, e->data, e->size);
      }
   }
   mtx_unlock(&pd->lock);
   return hr;
}

void
npt_com_assert_overridden(const char *iid_name, const char *method_name)
{
   npt_log("npt_com: %s::%s called without override -- this method requires "
           "a hand-written wrapper because its parameters cannot be "
           "auto-marshalled (skip_default).  Aborting.",
           iid_name ? iid_name : "?",
           method_name ? method_name : "?");
   abort();
}

/*
 * Explicit one-time init (not __attribute__((constructor))) to avoid
 * the Wine PE loader-lock deadlock: a ctor that took the loader lock
 * or synchronously hit a Unix bridge would deadlock against Wine's
 * PE loader.
 */

static _Atomic int g_npt_com_init_state;

static void
npt_com_init_impl(void)
{
   /* env_init first so subsequent NPT_PERF(...) branches see the bitmask. */
   npt_env_init();
   npt_profile_init();
   mtx_init(&g_memo_mutex, mtx_plain);

   /* Default ctors must run before override installers below read the
    * populated default vtbls. */
   npt_com_init_default_ctors();

   npt_overrides_d3d11_device_init();
   npt_overrides_d3d11_context_init();
   npt_overrides_d3d11_buffer_init();
   npt_overrides_d3d11_texture_init();
   npt_overrides_dxgi_output_init();
   npt_overrides_dxgi_factory_init();
   npt_overrides_dxgi_factorymedia_init();
   npt_overrides_dxgi_adapter_init();
   npt_overrides_d3d11_fence_init();
   npt_overrides_d3d11_query_init();

   npt_overrides_d3d12_device_init();
   npt_overrides_d3d12_resource_init();
   npt_overrides_d3d12_fence_init();
   npt_overrides_d3d12_queue_init();
   npt_overrides_d3d12_list_init();
   npt_overrides_event_handles_init();
   npt_d3d12_heap_overrides_init();
}

void
npt_com_init(void)
{
   NPT_CALL_ONCE(g_npt_com_init_state, npt_com_init_impl());
}
