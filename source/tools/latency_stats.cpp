#include <array>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
using U=uint64_t;using W=__uint128_t;
std::string decimal(W v){std::string s;do{s.insert(s.begin(),'0'+v%10);v/=10;}while(v);return s;}
int main(int argc,char**argv){try{
 if(argc!=2)throw std::runtime_error("expected count argument");
 U n=std::stoull(argv[1]);if(!n)throw std::runtime_error("zero observations");
 std::array<U,5> numer{9000,9500,9900,9990,9999},ranks{},sizes{},values{};
 const char* labels[]={"90","95","99","99_9","99_99"};std::array<W,5>sums{};
 for(int i=0;i<5;++i){ranks[i]=(W(n)*numer[i]+9999)/10000;sizes[i]=(W(n)*(10000-numer[i])+9999)/10000;}
 U count=0,last=0;W total=0;std::string line;std::ios::sync_with_stdio(false);std::cin.tie(nullptr);
 while(std::getline(std::cin,line)){
  U v=0;auto r=std::from_chars(line.data(),line.data()+line.size(),v);
  if(line.empty()||r.ec!=std::errc{}||r.ptr!=line.data()+line.size()||(count&&v<last))throw std::runtime_error("invalid sorted input");
  ++count;last=v;total+=v;if(count>n)throw std::runtime_error("too many records");
  for(int i=0;i<5;++i){if(count==ranks[i])values[i]=v;if(count>n-sizes[i])sums[i]+=v;}
 }
 if(std::cin.bad()||count!=n)throw std::runtime_error("count mismatch");
 std::cout<<std::setprecision(18)<<"{\"count\":"<<count<<",\"sum_ns\":\""<<decimal(total)<<"\",\"mean_us\":"<<(long double)total/n/1000;
 for(int i=0;i<5;++i)std::cout<<",\"p"<<labels[i]<<"_us\":"<<(long double)values[i]/1000<<",\"tm"<<labels[i]<<"_us\":"<<(long double)sums[i]/sizes[i]/1000;
 std::cout<<"}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
