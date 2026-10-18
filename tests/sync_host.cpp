// SPDX-License-Identifier: MIT
// Runs NetISO Sync on Linux against a NetISO server, for tests/test_sync.py.
//   sync-host <server port> <games folder> <launcher.xex> [boot.xex]
#include "../shared/library.h"
#include <dirent.h>
#include <stdlib.h>

class PosixStorage : public nia::Storage {
public:
    int ListFolders(const char* root, char names[][nia::FolderNameMax + 1], int capacity) {
        DIR* dir=opendir(root); if(!dir) return 0;
        int count=0;
        while(struct dirent* e=readdir(dir)) {
            if(e->d_name[0]=='.' || count>=capacity) continue;
            char path[nia::PathMax]; snprintf(path,sizeof(path),"%s/%s",root,e->d_name);
            struct stat st; if(stat(path,&st) || !S_ISDIR(st.st_mode)) continue;
            strcpy_s(names[count++],nia::FolderNameMax+1,e->d_name);
        }
        closedir(dir);
        return count;
    }
    bool ReadFile(const char* path, unsigned char* data, DWORD capacity, DWORD& size) {
        FILE* f=fopen(path,"rb"); if(!f) return false;
        size=DWORD(fread(data,1,capacity,f)); bool whole=fgetc(f)==EOF; fclose(f);
        return whole;
    }
    bool WriteFile(const char* path, const void* data, DWORD size) {
        FILE* f=fopen(path,"wb"); if(!f) return false;
        bool ok=fwrite(data,1,size,f)==size; return fclose(f)==0 && ok;
    }
    bool MakeFolder(const char* path) { return mkdir(path,0777)==0 || errno==EEXIST; }
    bool RemoveGame(const char* folder) {
        static const char* files[]={"default.xex","boot.xex","netiso.ini","install.ini","netiso.log"};
        for(int i=0;i<5;++i) { char path[nia::PathMax]; snprintf(path,sizeof(path),"%s/%s",folder,files[i]); unlink(path); }
        return rmdir(folder)==0;
    }
    unsigned long LastError() { return (unsigned long)errno; }
};

static void Print(const char* text) { printf("%s\n",text); fflush(stdout); }

int main(int argc, char** argv) {
    if(argc!=4 && argc!=5) { fprintf(stderr,"usage: sync-host <port> <games folder> <launcher.xex> [boot.xex]\n"); return 2; }
    static unsigned char launcher[4<<20];
    FILE* f=fopen(argv[3],"rb"); if(!f) { perror(argv[3]); return 2; }
    DWORD size=DWORD(fread(launcher,1,sizeof(launcher),f)); fclose(f);
    nia::IsvrClient client(5);
    if(!client.Connect(inet_addr("127.0.0.1"),atoi(argv[1]),5) || !client.Ping()) { printf("CONNECT FAILED %s\n",client.Error()); return 1; }
    static unsigned char boot[4<<20]; DWORD bootSize=0;
    if(argc==5) {
        FILE* b=fopen(argv[4],"rb"); if(!b) { perror(argv[4]); return 2; }
        bootSize=DWORD(fread(boot,1,sizeof(boot),b)); fclose(b);
    }
    PosixStorage storage; nia::SyncResult result;
    bool ok=nia::Sync(client,storage,argv[2],launcher,size,argc==5 ? boot : NULL,bootSize,Print,result);
    printf("RESULT %d added=%d kept=%d removed=%d skipped=%d updated=%d\n",ok,result.added,result.kept,result.removed,result.skipped,result.updated);
    return ok ? 0 : 1;
}
