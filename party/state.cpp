// SPDX-License-Identifier: MIT
// Reading the party NetDisc360 sends (see state.h).
#include "state.h"
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define ParseU64 _strtoui64
#else
#define ParseU64 strtoull
#endif

namespace party {
namespace {
// Copies at most capacity-1 bytes of [from, to), cut at a whole UTF-8 character.
void Copy(char* out, unsigned capacity, const char* from, const char* to) {
    unsigned length=unsigned(to-from);
    if(length>=capacity) {
        length=capacity-1;
        while(length && (from[length]&0xC0)==0x80) --length;
    }
    memcpy(out,from,length); out[length]=0;
}
unsigned Rgb(const char* text) {
    if(text[0]!='#' || strlen(text)!=7) return 0x107C10;
    return unsigned(strtoul(text+1,NULL,16));
}
}

bool Parse(const char* text, unsigned length, State& s) {
    memset(&s,0,sizeof(s));
    s.accent=0x107C10; s.accent2=0xFFFFFF; s.teams=2;
    for(int t=0;t<TeamsMost;++t) s.teamColour[t]=t;
    const char* end=text+length;
    const char* at=text;
    bool header=false;
    // Each line: a key, then fields after tabs.
    enum { FieldsMost = WheelMost+2 };
    while(at<end) {
        const char* stop=at;
        while(stop<end && *stop!='\n') ++stop;
        const char* field[FieldsMost+1]; const char* fieldEnd[FieldsMost+1]; int count=0;
        for(const char* p=at;count<=FieldsMost;) {
            const char* q=p;
            while(q<stop && *q!='\t') ++q;
            field[count]=p; fieldEnd[count]=q; ++count;
            if(q>=stop) break;
            p=q+1;
        }
        at=stop+1;
        char key[16]; Copy(key,sizeof(key),field[0],fieldEnd[0]);
        int n=count-1;  // Fields after the key.
        #define FIELD(i,out) Copy(out,sizeof(out),field[1+(i)],fieldEnd[1+(i)])
        char number[32];
        #define NUMBER(i) (FIELD(i,number),number)
        if(!strcmp(key,"netdisc-party")) header=n>=1 && atoi(NUMBER(0))==1;
        else if(!strcmp(key,"version") && n>=1) s.version=ParseU64(NUMBER(0),NULL,10);
        else if(!strcmp(key,"console") && n>=3) { FIELD(0,s.console); FIELD(1,s.address); s.gamemaster=atoi(NUMBER(2))==1; }
        else if(!strcmp(key,"party") && n>=4) { s.active=atoi(NUMBER(0))==1; FIELD(1,s.title); FIELD(2,s.line); FIELD(3,s.preset); }
        else if(!strcmp(key,"colours") && n>=2) { s.accent=Rgb(NUMBER(0)); s.accent2=Rgb(NUMBER(1)); }
        else if(!strcmp(key,"gamemaster") && n>=1) FIELD(0,s.gamemasterName);
        else if(!strcmp(key,"teams") && n>=1) s.teams=atoi(NUMBER(0));
        else if(!strcmp(key,"here")) { for(int i=0;i<n && i<WheelMost;++i) FIELD(i,s.here[i]); s.hereCount=n<WheelMost ? n : WheelMost; }
        else if(!strcmp(key,"spin") && n>=6) {
            s.spinId=unsigned(strtoul(NUMBER(0),NULL,10)); FIELD(1,s.kind);
            s.elapsed=atoi(NUMBER(2)); s.leadMs=atoi(NUMBER(3)); s.pauseMs=atoi(NUMBER(4)); FIELD(5,s.question);
        }
        else if(!strcmp(key,"wheel")) { for(int i=0;i<n && i<WheelMost;++i) FIELD(i,s.wheel[i]); s.wheelCount=n<WheelMost ? n : WheelMost; }
        else if(!strcmp(key,"step") && n>=4 && s.stepCount<WheelMost) {
            Step& step=s.steps[s.stepCount++];
            step.segment=atoi(NUMBER(0)); step.team=atoi(NUMBER(1)); step.to=atof(NUMBER(2)); step.ms=atoi(NUMBER(3));
        }
        else if(!strcmp(key,"round") && n>=2) { s.round=atoi(NUMBER(0)); s.kept=atoi(NUMBER(1))==1; }
        else if(!strcmp(key,"team") && s.teamCount<TeamsMost) {
            int t=s.teamCount++;
            for(int i=0;i<n && i<WheelMost;++i) FIELD(i,s.team[t][i]);
            s.teamSize[t]=n<WheelMost ? n : WheelMost;
        }
        else if(!strcmp(key,"signin") && n>=3 && s.signedCount<PerConsole) {
            SignedIn& in=s.signedIn[s.signedCount++];
            in.seat=atoi(NUMBER(0)); in.controller=atoi(NUMBER(1)); FIELD(2,in.gamertag);
            if(in.seat<0 || in.seat>=PerConsole || in.controller<0 || in.controller>3) --s.signedCount;
        }
        else if(!strcmp(key,"profiles") && n>=5) {
            FIELD(0,s.profiles); s.profilesDone=atoi(NUMBER(1)); s.profilesTotal=atoi(NUMBER(2)); FIELD(3,s.profilesNow); FIELD(4,s.profilesNote);
        }
        else if(!strcmp(key,"build") && n>=4) {
            s.serverBuild=unsigned(strtoul(NUMBER(0),NULL,10)); FIELD(1,s.serverCommit); FIELD(2,s.serverVersion); s.update=atoi(NUMBER(3))==1;
        }
        else if(!strcmp(key,"palette") && n>=5) {
            int i=atoi(NUMBER(0));
            if(i>=0 && i<PaletteMost) {
                Colour& c=s.palette[i];
                FIELD(1,c.name); c.fill=Rgb(NUMBER(2)); c.bright=Rgb(NUMBER(3)); c.ink=atoi(NUMBER(4))==1;
                if(i>=s.paletteCount) s.paletteCount=i+1;
            }
        }
        else if(!strcmp(key,"makeprofiles") && n>=1) FIELD(0,s.profileJob);
        else if(!strcmp(key,"balance") && n>=1) FIELD(0,s.balance);
        else if(!strcmp(key,"points") && n>=2 && s.pointCount<GuestsMost) { FIELD(0,s.pointName[s.pointCount]); s.points[s.pointCount++]=atoi(NUMBER(1)); }
        else if(!strcmp(key,"won") && n>=3) { s.wonRound=atoi(NUMBER(0)); FIELD(1,s.wonTeam); FIELD(2,s.wonGame); }
        else if(!strcmp(key,"teamcolours")) { for(int t=0;t<n && t<TeamsMost;++t) s.teamColour[t]=atoi(NUMBER(t)); }
        else if(!strcmp(key,"guest") && n>=4 && s.guestCount<GuestsMost) {
            Guest& g=s.guests[s.guestCount++];
            g.here=atoi(NUMBER(0))==1; FIELD(1,g.name); g.gamerscore=atoi(NUMBER(2)); g.joinedHere=atoi(NUMBER(3))==1;
        }
        else if(!strcmp(key,"known") && n>=4 && s.knownCount<KnownMost) {
            Known& k=s.known[s.knownCount++];
            FIELD(0,k.address); FIELD(1,k.name); FIELD(2,k.state); k.inParty=atoi(NUMBER(3))==1;
        }
        else if(!strcmp(key,"seat") && n>=2 && s.seatCount<SeatsMost) {
            Seat& seat=s.seats[s.seatCount++];
            FIELD(0,seat.address); FIELD(1,seat.console);
            for(int i=2;i<n && seat.count<PerConsole;++i) FIELD(i,seat.names[seat.count++]);
        }
        #undef NUMBER
        #undef FIELD
    }
    // A spin whose stops don't fit its wheel is left out.
    bool fits=s.wheelCount>0 && s.stepCount>0;
    for(int i=0;i<s.stepCount && fits;++i) fits=s.steps[i].segment>=0 && s.steps[i].segment<s.wheelCount && s.steps[i].ms>0 && s.steps[i].team<TeamsMost;
    if(!fits) { s.spinId=0; s.stepCount=s.wheelCount=0; }
    if(s.teams<2 || s.teams>TeamsMost) s.teams=2;
    s.valid=header;
    return header;
}

const char* const BalanceNames[BalanceCount]={"random","wins","gamerscore"};
const char* const BalanceLabels[BalanceCount]={"At random","Even by wins","Even by gamerscore"};
int BalanceOf(const State& s) {
    for(int b=0;b<BalanceCount;++b) if(!strcmp(s.balance,BalanceNames[b])) return b;
    return BalanceRandom;
}

Colour TeamColourOf(const State& s, int t) {
    static const Colour plain[TeamsMost]={{"Red",0xE5483E,0xFF6B61,false},{"Blue",0x3F7FE6,0x6EA8FF,false},{"Green",0x2C9A2C,0x5FD35F,false},{"Yellow",0xD9A400,0xFFD23F,true}};
    if(t<0 || t>=TeamsMost) t=0;
    int c=s.teamColour[t];
    if(s.paletteCount>0 && c>=0 && c<s.paletteCount) return s.palette[c];
    return plain[t];
}

int SpinMs(const State& s) {
    int total=s.leadMs;
    for(int i=0;i<s.stepCount;++i) total+=s.steps[i].ms+(i ? s.pauseMs : 0);
    return total;
}

double SpinAngle(const State& s, double ms, int* step, bool* stopped) {
    double angle=0, t=ms-s.leadMs;
    int at=-1;
    bool still=true;
    for(int i=0;i<s.stepCount;++i) {
        if(i) t-=s.pauseMs;
        if(t<0) break;
        at=i;
        const Step& p=s.steps[i];
        if(t<p.ms) {
            double f=1-t/p.ms;
            angle+=(p.to-angle)*(1-f*f*f);
            still=false;
            break;
        }
        angle=p.to;
        t-=p.ms;
    }
    if(step) *step=at;
    if(stopped) *stopped=still;
    return angle;
}

int Stopped(const State& s, double ms) {
    int step; bool still;
    SpinAngle(s,ms,&step,&still);
    return step<0 ? 0 : still ? step+1 : step;
}

int StopMs(const State& s, int i) {
    int at=s.leadMs;
    for(int k=0;k<=i && k<s.stepCount;++k) at+=s.steps[k].ms+(k ? s.pauseMs : 0);
    return at;
}

const Seat* MySeat(const State& s) {
    for(int i=0;i<s.seatCount;++i) if(!strcmp(s.seats[i].address,s.address)) return &s.seats[i];
    return NULL;
}

int TeamOf(const State& s, const char* name) {
    for(int t=0;t<s.teamCount;++t)
        for(int i=0;i<s.teamSize[t];++i) if(!strcmp(s.team[t][i],name)) return t;
    return -1;
}

namespace {
// name's letters and digits, lower case and plain (å as a, æ as ae, ß as ss).
void Plain(const char* name, char* out, unsigned capacity) {
    // Latin-1's letters from U+00C0, as plain ones; "" for ×/÷.
    static const char* const latin[64]={"a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","o","","o","u","u","u","u","y","th","ss",
                                        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","o","","o","u","u","u","u","y","th","y"};
    unsigned at=0;
    const unsigned char* p=reinterpret_cast<const unsigned char*>(name);
    while(*p && at+3<capacity) {
        if((*p>='a' && *p<='z') || (*p>='0' && *p<='9')) out[at++]=char(*p);
        else if(*p>='A' && *p<='Z') out[at++]=char(*p-'A'+'a');
        else if(*p==0xC3 && p[1]>=0x80 && p[1]<=0xBF) {
            const char* plain=latin[p[1]-0x80];
            while(*plain && at+1<capacity) out[at++]=*plain++;
            ++p;
        } else if(*p>=0xC0) {
            // Another letter (Ł, Č...): left out, with the rest of its bytes.
            while((p[1]&0xC0)==0x80) ++p;
        }
        ++p;
    }
    out[at]=0;
}
}

bool GamertagOf(const char* gamertag, const char* name) {
    char tag[64], plain[NameBytes*2];
    Plain(gamertag,tag,sizeof(tag)); Plain(name,plain,sizeof(plain));
    if(!tag[0]) return false;
    if(!strcmp(tag,plain)) return true;
    // A gamertag holds 15 characters: a longer name keeps its start ("Kristoffer H").
    size_t length=strlen(tag);
    return length>=4 && strlen(gamertag)>=12 && !strncmp(tag,plain,length);
}
}
