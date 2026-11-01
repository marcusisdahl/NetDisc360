// SPDX-License-Identifier: MIT
// NetDiscParty.xex's screen (see ui.h), laid out for a 1280x720 back buffer
// inside the title-safe area, in the launcher's colours and font. The wheel
// is drawn as the dashboard draws it (raffleWheel in web/index.html): the
// guests' names round it, clockwise from the wheel's top, the pointer at the
// right where the name it stops on reads level, each segment taking its
// team's colour once the wheel stopped on it, with confetti at each stop.
#include "ui.h"
#include "../third_party/imgui/imgui.h"
#include "../third_party/fonts/roboto_medium_latin.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef _MSC_VER
#define _snprintf_s(out,size,count,...) snprintf(out,size,__VA_ARGS__)
#endif

namespace party {
Screen::Screen() : spinMs(0), time(0), link(LinkLooking), view(ViewWheel), asking(0), choosing(-1), leaveIn(-1) {
    memset(&state,0,sizeof(state));
    server[0]=notice[0]=0;
    for(int i=0;i<PerConsole;++i) { players[i].controller=-1; players[i].gamertag[0]=0; }
}

static ImFont* Small; static ImFont* Body; static ImFont* Heading; static ImFont* Title; static ImFont* Names; static ImFont* Big;
enum { Left=64, Right=1216, Top=36, Bottom=684 };
static const float Pi=3.14159265f;
// Shown while it turns and this long after, before the screen goes back to the seats.
static const double AfterMs=30000;

static ImU32 Hex(unsigned int rgb, int alpha=255) { return IM_COL32((rgb>>16)&255,(rgb>>8)&255,rgb&255,alpha); }
static const unsigned int Background=0x0F1725,Card=0x192537,Raised=0x223349,White=0xE2EBF4,Muted=0x8FA6BA,Dim=0x51667A,
    Green=0x3ED9BC,Error=0xFFAC8B,ErrorStrong=0xF0624D,Ink=0x1B1E22,ButtonA=0x5DC21E,ButtonB=0xE5413A,ButtonX=0x2E7FE0,ButtonY=0xF2C811;
// The teams' colours: Red, Blue, Green and Yellow, as the dashboard's big screen has them.
static const unsigned int TeamFill[TeamsMost]={0xE5483E,0x3F7FE6,0x2C9A2C,0xD9A400};
static const unsigned int TeamText[TeamsMost]={0xFF6B61,0x6EA8FF,0x5FD35F,0xFFD23F};
static const char* const TeamName[TeamsMost]={"Team Red","Team Blue","Team Green","Team Yellow"};

static unsigned Mix(unsigned a, unsigned b, float t) {
    unsigned out=0;
    for(int shift=0;shift<24;shift+=8) {
        float x=float((a>>shift)&255), y=float((b>>shift)&255);
        out|=unsigned(x+(y-x)*t+0.5f)<<shift;
    }
    return out;
}

void InitUi() {
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();
    io.IniFilename=NULL; io.LogFilename=NULL; io.DisplaySize=ImVec2(1280,720);
    ImFontConfig config; config.FontDataOwnedByAtlas=false; config.OversampleH=2; config.OversampleV=1;
    void* data=const_cast<unsigned char*>(RobotoMediumLatin); const int size=int(sizeof(RobotoMediumLatin));
    // Latin letters (Ø, Å, Ł, Č...) for the guests' names, the dots and quotes.
    static const ImWchar latin[]={0x20,0x17F,0x2018,0x201D,0x2026,0x2026,0};
    Body=io.Fonts->AddFontFromMemoryTTF(data,size,26,&config,latin);
    Small=io.Fonts->AddFontFromMemoryTTF(data,size,21,&config,latin);
    Heading=io.Fonts->AddFontFromMemoryTTF(data,size,38,&config,latin);
    Title=io.Fonts->AddFontFromMemoryTTF(data,size,54,&config,latin);
    Names=io.Fonts->AddFontFromMemoryTTF(data,size,30,&config,latin);
    Big=io.Fonts->AddFontFromMemoryTTF(data,size,76,&config,latin);
    ImGuiStyle& style=ImGui::GetStyle();
    style.WindowPadding=ImVec2(0,0); style.WindowBorderSize=0; style.WindowRounding=0;
}

static ImDrawList* List;
static void Box(float x, float y, float w, float h, ImU32 color, float rounding=0) { List->AddRectFilled(ImVec2(x,y),ImVec2(x+w,y+h),color,rounding); }
static float Width(ImFont* font, const char* text) { return font->CalcTextSizeA(font->FontSize,10000,0,text).x; }
static void Text(ImFont* font, float x, float y, ImU32 color, const char* text) { List->AddText(font,font->FontSize,ImVec2(x,y),color,text); }
static void TextRight(ImFont* font, float right, float y, ImU32 color, const char* text) { Text(font,right-Width(font,text),y,color,text); }
// text cut to a width, with "..." when it doesn't fit (at a whole UTF-8 character).
static void Fit(ImFont* font, float w, const char* text, char* out, size_t capacity) {
    _snprintf_s(out,capacity,_TRUNCATE,"%s",text);
    if(Width(font,out)<=w) return;
    size_t length=strlen(out);
    while(length) {
        do --length; while(length && (out[length]&0xC0)==0x80);
        if(length+4>capacity) continue;
        memcpy(out+length,"...",4);
        if(Width(font,out)<=w) return;
    }
}
static void TextFit(ImFont* font, float x, float y, float w, ImU32 color, const char* text) {
    char cut[NameBytes*2]; Fit(font,w,text,cut,sizeof(cut));
    Text(font,x,y,color,cut);
}
static void TextCentred(ImFont* font, float cx, float y, float w, ImU32 color, const char* text) {
    char cut[NameBytes*2]; Fit(font,w,text,cut,sizeof(cut));
    Text(font,cx-Width(font,cut)/2,y,color,cut);
}
// A face button with its label after it. Returns the next x.
static float Button(float x, float y, const char* name, unsigned int color, const char* label) {
    List->AddCircleFilled(ImVec2(x+16,y+16),16,Hex(color),24);
    Text(Small,x+16-Width(Small,name)/2,y+4,Hex(Background),name);
    Text(Body,x+42,y+1,Hex(White),label);
    return x+42+Width(Body,label)+40;
}
// A crown, as the dashboard marks the Gamemaster's console.
static void Crown(float x, float y, float s, ImU32 color) {
    ImVec2 p[7]={ImVec2(x,y+s*0.8f),ImVec2(x,y+s*0.25f),ImVec2(x+s*0.3f,y+s*0.5f),ImVec2(x+s*0.5f,y),ImVec2(x+s*0.7f,y+s*0.5f),ImVec2(x+s,y+s*0.25f),ImVec2(x+s,y+s*0.8f)};
    List->AddConvexPolyFilled(p,3,color);
    ImVec2 q[3]={p[2],p[3],p[4]}; List->AddConvexPolyFilled(q,3,color);
    ImVec2 r[3]={p[4],p[5],p[6]}; List->AddConvexPolyFilled(r,3,color);
    Box(x,y+s*0.5f,s,s*0.3f,color);
    Box(x,y+s*0.88f,s,s*0.12f,color);
}

// Text from the centre c out along angle (radians, clockwise from 3 o'clock),
// its end right from c: drawn flat, then its corners turned round c.
static void RadialText(ImFont* font, ImVec2 c, float angle, float right, float room, ImU32 color, const char* text) {
    char cut[NameBytes*2]; Fit(font,room,text,cut,sizeof(cut));
    int first=List->VtxBuffer.Size;
    List->AddText(font,font->FontSize,ImVec2(c.x+right-Width(font,cut),c.y-font->FontSize*0.55f),color,cut);
    float cs=cosf(angle), sn=sinf(angle);
    for(int i=first;i<List->VtxBuffer.Size;++i) {
        ImVec2& p=List->VtxBuffer[i].pos;
        float dx=p.x-c.x, dy=p.y-c.y;
        p.x=c.x+dx*cs-dy*sn; p.y=c.y+dx*sn+dy*cs;
    }
}

// The wheel: names (count of them) round centre c, turned angle degrees
// clockwise; team[k] is segment k's team once stopped on (-1 still open, -2
// picked alone); landed is the segment it rests on (-1: none).
static void Wheel(ImVec2 c, float r, double angle, const Name* names, int count, const int* team, int landed, unsigned accent) {
    const unsigned rim=Mix(accent,0x000000,0.45f);
    const unsigned shade[3]={Mix(accent,0xFFFFFF,0.82f),Mix(accent,0xFFFFFF,0.62f),Mix(accent,0xFFFFFF,0.74f)};
    // A glow behind it, its rim and its shadow.
    for(int i=6;i>0;--i) List->AddCircleFilled(c,r+12+i*9,Hex(accent,6),64);
    List->AddCircleFilled(ImVec2(c.x,c.y+8),r+14,Hex(0x000000,90),96);
    List->AddCircleFilled(c,r+12,Hex(rim),96);
    const float w=2*Pi/float(count > 0 ? count : 1), turn=float(fmod(angle,360.0))*Pi/180;
    if(count<=1) {
        List->AddCircleFilled(c,r,Hex(shade[0]),96);
        if(count==1) RadialText(Names,c,-Pi/2+turn,r-26,r*0.68f,Hex(Ink),names[0]);
    }
    for(int k=0;k<count && count>1;++k) {
        float a0=-Pi/2+turn+k*w;
        int t=team[k];
        unsigned fill=t>=0 ? TeamFill[t] : t==-2 ? rim : shade[count%2 && k==count-1 ? 2 : k%2];
        List->PathLineTo(c);
        List->PathArcTo(c,r,a0,a0+w,4+int(w*24));
        List->PathFillConvex(Hex(fill));
    }
    for(int k=0;k<count && count>1;++k) {
        float a=-Pi/2+turn+k*w;
        List->AddLine(c,ImVec2(c.x+r*cosf(a),c.y+r*sinf(a)),Hex(0xFFFFFF,140),2);
    }
    if(landed>=0 && landed<count && count>1) {
        float a0=-Pi/2+turn+landed*w;
        List->PathLineTo(c);
        List->PathArcTo(c,r-3,a0,a0+w,4+int(w*24));
        List->PathStroke(Hex(0xFFFFFF),true,6);
    }
    for(int k=0;k<count && count>1;++k) {
        int t=team[k];
        ImU32 ink=t==-1 || t==3 ? Hex(Ink) : Hex(0xFFFFFF);
        RadialText(Names,c,-Pi/2+turn+(k+0.5f)*w,r-26,r*0.66f,ink,names[k]);
    }
    // Pegs on the lines, the hub, and the pointer at the right.
    for(int k=0;k<count && count>1;++k) {
        float a=-Pi/2+turn+k*w;
        List->AddCircleFilled(ImVec2(c.x+(r+1)*cosf(a),c.y+(r+1)*sinf(a)),5,Hex(0xFFFFFF),12);
    }
    List->AddCircleFilled(c,r*0.16f,Hex(rim),48);
    List->AddCircleFilled(c,r*0.11f,Hex(0xFFFFFF),48);
    const float tip=c.x+r-30, back=c.x+r+30;
    List->AddTriangleFilled(ImVec2(back,c.y-24+4),ImVec2(back,c.y+24+4),ImVec2(tip,c.y+4),Hex(0x000000,90));
    List->AddTriangleFilled(ImVec2(back,c.y-24),ImVec2(back,c.y+24),ImVec2(tip,c.y),Hex(0xFFFFFF));
    List->AddTriangle(ImVec2(back,c.y-24),ImVec2(back,c.y+24),ImVec2(tip,c.y),Hex(rim),3);
}

// Confetti, ms after the moment it was thrown, from nothing to gone in
// lastMs: from (x, y) in a burst, or (fromTop) falling across the screen.
// The same seed throws the same pieces, so every frame (and every console)
// draws them where the time says, with nothing kept between frames.
static unsigned Random(unsigned& state) { state^=state<<13; state^=state>>17; state^=state<<5; return state; }
static float Unit(unsigned& state) { return float(Random(state)%10000)/9999.0f; }
static void Confetti(float x, float y, double ms, double lastMs, int count, unsigned seed, const unsigned* colours, int colourCount, bool fromTop) {
    if(ms<0 || ms>=lastMs || colourCount<1) return;
    const float t=float(ms/1000.0), fade=ms>lastMs-600 ? float((lastMs-ms)/600) : 1.f;
    for(int i=0;i<count;++i) {
        unsigned state=seed*2654435761u^unsigned(i+1)*40503u; if(!state) state=1;
        Random(state); Random(state);
        float px, py;
        if(fromTop) {
            px=Left+Unit(state)*(Right-Left); py=-30-Unit(state)*260;
            px+=sinf(t*2.5f+Unit(state)*6)*28; py+=t*(170+Unit(state)*150);
        } else {
            float a=-Pi/2+(Unit(state)-0.5f)*2.6f, speed=260+Unit(state)*520;
            px=x+cosf(a)*speed*t*0.85f; py=y+sinf(a)*speed*t+0.5f*820*t*t;
        }
        float spin=Unit(state)*6.3f+t*(Unit(state)-0.5f)*14, w=9+Unit(state)*6, h=4+Unit(state)*3;
        float cs=cosf(spin), sn=sinf(spin);
        ImVec2 corner[4];
        static const float sx[4]={-1,1,1,-1}, sy[4]={-1,-1,1,1};
        for(int k=0;k<4;++k) corner[k]=ImVec2(px+sx[k]*w*cs-sy[k]*h*sn,py+sx[k]*w*sn+sy[k]*h*cs);
        List->AddQuadFilled(corner[0],corner[1],corner[2],corner[3],Hex(colours[i%colourCount],int(255*fade)));
    }
}
static const double BurstMs=2300, PartyMs=4500;

// A team's card: its colour on top, its name and who is on it.
static void TeamCard(float x, float y, float w, float h, int t, const Name* names, int count, bool small) {
    Box(x,y,w,h,Hex(Card),12);
    Box(x,y,w,8,Hex(TeamFill[t]),4);
    Text(Small,x+16,y+18,Hex(TeamText[t]),TeamName[t]);
    float ly=y+50, step=small ? 30.f : 34.f;
    ImFont* font=small ? Small : Body;
    for(int i=0;i<count && ly+step<=y+h+4;++i) {
        // The last line says how many more there are, when they don't all fit.
        if(ly+2*step>y+h+4 && i<count-1) {
            char more[32]; _snprintf_s(more,sizeof(more),_TRUNCATE,"and %d more",count-i);
            Text(font,x+16,ly,Hex(Muted),more);
            break;
        }
        TextFit(font,x+16,ly,w-32,Hex(White),names[i]);
        ly+=step;
    }
}

// A controller's ring of light: four quarters round c, the one for
// controller (0-3: top left, top right, bottom left, bottom right) lit.
static void Ring(ImVec2 c, float r, int controller, unsigned lit, float glow) {
    // Clockwise from the top left quarter: 0, 1, 3, 2.
    static const int quarter[4]={0,1,3,2};
    List->AddCircleFilled(c,r+10,Hex(Background),48);
    for(int q=0;q<4;++q) {
        int pad=quarter[q];
        float a0=Pi+q*Pi/2+0.12f, a1=Pi+(q+1)*Pi/2-0.12f;
        List->PathArcTo(c,r,a0,a1,16);
        List->PathStroke(pad==controller ? Hex(lit) : Hex(Raised),false,10);
        if(pad==controller && glow>0) { List->PathArcTo(c,r,a0,a1,16); List->PathStroke(Hex(lit,int(70*glow)),false,22); }
    }
    List->AddCircleFilled(c,r*0.62f,Hex(Raised),48);
    List->AddCircleFilled(c,r*0.5f,Hex(controller>=0 ? lit : Card),48);
    if(controller>=0) {
        char n[12]; _snprintf_s(n,sizeof(n),_TRUNCATE,"%d",controller+1);
        Text(Heading,c.x-Width(Heading,n)/2,c.y-22,Hex(Ink),n);
    }
}

// This console's players, signing in one after another: a card for each of
// the four seats, the one asked now lit, its guest's team colour on top.
static void DrawPlayers(const Screen& screen, const Seat& mine) {
    const State& s=screen.state;
    char text[256];
    const float top=172, cw=270, ch=378, gap=(Right-Left-4*cw)/3;
    int in=0;
    for(int i=0;i<mine.count;++i) in+=screen.players[i].controller>=0;
    _snprintf_s(text,sizeof(text),_TRUNCATE,"Playing on %s",s.console[0] ? s.console : "this Xbox");
    TextFit(Body,Left,top-44,700,Hex(Muted),text);
    _snprintf_s(text,sizeof(text),_TRUNCATE,"%d of %d signed in",in,mine.count);
    TextRight(Body,Right,top-44,Hex(in==mine.count ? Green : Muted),text);
    const float pulse=0.5f+0.5f*sinf(float(screen.time)*5);
    for(int i=0;i<PerConsole;++i) {
        const float x=Left+i*(cw+gap), y=top, cx=x+cw/2;
        if(i>=mine.count) {
            Box(x,y,cw,ch,Hex(Card,120),16);
            Ring(ImVec2(cx,y+130),52,-1,ButtonA,0);
            TextCentred(Body,cx,y+214,cw-32,Hex(Dim),"Free seat");
            continue;
        }
        const Player& p=screen.players[i];
        const int t=TeamOf(s,mine.names[i]);
        const bool asked=screen.asking==i, signedIn=p.controller>=0;
        Box(x,y,cw,ch,Hex(asked ? Raised : Card),16);
        // The team's colour across the top, its name in it, and round the card.
        if(t>=0) {
            Box(x,y,cw,56,Hex(TeamFill[t]),16);
            Box(x,y+40,cw,16,Hex(TeamFill[t]));
            List->AddRect(ImVec2(x,y),ImVec2(x+cw,y+ch),Hex(TeamFill[t]),16,0,3);
            Text(Body,x+18,y+13,Hex(t==3 ? Ink : 0xFFFFFF),TeamName[t]);
        }
        if(asked) List->AddRect(ImVec2(x,y),ImVec2(x+cw,y+ch),Hex(Mix(Raised,0xFFFFFF,0.35f+0.45f*pulse)),16,0,5);
        _snprintf_s(text,sizeof(text),_TRUNCATE,"P%d",i+1);
        TextRight(Body,x+cw-18,y+13,Hex(t>=0 ? (t==3 ? Ink : 0xFFFFFF) : Muted,t>=0 ? 200 : 255),text);
        Ring(ImVec2(cx,y+130),52,p.controller,signedIn ? ButtonA : Dim,signedIn ? 1.f : 0.f);
        TextCentred(Heading,cx,y+204,cw-28,Hex(White),mine.names[i]);
        if(signedIn) {
            List->AddCircleFilled(ImVec2(cx-Width(Body,"Signed in")/2-18,y+293),12,Hex(Green),20);
            List->AddLine(ImVec2(cx-Width(Body,"Signed in")/2-24,y+293),ImVec2(cx-Width(Body,"Signed in")/2-19,y+298),Hex(Ink),3);
            List->AddLine(ImVec2(cx-Width(Body,"Signed in")/2-19,y+298),ImVec2(cx-Width(Body,"Signed in")/2-11,y+287),Hex(Ink),3);
            Text(Body,cx-Width(Body,"Signed in")/2+2,y+278,Hex(Green),"Signed in");
            _snprintf_s(text,sizeof(text),_TRUNCATE,"Controller %d",p.controller+1);
            TextCentred(Small,cx,y+322,cw-32,Hex(Muted),text);
            if(p.gamertag[0]) TextCentred(Small,cx,y+350,cw-32,Hex(Muted),p.gamertag);
        } else if(asked && screen.choosing>=0) {
            TextCentred(Body,cx,y+278,cw-32,Hex(White),"Choose your profile");
            _snprintf_s(text,sizeof(text),_TRUNCATE,"on controller %d",screen.choosing+1);
            TextCentred(Small,cx,y+318,cw-32,Hex(Muted),text);
        } else if(asked) {
            const float bw=42+Width(Body,"Press");
            const float bx=cx-bw/2;
            List->AddCircleFilled(ImVec2(bx+16,y+294),16+3*pulse,Hex(ButtonA),24);
            Text(Small,bx+16-Width(Small,"A")/2,y+282,Hex(Background),"A");
            Text(Body,bx+42,y+279,Hex(White),"Press");
            TextCentred(Small,cx,y+322,cw-32,Hex(Muted),"on your own controller");
        } else TextCentred(Body,cx,y+282,cw-32,Hex(Dim),"Next");
    }
    // What to do now, under the cards.
    const float py=top+ch+16;
    if(screen.asking>=0 && screen.asking<mine.count) {
        _snprintf_s(text,sizeof(text),_TRUNCATE,screen.choosing>=0 ? "%s, choose your profile" : "%s, press A to sign in",mine.names[screen.asking]);
        TextCentred(Heading,(Left+Right)/2.f,py,Right-Left,Hex(White),text);
    } else if(screen.leaveIn>=0) {
        _snprintf_s(text,sizeof(text),_TRUNCATE,"Everyone's in! Back to Aurora in %d",screen.leaveIn);
        TextCentred(Heading,(Left+Right)/2.f,py,Right-Left,Hex(Green),text);
    } else TextCentred(Heading,(Left+Right)/2.f,py,Right-Left,Hex(Green),"Everyone's in! Have fun");
}

void DrawUi(const Screen& screen) {
    const State& s=screen.state;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1280,720));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,Hex(Background));
    ImGui::Begin("NetDiscParty",NULL,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus);
    List=ImGui::GetWindowDrawList();
    char text[256];
    const unsigned accent=s.valid ? s.accent : 0x107C10;

    // Header: the party on the left, this console's name on the right.
    Box(Left,Top+6,6,40,Hex(Mix(accent,0xFFFFFF,0.25f)),3);
    float nameW=s.console[0] ? Width(Heading,s.console) : 0;
    float titleRoom=Right-Left-20-(nameW>0 ? nameW+80 : 0);
    TextFit(Heading,Left+20,Top,titleRoom,Hex(White),s.valid && s.title[0] ? s.title : "LAN party");
    if(s.valid && s.line[0]) TextFit(Small,Left+20,Top+46,titleRoom,Hex(Muted),s.line);
    else TextFit(Small,Left+20,Top+46,titleRoom,Hex(Muted),"NetDisc360 raffle wheel");
    if(s.console[0]) {
        TextRight(Heading,Right,Top,Hex(White),s.console);
        const char* label=s.gamemaster ? "This Xbox  /  Gamemaster" : "This Xbox";
        float lx=Right-Width(Small,label);
        if(s.gamemaster) Crown(lx-30,Top+49,20,Hex(0xF2C811));
        Text(Small,lx,Top+46,Hex(Muted),label);
    }

    const Seat* seat=s.valid ? MySeat(s) : NULL;
    const bool players=screen.view==ViewPlayers && seat && seat->count>0;
    if(players) DrawPlayers(screen,*seat);
    else {
    // The wheel: the latest spin's, else the guests who are here, turning slowly.
    const ImVec2 centre(354,388);
    const float radius=222;
    int team[WheelMost];
    const bool spin=s.valid && s.spinId && s.stepCount>0;
    const int total=spin ? SpinMs(s) : 0;
    const bool live=spin && screen.spinMs<total+AfterMs, turning=spin && screen.spinMs<total;
    int step=-1, landed=-1, stopped=0;
    if(spin) {
        bool still=true;
        double angle=SpinAngle(s,screen.spinMs,&step,&still);
        stopped=Stopped(s,screen.spinMs);
        for(int k=0;k<s.wheelCount;++k) team[k]=-1;
        for(int i=0;i<stopped;++i) team[s.steps[i].segment]=s.steps[i].team>=0 ? s.steps[i].team : -2;
        if(still && step>=0) landed=s.steps[step].segment;
        Wheel(centre,radius,angle,s.wheel,s.wheelCount,team,landed,accent);
    } else {
        for(int k=0;k<WheelMost;++k) team[k]=-1;
        int count=s.valid ? s.hereCount : 0;
        static const Name none[1]={""};
        Wheel(centre,radius,screen.time*8,count ? s.here : none,count,team,-1,accent);
        if(!count) TextCentred(Body,centre.x,centre.y+radius*0.3f,radius*1.4f,Hex(Ink),s.valid ? "No guests here yet" : "");
    }

    // The right side: the spin as it goes, else where this console's players sit.
    const float px=700, pw=Right-px, py=Top+112;
    if(!s.valid) {
        Box(px,py,pw,300,Hex(Card),14);
        Text(Heading,px+32,py+30,Hex(White),screen.link==LinkLost ? "Can't reach NetDisc360" : "Looking for NetDisc360");
        TextFit(Body,px+32,py+92,pw-64,Hex(Muted),screen.server[0] ? "The PC server, at" : "No server address on this Xbox yet:");
        TextFit(Body,px+32,py+130,pw-64,Hex(White),screen.server[0] ? screen.server : "server= in Hdd1:\\netiso.xex.txt");
        TextFit(Small,px+32,py+190,pw-64,Hex(Muted),"Start NetDisc360 on the PC and turn the LAN party on");
        TextFit(Small,px+32,py+220,pw-64,Hex(Muted),"on its dashboard. This screen finds it by itself.");
    } else if(live && !strcmp(s.kind,"pick")) {
        Box(px,py,pw,360,Hex(Card),14);
        TextCentred(Body,px+pw/2,py+40,pw-48,Hex(Muted),s.question[0] ? s.question : turning ? "The wheel picks one guest" : "The wheel picked");
        if(stopped>0) {
            const char* name=s.wheel[s.steps[0].segment];
            TextCentred(Big,px+pw/2,py+130,pw-48,Hex(White),name);
            List->AddLine(ImVec2(px+pw/2-90,py+238),ImVec2(px+pw/2+90,py+238),Hex(Mix(accent,0xFFFFFF,0.4f)),4);
            TextCentred(Small,px+pw/2,py+262,pw-48,Hex(Muted),"Everyone sees the same wheel");
        } else if(s.leadMs>3000 && screen.spinMs<s.leadMs) {
            _snprintf_s(text,sizeof(text),_TRUNCATE,"Turns in %d",int((s.leadMs-screen.spinMs)/1000)+1);
            TextCentred(Heading,px+pw/2,py+150,pw-48,Hex(White),text);
        } else TextCentred(Heading,px+pw/2,py+150,pw-48,Hex(White),"...");
    } else if(live) {
        // The teams filling as the wheel stops on each guest.
        const int teams=s.teamCount>0 ? s.teamCount : s.teams;
        static Name filled[TeamsMost][WheelMost];  // Static: too big for the stack.
        int sizes[TeamsMost]={0,0,0,0};
        for(int i=0;i<stopped;++i) {
            int t=s.steps[i].team;
            if(t>=0 && t<TeamsMost) _snprintf_s(filled[t][sizes[t]++],NameBytes,_TRUNCATE,"%s",s.wheel[s.steps[i].segment]);
        }
        // A long lead: the consoles open the wheel first (OpenWheel on the server).
        if(turning && s.leadMs>3000 && screen.spinMs<s.leadMs) _snprintf_s(text,sizeof(text),_TRUNCATE,"The wheel turns in %d",int((s.leadMs-screen.spinMs)/1000)+1);
        else if(turning) _snprintf_s(text,sizeof(text),_TRUNCATE,"Spinning: %d of %d on a team",stopped,s.stepCount);
        else _snprintf_s(text,sizeof(text),_TRUNCATE,"Round %d: the teams are drawn",s.round>0 ? s.round : 1);
        Text(Body,px,py-6,Hex(Muted),text);
        // The guest just stopped on, big in their team's colour, popping in;
        // while it turns, who's next; once all are drawn, the teams are ready.
        const float by=py+36, bh=124;
        if(step>=0 && landed>=0 && s.steps[step].team>=0) {
            const int t=s.steps[step].team;
            const float age=float(screen.spinMs-StopMs(s,step)), pop=age<260 ? age/260 : 1.f, grow=(1-pop)*18;
            Box(px-grow,by-grow*0.4f,pw+grow*2,bh+grow*0.8f,Hex(TeamFill[t]),18);
            const ImU32 ink=Hex(t==3 ? Ink : 0xFFFFFF);
            TextFit(Big,px+28,by+6,pw-56,ink,s.wheel[landed]);
            _snprintf_s(text,sizeof(text),_TRUNCATE,"joins %s!",TeamName[t]);
            TextFit(Heading,px+30,by+80-6,pw-60,ink,text);
        } else {
            Box(px,by,pw,bh,Hex(Card),18);
            List->AddRect(ImVec2(px,by),ImVec2(px+pw,by+bh),Hex(Raised),18,0,2);
            if(turning) TextCentred(Heading,px+pw/2,by+bh/2-22,pw-48,Hex(Muted),stopped ? "Who's next?" : "Who's first?");
            else TextCentred(Heading,px+pw/2,by+bh/2-22,pw-48,Hex(Green),"The teams are ready!");
        }
        const int columns=teams>2 ? 2 : teams, rows=(teams+1)/2;
        const float gap=16, cw=(pw-gap*(columns-1))/columns, top=by+bh+18, ch=((Bottom-74)-top-gap*(rows-1))/rows;
        for(int t=0;t<teams;++t) TeamCard(px+(t%2)*(cw+gap),top+(t/2)*(ch+gap),cw,ch,t,filled[t],sizes[t],rows>1);
    } else if(s.round>0 && s.teamCount>0) {
        // Who plays on this console, and every team.
        const Seat* mine=MySeat(s);
        int t=mine && mine->count ? TeamOf(s,mine->names[0]) : -1;
        float h=mine && mine->count ? 64.f+mine->count*40.f : 120.f;
        Box(px,py,pw,h,Hex(Card),14);
        if(t>=0) Box(px,py,10,h,Hex(TeamFill[t]),5);
        _snprintf_s(text,sizeof(text),_TRUNCATE,"Playing on %s",s.console[0] ? s.console : "this Xbox");
        TextFit(Small,px+32,py+16,pw-220,Hex(Muted),text);
        if(t>=0) TextRight(Small,px+pw-24,py+16,Hex(TeamText[t]),TeamName[t]);
        if(mine && mine->count) for(int i=0;i<mine->count;++i) TextFit(Heading,px+32,py+50+i*40,pw-64,Hex(White),mine->names[i]);
        else TextFit(Body,px+32,py+56,pw-64,Hex(White),"No one is seated here this round");
        float ty=py+h+24;
        _snprintf_s(text,sizeof(text),_TRUNCATE,s.kept ? "Round %d" : "Round %d, not kept yet",s.round);
        Text(Small,px,ty,Hex(Muted),text);
        ty+=34;
        const int columns=s.teamCount>2 ? 2 : s.teamCount, rows=(s.teamCount+1)/2;
        const float gap=12, cw=(pw-gap*(columns-1))/columns, ch=((Bottom-80)-ty-gap*(rows-1))/rows;
        if(ch>70) for(int k=0;k<s.teamCount;++k) TeamCard(px+(k%2)*(cw+gap),ty+(k/2)*(ch+gap),cw,ch,k,s.team[k],s.teamSize[k],true);
    } else {
        Box(px,py,pw,170,Hex(Card),14);
        TextFit(Heading,px+32,py+28,pw-64,Hex(White),s.active ? "Waiting for the wheel" : "The LAN party starts soon");
        if(s.gamemaster) TextFit(Body,px+32,py+88,pw-64,Hex(Muted),"Press A to spin for teams, X to pick one guest.");
        else if(s.gamemasterName[0]) {
            _snprintf_s(text,sizeof(text),_TRUNCATE,"The Gamemaster spins it, on %s.",s.gamemasterName);
            TextFit(Body,px+32,py+88,pw-64,Hex(Muted),text);
        } else TextFit(Body,px+32,py+88,pw-64,Hex(Muted),"It's spun from NetDisc360's dashboard.");
        _snprintf_s(text,sizeof(text),_TRUNCATE,s.hereCount==1 ? "%d guest here" : "%d guests here",s.hereCount);
        Text(Small,px,py+194,Hex(Muted),text);
        // The guests in two columns.
        float gy=py+232, cw=(pw-16)/2;
        int perColumn=int((Bottom-80-gy)/34);
        for(int i=0;i<s.hereCount && i<perColumn*2;++i) {
            if(i==perColumn*2-1 && s.hereCount>perColumn*2) {
                _snprintf_s(text,sizeof(text),_TRUNCATE,"and %d more",s.hereCount-i);
                Text(Body,px+(i/perColumn)*(cw+16),gy+(i%perColumn)*34,Hex(Muted),text);
                break;
            }
            TextFit(Body,px+(i/perColumn)*(cw+16),gy+(i%perColumn)*34,cw,Hex(White),s.here[i]);
        }
    }

    // Confetti over it all: a burst from the pointer at each stop, in the
    // team's colours, and a shower once the last one stopped.
    if(spin && live) {
        const ImVec2 pointer(centre.x+radius+20,centre.y);
        for(int i=0;i<stopped;++i) {
            int t=s.steps[i].team;
            unsigned colours[3]={t>=0 ? TeamFill[t] : Mix(accent,0xFFFFFF,0.3f),0xFFFFFF,t>=0 ? TeamText[t] : 0xF2C811};
            Confetti(pointer.x,pointer.y,screen.spinMs-StopMs(s,i),BurstMs,46,s.spinId*31u+unsigned(i),colours,3,false);
        }
        if(stopped==s.stepCount) {
            unsigned colours[6]={TeamFill[0],TeamFill[1],TeamFill[2],TeamFill[3],0xFFFFFF,Mix(accent,0xFFFFFF,0.3f)};
            int count=!strcmp(s.kind,"pick") ? 6 : s.teams>=2 && s.teams<=TeamsMost ? s.teams : 4;
            if(!strcmp(s.kind,"pick")) { colours[0]=0xF2C811; colours[1]=Mix(accent,0xFFFFFF,0.3f); colours[2]=0xFFFFFF; count=3; }
            unsigned shown[6]; int n=0;
            for(int k=0;k<count;++k) shown[n++]=colours[k];
            shown[n++]=0xFFFFFF;
            Confetti(0,0,screen.spinMs-StopMs(s,s.stepCount-1),PartyMs,150,s.spinId*7u+99u,shown,n,true);
        }
    }
    }

    // A word back from the server on the Gamemaster's last spin.
    if(screen.notice[0]) {
        float nw=Width(Small,screen.notice)+48; if(nw>Right-Left) nw=Right-Left;
        Box(Right-nw,Bottom-112,nw,40,Hex(Raised),10);
        List->AddCircleFilled(ImVec2(Right-nw+20,Bottom-92),6,Hex(ErrorStrong),12);
        TextFit(Small,Right-nw+36,Bottom-103,nw-48,Hex(Error),screen.notice);
    }

    // Footer: the buttons, and the server.
    List->AddLine(ImVec2(Left,Bottom-56),ImVec2(Right,Bottom-56),Hex(Raised),1);
    float bx=Left;
    if(players) {
        if(screen.asking>=0) bx=Button(bx,Bottom-40,"A",ButtonA,"Sign in");
        else if(screen.leaveIn>=0) bx=Button(bx,Bottom-40,"A",ButtonA,"Go now");
        bx=Button(bx,Bottom-40,"Y",ButtonY,"See the wheel");
    } else {
        if(s.valid && s.gamemaster && s.active) {
            bx=Button(bx,Bottom-40,"A",ButtonA,"Spin for teams");
            bx=Button(bx,Bottom-40,"X",ButtonX,"Pick one");
        }
        if(seat && seat->count>0 && !(s.valid && s.spinId && s.stepCount>0 && screen.spinMs<SpinMs(s))) bx=Button(bx,Bottom-40,"Y",ButtonY,"Sign in");
    }
    bx=Button(bx,Bottom-40,"B",ButtonB,"Back to Aurora");
    const char* where=screen.link==LinkConnected ? "Connected to NetDisc360 at %s" : screen.link==LinkLost ? "Lost NetDisc360 at %s, trying again" : screen.link==LinkWaiting ? "Waiting for the network" : "Looking for NetDisc360 %s";
    _snprintf_s(text,sizeof(text),_TRUNCATE,where,screen.server);
    float tw=Width(Small,text);
    if(Right-tw>bx) {
        TextRight(Small,Right,Bottom-36,Hex(Dim),text);
        List->AddCircleFilled(ImVec2(Right-tw-14,Bottom-25),5,Hex(screen.link==LinkConnected ? Green : screen.link==LinkLost ? ErrorStrong : Dim),16);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::Render();
}
}
