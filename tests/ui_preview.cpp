// SPDX-License-Identifier: MIT
// Renders the launcher's loading screen (launcher/ui.cpp) on a PC, to review
// it without a console: Dear ImGui's triangles drawn in software into a
// 1280x720 BMP. The rasterizer is Xbox360-FastFTP's tests/ui_preview.cpp.
//   ui-preview out.bmp [server|network|mount|disc|start|no-server|not-listed|no-disc|wrong-disc|no-plugin|install]
//   ui-preview out.bmp intro <seconds>   the logo intro, that far in
//   ui-preview out.bmp run <seconds>     a whole start, intro to game, that far in (the README's GIF)
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../launcher/ui.h"
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
static void Error(nia::LoadScreen& s, int step, const char* title, const char* a, const char* b, const char* c, bool retry) {
    s.step=step; strcpy(s.errorTitle,title); strcpy(s.errorLines[0],a); strcpy(s.errorLines[1],b); strcpy(s.errorLines[2],c);
    s.canRetry=retry; s.countdown=57;
}
// A start as the console shows it, t seconds after the tile was pressed (the
// disc's quiet wait cut short): the step, the line and where the bar is.
static void Run(nia::LoadScreen& s, double t) {
    static const struct { double at; int step; double fraction; const char* status; } steps[]={
        {0,nia::StepServer,0.08,"Checking the NetISO server at 192.168.1.20"},
        {1.3,nia::StepMount,0.25,"Asking the server for the disc"},
        {2.4,nia::StepDisc,0.45,"The disc is going in. Waiting for the console to read it"},
        {5.4,nia::StepStart,1,"Starting Skate 3"}};
    int n=0;
    while(n+1<int(sizeof(steps)/sizeof(steps[0])) && t>=steps[n+1].at) ++n;
    s.step=steps[n].step; strcpy(s.status,steps[n].status);
    // The bar glides as main.cpp's Glide does, at 60 frames a second.
    double bar=0;
    for(double at=0;at<t;at+=1.0/60) {
        int k=0;
        while(k+1<int(sizeof(steps)/sizeof(steps[0])) && at>=steps[k+1].at) ++k;
        double target=steps[k].fraction;
        if(steps[k].step==nia::StepDisc) target+=0.4*(at-steps[k].at)/3.0;  // Reading the disc.
        double speed=(target-bar)*5; if(speed<0.6) speed=0.6;
        bar+=speed/60; if(bar>target) bar=target;
    }
    s.fraction=bar; s.time=t; s.elapsed=t+0.4; s.intro=t;
}
int main(int argc, char** argv) {
    nia::InitLoadUi();
    unsigned char* alpha=NULL; ImGui::GetIO().Fonts->GetTexDataAsAlpha8(&alpha,&TextureWidth,&TextureHeight); Texture=alpha;
    nia::LoadScreen s; const char* scene=argc>2 ? argv[2] : "mount";
    strcpy(s.iso,"Skate 3 (USA, Europe) (En,Fr,De,Es,It,Nl).iso"); nia::GameName(s.iso,s.game,sizeof(s.game));
    strcpy(s.server,"192.168.1.20"); s.titleId=0x454108E6; s.time=0.3; s.elapsed=12.4;
    double at=argc>3 ? atof(argv[3]) : 0;
    if(!strcmp(scene,"intro")) { s.step=nia::StepServer; s.fraction=0.08; strcpy(s.status,"Checking the NetISO server at 192.168.1.20"); s.intro=at; s.time=at; s.elapsed=at; }
    else if(!strcmp(scene,"run")) Run(s,at);
    else if(!strcmp(scene,"server")) { s.step=nia::StepServer; s.fraction=0.04; strcpy(s.status,"Checking the NetISO server at 192.168.1.20"); }
    else if(!strcmp(scene,"network")) { s.step=nia::StepServer; s.fraction=0.04; strcpy(s.status,"The Xbox's network is still starting after the reboot. Waiting for the server... 42 s"); }
    else if(!strcmp(scene,"mount")) { s.step=nia::StepMount; s.fraction=0.2; strcpy(s.status,"Asking the server for the disc"); }
    else if(!strcmp(scene,"disc")) { s.step=nia::StepDisc; s.fraction=0.62; strcpy(s.status,"The disc is going in. Waiting for the console to read it"); s.time=0.7; }
    else if(!strcmp(scene,"start")) { s.step=nia::StepStart; s.fraction=1; strcpy(s.status,"Starting Skate 3"); }
    else if(!strcmp(scene,"no-server")) Error(s,nia::StepServer,"Can't reach the NetISO server",
        "Nothing answered at 192.168.1.20, port 4323.",
        "Start NetDisc 360 on the PC, and check that the PC and the Xbox",
        "are on the same network. The address is in Usb0:\\netiso.xex.txt.",true);
    else if(!strcmp(scene,"not-listed")) Error(s,nia::StepMount,"Skate 3 isn't on the server",
        "The server doesn't list Skate 3 (USA, Europe) (En,Fr,De,Es,It,Nl).iso.",
        "It may have been renamed, moved or deleted on the PC. The server",
        "removes this tile from Aurora once the ISO is gone.",true);
    else if(!strcmp(scene,"no-disc")) Error(s,nia::StepDisc,"The disc didn't show up",
        "The server mounted the game, but the console didn't read a disc within",
        "30 seconds. Try again; if it keeps happening, reboot the Xbox.","",true);
    else if(!strcmp(scene,"wrong-disc")) Error(s,nia::StepDisc,"A different game is in the drive",
        "The mounted disc is Title ID 4D5307E6, not Skate 3's 454108E6.",
        "The ISO may not be the game this tile was made for.","",true);
    else if(!strcmp(scene,"no-plugin")) Error(s,nia::StepMount,"The NetISO plugin isn't running",
        "NetISO.xex isn't loaded, so there is no NetISO drive to mount from.",
        "Add it to DashLaunch's plugins in launch.ini and reboot the Xbox.","",false);
    else if(!strcmp(scene,"install")) {
        strcpy(s.iso,"GTA V.iso"); nia::GameName(s.iso,s.game,sizeof(s.game)); s.titleId=0x545408A7;
        Error(s,nia::StepServer,"GTA V needs its content installed first",
            "GTA V won't play until GTA V - Disc 2 installs its content.",
            "A starts GTA V - Disc 2 to install it. Then start GTA V again.",
            "X starts GTA V anyway, if you installed it some other way.",true);
        s.question=true; strcpy(s.buttonA,"Install now"); strcpy(s.buttonX,"Play anyway");
    }
    ImGui::GetIO().DeltaTime=1.0f/60;
    nia::DrawLoadUi(s);
    Rasterize(ImGui::GetDrawData());
    unsigned char header[54]={'B','M'};
    Put32(header+2,54+sizeof(pixels)); Put32(header+10,54); Put32(header+14,40); Put32(header+18,1280); Put32(header+22,unsigned(-720));
    Put16(header+26,1); Put16(header+28,32); Put32(header+34,sizeof(pixels));
    FILE* output=fopen(argc>1 ? argv[1] : "preview.bmp","wb"); if(!output) return 1;
    fwrite(header,sizeof(header),1,output);
    for(int i=0;i<1280*720;++i) { unsigned char p[4]; Put32(p,pixels[i]); fwrite(p,4,1,output); }
    fclose(output);
    ImGui::DestroyContext();
    return 0;
}
