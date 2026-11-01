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
unsigned Colour(const char* text) {
    if(text[0]!='#' || strlen(text)!=7) return 0x107C10;
    return unsigned(strtoul(text+1,NULL,16));
}
}

bool Parse(const char* text, unsigned length, State& s) {
    memset(&s,0,sizeof(s));
    s.accent=0x107C10; s.accent2=0xFFFFFF; s.teams=2;
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
        else if(!strcmp(key,"colours") && n>=2) { s.accent=Colour(NUMBER(0)); s.accent2=Colour(NUMBER(1)); }
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

const Seat* MySeat(const State& s) {
    for(int i=0;i<s.seatCount;++i) if(!strcmp(s.seats[i].address,s.address)) return &s.seats[i];
    return NULL;
}

int TeamOf(const State& s, const char* name) {
    for(int t=0;t<s.teamCount;++t)
        for(int i=0;i<s.teamSize[t];++i) if(!strcmp(s.team[t][i],name)) return t;
    return -1;
}
}
