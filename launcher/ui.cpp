// SPDX-License-Identifier: MIT
// The loading screen, laid out for a 1280x720 back buffer inside the
// title-safe area (the central 90%), with Xbox360-FastFTP's colours, font
// and shapes.
#include "ui.h"
#include "../third_party/imgui/imgui.h"
#include "../third_party/fonts/roboto_medium.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace nia {
LoadScreen::LoadScreen() : titleId(0), step(StepServer), fraction(0), canRetry(false), countdown(0), time(0), elapsed(-1) {
    game[0]=iso[0]=server[0]=status[0]=errorTitle[0]=0;
    for(int i=0;i<3;++i) errorLines[i][0]=0;
}

void GameName(const char* iso, char* name, size_t capacity) {
    // The name up to the first " (" (region and languages), without ".iso".
    size_t length=strlen(iso);
    const char* dot=strrchr(iso,'.');
    if(dot && (!_stricmp(dot,".iso"))) length=size_t(dot-iso);
    const char* paren=strstr(iso," (");
    if(paren && size_t(paren-iso)<length && paren>iso) length=size_t(paren-iso);
    if(length>=capacity) length=capacity-1;
    memcpy(name,iso,length); name[length]=0;
}

static ImFont* Small; static ImFont* Body; static ImFont* Heading; static ImFont* Title;
enum { Left=64, Right=1216, Top=36, Bottom=684 };

static ImU32 Hex(unsigned int rgb, int alpha=255) { return IM_COL32((rgb>>16)&255,(rgb>>8)&255,rgb&255,alpha); }
static const unsigned int Background=0x0F1725,Card=0x192537,Raised=0x223349,White=0xE2EBF4,Muted=0x8FA6BA,Dim=0x51667A,
    Green=0x3ED9BC,Error=0xFFAC8B,ErrorStrong=0xF0624D,ButtonA=0x5DC21E,ButtonB=0xE5413A,ButtonX=0x2E7FE0;

void InitLoadUi() {
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();
    io.IniFilename=NULL; io.LogFilename=NULL; io.DisplaySize=ImVec2(1280,720);
    ImFontConfig config; config.FontDataOwnedByAtlas=false; config.OversampleH=2; config.OversampleV=1;
    void* data=const_cast<unsigned char*>(RobotoMedium); const int size=int(sizeof(RobotoMedium));
    static const ImWchar ascii[]={0x20,0x7E,0};
    Body=io.Fonts->AddFontFromMemoryTTF(data,size,26,&config,ascii);
    Small=io.Fonts->AddFontFromMemoryTTF(data,size,21,&config,ascii);
    Heading=io.Fonts->AddFontFromMemoryTTF(data,size,38,&config,ascii);
    Title=io.Fonts->AddFontFromMemoryTTF(data,size,60,&config,ascii);
    ImGuiStyle& style=ImGui::GetStyle();
    style.WindowPadding=ImVec2(0,0); style.WindowBorderSize=0; style.WindowRounding=0;
}

static ImDrawList* List;
static void Box(float x, float y, float w, float h, ImU32 color, float rounding=0) { List->AddRectFilled(ImVec2(x,y),ImVec2(x+w,y+h),color,rounding); }
static float Width(ImFont* font, const char* text) { return font->CalcTextSizeA(font->FontSize,10000,0,text).x; }
static void Text(ImFont* font, float x, float y, ImU32 color, const char* text) { List->AddText(font,font->FontSize,ImVec2(x,y),color,text); }
static void TextRight(ImFont* font, float right, float y, ImU32 color, const char* text) { Text(font,right-Width(font,text),y,color,text); }
// Text cut to a width, with "..." when it doesn't fit.
static void TextFit(ImFont* font, float x, float y, float w, ImU32 color, const char* text) {
    if(Width(font,text)<=w) { Text(font,x,y,color,text); return; }
    char cut[256]; size_t length=strlen(text); if(length>sizeof(cut)-4) length=sizeof(cut)-4;
    while(length) {
        memcpy(cut,text,length); strcpy_s(cut+length,4,"...");
        if(Width(font,cut)<=w) break;
        --length;
    }
    Text(font,x,y,color,cut);
}
// A face button with its label after it. Returns the next x.
static float Button(float x, float y, const char* name, unsigned int color, const char* label) {
    List->AddCircleFilled(ImVec2(x+16,y+16),16,Hex(color),24);
    Text(Small,x+16-Width(Small,name)/2,y+4,Hex(Background),name);
    Text(Body,x+42,y+1,Hex(White),label);
    return x+42+Width(Body,label)+40;
}
static void Check(ImVec2 c, float r, ImU32 color) {
    List->AddLine(ImVec2(c.x-r*0.45f,c.y+r*0.02f),ImVec2(c.x-r*0.1f,c.y+r*0.38f),color,3);
    List->AddLine(ImVec2(c.x-r*0.1f,c.y+r*0.38f),ImVec2(c.x+r*0.48f,c.y-r*0.36f),color,3);
}
static void Cross(ImVec2 c, float r, ImU32 color) {
    List->AddLine(ImVec2(c.x-r*0.38f,c.y-r*0.38f),ImVec2(c.x+r*0.38f,c.y+r*0.38f),color,3);
    List->AddLine(ImVec2(c.x+r*0.38f,c.y-r*0.38f),ImVec2(c.x-r*0.38f,c.y+r*0.38f),color,3);
}

// The four steps in a row: done ones ticked, the current one pulsing, a
// failed one crossed in red.
static void Steps(const LoadScreen& s, float x, float y, float w, bool failed) {
    static const char* names[StepCount]={"Server","Mount","Disc","Start"};
    const float r=17, gap=(w-2*r)/(StepCount-1);
    for(int i=0;i<StepCount-1;++i) {
        float x0=x+r+i*gap+r+8, x1=x+r+(i+1)*gap-r-8;
        List->AddLine(ImVec2(x0,y+r),ImVec2(x1,y+r),Hex(i<s.step ? Green : Raised),3);
    }
    for(int i=0;i<StepCount;++i) {
        ImVec2 c(x+r+i*gap,y+r);
        ImU32 label=Hex(Dim);
        if(i<s.step) {
            List->AddCircleFilled(c,r,Hex(Green),32); Check(c,r,Hex(Background)); label=Hex(Muted);
        } else if(i==s.step && failed) {
            List->AddCircleFilled(c,r,Hex(ErrorStrong),32); Cross(c,r,Hex(Background)); label=Hex(Error);
        } else if(i==s.step) {
            float pulse=0.5f+0.5f*float(sin(s.time*4.0));
            List->AddCircle(c,r,Hex(Green),32,3);
            List->AddCircleFilled(c,5+3*pulse,Hex(Green,int(140+115*pulse)),24);
            label=Hex(White);
        } else List->AddCircle(c,r,Hex(Raised),32,3);
        Text(Small,c.x-Width(Small,names[i])/2,y+2*r+10,label,names[i]);
    }
}

static void Progress(float x, float y, float w, double fraction, double time) {
    if(fraction<0) fraction=0;
    if(fraction>1) fraction=1;
    Box(x,y,w,10,Hex(Raised),5);
    float filled=float(w*fraction); if(filled<10) filled=10;
    Box(x,y,filled,10,Hex(Green),5);
    // A soft highlight running along the filled part shows it's alive.
    float at=float(fmod(time*0.6,1.0))*filled;
    List->AddRectFilledMultiColor(ImVec2(x+at-60<x ? x : x+at-60,y),ImVec2(x+at,y+10),Hex(0xFFFFFF,0),Hex(0xFFFFFF,70),Hex(0xFFFFFF,70),Hex(0xFFFFFF,0));
}

void DrawLoadUi(const LoadScreen& s) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1280,720));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,Hex(Background));
    ImGui::Begin("NetISO",NULL,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus);
    List=ImGui::GetWindowDrawList();
    const bool failed=s.errorTitle[0]!=0;
    char text[160];

    // Header: name and what it does on the left, the server on the right.
    Box(Left,Top+6,6,40,Hex(Green),3);
    Text(Heading,Left+20,Top,Hex(White),"NetISO");
    Text(Small,Left+20+Width(Heading,"NetISO")+16,Top+15,Hex(Muted),"Playing from your NetISO server");
    float headerRight=Right;
    if(s.server[0]) {
        TextRight(Heading,Right,Top,Hex(White),s.server);
        const char* label="NetISO server  /  port 4323";
        float lx=Right-Width(Small,label);
        List->AddCircleFilled(ImVec2(lx-14,Top+57),5,Hex(failed && s.step==StepServer ? ErrorStrong : Green),16);
        Text(Small,lx,Top+46,Hex(Muted),label);
        float sx=Right-Width(Heading,s.server);
        headerRight=(sx<lx-19 ? sx : lx-19)-56;
    }
    // How long since the tile was pressed, left of the server.
    if(s.elapsed>=0) {
        int seconds=int(s.elapsed);
        if(seconds>=3600) _snprintf_s(text,sizeof(text),_TRUNCATE,"%d:%02d:%02d",seconds/3600,seconds/60%60,seconds%60);
        else _snprintf_s(text,sizeof(text),_TRUNCATE,"%d:%02d",seconds/60,seconds%60);
        TextRight(Heading,headerRight,Top,Hex(White),text);
        TextRight(Small,headerRight,Top+46,Hex(Muted),"Elapsed");
    }

    // The game.
    const float cardY=Top+104, cardH=452;
    Box(Left,cardY,Right-Left,cardH,Hex(Card),14);
    float x=Left+40, y=cardY+30, w=Right-Left-80;
    TextFit(Title,x,y,w,Hex(White),s.game[0] ? s.game : "Starting the game");
    y+=72;
    if(s.titleId) _snprintf_s(text,sizeof(text),_TRUNCATE,"%s   /   Title ID %08X",s.iso,s.titleId);
    else _snprintf_s(text,sizeof(text),_TRUNCATE,"%s",s.iso);
    TextFit(Small,x,y,w,Hex(Muted),text);
    y+=58;
    Steps(s,x+40,y,w-80,failed);
    y+=92;
    if(failed) {
        // What went wrong and what to do, in place of the progress bar.
        Box(x-12,y-14,w+24,cardY+cardH-y-8,Hex(Raised),12);
        List->AddCircleFilled(ImVec2(x+20,y+22),18,Hex(ErrorStrong),32);
        Text(Heading,x+20-Width(Heading,"!")/2,y+1,Hex(Background),"!");
        TextFit(Heading,x+56,y+2,w-64,Hex(Error),s.errorTitle);
        float ly=y+52;
        for(int i=0;i<3;++i) if(s.errorLines[i][0]) { TextFit(Body,x+56,ly,w-64,Hex(i ? Muted : White),s.errorLines[i]); ly+=34; }
    } else {
        Progress(x,y+6,w,s.fraction,s.time);
        _snprintf_s(text,sizeof(text),_TRUNCATE,"%d%%",int(s.fraction>=1 ? 100 : s.fraction<0 ? 0 : s.fraction*100));
        TextRight(Body,x+w,y+34,Hex(Muted),text);
        TextFit(Body,x,y+34,w-96,Hex(White),s.status);
    }

    // Footer: what the buttons do.
    List->AddLine(ImVec2(Left,Bottom-56),ImVec2(Right,Bottom-56),Hex(Raised),1);
    float bx=Left;
    if(failed) {
        bx=Button(bx,Bottom-40,"A",ButtonA,"Back to Aurora");
        if(s.canRetry) bx=Button(bx,Bottom-40,"X",ButtonX,"Try again");
        if(s.countdown>0) {
            _snprintf_s(text,sizeof(text),_TRUNCATE,"Back to Aurora in %d s",s.countdown);
            TextRight(Small,Right,Bottom-36,Hex(Dim),text);
        }
    } else {
        Button(bx,Bottom-40,"B",ButtonB,"Cancel");
        TextRight(Small,Right,Bottom-36,Hex(Dim),"The game starts by itself");
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::Render();
}
}
