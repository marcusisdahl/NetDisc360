// SPDX-License-Identifier: MIT
// Dear ImGui renderer for the Xbox 360's Direct3D. The stock imgui_impl_dx9
// relies on FVF vertices and fixed-function texture stages, which the 360
// does not have, so this draws with a small shader pair instead.
#include <xtl.h>
#include <d3dx9.h>
#include "imgui_impl_xbox360.h"
#include "../third_party/imgui/imgui.h"

static IDirect3DDevice9* Device=NULL;
static IDirect3DVertexShader9* VertexShader=NULL;
static IDirect3DPixelShader9* PixelShader=NULL;
static IDirect3DVertexDeclaration9* Declaration=NULL;
static IDirect3DTexture9* FontTexture=NULL;
// Indices of one draw command, rebased so only the vertices it uses are copied.
static ImVector<ImDrawIdx> Indices;

static bool Compile(const char* source,const char* profile,ID3DXBuffer** code) {
    ID3DXBuffer* errors=NULL;
    HRESULT hr=D3DXCompileShader(source,UINT(strlen(source)),NULL,NULL,"main",profile,0,code,&errors,NULL);
    if(errors) errors->Release();
    return SUCCEEDED(hr);
}
bool ImGuiXbox360Init(IDirect3DDevice9* device) {
    Device=device;
    // c0 maps pixels to clip space: x*c0.x+c0.z, y*c0.y+c0.w.
    const char* vs="float4 Scale:register(c0); struct V { float2 p:POSITION0; float2 t:TEXCOORD0; float4 c:COLOR0; };"
        "struct O { float4 p:POSITION0; float2 t:TEXCOORD0; float4 c:COLOR0; };"
        "O main(V v) { O o; o.p=float4(v.p*Scale.xy+Scale.zw,0,1); o.t=v.t; o.c=v.c; return o; }";
    // The font atlas is white with coverage in alpha; untextured shapes use
    // its one opaque white texel.
    const char* ps="sampler Atlas:register(s0); float4 main(float2 t:TEXCOORD0,float4 c:COLOR0):COLOR0 { return c*tex2D(Atlas,t); }";
    ID3DXBuffer* code=NULL;
    if(!Compile(vs,"vs_2_0",&code)) return false;
    HRESULT hr=Device->CreateVertexShader(reinterpret_cast<DWORD*>(code->GetBufferPointer()),&VertexShader);
    code->Release(); if(FAILED(hr)) return false;
    if(!Compile(ps,"ps_2_0",&code)) return false;
    hr=Device->CreatePixelShader(reinterpret_cast<DWORD*>(code->GetBufferPointer()),&PixelShader);
    code->Release(); if(FAILED(hr)) return false;
    const D3DVERTEXELEMENT9 elements[]={
        {0,IM_OFFSETOF(ImDrawVert,pos),D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {0,IM_OFFSETOF(ImDrawVert,uv),D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
        {0,IM_OFFSETOF(ImDrawVert,col),D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},D3DDECL_END()};
    if(FAILED(Device->CreateVertexDeclaration(elements,&Declaration))) return false;

    // Font atlas as a linear A8R8G8B8 texture. Each texel is written as a
    // whole DWORD, so the console's byte order needs no special handling.
    ImGuiIO& io=ImGui::GetIO();
    unsigned char* alpha=NULL; int width=0,height=0;
    io.Fonts->GetTexDataAsAlpha8(&alpha,&width,&height);
    if(FAILED(Device->CreateTexture(width,height,1,0,D3DFMT_LIN_A8R8G8B8,0,&FontTexture,NULL))) return false;
    D3DLOCKED_RECT locked;
    if(FAILED(FontTexture->LockRect(0,&locked,NULL,0))) return false;
    for(int y=0;y<height;++y) {
        DWORD* row=reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.pBits)+y*locked.Pitch);
        for(int x=0;x<width;++x) row[x]=(DWORD(alpha[y*width+x])<<24)|0x00FFFFFF;
    }
    FontTexture->UnlockRect(0);
    io.Fonts->SetTexID(FontTexture);
    io.Fonts->ClearTexData();
    return true;
}
void ImGuiXbox360Render(ImDrawData* data) {
    const float width=data->DisplaySize.x,height=data->DisplaySize.y;
    if(width<=0 || height<=0) return;
    // D3D9 pixel centres sit on whole coordinates, so shift by half a pixel
    // to keep the font atlas texels aligned with screen pixels.
    const float scale[4]={2/width,-2/height,-1-1/width,1+1/height};
    Device->SetRenderState(D3DRS_ZENABLE,FALSE); Device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    Device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE); Device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
    Device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA); Device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    Device->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE);
    Device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR); Device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
    Device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP); Device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    Device->SetVertexDeclaration(Declaration); Device->SetVertexShader(VertexShader); Device->SetPixelShader(PixelShader);
    Device->SetVertexShaderConstantF(0,scale,1);
    for(int n=0;n<data->CmdListsCount;++n) {
        const ImDrawList* list=data->CmdLists[n];
        for(int c=0;c<list->CmdBuffer.Size;++c) {
            const ImDrawCmd& cmd=list->CmdBuffer[c];
            if(cmd.UserCallback || !cmd.ElemCount) continue;
            ImVec2 a(cmd.ClipRect.x-data->DisplayPos.x,cmd.ClipRect.y-data->DisplayPos.y);
            ImVec2 b(cmd.ClipRect.z-data->DisplayPos.x,cmd.ClipRect.w-data->DisplayPos.y);
            if(a.x<0) a.x=0; if(a.y<0) a.y=0; if(b.x>width) b.x=width; if(b.y>height) b.y=height;
            if(b.x<=a.x || b.y<=a.y) continue;
            const RECT clip={LONG(a.x),LONG(a.y),LONG(b.x),LONG(b.y)};
            Device->SetScissorRect(&clip);
            Device->SetTexture(0,static_cast<IDirect3DTexture9*>(cmd.GetTexID()));
            const ImDrawIdx* source=list->IdxBuffer.Data+cmd.IdxOffset;
            unsigned int low=0xFFFF,high=0;
            for(unsigned int i=0;i<cmd.ElemCount;++i) { if(source[i]<low) low=source[i]; if(source[i]>high) high=source[i]; }
            Indices.resize(int(cmd.ElemCount));
            for(unsigned int i=0;i<cmd.ElemCount;++i) Indices[int(i)]=ImDrawIdx(source[i]-low);
            Device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,high-low+1,cmd.ElemCount/3,Indices.Data,D3DFMT_INDEX16,
                list->VtxBuffer.Data+cmd.VtxOffset+low,sizeof(ImDrawVert));
        }
    }
    Device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
    Device->SetTexture(0,NULL);
}
