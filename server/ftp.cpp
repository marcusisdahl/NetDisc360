// SPDX-License-Identifier: MIT
#include "ftp.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nia {
// A TCP connection with deadlines, nonblocking underneath like IsvrClient.
class Link {
public:
    Link() : socket_(INVALID_SOCKET) {}
    ~Link() { Close(); }
    bool Open() const { return socket_!=INVALID_SOCKET; }
    void Close() { if(socket_!=INVALID_SOCKET) { closesocket(socket_); socket_=INVALID_SOCKET; } }
    // Sends the rest of the data, then tells the other side nothing more comes.
    void Finish() { if(socket_!=INVALID_SOCKET) shutdown(socket_,SD_SEND); }
    bool Connect(DWORD address, int port, double seconds) {
        Close();
        socket_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(socket_==INVALID_SOCKET) return false;
        u_long nonblocking=1;
        if(ioctlsocket(socket_,FIONBIO,&nonblocking)) { Close(); return false; }
        sockaddr_in to; memset(&to,0,sizeof(to)); to.sin_family=AF_INET; to.sin_addr.s_addr=address; to.sin_port=htons(u_short(port));
        if(!connect(socket_,reinterpret_cast<sockaddr*>(&to),sizeof(to))) return true;
        if(WSAGetLastError()!=WSAEWOULDBLOCK) { Close(); return false; }
        const double deadline=Now()+seconds;
        while(Now()<deadline) {
            fd_set writable,failed; FD_ZERO(&writable); FD_ZERO(&failed); FD_SET(socket_,&writable); FD_SET(socket_,&failed);
            timeval interval={0,100000};
            int rc=select(0,NULL,&writable,&failed,&interval);
            if(rc==SOCKET_ERROR) break;
            if(rc==0) continue;
            int status=0; int length=sizeof(status);
            if(getsockopt(socket_,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&status),&length)) status=1;
            if(FD_ISSET(socket_,&failed) || status) break;
            return true;
        }
        Close();
        return false;
    }
    bool Wait(bool writing, double deadline) {
        while(Now()<deadline) {
            fd_set set; FD_ZERO(&set); FD_SET(socket_,&set);
            timeval interval={0,100000};
            int rc=select(0,writing ? NULL : &set,writing ? &set : NULL,NULL,&interval);
            if(rc>0) return true;
            if(rc==SOCKET_ERROR) return false;
        }
        return false;
    }
    bool SendAll(const void* data, size_t count, double seconds) {
        const char* p=static_cast<const char*>(data);
        double deadline=Now()+seconds;
        while(count) {
            if(!Wait(true,deadline)) return false;
            int part=count>65536 ? 65536 : int(count);
            int sent=send(socket_,p,part,0);
            if(sent>0) { p+=sent; count-=size_t(sent); deadline=Now()+seconds; }
            else if(sent==0 || WSAGetLastError()!=WSAEWOULDBLOCK) return false;
        }
        return true;
    }
    // Bytes received, 0 when the other side closed, -1 on an error or timeout.
    int Receive(void* data, int capacity, double seconds) {
        const double deadline=Now()+seconds;
        for(;;) {
            if(!Wait(false,deadline)) return -1;
            int got=recv(socket_,static_cast<char*>(data),capacity,0);
            if(got>=0) return got;
            if(WSAGetLastError()!=WSAEWOULDBLOCK) return -1;
        }
    }
private:
    SOCKET socket_;
};

FtpClient::FtpClient(DWORD address, int port, const char* user, const char* password, double timeout)
    : address_(address), port_(port), user_(user), password_(password), timeout_(timeout), control_(new Link), code_(0), missing_(false) { error_[0]=0; }
FtpClient::~FtpClient() { Close(); delete control_; }

void FtpClient::Close() {
    if(control_->Open()) { control_->SendAll("QUIT\r\n",6,1); control_->Close(); }
    buffer_.clear(); folder_.clear();
}
bool FtpClient::Fail(const char* format, ...) {
    va_list a; va_start(a,format); _vsnprintf_s(error_,sizeof(error_),_TRUNCATE,format,a); va_end(a);
    // FtpDll's replies can fall behind after a failure; start over next time.
    control_->Close(); buffer_.clear(); folder_.clear();
    return false;
}
bool FtpClient::ReadLine(std::string& line) {
    for(;;) {
        size_t end=buffer_.find('\n');
        if(end!=std::string::npos) {
            line=buffer_.substr(0,end); buffer_.erase(0,end+1);
            if(!line.empty() && line[line.size()-1]=='\r') line.erase(line.size()-1);
            return true;
        }
        char part[1024];
        int got=control_->Receive(part,sizeof(part),timeout_);
        if(got<=0) { code_=0; return Fail(got ? "No reply from the Xbox's FTP server for %.0f s" : "The Xbox's FTP server closed the connection",timeout_); }
        buffer_.append(part,size_t(got));
        if(buffer_.size()>65536) return Fail("The Xbox's FTP server sent an overlong line");
    }
}
// One reply, which may span lines ("123-first" ... "123 last").
bool FtpClient::Reply() {
    std::string line;
    if(!ReadLine(line)) return false;
    if(line.size()<3 || !isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1]) || !isdigit((unsigned char)line[2]))
        return Fail("Unexpected FTP reply: %.200s",line.c_str());
    std::string code=line.substr(0,3);
    text_=line;
    if(line.size()>3 && line[3]=='-') {
        do { if(!ReadLine(line)) return false; } while(line.compare(0,3,code) || (line.size()>3 && line[3]!=' '));
        text_=line;
    }
    code_=atoi(code.c_str());
    return true;
}
bool FtpClient::Send(const char* format, ...) {
    char line[1024];
    va_list a; va_start(a,format); int n=_vsnprintf_s(line,sizeof(line)-2,_TRUNCATE,format,a); va_end(a);
    if(n<0) return Fail("FTP command too long");
    memcpy(line+n,"\r\n",2);
    if(!control_->SendAll(line,size_t(n)+2,timeout_)) { code_=0; return Fail("Lost the connection to the Xbox's FTP server"); }
    return true;
}
// Sends a command and checks that its reply is in the expected hundred (2 for 2xx).
bool FtpClient::Command(int expected, const char* format, ...) {
    char line[1024];
    va_list a; va_start(a,format); _vsnprintf_s(line,sizeof(line),_TRUNCATE,format,a); va_end(a);
    if(!Send("%s",line) || !Reply()) return false;
    if(code_/100!=expected) {
        std::string reply=text_;
        // Keep the user's password out of the message.
        if(!_strnicmp(line,"PASS ",5)) strcpy_s(line,"PASS");
        return Fail("%s: %s",line,reply.c_str());
    }
    return true;
}

bool FtpClient::Open() {
    missing_=false;
    if(control_->Open()) return true;
    buffer_.clear(); folder_.clear();
    if(!control_->Connect(address_,port_,5)) {
        char where[16]; AddressText(address_,where,sizeof(where)); code_=0;
        return Fail("The Xbox at %s doesn't answer on FTP port %d (Aurora not running?)",where,port_);
    }
    if(!Reply()) return false;
    if(code_!=220) return Fail("Unexpected FTP greeting: %s",text_.c_str());
    if(!Send("USER %s",user_.c_str()) || !Reply()) return false;
    if(code_==331 && !Command(2,"PASS %s",password_.c_str())) return false;
    if(code_/100!=2) return Fail("The Xbox's FTP server refused the login: %s",text_.c_str());
    return Command(2,"TYPE I");
}

bool FtpClient::Enter(const std::string& folder) {
    if(folder_==folder) return true;
    if(!Command(2,"CWD %s",folder.c_str())) return false;
    folder_=folder;
    return true;
}

bool FtpClient::Passive(Link& data) {
    if(!Command(2,"PASV")) return false;
    size_t open=text_.find('(');
    unsigned v[6];
    if(open==std::string::npos || sscanf(text_.c_str()+open+1,"%u,%u,%u,%u,%u,%u",v,v+1,v+2,v+3,v+4,v+5)!=6 || v[4]>255 || v[5]>255)
        return Fail("Unexpected PASV reply: %s",text_.c_str());
    // The control connection's address, whatever the reply says.
    if(!data.Connect(address_,int(v[4]*256+v[5]),5)) { code_=0; return Fail("Couldn't open an FTP data connection to port %u",v[4]*256+v[5]); }
    return true;
}

bool FtpClient::ReceiveAll(Link& data, std::vector<unsigned char>& out, size_t limit) {
    unsigned char part[65536];
    for(;;) {
        int got=data.Receive(part,sizeof(part),timeout_);
        if(got==0) return true;
        if(got<0) { code_=0; return Fail("The FTP data connection stalled"); }
        if(out.size()+size_t(got)>limit) { code_=0; return Fail("The file is larger than %lu bytes",(unsigned long)limit); }
        out.insert(out.end(),part,part+got);
    }
}

static void SplitPath(const char* path, std::string& folder, std::string& name) {
    const char* slash=strrchr(path,'/');
    if(!slash) { folder="/"; name=path; return; }
    folder.assign(path,size_t(slash-path)); if(folder.empty()) folder="/";
    name=slash+1;
}

bool FtpClient::List(const char* folder, std::vector<FtpEntry>& entries) {
    entries.clear();
    if(!Open() || !Enter(folder)) return false;
    Link data;
    if(!Passive(data) || !Send("LIST") || !Reply()) return false;
    if(code_/100==1 || code_/100==2) {
        bool done=code_/100==2;
        std::vector<unsigned char> text;
        if(!ReceiveAll(data,text,16<<20)) return false;
        data.Close();
        if(!done && !Reply()) return false;
        if(code_/100!=2) return Fail("LIST: %s",text_.c_str());
        std::string all(text.begin(),text.end());
        for(size_t at=0;at<all.size();) {
            size_t end=all.find('\n',at); if(end==std::string::npos) end=all.size();
            std::string line=all.substr(at,end-at); at=end+1;
            FtpEntry entry;
            if(ParseListLine(line,entry)) entries.push_back(entry);
        }
        return true;
    }
    return Fail("LIST: %s",text_.c_str());
}

bool FtpClient::Retrieve(const char* path, std::vector<unsigned char>& out, size_t limit) {
    out.clear();
    std::string folder,name; SplitPath(path,folder,name);
    if(!Open()) return false;
    if(!Enter(folder)) { missing_=code_==550; return false; }
    Link data;
    if(!Passive(data) || !Send("RETR %s",name.c_str()) || !Reply()) return false;
    if(code_/100!=1 && code_/100!=2) {
        missing_=code_==550;
        return Fail("RETR %s: %s",path,text_.c_str());
    }
    bool done=code_/100==2;
    if(!ReceiveAll(data,out,limit)) return false;
    data.Close();
    if(!done && !Reply()) return false;
    if(code_/100!=2) return Fail("RETR %s: %s",path,text_.c_str());
    return true;
}

bool FtpClient::Store(const char* path, const void* bytes, size_t size) {
    std::string folder,name; SplitPath(path,folder,name);
    if(!Open() || !Enter(folder)) return false;
    Link data;
    if(!Passive(data) || !Send("STOR %s",name.c_str()) || !Reply()) return false;
    if(code_/100!=1) return Fail("STOR %s: %s",path,text_.c_str());
    if(!data.SendAll(bytes,size,timeout_)) { code_=0; return Fail("Sending %s to the Xbox failed",path); }
    data.Finish(); data.Close();
    if(!Reply()) return false;
    if(code_/100!=2) return Fail("STOR %s: %s",path,text_.c_str());
    return true;
}

bool FtpClient::Exists(const char* folder) {
    if(!Open()) return false;
    if(Enter(folder)) return true;
    return false;
}

bool FtpClient::MakeFolder(const char* path) {
    std::string parent,name; SplitPath(path,parent,name);
    if(!Open() || !Enter(parent)) return false;
    if(Command(2,"MKD %s",name.c_str())) return true;
    // Already there is fine.
    return Exists(path);
}

bool FtpClient::Delete(const char* path) {
    std::string folder,name; SplitPath(path,folder,name);
    if(!Open()) return false;
    if(!Enter(folder)) { missing_=code_==550; return false; }
    if(Command(2,"DELE %s",name.c_str())) return true;
    missing_=code_==550;
    return false;
}

bool FtpClient::RemoveFolder(const char* path) {
    std::string parent,name; SplitPath(path,parent,name);
    if(!Open() || !Enter(parent)) return false;
    return Command(2,"RMD %s",name.c_str());
}

bool ParseListLine(const std::string& line, FtpEntry& entry) {
    // "drwxrwxrwx   1 root root             0 Oct 16 19:44 NetISO Sync": the
    // name is what follows the eighth field.
    if(line.size()<10 || (line[0]!='d' && line[0]!='-')) return false;
    size_t at=0;
    for(int field=0;field<8;++field) {
        while(at<line.size() && line[at]==' ') ++at;
        if(at==line.size()) return false;
        while(at<line.size() && line[at]!=' ') ++at;
    }
    if(at==line.size()) return false;
    ++at;  // The one space before the name; the name may start with spaces.
    std::string name=line.substr(at);
    while(!name.empty() && (name[name.size()-1]=='\r' || name[name.size()-1]=='\n')) name.erase(name.size()-1);
    if(name.empty() || name=="." || name=="..") return false;
    entry.name=name; entry.folder=line[0]=='d';
    return true;
}

int FtpStorage::ListFolders(const char* root, char names[][FolderNameMax + 1], int capacity) {
    std::vector<FtpEntry> entries;
    if(!ftp_.List(root,entries)) return 0;
    int count=0;
    for(size_t i=0;i<entries.size() && count<capacity;++i)
        if(entries[i].folder && entries[i].name.size()<=FolderNameMax) strcpy_s(names[count++],FolderNameMax+1,entries[i].name.c_str());
    return count;
}
bool FtpStorage::ReadFile(const char* path, unsigned char* data, DWORD capacity, DWORD& size) {
    std::vector<unsigned char> bytes;
    if(!ftp_.Retrieve(path,bytes,capacity)) return false;
    memcpy(data,bytes.data(),bytes.size()); size=DWORD(bytes.size());
    return true;
}
bool FtpStorage::WriteFile(const char* path, const void* data, DWORD size) { return ftp_.Store(path,data,size); }
bool FtpStorage::MakeFolder(const char* path) { return ftp_.MakeFolder(path); }
bool FtpStorage::RemoveGame(const char* folder) {
    static const char* files[]={"default.xex","boot.xex","netiso.ini","netiso.log"};
    for(int i=0;i<4;++i) { char path[PathMax]; _snprintf_s(path,sizeof(path),_TRUNCATE,"%s/%s",folder,files[i]); ftp_.Delete(path); }
    return ftp_.RemoveFolder(folder);
}
unsigned long FtpStorage::LastError() { return (unsigned long)ftp_.Code(); }
}
