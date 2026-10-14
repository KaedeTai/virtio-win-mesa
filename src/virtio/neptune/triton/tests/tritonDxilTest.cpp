/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Verification harness for tritonDxil.c.
 *
 * Every vertex shader is checked against two independent references for
 * the same register -> (semantic name, semantic index) mapping:
 *  - DXC's own reflection (IDxcContainerReflection -> ID3D12ShaderReflection)
 *    of a container holding only the DXIL part, which reads the same
 *    dx.entryPoints metadata through Microsoft's implementation;
 *  - the container's ISG1 chunk, where the producer emitted one.
 * Shaders come from every path the runtime can hand Triton: DXC SM6
 * compiles (several shader models, 16-bit types, embedded debug info,
 * -O0), and fxc DXBC converted by dxilconv both as a full container
 * (Convert) and as the driver-side module (ConvertInDriver) the runtime
 * produces for a DXBC app.  Truncated programs must be rejected cleanly.
 *
 * Needs dxcompiler.dll (and dxil.dll) beside the exe or on PATH, plus the
 * system's d3dcompiler_47.dll and dxilconv.dll.
 */

#include <windows.h>
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <dxcapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../tritonDxil.h"
#include "../tritonDxbc.h"
uint32_t npt_host_workaround_flags(void) { return 0; }
}

static int g_checks, g_fails;
#define CHECK(cond, ...) do {                                   \
        ++g_checks;                                             \
        if (!(cond)) {                                          \
            ++g_fails;                                          \
            printf("FAIL %s:%d: ", g_case, __LINE__);           \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static const char *g_case = "";

/* ---------- references ---------- */

static DxcCreateInstanceProc s_dxcCreate;
static DxcCreateInstanceProc s_dxilconvCreate;
static pD3DCompile s_d3dCompile;
static IDxcUtils *s_utils;
static IDxcCompiler3 *s_compiler;

struct __declspec(uuid("5F956ED5-78D1-4B15-8247-F7187614A041")) IDxbcConverter
    : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Convert(
        LPCVOID pDxbc, UINT32 DxbcSize, LPCWSTR pExtraOptions,
        LPVOID *ppDxil, UINT32 *pDxilSize, LPWSTR *ppDiag) = 0;
    virtual HRESULT STDMETHODCALLTYPE ConvertInDriver(
        const UINT32 *pBytecode, LPCVOID pInputSignature,
        UINT32 NumInputSignatureElements, LPCVOID pOutputSignature,
        UINT32 NumOutputSignatureElements, LPCVOID pPatchConstantSignature,
        UINT32 NumPatchConstantSignatureElements, LPCWSTR pExtraOptions,
        IDxcBlob **ppDxilModule, LPWSTR *ppDiag) = 0;
};
static const CLSID CLSID_DxbcConverter = {
    0x4900391e, 0xb752, 0x4edd, {0xa8, 0x85, 0x6f, 0xb7, 0x6e, 0x25, 0xad, 0xdb}};

typedef std::vector<unsigned char> Bytes;

struct RefParam { std::string name; UINT index, reg; };

/* DXC reflection of a container's DXIL part: allocated input rows only. */
static bool reflectInputs(const Bytes &container, std::vector<RefParam> &out)
{
    IDxcBlobEncoding *blob = nullptr;
    IDxcContainerReflection *refl = nullptr;
    ID3D12ShaderReflection *sr = nullptr;
    bool ok = false;
    UINT32 part = 0;
    D3D12_SHADER_DESC sd;
    if (FAILED(s_utils->CreateBlob(container.data(), (UINT32)container.size(),
                                   DXC_CP_ACP, &blob)) ||
        FAILED(s_dxcCreate(CLSID_DxcContainerReflection,
                           IID_PPV_ARGS(&refl))) ||
        FAILED(refl->Load(blob)) ||
        FAILED(refl->FindFirstPartKind(DXC_PART_DXIL, &part)) ||
        FAILED(refl->GetPartReflection(part, IID_PPV_ARGS(&sr))) ||
        FAILED(sr->GetDesc(&sd)))
        goto done;
    for (UINT i = 0; i < sd.InputParameters; i++) {
        D3D12_SIGNATURE_PARAMETER_DESC pd;
        if (FAILED(sr->GetInputParameterDesc(i, &pd)))
            goto done;
        if (pd.Register == ~0u)
            continue;
        out.push_back({pd.SemanticName, pd.SemanticIndex, pd.Register});
    }
    ok = true;
done:
    if (sr) sr->Release();
    if (refl) refl->Release();
    if (blob) blob->Release();
    return ok;
}

/* A container holding exactly one DXIL part, as Triton builds for the bare
 * program the runtime hands it. */
static Bytes wrapProgram(const void *program, size_t cb)
{
    const size_t pad = (cb + 3) & ~(size_t)3;
    Bytes c(36 + 8 + pad, 0);
    auto put = [&](size_t off, UINT32 v) { memcpy(&c[off], &v, 4); };
    put(0, 0x43425844u);
    c[20] = 1;
    put(24, (UINT32)c.size());
    put(28, 1);
    put(32, 36);
    put(36, 0x4C495844u);
    put(40, (UINT32)cb);
    memcpy(&c[44], program, cb);
    return c;
}

/* ---------- the check ---------- */

static void checkProgram(const void *program, size_t cbProgram,
                         const Bytes *withIsg1)
{
    TritonDxilInputSig *sig = tritonDxilReadInputSig(program, cbProgram);
    CHECK(sig != nullptr, "reader rejected a valid program (%zu bytes)",
          cbProgram);
    if (!sig)
        return;

    std::vector<RefParam> ref;
    CHECK(reflectInputs(wrapProgram(program, cbProgram), ref),
          "DXC reflection failed");

    uint32_t rows = 0;
    for (uint32_t i = 0; i < sig->Count; i++) {
        const TritonDxilInput *e = &sig->Elems[i];
        if (e->StartRow < 0)
            continue;
        rows += e->Rows;
        CHECK(e->StartCol == 0, "%s starts at column %u", e->Name,
              e->StartCol);
    }
    CHECK(rows == ref.size(), "reader has %u allocated rows, DXC %zu", rows,
          ref.size());
    for (const RefParam &p : ref) {
        uint32_t idx = ~0u;
        const char *name = tritonDxilInputSemantic(sig, p.reg, &idx);
        CHECK(name && !_stricmp(p.name.c_str(), name) && idx == p.index,
              "v%u: DXC %s%u, reader %s%u", p.reg, p.name.c_str(), p.index,
              name ? name : "(none)", idx);
    }

    if (withIsg1) {
        TritonDxbcSig isg[64];
        const UINT n = tritonDxbcParseSig(withIsg1->data(), withIsg1->size(),
                                          TRUE, isg, 64);
        UINT allocated = 0;
        for (UINT i = 0; i < n; i++) {
            if (isg[i].Register >= 32)
                continue;
            allocated++;
            /* TritonDxbcSig.Name truncates; the lookup points into the
             * container. */
            UINT isgIdx = ~0u;
            const char *isgName = tritonDxbcInputSemantic(
                withIsg1->data(), withIsg1->size(), isg[i].Register, &isgIdx);
            uint32_t idx = ~0u;
            const char *name =
                tritonDxilInputSemantic(sig, isg[i].Register, &idx);
            CHECK(isgName && name && !_stricmp(name, isgName) &&
                  idx == isgIdx,
                  "v%u: ISG1 %s%u, reader %s%u", isg[i].Register,
                  isgName ? isgName : "(none)", isgIdx,
                  name ? name : "(none)", idx);
        }
        CHECK(n > 0 || ref.empty(), "container has no ISG1");
        CHECK(allocated == rows, "ISG1 has %u allocated rows, reader %u",
              allocated, rows);
    }

    /* Every prefix that cuts into the bitcode is malformed and must be
     * rejected, not misread.  (Producers may pad the part past it.) */
    UINT32 hdr[6];
    memcpy(hdr, program, sizeof(hdr));
    const size_t bcEnd = 8 + (size_t)hdr[4] + hdr[5];
    for (size_t cut = 0; cut < bcEnd; cut += (cut < 64 ? 1 : 7)) {
        TritonDxilInputSig *t = tritonDxilReadInputSig(program, cut);
        CHECK(t == nullptr, "accepted a %zu-byte prefix of %zu", cut, bcEnd);
        tritonDxilFreeInputSig(t);
    }
    tritonDxilFreeInputSig(sig);
}

static void checkContainer(const Bytes &container)
{
    size_t cb = 0;
    const void *prog = tritonDxilFindProgram(container.data(),
                                             container.size(), &cb);
    CHECK(prog != nullptr, "no DXIL part");
    if (prog)
        checkProgram(prog, cb, &container);
}

/* ---------- producers ---------- */

static bool compileDxc(const char *src, const wchar_t *target,
                       std::vector<const wchar_t *> extra, Bytes &out)
{
    std::vector<const wchar_t *> args = {L"-E", L"main", L"-T", target};
    args.insert(args.end(), extra.begin(), extra.end());
    DxcBuffer b = {src, strlen(src), DXC_CP_UTF8};
    IDxcResult *res = nullptr;
    IDxcBlob *obj = nullptr;
    HRESULT status = E_FAIL;
    bool ok = SUCCEEDED(s_compiler->Compile(&b, args.data(),
                                            (UINT32)args.size(), nullptr,
                                            IID_PPV_ARGS(&res))) &&
              SUCCEEDED(res->GetStatus(&status)) && SUCCEEDED(status) &&
              SUCCEEDED(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj),
                                       nullptr)) && obj;
    if (!ok && res) {
        IDxcBlobUtf8 *err = nullptr;
        if (SUCCEEDED(res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err),
                                     nullptr)) && err) {
            printf("dxc: %s\n", err->GetStringPointer());
            err->Release();
        }
    }
    if (ok) {
        const unsigned char *p = (const unsigned char *)obj->GetBufferPointer();
        out.assign(p, p + obj->GetBufferSize());
    }
    if (obj) obj->Release();
    if (res) res->Release();
    return ok;
}

static bool compileFxc(const char *src, const char *target, Bytes &out)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = s_d3dCompile(src, strlen(src), nullptr, nullptr, nullptr,
                              "main", target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        printf("fxc: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    const unsigned char *p = (const unsigned char *)code->GetBufferPointer();
    out.assign(p, p + code->GetBufferSize());
    code->Release();
    if (err) err->Release();
    return true;
}

/* D3D12DDIARG_SIGNATURE_ENTRY_0012, without the DDI headers. */
struct DdiSigEntry {
    UINT SystemValue;            /* D3D10_SB_NAME */
    UINT Register;
    BYTE Mask;
    BYTE Stream;
    UINT RegisterComponentType;  /* D3D10_SB_REGISTER_COMPONENT_TYPE */
    UINT MinPrecision;
};

/* The DDI signature the runtime derives from a DXBC container's input or
 * output chunk -- ISGN/OSGN, or ISG1/OSG1 when the shader uses minimum
 * precision: one register-only entry per element (D3D_NAME and
 * D3D10_SB_NAME agree for every vertex-shader system value). */
static std::vector<DdiSigEntry> ddiSignature(const Bytes &dxbc, bool input)
{
    std::vector<DdiSigEntry> out;
    UINT32 nParts;
    memcpy(&nParts, &dxbc[28], 4);
    for (UINT32 i = 0; i < nParts; i++) {
        UINT32 off, fourcc;
        memcpy(&off, &dxbc[32 + 4 * i], 4);
        memcpy(&fourcc, &dxbc[off], 4);
        const bool v1 = fourcc == (input ? 0x31475349u /* ISG1 */
                                         : 0x3147534Fu /* OSG1 */);
        if (!v1 && fourcc != (input ? 0x4E475349u /* ISGN */
                                    : 0x4E47534Fu /* OSGN */))
            continue;
        const unsigned char *chunk = &dxbc[off + 8];
        UINT32 count;
        memcpy(&count, chunk, 4);
        for (UINT32 e = 0; e < count; e++) {
            /* ISG1 entries lead with Stream and end with MinPrecision. */
            const unsigned char *el = chunk + 8 + (v1 ? 32 : 24) * e + (v1 ? 4 : 0);
            DdiSigEntry d = {};
            memcpy(&d.SystemValue, el + 8, 4);
            memcpy(&d.RegisterComponentType, el + 12, 4);
            memcpy(&d.Register, el + 16, 4);
            d.Mask = el[20];
            if (v1)
                memcpy(&d.MinPrecision, el + 24, 4);
            out.push_back(d);
        }
    }
    return out;
}

static void checkDxbc(const char *src, const char *target)
{
    Bytes dxbc;
    CHECK(compileFxc(src, target, dxbc), "fxc %s failed", target);
    if (dxbc.empty())
        return;

    IDxbcConverter *conv = nullptr;
    CHECK(SUCCEEDED(s_dxilconvCreate(CLSID_DxbcConverter,
                                     IID_PPV_ARGS(&conv))),
          "no dxilconv");
    if (!conv)
        return;

    /* Convert: a full container with signatures, as an app could ship. */
    void *dxil = nullptr;
    UINT32 cbDxil = 0;
    LPWSTR diag = nullptr;
    HRESULT hr = conv->Convert(dxbc.data(), (UINT32)dxbc.size(), nullptr,
                               &dxil, &cbDxil, &diag);
    CHECK(SUCCEEDED(hr) && dxil, "Convert failed 0x%08lx", hr);
    if (SUCCEEDED(hr) && dxil) {
        const unsigned char *p = (const unsigned char *)dxil;
        checkContainer(Bytes(p, p + cbDxil));
        CoTaskMemFree(dxil);
    }
    if (diag)
        CoTaskMemFree(diag);

    /* ConvertInDriver: what the runtime hands a DXIL-consuming driver for
     * a DXBC app -- bare bitcode built from the token stream and the DDI's
     * register-only signature, wrapped in a program header. */
    const UINT32 *tokens = nullptr;
    UINT32 nParts;
    memcpy(&nParts, &dxbc[28], 4);
    for (UINT32 i = 0; i < nParts; i++) {
        UINT32 off, fourcc;
        memcpy(&off, &dxbc[32 + 4 * i], 4);
        memcpy(&fourcc, &dxbc[off], 4);
        if (fourcc == 0x52444853u /* SHDR */ || fourcc == 0x58454853u /* SHEX */) {
            tokens = (const UINT32 *)&dxbc[off + 8];
        }
    }
    const std::vector<DdiSigEntry> in = ddiSignature(dxbc, true);
    const std::vector<DdiSigEntry> out = ddiSignature(dxbc, false);
    IDxcBlob *module = nullptr;
    diag = nullptr;
    hr = conv->ConvertInDriver(tokens, in.data(), (UINT32)in.size(),
                               out.data(), (UINT32)out.size(), nullptr, 0,
                               nullptr, &module, &diag);
    CHECK(SUCCEEDED(hr) && module, "ConvertInDriver failed 0x%08lx", hr);
    if (SUCCEEDED(hr) && module) {
        const size_t cbBc = module->GetBufferSize();
        Bytes prog(24 + ((cbBc + 3) & ~(size_t)3), 0);
        const UINT32 hdr[6] = {
            0x00010060u /* vs_6_0 */, (UINT32)(prog.size() / 4),
            0x4C495844u, 0x100, 16, (UINT32)cbBc };
        memcpy(prog.data(), hdr, sizeof(hdr));
        memcpy(&prog[24], module->GetBufferPointer(), cbBc);
        checkProgram(prog.data(), prog.size(), nullptr);
        module->Release();
    }
    if (diag)
        CoTaskMemFree(diag);
    conv->Release();
}

/* ---------- corpus ---------- */

static const char HLSL_BASIC[] =
    "struct I { float3 p : POSITION; float3 n : NORMAL; float2 t : TEXCOORD0; };\n"
    "float4 main(I i) : SV_Position { return float4(i.p + i.n, i.t.x); }\n";

static const char HLSL_INDICES[] =
    "struct I { float4 a : TEXCOORD7; float4 b : COLOR1; float4 c : TEXCOORD3;\n"
    "           float4 d : COLOR0; };\n"
    "float4 main(I i) : SV_Position { return i.a + i.b + i.c + i.d; }\n";

static const char HLSL_MATRIX[] =
    "struct I { float4x4 w : WORLD; float3 p : POSITION;\n"
    "           row_major float3x4 b : BONE5; float2x2 m : MAT; };\n"
    "float4 main(I i) : SV_Position {\n"
    "  return mul(float4(i.p, 1), i.w) + float4(mul(i.b, float4(i.p, 1)), 0)\n"
    "         + float4(i.m[0], i.m[1]); }\n";

static const char HLSL_ARRAY[] =
    "struct I { float4 a[3] : ARR2; uint2 x[2] : IDX; float4 p : POSITION; };\n"
    "float4 main(I i) : SV_Position {\n"
    "  return i.a[0] + i.a[1] + i.a[2] + i.p + float4(i.x[0] + i.x[1], 0, 0); }\n";

static const char HLSL_SV_MIXED[] =
    "float4 main(float3 p : POSITION, uint vid : SV_VertexID,\n"
    "            float2 t : TEXCOORD0, uint iid : SV_InstanceID,\n"
    "            float4 c : COLOR) : SV_Position {\n"
    "  return float4(p, vid + iid) + float4(t, 0, 0) + c; }\n";

static const char HLSL_UNUSED[] =
    "float4 main(float3 p : POSITION, float4 unused : TEXCOORD5,\n"
    "            float2 t : TEXCOORD0) : SV_Position { return float4(p, t.x); }\n";

static const char HLSL_INTS[] =
    "float4 main(int4 a : BLENDINDICES, uint b : FLAGS, float c : WEIGHT,\n"
    "            min16float4 d : COLOR, min16uint e : MINU) : SV_Position {\n"
    "  return float4(a) + b + c + d + e; }\n";

static const char HLSL_HALF[] =
    "float4 main(half4 a : COLOR, uint16_t2 b : SHORTS, float3 p : POSITION)\n"
    "  : SV_Position { return float4(p, 1) + a + float4(b, 0, 0); }\n";

static const char HLSL_NAMES[] =
    "float4 main(float4 a : NPTA0, float4 b : position, float4 c :\n"
    "  SEMANTIC_NAMES_MAY_BE_SIXTY_FOUR_CHARACTERS_LONG_THAT_IS_THE_MAX)\n"
    "  : SV_Position { return a + b + c; }\n";

/* fxc aborts on a 64-character semantic; the rest of HLSL_NAMES. */
static const char HLSL_NAMES_FXC[] =
    "float4 main(float4 a : NPTA0, float4 b : position) : SV_Position\n"
    "  { return a + b; }\n";

static const char HLSL_VID_ONLY[] =
    "float4 main(uint vid : SV_VertexID) : SV_Position { return vid; }\n";

static const char HLSL_NO_INPUTS[] =
    "float4 main() : SV_Position { return 1; }\n";

static std::string hlslMany(int n)
{
    std::string s = "struct I {";
    for (int i = 0; i < n; i++)
        s += " float4 a" + std::to_string(i) + " : ATTR" + std::to_string(i * 3) + ";";
    s += " };\nfloat4 main(I i) : SV_Position { float4 r = 0;";
    for (int i = 0; i < n; i++)
        s += " r += i.a" + std::to_string(i) + ";";
    return s + " return r; }\n";
}

static void runDxc(const char *name, const char *src, const wchar_t *target,
                   std::vector<const wchar_t *> extra = {})
{
    char label[128];
    snprintf(label, sizeof(label), "%s/%ls", name, target);
    g_case = label;
    Bytes c;
    CHECK(compileDxc(src, target, extra, c), "dxc failed");
    if (!c.empty())
        checkContainer(c);
}

int main(void)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    HMODULE dxc = LoadLibraryW(L"dxcompiler.dll");
    HMODULE conv = LoadLibraryW(L"dxilconv.dll");
    HMODULE d3dc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dxc || !conv || !d3dc) {
        printf("missing dxcompiler.dll / dxilconv.dll / d3dcompiler_47.dll\n");
        return 2;
    }
    s_dxcCreate = (DxcCreateInstanceProc)GetProcAddress(dxc, "DxcCreateInstance");
    s_dxilconvCreate = (DxcCreateInstanceProc)GetProcAddress(conv, "DxcCreateInstance");
    s_d3dCompile = (pD3DCompile)GetProcAddress(d3dc, "D3DCompile");
    if (!s_dxcCreate || !s_dxilconvCreate || !s_d3dCompile ||
        FAILED(s_dxcCreate(CLSID_DxcUtils, IID_PPV_ARGS(&s_utils))) ||
        FAILED(s_dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&s_compiler)))) {
        printf("cannot create the DXC compiler\n");
        return 2;
    }

    const std::string many16 = hlslMany(16), many32 = hlslMany(32);
    struct { const char *name; const char *src; } cases[] = {
        {"basic", HLSL_BASIC},       {"indices", HLSL_INDICES},
        {"matrix", HLSL_MATRIX},     {"array", HLSL_ARRAY},
        {"sv-mixed", HLSL_SV_MIXED}, {"unused", HLSL_UNUSED},
        {"ints", HLSL_INTS},         {"names", HLSL_NAMES},
        {"vid-only", HLSL_VID_ONLY}, {"no-inputs", HLSL_NO_INPUTS},
        {"many16", many16.c_str()},  {"many32", many32.c_str()},
    };
    for (const auto &k : cases) {
        for (const wchar_t *t : {L"vs_6_0", L"vs_6_6", L"vs_6_8"})
            runDxc(k.name, k.src, t);
        runDxc(k.name, k.src, L"vs_6_0", {L"-Od"});
        runDxc(k.name, k.src, L"vs_6_0", {L"-Zi", L"-Qembed_debug"});
        runDxc(k.name, k.src, L"vs_6_2", {L"-enable-16bit-types"});
        runDxc(k.name, k.src, L"vs_6_0", {L"-Zpr"});
    }
    runDxc("half", HLSL_HALF, L"vs_6_2", {L"-enable-16bit-types"});

    /* DXBC: the same shaders through fxc, except that vs_4_0 takes at
     * most 16 inputs. */
    for (const auto &k : cases) {
        const char *src = k.src == HLSL_NAMES ? HLSL_NAMES_FXC : k.src;
        for (const char *t : {"vs_4_0", "vs_5_0"}) {
            if (k.src == many32.c_str() && !strcmp(t, "vs_4_0"))
                continue;
            char label[128];
            snprintf(label, sizeof(label), "dxbc-%s/%s", k.name, t);
            g_case = label;
            checkDxbc(src, t);
        }
    }

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
