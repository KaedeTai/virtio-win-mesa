/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Overrides for the methods that take an app event HANDLE, beyond
 * SetEventOnCompletion (npt_overrides_d3d1{1,2}_fence.c).  The host
 * resolves an event argument only through a token npt_event_arm
 * registered, and maps anything else to NULL, so the generated thunks,
 * which pass the app's HANDLE through, would never signal the event; and
 * a NULL event turns ID3D12Device1::SetEventOnMultipleFenceCompletion
 * into a wait on the host ring's dispatch thread.
 *
 *  - One-shot completions (multi-fence wait, Flush1, EnqueueSetEvent,
 *    SetBackgroundProcessingMode) arm a single-use token and pass it in
 *    place of the HANDLE; the host signals the token's proxy and the
 *    event waiter signals the app's event.
 *  - The NULL-event multi-fence wait blocks guest-side, like
 *    npt_event_block_until_value.
 *  - The video-memory budget notification is a standing registration
 *    that a single-use token cannot serve.  The budget the guest reports
 *    never changes, so it is accepted locally and never signalled.
 */

#include <inttypes.h>
#include <stdatomic.h>

#include "npt_com.h"
#include "npt_device.h"
#include "npt_event.h"
#include "npt_overrides.h"
#include "npt_ring.h"

#include "neptune-protocol/npt_protocol_client_id3d11devicecontext.h"
#include "neptune-protocol/npt_protocol_client_id3d12device.h"
#include "neptune-protocol/npt_protocol_client_id3d12fence.h"
#include "neptune-protocol/npt_protocol_client_idxgiadapter.h"
#include "neptune-protocol/npt_protocol_client_idxgidevice.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_guest_id3d11devicecontext.h"
#include "neptune-protocol/npt_protocol_guest_id3d12device.h"
#include "neptune-protocol/npt_protocol_guest_idxgidevice.h"

/* The ring a call on `self` rides, ordered after the method ring that
 * carried the token's REGISTER and ARM: the host must have registered the
 * token by the time it decodes the call that names it. */
static struct npt_ring *
event_call_ring(void *self)
{
   struct npt_device *dev = npt_com_self_device(self);
   struct npt_ring *ring = npt_com_self_ring(self);
   struct npt_ring *method = npt_device_method_ring(dev);
   if (ring != method)
      npt_ring_order_after_ring(ring, method, npt_ring_seqno_now(method));
   return ring;
}

static HANDLE
event_token_handle(uint64_t token)
{
   return (HANDLE)(uintptr_t)token;
}

/* ---------- ID3D12Device1::SetEventOnMultipleFenceCompletion ---------- */

static bool
fences_reached(ID3D12Fence *const *fences, const UINT64 *values, UINT count,
               D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags)
{
   const bool any = (flags & D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY) != 0;
   for (UINT i = 0; i < count; i++) {
      const struct npt_id3d12fence_client_vtbl *v =
         (const void *)((struct npt_com_base *)fences[i])->lpVtbl;
      const bool reached = v->GetCompletedValue(fences[i]) >= values[i];
      if (any && reached)
         return true;
      if (!any && !reached)
         return false;
   }
   return !any || count == 0;
}

/* Matches npt_event_block_until_value: the armed wake is the fast path,
 * the slice bounds the cost of a lost one. */
#define EVENT_MULTI_SLICE_MS 20
#define EVENT_MULTI_STALL_SLICES 500

static HRESULT NPT_STDMETHODCALLTYPE
dev1_SetEventOnMultipleFenceCompletion_override(
   void *self, ID3D12Fence **ppFences, const UINT64 *pFenceValues,
   UINT NumFences, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent)
{
   struct npt_device *dev = npt_com_self_device(self);
   if (NumFences && (!ppFences || !pFenceValues))
      return NPT_E_INVALIDARG;
   for (UINT i = 0; i < NumFences; i++) {
      if (!ppFences[i])
         return NPT_E_INVALIDARG;
   }

   if (hEvent) {
      const uint64_t token = npt_event_arm(dev, hEvent);
      if (!token)
         return NPT_E_FAIL;
      npt_async_ID3D12Device1_SetEventOnMultipleFenceCompletion(
         event_call_ring(self), npt_com_self_id(self), ppFences,
         pFenceValues, NumFences, Flags, event_token_handle(token));
      return NPT_S_OK;
   }

   /* NULL event: block until reached.  Never forward that form: the
    * host would wait on its ring dispatch thread, behind which the
    * Signals that satisfy it may be queued. */
   if (fences_reached(ppFences, pFenceValues, NumFences, Flags))
      return NPT_S_OK;
   HANDLE ev = (HANDLE)npt_event_thread_temp_event();
   uint64_t token = 0;
   if (ev) {
      WaitForSingleObject(ev, 0);
      token = npt_event_arm(dev, (void *)ev);
      if (token) {
         npt_async_ID3D12Device1_SetEventOnMultipleFenceCompletion(
            event_call_ring(self), npt_com_self_id(self), ppFences,
            pFenceValues, NumFences, Flags, event_token_handle(token));
      }
   }
   uint32_t slices = 0;
   while (!fences_reached(ppFences, pFenceValues, NumFences, Flags)) {
      if (ev)
         WaitForSingleObject(ev, EVENT_MULTI_SLICE_MS);
      else
         npt_relax_sleep_us(1000);
      if (++slices % EVENT_MULTI_STALL_SLICES == 0) {
         npt_log("d3d12 SetEventOnMultipleFenceCompletion(%u fences, "
                 "flags=0x%x, NULL) still waiting after ~%us (armed=%d)",
                 NumFences, (unsigned)Flags,
                 slices * EVENT_MULTI_SLICE_MS / 1000u, token != 0);
      }
   }
   return NPT_S_OK;
}

/* ---------- ID3D12Device6::SetBackgroundProcessingMode ---------- */

static HRESULT NPT_STDMETHODCALLTYPE
dev6_SetBackgroundProcessingMode_override(
   void *self, D3D12_BACKGROUND_PROCESSING_MODE Mode,
   D3D12_MEASUREMENTS_ACTION MeasurementsAction,
   HANDLE hEventToSignalUponCompletion, BOOL *pbFurtherMeasurementsDesired)
{
   HANDLE wire_event = NULL;
   if (hEventToSignalUponCompletion) {
      const uint64_t token = npt_event_arm(npt_com_self_device(self),
                                           hEventToSignalUponCompletion);
      if (!token)
         return NPT_E_FAIL;
      wire_event = event_token_handle(token);
   }
   return npt_call_ID3D12Device6_SetBackgroundProcessingMode(
      event_call_ring(self), npt_com_self_id(self), Mode, MeasurementsAction,
      wire_event, pbFurtherMeasurementsDesired);
}

/* ---------- ID3D11DeviceContext3::Flush1 ---------- */

static void NPT_STDMETHODCALLTYPE
ctx3_Flush1_override(void *self, D3D11_CONTEXT_TYPE ContextType, HANDLE hEvent)
{
   uint64_t token = 0;
   if (hEvent) {
      token = npt_event_arm(npt_com_self_device(self), hEvent);
      if (!token) {
         /* No arm means no completion signal; the flush still happens,
          * and an event left unsignalled would park the app for good. */
         npt_async_ID3D11DeviceContext3_Flush1(npt_com_self_ring(self),
                                               npt_com_self_id(self),
                                               ContextType, NULL);
         SetEvent(hEvent);
         return;
      }
   }
   npt_async_ID3D11DeviceContext3_Flush1(
      token ? event_call_ring(self) : npt_com_self_ring(self),
      npt_com_self_id(self), ContextType,
      token ? event_token_handle(token) : NULL);
}

/* ---------- IDXGIDevice2::EnqueueSetEvent ---------- */

static HRESULT NPT_STDMETHODCALLTYPE
dxgidev2_EnqueueSetEvent_override(void *self, HANDLE hEvent)
{
   if (!hEvent)
      return NPT_E_INVALIDARG;
   const uint64_t token = npt_event_arm(npt_com_self_device(self), hEvent);
   if (!token)
      return NPT_E_OUTOFMEMORY;
   npt_async_IDXGIDevice2_EnqueueSetEvent(event_call_ring(self),
                                          npt_com_self_id(self),
                                          event_token_handle(token));
   return NPT_S_OK;
}

/* ---------- IDXGIAdapter3 budget notification ---------- */

static _Atomic DWORD budget_cookie_next = 1;

static HRESULT NPT_STDMETHODCALLTYPE
adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override(
   void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self;
   if (!hEvent || !pdwCookie)
      return NPT_E_INVALIDARG;
   *pdwCookie = atomic_fetch_add(&budget_cookie_next, 1);
   return NPT_S_OK;
}

static void NPT_STDMETHODCALLTYPE
adp3_UnregisterVideoMemoryBudgetChangeNotification_override(void *self,
                                                            DWORD dwCookie)
{
   (void)self;
   (void)dwCookie;
}

void
npt_overrides_event_handles_init(void)
{
   NPT_REGISTER_OVERRIDE(id3d12device1, SetEventOnMultipleFenceCompletion,
                         dev1_SetEventOnMultipleFenceCompletion_override);
   NPT_REGISTER_OVERRIDE(id3d12device6, SetBackgroundProcessingMode,
                         dev6_SetBackgroundProcessingMode_override);
   NPT_REGISTER_OVERRIDE(id3d11devicecontext3, Flush1,
                         ctx3_Flush1_override);
   NPT_REGISTER_OVERRIDE(idxgidevice2, EnqueueSetEvent,
                         dxgidev2_EnqueueSetEvent_override);
   NPT_REGISTER_OVERRIDE(
      idxgiadapter3, RegisterVideoMemoryBudgetChangeNotificationEvent,
      adp3_RegisterVideoMemoryBudgetChangeNotificationEvent_override);
   NPT_REGISTER_OVERRIDE(
      idxgiadapter3, UnregisterVideoMemoryBudgetChangeNotification,
      adp3_UnregisterVideoMemoryBudgetChangeNotification_override);
}
