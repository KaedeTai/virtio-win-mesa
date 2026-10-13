/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D11Device{,1..5} overrides:
 *  - CreateBuffer/CreateTexture*: pInitialData->pSysMem can't be
 *    auto-marshalled; create with NULL data + ship payload via
 *    RESOURCE_UPDATE.
 *  - Device4::RegisterDeviceRemovedEvent: short-circuit to the
 *    host library's S_OK + cookie 0xdeadbeef stub.
 *  - Device4::UnregisterDeviceRemoved: noop.
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_object.h"
#include "npt_overrides.h"
#include "npt_resource.h"

#include "neptune-protocol/npt_protocol_client_id3d11device.h"
#include "neptune-protocol/npt_protocol_guest_id3d11device.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* Sentinel cookie matching the host library's stub. */
#define NPT_DEVICE_REMOVED_COOKIE 0xdeadbeef

static const GUID *const device_tiers[] = {
   &NPT_IID_ID3D11Device,  &NPT_IID_ID3D11Device1, &NPT_IID_ID3D11Device2,
   &NPT_IID_ID3D11Device3, &NPT_IID_ID3D11Device4, &NPT_IID_ID3D11Device5,
   NULL,
};

/* A DYNAMIC buffer the host can rename onto guest shmems
 * (npt_d3d11_buffer_set_external_eligible).  Small ones stay on the map
 * ring: what sharing saves is the host copy of the whole buffer at each
 * NO_OVERWRITE Unmap, which only matters for a large one. */
static bool
dev_buffer_external_eligible(const struct npt_device *dev,
                             const D3D11_BUFFER_DESC *desc)
{
   return (dev->renderer->info.caps_flags &
           NPT_CAPSET_CAP_D3D11_EXTERNAL_BUFFERS) &&
          desc && desc->Usage == D3D11_USAGE_DYNAMIC &&
          (desc->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) &&
          !(desc->CPUAccessFlags & D3D11_CPU_ACCESS_READ) &&
          desc->ByteWidth >= NPT_D3D11_EXTERNAL_BUFFER_MIN_BYTES;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateBuffer_override(void *self,
                          const D3D11_BUFFER_DESC *pDesc,
                          const D3D11_SUBRESOURCE_DATA *pInitialData,
                          ID3D11Buffer **ppBuffer)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   /* Sync under multi-ring (cross-ring Create->Use race), async on
    * single-ring (FIFO ordering covers it). */
   ID3D11Buffer *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device_CreateBuffer(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateBuffer(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
      hr = (HRESULT)0;  /* deferred-fatal: assume S_OK */
   }

   if (NPT_SUCCEEDED(hr) && raw && pInitialData && pInitialData->pSysMem &&
       pDesc && pDesc->ByteWidth > 0) {
      /* MSDN: SysMemPitch / SysMemSlicePitch ignored for buffers. */
      const struct npt_update_split split = {
         .extent = { 0, 0, 0, pDesc->ByteWidth, 1, 1 },
         .rows = 1, .row_texels = 1, .buffer = true,
      };
      npt_resource_update(npt_device_method_ring(dev),
                          (uint64_t)(uintptr_t)raw, /*subresource=*/0,
                          /*box=*/NULL, /*row_pitch=*/0, /*depth_pitch=*/0,
                          pInitialData->pSysMem, pDesc->ByteWidth,
                          pDesc->ByteWidth, &split);
   }

   if (NPT_SUCCEEDED(hr) && raw) {
      struct npt_d3d11_buffer *b = (struct npt_d3d11_buffer *)
         npt_com_get_or_wrap_or_release(dev, &NPT_IID_ID3D11Buffer,
                                        (uint64_t)(uintptr_t)raw,
                                        (struct npt_com_base *)self);
      if (b && pDesc)
         npt_d3d11_buffer_set_desc(b, pDesc);
      if (b && ppBuffer && dev_buffer_external_eligible(dev, pDesc))
         npt_d3d11_buffer_set_external_eligible(b);
      if (ppBuffer) {
         *ppBuffer = (ID3D11Buffer *)b;
      } else if (b) {
         /* Validation-only call: drop the caller-owned pub_ref. */
         npt_com_default_release(b);
      }
   } else if (ppBuffer) {
      *ppBuffer = NULL;
   }
   return hr;
}

/* Upload pSysMem via RESOURCE_UPDATE post-Create (the protocol can't
 * serialize unsized void*).  Subresource count = MipLevels * ArraySize.
 *
 * D3D defines SysMemSlicePitch for 3D only and SysMemPitch for 2D and 3D
 * only; outside its resource type each field is free to hold stale
 * garbage, so 1D ignores both and sizes one tight row from the format.
 * The ring reservation charges the full pitch for every row -- the
 * host may consume that much -- but the copy stops at the extent D3D
 * guarantees readable in pSysMem, (rows-1)*pitch + RowSizeInBytes per
 * slice.  Planar formats copy the full footprint, which MSDN documents
 * as rowPitch * subresource_rows with no final-row discount.  The
 * app's pitches already account for the format, so no block-size
 * scaling of the pitch is needed -- only the row count, since one row
 * of memory covers four texel rows under block compression. */
static void
npt_upload_texture_initial_data(struct npt_device *dev,
                                uint64_t tex_id,
                                const D3D11_SUBRESOURCE_DATA *init,
                                uint32_t num_subresources,
                                uint32_t width, uint32_t height, uint32_t depth,
                                uint32_t mip_levels, DXGI_FORMAT format)
{
   if (!init || !num_subresources)
      return;
   for (uint32_t sub = 0; sub < num_subresources; sub++) {
      const D3D11_SUBRESOURCE_DATA *d = &init[sub];
      if (!d->pSysMem)
         continue;
      const uint32_t mip = sub % mip_levels;
      const uint32_t mw = width  > (1u << mip) ? (width  >> mip) : 1u;
      const uint32_t mh = height > (1u << mip) ? (height >> mip) : 1u;
      const uint32_t md = depth  > (1u << mip) ? (depth  >> mip) : 1u;
      const uint32_t rows = npt_dxgi_format_subresource_rows(format, mh);
      const uint32_t last_row = npt_dxgi_format_row_bytes(format, mw);
      const bool tight = last_row &&
         rows == npt_dxgi_format_block_rows(format, mh);
      uint64_t full = 0, copy = 0;
      if (depth > 1u) {
         if (!d->SysMemSlicePitch)
            continue;
         const uint64_t strides =
            (uint64_t)d->SysMemSlicePitch * (md - 1u);
         const uint64_t slice = (uint64_t)d->SysMemPitch * rows;
         full = strides + (slice ? slice : d->SysMemSlicePitch);
         copy = tight && d->SysMemPitch
            ? strides + (uint64_t)d->SysMemPitch * (rows - 1u) + last_row
            : full;
      } else if (height > 1u) {
         if (!d->SysMemPitch)
            continue;
         full = (uint64_t)d->SysMemPitch * rows;
         copy = tight
            ? (uint64_t)d->SysMemPitch * (rows - 1u) + last_row
            : full;
      } else {
         /* A single row: neither pitch carries information D3D
          * promises. */
         full = copy = last_row;
      }
      if (copy > full)
         copy = full;
      if (full == 0 || copy == 0)
         continue;
      const struct npt_update_split split = {
         .extent = { 0, 0, 0, mw, mh, md },
         .rows = rows,
         .row_texels = npt_dxgi_format_row_texels(format),
         .buffer = false,
      };
      const bool planar = rows != npt_dxgi_format_block_rows(format, mh);
      const bool banded = !planar && (depth <= 1u || d->SysMemPitch);
      npt_resource_update(npt_device_method_ring(dev), tex_id, sub,
                          /*box=*/NULL, d->SysMemPitch, d->SysMemSlicePitch,
                          d->pSysMem, full, copy, banded ? &split : NULL);
   }
}

/* Shared post-Create for CreateTexture{1,2,3}D{,1}; only the desc-fill
 * differs.  out_resource=NULL = MSDN validation-only path. */
static void
finish_create_texture(void *self,
                      const GUID *iid, void *raw,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      const struct npt_d3d11_texture_desc *desc,
                      void **out_resource)
{
   if (!raw) {
      if (out_resource) *out_resource = NULL;
      return;
   }
   struct npt_device *dev = npt_com_self_device(self);
   const uint64_t raw_id  = (uint64_t)(uintptr_t)raw;

   if (pInitialData) {
      const uint32_t mips = npt_d3d11_texture_desc_mip_levels(desc);
      const uint32_t arr  = desc->array_size ? desc->array_size : 1u;
      npt_upload_texture_initial_data(dev, raw_id,
                                      pInitialData, mips * arr,
                                      desc->width, desc->height, desc->depth,
                                      mips, desc->format);
   }

   struct npt_d3d11_texture *t = (struct npt_d3d11_texture *)
      npt_com_get_or_wrap_or_release(dev, iid, raw_id,
                                     (struct npt_com_base *)self);
   if (t && npt_d3d11_texture_cast(t))
      npt_d3d11_texture_set_desc(t, desc);
   if (out_resource) {
      *out_resource = t;
   } else if (t) {
      /* Validation-only call: drop the caller-owned pub_ref. */
      npt_com_default_release(t);
   }
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateTexture1D_override(void *self,
                              const D3D11_TEXTURE1D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture1D **ppTexture1D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   /* Sync/async policy: see dev_CreateBuffer_override. */
   ID3D11Texture1D *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device_CreateTexture1D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateTexture1D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc) {
      const struct npt_d3d11_texture_desc d = {
         .width = pDesc->Width,
         .height = 1u,
         .depth = 1u,
         .mip_levels = pDesc->MipLevels,
         .array_size = pDesc->ArraySize,
         .format = pDesc->Format,
         .sample_count = 1u,
         .sample_quality = 0u,
         .usage = pDesc->Usage,
         .bind_flags = pDesc->BindFlags,
         .cpu_access_flags = pDesc->CPUAccessFlags,
         .misc_flags = pDesc->MiscFlags,
         .texture_layout = (D3D11_TEXTURE_LAYOUT)0,
      };
      finish_create_texture(self, &NPT_IID_ID3D11Texture1D, raw,
                            pInitialData, &d, (void **)ppTexture1D);
   } else if (ppTexture1D) {
      *ppTexture1D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateTexture2D_override(void *self,
                              const D3D11_TEXTURE2D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture2D **ppTexture2D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   /* Sync/async policy: see dev_CreateBuffer_override. */
   ID3D11Texture2D *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device_CreateTexture2D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateTexture2D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc) {
      const struct npt_d3d11_texture_desc d = {
         .width = pDesc->Width,
         .height = pDesc->Height,
         .depth = 1u,
         .mip_levels = pDesc->MipLevels,
         .array_size = pDesc->ArraySize,
         .format = pDesc->Format,
         .sample_count = pDesc->SampleDesc.Count,
         .sample_quality = pDesc->SampleDesc.Quality,
         .usage = pDesc->Usage,
         .bind_flags = pDesc->BindFlags,
         .cpu_access_flags = pDesc->CPUAccessFlags,
         .misc_flags = pDesc->MiscFlags,
         .texture_layout = (D3D11_TEXTURE_LAYOUT)0,
      };
      finish_create_texture(self, &NPT_IID_ID3D11Texture2D, raw,
                            pInitialData, &d, (void **)ppTexture2D);
   } else if (ppTexture2D) {
      *ppTexture2D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateTexture3D_override(void *self,
                              const D3D11_TEXTURE3D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture3D **ppTexture3D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   /* Sync/async policy: see dev_CreateBuffer_override. */
   ID3D11Texture3D *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device_CreateTexture3D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateTexture3D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc) {
      const struct npt_d3d11_texture_desc d = {
         .width = pDesc->Width,
         .height = pDesc->Height,
         .depth = pDesc->Depth,
         .mip_levels = pDesc->MipLevels,
         .array_size = 1u,
         .format = pDesc->Format,
         .sample_count = 1u,
         .sample_quality = 0u,
         .usage = pDesc->Usage,
         .bind_flags = pDesc->BindFlags,
         .cpu_access_flags = pDesc->CPUAccessFlags,
         .misc_flags = pDesc->MiscFlags,
         .texture_layout = (D3D11_TEXTURE_LAYOUT)0,
      };
      finish_create_texture(self, &NPT_IID_ID3D11Texture3D, raw,
                            pInitialData, &d, (void **)ppTexture3D);
   } else if (ppTexture3D) {
      *ppTexture3D = NULL;
   }
   return hr;
}

/* DESC1 = DESC + TextureLayout; otherwise identical to the non-1 path. */
static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateTexture2D1_override(void *self,
                               const D3D11_TEXTURE2D_DESC1 *pDesc1,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture2D1 **ppTexture2D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   ID3D11Texture2D1 *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device3_CreateTexture2D1(
         npt_device_method_ring(dev), self_id, pDesc1, NULL, &raw);
   } else {
      npt_async_ID3D11Device3_CreateTexture2D1(
         npt_device_method_ring(dev), self_id, pDesc1, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc1) {
      const struct npt_d3d11_texture_desc d = {
         .width = pDesc1->Width,
         .height = pDesc1->Height,
         .depth = 1u,
         .mip_levels = pDesc1->MipLevels,
         .array_size = pDesc1->ArraySize,
         .format = pDesc1->Format,
         .sample_count = pDesc1->SampleDesc.Count,
         .sample_quality = pDesc1->SampleDesc.Quality,
         .usage = pDesc1->Usage,
         .bind_flags = pDesc1->BindFlags,
         .cpu_access_flags = pDesc1->CPUAccessFlags,
         .misc_flags = pDesc1->MiscFlags,
         .texture_layout = pDesc1->TextureLayout,
      };
      finish_create_texture(self, &NPT_IID_ID3D11Texture2D1, raw,
                            pInitialData, &d, (void **)ppTexture2D);
   } else if (ppTexture2D) {
      *ppTexture2D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateTexture3D1_override(void *self,
                               const D3D11_TEXTURE3D_DESC1 *pDesc1,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture3D1 **ppTexture3D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   ID3D11Texture3D1 *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled) {
      hr = npt_call_ID3D11Device3_CreateTexture3D1(
         npt_device_method_ring(dev), self_id, pDesc1, NULL, &raw);
   } else {
      npt_async_ID3D11Device3_CreateTexture3D1(
         npt_device_method_ring(dev), self_id, pDesc1, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc1) {
      const struct npt_d3d11_texture_desc d = {
         .width = pDesc1->Width,
         .height = pDesc1->Height,
         .depth = pDesc1->Depth,
         .mip_levels = pDesc1->MipLevels,
         .array_size = 1u,
         .format = pDesc1->Format,
         .sample_count = 1u,
         .sample_quality = 0u,
         .usage = pDesc1->Usage,
         .bind_flags = pDesc1->BindFlags,
         .cpu_access_flags = pDesc1->CPUAccessFlags,
         .misc_flags = pDesc1->MiscFlags,
         .texture_layout = pDesc1->TextureLayout,
      };
      finish_create_texture(self, &NPT_IID_ID3D11Texture3D1, raw,
                            pInitialData, &d, (void **)ppTexture3D);
   } else if (ppTexture3D) {
      *ppTexture3D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev4_RegisterDeviceRemovedEvent_override(void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self; (void)hEvent;
   if (pdwCookie)
      *pdwCookie = NPT_DEVICE_REMOVED_COOKIE;
   return NPT_S_OK;
}

static void NPT_STDMETHODCALLTYPE
dev4_UnregisterDeviceRemoved_override(void *self, DWORD dwCookie)
{
   (void)self; (void)dwCookie;
}

/* Tag each deferred context before the app sees it
 * (npt_d3d11_context_is_immediate). */
static HRESULT
dev_tag_deferred(HRESULT hr, void **ppDeferredContext)
{
   if (NPT_SUCCEEDED(hr) && ppDeferredContext && *ppDeferredContext)
      npt_d3d11_context_set_deferred(*ppDeferredContext);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateDeferredContext_override(void *self, UINT ContextFlags,
                                   ID3D11DeviceContext **ppDeferredContext)
{
   return dev_tag_deferred(
      npt_id3d11device_default_CreateDeferredContext(self, ContextFlags,
                                                     ppDeferredContext),
      (void **)ppDeferredContext);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev1_CreateDeferredContext1_override(void *self, UINT ContextFlags,
                                     ID3D11DeviceContext1 **ppDeferredContext)
{
   return dev_tag_deferred(
      npt_id3d11device1_default_CreateDeferredContext1(self, ContextFlags,
                                                       ppDeferredContext),
      (void **)ppDeferredContext);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev2_CreateDeferredContext2_override(void *self, UINT ContextFlags,
                                     ID3D11DeviceContext2 **ppDeferredContext)
{
   return dev_tag_deferred(
      npt_id3d11device2_default_CreateDeferredContext2(self, ContextFlags,
                                                       ppDeferredContext),
      (void **)ppDeferredContext);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateDeferredContext3_override(void *self, UINT ContextFlags,
                                     ID3D11DeviceContext3 **ppDeferredContext)
{
   return dev_tag_deferred(
      npt_id3d11device3_default_CreateDeferredContext3(self, ContextFlags,
                                                       ppDeferredContext),
      (void **)ppDeferredContext);
}

void
npt_overrides_d3d11_device_init(void)
{
   NPT_REGISTER_OVERRIDE(id3d11device, CreateDeferredContext,
                                       dev_CreateDeferredContext_override);
   NPT_REGISTER_OVERRIDE(id3d11device1, CreateDeferredContext1,
                                       dev1_CreateDeferredContext1_override);
   NPT_REGISTER_OVERRIDE(id3d11device2, CreateDeferredContext2,
                                       dev2_CreateDeferredContext2_override);
   NPT_REGISTER_OVERRIDE(id3d11device3, CreateDeferredContext3,
                                       dev3_CreateDeferredContext3_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateBuffer,
                                       dev_CreateBuffer_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateTexture1D,
                                       dev_CreateTexture1D_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateTexture2D,
                                       dev_CreateTexture2D_override);
   NPT_REGISTER_OVERRIDE(id3d11device, CreateTexture3D,
                                       dev_CreateTexture3D_override);
   NPT_REGISTER_OVERRIDE(id3d11device3, CreateTexture2D1,
                                       dev3_CreateTexture2D1_override);
   NPT_REGISTER_OVERRIDE(id3d11device3, CreateTexture3D1,
                                       dev3_CreateTexture3D1_override);
   NPT_REGISTER_OVERRIDE(id3d11device4, RegisterDeviceRemovedEvent,
                                       dev4_RegisterDeviceRemovedEvent_override);
   NPT_REGISTER_OVERRIDE(id3d11device4, UnregisterDeviceRemoved,
                                       dev4_UnregisterDeviceRemoved_override);

   npt_com_register_family(device_tiers, 0, NULL);
}
