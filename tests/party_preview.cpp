// SPDX-License-Identifier: MIT
// Renders NetDiscParty.xex's screen (party/ui.cpp) on a PC, to review it
// without a console: the party as the server sends it (a text file, see
// PartyText in server/main.cpp) drawn into 1280x720 BMPs, one for each
// moment of the spin asked for. The rasterizer is tests/ui_preview.cpp's.
//   party-preview <out-prefix> <party.txt> <from ms> [<to ms> <every ms>] [lost|looking]
// writes <out-prefix>-0000.bmp and on, the spin that far in.
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../party/ui.h"
#include "../third_party/imgui/imgui.h"
static unsigned int pixels[1280*720];
static const unsigned char* Texture; static int TextureWidth,TextureHeight;

static float Sample(float u,float v) {
    float x=u*TextureWidth-0.5f,y=v*TextureHeight-0.5f;
    int x0=int(x<0 ? x-1 : x),y0=int(y<0 ? y-1 : y); float fx=x-x0,fy=y-y0,sum=0;
    for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx) {
        int tx=x0+dx,ty=y0+dy; if(tx<0) tx=0; if(ty<0) ty=0;
        if(tx>=TextureWidth) tx=TextureWidth-1; if(ty>=TextureHeight) ty=TextureHeight-1;
        sum+=Texture[ty*TextureWidth+tx]/255.0f*(dx ? fx : 1-fx)*(dy ? fy : 1-fy);
    }
    return sum;
}
static float Channel(ImU32 c,int shift) { return float((c>>shift)&255); }
// Same pipeline as the Xbox renderer: vertex colour times texture alpha,
// blended over the target, clipped to the command's rectangle.
static void Triangle(const ImDrawVert* v[3],const ImVec4& clip) {
    float minX=v[0]->pos.x,maxX=minX,minY=v[0]->pos.y,maxY=minY;
    for(int i=1;i<3;++i) {
        if(v[i]->pos.x<minX) minX=v[i]->pos.x; if(v[i]->pos.x>maxX) maxX=v[i]->pos.x;
        if(v[i]->pos.y<minY) minY=v[i]->pos.y; if(v[i]->pos.y>maxY) maxY=v[i]->pos.y;
    }
    if(minX<clip.x) minX=clip.x; if(minY<clip.y) minY=clip.y; if(maxX>clip.z) maxX=clip.z; if(maxY>clip.w) maxY=clip.w;
    float area=(v[1]->pos.x-v[0]->pos.x)*(v[2]->pos.y-v[0]->pos.y)-(v[1]->pos.y-v[0]->pos.y)*(v[2]->pos.x-v[0]->pos.x);
    if(area==0) return;
    for(int y=int(minY);y<int(maxY+1) && y<720;++y) for(int x=int(minX);x<int(maxX+1) && x<1280;++x) {
        if(x<0 || y<0 || x<clip.x || y<clip.y || x>=clip.z || y>=clip.w) continue;
        float px=x+0.5f,py=y+0.5f,w[3];
        for(int i=0;i<3;++i) {
            const ImDrawVert* a=v[(i+1)%3]; const ImDrawVert* b=v[(i+2)%3];
            w[i]=((b->pos.x-a->pos.x)*(py-a->pos.y)-(b->pos.y-a->pos.y)*(px-a->pos.x))/area;
        }
        if(w[0]<0 || w[1]<0 || w[2]<0) continue;
        float u=0,t=0,c[4]={0,0,0,0}; static const int shifts[4]={IM_COL32_R_SHIFT,IM_COL32_G_SHIFT,IM_COL32_B_SHIFT,IM_COL32_A_SHIFT};
        for(int i=0;i<3;++i) { u+=w[i]*v[i]->uv.x; t+=w[i]*v[i]->uv.y; for(int k=0;k<4;++k) c[k]+=w[i]*Channel(v[i]->col,shifts[k]); }
        float alpha=c[3]/255.0f*Sample(u,t);
        unsigned int& p=pixels[y*1280+x]; float d[3]={float((p>>16)&255),float((p>>8)&255),float(p&255)};
        unsigned int out=0xFF000000;
        for(int k=0;k<3;++k) out|=unsigned(c[k]*alpha+d[k]*(1-alpha)+0.5f)<<(16-8*k);
        p=out;
    }
}
static void Rasterize(const ImDrawData* data) {
    for(int n=0;n<data->CmdListsCount;++n) {
        const ImDrawList* list=data->CmdLists[n];
        for(int c=0;c<list->CmdBuffer.Size;++c) {
            const ImDrawCmd& cmd=list->CmdBuffer[c];
            for(unsigned int i=0;i<cmd.ElemCount;i+=3) {
                const ImDrawVert* v[3];
                for(int k=0;k<3;++k) v[k]=&list->VtxBuffer[cmd.VtxOffset+list->IdxBuffer[cmd.IdxOffset+i+k]];
                Triangle(v,cmd.ClipRect);
            }
        }
    }
}
static void Put16(unsigned char* p,unsigned int v) { p[0]=v&255; p[1]=(v>>8)&255; }
static void Put32(unsigned char* p,unsigned int v) { Put16(p,v); Put16(p+2,v>>16); }
static bool Write(const char* path) {
    unsigned char header[54]={'B','M'};
    Put32(header+2,54+sizeof(pixels)); Put32(header+10,54); Put32(header+14,40); Put32(header+18,1280); Put32(header+22,unsigned(-720));
    Put16(header+26,1); Put16(header+28,32); Put32(header+34,sizeof(pixels));
    FILE* output=fopen(path,"wb"); if(!output) return false;
    fwrite(header,sizeof(header),1,output);
    for(int i=0;i<1280*720;++i) { unsigned char p[4]; Put32(p,pixels[i]); fwrite(p,4,1,output); }
    fclose(output);
    return true;
}
int main(int argc, char** argv) {
    if(argc<4) { fprintf(stderr,"party-preview <out-prefix> <party.txt> <from ms> [<to ms> <every ms>] [lost|looking]\n"); return 2; }
    static char text[1<<16];
    FILE* in=fopen(argv[2],"rb"); if(!in) { perror(argv[2]); return 1; }
    unsigned length=unsigned(fread(text,1,sizeof(text),in)); fclose(in);
    party::InitUi();
    unsigned char* alpha=NULL; ImGui::GetIO().Fonts->GetTexDataAsAlpha8(&alpha,&TextureWidth,&TextureHeight); Texture=alpha;
    static party::Screen s;
    party::Parse(text,length,s.state);
    strcpy(s.server,"192.168.1.40"); s.link=party::LinkConnected;
    const char* mode=argc>6 ? argv[6] : "";
    if(!strcmp(mode,"lost")) { s.link=party::LinkLost; s.state.valid=false; }
    if(!strcmp(mode,"looking")) { s.link=party::LinkLooking; s.state.valid=false; }
    double from=atof(argv[3]), to=argc>4 ? atof(argv[4]) : from, every=argc>5 ? atof(argv[5]) : 1;
    if(every<=0) every=1;
    int frame=0;
    for(double ms=from;ms<=to;ms+=every,++frame) {
        s.spinMs=ms; s.time=ms/1000;
        ImGui::GetIO().DeltaTime=1.0f/60;
        party::DrawUi(s);
        for(int i=0;i<1280*720;++i) pixels[i]=0xFF0F1725;
        Rasterize(ImGui::GetDrawData());
        char path[512]; snprintf(path,sizeof(path),"%s-%04d.bmp",argv[1],frame);
        if(!Write(path)) { perror(path); return 1; }
    }
    ImGui::DestroyContext();
    return 0;
}
