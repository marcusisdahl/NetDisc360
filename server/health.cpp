// SPDX-License-Identifier: MIT
#include "health.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace health {
namespace {

DWORD Big32(const unsigned char* p) { return (DWORD(p[0])<<24)|(DWORD(p[1])<<16)|(DWORD(p[2])<<8)|DWORD(p[3]); }

std::string Lower(std::string text) {
    for(size_t i=0;i<text.size();++i) text[i]=char(tolower((unsigned char)text[i]));
    return text;
}

std::string Trim(const std::string& text) {
    size_t a=text.find_first_not_of(" \t\r\n\xEF\xBB\xBF"), b=text.find_last_not_of(" \t\r\n");
    return a==std::string::npos ? std::string() : text.substr(a,b-a+1);
}

// The file's lines, each with its own line end, and the line end it uses
// (CRLF, as DashLaunch's own launch.ini, unless it has only LF).
std::vector<std::string> Lines(const std::string& text, std::string& end) {
    std::vector<std::string> lines;
    for(size_t at=0;at<text.size();) {
        size_t stop=text.find('\n',at);
        stop=stop==std::string::npos ? text.size() : stop+1;
        lines.push_back(text.substr(at,stop-at));
        at=stop;
    }
    end=text.find('\n')!=std::string::npos && text.find("\r\n")==std::string::npos ? "\n" : "\r\n";
    return lines;
}

std::string Joined(const std::vector<std::string>& lines) {
    std::string out;
    for(size_t i=0;i<lines.size();++i) out+=lines[i];
    return out;
}

// A line without its comment (; or # starts one) and spaces.
std::string Content(const std::string& line) {
    std::string text=Trim(line);
    if(!text.empty() && text[0]=='#') return std::string();
    size_t comment=text.find(';');
    return Trim(comment==std::string::npos ? text : text.substr(0,comment));
}

// The key of a key = value line, in lower case; "" for any other line.
std::string Key(const std::string& line) {
    std::string text=Content(line);
    size_t equals=text.find('=');
    return equals==std::string::npos || text[0]=='[' ? std::string() : Lower(Trim(text.substr(0,equals)));
}

bool IsSection(const std::string& line) { std::string text=Content(line); return !text.empty() && text[0]=='['; }

// Where launch.ini's [Plugins] section starts (its header line) and ends
// (the next section's header, or the end); false if it has none.
bool PluginsSection(const std::vector<std::string>& lines, size_t& header, size_t& end) {
    for(header=0;header<lines.size();++header) if(Lower(Content(lines[header]))=="[plugins]") break;
    if(header==lines.size()) return false;
    for(end=header+1;end<lines.size() && !IsSection(lines[end]);++end) {}
    return true;
}

std::string Ending(const std::string& line, const std::string& end) {
    return line.size()>=2 && !line.compare(line.size()-2,2,"\r\n") ? "\r\n" : !line.empty() && line.back()=='\n' ? "\n" : end;
}

}

DWORD XexBase(const unsigned char* data, size_t size) {
    if(size<24 || memcmp(data,"XEX2",4)) return 0;
    DWORD count=Big32(data+20);
    for(DWORD i=0;i<count && 24+size_t(i)*8+8<=size;++i)
        if(Big32(data+24+i*8)==0x10201) return Big32(data+28+i*8);
    DWORD security=Big32(data+16);
    return size_t(security)+0x114<=size ? Big32(data+security+0x110) : 0;
}

bool ParseModules(const std::string& answer, std::vector<Module>& out) {
    out.clear();
    std::string end;
    std::vector<std::string> lines=Lines(answer,end);
    for(size_t i=0;i<lines.size();++i) {
        const std::string& line=lines[i];
        size_t name=Lower(line).find("name=\"");
        if(name==std::string::npos) continue;
        size_t close=line.find('"',name+6);
        if(close==std::string::npos) continue;
        Module m;
        m.name=line.substr(name+6,close-name-6);
        m.base=m.size=0;
        std::string lower=Lower(line);
        size_t at;
        if((at=lower.find(" base=",close))!=std::string::npos) m.base=DWORD(strtoul(line.c_str()+at+6,NULL,0));
        if((at=lower.find(" size=",close))!=std::string::npos) m.size=DWORD(strtoul(line.c_str()+at+6,NULL,0));
        out.push_back(m);
    }
    return !out.empty();
}

std::string SetPlugin(const std::string& ini, int slot, const std::string& path) {
    std::string end;
    std::vector<std::string> lines=Lines(ini,end);
    const std::string key="plugin"+std::to_string(slot), line=key+" = "+path;
    size_t header, stop;
    if(!PluginsSection(lines,header,stop)) {
        if(!lines.empty() && lines.back().find('\n')==std::string::npos) lines.back()+=end;
        lines.push_back("[Plugins]"+end);
        lines.push_back(line+end);
        return Joined(lines);
    }
    size_t last=header;
    for(size_t i=header+1;i<stop;++i) {
        std::string k=Key(lines[i]);
        if(k==key) { lines[i]=line+Ending(lines[i],end); return Joined(lines); }
        if(!k.compare(0,6,"plugin")) last=i;
    }
    if(lines[last].find('\n')==std::string::npos) lines[last]+=end;
    lines.insert(lines.begin()+last+1,line+end);
    return Joined(lines);
}

std::string CommentPlugin(const std::string& ini, int slot) {
    std::string end;
    std::vector<std::string> lines=Lines(ini,end);
    size_t header, stop;
    if(!PluginsSection(lines,header,stop)) return ini;
    const std::string key="plugin"+std::to_string(slot);
    for(size_t i=header+1;i<stop;++i) {
        if(Key(lines[i])!=key) continue;
        size_t first=lines[i].find_first_not_of(" \t");
        lines[i].insert(first==std::string::npos ? 0 : first,"; ");
    }
    return Joined(lines);
}

std::string SetServer(const std::string& ini, const std::string& address) {
    std::string end;
    std::vector<std::string> lines=Lines(ini,end);
    for(size_t i=0;i<lines.size();++i) {
        if(Key(lines[i])!="server") continue;
        std::string& line=lines[i];
        size_t equals=line.find('='), at=line.find_first_not_of(" \t",equals+1);
        if(at==std::string::npos) at=line.size();
        size_t stop=line.find_first_of(": \t;\r\n",at);
        if(stop==std::string::npos) stop=line.size();
        line=line.substr(0,at)+address+line.substr(stop);
        if(line.find('\n')==std::string::npos && i+1<lines.size()) line+=end;
        return Joined(lines);
    }
    lines.insert(lines.begin(),"server="+address+end);
    return Joined(lines);
}

}
