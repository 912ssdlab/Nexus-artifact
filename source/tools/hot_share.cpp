#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
using U=uint64_t;
struct Reader {
 FILE*f;char buf[1<<20];size_t pos=0,end=0;
 explicit Reader(const char*p):f(fopen(p,"rb")){if(!f)throw std::runtime_error("open failed");}
 ~Reader(){fclose(f);}
 int get(){if(pos==end){end=fread(buf,1,sizeof buf,f);pos=0;if(!end){if(ferror(f))throw std::runtime_error("read failed");return EOF;}}return buf[pos++];}
 bool number(U&n){int c;do{c=get();}while(c!=EOF&&c<=32);if(c==EOF)return false;n=0;
  do{if(c<'0'||c>'9')throw std::runtime_error("non-digit");n=n*10+c-'0';c=get();}while(c!=EOF&&c>32);return true;}
};
int main(int argc,char**argv){
 if(argc!=3)return 2;
 Reader reader(argv[1]);U expected=std::stoull(argv[2]);
 std::unordered_map<U,std::array<uint32_t,64>> pages;
 U n=0,t,cpu,addr,size,rw,prev=0,reads=0;
 while(reader.number(t)){
  if(!reader.number(cpu)||!reader.number(addr)||!reader.number(size)||!reader.number(rw)
    ||size!=64||rw>1||(n&&t<prev))throw std::runtime_error("invalid trace record");
  prev=t;++n;reads+=rw==1;
  auto &count=pages[addr>>12][(addr>>6)&63];
  if(count==std::numeric_limits<uint32_t>::max())throw std::runtime_error("counter overflow");
  ++count;
  if(n%50000000==0)std::cerr<<"processed "<<n<<" / "<<expected<<"\n";
 }
 if(n!=expected||!n)throw std::runtime_error("request count mismatch");
 U unique=0,top16=0,top4=0,total=0;
 for(auto &kv:pages){
  auto &counts=kv.second;
  for(U c:counts){unique+=c>0;total+=c;}
  std::sort(counts.begin(),counts.end(),std::greater<uint32_t>());
  for(unsigned i=0;i<16;++i){top16+=counts[i];if(i<4)top4+=counts[i];}
 }
 if(total!=n||top16>n||top16*4<n)throw std::runtime_error("aggregation invariant");
 std::cout<<std::setprecision(18)<<"{\"requests\":"<<n<<",\"reads\":"<<reads
 <<",\"unique_pages\":"<<pages.size()<<",\"unique_cls\":"<<unique
 <<",\"top4_accesses_audit\":"<<top4<<",\"top16_accesses\":"<<top16
 <<",\"hot_cl_share\":"<<(long double)top16/n<<"}\n";
}
