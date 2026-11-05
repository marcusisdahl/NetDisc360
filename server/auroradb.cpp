// SPDX-License-Identifier: MIT
// Aurora's content database (see auroradb.h). The page, cell and record
// layouts are SQLite's file format (sqlite.org/fileformat2.html).
#include "auroradb.h"
#include <ctype.h>
#include <string.h>

namespace nia {

namespace {
typedef std::vector<unsigned char> Bytes;

unsigned Big(const unsigned char* p, int n) { unsigned v=0; for(int i=0;i<n;++i) v=(v<<8)|p[i]; return v; }

// A varint at p, no further than end; 0 bytes if it runs past end.
int Varint(const unsigned char* p, const unsigned char* end, unsigned long long& v) {
    v=0;
    for(int i=0;i<9;++i) {
        if(p+i>=end) return 0;
        if(i==8) { v=(v<<8)|p[i]; return 9; }
        v=(v<<7)|(p[i]&0x7F);
        if(!(p[i]&0x80)) return i+1;
    }
    return 0;
}

class Reader {
public:
    Reader(const Bytes& file, std::string& error) : file_(file), error_(error), pageSize_(0), usable_(0), pages_(0), visited_(0) {}

    bool Open() {
        if(file_.size()<100 || memcmp(file_.data(),"SQLite format 3",16)) return Fail("not an SQLite database");
        pageSize_=Big(&file_[16],2);
        if(pageSize_==1) pageSize_=65536;
        if(pageSize_<512 || (pageSize_&(pageSize_-1))) return Fail("its page size is wrong");
        usable_=pageSize_-file_[20];
        if(usable_<480) return Fail("its page size is wrong");
        pages_=unsigned(file_.size()/pageSize_);
        return true;
    }

    // Each row of the table whose b-tree starts at root: its row id and
    // record, through take.
    template<class Take> bool Walk(unsigned root, Take& take, int depth=0) {
        if(depth>20 || ++visited_>pages_*2) return Fail("its tables loop");
        const unsigned char* page=Page(root);
        if(!page) return false;
        const unsigned char* end=page+usable_;
        const unsigned char* head=page+(root==1 ? 100 : 0);
        unsigned type=head[0], cells=Big(head+3,2);
        bool leaf=type==0x0D;
        if(!leaf && type!=0x05) return Fail("a table page isn't one");
        const unsigned char* pointers=head+(leaf ? 8 : 12);
        if(pointers+cells*2>end) return Fail("a table page is cut short");
        for(unsigned i=0;i<cells;++i) {
            unsigned at=Big(pointers+i*2,2);
            if(at>=usable_) return Fail("a cell is outside its page");
            const unsigned char* cell=page+at;
            if(!leaf) {
                if(cell+4>end || !Walk(Big(cell,4),take,depth+1)) return false;
                continue;
            }
            unsigned long long size,rowid;
            int n=Varint(cell,end,size);
            if(!n) return Fail("a cell is cut short");
            int m=Varint(cell+n,end,rowid);
            if(!m) return Fail("a cell is cut short");
            Bytes record;
            if(!Payload(cell+n+m,end,size,record)) return false;
            if(!take(rowid,record)) return false;
        }
        return leaf || Walk(Big(head+8,4),take,depth+1);
    }

    bool Fail(const char* why) { error_=why; return false; }

private:
    const unsigned char* Page(unsigned number) {
        if(!number || number>pages_) { Fail("a page number is past the end of the file"); return NULL; }
        return &file_[size_t(number-1)*pageSize_];
    }
    // A table leaf cell's payload, with what spills into overflow pages.
    bool Payload(const unsigned char* p, const unsigned char* end, unsigned long long size, Bytes& out) {
        if(size>file_.size()) return Fail("a record is larger than the file");
        unsigned long long x=usable_-35, local=size;
        if(size>x) {
            unsigned long long most=(usable_-12)*32/255-23, k=most+(size-most)%(usable_-4);
            local=k<=x ? k : most;
        }
        if(p+local+(local<size ? 4 : 0)>end) return Fail("a record is cut short");
        out.assign(p,p+local);
        if(local==size) return true;
        unsigned next=Big(p+local,4), hops=0;
        while(out.size()<size) {
            const unsigned char* page=Page(next);
            if(!page || ++hops>pages_) return Fail("a record's overflow pages are broken");
            size_t part=size_t(size-out.size());
            if(part>usable_-4) part=usable_-4;
            out.insert(out.end(),page+4,page+4+part);
            next=Big(page,4);
        }
        return true;
    }
    const Bytes& file_;
    std::string& error_;
    unsigned pageSize_, usable_, pages_, visited_;
};

// The values of a record, in the table's column order.
bool Values(const Bytes& record, std::vector<SqlValue>& out) {
    out.clear();
    const unsigned char* begin=record.data(), *end=begin+record.size();
    unsigned long long headerSize;
    int n=Varint(begin,end,headerSize);
    if(!n || headerSize>record.size()) return false;
    const unsigned char* types=begin+n, *data=begin+headerSize;
    while(types<begin+headerSize) {
        unsigned long long type;
        int m=Varint(types,begin+headerSize,type);
        if(!m) return false;
        types+=m;
        SqlValue v; v.null=false; v.number=0;
        static const int widths[]={0,1,2,3,4,6,8,8,0,0};
        if(type<10) {
            int width=widths[type];
            if(data+width>end) return false;
            if(type==0) v.null=true;
            else if(type==8 || type==9) v.number=type==9;
            else if(type==7) v.null=true;  // A float; nothing here needs one.
            else {
                long long x=(signed char)data[0];
                for(int i=1;i<width;++i) x=(x<<8)|data[i];  // Sign from the first byte.
                v.number=x;
            }
            data+=width;
        } else if(type>=12) {
            unsigned long long bytes=(type-12)/2;
            if(bytes>(unsigned long long)(end-data)) return false;
            v.text.assign((const char*)data,size_t(bytes));
            data+=bytes;
        } else return false;
        out.push_back(v);
    }
    return true;
}

// Column names of a CREATE TABLE statement, in order, and which one is the
// INTEGER PRIMARY KEY (the row id), or -1.
void ColumnNames(const std::string& sql, std::vector<std::string>& names, int& rowid) {
    names.clear(); rowid=-1;
    size_t open=sql.find('('), close=sql.rfind(')');
    if(open==std::string::npos || close==std::string::npos || close<open) return;
    std::vector<std::string> parts;
    std::string part; int depth=0;
    for(size_t i=open+1;i<close;++i) {
        char c=sql[i];
        if(c=='(') ++depth; else if(c==')') --depth;
        if(c==',' && !depth) { parts.push_back(part); part.clear(); } else part+=c;
    }
    parts.push_back(part);
    for(size_t i=0;i<parts.size();++i) {
        std::string def=parts[i];
        size_t at=0;
        while(at<def.size() && isspace((unsigned char)def[at])) ++at;
        size_t from=at;
        char quote=at<def.size() && strchr("\"`[",def[at]) ? def[at] : 0;
        std::string name;
        if(quote) {
            char closing=quote=='[' ? ']' : quote;
            size_t to=def.find(closing,at+1);
            name=def.substr(at+1,to==std::string::npos ? std::string::npos : to-at-1);
        } else {
            while(at<def.size() && !isspace((unsigned char)def[at])) ++at;
            name=def.substr(from,at-from);
        }
        std::string upper;
        for(size_t k=0;k<def.size();++k) upper+=isspace((unsigned char)def[k]) ? ' ' : char(toupper((unsigned char)def[k]));  // Aurora's has tabs.
        std::string first;
        for(size_t k=0;k<name.size();++k) first+=char(toupper((unsigned char)name[k]));
        if(!quote && (first=="PRIMARY" || first=="UNIQUE" || first=="CHECK" || first=="FOREIGN" || first=="CONSTRAINT")) continue;
        if(upper.find(" INTEGER")!=std::string::npos && upper.find("PRIMARY KEY")!=std::string::npos) rowid=int(names.size());
        names.push_back(name);
    }
}

struct Schema {
    const char* table; unsigned root; std::string sql;
    bool operator()(unsigned long long, const Bytes& record) {
        std::vector<SqlValue> v;
        // sqlite_schema: type, name, tbl_name, rootpage, sql.
        if(Values(record,v) && v.size()>=5 && v[0].text=="table" && !strcmp(v[1].text.c_str(),table)) { root=unsigned(v[3].number); sql=v[4].text; }
        return true;
    }
};

struct Rows {
    std::vector<int> pick;  // For each wanted column, its place in the record; -1 row id, -2 none.
    std::vector<std::vector<SqlValue> >* rows;
    std::string* error;
    bool operator()(unsigned long long rowid, const Bytes& record) {
        std::vector<SqlValue> v;
        if(!Values(record,v)) { *error="a row can't be read"; return false; }
        std::vector<SqlValue> row(pick.size());
        for(size_t i=0;i<pick.size();++i) {
            SqlValue& out=row[i];
            out.null=true; out.number=0;
            if(pick[i]==-1) { out.null=false; out.number=(long long)rowid; }
            else if(pick[i]>=0 && size_t(pick[i])<v.size()) out=v[size_t(pick[i])];
        }
        rows->push_back(row);
        return true;
    }
};
}

bool ReadSqlTable(const std::vector<unsigned char>& file, const char* table, const std::vector<std::string>& columns,
                  std::vector<std::vector<SqlValue> >& rows, std::string& error) {
    rows.clear();
    Reader reader(file,error);
    if(!reader.Open()) return false;
    Schema schema; schema.table=table; schema.root=0;
    if(!reader.Walk(1,schema)) return false;
    if(!schema.root) return reader.Fail("it has no such table");
    std::vector<std::string> names; int rowid;
    ColumnNames(schema.sql,names,rowid);
    Rows take; take.rows=&rows; take.error=&error;
    for(size_t i=0;i<columns.size();++i) {
        int at=-2;
        for(size_t k=0;k<names.size() && at==-2;++k)
            if(!_stricmp(names[k].c_str(),columns[i].c_str())) at=int(k)==rowid ? -1 : int(k);
        take.pick.push_back(at);
    }
    return reader.Walk(schema.root,take);
}

bool ReadAuroraItems(const std::vector<unsigned char>& file, std::vector<AuroraItem>& items, std::string& error) {
    items.clear();
    std::vector<std::string> columns;
    columns.push_back("Id"); columns.push_back("Directory"); columns.push_back("TitleId"); columns.push_back("TitleName");
    std::vector<std::vector<SqlValue> > rows;
    if(!ReadSqlTable(file,"ContentItems",columns,rows,error)) return false;
    for(size_t i=0;i<rows.size();++i) {
        if(rows[i][0].null || rows[i][1].null) continue;
        AuroraItem item;
        item.id=rows[i][0].number; item.directory=rows[i][1].text;
        item.titleId=unsigned(rows[i][2].number);  // Aurora keeps them as signed numbers.
        item.titleName=rows[i][3].text;
        items.push_back(item);
    }
    return true;
}
}
