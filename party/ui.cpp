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
Screen::Screen() : spinMs(0), time(0), link(LinkLooking), view(ViewWheel), asking(0), choosing(-1), leaveIn(-1), copying(false), noticeGood(false),
                   setup(false), setupTab(TabPlayers), setupRow(0), setupTop(0), hostRow(0), typing(-1), updating(false),
                   picking(-1), pickRow(0), pickTop(0) {
    memset(&state,0,sizeof(state));
    server[0]=notice[0]=version[0]=pickSigned[0]=0;
    for(int i=0;i<PerConsole;++i) { players[i].controller=-1; players[i].gamertag[0]=0; voteRow[i]=0; padOn[i]=true; }
}

static ImFont* Small; static ImFont* Body; static ImFont* Heading; static ImFont* Title; static ImFont* Names; static ImFont* Big;
enum { Left=64, Right=1216, Top=36, Bottom=684 };
static const float Pi=3.14159265f;
// Shown while it turns and this long after, before the screen goes back to the seats.
static const double AfterMs=30000;

static ImU32 Hex(unsigned int rgb, int alpha=255) { return IM_COL32((rgb>>16)&255,(rgb>>8)&255,rgb&255,alpha); }
static const unsigned int Background=0x0F1725,Card=0x192537,Raised=0x223349,White=0xE2EBF4,Muted=0x8FA6BA,Dim=0x51667A,
    Green=0x3ED9BC,Error=0xFFAC8B,ErrorStrong=0xF0624D,Ink=0x1B1E22,ButtonA=0x5DC21E,ButtonB=0xE5413A,ButtonX=0x2E7FE0,ButtonY=0xF2C811;
// The teams' colours, as the party has them (TeamColourOf), set for each
// frame: the fill, a brighter one for text, the name ("Team Red"), and
// whether text on the fill is dark (TeamOn).
static unsigned int TeamFill[TeamsMost], TeamText[TeamsMost];
static char TeamName[TeamsMost][32];
static bool TeamInk[TeamsMost];
static void SetTeams(const State& s) {
    for(int t=0;t<TeamsMost;++t) {
        Colour c=TeamColourOf(s,t);
        TeamFill[t]=c.fill; TeamText[t]=c.bright; TeamInk[t]=c.ink;
        _snprintf_s(TeamName[t],sizeof(TeamName[t]),_TRUNCATE,"Team %s",c.name);
    }
}
static unsigned TeamOn(int t);  // Text on team t's fill.

static unsigned TeamOn(int t) { return TeamInk[t] ? Ink : 0xFFFFFF; }

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
        ImU32 ink=t==-1 ? Hex(Ink) : t>=0 ? Hex(TeamOn(t)) : Hex(0xFFFFFF);
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
// What's being copied to this console, for the screen ("" when nothing).
static void CopyingText(const Screen& screen, char* text, size_t capacity) {
    const State& s=screen.state;
    text[0]=0;
    if(!screen.copying) return;
    if(!strcmp(s.profiles,"copying") && s.profilesNow[0] && s.profilesTotal>1)
        _snprintf_s(text,capacity,_TRUNCATE,"Copying %s's profile (%d of %d)",s.profilesNow,s.profilesDone+1,s.profilesTotal);
    else if(!strcmp(s.profiles,"copying") && s.profilesNow[0]) _snprintf_s(text,capacity,_TRUNCATE,"Copying %s's profile",s.profilesNow);
    else _snprintf_s(text,capacity,_TRUNCATE,"Copying guest profiles to this Xbox");
}
// A small turning arc: work under way.
static void Spinner(float x, float y, float r, double time, unsigned colour) {
    const float a=float(time*5.0);
    List->PathArcTo(ImVec2(x,y),r,a,a+4.2f,20);
    List->PathStroke(Hex(colour),0,3);
}

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
            Text(Body,x+18,y+13,Hex(TeamOn(t)),TeamName[t]);
        }
        if(asked) List->AddRect(ImVec2(x,y),ImVec2(x+cw,y+ch),Hex(Mix(Raised,0xFFFFFF,0.35f+0.45f*pulse)),16,0,5);
        _snprintf_s(text,sizeof(text),_TRUNCATE,"P%d",i+1);
        TextRight(Body,x+cw-18,y+13,Hex(t>=0 ? TeamOn(t) : Muted,t>=0 ? 200 : 255),text);
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
        } else if(asked && screen.copying) {
            Spinner(cx,y+294,14,screen.time,Muted);
            TextCentred(Small,cx,y+322,cw-32,Hex(Muted),"Getting profiles ready");
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
    if(screen.copying && screen.asking>=0) {
        char line[NoteBytes];
        CopyingText(screen,line,sizeof(line));
        _snprintf_s(text,sizeof(text),_TRUNCATE,"%s, one moment",line);
        TextCentred(Heading,(Left+Right)/2.f,py,Right-Left,Hex(White),text);
    } else if(screen.asking>=0 && screen.asking<mine.count) {
        _snprintf_s(text,sizeof(text),_TRUNCATE,screen.choosing>=0 ? "%s, choose your profile" : "%s, press A to sign in",mine.names[screen.asking]);
        TextCentred(Heading,(Left+Right)/2.f,py,Right-Left,Hex(White),text);
    } else if(screen.leaveIn>=0) {
        _snprintf_s(text,sizeof(text),_TRUNCATE,"Everyone's in! Back to Aurora in %d",screen.leaveIn);
        TextCentred(Heading,(Left+Right)/2.f,py,Right-Left,Hex(Green),text);
    } else TextCentred(Heading,(Left+Right)/2.f,py,Right-Left,Hex(Green),"Everyone's in! Have fun");
    // Who has no profile yet, or why one didn't come.
    if(screen.state.profilesNote[0]) TextCentred(Small,(Left+Right)/2.f,py+42,Right-Left,Hex(!strcmp(screen.state.profiles,"failed") ? Error : Muted),screen.state.profilesNote);
}

int SetupRows(const State& s, int tab) {
    if(tab==TabPlayers) return 1+s.guestCount;
    if(tab==TabTeams) return 2+(s.teams>=2 && s.teams<=TeamsMost ? s.teams : 2);
    if(tab==TabScores) return s.round>0 ? s.teamCount+(s.wonRound>0 ? 1 : 0) : 0;
    return ConsoleActions+s.knownCount;
}
int HostChoices(const State& s, int* out) {
    int n=0;
    // This console first, then the others that are on.
    for(int i=0;i<s.knownCount;++i) if(!strcmp(s.known[i].address,s.address)) out[n++]=i;
    for(int i=0;i<s.knownCount;++i) if(strcmp(s.known[i].address,s.address) && strcmp(s.known[i].state,"off")) out[n++]=i;
    return n;
}
static const char* StateText(const char* state) {
    return !strcmp(state,"wheel") ? "In the party" : !strcmp(state,"aurora") ? "In Aurora" : !strcmp(state,"game") ? "In a game" : "Off";
}
// A tick box: ticked or not, highlighted when its row is picked.
static void Tick(float x, float y, bool on, bool picked) {
    Box(x,y,30,30,Hex(on ? Green : Raised),7);
    if(!on) Box(x+3,y+3,24,24,Hex(picked ? Raised : Card),5);
    if(on) {
        List->AddLine(ImVec2(x+7,y+16),ImVec2(x+13,y+22),Hex(Ink),4);
        List->AddLine(ImVec2(x+13,y+22),ImVec2(x+24,y+9),Hex(Ink),4);
    }
}
// A small arrow (dir -1 left, 1 right) for a value that changes with left and right.
static void Arrow(float x, float y, int dir, unsigned colour) {
    if(dir<0) List->AddTriangleFilled(ImVec2(x+10,y),ImVec2(x+10,y+20),ImVec2(x,y+10),Hex(colour));
    else List->AddTriangleFilled(ImVec2(x,y),ImVec2(x,y+20),ImVec2(x+10,y+10),Hex(colour));
}
// A shoulder or Start button's label, as a rounded tag. Returns the next x.
static float Tag(float x, float y, const char* name, const char* label) {
    float w=Width(Small,name)+20;
    Box(x,y+3,w,28,Hex(Raised),8);
    Text(Small,x+10,y+6,Hex(White),name);
    Text(Body,x+w+10,y+1,Hex(White),label);
    return x+w+10+Width(Body,label)+36;
}

// While there's no Gamemaster: the consoles that are on, to choose one.
static void DrawHostChoice(const Screen& screen, float px, float py, float pw) {
    const State& s=screen.state;
    char text[160];
    int choices[KnownMost];
    int n=HostChoices(s,choices);
    Text(Heading,px,py-8,Hex(White),"Who's the Gamemaster?");
    TextFit(Body,px,py+40,pw,Hex(Muted),"Their Xbox sets up the party and spins the wheel.");
    float y=py+96;
    for(int r=0;r<n && r<7;++r) {
        const Known& k=s.known[choices[r]];
        const bool picked=r==screen.hostRow, mine=!strcmp(k.address,s.address);
        Box(px,y,pw,58,Hex(picked ? Raised : Card),12);
        if(picked) List->AddRect(ImVec2(px,y),ImVec2(px+pw,y+58),Hex(Mix(s.accent,0xFFFFFF,0.35f)),12,0,3);
        Crown(px+20,y+18,24,Hex(picked ? 0xF2C811 : Dim));
        TextFit(Heading,px+60,y+8,pw-300,Hex(White),k.name[0] ? k.name : k.address);
        _snprintf_s(text,sizeof(text),_TRUNCATE,"%s%s",mine ? "This Xbox  /  " : "",StateText(k.state));
        TextRight(Small,px+pw-20,y+18,Hex(mine ? Green : Muted),text);
        y+=68;
    }
    if(!n) TextFit(Body,px,y,pw,Hex(Muted),"No console is on yet.");
}

// The party's lobby: press A to join; who's here.
static void DrawLobby(const Screen& screen, float px, float py, float pw) {
    const State& s=screen.state;
    char text[160];
    const unsigned accent=s.accent;
    // The join card.
    Box(px,py,pw,150,Hex(Card),16);
    Box(px,py,10,150,Hex(Mix(accent,0xFFFFFF,0.2f)),5);
    const float pulse=0.5f+0.5f*sinf(float(screen.time)*4);
    if(screen.typing>=0) {
        _snprintf_s(text,sizeof(text),_TRUNCATE,"Controller %d is typing a name",screen.typing+1);
        TextFit(Heading,px+36,py+28,pw-72,Hex(White),text);
        TextFit(Body,px+36,py+82,pw-72,Hex(Muted),"Your name or gamertag, as the others know you");
    } else {
        List->AddCircleFilled(ImVec2(px+58,py+56),24+3*pulse,Hex(ButtonA),32);
        Text(Heading,px+58-Width(Heading,"A")/2,py+36,Hex(Background),"A");
        TextFit(Heading,px+98,py+30,pw-130,Hex(White),"Press A to join the party");
        TextFit(Body,px+98,py+82,pw-130,Hex(Muted),s.userCount ? "On your own controller, then pick your name" : "On your own controller, then type your name");
    }
    // Who's here.
    _snprintf_s(text,sizeof(text),_TRUNCATE,s.hereCount==1 ? "1 player here" : "%d players here",s.hereCount);
    Text(Body,px,py+176,Hex(Muted),text);
    if(s.gamemasterName[0]) {
        _snprintf_s(text,sizeof(text),_TRUNCATE,"Gamemaster: %s",s.gamemasterName);
        float w=Width(Small,text);
        Crown(px+pw-w-34,py+180,20,Hex(0xF2C811));
        TextRight(Small,px+pw,py+180,Hex(Muted),text);
    }
    float gy=py+218, cw=(pw-16)/2;
    int perColumn=int((Bottom-80-gy)/38);
    if(perColumn<1) perColumn=1;
    for(int i=0;i<s.hereCount && i<perColumn*2;++i) {
        float x=px+(i/perColumn)*(cw+16), y=gy+(i%perColumn)*38;
        if(i==perColumn*2-1 && s.hereCount>perColumn*2) {
            _snprintf_s(text,sizeof(text),_TRUNCATE,"and %d more",s.hereCount-i);
            Text(Body,x,y,Hex(Muted),text);
            break;
        }
        bool joinedHere=false;
        for(int g=0;g<s.guestCount;++g) if(!strcmp(s.guests[g].name,s.here[i])) joinedHere=s.guests[g].joinedHere;
        List->AddCircleFilled(ImVec2(x+8,y+17),6,Hex(joinedHere ? Green : Mix(accent,0xFFFFFF,0.4f)),12);
        TextFit(Body,x+24,y+2,cw-24,Hex(White),s.here[i]);
    }
    if(!s.hereCount) TextFit(Body,px,gy,pw,Hex(Dim),"No one has joined yet.");
}

int PickRows(const Screen& screen) { return 1+(screen.pickSigned[0] ? 1 : 0)+screen.state.userCount; }
int PickUser(const Screen& screen, int r) {
    int u=r-1-(screen.pickSigned[0] ? 1 : 0);
    return u>=0 && u<screen.state.userCount ? u : -1;
}

// Joining: "Who are you?", over the screen, for the controller that
// pressed A: a new name, the profile signed in on it, or a user (their
// profile here, on another console or kept on the PC comes with the others
// once the wheel seats them).
static void DrawPicker(const Screen& screen) {
    const State& s=screen.state;
    char text[256];
    const float x=Left, y=Top+92, w=Right-Left, h=Bottom-70-y;
    Box(0,0,1280,720,Hex(Background,205));
    Box(x,y,w,h,Hex(Card),18);
    _snprintf_s(text,sizeof(text),_TRUNCATE,"Who are you, controller %d?",screen.picking+1);
    Text(Heading,x+32,y+22,Hex(White),text);
    TextFit(Small,x+32,y+68,w-64,Hex(Muted),"Pick your name: your own profile comes to your Xbox with everyone's after the wheel, with your saves and gamerscore.");
    const int rows=PickRows(screen);
    const float rowH=52, top=y+108;
    for(int r=screen.pickTop;r<rows && r<screen.pickTop+PickShown;++r) {
        const float ry=top+(r-screen.pickTop)*(rowH+6);
        const bool picked=r==screen.pickRow;
        Box(x+24,ry,w-48,rowH,Hex(picked ? Raised : Mix(Card,Background,0.4f)),10);
        if(picked) List->AddRect(ImVec2(x+24,ry),ImVec2(x+w-24,ry+rowH),Hex(Mix(s.accent,0xFFFFFF,0.35f)),10,0,3);
        const float lx=x+48, ly=ry+12;
        const int u=PickUser(screen,r);
        if(r==0) {
            List->AddCircleFilled(ImVec2(lx+15,ry+26),15,Hex(Green),24);
            Box(lx+8,ry+24,14,4,Hex(Ink)); Box(lx+13,ry+19,4,14,Hex(Ink));
            Text(Body,lx+46,ly,Hex(White),"I'm new: type a name");
            TextRight(Small,x+w-48,ly+4,Hex(Muted),"gets a profile of their own");
            continue;
        }
        if(u<0) {
            // The profile signed in on this controller, which is no user's yet.
            List->AddCircleFilled(ImVec2(lx+15,ry+26),15,Hex(Mix(s.accent,0xFFFFFF,0.3f)),24);
            char first[2]={screen.pickSigned[0],0};
            TextCentred(Small,lx+15,ry+15,30,Hex(Ink),first);
            _snprintf_s(text,sizeof(text),_TRUNCATE,"%s",screen.pickSigned);
            TextFit(Body,lx+46,ly,w-460,Hex(White),text);
            TextRight(Small,x+w-48,ly+4,Hex(Green),"signed in on this controller");
            continue;
        }
        const User& usr=s.users[u];
        bool here=false;
        for(int g=0;g<s.guestCount;++g) if(!strcmp(s.guests[g].name,usr.name)) here=s.guests[g].here;
        List->AddCircleFilled(ImVec2(lx+15,ry+26),15,Hex(here ? Green : Raised),24);
        char initial[8]={0}; int n=0;
        for(const char* c=usr.name;*c && n<6 && (n==0 || (*c&0xC0)==0x80);++c) initial[n++]=*c;
        TextCentred(Small,lx+15,ry+15,30,Hex(here ? Ink : White),initial);
        float nameW=Width(Body,usr.name);
        if(nameW>w*0.34f) nameW=w*0.34f;
        TextFit(Body,lx+46,ly,w*0.34f,Hex(White),usr.name);
        if(usr.gamertag[0] && strcmp(usr.gamertag,usr.name)) {
            if(usr.gamerscore>=0) _snprintf_s(text,sizeof(text),_TRUNCATE,"%s  /  %d G",usr.gamertag,usr.gamerscore);
            else _snprintf_s(text,sizeof(text),_TRUNCATE,"%s",usr.gamertag);
            TextFit(Small,lx+46+nameW+16,ly+4,w*0.26f,Hex(Muted),text);
        } else if(usr.gamerscore>=0) {
            _snprintf_s(text,sizeof(text),_TRUNCATE,"%d G",usr.gamerscore);
            TextFit(Small,lx+46+nameW+16,ly+4,w*0.26f,Hex(Muted),text);
        }
        unsigned tone=Muted;
        if(!strcmp(usr.where,"here")) { _snprintf_s(text,sizeof(text),_TRUNCATE,"Profile on this Xbox"); tone=Green; }
        else if(!strcmp(usr.where,"console")) _snprintf_s(text,sizeof(text),_TRUNCATE,"Profile on %s: comes here",usr.from[0] ? usr.from : "another Xbox");
        else if(!strcmp(usr.where,"pc")) _snprintf_s(text,sizeof(text),_TRUNCATE,"Profile kept on the PC: comes here");
        else _snprintf_s(text,sizeof(text),_TRUNCATE,"Gets a new profile");
        if(here) { _snprintf_s(text,sizeof(text),_TRUNCATE,"Joined"); tone=Green; }
        TextRight(Small,x+w-48,ly+4,Hex(tone),text);
    }
    if(screen.pickTop>0) Text(Small,x+w/2-40,top-26,Hex(Dim),"More above");
    if(screen.pickTop+PickShown<rows) Text(Small,x+w/2-40,top+PickShown*(rowH+6)-2,Hex(Dim),"More below");
}

// The Gamemaster's setup, over the screen: Players, Teams and Consoles.
// The game vote, over everything: every game with its votes; each of this
// console's controllers has its own pick (its number at the left), and its
// vote shows as its number at the right.
static void DrawVote(const Screen& screen) {
    const State& s=screen.state;
    char text[200];
    const float x=Left, y=Top+96, w=Right-Left, h=Bottom-60-y;
    Box(0,0,1280,720,Hex(Background,215));
    Box(x,y,w,h,Hex(Card),18);
    Text(Heading,x+32,y+20,Hex(White),"Vote for the next game");
    _snprintf_s(text,sizeof(text),_TRUNCATE,"%d s left",s.voteLeft);
    TextRight(Heading,x+w-32,y+20,Hex(s.voteLeft<=10 ? 0xF2C811 : Muted),text);
    TextFit(Small,x+32,y+66,w-64,Hex(Muted),"Each player picks with their own controller and presses A. The wheel then spins between the top games.");
    int most=1;
    for(int i=0;i<s.voteCount;++i) if(s.voteVotes[i]>most) most=s.voteVotes[i];
    const float top=y+104, rowH=(h-120)/(s.voteCount>6 ? s.voteCount : 6);
    for(int i=0;i<s.voteCount;++i) {
        const float ry=top+i*rowH;
        bool picked=false;
        for(int pad=0;pad<PerConsole;++pad) if(screen.padOn[pad] && screen.voteRow[pad]==i) picked=true;
        Box(x+24,ry,w-48,rowH-6,Hex(picked ? Raised : Mix(Card,Background,0.4f)),10);
        // Controllers whose pick is here, at the left.
        float cx=x+40;
        for(int pad=0;pad<PerConsole;++pad) if(screen.padOn[pad] && screen.voteRow[pad]==i) {
            List->AddCircleFilled(ImVec2(cx+12,ry+(rowH-6)/2),12,Hex(Mix(s.accent,0xFFFFFF,0.3f)),20);
            char n[4]; _snprintf_s(n,sizeof(n),_TRUNCATE,"%d",pad+1);
            TextCentred(Small,cx+12,ry+(rowH-6)/2-11,20,Hex(Ink),n);
            cx+=28;
        }
        const float nx=x+160;
        TextFit(Body,nx,ry+(rowH-6)/2-15,w*0.42f,Hex(White),s.voteGame[i]);
        // Its votes: a bar and the number; this console's votes as numbers.
        const float bx=x+w*0.58f, bw=w*0.26f;
        Box(bx,ry+(rowH-6)/2-6,bw,12,Hex(Background),6);
        if(s.voteVotes[i]) Box(bx,ry+(rowH-6)/2-6,bw*s.voteVotes[i]/most,12,Hex(Mix(s.accent,0xFFFFFF,0.35f)),6);
        _snprintf_s(text,sizeof(text),_TRUNCATE,"%d",s.voteVotes[i]);
        TextRight(Body,x+w-110,ry+(rowH-6)/2-15,Hex(White),text);
        float vx=x+w-48;
        for(int pad=PerConsole-1;pad>=0;--pad) if(s.myVote[pad]==i) {
            List->AddCircleFilled(ImVec2(vx-10,ry+(rowH-6)/2),10,Hex(Green),18);
            char n[4]; _snprintf_s(n,sizeof(n),_TRUNCATE,"%d",pad+1);
            TextCentred(Small,vx-10,ry+(rowH-6)/2-11,18,Hex(Ink),n);
            vx-=24;
        }
    }
}

static void DrawSetup(const Screen& screen) {
    const State& s=screen.state;
    char text[200];
    const float x=Left, y=Top+92, w=Right-Left, h=Bottom-70-y;
    Box(0,0,1280,720,Hex(Background,200));
    Box(x,y,w,h,Hex(Card),18);
    Text(Heading,x+32,y+22,Hex(White),"Set up the party");
    // The tabs, LB and RB between them.
    static const char* const tabs[TabsCount]={"Players","Teams","Scores","Consoles"};
    float tx=x+w-32;
    for(int t=TabsCount-1;t>=0;--t) {
        float tw=Width(Body,tabs[t])+36;
        tx-=tw;
        const bool on=t==screen.setupTab;
        Box(tx,y+24,tw,42,Hex(on ? Mix(s.accent,0x000000,0.1f) : Raised),21);
        Text(Body,tx+18,y+31,Hex(on ? 0xFFFFFF : Muted),tabs[t]);
        tx-=10;
    }
    Text(Small,tx-Width(Small,"LB / RB")-10,y+34,Hex(Dim),"LB / RB");
    const int rows=SetupRows(s,screen.setupTab);
    const float rowH=52, top=y+92;
    for(int r=screen.setupTop;r<rows && r<screen.setupTop+SetupShown;++r) {
        const float ry=top+(r-screen.setupTop)*(rowH+6);
        const bool picked=r==screen.setupRow;
        Box(x+24,ry,w-48,rowH,Hex(picked ? Raised : Mix(Card,Background,0.4f)),10);
        if(picked) List->AddRect(ImVec2(x+24,ry),ImVec2(x+w-24,ry+rowH),Hex(Mix(s.accent,0xFFFFFF,0.35f)),10,0,3);
        const float lx=x+48, ly=ry+12;
        if(screen.setupTab==TabPlayers) {
            if(r==0) {
                List->AddCircleFilled(ImVec2(lx+15,ry+26),15,Hex(Green),24);
                Box(lx+8,ry+24,14,4,Hex(Ink)); Box(lx+13,ry+19,4,14,Hex(Ink));
                Text(Body,lx+46,ly,Hex(White),"Add a player");
                TextRight(Small,x+w-48,ly+4,Hex(Muted),"A types a name");
                continue;
            }
            const Guest& g=s.guests[r-1];
            Tick(lx,ry+11,g.here,picked);
            TextFit(Body,lx+46,ly,w-420,Hex(g.here ? White : Muted),g.name);
            if(g.gamerscore>=0) { _snprintf_s(text,sizeof(text),_TRUNCATE,"%d G",g.gamerscore); TextRight(Body,x+w-48,ly,Hex(Muted),text); }
            if(g.joinedHere) TextRight(Small,x+w-170,ly+4,Hex(Green),"joined here");
        } else if(screen.setupTab==TabTeams) {
            if(r==0) {
                Text(Body,lx,ly,Hex(White),"Teams");
                _snprintf_s(text,sizeof(text),_TRUNCATE,"%d",s.teams);
                float vx=x+w-140;
                Arrow(vx-40,ry+16,-1,picked ? White : Dim);
                Text(Heading,vx-Width(Heading,text)/2+10,ry+4,Hex(White),text);
                Arrow(vx+50,ry+16,1,picked ? White : Dim);
                continue;
            }
            if(r==1) {
                Text(Body,lx,ly,Hex(White),"Teams drawn");
                const char* how=BalanceLabels[BalanceOf(s)];
                float vx=x+w-270;
                Arrow(vx-40,ry+16,-1,picked ? White : Dim);
                TextCentred(Body,vx+95,ly,200,Hex(White),how);
                Arrow(vx+230,ry+16,1,picked ? White : Dim);
                continue;
            }
            const int t=r-2;
            _snprintf_s(text,sizeof(text),_TRUNCATE,"Team %d",t+1);
            Text(Body,lx,ly,Hex(White),text);
            float sx=x+w-420;
            Box(sx,ry+8,300,36,Hex(TeamFill[t]),18);
            TextCentred(Body,sx+150,ly,280,Hex(TeamOn(t)),TeamName[t]);
            Arrow(sx-30,ry+16,-1,picked ? White : Dim);
            Arrow(sx+320,ry+16,1,picked ? White : Dim);
        } else if(screen.setupTab==TabScores) {
            if(r<s.teamCount) {
                Box(lx,ry+8,300,36,Hex(TeamFill[r]),18);
                TextCentred(Body,lx+150,ly,280,Hex(TeamOn(r)),TeamName[r]);
                Text(Body,lx+320,ly,Hex(White),"won this round");
                continue;
            }
            Box(lx,ry+11,30,30,Hex(ButtonA),15);
            _snprintf_s(text,sizeof(text),_TRUNCATE,"Take back round %d (%s)",s.wonRound,s.wonTeam);
            TextFit(Body,lx+46,ly,w-140,Hex(White),text);
        } else {
            if(r==0) {
                Box(lx,ry+11,30,30,Hex(ButtonA),15);
                Text(Body,lx+46,ly,Hex(White),"Open the party on all consoles");
                TextRight(Small,x+w-48,ly+4,Hex(Muted),"those in Aurora start it, and join");
                continue;
            }
            if(r==1) {
                Box(lx,ry+11,30,30,Hex(ButtonA),15);
                Text(Body,lx+46,ly,Hex(White),"Back to the party on all");
                TextRight(Small,x+w-48,ly+4,Hex(Muted),"the party's consoles; games end");
                continue;
            }
            const Known& k=s.known[r-ConsoleActions];
            const bool master=s.gamemasterName[0] && !strcmp(k.name[0] ? k.name : k.address,s.gamemasterName);
            Tick(lx,ry+11,k.inParty || master,picked);
            if(master) Crown(lx+46,ry+14,22,Hex(0xF2C811));
            TextFit(Body,lx+(master ? 80 : 46),ly,w-520,Hex(White),k.name[0] ? k.name : k.address);
            _snprintf_s(text,sizeof(text),_TRUNCATE,"%s%s",!strcmp(k.address,s.address) ? "This Xbox  /  " : "",StateText(k.state));
            TextRight(Small,x+w-48,ly+4,Hex(!strcmp(k.state,"off") ? Dim : Muted),text);
        }
    }
    // Consoles: how many are ready for the next game, and what's in the way.
    if(screen.setupTab==TabConsoles && s.readyOf>0) {
        float sy=top+(rows<SetupShown ? rows : SetupShown)*(rowH+6)+8;
        const bool all=s.readyCount>=s.readyOf;
        _snprintf_s(text,sizeof(text),_TRUNCATE,all ? "All %d consoles are ready" : "%d of %d consoles ready",all ? s.readyOf : s.readyCount,s.readyOf);
        Text(Body,x+48,sy,Hex(all ? Green : 0xF2C811),text);
        if(!all && s.readyNote[0]) TextFit(Small,x+48+Width(Body,text)+20,sy+4,w-140-Width(Body,text),Hex(Muted),s.readyNote);
    }
    // Scores: the night's points under the rows (or why there are no rows).
    if(screen.setupTab==TabScores) {
        float sy=top+(rows<SetupShown ? rows : SetupShown)*(rowH+6)+8;
        if(!rows) { TextFit(Body,x+48,sy,w-96,Hex(Muted),"Draw the teams first; after each game, say which team won here."); sy+=44; }
        if(s.pointCount) {
            char line[512]; int used=_snprintf_s(line,sizeof(line),_TRUNCATE,"Points tonight:");
            for(int i=0;i<s.pointCount && used>0 && used<int(sizeof(line))-8;++i) {
                int more=_snprintf_s(line+used,sizeof(line)-used,_TRUNCATE,"%s %s %d",i ? "  /" : "",s.pointName[i],s.points[i]);
                if(more<0) break;
                used+=more;
            }
            TextFit(Body,x+48,sy,w-96,Hex(White),line);
        } else if(rows) TextFit(Small,x+48,sy,w-96,Hex(Muted),"Each player of the team that won gets a point.");
    }
    // More above or below.
    if(screen.setupTop>0) Text(Small,x+w/2-40,top-26,Hex(Dim),"More above");
    if(screen.setupTop+SetupShown<rows) Text(Small,x+w/2-40,top+SetupShown*(rowH+6)-2,Hex(Dim),"More below");
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
    SetTeams(s);

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
            const ImU32 ink=Hex(TeamOn(t));
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
        if(s.pointCount) {
            // The night's top three.
            char line[256]; int used=0;
            for(int i=0;i<s.pointCount && i<3 && used<int(sizeof(line))-8;++i) {
                int more=_snprintf_s(line+used,sizeof(line)-used,_TRUNCATE,"%s%s %d",i ? "   " : "Points: ",s.pointName[i],s.points[i]);
                if(more<0) break;
                used+=more;
            }
            if(used>0) TextRight(Small,px+pw,ty,Hex(0xF2C811),line);
        }
        if(s.gamemaster && s.readyOf>0 && s.readyCount<s.readyOf) {
            // The Gamemaster sees what's in the way before Start on all.
            _snprintf_s(text,sizeof(text),_TRUNCATE,"%d of %d consoles ready. %s",s.readyCount,s.readyOf,s.readyNote);
            TextFit(Small,px,ty+26,pw,Hex(0xF2C811),text);
            ty+=26;
        }
        ty+=34;
        const int columns=s.teamCount>2 ? 2 : s.teamCount, rows=(s.teamCount+1)/2;
        const float gap=12, cw=(pw-gap*(columns-1))/columns, ch=((Bottom-80)-ty-gap*(rows-1))/rows;
        if(ch>70) for(int k=0;k<s.teamCount;++k) TeamCard(px+(k%2)*(cw+gap),ty+(k/2)*(ch+gap),cw,ch,k,s.team[k],s.teamSize[k],true);
    } else if(!s.gamemasterName[0]) DrawHostChoice(screen,px,py,pw);
    else DrawLobby(screen,px,py,pw);

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

    // The Gamemaster's setup, over the rest; the game vote over everything.
    if(screen.setup && s.valid) DrawSetup(screen);
    else if(s.valid && s.voteOpen) DrawVote(screen);
    else if(s.valid && screen.picking>=0) DrawPicker(screen);

    // A word back from the server: on the last spin or change (a problem), or good news.
    if(screen.notice[0]) {
        float nw=Width(Small,screen.notice)+48; if(nw>Right-Left) nw=Right-Left;
        Box(Right-nw,Bottom-112,nw,40,Hex(Raised),10);
        List->AddCircleFilled(ImVec2(Right-nw+20,Bottom-92),6,Hex(screen.noticeGood ? Green : ErrorStrong),12);
        TextFit(Small,Right-nw+36,Bottom-103,nw-48,Hex(screen.noticeGood ? White : Error),screen.notice);
    }

    // The guests' profiles on their way here (on the wheel; the players
    // screen says it under the cards).
    if(!players) {
        char line[NoteBytes];
        const bool failed=!strcmp(s.profiles,"failed") && s.profilesNote[0];
        CopyingText(screen,line,sizeof(line));
        if(!line[0] && failed) _snprintf_s(line,sizeof(line),_TRUNCATE,"%s",s.profilesNote);
        if(line[0]) {
            const float ly=Bottom-112-(screen.notice[0] ? 52 : 0);
            float nw=Width(Small,line)+60; if(nw>Right-Left) nw=Right-Left;
            Box(Right-nw,ly,nw,40,Hex(Raised),10);
            if(screen.copying) Spinner(Right-nw+22,ly+20,9,screen.time,Green);
            else List->AddCircleFilled(ImVec2(Right-nw+22,ly+20),6,Hex(ErrorStrong),12);
            TextFit(Small,Right-nw+42,ly+9,nw-54,Hex(screen.copying ? White : Error),line);
        }
    }

    // Footer: the buttons, and the server.
    List->AddLine(ImVec2(Left,Bottom-56),ImVec2(Right,Bottom-56),Hex(Raised),1);
    float bx=Left;
    const bool turning=s.valid && s.spinId && s.stepCount>0 && screen.spinMs<SpinMs(s);
    if(s.valid && screen.picking>=0 && !screen.setup && !s.voteOpen) {
        bx=Button(bx,Bottom-40,"A",ButtonA,"That's me");
        bx=Tag(bx,Bottom-40,"Up / Down","Choose");
        bx=Button(bx,Bottom-40,"B",ButtonB,"Back");
    } else if(s.valid && s.voteOpen && !screen.setup) {
        bx=Tag(bx,Bottom-40,"Up / Down","Choose");
        if(s.gamemaster) bx=Tag(bx,Bottom-40,"START","Spin now");
    } else if(screen.setup && s.valid) {
        bx=Button(bx,Bottom-40,"A",ButtonA,screen.setupTab==TabTeams ? "Next" : screen.setupTab==TabPlayers && screen.setupRow>0 ? "Playing or not" :
                  screen.setupTab==TabScores ? (screen.setupRow<s.teamCount ? "Won" : "Take back") : "Choose");
        if(screen.setupTab==TabTeams) bx=Tag(bx,Bottom-40,"< >","Change");
        if(screen.setupTab==TabConsoles && screen.setupRow>=ConsoleActions) bx=Button(bx,Bottom-40,"Y",ButtonY,"Gamemaster");
        bx=Button(bx,Bottom-40,"B",ButtonB,"Done");
    } else if(players) {
        if(screen.asking>=0 && !screen.copying) bx=Button(bx,Bottom-40,"A",ButtonA,"Sign in");
        else if(screen.leaveIn>=0) bx=Button(bx,Bottom-40,"A",ButtonA,"Go now");
        bx=Button(bx,Bottom-40,"Y",ButtonY,"See the wheel");
    } else if(s.valid && !s.gamemasterName[0]) {
        bx=Button(bx,Bottom-40,"A",ButtonA,"Choose");
    } else {
        if(s.valid && !turning) bx=Button(bx,Bottom-40,"A",ButtonA,"Join");
        if(s.valid && s.gamemaster && !turning) {
            bx=Tag(bx,Bottom-40,"START","Spin");
            bx=Button(bx,Bottom-40,"X",ButtonX,"Pick one");
            bx=Tag(bx,Bottom-40,"RB","Set up");
            bx=Tag(bx,Bottom-40,"LB","Game vote");
        }
        if(seat && seat->count>0 && !turning) bx=Button(bx,Bottom-40,"Y",ButtonY,"Sign in");
    }
    // Not while profiles are copied here: leaving would cut them off.
    if(screen.copying) {
        Spinner(bx+16,Bottom-24,10,screen.time,Muted);
        Text(Body,bx+38,Bottom-39,Hex(Muted),"Copying profiles, please wait");
        bx+=38+Width(Body,"Copying profiles, please wait")+32;
    } else if(!(screen.setup && s.valid) && !(s.valid && s.voteOpen) && !(s.valid && screen.picking>=0)) bx=Button(bx,Bottom-40,"B",ButtonB,"Aurora");
    const char* where=screen.link==LinkConnected ? "Connected to NetDisc360 at %s" : screen.link==LinkLost ? "Lost NetDisc360 at %s, trying again" : screen.link==LinkWaiting ? "Waiting for the network" : "Looking for NetDisc360 %s";
    _snprintf_s(text,sizeof(text),_TRUNCATE,where,screen.server);
    float tw=Width(Small,text);
    if(Right-tw>bx) {
        TextRight(Small,Right,Bottom-36,Hex(Dim),text);
        List->AddCircleFilled(ImVec2(Right-tw-14,Bottom-25),5,Hex(screen.link==LinkConnected ? Green : screen.link==LinkLost ? ErrorStrong : Dim),16);
    }

    // This app's version and build, small, at the top right under the console's name.
    if(screen.version[0]) TextRight(Small,Right,Top+74,Hex(Dim),screen.version);

    // Making way for the server's newer app.
    if(screen.updating) {
        Box(0,0,1280,720,Hex(Background,220));
        Box(340,250,600,200,Hex(Card),18);
        Spinner(400,322,18,screen.time,Green);
        Text(Heading,436,296,Hex(White),"Updating the party app");
        TextFit(Body,436,350,480,Hex(Muted),"NetDisc360 has a newer one. Back in a moment.");
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::Render();
}
}
