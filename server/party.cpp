// SPDX-License-Identifier: MIT
// A LAN party and its team raffle (see party.h).
#include "party.h"
#include <algorithm>
#include <ctype.h>
#include <math.h>
#include <random>
#include <stdio.h>
#include <stdlib.h>

namespace nia {
namespace {
// The next character of UTF-8 text at at, moving past it; false at a byte
// that doesn't start one (or a sequence cut short, overlong or a surrogate).
bool NextChar(const std::string& text, size_t& at, unsigned& c) {
    unsigned char b=(unsigned char)text[at];
    int more=b<0x80 ? 0 : (b&0xE0)==0xC0 ? 1 : (b&0xF0)==0xE0 ? 2 : (b&0xF8)==0xF0 ? 3 : -1;
    if(more<0 || at+size_t(more)>=text.size()) return false;
    c=more==0 ? b : more==1 ? (b&0x1F) : more==2 ? (b&0x0F) : (b&0x07);
    for(int i=1;i<=more;++i) {
        unsigned char n=(unsigned char)text[at+i];
        if((n&0xC0)!=0x80) return false;
        c=(c<<6)|(n&0x3F);
    }
    static const unsigned least[]={0,0x80,0x800,0x10000};
    if(c<least[more] || c>0x10FFFF || (c>=0xD800 && c<0xE000)) return false;
    at+=size_t(more)+1;
    return true;
}

// name in one case, for comparing: ASCII and Latin-1 letters in lower case.
std::string Folded(const std::string& name) {
    std::string out;
    for(size_t at=0;at<name.size();) {
        size_t from=at;
        unsigned c;
        if(!NextChar(name,at,c)) { out+=name[at++]; continue; }
        if(c>='A' && c<='Z') out+=char(c+32);
        else if(c>=0xC0 && c<=0xDE && c!=0xD7) { c+=32; out+=char(0xC0|(c>>6)); out+=char(0x80|(c&0x3F)); }
        else out.append(name,from,at-from);
    }
    return out;
}

std::string Number(long long n) { char text[24]; _snprintf_s(text,sizeof(text),_TRUNCATE,"%lld",n); return text; }

// The draws use std::mt19937's own numbers, which the standard fixes, and not
// its distributions, which each library makes its own way: the same seed then
// draws the same teams with MSVC and with GCC.
unsigned Below(std::mt19937& random, size_t n) { return unsigned(random()%unsigned(n)); }
template<class T> void Shuffle(std::vector<T>& items, std::mt19937& random) {
    for(size_t i=items.size();i>1;--i) std::swap(items[i-1],items[Below(random,i)]);
}

// Teams as a set of sets, to compare.
std::vector<std::vector<std::string> > TeamSet(const std::vector<Team>& teams) {
    std::vector<std::vector<std::string> > out;
    for(size_t t=0;t<teams.size();++t) {
        std::vector<std::string> team;
        for(size_t i=0;i<teams[t].size();++i) team.push_back(Folded(teams[t][i]));
        std::sort(team.begin(),team.end());
        if(!team.empty()) out.push_back(team);
    }
    std::sort(out.begin(),out.end());
    return out;
}

// A draw, as indices into people, and what it breaks.
struct Draw {
    std::vector<std::vector<int> > teams;
    std::vector<size_t> split, joined;  // The together pairs it splits, the apart pairs it puts on one team.
    bool repeat;
    Draw() : repeat(false) {}
    size_t Faults() const { return split.size()+joined.size()+(repeat ? 1 : 0); }
};

// Name lists in the settings file: % | , ; = written as %XX, so a list can
// be split at them.
std::string Escaped(const std::string& name) {
    std::string out;
    for(size_t i=0;i<name.size();++i) {
        char c=name[i];
        if(c=='%' || c=='|' || c==',' || c==';' || c=='=') { char hex[4]; _snprintf_s(hex,sizeof(hex),_TRUNCATE,"%%%02X",(unsigned char)c); out+=hex; }
        else out+=c;
    }
    return out;
}
std::string Unescaped(const std::string& text) {
    std::string out;
    for(size_t i=0;i<text.size();++i) {
        if(text[i]=='%' && i+2<text.size() && isxdigit((unsigned char)text[i+1]) && isxdigit((unsigned char)text[i+2])) {
            out+=char(strtol(text.substr(i+1,2).c_str(),NULL,16));
            i+=2;
        } else out+=text[i];
    }
    return out;
}
std::vector<std::string> Split(const std::string& text, char at) {
    std::vector<std::string> out;
    for(size_t from=0;from<=text.size();) {
        size_t to=text.find(at,from);
        if(to==std::string::npos) to=text.size();
        out.push_back(text.substr(from,to-from));
        from=to+1;
    }
    return out;
}
std::string TeamText(const Team& team) {
    std::string out;
    for(size_t i=0;i<team.size();++i) out+=(i ? "," : "")+Escaped(team[i]);
    return out;
}
Team ReadTeam(const std::string& text) {
    Team out;
    std::vector<std::string> names=Split(text,',');
    for(size_t i=0;i<names.size();++i) if(!names[i].empty()) out.push_back(Unescaped(names[i]));
    return out;
}
// Teams as "A,B;C,D".
std::string TeamsText(const std::vector<Team>& teams) {
    std::string out;
    for(size_t i=0;i<teams.size();++i) out+=(i ? ";" : "")+TeamText(teams[i]);
    return out;
}
std::vector<Team> ReadTeams(const std::string& text) {
    std::vector<Team> out;
    if(text.empty()) return out;
    std::vector<std::string> teams=Split(text,';');
    for(size_t i=0;i<teams.size();++i) { Team team=ReadTeam(teams[i]); if(!team.empty()) out.push_back(team); }
    return out;
}

long long Listed(const config::File& f, const char* key) { return std::max(0LL,std::min(1000LL,f.Number(key,0))); }

// A numbered list (key=count, key1..keyN), with the old entries past its end removed.
void SetList(config::File& f, const char* count, const char* each, const std::vector<std::string>& values) {
    long long before=Listed(f,count);
    f.Set(count,Number((long long)values.size()));
    for(size_t i=0;i<values.size();++i) f.Set((each+Number((long long)i+1)).c_str(),values[i]);
    for(long long i=(long long)values.size()+1;i<=before;++i) f.Remove((each+Number(i)).c_str());
}
std::vector<std::string> GetList(const config::File& f, const char* count, const char* each) {
    std::vector<std::string> out;
    for(long long i=1,n=Listed(f,count);i<=n;++i) out.push_back(f.Get((each+Number(i)).c_str()));
    return out;
}

// Names kept to those of guests, in the list's spelling, each once.
Team OnlyGuests(const Party& p, const Team& names, std::vector<std::string>& seen) {
    Team out;
    for(size_t i=0;i<names.size();++i) {
        std::string name=GuestNamed(p,names[i]);
        if(name.empty() || std::find(seen.begin(),seen.end(),Folded(name))!=seen.end()) continue;
        seen.push_back(Folded(name));
        out.push_back(name);
    }
    return out;
}

void TidyPairs(const Party& p, std::vector<NamePair>& pairs, const std::vector<NamePair>& other) {
    std::vector<NamePair> out;
    for(size_t i=0;i<pairs.size() && out.size()<PartyPairsMost;++i) {
        std::string a=GuestNamed(p,pairs[i].first), b=GuestNamed(p,pairs[i].second);
        if(a.empty() || b.empty() || SameName(a,b)) continue;
        bool listed=false;
        for(size_t k=0;k<out.size() && !listed;++k) listed=(SameName(out[k].first,a) && SameName(out[k].second,b)) || (SameName(out[k].first,b) && SameName(out[k].second,a));
        for(size_t k=0;k<other.size() && !listed;++k) listed=(SameName(other[k].first,a) && SameName(other[k].second,b)) || (SameName(other[k].first,b) && SameName(other[k].second,a));
        if(!listed) out.push_back(std::make_pair(a,b));
    }
    pairs.swap(out);
}
}  // namespace

bool CleanName(const std::string& typed, size_t least, size_t most, const char* what, std::string& out, std::string& error) {
    size_t a=typed.find_first_not_of(" \t\r\n"), b=typed.find_last_not_of(" \t\r\n");
    std::string name=a==std::string::npos ? std::string() : typed.substr(a,b-a+1);
    size_t count=0;
    for(size_t at=0;at<name.size();++count) {
        unsigned c;
        if(!NextChar(name,at,c)) { error=std::string(what)+" isn't text the dashboard can show (UTF-8)."; return false; }
        if(c<0x20 || (c>=0x7F && c<0xA0)) { error=std::string(what)+" can't have a line break, tab or other control character in it."; return false; }
    }
    if(count<least) { error=std::string(what)+" can't be empty."; return false; }
    if(count>most) { error=std::string(what)+" is "+Number((long long)count)+" characters long; "+Number((long long)most)+" at most."; return false; }
    out=name;
    return true;
}

bool SameName(const std::string& a, const std::string& b) { return Folded(a)==Folded(b); }

std::string GuestNamed(const Party& party, const std::string& name) {
    for(size_t i=0;i<party.guests.size();++i) if(SameName(party.guests[i].name,name)) return party.guests[i].name;
    return std::string();
}

bool CleanColour(const std::string& typed, std::string& out) {
    if(typed.size()!=7 || typed[0]!='#') return false;
    std::string colour="#";
    for(size_t i=1;i<7;++i) {
        if(!isxdigit((unsigned char)typed[i])) return false;
        colour+=char(tolower((unsigned char)typed[i]));
    }
    out=colour;
    return true;
}

bool KnownPreset(const std::string& preset) { return preset=="classic" || preset=="birthday" || preset=="company" || preset=="retro"; }

std::vector<std::string> GuestsHere(const Party& party) {
    std::vector<std::string> out;
    for(size_t i=0;i<party.guests.size();++i) if(party.guests[i].here) out.push_back(party.guests[i].name);
    return out;
}

void TidyParty(Party& p) {
    std::vector<PartyGuest> guests;
    std::string error;
    for(size_t i=0;i<p.guests.size() && guests.size()<PartyGuestsMost;++i) {
        PartyGuest g=p.guests[i];
        if(!CleanName(g.name,1,GuestNameMost,"A guest's name",g.name,error)) continue;
        bool listed=false;
        for(size_t k=0;k<guests.size() && !listed;++k) listed=SameName(guests[k].name,g.name);
        if(!listed) guests.push_back(g);
    }
    p.guests.swap(guests);
    TidyPairs(p,p.together,std::vector<NamePair>());
    TidyPairs(p,p.apart,p.together);
    p.teams=std::max<int>(TeamsLeast,std::min<int>(TeamsMost,p.teams));
    // Four colours, each once: a missing or repeated one gets the first not taken.
    {
        std::vector<int> colours;
        bool taken[TeamColoursCount]={false};
        for(int t=0;t<TeamsMost;++t) {
            int c=t<int(p.teamColours.size()) ? p.teamColours[t] : -1;
            if(c<0 || c>=TeamColoursCount || taken[c]) { c=0; while(taken[c]) ++c; }
            taken[c]=true;
            colours.push_back(c);
        }
        p.teamColours.swap(colours);
    }
    p.round=std::max(0,p.round);
    if(!KnownPreset(p.theme.preset)) p.theme.preset="classic";
    PartyTheme plain;
    if(!CleanColour(p.theme.accent,p.theme.accent)) p.theme.accent=plain.accent;
    if(!CleanColour(p.theme.accent2,p.theme.accent2)) p.theme.accent2=plain.accent2;
    if(!CleanName(p.theme.person,0,GuestNameMost,"The name",p.theme.person,error)) p.theme.person.clear();
    p.theme.age=std::max(0,std::min(150,p.theme.age));
    if(!CleanName(p.name,0,PartyNameMost,"The party's name",p.name,error)) p.name.clear();
    // The consoles each once, the Gamemaster's first.
    std::vector<std::string> consoles;
    if(!p.gamemaster.empty()) consoles.push_back(p.gamemaster);
    for(size_t i=0;i<p.consoles.size();++i)
        if(!p.consoles[i].empty() && std::find(consoles.begin(),consoles.end(),p.consoles[i])==consoles.end()) consoles.push_back(p.consoles[i]);
    p.consoles.swap(consoles);
    // The draw and the last teams name guests only; seats only the party's consoles.
    std::vector<std::string> seen;
    for(size_t t=0;t<p.drawnTeams.size();++t) p.drawnTeams[t]=OnlyGuests(p,p.drawnTeams[t],seen);
    p.drawnTeams.erase(std::remove_if(p.drawnTeams.begin(),p.drawnTeams.end(),[](const Team& t) { return t.empty(); }),p.drawnTeams.end());
    seen.clear();
    Seats seats;
    for(size_t s=0;s<p.seats.size();++s) {
        if(std::find(p.consoles.begin(),p.consoles.end(),p.seats[s].first)==p.consoles.end()) continue;
        bool listed=false;
        for(size_t k=0;k<seats.size() && !listed;++k) listed=seats[k].first==p.seats[s].first;
        if(listed) continue;
        Team team=OnlyGuests(p,p.seats[s].second,seen);
        if(team.size()>SeatsPerConsole) team.resize(SeatsPerConsole);
        seats.push_back(std::make_pair(p.seats[s].first,team));
    }
    p.seats.swap(seats);
    if(!p.drawn) { p.drawnTeams.clear(); p.seats.clear(); p.note.clear(); p.kept=false; }
    // Last round's teams stay as they were: a guest who left changes nothing there.
    std::vector<Team> last;
    for(size_t t=0;t<p.last.size();++t) {
        Team team;
        for(size_t i=0;i<p.last[t].size() && team.size()<PartyGuestsMost;++i)
            if(CleanName(p.last[t][i],1,GuestNameMost,"A name",p.last[t][i],error)) team.push_back(p.last[t][i]);
        if(!team.empty() && last.size()<TeamsMost) last.push_back(team);
    }
    p.last.swap(last);
}

bool Raffle(const std::vector<std::string>& people, int teams, const std::vector<NamePair>& together, const std::vector<NamePair>& apart,
            const std::vector<Team>& last, unsigned seed, std::vector<Team>& out, std::string& note, std::string& error) {
    out.clear(); note.clear();
    if(teams<TeamsLeast || teams>TeamsMost) { error="Draw 2, 3 or 4 teams."; return false; }
    size_t n=people.size(), k=size_t(teams);
    if(n<k) {
        error=n==0 ? "No one is here yet: tick the guests who are here." : "Only "+Number((long long)n)+(n==1 ? " guest is" : " guests are")+" here; "+Number(teams)+" teams need at least "+Number(teams)+".";
        return false;
    }
    // The rules between people who are here, as indices.
    std::vector<std::string> folded;
    for(size_t i=0;i<n;++i) folded.push_back(Folded(people[i]));
    std::vector<std::pair<int,int> > with, without;
    for(int rule=0;rule<2;++rule) {
        const std::vector<NamePair>& pairs=rule ? apart : together;
        for(size_t i=0;i<pairs.size();++i) {
            std::vector<std::string>::const_iterator a=std::find(folded.begin(),folded.end(),Folded(pairs[i].first));
            std::vector<std::string>::const_iterator b=std::find(folded.begin(),folded.end(),Folded(pairs[i].second));
            if(a==folded.end() || b==folded.end() || a==b) continue;
            (rule ? without : with).push_back(std::make_pair(int(a-folded.begin()),int(b-folded.begin())));
        }
    }
    // Those to keep together, as groups (a with b and b with c: all three).
    std::vector<int> group(n);
    for(size_t i=0;i<n;++i) group[i]=int(i);
    auto root=[&](int i) -> int { while(group[size_t(i)]!=i) i=group[size_t(i)]=group[size_t(group[size_t(i)])]; return i; };
    for(size_t i=0;i<with.size();++i) group[size_t(root(with[i].first))]=root(with[i].second);
    std::vector<std::vector<int> > groups;
    std::vector<int> groupOf(n,-1);
    for(size_t i=0;i<n;++i) {
        int r=root(int(i));
        if(groupOf[size_t(r)]<0) { groupOf[size_t(r)]=int(groups.size()); groups.push_back(std::vector<int>()); }
        groups[size_t(groupOf[size_t(r)])].push_back(int(i));
    }
    std::vector<std::vector<std::string> > lastSet=TeamSet(last);
    std::vector<size_t> sizes(k,n/k);
    for(size_t t=0;t<n%k;++t) ++sizes[t];
    std::mt19937 random(seed);
    Draw best;
    bool any=false;
    for(int attempt=0;attempt<RaffleTries;++attempt) {
        Draw d;
        d.teams.assign(k,std::vector<int>());
        std::vector<int> team(n,-1);
        std::vector<std::vector<int> > order=groups;
        Shuffle(order,random);
        std::stable_sort(order.begin(),order.end(),[](const std::vector<int>& a, const std::vector<int>& b) { return a.size()>b.size(); });
        // How many apart pairs putting these people on team t would make.
        auto clashes=[&](const std::vector<int>& members, size_t t) -> size_t {
            size_t count=0;
            for(size_t r=0;r<without.size();++r)
                for(size_t m=0;m<members.size();++m) {
                    int other=without[r].first==members[m] ? without[r].second : without[r].second==members[m] ? without[r].first : -1;
                    if(other>=0 && team[size_t(other)]==int(t)) ++count;
                }
            return count;
        };
        // The teams with room for members, and of those the ones it clashes least with.
        auto fitting=[&](const std::vector<int>& members, std::vector<size_t>& fit) {
            fit.clear();
            size_t fewest=size_t(-1);
            for(size_t t=0;t<k;++t) {
                if(d.teams[t].size()+members.size()>sizes[t]) continue;
                size_t c=clashes(members,t);
                if(c<fewest) { fewest=c; fit.clear(); }
                if(c==fewest) fit.push_back(t);
            }
        };
        // Each group on a team with room for it all, one without anyone its
        // people are to be kept apart from if there is one, picked at
        // random among those; a group too big for the room left goes one by
        // one (there's always room for one: the sizes add up to everyone).
        for(size_t g=0;g<order.size();++g) {
            std::vector<size_t> fit;
            fitting(order[g],fit);
            std::vector<std::vector<int> > parts;
            if(!fit.empty()) parts.push_back(order[g]);
            else for(size_t m=0;m<order[g].size();++m) parts.push_back(std::vector<int>(1,order[g][m]));
            for(size_t p=0;p<parts.size();++p) {
                fitting(parts[p],fit);
                size_t t=fit[Below(random,fit.size())];
                for(size_t m=0;m<parts[p].size();++m) { d.teams[t].push_back(parts[p][m]); team[size_t(parts[p][m])]=int(t); }
            }
        }
        for(size_t r=0;r<with.size();++r) if(team[size_t(with[r].first)]!=team[size_t(with[r].second)]) d.split.push_back(r);
        for(size_t r=0;r<without.size();++r) if(team[size_t(without[r].first)]==team[size_t(without[r].second)]) d.joined.push_back(r);
        if(!lastSet.empty()) {
            std::vector<Team> named(k);
            for(size_t t=0;t<k;++t) for(size_t m=0;m<d.teams[t].size();++m) named[t].push_back(people[size_t(d.teams[t][m])]);
            d.repeat=TeamSet(named)==lastSet;
        }
        if(!any || d.Faults()<best.Faults()) { best=d; any=true; }
        if(!best.Faults()) break;
    }
    // Each team in the guest list's order.
    for(size_t t=0;t<k;++t) {
        std::sort(best.teams[t].begin(),best.teams[t].end());
        Team named;
        for(size_t m=0;m<best.teams[t].size();++m) named.push_back(people[size_t(best.teams[t][m])]);
        out.push_back(named);
    }
    std::vector<std::string> notes;
    for(size_t i=0;i<best.split.size();++i)
        notes.push_back(people[size_t(with[best.split[i]].first)]+" and "+people[size_t(with[best.split[i]].second)]+" couldn't be kept together.");
    for(size_t i=0;i<best.joined.size();++i)
        notes.push_back(people[size_t(without[best.joined[i]].first)]+" and "+people[size_t(without[best.joined[i]].second)]+" couldn't be kept apart.");
    if(best.repeat) notes.push_back("These are last round's teams again: no other draw follows the rules.");
    for(size_t i=0;i<notes.size();++i) note+=(i ? " " : "")+notes[i];
    return true;
}

bool Seat(const std::vector<Team>& teams, const std::vector<std::string>& consoles, Seats& seats, std::string& error) {
    seats.clear();
    size_t people=0;
    for(size_t t=0;t<teams.size();++t) people+=teams[t].size();
    if(consoles.empty()) { error="Choose the Gamemaster console and tick the party's consoles first."; return false; }
    if(people>consoles.size()*SeatsPerConsole) {
        size_t need=(people+SeatsPerConsole-1)/SeatsPerConsole;
        error=Number((long long)people)+" people need at least "+Number((long long)need)+" consoles (up to "+Number(SeatsPerConsole)+
              " on one, split-screen), and the party has "+Number((long long)consoles.size())+". Tick more consoles, or untick guests who aren't playing.";
        return false;
    }
    for(size_t i=0;i<consoles.size();++i) seats.push_back(std::make_pair(consoles[i],Team()));
    // Each team in parts of up to SeatsPerConsole, biggest first.
    std::vector<Team> parts;
    for(size_t t=0;t<teams.size();++t)
        for(size_t at=0;at<teams[t].size();at+=SeatsPerConsole)
            parts.push_back(Team(teams[t].begin()+long(at),teams[t].begin()+long(std::min<size_t>(teams[t].size(),at+SeatsPerConsole))));
    std::stable_sort(parts.begin(),parts.end(),[](const Team& a, const Team& b) { return a.size()>b.size(); });
    if(parts.size()<=consoles.size()) {
        for(size_t p=0;p<parts.size();++p) seats[p].second=parts[p];
        return true;
    }
    // Fewer consoles than parts: parts share a console where they fit whole
    // (the first with room, the Gamemaster's first), and what fits nowhere
    // whole takes the seats left.
    std::vector<std::string> rest;
    for(size_t p=0;p<parts.size();++p) {
        size_t c=0;
        while(c<seats.size() && seats[c].second.size()+parts[p].size()>SeatsPerConsole) ++c;
        if(c<seats.size()) seats[c].second.insert(seats[c].second.end(),parts[p].begin(),parts[p].end());
        else rest.insert(rest.end(),parts[p].begin(),parts[p].end());
    }
    for(size_t i=0;i<rest.size();++i) {
        size_t c=0;
        while(seats[c].second.size()>=SeatsPerConsole) ++c;  // There is room: people <= consoles*SeatsPerConsole.
        seats[c].second.push_back(rest[i]);
    }
    return true;
}

void SaveParty(config::File& f, const Party& p) {
    f.Set("party_active",p.active ? "1" : "0");
    f.Set("party_name",p.name);
    f.Set("party_theme",p.theme.preset);
    f.Set("party_theme_accent",p.theme.accent);
    f.Set("party_theme_accent2",p.theme.accent2);
    f.Set("party_theme_person",p.theme.person);
    f.Set("party_theme_age",Number(p.theme.age));
    f.Set("party_gamemaster",p.gamemaster);
    std::string consoles;
    for(size_t i=0;i<p.consoles.size();++i) consoles+=(i ? ", " : "")+p.consoles[i];
    f.Set("party_consoles",consoles);
    // Each guest as "1|name" (1: here).
    std::vector<std::string> guests;
    for(size_t i=0;i<p.guests.size();++i) guests.push_back(std::string(p.guests[i].here ? "1" : "0")+"|"+Escaped(p.guests[i].name));
    SetList(f,"party_guests","party_guest",guests);
    f.Set("party_teams",Number(p.teams));
    std::string colours;
    for(size_t i=0;i<p.teamColours.size();++i) colours+=(i ? "," : "")+Number(p.teamColours[i]);
    f.Set("party_team_colours",colours);
    for(int rule=0;rule<2;++rule) {
        const std::vector<NamePair>& pairs=rule ? p.apart : p.together;
        std::vector<std::string> lines;
        for(size_t i=0;i<pairs.size();++i) lines.push_back(Escaped(pairs[i].first)+"|"+Escaped(pairs[i].second));
        SetList(f,rule ? "party_apart" : "party_together",rule ? "party_apart" : "party_together",lines);
    }
    f.Set("party_round",Number(p.round));
    f.Set("party_last",TeamsText(p.last));
    f.Set("party_drawn",p.drawn ? "1" : "0");
    f.Set("party_drawn_teams",TeamsText(p.drawnTeams));
    // Seats as "10.10.1.42=A,B;10.10.1.43=C".
    std::string seats;
    for(size_t i=0;i<p.seats.size();++i) seats+=(i ? ";" : "")+p.seats[i].first+"="+TeamText(p.seats[i].second);
    f.Set("party_seats",seats);
    f.Set("party_note",p.note);
    f.Set("party_kept",p.kept ? "1" : "0");
}

void SaveGuestStats(config::File& f, const std::vector<GuestStats>& stats) {
    std::vector<std::string> lines;
    for(size_t i=0;i<stats.size();++i) {
        const GuestStats& g=stats[i];
        lines.push_back(Escaped(g.name)+"|"+Number(g.gamerscore)+"|"+Number(g.achievements)+"|"+Number(g.partyStart)+"|"+Number(g.readAt)+"|"+
                        Number(g.playSeconds)+"|"+Number(g.plays)+"|"+Number(g.parties)+"|"+Number(g.lastParty)+"|"+Escaped(g.lastGame));
    }
    SetList(f,"guest_stats","guest_stat",lines);
}
void LoadGuestStats(const config::File& f, std::vector<GuestStats>& stats) {
    stats.clear();
    std::vector<std::string> lines=GetList(f,"guest_stats","guest_stat");
    for(size_t i=0;i<lines.size();++i) {
        std::vector<std::string> v=Split(lines[i],'|');
        if(v.size()<10) continue;
        GuestStats g;
        g.name=Unescaped(v[0]);
        g.gamerscore=atoi(v[1].c_str()); g.achievements=atoi(v[2].c_str()); g.partyStart=atoi(v[3].c_str());
        g.readAt=atoll(v[4].c_str()); g.playSeconds=std::max(0LL,atoll(v[5].c_str()));
        g.plays=std::max(0,atoi(v[6].c_str())); g.parties=std::max(0,atoi(v[7].c_str())); g.lastParty=atoll(v[8].c_str());
        g.lastGame=Unescaped(v[9]);
        if(!g.name.empty()) stats.push_back(g);
    }
}

void LoadParty(const config::File& f, Party& p) {
    p=Party();
    p.active=f.Get("party_active")=="1";
    p.name=f.Get("party_name");
    if(f.Has("party_theme")) p.theme.preset=f.Get("party_theme");
    if(f.Has("party_theme_accent")) p.theme.accent=f.Get("party_theme_accent");
    if(f.Has("party_theme_accent2")) p.theme.accent2=f.Get("party_theme_accent2");
    p.theme.person=f.Get("party_theme_person");
    p.theme.age=int(std::max(0LL,std::min(150LL,f.Number("party_theme_age",0))));
    p.gamemaster=f.Get("party_gamemaster");
    std::vector<std::string> consoles=Split(f.Get("party_consoles"),',');
    for(size_t i=0;i<consoles.size();++i) {
        size_t a=consoles[i].find_first_not_of(' '), b=consoles[i].find_last_not_of(' ');
        if(a!=std::string::npos) p.consoles.push_back(consoles[i].substr(a,b-a+1));
    }
    std::vector<std::string> guests=GetList(f,"party_guests","party_guest");
    for(size_t i=0;i<guests.size();++i) {
        size_t bar=guests[i].find('|');
        if(bar==std::string::npos) continue;
        PartyGuest g={Unescaped(guests[i].substr(bar+1)),guests[i].substr(0,bar)=="1"};
        p.guests.push_back(g);
    }
    p.teams=int(std::max<long long>(TeamsLeast,std::min<long long>(TeamsMost,f.Number("party_teams",2))));
    if(f.Has("party_team_colours")) {
        std::vector<std::string> colours=Split(f.Get("party_team_colours"),',');
        p.teamColours.clear();
        for(size_t i=0;i<colours.size();++i) p.teamColours.push_back(atoi(colours[i].c_str()));
    }
    for(int rule=0;rule<2;++rule) {
        std::vector<std::string> lines=GetList(f,rule ? "party_apart" : "party_together",rule ? "party_apart" : "party_together");
        for(size_t i=0;i<lines.size();++i) {
            size_t bar=lines[i].find('|');
            if(bar!=std::string::npos) (rule ? p.apart : p.together).push_back(std::make_pair(Unescaped(lines[i].substr(0,bar)),Unescaped(lines[i].substr(bar+1))));
        }
    }
    p.round=int(std::max(0LL,std::min(1000000LL,f.Number("party_round",0))));
    p.last=ReadTeams(f.Get("party_last"));
    p.drawn=f.Get("party_drawn")=="1";
    p.drawnTeams=ReadTeams(f.Get("party_drawn_teams"));
    std::string seats=f.Get("party_seats");
    std::vector<std::string> each=seats.empty() ? std::vector<std::string>() : Split(seats,';');
    for(size_t i=0;i<each.size();++i) {
        size_t equals=each[i].find('=');
        if(equals!=std::string::npos && equals) p.seats.push_back(std::make_pair(each[i].substr(0,equals),ReadTeam(each[i].substr(equals+1))));
    }
    p.note=f.Get("party_note");
    p.kept=f.Get("party_kept")=="1";
    TidyParty(p);
}

// The raffle wheel -----------------------------------------------------------

int Spin::TotalMs() const {
    int total=leadMs;
    for(size_t i=0;i<steps.size();++i) total+=steps[i].ms+(i ? pauseMs : 0);
    return total;
}

namespace {
// A step to the next stop, turns whole turns and then on to segment of
// the wheel, somewhere between a fifth and four fifths of the way across it
// (never on a line between two names).
SpinStep StepTo(const Spin& spin, double from, int segment, int team, int turns, int ms, std::mt19937& random) {
    double width=360.0/double(spin.wheel.size());
    double inside=0.2+0.6*double(random()%1000)/999.0;
    // The pointer is at the right (3 o'clock), where a name reads level: the
    // wheel's point under it is 90-angle (clockwise from the wheel's top).
    double wanted=fmod(90.0-(segment+inside)*width,360.0);
    if(wanted<0) wanted+=360;
    double now=fmod(from,360.0), ahead=wanted-now;
    if(ahead<0) ahead+=360;
    SpinStep step={segment,team,from+turns*360.0+ahead,ms};
    // Three decimals, as the console's text sends it, so every screen stops at the same place.
    step.to=floor(step.to*1000+0.5)/1000;
    return step;
}
}

void PlanTeamSpin(const std::vector<Team>& teams, unsigned seed, Spin& out) {
    std::mt19937 random(seed^0x5EED5u);
    out.kind="teams"; out.title.clear(); out.teams=int(teams.size());
    out.leadMs=SpinLeadMs; out.pauseMs=SpinPauseMs;
    out.wheel.clear(); out.steps.clear();
    // Each team's members in an order of their own, then a team at a time.
    std::vector<Team> order=teams;
    size_t longest=0;
    for(size_t t=0;t<order.size();++t) { Shuffle(order[t],random); longest=std::max(longest,order[t].size()); }
    std::vector<std::pair<std::string,int> > picks;
    for(size_t r=0;r<longest;++r)
        for(size_t t=0;t<order.size();++t)
            if(r<order[t].size()) picks.push_back(std::make_pair(order[t][r],int(t)));
    for(size_t i=0;i<picks.size();++i) out.wheel.push_back(picks[i].first);
    Shuffle(out.wheel,random);
    // The first turn is the long one; then quicker, the more there are.
    int each=picks.size()<=6 ? 4800 : picks.size()<=12 ? 4000 : 3200;
    double at=0;
    for(size_t i=0;i<picks.size();++i) {
        int segment=int(std::find(out.wheel.begin(),out.wheel.end(),picks[i].first)-out.wheel.begin());
        SpinStep step=StepTo(out,at,segment,picks[i].second,i ? 2 : 5,i ? each : int(SpinFirstMs),random);
        out.steps.push_back(step);
        at=step.to;
    }
}

void PlanPickSpin(const std::vector<std::string>& people, size_t picked, const std::string& title, unsigned seed, Spin& out) {
    std::mt19937 random(seed^0x9105u);
    out.kind="pick"; out.title=title; out.teams=0;
    out.leadMs=SpinLeadMs; out.pauseMs=SpinPauseMs;
    out.wheel=people; out.steps.clear();
    std::string chosen=picked<people.size() ? people[picked] : std::string();
    Shuffle(out.wheel,random);
    int segment=int(std::find(out.wheel.begin(),out.wheel.end(),chosen)-out.wheel.begin());
    if(out.wheel.empty()) return;
    out.steps.push_back(StepTo(out,0,segment,-1,6,8500,random));
}

double SpinAngle(const Spin& spin, double ms, int* step, bool* stopped) {
    double angle=0, t=ms-spin.leadMs;
    int at=-1;
    bool still=true;
    for(size_t i=0;i<spin.steps.size();++i) {
        if(i) t-=spin.pauseMs;
        if(t<0) break;
        at=int(i);
        const SpinStep& s=spin.steps[i];
        if(t<s.ms) {
            double f=1-t/s.ms;
            angle+=(s.to-angle)*(1-f*f*f);
            still=false;
            break;
        }
        angle=s.to;
        t-=s.ms;
    }
    if(step) *step=at;
    if(stopped) *stopped=still;
    return angle;
}

const TeamColour TeamPalette[TeamColoursCount]={
    {"Red","#e5483e","#b3261e","#ff6b61",false},
    {"Blue","#3f7fe6","#2559b0","#6ea8ff",false},
    {"Green","#2c9a2c","#0e6e0e","#5fd35f",false},
    {"Yellow","#d9a400","#7f6200","#ffd23f",true},
    {"Orange","#f07c1e","#a8500c","#ffa257",false},
    {"Purple","#8e52d6","#6a35b0","#b98cff",false},
    {"Pink","#e0509a","#b02a72","#ff8cc6",false},
    {"Teal","#149c9c","#0b6d6d","#4fd8d8",false},
};
std::string TeamName(const std::vector<int>& colours, int t) {
    int c=t>=0 && t<int(colours.size()) ? colours[t] : t;
    return c>=0 && c<TeamColoursCount ? std::string("Team ")+TeamPalette[c].name : "Team "+Number(t+1);
}

std::string PartyTitle(const Party& p) {
    if(!p.name.empty()) return p.name;
    return p.theme.preset=="birthday" && !p.theme.person.empty() ? p.theme.person+"\xE2\x80\x99s birthday" : std::string("LAN party");
}

std::string PartyLine(const Party& p) {
    const PartyTheme& t=p.theme;
    if(t.preset=="birthday") {
        std::string age;
        if(t.age>0) {
            int tens=t.age%100, ones=t.age%10;
            age=Number(t.age)+(tens>=11 && tens<=13 ? "th" : ones==1 ? "st" : ones==2 ? "nd" : ones==3 ? "rd" : "th");
        }
        if(!age.empty() && !t.person.empty()) return "Happy "+age+", "+t.person+"!";
        if(!t.person.empty()) return "Happy birthday, "+t.person+"!";
        return age.empty() ? "Happy birthday!" : "Happy "+age+" birthday!";
    }
    if(t.preset=="retro") return "Insert coin \xC2\xB7 Press Start";
    return p.name.empty() ? std::string() : std::string("LAN party");
}
}
